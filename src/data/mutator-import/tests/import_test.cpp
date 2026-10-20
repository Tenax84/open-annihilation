// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Legacy mutator import, on made-up files. A .tam is read, an id and a credit
// are taken from its text, and a package is built. Zip names without the
// UTF-8 flag are the names the streamed reader decoded.

#include "oa/data/mutator_import/legacy.hpp"

#include "oa/data/script_host/script_host.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/formats/oamut/manifest.hpp"
#include "oa/test/check.hpp"
#include "oa/test/raw_zip.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace importing = oa::data::mutator_import;
namespace oamut = oa::formats::oamut;

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

importing::ImportOptions test_options() {
    importing::ImportOptions options;
    options.engine = ">= 0.8.5";
    return options;
}

importing::LegacyFile text_file(std::string name, std::string_view text) {
    importing::LegacyFile file;
    file.name = std::move(name);
    file.bytes.assign(text.begin(), text.end());
    return file;
}

importing::LegacyFile byte_file(std::string name, std::vector<uint8_t> bytes) {
    importing::LegacyFile file;
    file.name = std::move(name);
    file.bytes = std::move(bytes);
    return file;
}

std::string as_text(const std::vector<uint8_t>& bytes) {
    return {bytes.begin(), bytes.end()};
}

bool warning_has(const std::vector<std::string>& warnings, std::string_view part) {
    for (const std::string& warning : warnings) {
        if (warning.find(part) != std::string::npos)
            return true;
    }
    return false;
}

const std::vector<uint8_t>* file_named(const importing::Imported& imported, std::string_view path) {
    for (const auto& file : imported.files) {
        if (file.first == path)
            return &file.second;
    }
    return nullptr;
}

void expect_round_trip(const importing::Imported& imported) {
    const std::string yaml = oamut::write_manifest(imported.manifest);
    const std::vector<uint8_t>* stored = file_named(imported, "oamut.yaml");
    if (stored == nullptr || as_text(*stored) != yaml) {
        std::fprintf(stderr, "oamut.yaml does not match the manifest\n");
    }
    OA_CHECK(stored != nullptr && as_text(*stored) == yaml);
    oamut::Manifest read_back;
    std::vector<oamut::Problem> problems;
    const bool read = oamut::read_manifest(*stored, read_back, problems);
    if (!read) {
        for (const oamut::Problem& problem : problems)
            std::fprintf(stderr, "%s\n", oamut::describe(problem).c_str());
    }
    OA_CHECK(read);
    OA_CHECK(problems.empty());

    const std::vector<uint8_t>* script = file_named(imported, "mutator.lua");
    if (script == nullptr)
        std::fprintf(stderr, "mutator.lua is missing\n");
    OA_CHECK(script != nullptr);
    oa::data::script_host::ScriptError error;
    const bool compiled =
        script != nullptr && oa::data::script_host::compile_check(*script, "mutator.lua", error);
    if (!compiled)
        std::fprintf(stderr, "mutator.lua: %s\n", error.message.c_str());
    OA_CHECK(compiled);
}

std::vector<uint8_t> hpi_of(std::vector<oa::HpiWriteFile> files) {
    return oa::write_hpi(files);
}

