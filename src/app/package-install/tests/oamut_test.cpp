// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A mutator installed into a scratch Mutators folder: a package with lua/,
// rules/, files/, an icon and a note; a reinstall question; an update that
// keeps .backup and a roll back that swaps the two; a catalogue release of
// the same registry replaced with no question; another registry asking, the
// question naming both; a folder that holds something else left as it was;
// an uninstall the discarder deletes; a package inside one top folder; and
// each refusal, with its sentence and nothing written. No script is parsed.

#include "oa/app/package_install.hpp"
#include "oa/app/package_install/oamut.hpp"
#include "oa/base/sha256.hpp"
#include "oa/formats/oamut/manifest.hpp"
#include "oa/formats/png.hpp"
#include "oa/formats/zip.hpp"
#include "oa/test/check.hpp"
#include "oa/test/raw_zip.hpp"
#include "oa/test/scratch_directory.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace install = oa::app::package_install;
namespace mut = oa::formats::oamut;
namespace oamut = install::oamut;
namespace png = oa::formats::png;
namespace raw = oa::test::raw_zip;
namespace zip = oa::formats::zip;
using install::Change;

/// The mutator the tests install.
constexpr std::string_view kId = "steady-hands";

/// A scratch folder, deleted when the test ends.
class Scratch {
  public:

    Scratch() : path_(oa::test::make_scratch_directory("oa-package-install-oamut-")) {}

    ~Scratch() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }

    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;

    [[nodiscard]] const fs::path& path() const { return path_; }

  private:

    fs::path path_;
};

/// One file of a package.
struct Piece {
    std::string name{};
    std::vector<uint8_t> bytes{};
};

/// Returns text as bytes.
///
/// @param text the text
/// @return its bytes
std::vector<uint8_t> bytes_of(std::string_view text) {
    return {text.begin(), text.end()};
}

/// Writes a stored PNG.
///
/// @param width its width
/// @param height its height
/// @param color its colour type
/// @return the file; empty when it could not be written
std::vector<uint8_t> png_image(uint32_t width, uint32_t height, png::ColorType color) {
    png::Header header{};
    header.width = width;
    header.height = height;
    header.bit_depth = 8;
    header.color_type = color;
    const std::size_t row = png::row_bytes(header, {});
    const std::vector<uint8_t> rows(row * static_cast<std::size_t>(height));
    png::Image image{};
    image.header = header;
    image.rows = rows;
    std::vector<uint8_t> out;
    OA_CHECK(png::write_stored(image, &out));
    return out;
}

/// A mutator that reads.
///
/// @param minor the second version part; the first is 1
/// @param engine its engine requirement; empty when it names none
/// @param id its id
/// @return the manifest
mut::Manifest
manifest_of(uint16_t minor, std::string engine = {}, std::string id = std::string(kId)) {
    mut::Manifest manifest;
    manifest.format = mut::format_version;
    manifest.id = std::move(id);
    manifest.name = "Steady Hands";
    manifest.version.major = 1;
    manifest.version.minor = minor;
    manifest.version.parts = 2;
    manifest.author.name = "Ada";
    manifest.tags = {"economy"};
    manifest.works_with.push_back(mut::Condition{"*", {}, {}});
    manifest.layers = {mut::Layer::units};
    manifest.icon = "icons/steady.png";
    manifest.requires_engine = std::move(engine);
    return manifest;
}

/// The files a good mutator holds, and its manifest.
///
/// @param manifest the manifest
/// @param icon the icon
/// @param entry the entry script
/// @return the package's files
std::vector<Piece> pieces_of(
    const mut::Manifest& manifest,
    std::vector<uint8_t> icon,
    std::vector<uint8_t> entry = bytes_of("return {}\n")
) {
    std::vector<Piece> pieces;
    pieces.push_back({"oamut.yaml", bytes_of(mut::write_manifest(manifest))});
    pieces.push_back({manifest.entry, std::move(entry)});
    pieces.push_back({"lua/hands.lua", bytes_of("return {}\n")});
    pieces.push_back({"rules/economy.sm", bytes_of("sm\n")});
    pieces.push_back({"files/unit.fbi", bytes_of("fbi\n")});
    if (!manifest.icon.empty())
        pieces.push_back({manifest.icon, std::move(icon)});
    pieces.push_back({"README.md", bytes_of("# Steady Hands\n")});
    return pieces;
}

