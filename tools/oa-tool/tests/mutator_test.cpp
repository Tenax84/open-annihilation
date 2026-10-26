// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// oa-tool pack and check for a made-up mutator, run in-process. The folder
// packs to a .oamut in a fixed order, a second pack after the files' times
// change is the same bytes, and check reports the catalogue facts. A broken
// manifest, script, rule file, layout or icon is refused by pack and by check.

#include "command.hpp"

#include "oa/base/sha256.hpp"
#include "oa/formats/json.hpp"
#include "oa/formats/png.hpp"
#include "oa/formats/zip.hpp"
#include "oa/formats/zip/stream.hpp"
#include "oa/platform/files.hpp"
#include "oa/test/check.hpp"
#include "oa/test/scratch_directory.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace json = oa::formats::json;
namespace png = oa::formats::png;
namespace sha256 = oa::base::sha256;
namespace zip = oa::formats::zip;

/// What is wrong with a made-up mutator, or nothing.
enum class Fault : uint8_t {
    none,
    manifest,
    lua,
    precompiled,
    rules,
    layout,
    icon,
};

struct Scratch {
    fs::path path;

    Scratch() : path(oa::test::make_scratch_directory("oa-tool-mutator")) {}

    ~Scratch() {
        std::error_code error;
        fs::remove_all(path, error);
    }

    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
};

struct Captured {
    int status = 0;
    std::string out;
    std::string err;
};

/// A manifest that names every key. `version` is the version text.
constexpr std::string_view kManifest =
    "oamut: 1\n"
    "id: steady-hands\n"
    "name: \"Steady Hands\"\n"
    "version: \"2.0\"\n"
    "author:\n"
    "  name: \"A. Maker\"\n"
    "  email: \"maker@example.com\"\n"
    "description: \"Hands stay steady.\\nThe second line stays in the description.\"\n"
    "homepage: \"https://example.com/steady\"\n"
    "icon: \"icons/badge.png\"\n"
    "tags: [\"economy\", \"gameplay\"]\n"
    "works-with: [\"ridge >= 1\", \"ta-3.1c\"]\n"
    "requires:\n"
    "  engine: \">= 0.8.5\"\n"
    "  mutators: [\"long-shadows >= 1\", \"calm-seas\"]\n"
    "conflicts-with: [\"deep-water < 2\", \"shaky-grip\"]\n"
    "layers: [\"files\", \"units\"]\n"
    "options:\n"
    "  factor:\n"
    "    type: choice\n"
    "    label: \"Build speed\"\n"
    "    values: [2, 4, 8]\n"
    "    labels: [\"Double\", \"Four\", \"Eight\"]\n"
    "    default: 2\n"
    "  announce:\n"
    "    type: toggle\n"
    "    label: \"Announce\"\n"
    "    default: true\n"
    "  pace:\n"
    "    type: number\n"
    "    label: \"Pace\"\n"
    "    min: 0.5\n"
    "    max: 2.5\n"
    "    step: 0.5\n"
    "    default: 1.5\n"
    "strings:\n"
    "  de:\n"
    "    name: \"Feste Haende\"\n"
    "    description: \"Haende bleiben fest.\"\n"
    "    options:\n"
    "      factor: \"Bautempo\"\n"
    "    labels:\n"
    "      factor: [\"Doppelt\", \"Vier\", \"Acht\"]\n"
    "entry: \"mutator.lua\"\n"
    "legacy:\n"
    "  - name: \"First Original\"\n"
    "    prefix: \"{}\"\n"
    "    compatibility: [\"AB\"]\n"
    "    files: [\"first.sm\"]\n";

constexpr std::string_view kRule = "[*Group*]\n"
                                   "{\n"
                                   "[Needs]\n"
                                   "{\n"
                                   "Side=\"Core\";\n"
                                   "}\n"
                                   "[Changes]\n"
                                   "{\n"
                                   "Kept=1;\n"
                                   "}\n"
                                   "}\n";

constexpr std::string_view kBrokenRule = "[*Group*]{[Needs]{A=hello;}[Changes]{Kept=9;}}\n";

Captured run(std::vector<std::string> arguments) {
    std::ostringstream out;
    std::ostringstream err;
    oa::tool::Output output{out, err};
    const int status = oa::tool::run_tool(arguments, output);
    return {status, out.str(), err.str()};
}

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

void write_text(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    OA_CHECK(static_cast<bool>(out));
}