void test_read_tam() {
    const importing::LegacyFile crlf =
        text_file("a.tam", "* Steady Hands *\r\nHands stay steady\r\n*\r\n0\r\n");
    const importing::LegacyFile lf =
        text_file("a.tam", "* Steady Hands *\nHands stay steady\n*\n0\n");
    const importing::LegacyFile bare =
        text_file("a.tam", "* Steady Hands *\nHands stay steady\n*\n0");
    for (const importing::LegacyFile& file : {crlf, lf, bare}) {
        importing::Tam tam;
        std::string problem = "unchanged";
        const bool read = importing::read_tam(file.bytes, tam, problem);
        if (!read)
            std::fprintf(stderr, "read_tam: %s\n", problem.c_str());
        OA_CHECK(read);
        OA_CHECK(tam.title == "* Steady Hands *");
        OA_CHECK(tam.description == "Hands stay steady");
        OA_CHECK(tam.codes.size() == 1 && tam.codes[0] == "*");
        OA_CHECK(tam.companion_count == 0);
        OA_CHECK(tam.archive_name.empty());
    }

    std::vector<uint8_t> marked{0xDC};
    const std::string rest = "berwind\nA line\n*\n0";
    marked.insert(marked.end(), rest.begin(), rest.end());
    importing::Tam marked_tam;
    std::string marked_problem;
    OA_CHECK(importing::read_tam(marked, marked_tam, marked_problem));
    OA_CHECK(
        marked_tam.title == "\xC3\x9C"
                            "berwind"
    );

    OA_CHECK(importing::description_text("a##b") == "a\nb");
    OA_CHECK(importing::description_text("a####b") == "a\n\nb");
    OA_CHECK(importing::description_text("a#b") == "a\nb");
    OA_CHECK(importing::description_text("a###b") == "a\n\nb");
    OA_CHECK(importing::description_text("a\t\tb") == "a b");
    OA_CHECK(importing::description_text("a  b") == "a  b");

    const importing::LegacyFile fifth = text_file("a.tam", "Steady\nA palette\n*\n0\nridge.ccx\n");
    importing::Tam fifth_tam;
    std::string fifth_problem;
    OA_CHECK(importing::read_tam(fifth.bytes, fifth_tam, fifth_problem));
    OA_CHECK(fifth_tam.archive_name == "ridge.ccx");
    OA_CHECK(fifth_tam.companion_count == 0);

    const importing::LegacyFile blank = text_file("a.tam", "Steady\nA line\n\nOTA TACC\n1\n");
    importing::Tam blank_tam;
    std::string blank_problem;
    OA_CHECK(importing::read_tam(blank.bytes, blank_tam, blank_problem));
    OA_CHECK(blank_tam.codes.size() == 2);
    OA_CHECK(blank_tam.codes[0] == "OTA" && blank_tam.codes[1] == "TACC");
    OA_CHECK(blank_tam.companion_count == 1);
    OA_CHECK(warning_has(blank_tam.warnings, "empty line"));
}

void test_id_and_credit() {
    OA_CHECK(importing::id_from_title("* Steady Hands *") == "steady-hands");
    OA_CHECK(
        importing::id_from_title(
            "\xC3\x9C"
            "berwind"
        ) == "uberwind"
    );
    OA_CHECK(importing::id_from_title("Cost 1/2 & More!") == "cost-1-2-more");
    OA_CHECK(importing::id_from_title("!!!").empty());

    OA_CHECK(importing::credit_of("Created By:Ridge Works") == "Ridge Works");
    OA_CHECK(importing::credit_of("Made by Ridge Works") == "Ridge Works");
    OA_CHECK(importing::credit_of("by Ridge") == "Ridge");
    OA_CHECK(importing::credit_of("Hands stay steady.").empty());
    OA_CHECK(importing::credit_of("bypass the gate").empty());
}

std::string tam_text(
    std::string_view title,
    std::string_view description,
    std::string_view codes,
    int count,
    std::string_view extra = {}
) {
    std::string text;
    text += title;
    text += "\n";
    text += description;
    text += "\n";
    text += codes;
    text += "\n";
    text += static_cast<char>('0' + count);
    text += "\n";
    if (!extra.empty()) {
        text += extra;
        text += "\n";
    }
    return text;
}