/// Writes a stored archive.
///
/// @param file where it goes
/// @param pieces its files
void write_zip(const fs::path& file, const std::vector<Piece>& pieces) {
    std::vector<zip::NewEntry> entries;
    entries.reserve(pieces.size());
    for (const Piece& piece : pieces)
        entries.push_back({piece.name, piece.bytes});
    std::vector<uint8_t> archive;
    zip::ZipError error{};
    OA_CHECK(zip::write_archive(entries, archive, error));
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(
        reinterpret_cast<const char*>(archive.data()), static_cast<std::streamsize>(archive.size())
    );
    OA_CHECK(static_cast<bool>(out));
}

/// Writes raw archive bytes.
///
/// @param file where it goes
/// @param bytes the archive
void write_bytes(const fs::path& file, const std::vector<uint8_t>& bytes) {
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(
        reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
    );
    OA_CHECK(static_cast<bool>(out));
}

/// Returns the SHA-256 of a file.
///
/// @param file the file
/// @return its digest
oa::base::sha256::Digest file_hash(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>{in}, {}};
    return oa::base::sha256::digest_of(bytes);
}

/// A catalogue origin for the mutator.
///
/// @param registry the registry
/// @param release the release
/// @param file the package, whose hash is recorded; empty records none yet
/// @return the origin
install::Origin catalogue_origin(std::string registry, int64_t release, const fs::path& file) {
    install::Origin origin{};
    origin.kind = install::OriginKind::catalogue;
    origin.registry = std::move(registry);
    origin.catalogue_id = std::string(kId);
    origin.release = release;
    origin.installed = "2000-01-01";
    if (!file.empty())
        origin.sha256 = file_hash(file);
    return origin;
}

/// A file origin with the date the checks record.
///
/// @return the origin
install::Origin file_origin() {
    install::Origin origin{};
    origin.installed = "2000-01-01";
    return origin;
}

/// What an install did.
struct Installed {
    install::InstallPlan plan{};
    install::ChangeResult result{};
    install::Problem problem{};
};

/// Opens a package, plans it and, when asked, puts the change in place.
///
/// @param file the package
/// @param mutators the Mutators folder
/// @param origin where it came from
/// @param commit the change to make; nothing plans only
/// @return what it did
Installed open_and_plan(
    const fs::path& file,
    const fs::path& mutators,
    const install::Origin& origin,
    const Change* commit
) {
    Installed done{};
    const install::PackageKind* kind = install::find_kind("oamut");
    OA_CHECK(kind != nullptr);
    if (kind == nullptr)
        return done;
    const auto opened = install::open_package(file);
    if (!opened.package) {
        done.problem = opened.problem;
        return done;
    }
    install::Incoming incoming = opened.package->incoming;
    incoming.origin = origin;
    done.plan = oamut::plan_install(incoming, install::folder_hooks(*kind, mutators));
    if (commit == nullptr)
        return done;
    const std::string folder =
        *commit == Change::install && !done.plan.alongside.empty() && done.plan.target.empty()
            ? done.plan.alongside
            : done.plan.target;
    const install::InstalledPackage expected = done.plan.replacing || *commit != Change::install
                                                   ? done.plan.installed
                                                   : install::InstalledPackage{};
    install::Unpacking unpacking;
    if (!unpacking.start(*opened.package, mutators, folder, expected, origin, done.problem))
        return done;
    auto step = zip::StreamStep::more;
    while (step == zip::StreamStep::more)
        step = unpacking.step(uint64_t{1} << 20, done.problem);
    if (step != zip::StreamStep::done) {
        unpacking.cancel();
        return done;
    }
    install::ChangeOptions options{};
    options.expected = expected;
    options.first_retry_ms = 0;
    done.result = install::commit_change(*kind, mutators, folder, *commit, options);
    install::Discarder discarder;
    discarder.add(done.result.discards);
    discarder.add(unpacking.discards());
    while (discarder.step()) {
    }
    return done;
}