void write_bytes(const fs::path& path, std::span<const uint8_t> bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(
        reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
    );
    OA_CHECK(static_cast<bool>(out));
}

std::vector<uint8_t> read_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// Writes a stored PNG.
///
/// @param side its width and height
/// @return the file; empty when it could not be written
std::vector<uint8_t> png_image(uint32_t side) {
    png::Header header{};
    header.width = side;
    header.height = side;
    header.bit_depth = 8;
    header.color_type = png::ColorType::grey;
    const std::size_t row = png::row_bytes(header, {});
    const std::vector<uint8_t> rows(row * static_cast<std::size_t>(side));
    png::Image image{};
    image.header = header;
    image.rows = rows;
    std::vector<uint8_t> out;
    OA_CHECK(png::write_stored(image, &out));
    return out;
}

/// Writes the made-up mutator, with one fault when `fault` names one.
///
/// @param folder the folder
/// @param fault what is broken
/// @param icon the icon's bytes, so a check can hash the same file
void make_mutator(const fs::path& folder, Fault fault, const std::vector<uint8_t>& icon) {
    std::string manifest{kManifest};
    if (fault == Fault::manifest)
        manifest.replace(
            manifest.find("version: \"2.0\""),
            std::string("version: \"2.0\"").size(),
            "version: \"nope\""
        );
    write_text(folder / "oamut.yaml", manifest);
    write_text(folder / "README.md", "A made-up mutator.\n");
    write_text(folder / "files" / "units" / "x.fbi", "unit\n");
    const uint32_t side = fault == Fault::icon ? 300 : 64;
    const std::vector<uint8_t> picture = side == 64 ? icon : png_image(side);
    write_bytes(folder / "icons" / "badge.png", picture);
    if (fault == Fault::lua)
        write_text(folder / "lua" / "util.lua", "local value = 1\nreturn (\n");
    else
        write_text(folder / "lua" / "util.lua", "local value = 1\nreturn value\n");
    if (fault == Fault::precompiled) {
        const uint8_t binary[] = {0x1B, 'L', 'u', 'a'};
        write_bytes(folder / "mutator.lua", binary);
    } else {
        write_text(folder / "mutator.lua", "local value = 1\nreturn value\n");
    }
    write_text(folder / "rules" / "steady.sm", fault == Fault::rules ? kBrokenRule : kRule);
    if (fault == Fault::layout)
        write_text(folder / "stray.bin", "nope\n");
}

struct OpenFile {
    std::FILE* stream{};
    oa::platform::Files files{};

    OpenFile() = default;
    OpenFile(const OpenFile&) = delete;
    OpenFile& operator=(const OpenFile&) = delete;

    ~OpenFile() {
        if (stream != nullptr)
            std::fclose(stream);
    }
};

bool read_at(void* context, uint64_t offset, std::span<uint8_t> bytes) {
    auto& file = *static_cast<OpenFile*>(context);
    auto* handle = reinterpret_cast<oa::platform::FileHandle*>(file.stream);
    if (file.stream == nullptr ||
        file.files.seek(
            nullptr, handle, static_cast<int64_t>(offset), oa::platform::SeekOrigin::begin
        ) != 0)
        return false;
    std::size_t done = 0;
    while (done < bytes.size()) {
        const std::size_t got =
            std::fread(bytes.data() + done, 1, bytes.size() - done, file.stream);
        if (got == 0)
            return false;
        done += got;
    }
    return true;
}

bool read_directory(const fs::path& path, zip::StreamDirectory& directory) {
    OpenFile file;
    file.stream = oa::platform::open_file(path, "rb");
    if (file.stream == nullptr)
        return false;
    file.files = oa::platform::stdio_files();
    auto* handle = reinterpret_cast<oa::platform::FileHandle*>(file.stream);
    if (file.files.seek(nullptr, handle, 0, oa::platform::SeekOrigin::end) != 0)
        return false;
    const int64_t end = file.files.tell(nullptr, handle);
    if (end < 0)
        return false;
    zip::ZipError error{};
    return zip::read_stream_directory(
        {&file, read_at}, static_cast<uint64_t>(end), {}, directory, error
    );
}