void test_rules_only() {
    const std::string body =
        tam_text("* Steady Hands *", "Hands stay steady.##Created By:Ridge Works", "*", 0);
    const importing::LegacyFile files[] = {
        text_file("{}Steady.tam", body),
        text_file("{}steady.sm", kRule),
        text_file("readme.txt", "notes"),
    };
    const importing::ImportResult result = importing::import_legacy(files, test_options());
    if (!result.problems.empty())
        std::fprintf(stderr, "%s\n", result.problems.front().c_str());
    OA_CHECK(result.problems.empty());
    OA_CHECK(result.mutators.size() == 1);
    OA_CHECK(warning_has(result.warnings, "readme.txt"));
    const importing::Imported& imported = result.mutators[0];
    expect_round_trip(imported);
    OA_CHECK(imported.manifest.id == "steady-hands");
    OA_CHECK(imported.manifest.name == "Steady Hands");
    OA_CHECK(imported.manifest.author.name == "Ridge Works");
    OA_CHECK(imported.manifest.description == "Hands stay steady.\nCreated By:Ridge Works");
    OA_CHECK(imported.manifest.works_with.size() == 1);
    OA_CHECK(imported.manifest.works_with[0].id == "*");
    OA_CHECK(imported.manifest.layers.size() == 1);
    OA_CHECK(imported.manifest.layers[0] == oamut::Layer::units);
    OA_CHECK(imported.manifest.legacy.size() == 1);
    OA_CHECK(imported.manifest.legacy[0].prefix == "{}");
    OA_CHECK(imported.manifest.legacy[0].name == "* Steady Hands *");
    OA_CHECK(warning_has(imported.warnings, "gameplay"));
    const std::vector<uint8_t>* rules = file_named(imported, "rules/steady-hands.sm");
    OA_CHECK(rules != nullptr && as_text(*rules) == kRule);
    OA_CHECK(file_named(imported, "files/steady-hands.ccx") == nullptr);
}

void test_companion_and_hard() {
    oa::HpiWriteFile unit;
    unit.path = "units/RIDGEA.fbi";
    unit.bytes = {'U', 'N', 'I', 'T'};
    const std::vector<uint8_t> archive = hpi_of({unit});
    const std::string body = tam_text("Steady Hands", "by Ridge", "*", 1);
    const importing::LegacyFile soft_files[] = {
        text_file("{}Steady.tam", body),
        text_file("{}Steady.sm", kRule),
        byte_file("{}Steady(1).ccx", archive),
    };
    const importing::ImportResult soft = importing::import_legacy(soft_files, test_options());
    if (!soft.problems.empty())
        std::fprintf(stderr, "soft: %s\n", soft.problems.front().c_str());
    OA_CHECK(soft.problems.empty());
    OA_CHECK(soft.mutators.size() == 1);
    expect_round_trip(soft.mutators[0]);
    const std::vector<uint8_t>* packed = file_named(soft.mutators[0], "files/steady-hands.ccx");
    OA_CHECK(packed != nullptr && *packed == archive);
    bool files_layer = false;
    bool units_layer = false;
    for (const oamut::Layer layer : soft.mutators[0].manifest.layers) {
        files_layer = files_layer || layer == oamut::Layer::files;
        units_layer = units_layer || layer == oamut::Layer::units;
    }
    OA_CHECK(files_layer && units_layer);

    const std::string hard_body = tam_text("Steady Hands", "by Ridge", "*", 0);
    const importing::LegacyFile hard_files[] = {
        text_file("}{Steady.tam", hard_body),
        byte_file("}{Steady.ccx", archive),
    };
    const importing::ImportResult hard = importing::import_legacy(hard_files, test_options());
    if (!hard.problems.empty())
        std::fprintf(stderr, "hard: %s\n", hard.problems.front().c_str());
    OA_CHECK(hard.problems.empty());
    OA_CHECK(hard.mutators.size() == 1);
    OA_CHECK(hard.mutators[0].manifest.legacy[0].prefix == "}{");
    const std::vector<uint8_t>* hard_archive =
        file_named(hard.mutators[0], "files/steady-hands.ccx");
    OA_CHECK(hard_archive != nullptr && *hard_archive == archive);
    OA_CHECK(file_named(hard.mutators[0], "rules/steady-hands.sm") == nullptr);

    const std::string named_body = tam_text("Steady Hands", "by Ridge", "*", 0, "ridge.ccx");
    const importing::LegacyFile named_files[] = {
        text_file("}{Steady.tam", named_body),
        byte_file("ridge.ccx", archive),
    };
    const importing::ImportResult named = importing::import_legacy(named_files, test_options());
    if (!named.problems.empty())
        std::fprintf(stderr, "named: %s\n", named.problems.front().c_str());
    OA_CHECK(named.problems.empty());
    OA_CHECK(named.mutators.size() == 1);
    const std::vector<uint8_t>* named_archive =
        file_named(named.mutators[0], "files/steady-hands.ccx");
    OA_CHECK(named_archive != nullptr && *named_archive == archive);
    bool names_ridge = false;
    for (const std::string& legacy_name : named.mutators[0].manifest.legacy[0].files) {
        if (legacy_name == "ridge.ccx")
            names_ridge = true;
    }
    OA_CHECK(names_ridge);
}