/// Requires a refusal whose sentence holds a phrase, and that Mutators is untouched.
///
/// @param scratch the scratch folder
/// @param pieces the package
/// @param phrase the sentence holds this
/// @param refusal why
void expect_refused(
    const Scratch& scratch,
    std::vector<Piece> pieces,
    std::string_view phrase,
    install::Refusal refusal
) {
    const fs::path file = scratch.path() / "refused.oamut";
    const fs::path mutators = scratch.path() / "Mutators";
    write_zip(file, pieces);
    const auto opened = install::open_package(file);
    OA_CHECK(!opened.package);
    OA_CHECK(opened.problem.refusal == refusal);
    const std::string sentence = oamut::refusal_text(opened.problem);
    OA_CHECK(sentence.find(phrase) != std::string::npos);
    if (sentence.find(phrase) == std::string::npos)
        std::fprintf(stderr, "refusal: %s\n", sentence.c_str());
    OA_CHECK(!fs::exists(mutators));
}

/// Tells whether a prompt offers PLAY NOW.
///
/// @param prompt the prompt
/// @return true when a button answers play now
bool offers_play(const install::PackagePrompt& prompt) {
    for (const install::Answer answer : prompt.answers)
        if (answer == install::Answer::play_now)
            return true;
    return false;
}

void test_installs(const Scratch& scratch) {
    const fs::path mutators = scratch.path() / "install" / "Mutators";
    const fs::path packages = scratch.path() / "install";
    const std::vector<uint8_t> icon = png_image(16, 16, png::ColorType::grey);
    const fs::path first_file = packages / "steady-hands-1.0.oamut";
    write_zip(first_file, pieces_of(manifest_of(0), icon));
    const Installed asked = open_and_plan(first_file, mutators, file_origin(), nullptr);
    OA_CHECK(asked.plan.kind == install::PlanKind::ask_install);
    OA_CHECK(asked.plan.target == kId && asked.plan.alongside == kId);
    const Change install_change = Change::install;
    const Installed first = open_and_plan(first_file, mutators, file_origin(), &install_change);
    OA_CHECK(first.plan.kind == install::PlanKind::ask_install && first.result.changed);
    const fs::path folder = mutators / std::string(kId);
    OA_CHECK(fs::exists(folder / "oamut.yaml"));
    OA_CHECK(fs::exists(folder / "mutator.lua"));
    OA_CHECK(fs::exists(folder / "lua" / "hands.lua"));
    OA_CHECK(fs::exists(folder / "rules" / "economy.sm"));
    OA_CHECK(fs::exists(folder / "files" / "unit.fbi"));
    OA_CHECK(fs::exists(folder / "icons" / "steady.png"));
    OA_CHECK(fs::exists(folder / "README.md"));
    OA_CHECK(!fs::exists(folder / ".backup"));
    {
        const auto record = install::read_origin(folder);
        OA_CHECK(
            record && record->kind == install::OriginKind::file &&
            record->sha256 == file_hash(first_file)
        );
    }

    const Installed again = open_and_plan(first_file, mutators, file_origin(), nullptr);
    OA_CHECK(again.plan.kind == install::PlanKind::ask_reinstall);
    install::Incoming same{};
    same.name = "Steady Hands";
    same.version = "1.0";
    same.revision = 0;
    same.origin = file_origin();
    const install::PackagePrompt reinstall =
        oamut::question_prompt(again.plan, same, "steady-hands-1.0.oamut", mutators, true);
    OA_CHECK(reinstall.prompt.title == "REINSTALL MUTATOR");
    OA_CHECK(!reinstall.prompt.paragraphs.empty());
    OA_CHECK(reinstall.prompt.paragraphs[0].text.find("revision 0") != std::string::npos);
    OA_CHECK(!offers_play(reinstall));

    const fs::path second_file = packages / "steady-hands-1.1.oamut";
    write_zip(second_file, pieces_of(manifest_of(1), icon));
    const Installed update = open_and_plan(second_file, mutators, file_origin(), nullptr);
    OA_CHECK(update.plan.kind == install::PlanKind::ask_update);
    const Change replace_change = Change::replace;
    const Installed replaced = open_and_plan(second_file, mutators, file_origin(), &replace_change);
    OA_CHECK(replaced.plan.kind == install::PlanKind::ask_update && replaced.result.changed);
    OA_CHECK(replaced.result.backup_kept);
    OA_CHECK(oamut::read_installed(folder).version == "1.1");
    OA_CHECK(oamut::read_installed(folder / ".backup").version == "1.0");

    const install::PackageKind* kind = install::find_kind("oamut");
    OA_CHECK(kind != nullptr);
    if (kind != nullptr) {
        install::ChangeOptions back{};
        back.first_retry_ms = 0;
        const install::ChangeResult rolled =
            install::commit_change(*kind, mutators, std::string(kId), Change::roll_back, back);
        OA_CHECK(rolled.changed);
        OA_CHECK(oamut::read_installed(folder).version == "1.0");
        OA_CHECK(oamut::read_installed(folder / ".backup").version == "1.1");
    }
}