/// Packs the folder's files into a stored zip so check can read a refusal
/// pack itself will not leave behind. `prefix` is a top folder and its
/// slash, the shape Finder writes, or empty for a flat package.
///
/// @param folder the folder
/// @param file the zip
/// @param prefix a top folder to put every file under, or empty
void zip_folder(const fs::path& folder, const fs::path& file, std::string_view prefix = {}) {
    std::vector<std::string> names;
    std::vector<std::vector<uint8_t>> blobs;
    for (fs::recursive_directory_iterator cursor(folder), end; cursor != end; ++cursor) {
        if (cursor->is_directory())
            continue;
        const std::string relative = cursor->path().lexically_relative(folder).generic_string();
        names.push_back(std::string{prefix} + relative);
        blobs.push_back(read_bytes(cursor->path()));
    }
    std::vector<zip::NewEntry> entries;
    entries.reserve(names.size());
    for (std::size_t index = 0; index < names.size(); ++index)
        entries.push_back(zip::NewEntry{names[index], blobs[index]});
    std::vector<uint8_t> archive;
    zip::ZipError error{};
    OA_CHECK(zip::write_archive(entries, archive, error));
    write_bytes(file, archive);
}

const json::Json* member(const json::Json& object, std::string_view name) {
    return object.find(name);
}

std::string text_of(const json::Json* value) {
    if (value == nullptr || value->string() == nullptr)
        return {};
    return *value->string();
}

void touch_tree(const fs::path& folder) {
    const auto when = fs::file_time_type::clock::now() + std::chrono::hours(48);
    std::error_code error;
    fs::last_write_time(folder, when, error);
    for (fs::recursive_directory_iterator cursor(folder), end; cursor != end; ++cursor)
        fs::last_write_time(cursor->path(), when, error);
}

/// The keys a mutator's report carries, in the order check writes them.
constexpr std::string_view kKeys[] = {
    "check",
    "ok",
    "problems",
    "warnings",
    "file",
    "kind",
    "id",
    "name",
    "version",
    "revision",
    "size",
    "sha256",
    "unpacked_size",
    "files",
    "summary",
    "description",
    "homepage",
    "tags",
    "author",
    "requires",
    "works_with",
    "conflicts_with",
    "layers",
    "options",
    "icon",
    "entry",
    "scripts",
    "rules",
    "legacy",
};