void test_codes_and_layers() {
    importing::ImportOptions star = test_options();
    const importing::LegacyFile star_files[] = {
        text_file("{}Star.tam", tam_text("Star", "by Ridge", "*", 0)),
        text_file("{}Star.sm", kRule),
    };
    const importing::ImportResult star_result = importing::import_legacy(star_files, star);
    OA_CHECK(star_result.mutators.size() == 1);
    OA_CHECK(star_result.mutators[0].manifest.works_with.size() == 1);
    OA_CHECK(star_result.mutators[0].manifest.works_with[0].id == "*");

    const importing::LegacyFile both_files[] = {
        text_file("{}Both.tam", tam_text("Both Codes", "by Ridge", "OTA TACC", 0)),
        text_file("{}Both.sm", kRule),
    };
    const importing::ImportResult both = importing::import_legacy(both_files, test_options());
    OA_CHECK(both.mutators.size() == 1);
    OA_CHECK(both.mutators[0].manifest.works_with.size() == 1);
    OA_CHECK(both.mutators[0].manifest.works_with[0].id == "ta-3.1c");
    OA_CHECK(warning_has(both.mutators[0].warnings, "Core Contingency"));
    OA_CHECK(both.mutators[0].manifest.legacy[0].compatibility.size() == 2);

    importing::ImportOptions mapped = test_options();
    mapped.code_map.emplace_back("XX", "ridge");
    const importing::LegacyFile mapped_files[] = {
        text_file("{}Mapped.tam", tam_text("Mapped", "by Ridge", "XX", 0)),
        text_file("{}Mapped.sm", kRule),
    };
    const importing::ImportResult mapped_result = importing::import_legacy(mapped_files, mapped);
    OA_CHECK(mapped_result.mutators.size() == 1);
    OA_CHECK(mapped_result.mutators[0].manifest.works_with.size() == 1);
    OA_CHECK(mapped_result.mutators[0].manifest.works_with[0].id == "ridge");
    OA_CHECK(mapped_result.mutators[0].manifest.legacy[0].compatibility.size() == 1);
    OA_CHECK(mapped_result.mutators[0].manifest.legacy[0].compatibility[0] == "XX");

    const importing::LegacyFile open_files[] = {
        text_file("{}Open.tam", tam_text("Open", "by Ridge", "YY ZZ", 0)),
        text_file("{}Open.sm", kRule),
    };
    const importing::ImportResult open = importing::import_legacy(open_files, test_options());
    OA_CHECK(open.mutators.size() == 1);
    OA_CHECK(open.mutators[0].manifest.works_with.size() == 1);
    OA_CHECK(open.mutators[0].manifest.works_with[0].id == "*");
    OA_CHECK(warning_has(open.mutators[0].warnings, "YY is not mapped"));
    OA_CHECK(warning_has(open.mutators[0].warnings, "ZZ is not mapped"));
    OA_CHECK(warning_has(open.mutators[0].warnings, "works with every mod"));

    const std::string weapons =
        std::string{kRule} + "folder=weapons; extension=tdf;\n" + std::string{kRule};
    const importing::LegacyFile weapon_files[] = {
        text_file("{}Arms.tam", tam_text("Arms", "by Ridge", "*", 0)),
        text_file("{}Arms.sm", weapons),
    };
    const importing::ImportResult weapon_result =
        importing::import_legacy(weapon_files, test_options());
    if (!weapon_result.problems.empty())
        std::fprintf(stderr, "weapons: %s\n", weapon_result.problems.front().c_str());
    OA_CHECK(weapon_result.mutators.size() == 1);
    OA_CHECK(weapon_result.mutators[0].manifest.layers.size() == 2);
    OA_CHECK(weapon_result.mutators[0].manifest.layers[0] == oamut::Layer::units);
    OA_CHECK(weapon_result.mutators[0].manifest.layers[1] == oamut::Layer::weapons);

    const std::string maps = std::string{"folder = maps; extension = ota;\n"} + std::string{kRule};
    const importing::LegacyFile map_files[] = {
        text_file("{}Charts.tam", tam_text("Charts", "by Ridge", "*", 0)),
        text_file("{}Charts.sm", maps),
    };
    const importing::ImportResult map_result = importing::import_legacy(map_files, test_options());
    if (!map_result.problems.empty())
        std::fprintf(stderr, "maps: %s\n", map_result.problems.front().c_str());
    OA_CHECK(map_result.mutators.size() == 1);
    OA_CHECK(map_result.mutators[0].manifest.layers.size() == 1);
    OA_CHECK(map_result.mutators[0].manifest.layers[0] == oamut::Layer::maps);
}