void test_catalogue(const Scratch& scratch) {
    const fs::path mutators = scratch.path() / "catalogue" / "Mutators";
    const fs::path packages = scratch.path() / "catalogue";
    const std::vector<uint8_t> icon = png_image(16, 16, png::ColorType::grey);
    const fs::path first_file = packages / "steady-hands-1.0.oamut";
    write_zip(first_file, pieces_of(manifest_of(0), icon));
    const Change install_change = Change::install;
    const Installed first = open_and_plan(
        first_file, mutators, catalogue_origin("ridge", 1, first_file), &install_change
    );
    OA_CHECK(first.plan.kind == install::PlanKind::install && !first.plan.replacing);
    OA_CHECK(first.result.changed);
    const fs::path folder = mutators / std::string(kId);
    {
        const auto record = install::read_origin(folder);
        OA_CHECK(
            record && record->kind == install::OriginKind::catalogue &&
            record->registry == "ridge" && record->sha256 == file_hash(first_file)
        );
    }

    const fs::path second_file = packages / "steady-hands-1.1.oamut";
    write_zip(second_file, pieces_of(manifest_of(1), icon));
    const Change replace_change = Change::replace;
    const Installed replaced = open_and_plan(
        second_file, mutators, catalogue_origin("ridge", 2, second_file), &replace_change
    );
    OA_CHECK(replaced.plan.kind == install::PlanKind::install && replaced.plan.replacing);
    OA_CHECK(replaced.result.changed && replaced.result.backup_kept);
    OA_CHECK(oamut::read_installed(folder).version == "1.1");
    OA_CHECK(fs::exists(folder / ".backup" / "oamut.yaml"));

    const fs::path third_file = packages / "steady-hands-2.oamut";
    mut::Manifest third = manifest_of(0);
    third.version.major = 2;
    third.version.minor = 0;
    write_zip(third_file, pieces_of(third, icon));
    const Installed other =
        open_and_plan(third_file, mutators, catalogue_origin("harbour", 1, third_file), nullptr);
    OA_CHECK(other.plan.kind == install::PlanKind::ask_version);
    install::Incoming harbour{};
    harbour.id = std::string(kId);
    harbour.name = "Steady Hands";
    harbour.version = "2.0";
    harbour.origin = catalogue_origin("harbour", 1, {});
    const install::PackagePrompt prompt =
        oamut::question_prompt(other.plan, harbour, "steady-hands-2.oamut", mutators, false);
    OA_CHECK(prompt.prompt.title == "REPLACE MUTATOR");
    OA_CHECK(!prompt.prompt.paragraphs.empty());
    OA_CHECK(
        prompt.prompt.paragraphs[0].text.find("from ridge") != std::string::npos &&
        prompt.prompt.paragraphs[0].text.find("from harbour") != std::string::npos
    );
    OA_CHECK(oamut::read_installed(folder).version == "1.1");
}