void expect_facts(
    const json::Json& report,
    const std::vector<uint8_t>& icon,
    std::string_view file,
    std::optional<std::string_view> clashes = std::nullopt
) {
    std::vector<std::string_view> keys(std::begin(kKeys), std::end(kKeys));
    if (clashes.has_value()) {
        for (std::size_t index = 0; index < keys.size(); ++index) {
            if (keys[index] == "layers") {
                keys.insert(keys.begin() + static_cast<std::ptrdiff_t>(index) + 1, "clashes");
                break;
            }
        }
    }
    const std::span<const std::string> names = report.names();
    OA_CHECK(names.size() == keys.size());
    if (names.size() == keys.size()) {
        for (std::size_t index = 0; index < names.size(); ++index)
            OA_CHECK(names[index] == keys[index]);
    }
    if (clashes.has_value())
        OA_CHECK(text_of(member(report, "clashes")) == *clashes);
    else
        OA_CHECK(member(report, "clashes") == nullptr);
    OA_CHECK(member(report, "ok") != nullptr && member(report, "ok")->boolean() == true);
    OA_CHECK(text_of(member(report, "kind")) == "oamut");
    OA_CHECK(
        member(report, "revision") != nullptr &&
        member(report, "revision")->type() == json::JsonType::null
    );
    OA_CHECK(text_of(member(report, "id")) == "steady-hands");
    OA_CHECK(text_of(member(report, "name")) == "Steady Hands");
    OA_CHECK(text_of(member(report, "version")) == "2.0");
    OA_CHECK(text_of(member(report, "file")) == file);
    OA_CHECK(text_of(member(report, "summary")) == "Hands stay steady.");
    OA_CHECK(
        text_of(member(report, "description")) ==
        "Hands stay steady.\nThe second line stays in the description."
    );
    OA_CHECK(text_of(member(report, "homepage")) == "https://example.com/steady");
    OA_CHECK(text_of(member(report, "author")) == "A. Maker");
    OA_CHECK(text_of(member(report, "entry")) == "mutator.lua");
    OA_CHECK(member(report, "scripts") != nullptr && member(report, "scripts")->integer() == 2);

    const json::Json* tags = member(report, "tags");
    OA_CHECK(tags != nullptr && tags->elements().size() == 2);
    if (tags != nullptr && tags->elements().size() == 2) {
        OA_CHECK(text_of(&tags->elements()[0]) == "economy");
        OA_CHECK(text_of(&tags->elements()[1]) == "gameplay");
    }

    const json::Json* needed = member(report, "requires");
    OA_CHECK(needed != nullptr);
    if (needed != nullptr) {
        OA_CHECK(text_of(needed->find("engine")) == ">= 0.8.5");
        const json::Json* mutators = needed->find("mutators");
        OA_CHECK(mutators != nullptr && mutators->elements().size() == 2);
        if (mutators != nullptr && mutators->elements().size() == 2) {
            const json::Json& first = mutators->elements()[0];
            OA_CHECK(first.names().size() == 2);
            OA_CHECK(text_of(first.find("id")) == "long-shadows");
            OA_CHECK(text_of(first.find("version")) == ">= 1");
            const json::Json& second = mutators->elements()[1];
            OA_CHECK(second.names().size() == 1);
            OA_CHECK(text_of(second.find("id")) == "calm-seas");
            OA_CHECK(second.find("version") == nullptr);
        }
    }

    const json::Json* works = member(report, "works_with");
    OA_CHECK(works != nullptr && works->elements().size() == 2);
    if (works != nullptr && works->elements().size() == 2) {
        OA_CHECK(text_of(&works->elements()[0]) == "ridge >= 1");
        OA_CHECK(text_of(&works->elements()[1]) == "ta-3.1c");
    }

    const json::Json* conflicts = member(report, "conflicts_with");
    OA_CHECK(conflicts != nullptr && conflicts->elements().size() == 2);
    if (conflicts != nullptr && conflicts->elements().size() == 2) {
        OA_CHECK(text_of(conflicts->elements()[0].find("id")) == "deep-water");
        OA_CHECK(text_of(conflicts->elements()[0].find("version")) == "< 2");
        OA_CHECK(conflicts->elements()[1].names().size() == 1);
        OA_CHECK(text_of(conflicts->elements()[1].find("id")) == "shaky-grip");
    }

    const json::Json* layers = member(report, "layers");
    OA_CHECK(layers != nullptr && layers->elements().size() == 2);
    if (layers != nullptr && layers->elements().size() == 2) {
        OA_CHECK(text_of(&layers->elements()[0]) == "files");
        OA_CHECK(text_of(&layers->elements()[1]) == "units");
    }

    const json::Json* options = member(report, "options");
    OA_CHECK(options != nullptr && options->elements().size() == 3);
    if (options != nullptr && options->elements().size() == 3) {
        const json::Json& choice = options->elements()[0];
        OA_CHECK(text_of(choice.find("id")) == "factor");
        OA_CHECK(text_of(choice.find("label")) == "Build speed");
        OA_CHECK(text_of(choice.find("type")) == "choice");
        OA_CHECK(
            choice.find("default") != nullptr && choice.find("default")->number_text() != nullptr &&
            *choice.find("default")->number_text() == "2"
        );
        const json::Json* values = choice.find("values");
        OA_CHECK(values != nullptr && values->elements().size() == 3);
        if (values != nullptr && values->elements().size() == 3) {
            OA_CHECK(
                values->elements()[0].number_text() != nullptr &&
                *values->elements()[0].number_text() == "2"
            );
            OA_CHECK(
                values->elements()[1].number_text() != nullptr &&
                *values->elements()[1].number_text() == "4"
            );
            OA_CHECK(
                values->elements()[2].number_text() != nullptr &&
                *values->elements()[2].number_text() == "8"
            );
        }
        const json::Json* labels = choice.find("labels");
        OA_CHECK(labels != nullptr && labels->elements().size() == 3);
        if (labels != nullptr && labels->elements().size() == 3) {
            OA_CHECK(text_of(&labels->elements()[0]) == "Double");
            OA_CHECK(text_of(&labels->elements()[1]) == "Four");
            OA_CHECK(text_of(&labels->elements()[2]) == "Eight");
        }
        const json::Json& toggle = options->elements()[1];
        OA_CHECK(text_of(toggle.find("type")) == "toggle");
        OA_CHECK(toggle.find("default") != nullptr && toggle.find("default")->boolean() == true);
        const json::Json& number = options->elements()[2];
        OA_CHECK(text_of(number.find("id")) == "pace");
        OA_CHECK(text_of(number.find("type")) == "number");
        OA_CHECK(
            number.find("min") != nullptr && number.find("min")->number_text() != nullptr &&
            *number.find("min")->number_text() == "0.5"
        );
        OA_CHECK(
            number.find("max") != nullptr && number.find("max")->number_text() != nullptr &&
            *number.find("max")->number_text() == "2.5"
        );
        OA_CHECK(
            number.find("step") != nullptr && number.find("step")->number_text() != nullptr &&
            *number.find("step")->number_text() == "0.5"
        );
        OA_CHECK(
            number.find("default") != nullptr && number.find("default")->number_text() != nullptr &&
            *number.find("default")->number_text() == "1.5"
        );
    }

    const json::Json* icon_fact = member(report, "icon");
    const auto hex = sha256::to_hex(sha256::digest_of(icon));
    OA_CHECK(icon_fact != nullptr);
    if (icon_fact != nullptr) {
        OA_CHECK(text_of(icon_fact->find("path")) == "icons/badge.png");
        OA_CHECK(text_of(icon_fact->find("sha256")) == std::string_view(hex.data(), hex.size()));
        OA_CHECK(
            icon_fact->find("size") != nullptr &&
            icon_fact->find("size")->integer() == static_cast<int64_t>(icon.size())
        );
        OA_CHECK(icon_fact->find("width") != nullptr && icon_fact->find("width")->integer() == 64);
        OA_CHECK(
            icon_fact->find("height") != nullptr && icon_fact->find("height")->integer() == 64
        );
    }

    const json::Json* rules = member(report, "rules");
    OA_CHECK(rules != nullptr && rules->elements().size() == 1);
    if (rules != nullptr && !rules->elements().empty())
        OA_CHECK(text_of(&rules->elements()[0]) == "rules/steady.sm");

    const json::Json* legacy = member(report, "legacy");
    OA_CHECK(legacy != nullptr && legacy->elements().size() == 1);
    if (legacy != nullptr && !legacy->elements().empty()) {
        const json::Json& record = legacy->elements()[0];
        OA_CHECK(text_of(record.find("name")) == "First Original");
        OA_CHECK(text_of(record.find("prefix")) == "{}");
        const json::Json* codes = record.find("compatibility");
        const json::Json* files = record.find("files");
        OA_CHECK(
            codes != nullptr && codes->elements().size() == 1 &&
            text_of(&codes->elements()[0]) == "AB"
        );
        OA_CHECK(
            files != nullptr && files->elements().size() == 1 &&
            text_of(&files->elements()[0]) == "first.sm"
        );
    }
}