void test_refusals_and_shared_id() {
    const importing::LegacyFile missing_rule[] = {
        text_file("{}Steady.tam", tam_text("Steady", "by Ridge", "*", 0)),
    };
    const importing::ImportResult no_rule = importing::import_legacy(missing_rule, test_options());
    OA_CHECK(no_rule.mutators.empty());
    OA_CHECK(!no_rule.problems.empty());
    OA_CHECK(no_rule.problems[0].find("missing rule file") != std::string::npos);
    OA_CHECK(no_rule.problems[0].find("{}Steady.sm") != std::string::npos);

    const importing::LegacyFile missing_archive[] = {
        text_file("{}Steady.tam", tam_text("Steady", "by Ridge", "*", 1)),
        text_file("{}Steady.sm", kRule),
    };
    const importing::ImportResult no_archive =
        importing::import_legacy(missing_archive, test_options());
    OA_CHECK(no_archive.mutators.empty());
    OA_CHECK(!no_archive.problems.empty());
    OA_CHECK(no_archive.problems[0].find("missing archive") != std::string::npos);
    OA_CHECK(no_archive.problems[0].find("{}Steady(1).ccx") != std::string::npos);

    const importing::LegacyFile broken[] = {
        text_file("{}Broken.tam", tam_text("Broken", "by Ridge", "*", 0)),
        text_file("{}Broken.sm", "[*Group*]{\n"),
        text_file("{}Whole.tam", tam_text("Whole", "by Ridge", "*", 0)),
        text_file("{}Whole.sm", kRule),
    };
    const importing::ImportResult refused = importing::import_legacy(broken, test_options());
    OA_CHECK(refused.mutators.size() == 1);
    OA_CHECK(refused.mutators[0].manifest.id == "whole");
    OA_CHECK(!refused.problems.empty());
    OA_CHECK(refused.problems[0].find("line 1") != std::string::npos);
    OA_CHECK(refused.problems[0].find("block is not closed") != std::string::npos);

    const importing::LegacyFile shared[] = {
        text_file("{}Alpha.tam", tam_text("* Steady Hands *", "by Ridge", "*", 0)),
        text_file("{}Alpha.sm", kRule),
        text_file("{}Beta.tam", tam_text("* Steady Hands *", "by Ridge", "*", 0)),
        text_file("{}Beta.sm", kRule),
    };
    const importing::ImportResult pair = importing::import_legacy(shared, test_options());
    if (pair.problems.size() > 0)
        std::fprintf(stderr, "shared: %s\n", pair.problems.front().c_str());
    OA_CHECK(pair.mutators.size() == 2);
    OA_CHECK(pair.mutators[0].manifest.id == "steady-hands");
    OA_CHECK(pair.mutators[1].manifest.id == "steady-hands-2");
    OA_CHECK(warning_has(pair.mutators[1].warnings, "steady-hands-2"));
    expect_round_trip(pair.mutators[0]);
    expect_round_trip(pair.mutators[1]);
}