void test_not_a_pack(const Scratch& scratch) {
    const fs::path mutators = scratch.path() / "own" / "Mutators";
    const fs::path file = scratch.path() / "own" / "steady-hands.oamut";
    write_zip(file, pieces_of(manifest_of(0), png_image(16, 16, png::ColorType::grey)));
    const fs::path notes = mutators / std::string(kId) / "notes.txt";
    fs::create_directories(notes.parent_path());
    {
        std::ofstream out(notes, std::ios::binary);
        out << "kept\n";
    }
    OA_CHECK(oamut::read_installed(mutators / std::string(kId)).kind == install::FolderKind::other);
    const Installed refused = open_and_plan(file, mutators, file_origin(), nullptr);
    OA_CHECK(refused.plan.kind == install::PlanKind::refuse);
    OA_CHECK(refused.plan.refusal == install::Refusal::not_a_pack);
    install::Problem problem{};
    problem.refusal = refused.plan.refusal;
    problem.subject = std::string(kId);
    OA_CHECK(
        oamut::refusal_text(problem) ==
        "The folder steady-hands does not hold this mutator. It is left as it is."
    );
    std::ifstream in(notes, std::ios::binary);
    const std::string kept{std::istreambuf_iterator<char>{in}, {}};
    OA_CHECK(kept == "kept\n");
}

void test_uninstall(const Scratch& scratch) {
    const fs::path mutators = scratch.path() / "remove" / "Mutators";
    const fs::path file = scratch.path() / "remove" / "steady-hands.oamut";
    write_zip(file, pieces_of(manifest_of(0), png_image(16, 16, png::ColorType::grey)));
    const Change install_change = Change::install;
    const Installed done =
        open_and_plan(file, mutators, catalogue_origin("ridge", 1, file), &install_change);
    OA_CHECK(done.result.changed);
    const install::PackageKind* kind = install::find_kind("oamut");
    OA_CHECK(kind != nullptr);
    if (kind == nullptr)
        return;
    install::ChangeOptions options{};
    options.first_retry_ms = 0;
    const install::UninstallResult removed =
        install::uninstall_package(*kind, mutators, std::string(kId), options);
    OA_CHECK(removed.removed);
    install::Discarder discarder;
    discarder.add(removed.discards);
    while (discarder.step()) {
    }
    OA_CHECK(!fs::exists(mutators / std::string(kId)));
    if (fs::exists(mutators)) {
        for (const fs::directory_entry& entry : fs::directory_iterator(mutators)) {
            const std::string name = entry.path().filename().string();
            // The installer's lock file stays, as .oamod-lock does.
            OA_CHECK(
                name != std::string(kId) && (name == ".oamut-lock" || !name.starts_with(".oamut-"))
            );
        }
    }
}