json::Json parse_report(std::string_view text) {
    json::JsonError error{};
    const std::optional<json::Json> parsed = json::parse_json(text, error);
    OA_CHECK(parsed.has_value() && parsed->type() == json::JsonType::object);
    if (!parsed)
        return json::Json{};
    return *parsed;
}

void test_pack_and_check(const fs::path& scratch) {
    const std::vector<uint8_t> icon = png_image(64);
    const fs::path folder = scratch / "steady";
    make_mutator(folder, Fault::none, icon);
    const fs::path out = scratch / "steady-hands-2.0.oamut";
    const Captured packed = run({"pack", "--out", out.string(), folder.string()});
    OA_CHECK(packed.status == oa::tool::exit_done);
    if (packed.status != oa::tool::exit_done)
        std::fprintf(stderr, "pack: %s\n", packed.err.c_str());
    OA_CHECK(packed.err.empty());
    OA_CHECK(!fs::exists(fs::path(out.string() + ".part")));

    zip::StreamDirectory directory;
    OA_CHECK(read_directory(out, directory));
    const std::pair<std::string_view, zip::Method> expect[] = {
        {"oamut.yaml", zip::Method::deflated},
        {"README.md", zip::Method::deflated},
        {"files/units/x.fbi", zip::Method::deflated},
        {"icons/badge.png", zip::Method::stored},
        {"lua/util.lua", zip::Method::deflated},
        {"mutator.lua", zip::Method::deflated},
        {"rules/steady.sm", zip::Method::deflated},
    };
    OA_CHECK(directory.entries.size() == 7);
    if (directory.entries.size() == 7) {
        for (std::size_t index = 0; index < 7; ++index) {
            OA_CHECK(directory.entries[index].name == expect[index].first);
            OA_CHECK(directory.entries[index].method == expect[index].second);
        }
    }

    touch_tree(folder);
    const fs::path again = scratch / "steady-again.oamut";
    const Captured repacked = run({"pack", folder.string(), "--out", again.string()});
    OA_CHECK(repacked.status == oa::tool::exit_done);
    OA_CHECK(read_bytes(out) == read_bytes(again));

    const Captured checked = run({"check", out.string(), "--json"});
    OA_CHECK(checked.status == oa::tool::exit_done);
    if (checked.status != oa::tool::exit_done)
        std::fprintf(stderr, "check: %s\n%s\n", checked.out.c_str(), checked.err.c_str());
    const Captured grouped = run({"mutator", "check", "--json", out.string()});
    OA_CHECK(grouped.status == oa::tool::exit_done);
    OA_CHECK(checked.out == grouped.out);
    expect_facts(parse_report(checked.out), icon, out.filename().string());

    const Captured text = run({"check", out.string()});
    OA_CHECK(text.status == oa::tool::exit_done);
    OA_CHECK(text.out.size() >= std::string("result: ok\n").size());
    if (text.out.size() >= std::string("result: ok\n").size())
        OA_CHECK(text.out.ends_with("result: ok\n"));
    OA_CHECK(!contains(text.out, "clashes:"));

    const fs::path cwd = scratch / "cwd";
    fs::create_directories(cwd);
    {
        const fs::path previous = fs::current_path();
        fs::current_path(cwd);
        const Captured named = run({"pack", folder.string()});
        fs::current_path(previous);
        OA_CHECK(named.status == oa::tool::exit_done);
        OA_CHECK(contains(named.out, "packed steady-hands-2.0.oamut: "));
    }
    OA_CHECK(fs::exists(cwd / "steady-hands-2.0.oamut"));
}