void test_protected_entries() {
    oa::HpiWriteFile unit;
    unit.path = "units/RIDGEA.fbi";
    unit.bytes = {'U', 'N', 'I', 'T'};
    oa::HpiWriteFile gui;
    gui.path = "guis/RIDGEA1.GUI";
    gui.bytes = {'G', 'U', 'I'};
    oa::HpiWriteFile palette;
    palette.path = "palettes/ridge.pal";
    palette.bytes = {'P', 'A', 'L'};
    const std::vector<uint8_t> mixed = hpi_of({unit, gui, palette});
    const importing::LegacyFile mixed_files[] = {
        text_file("{}Steady.tam", tam_text("Steady", "by Ridge", "*", 1)),
        text_file("{}Steady.sm", kRule),
        byte_file("{}Steady(1).ccx", mixed),
    };
    const importing::ImportResult result = importing::import_legacy(mixed_files, test_options());
    if (!result.problems.empty())
        std::fprintf(stderr, "protected: %s\n", result.problems.front().c_str());
    OA_CHECK(result.mutators.size() == 1);
    expect_round_trip(result.mutators[0]);
    const std::vector<uint8_t>* packed = file_named(result.mutators[0], "files/steady.ccx");
    OA_CHECK(packed != nullptr);
    auto opened = packed == nullptr ? oa::HpiArchive::open(std::vector<uint8_t>{})
                                    : oa::HpiArchive::open(std::vector<uint8_t>(*packed));
    OA_CHECK(opened.ok());
    if (opened.ok()) {
        const std::vector<oa::ArchiveEntry> entries = opened.value->entries();
        if (entries.size() != 1)
            std::fprintf(stderr, "protected entries: %zu\n", entries.size());
        OA_CHECK(entries.size() == 1);
        OA_CHECK(entries.size() == 1 && entries[0].path == "units/RIDGEA.fbi");
        const auto body = opened.value->read("units/RIDGEA.fbi");
        OA_CHECK(body.ok() && body.value && *body.value == unit.bytes);
    }
    OA_CHECK(warning_has(result.mutators[0].warnings, "guis/RIDGEA1.GUI"));
    OA_CHECK(warning_has(result.mutators[0].warnings, "menus-only"));
    OA_CHECK(warning_has(result.mutators[0].warnings, "palettes/ridge.pal"));
    OA_CHECK(warning_has(result.mutators[0].warnings, "read-once"));

    const std::vector<uint8_t> only_palette = hpi_of({palette});
    const importing::LegacyFile palette_files[] = {
        text_file("}{Tint.tam", tam_text("Tint", "by Ridge", "*", 0)),
        byte_file("}{Tint.ccx", only_palette),
    };
    const importing::ImportResult tint = importing::import_legacy(palette_files, test_options());
    if (!tint.problems.empty())
        std::fprintf(stderr, "tint: %s\n", tint.problems.front().c_str());
    OA_CHECK(tint.mutators.size() == 1);
    expect_round_trip(tint.mutators[0]);
    OA_CHECK(tint.mutators[0].manifest.layers.size() == 1);
    OA_CHECK(tint.mutators[0].manifest.layers[0] == oamut::Layer::files);
    OA_CHECK(file_named(tint.mutators[0], "files/tint.ccx") == nullptr);
    OA_CHECK(warning_has(tint.mutators[0].warnings, "palettes/ridge.pal"));
    OA_CHECK(warning_has(tint.mutators[0].warnings, "read-once"));
    OA_CHECK(warning_has(tint.mutators[0].warnings, "the mutator changes nothing"));
}