void test_nested(const Scratch& scratch) {
    const fs::path mutators = scratch.path() / "nested" / "Mutators";
    const fs::path file = scratch.path() / "nested" / "bundled.oamut";
    std::vector<Piece> pieces = pieces_of(manifest_of(0), png_image(16, 16, png::ColorType::grey));
    for (Piece& piece : pieces)
        piece.name = "bundle/" + piece.name;
    write_zip(file, pieces);
    const Change install_change = Change::install;
    const Installed done =
        open_and_plan(file, mutators, catalogue_origin("ridge", 1, file), &install_change);
    OA_CHECK(done.plan.kind == install::PlanKind::install && done.result.changed);
    const fs::path folder = mutators / std::string(kId);
    OA_CHECK(fs::exists(folder / "oamut.yaml"));
    OA_CHECK(fs::exists(folder / "mutator.lua"));
    OA_CHECK(!fs::exists(mutators / "bundle"));
}

void test_refusals(const Scratch& scratch) {
    const std::vector<uint8_t> icon = png_image(16, 16, png::ColorType::grey);
    const mut::Manifest good = manifest_of(0);

    expect_refused(
        scratch,
        {{"mutator.lua", bytes_of("return {}\n")}},
        "It holds no oamut.yaml at its top, or in a single folder at its top.",
        install::Refusal::no_manifest
    );

    expect_refused(
        scratch,
        {{"oamut.yaml",
          bytes_of(
              "oamut: 1\n"
              "id: \"steady-hands\"\n"
              "name: \"Steady Hands\"\n"
              "version: \"1.0\"\n"
              "author:\n"
              "  name: \"Ada\"\n"
          )},
         {"mutator.lua", bytes_of("return {}\n")}},
        "Its oamut.yaml has errors:",
        install::Refusal::manifest_errors
    );
    {
        const fs::path file = scratch.path() / "broken.oamut";
        write_zip(
            file,
            {{"oamut.yaml",
              bytes_of(
                  "oamut: 1\n"
                  "id: \"steady-hands\"\n"
                  "name: \"Steady Hands\"\n"
                  "version: \"1.0\"\n"
                  "author:\n"
                  "  name: \"Ada\"\n"
              )},
             {"mutator.lua", bytes_of("return {}\n")}}
        );
        const auto opened = install::open_package(file);
        OA_CHECK(!opened.package && !opened.problem.lines.empty());
    }

    auto missing = pieces_of(good, icon);
    missing.erase(
        std::remove_if(
            missing.begin(),
            missing.end(),
            [](const Piece& piece) { return piece.name == "mutator.lua"; }
        ),
        missing.end()
    );
    expect_refused(
        scratch, missing, "Its script mutator.lua is missing.", install::Refusal::script_missing
    );

    expect_refused(
        scratch,
        pieces_of(good, icon, {0x1B, 'L', 'u', 'a'}),
        "mutator.lua is precompiled Lua.",
        install::Refusal::precompiled
    );

    auto module = pieces_of(good, icon);
    for (Piece& piece : module)
        if (piece.name == "lua/hands.lua")
            piece.bytes = {0x1B, 'L', 'u', 'a'};
    expect_refused(
        scratch, module, "lua/hands.lua is precompiled Lua.", install::Refusal::precompiled
    );

    std::vector<uint8_t> huge(
        static_cast<std::size_t>(oamut::most_script_bytes) + 1, static_cast<uint8_t>('a')
    );
    expect_refused(
        scratch,
        pieces_of(good, icon, std::move(huge)),
        "is larger than 1 MiB.",
        install::Refusal::script_missing
    );

    auto notes_under_lua = pieces_of(good, icon);
    notes_under_lua.push_back({"lua/notes.txt", bytes_of("note\n")});
    expect_refused(
        scratch,
        notes_under_lua,
        "The package holds lua/notes.txt, outside the folders a mutator may hold.",
        install::Refusal::outside_layout
    );

    auto stray = pieces_of(good, icon);
    stray.push_back({"extra.bin", bytes_of("bin\n")});
    expect_refused(
        scratch,
        stray,
        "The package holds extra.bin, outside the folders a mutator may hold.",
        install::Refusal::outside_layout
    );

    expect_refused(
        scratch,
        pieces_of(good, bytes_of("not a png")),
        "Its icon icons/steady.png is not a square PNG of at most 256 pixels and 256 KiB.",
        install::Refusal::icon_invalid
    );

    const std::vector<uint8_t> wide = png_image(300, 300, png::ColorType::grey);
    OA_CHECK(wide.size() <= static_cast<std::size_t>(oamut::most_icon_bytes));
    expect_refused(
        scratch,
        pieces_of(good, wide),
        "Its icon icons/steady.png is not a square PNG of at most 256 pixels and 256 KiB.",
        install::Refusal::icon_invalid
    );

    expect_refused(
        scratch,
        pieces_of(good, png_image(200, 100, png::ColorType::grey)),
        "Its icon icons/steady.png is not a square PNG of at most 256 pixels and 256 KiB.",
        install::Refusal::icon_invalid
    );

    const std::vector<uint8_t> heavy = png_image(512, 512, png::ColorType::grey);
    OA_CHECK(heavy.size() > static_cast<std::size_t>(oamut::most_icon_bytes));
    expect_refused(
        scratch,
        pieces_of(good, heavy),
        "Its icon icons/steady.png is not a square PNG of at most 256 pixels and 256 KiB.",
        install::Refusal::icon_invalid
    );

    expect_refused(
        scratch,
        pieces_of(manifest_of(0, ">= 9999.0.0"), icon),
        "needs Open Annihilation 9999.0.0 or later; this is Open Annihilation ",
        install::Refusal::engine_unmet
    );

    expect_refused(
        scratch,
        pieces_of(manifest_of(0, {}, "con"), icon),
        "Its id, con, cannot name a folder on Windows.",
        install::Refusal::reserved_id
    );

    std::vector<raw::RawFile> deep{raw::stored_text("oamut.yaml", "oamut: 1\n")};
    std::string chain;
    for (int part = 0; part < 253; ++part)
        chain += "a/";
    for (std::size_t top = 0; top * 254 <= install::max_package_folders; ++top)
        deep.push_back(raw::stored_text(std::to_string(top) + "/" + chain + "x", ""));
    const fs::path deep_file = scratch.path() / "deep.oamut";
    write_bytes(deep_file, raw::build_raw_archive(deep).bytes);
    const auto opened = install::open_package(deep_file);
    OA_CHECK(!opened.package && opened.problem.refusal == install::Refusal::too_many_folders);
    OA_CHECK(
        oamut::refusal_text(opened.problem).find("more than a mutator may take") !=
        std::string::npos
    );
    OA_CHECK(!fs::exists(scratch.path() / "Mutators"));
}