void test_refusal(const fs::path& scratch, Fault fault, std::string_view snippet) {
    const std::vector<uint8_t> icon = png_image(64);
    const fs::path folder = scratch / ("fault-" + std::to_string(static_cast<unsigned>(fault)));
    make_mutator(folder, fault, icon);
    const fs::path out = scratch / (folder.filename().string() + ".oamut");
    const Captured packed = run({"pack", folder.string(), "--out", out.string()});
    OA_CHECK(packed.status == oa::tool::exit_failed);
    OA_CHECK(contains(packed.err, snippet));
    OA_CHECK(!fs::exists(out));
    OA_CHECK(!fs::exists(fs::path(out.string() + ".part")));

    const fs::path zip = scratch / (folder.filename().string() + "-stored.oamut");
    zip_folder(folder, zip);
    const Captured checked = run({"check", zip.string()});
    OA_CHECK(checked.status == oa::tool::exit_failed);
    const std::string both = checked.out + checked.err;
    OA_CHECK(contains(both, snippet));
    const Captured grouped = run({"mutator", "check", zip.string()});
    OA_CHECK(grouped.status == oa::tool::exit_failed);
    OA_CHECK(grouped.out == checked.out);
}

void test_refusals(const fs::path& scratch) {
    test_refusal(scratch, Fault::manifest, "oamut.yaml:");
    test_refusal(scratch, Fault::lua, "lua/util.lua:3:");
    test_refusal(scratch, Fault::precompiled, "precompiled");
    test_refusal(scratch, Fault::rules, "rules/steady.sm:1:");
    test_refusal(scratch, Fault::layout, "stray.bin");
    test_refusal(scratch, Fault::icon, "256");
}