void test_zip() {
    const std::string body = std::string("\xDC") + "berwind\nA line\n*\n0\n";
    std::string tam_name = "{}";
    tam_name.push_back(static_cast<char>(0x9A));
    tam_name += "berwind.tam";
    std::string sm_name = "{}";
    sm_name.push_back(static_cast<char>(0x9A));
    sm_name += "berwind.sm";
    const std::vector<oa::test::raw_zip::RawFile> one_pair = {
        oa::test::raw_zip::stored_text(tam_name, body),
        oa::test::raw_zip::stored_text(sm_name, kRule),
    };
    const oa::test::raw_zip::RawArchive archive = oa::test::raw_zip::build_raw_archive(one_pair);
    std::vector<importing::LegacyFile> files;
    std::string problem;
    const bool read = importing::files_from_zip(archive.bytes, files, problem);
    if (!read)
        std::fprintf(stderr, "zip: %s\n", problem.c_str());
    OA_CHECK(read);
    OA_CHECK(files.size() == 2);
    const std::string expect_tam = std::string("{}") + "\xC3\x9C" + "berwind.tam";
    const std::string expect_sm = std::string("{}") + "\xC3\x9C" + "berwind.sm";
    if (files.size() == 2 && (files[0].name != expect_tam || files[1].name != expect_sm)) {
        std::fprintf(stderr, "zip names: %s | %s\n", files[0].name.c_str(), files[1].name.c_str());
    }
    OA_CHECK(files.size() == 2 && files[0].name == expect_tam && files[1].name == expect_sm);
    const importing::ImportResult imported = importing::import_legacy(files, test_options());
    if (!imported.problems.empty())
        std::fprintf(stderr, "uberwind: %s\n", imported.problems.front().c_str());
    OA_CHECK(imported.mutators.size() == 1);
    OA_CHECK(imported.mutators[0].manifest.id == "uberwind");
    expect_round_trip(imported.mutators[0]);

    const std::vector<oa::test::raw_zip::RawFile> two = {
        oa::test::raw_zip::stored_text("{}Alpha.tam", tam_text("Alpha", "by Ridge", "*", 0)),
        oa::test::raw_zip::stored_text("{}Alpha.sm", kRule),
        oa::test::raw_zip::stored_text("{}Beta.tam", tam_text("Beta", "by Ridge", "*", 0)),
        oa::test::raw_zip::stored_text("{}Beta.sm", kRule),
    };
    const oa::test::raw_zip::RawArchive both = oa::test::raw_zip::build_raw_archive(two);
    std::vector<importing::LegacyFile> both_files;
    OA_CHECK(importing::files_from_zip(both.bytes, both_files, problem));
    const importing::ImportResult pair = importing::import_legacy(both_files, test_options());
    if (!pair.problems.empty())
        std::fprintf(stderr, "pair: %s\n", pair.problems.front().c_str());
    OA_CHECK(pair.mutators.size() == 2);
    expect_round_trip(pair.mutators[0]);
    expect_round_trip(pair.mutators[1]);

    std::vector<oa::test::raw_zip::RawFile> with_note;
    with_note.push_back(
        oa::test::raw_zip::stored_text("{}Alpha.tam", tam_text("Alpha", "by Ridge", "*", 0))
    );
    with_note.push_back(oa::test::raw_zip::stored_text("{}Alpha.sm", kRule));
    with_note.push_back(oa::test::raw_zip::stored_text("readme.txt", "secret-notes"));
    const oa::test::raw_zip::RawArchive noted = oa::test::raw_zip::build_raw_archive(with_note);
    std::vector<importing::LegacyFile> noted_files;
    noted_files.push_back(text_file("keep.tam", "x"));
    OA_CHECK(importing::files_from_zip(noted.bytes, noted_files, problem));
    OA_CHECK(noted_files.size() == 3);
    if (noted_files.size() == 3) {
        OA_CHECK(noted_files[2].name == "readme.txt");
        OA_CHECK(noted_files[2].bytes.empty());
        OA_CHECK(!noted_files[0].bytes.empty());
        OA_CHECK(!noted_files[1].bytes.empty());
    }
    const importing::ImportResult noted_import =
        importing::import_legacy(noted_files, test_options());
    OA_CHECK(warning_has(noted_import.warnings, "readme.txt"));

    // Longer than a short string, so the name is not kept in the string object.
    const std::string long_name = "{}LongerSteadyHandsFile";
    std::vector<oa::test::raw_zip::RawFile> long_files;
    long_files.push_back(
        oa::test::raw_zip::stored_text(
            long_name + ".tam", tam_text("Longer Steady", "by Ridge", "*", 0)
        )
    );
    long_files.push_back(oa::test::raw_zip::stored_text(long_name + ".sm", kRule));
    const oa::test::raw_zip::RawArchive long_archive =
        oa::test::raw_zip::build_raw_archive(long_files);
    std::vector<importing::LegacyFile> long_read;
    OA_CHECK(importing::files_from_zip(long_archive.bytes, long_read, problem));
    OA_CHECK(long_read.size() == 2);
    if (long_read.size() == 2) {
        OA_CHECK(long_read[0].name == long_name + ".tam");
        OA_CHECK(!long_read[0].bytes.empty());
        OA_CHECK(long_read[1].name == long_name + ".sm");
        OA_CHECK(!long_read[1].bytes.empty());
    }
    const importing::ImportResult long_import = importing::import_legacy(long_read, test_options());
    if (!long_import.problems.empty())
        std::fprintf(stderr, "long name: %s\n", long_import.problems.front().c_str());
    OA_CHECK(long_import.mutators.size() == 1);
    if (!long_import.mutators.empty())
        OA_CHECK(long_import.mutators[0].manifest.id == "longer-steady");

    std::vector<oa::test::raw_zip::RawFile> heavy;
    heavy.push_back(
        oa::test::raw_zip::stored_text("{}Wide.tam", tam_text("Wide", "by Ridge", "*", 0))
    );
    heavy.push_back(oa::test::raw_zip::stored_text("{}Wide.sm", kRule));
    // The zip reader allows an uncompressed size of at most 1032 times the
    // stored size. Each pad claims 64 MiB and is not inflated: together with
    // the .tam and the .sm they pass 512 MiB, so the import refuses them first.
    constexpr uint64_t uncompressed = uint64_t{64} * 1024 * 1024;
    const std::size_t compressed = static_cast<std::size_t>(uncompressed / 1032) + 1;
    for (int number = 0; number < 8; ++number) {
        oa::test::raw_zip::RawFile bomb;
        bomb.name = "pad" + std::to_string(number) + ".ccx";
        bomb.method = 8;
        bomb.bytes = uncompressed;
        bomb.data.assign(compressed, 0);
        heavy.push_back(std::move(bomb));
    }
    const oa::test::raw_zip::RawArchive too_big = oa::test::raw_zip::build_raw_archive(heavy);
    std::vector<importing::LegacyFile> kept;
    kept.push_back(text_file("keep.tam", "x"));
    const bool refused = importing::files_from_zip(too_big.bytes, kept, problem);
    OA_CHECK(!refused);
    OA_CHECK(problem.find("512 MiB") != std::string::npos);
    OA_CHECK(kept.size() == 1);
    OA_CHECK(kept[0].name == "keep.tam");
}

} // namespace

int main() {
    test_read_tam();
    test_id_and_credit();
    test_rules_only();
    test_companion_and_hard();
    test_codes_and_layers();
    test_refusals_and_shared_id();
    test_protected_entries();
    test_zip();
    return oa::test::check_exit_status();
}