void test_installed_prompt() {
    install::Incoming incoming{};
    incoming.name = "Steady Hands";
    incoming.version = "1.0";
    const install::PackagePrompt made =
        oamut::installed_prompt(incoming, fs::path("Mutators") / "steady-hands", true);
    OA_CHECK(made.prompt.title == "MUTATOR INSTALLED");
    OA_CHECK(!made.prompt.paragraphs.empty());
    OA_CHECK(
        made.prompt.paragraphs[0].text.find("Turn it on with the Mutators button") !=
        std::string::npos
    );
    OA_CHECK(!offers_play(made));
    install::ChangeResult result{};
    result.changed = true;
    const install::PackagePrompt updated = oamut::updated_prompt(
        Change::replace,
        incoming,
        install::InstalledPackage{},
        fs::path("Mutators") / "steady-hands",
        result,
        true
    );
    OA_CHECK(updated.prompt.title == "MUTATOR UPDATED");
    OA_CHECK(!offers_play(updated));
}

} // namespace

int main() {
    const Scratch scratch;
    test_installs(scratch);
    test_catalogue(scratch);
    test_not_a_pack(scratch);
    test_uninstall(scratch);
    test_nested(scratch);
    test_refusals(scratch);
    test_installed_prompt();
    return oa::test::check_exit_status();
}