/// A package whose files sit under one top folder is checked like a flat one.
void test_top_folder(const fs::path& scratch) {
    const std::vector<uint8_t> icon = png_image(64);
    const fs::path folder = scratch / "wrap-good";
    make_mutator(folder, Fault::none, icon);
    const fs::path flat = scratch / "wrap-flat.oamut";
    const Captured packed = run({"pack", "--out", flat.string(), folder.string()});
    OA_CHECK(packed.status == oa::tool::exit_done);
    const fs::path wrapped = scratch / "wrap-nested.oamut";
    zip_folder(folder, wrapped, "steady/");
    const Captured flat_check = run({"check", flat.string(), "--json"});
    const Captured wrap_check = run({"check", wrapped.string(), "--json"});
    OA_CHECK(flat_check.status == oa::tool::exit_done);
    OA_CHECK(wrap_check.status == oa::tool::exit_done);
    if (wrap_check.status != oa::tool::exit_done)
        std::fprintf(stderr, "wrapped: %s\n%s\n", wrap_check.out.c_str(), wrap_check.err.c_str());
    const json::Json flat_report = parse_report(flat_check.out);
    const json::Json wrap_report = parse_report(wrap_check.out);
    expect_facts(flat_report, icon, flat.filename().string());
    expect_facts(wrap_report, icon, wrapped.filename().string());
    const std::span<const std::string> flat_names = flat_report.names();
    const std::span<const std::string> wrap_names = wrap_report.names();
    OA_CHECK(flat_names.size() == wrap_names.size());
    if (flat_names.size() == wrap_names.size()) {
        for (std::size_t index = 0; index < flat_names.size(); ++index)
            OA_CHECK(flat_names[index] == wrap_names[index]);
    }

    const fs::path broken_folder = scratch / "wrap-broken";
    make_mutator(broken_folder, Fault::lua, icon);
    const fs::path broken = scratch / "wrap-broken.oamut";
    zip_folder(broken_folder, broken, "steady/");
    const Captured refused = run({"check", broken.string()});
    OA_CHECK(refused.status == oa::tool::exit_failed);
    OA_CHECK(contains(refused.out + refused.err, "lua/util.lua:3:"));
}

/// A manifest's clash mode is reported, and a manifest without one leaves it out.
void test_clash_modes(const fs::path& scratch) {
    const std::vector<uint8_t> icon = png_image(64);
    const std::string_view modes[] = {"add", "replace"};
    for (const std::string_view mode : modes) {
        const std::string stem = std::string("clash-") + std::string(mode);
        const fs::path folder = scratch / stem;
        make_mutator(folder, Fault::none, icon);
        std::string manifest{kManifest};
        const std::string layers = "layers: [\"files\", \"units\"]\n";
        const auto at = manifest.find(layers);
        OA_CHECK(at != std::string::npos);
        if (at != std::string::npos) {
            manifest.insert(at + layers.size(), "clashes: \"" + std::string(mode) + "\"\n");
        }
        write_text(folder / "oamut.yaml", manifest);
        const fs::path out = scratch / (stem + ".oamut");
        const Captured packed = run({"pack", "--out", out.string(), folder.string()});
        OA_CHECK(packed.status == oa::tool::exit_done);
        if (packed.status != oa::tool::exit_done)
            std::fprintf(stderr, "pack %s: %s\n", stem.c_str(), packed.err.c_str());
        const Captured checked = run({"mutator", "check", "--json", out.string()});
        OA_CHECK(checked.status == oa::tool::exit_done);
        expect_facts(parse_report(checked.out), icon, out.filename().string(), mode);
        const Captured text = run({"check", out.string()});
        OA_CHECK(text.status == oa::tool::exit_done);
        OA_CHECK(contains(text.out, "clashes: " + std::string(mode) + "\n"));
        OA_CHECK(text.out.ends_with("result: ok\n"));
    }
}

void test_mutator_usage(const fs::path& scratch) {
    const fs::path mod = scratch / "mod";
    write_text(mod / "oamod.yaml", "oamod: 1\n");
    const Captured packed = run({"mutator", "pack", mod.string()});
    OA_CHECK(packed.status == oa::tool::exit_usage);
    OA_CHECK(contains(packed.err, "oamut.yaml"));
    OA_CHECK(contains(packed.err, "run 'oa-tool help mutator pack'\n"));

    const fs::path oamod = scratch / "example.oamod";
    write_text(oamod, "not a package\n");
    const Captured checked = run({"mutator", "check", oamod.string()});
    OA_CHECK(checked.status == oa::tool::exit_usage);
    OA_CHECK(contains(checked.err, "run 'oa-tool help mutator check'\n"));
}

} // namespace

int main() {
    const Scratch scratch;
    const auto run_case = [](const char* name, auto&& function) {
        try {
            function();
        } catch (const std::exception& error) {
            std::fprintf(stderr, "%s: %s\n", name, error.what());
            ++oa::test::failed_checks();
        }
    };
    run_case("test_pack_and_check", [&] { test_pack_and_check(scratch.path); });
    run_case("test_refusals", [&] { test_refusals(scratch.path); });
    run_case("test_top_folder", [&] { test_top_folder(scratch.path); });
    run_case("test_clash_modes", [&] { test_clash_modes(scratch.path); });
    run_case("test_mutator_usage", [&] { test_mutator_usage(scratch.path); });
    return oa::test::check_exit_status();
}
