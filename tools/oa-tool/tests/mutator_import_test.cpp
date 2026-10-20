// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// oa-tool mutator import, run in-process. Made-up .tam files and a zip become
// mutator folders. A packed file is one mutator check accepts. A link is not
// followed, and one refused mutator does not stop the others.

#include "command.hpp"

#include "oa/test/check.hpp"
#include "oa/test/raw_zip.hpp"
#include "oa/test/scratch_directory.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace raw_zip = oa::test::raw_zip;

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

struct Scratch {
    fs::path path;

    Scratch() : path(oa::test::make_scratch_directory("P05-mutator-import")) {}

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

std::string read_text(const fs::path& path) {
    const std::vector<uint8_t> bytes = read_bytes(path);
    return {bytes.begin(), bytes.end()};
}

/// One made-up .tam: a title, a description, codes and a companion count.
///
/// @param title the title line
/// @param description the description line
/// @param codes the compatibility line
/// @param count the companion count, one digit
/// @return the file, with CR LF ends
std::string
tam_text(std::string_view title, std::string_view description, std::string_view codes, char count) {
    std::string text;
    text.append(title);
    text.append("\r\n");
    text.append(description);
    text.append("\r\n");
    text.append(codes);
    text.append("\r\n");
    text.push_back(count);
    text.append("\r\n");
    return text;
}

void test_folder() {
    Scratch scratch;
    const fs::path source = scratch.path / "in";
    const fs::path out = scratch.path / "out";
    const fs::path tam = source / "{}SteadyHands.tam";
    write_text(
        tam, tam_text("Steady Hands", "Hands stay steady.\tCreated By: Ridge Works", "*", '0')
    );
    write_text(source / "{}SteadyHands.sm", kRule);
    write_text(source / "readme.txt", "not a mutator\n");

    const Captured imported = run({"mutator", "import", tam.string(), "--out", out.string()});
    OA_CHECK(imported.status == oa::tool::exit_done);
    OA_CHECK(imported.err.empty());
    OA_CHECK(contains(imported.out, "imported steady-hands from " + tam.string()));
    OA_CHECK(contains(imported.out, ": 1 rule files, 0 archives\n"));
    OA_CHECK(contains(imported.out, "tags default to gameplay"));
    OA_CHECK(!contains(imported.out, "readme.txt"));

    const fs::path folder = out / "steady-hands";
    const std::string yaml = read_text(folder / "oamut.yaml");
    OA_CHECK(contains(yaml, "id: \"steady-hands\""));
    OA_CHECK(contains(yaml, ">= 0.8.5"));
    OA_CHECK(contains(yaml, "Ridge Works"));
    OA_CHECK(contains(yaml, "gameplay"));
    OA_CHECK(fs::is_regular_file(folder / "mutator.lua"));
    OA_CHECK(contains(read_text(folder / "mutator.lua"), "TA: Mutation mutator"));
    const std::vector<uint8_t> rules = read_bytes(folder / "rules" / "steady-hands.sm");
    OA_CHECK(std::string(rules.begin(), rules.end()) == kRule);
}

void test_zip_and_pack() {
    Scratch scratch;
    const std::string text =
        tam_text("Steady Hands", "Hands stay steady.\tCreated By: Ridge Works", "*", '0');
    std::vector<raw_zip::RawFile> entries;
    entries.push_back(raw_zip::stored_text("{}SteadyHands.tam", text));
    entries.push_back(raw_zip::stored_text("{}SteadyHands.sm", kRule));
    const raw_zip::RawArchive archive = raw_zip::build_raw_archive(entries);
    const fs::path zip = scratch.path / "hands.zip";
    write_bytes(zip, archive.bytes);

    const fs::path out = scratch.path / "out";
    const Captured imported =
        run({"mutator", "import", zip.string(), "--out", out.string(), "--pack"});
    OA_CHECK(imported.status == oa::tool::exit_done);
    OA_CHECK(contains(imported.out, "imported steady-hands from " + zip.string()));
    OA_CHECK(contains(imported.out, "1 rule files, 0 archives"));
    const fs::path package = out / "steady-hands-1.0.oamut";
    OA_CHECK(fs::is_regular_file(package));
    const std::vector<uint8_t> rules =
        read_bytes(out / "steady-hands" / "rules" / "steady-hands.sm");
    OA_CHECK(std::string(rules.begin(), rules.end()) == kRule);

    const Captured checked = run({"mutator", "check", package.string()});
    OA_CHECK(checked.status == oa::tool::exit_done);
    OA_CHECK(checked.out.ends_with("result: ok\n"));
}

void test_force_and_id() {
    Scratch scratch;
    const fs::path source = scratch.path / "in";
    const fs::path tam = source / "{}SteadyHands.tam";
    write_text(tam, tam_text("Steady Hands", "Hands stay steady.", "*", '0'));
    write_text(source / "{}SteadyHands.sm", kRule);
    const fs::path out = scratch.path / "out";

    const Captured first = run({"mutator", "import", tam.string(), "--out", out.string()});
    OA_CHECK(first.status == oa::tool::exit_done);
    const Captured blocked = run({"mutator", "import", tam.string(), "--out", out.string()});
    OA_CHECK(blocked.status == oa::tool::exit_failed);
    OA_CHECK(contains(blocked.out, "the folder already exists"));
    const std::vector<uint8_t> kept =
        read_bytes(out / "steady-hands" / "rules" / "steady-hands.sm");
    OA_CHECK(std::string(kept.begin(), kept.end()) == kRule);

    const Captured forced =
        run({"mutator", "import", tam.string(), "--out", out.string(), "--force"});
    OA_CHECK(forced.status == oa::tool::exit_done);
    OA_CHECK(contains(forced.out, "imported steady-hands from "));

    const fs::path named = scratch.path / "named";
    const Captured override_id = run(
        {"mutator",
         "import",
         tam.string(),
         "--out",
         named.string(),
         "--id",
         "ridge-grip",
         "--version",
         "2.0",
         "--pack"}
    );
    OA_CHECK(override_id.status == oa::tool::exit_done);
    OA_CHECK(contains(override_id.out, "imported ridge-grip from "));
    const std::string yaml = read_text(named / "ridge-grip" / "oamut.yaml");
    OA_CHECK(contains(yaml, "ridge-grip"));
    OA_CHECK(contains(yaml, "2.0"));
    OA_CHECK(fs::is_regular_file(named / "ridge-grip-2.0.oamut"));
}

void test_usage(const Scratch& scratch) {
    const Captured bare = run({"mutator", "import"});
    OA_CHECK(bare.status == oa::tool::exit_usage);
    OA_CHECK(contains(bare.err, "oa-tool mutator import:"));

    const fs::path out = scratch.path / "usage-out";
    const Captured no_out = run({"mutator", "import", "hands.tam"});
    OA_CHECK(no_out.status == oa::tool::exit_usage);
    OA_CHECK(contains(no_out.err, "--out is required"));

    const Captured no_input = run({"mutator", "import", "--out", out.string()});
    OA_CHECK(no_input.status == oa::tool::exit_usage);
    OA_CHECK(contains(no_input.err, "name an input"));
    OA_CHECK(!fs::exists(out));

    const Captured two_ids =
        run({"mutator", "import", "a.tam", "b.tam", "--id", "ridge", "--out", out.string()});
    OA_CHECK(two_ids.status == oa::tool::exit_usage);
    OA_CHECK(contains(two_ids.err, "--id and --version apply to one mutator"));
    OA_CHECK(!fs::exists(out));

    const Captured two_versions =
        run({"mutator", "import", "a.tam", "b.tam", "--version", "2.0", "--out", out.string()});
    OA_CHECK(two_versions.status == oa::tool::exit_usage);
    OA_CHECK(!fs::exists(out));

    const Captured built_in =
        run({"mutator", "import", "a.tam", "--out", out.string(), "--works-with", "OTA=ridge"});
    OA_CHECK(built_in.status == oa::tool::exit_usage);
    OA_CHECK(contains(built_in.err, "OTA and TACC are already mapped"));

    const Captured broken =
        run({"mutator", "import", "a.tam", "--out", out.string(), "--works-with", "XX"});
    OA_CHECK(broken.status == oa::tool::exit_usage);
    OA_CHECK(contains(broken.err, "CODE=ENTRY"));

    const Captured repeated = run(
        {"mutator",
         "import",
         "a.tam",
         "--out",
         out.string(),
         "--works-with",
         "XX=ridge",
         "--works-with",
         "xx=other"}
    );
    OA_CHECK(repeated.status == oa::tool::exit_usage);
    OA_CHECK(contains(repeated.err, "repeated"));

    const Captured other =
        run({"mutator", "import", (scratch.path / "notes.txt").string(), "--out", out.string()});
    write_text(scratch.path / "notes.txt", "nope\n");
    const Captured text =
        run({"mutator", "import", (scratch.path / "notes.txt").string(), "--out", out.string()});
    OA_CHECK(other.status == oa::tool::exit_usage);
    OA_CHECK(text.status == oa::tool::exit_usage);
    OA_CHECK(contains(text.err, "a .tam or a .zip"));
    OA_CHECK(!fs::exists(out));
}

void test_one_refused() {
    Scratch scratch;
    const fs::path source = scratch.path / "in";
    const fs::path steady = source / "{}SteadyHands.tam";
    const fs::path other = source / "{}OtherHands.tam";
    write_text(steady, tam_text("Steady Hands", "Hands stay steady.", "*", '0'));
    write_text(source / "{}SteadyHands.sm", kRule);
    write_text(other, tam_text("Other Hands", "Other hands.", "*", '0'));
    const fs::path out = scratch.path / "out";

    const Captured imported =
        run({"mutator", "import", steady.string(), other.string(), "--out", out.string()});
    OA_CHECK(imported.status == oa::tool::exit_failed);
    OA_CHECK(contains(imported.out, "imported steady-hands from "));
    OA_CHECK(contains(imported.out, "missing rule file {}OtherHands.sm"));
    OA_CHECK(fs::is_regular_file(out / "steady-hands" / "oamut.yaml"));
    OA_CHECK(!fs::exists(out / "other-hands"));
}

void test_mapping() {
    Scratch scratch;
    const fs::path source = scratch.path / "in";
    const fs::path tam = source / "{}SteadyHands.tam";
    write_text(tam, tam_text("Steady Hands", "Hands stay steady.", "XX", '0'));
    write_text(source / "{}SteadyHands.sm", kRule);
    const fs::path out = scratch.path / "out";
    const Captured imported =
        run({"mutator", "import", tam.string(), "--out", out.string(), "--works-with", "XX=ridge"});
    OA_CHECK(imported.status == oa::tool::exit_done);
    OA_CHECK(contains(read_text(out / "steady-hands" / "oamut.yaml"), "ridge"));
    OA_CHECK(!contains(imported.out, "works with every mod"));
}

void test_not_into_input() {
    Scratch scratch;
    const fs::path source = scratch.path / "in";
    const fs::path tam = source / "{}SteadyHands.tam";
    write_text(tam, tam_text("Steady Hands", "Hands stay steady.", "*", '0'));
    write_text(source / "{}SteadyHands.sm", kRule);
    const Captured imported = run({"mutator", "import", tam.string(), "--out", source.string()});
    OA_CHECK(imported.status == oa::tool::exit_failed);
    OA_CHECK(contains(imported.out, "refusing to write into the input's folder"));
    OA_CHECK(!fs::exists(source / "steady-hands"));
    OA_CHECK(read_text(source / "{}SteadyHands.sm") == kRule);
}

/// A composed .tam finds a decomposed companion, where the platform can hold both.
void test_composed_name() {
    Scratch scratch;
    const fs::path source = scratch.path / "in";
    const std::string composed = std::string("{}") + "\xC3\x9C" + "berwind";
    const std::string decomposed = std::string("{}u") + "\xCC\x88" + "berwind";
    // The names are UTF-8, as oa-tool reads its arguments; a path made from a
    // plain string would use Windows' code page instead.
    const auto utf8_path = [](const std::string& text) {
        return fs::path(std::u8string(text.begin(), text.end()));
    };
    const fs::path tam_path = source / utf8_path(composed + ".tam");
    const fs::path sm_path = source / utf8_path(decomposed + ".sm");
    // The title is Windows-1252. Byte 0xDC is Ü, which the id folds to u.
    write_text(
        tam_path,
        tam_text(
            "\xDC"
            "berwind",
            "by Ridge",
            "*",
            '0'
        )
    );
    write_text(sm_path, kRule);
    std::error_code error;
    if (!fs::is_regular_file(tam_path, error) || !fs::is_regular_file(sm_path, error)) {
        std::fprintf(stderr, "mutator import: skipped the composed name\n");
        return;
    }
    const auto tam_size = fs::file_size(tam_path, error);
    const auto sm_size = fs::file_size(sm_path, error);
    if (error || tam_size == sm_size) {
        std::fprintf(stderr, "mutator import: skipped the composed name\n");
        return;
    }
    const auto folder = source.generic_u8string();
    std::string given(folder.begin(), folder.end());
    if (given.empty() || given.back() != '/')
        given.push_back('/');
    given += composed;
    given += ".tam";
    const fs::path out = scratch.path / "out";
    const Captured imported = run({"mutator", "import", given, "--out", out.string()});
    OA_CHECK(imported.status == oa::tool::exit_done);
    OA_CHECK(contains(imported.out, "imported uberwind from "));
    OA_CHECK(fs::is_regular_file(out / "uberwind" / "rules" / "uberwind.sm"));
}

/// A companion that is a link is not read, where the system can make a link.
void test_link() {
    Scratch scratch;
    const fs::path source = scratch.path / "in";
    const fs::path outside = scratch.path / "outside";
    const fs::path tam = source / "{}SteadyHands.tam";
    write_text(tam, tam_text("Steady Hands", "Hands stay steady.", "*", '0'));
    write_text(outside / "secret.sm", "linked-secret\n");
    const fs::path link = source / "{}SteadyHands.sm";
    std::error_code link_error;
    fs::create_symlink(outside / "secret.sm", link, link_error);
    std::error_code status_error;
    if (link_error || !fs::is_symlink(fs::symlink_status(link, status_error))) {
        std::fprintf(stderr, "mutator import: skipped the link case\n");
        return;
    }
    const fs::path out = scratch.path / "out";
    const Captured imported = run({"mutator", "import", tam.string(), "--out", out.string()});
    OA_CHECK(imported.status != oa::tool::exit_done);
    OA_CHECK(contains(imported.out, "refusing to follow a link"));
    OA_CHECK(!fs::exists(out / "steady-hands"));
    OA_CHECK(read_text(outside / "secret.sm") == "linked-secret\n");
    OA_CHECK(!contains(read_text(source / "{}SteadyHands.tam"), "linked-secret"));
}

} // namespace

int main() {
    test_folder();
    test_zip_and_pack();
    test_force_and_id();
    {
        const Scratch scratch;
        test_usage(scratch);
    }
    test_one_refused();
    test_mapping();
    test_not_into_input();
    test_composed_name();
    test_link();
    return oa::test::check_exit_status();
}
