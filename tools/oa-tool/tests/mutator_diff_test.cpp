// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// oa-tool mutator diff, run in-process. A stand-in game holds three made-up
// units. Package folders double a builder's worker time, drop flyers, mask
// one unit and set a map profile. With --data the same doubling is checked
// against the installed game.

#include "command.hpp"
#include "mutator_report.hpp"

#include "oa/base/sha256.hpp"
#include "oa/data/defs/unit_header.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/test/check.hpp"
#include "oa/test/game_data.hpp"
#include "oa/test/scratch_directory.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace report = oa::tool::mutator_report;
namespace sha256 = oa::base::sha256;

struct Scratch {
    fs::path path;

    Scratch() : path(oa::test::make_scratch_directory("L01-mutator-diff")) {}

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

/// Restores the working directory when the check returns.
struct WorkingDirectory {
    fs::path previous;

    explicit WorkingDirectory(const fs::path& next) : previous(fs::current_path()) {
        fs::current_path(next);
    }

    ~WorkingDirectory() {
        std::error_code error;
        fs::current_path(previous, error);
    }

    WorkingDirectory(const WorkingDirectory&) = delete;
    WorkingDirectory& operator=(const WorkingDirectory&) = delete;
};

Captured run(std::vector<std::string> arguments) {
    std::ostringstream out;
    std::ostringstream err;
    oa::tool::Output output{out, err};
    const int status = oa::tool::run_tool(std::move(arguments), output);
    return {status, out.str(), err.str()};
}

void show(const Captured& captured) {
    std::fprintf(
        stderr,
        "status %d\nstdout:\n%sstderr:\n%s",
        captured.status,
        captured.out.c_str(),
        captured.err.c_str()
    );
}

void expect_status(const Captured& captured, int status) {
    if (captured.status != status)
        show(captured);
    OA_CHECK(captured.status == status);
}

void expect_has(const Captured& captured, std::string_view needle) {
    if (captured.out.find(needle) == std::string::npos) {
        std::fprintf(stderr, "missing from stdout: %s\n", std::string(needle).c_str());
        show(captured);
    }
    OA_CHECK(captured.out.find(needle) != std::string::npos);
}

void expect_has_not(const Captured& captured, std::string_view needle) {
    if (captured.out.find(needle) != std::string::npos) {
        std::fprintf(stderr, "unexpected stdout: %s\n", std::string(needle).c_str());
        show(captured);
    }
    OA_CHECK(captured.out.find(needle) == std::string::npos);
}

void expect_err(const Captured& captured, std::string_view needle) {
    if (captured.err.find(needle) == std::string::npos) {
        std::fprintf(stderr, "missing from stderr: %s\n", std::string(needle).c_str());
        show(captured);
    }
    OA_CHECK(captured.err.find(needle) != std::string::npos);
}

void expect_out(const Captured& captured, const std::string& expected) {
    if (captured.out != expected) {
        std::fprintf(
            stderr,
            "stdout mismatch\n--- got ---\n%s--- expected ---\n%s",
            captured.out.c_str(),
            expected.c_str()
        );
        show(captured);
    }
    OA_CHECK(captured.out == expected);
}

/// The notice a unit must carry, with a year filled in. The text comes from
/// the unit header's own constant.
std::string notice() {
    std::string text(oa::data::defs::unit_copyright);
    const auto year = text.find("0000");
    OA_CHECK(year != std::string::npos);
    if (year != std::string::npos)
        text.replace(year, 4, "1999");
    return text;
}

std::string fbi(std::string_view name, std::string_view body, const std::string& copyright) {
    return "[UNITINFO]\n{\nUnitName=" + std::string(name) + ";\n" + std::string(body) +
           "Version=1;\nCopyright=" + copyright + ";\n}\n";
}

std::vector<uint8_t> bytes_of(std::string_view text) {
    return {text.begin(), text.end()};
}

void write_text(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    OA_CHECK(static_cast<bool>(out));
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    OA_CHECK(static_cast<bool>(out));
}

void write_bytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    OA_CHECK(static_cast<bool>(out));
    out.write(
        reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
    );
    OA_CHECK(static_cast<bool>(out));
}

std::string manifest(std::string_view id, std::string_view layers) {
    return "oamut: 1\nid: " + std::string(id) + "\nname: \"" + std::string(id) +
           "\"\nversion: \"1\"\nauthor:\n  name: Maker\ntags: [\"gameplay\"]\nworks-with: "
           "[\"*\"]\nlayers: " +
           std::string(layers) + "\n";
}

fs::path write_package(
    const Scratch& scratch, std::string_view id, std::string_view layers, std::string_view body
) {
    const fs::path folder = scratch.path / "packages" / std::string(id);
    write_text(folder / "oamut.yaml", manifest(id, layers));
    write_text(folder / "mutator.lua", body);
    return folder;
}

/// The stand-in game. rev31.gp3 holds three units, a menu, a GUI and one map.
/// The GUI repeats [GADGET1], as 3.1c's build pages repeat their gadgets:
/// BRAVO's build button, then an order button.
fs::path write_game(const Scratch& scratch, const std::string& copyright) {
    const std::vector<oa::HpiWriteFile> files = {
        {"units/ALPHA.fbi",
         bytes_of(fbi("ALPHA", "Builder=1;\nBMcode=0;\nWorkerTime=100;\n", copyright)),
         0},
        {"units/BRAVO.fbi", bytes_of(fbi("BRAVO", "CanFly=1;\nMaxDamage=500;\n", copyright)), 0},
        {"units/CHARLIE.fbi",
         bytes_of(fbi("CHARLIE", "Builder=1;\nBMcode=1;\nWorkerTime=50;\n", copyright)),
         0},
        {"weapons/test.tdf",
         bytes_of(
             "[GUN]\n{\nID=1;\nrange=300;\n[DAMAGE]\n{\ndefault=10;\n}\n}\n"
             "[CANNON]\n{\nID=2;\nrange=400;\n}\n"
         ),
         0},
        {"download/alpha.tdf",
         bytes_of(
             "[MENU0]\n{\nUNITMENU=ALPHA;\nMENU=1;\nBUTTON=1;\nUNITNAME=BRAVO;\n}\n"
             "[MENU1]\n{\nUNITMENU=ALPHA;\nMENU=1;\nBUTTON=2;\nUNITNAME=CHARLIE;\n}\n"
         ),
         0},
        {"guis/ALPHA1.gui",
         bytes_of(
             "[GADGET0]\n{\n[COMMON]\n{\nid=0;\nname=ALPHA1;\nactive=1;\n}\n}\n"
             "[GADGET1]\n{\n[COMMON]\n{\nid=1;\nname=BRAVO;\nactive=1;\ncommonattribs=4;\n}\n"
             "grayedout=0;\n}\n"
             "[GADGET1]\n{\n[COMMON]\n{\nid=1;\nname=ALPHAMOVE;\nactive=1;\n"
             "commonattribs=0;\n}\ngrayedout=0;\n}\n"
         ),
         0},
        {"gamedata/sidedata.tdf", bytes_of("[SIDE0]\n{\nname=Arm;\n}\n"), 0},
        {"maps/isle.ota",
         bytes_of("[GlobalHeader]\n{\n[Schema 0]\n{\ntype=Network 1;\naiprofile=DEFAULT;\n}\n}\n"),
         0},
        {"objects3d/bravo.3do", std::vector<uint8_t>{9, 8, 7}, 0},
        {"ai/TURBO.txt", bytes_of("turbo\n"), 0},
    };
    const fs::path game = scratch.path / "game";
    write_bytes(game / "rev31.gp3", oa::write_hpi(files));
    return game;
}

/// Two reports that drop BRAVO in different ways fold to the same lines.
void folded_removal() {
    report::Dump left_after;
    report::Dump right_after;
    report::DumpedFile gui;
    gui.parsed = true;
    gui.fields["[gadget0][common] name"] = "BRAVO";
    left_after.files["guis/alpha1.gui"] = gui;
    right_after.files["guis/alpha1.gui"] = gui;
    left_after.hidden.insert("units/bravo.fbi");
    report::DumpedFile fbi;
    fbi.parsed = true;
    right_after.files["units/bravo.fbi"] = fbi;

    const std::vector<std::string> left = {
        "unit dropped BRAVO hidden",
        "file hidden units/bravo.fbi",
        "menu removed ALPHA 1 1 BRAVO",
        "field guis/alpha1.gui [gadget0] grayedout: 0 -> 1",
        "field download/alpha.tdf [menu0] unitname: BRAVO -> (absent)",
        "field units/alpha.fbi [unitinfo] workertime: 1 -> 2",
    };
    const std::vector<std::string> right = {
        "unit dropped BRAVO copyright",
        "field units/bravo.fbi [unitinfo] copyright: old -> ",
        "field units/bravo.fbi [unitinfo] version: 1 -> 4",
        "menu removed ALPHA 1 1 BRAVO",
        "field guis/alpha1.gui [gadget0][common] commonattribs: 4 -> 0",
        "field units/alpha.fbi [unitinfo] workertime: 1 -> 2",
    };
    const report::Comparison compared = report::compare(left, right, left_after, right_after, {});
    OA_CHECK(compared.left_only.empty());
    OA_CHECK(compared.right_only.empty());
    OA_CHECK(compared.unused_patterns.empty());
}

/// A pattern with no reason above it is refused, and the line number is kept.
void allow_file_reason() {
    std::string error;
    OA_CHECK(!report::parse_allowed("left only *\n", &error));
    OA_CHECK(error == "1: a pattern needs a reason above it");
    OA_CHECK(!report::parse_allowed("# a reason\nleft only *\n\nright only *\n", &error));
    OA_CHECK(error == "4: a pattern needs a reason above it");
    const std::optional<report::AllowList> allowed = report::parse_allowed(
        "# a reason\nleft only *\nright only *\n\n# another\nleft only x\n", &error
    );
    OA_CHECK(allowed.has_value() && allowed->patterns.size() == 3);
}

std::string package_arg(const fs::path& folder) {
    return folder.string();
}

void fixture_cases() {
    folded_removal();
    allow_file_reason();

    Scratch scratch;
    const std::string copyright = notice();
    const fs::path game = write_game(scratch, copyright);
    const fs::path boost = write_package(
        scratch,
        "builder-boost",
        "[\"units\"]",
        "function mutate(ctx)\n"
        "  return {\n"
        "    rules = {\n"
        "      where { builder = true, workertime = range(1, 1000000) }:change {\n"
        "        workertime = scale(2)\n"
        "      }\n"
        "    }\n"
        "  }\n"
        "end\n"
    );
    const fs::path boost_sm = write_package(
        scratch,
        "builder-boost-sm",
        "[\"units\"]",
        "function mutate(ctx)\n"
        "  return { rules = { sm \"rules/boost.sm\" } }\n"
        "end\n"
    );
    write_text(
        boost_sm / "rules" / "boost.sm",
        "[*group*]\n{\n[Needs]\n{\nbuilder=1,1;\nworkertime=1,1000000;\n}\n"
        "[Changes]\n{\nworkertime=*2;\n}\n}\n"
    );
    const fs::path boost_3 = write_package(
        scratch,
        "builder-boost-3",
        "[\"units\"]",
        "function mutate(ctx)\n"
        "  return { rules = { sm \"rules/boost.sm\" } }\n"
        "end\n"
    );
    write_text(
        boost_3 / "rules" / "boost.sm",
        "[*group*]\n{\n[Needs]\n{\nbuilder=1,1;\nworkertime=1,1000000;\n}\n"
        "[Changes]\n{\nworkertime=*3;\n}\n}\n"
    );
    const fs::path no_flyers = write_package(
        scratch,
        "no-flyers",
        "[\"units\"]",
        "function mutate(ctx)\n"
        "  return {\n"
        "    units = function(units, ctx)\n"
        "      units:where { canfly = true }:remove()\n"
        "    end\n"
        "  }\n"
        "end\n"
    );
    const fs::path no_flyers_sm = write_package(
        scratch,
        "no-flyers-sm",
        "[\"units\"]",
        "function mutate(ctx)\n"
        "  return { rules = { sm \"rules/drop.sm\" } }\n"
        "end\n"
    );
    write_text(
        no_flyers_sm / "rules" / "drop.sm",
        "[*group*]\n{\n[Needs]\n{\ncanfly=1,1;\n}\n[Changes]\n{\nCopyright=\"\";\nVersion=4;\n}\n}"
        "\n"
    );
    const fs::path no_flyers_noname = write_package(
        scratch,
        "no-flyers-noname",
        "[\"units\"]",
        "function mutate(ctx)\n"
        "  return { rules = { sm \"rules/drop.sm\" } }\n"
        "end\n"
    );
    write_text(
        no_flyers_noname / "rules" / "drop.sm",
        "[*group*]\n{\n[Needs]\n{\ncanfly=1,1;\n}\n[Changes]\n{\nCopyright=\"\";\nVersion=4;\n"
        "UnitName=\"\";\n}\n}\n"
    );
    const std::vector<uint8_t> delta{1, 2, 3, 4, 5};
    const fs::path extra = write_package(
        scratch,
        "extra-model",
        "[\"files\"]",
        "function mutate(ctx)\n"
        "  return {\n"
        "    files = { add = { \"files\" }, mask = { \"units/CHARLIE.fbi\" } }\n"
        "  }\n"
        "end\n"
    );
    write_bytes(extra / "files" / "objects3d" / "delta.3do", delta);
    const fs::path turbo = write_package(
        scratch,
        "ai-turbo",
        "[\"maps\"]",
        "function mutate(ctx)\n"
        "  return {\n"
        "    maps = function(map, ctx)\n"
        "      map.schemas:set { aiprofile = \"TURBO\" }\n"
        "    end\n"
        "  }\n"
        "end\n"
    );
    const fs::path broken = write_package(
        scratch,
        "broken-script",
        "[\"units\"]",
        "function mutate(ctx)\n"
        "  error(\"stopped\")\n"
        "end\n"
    );
    write_text(
        scratch.path / "ridge-mod" / "oamod.yaml",
        "oamod: 1\n"
        "id: ridge-mod\n"
        "name: Ridge Mod\n"
        "version: \"2\"\n"
        "author: {name: unknown}\n"
        "packaging: {revision: 1, date: 2026-10-04, packager: test}\n"
    );

    const std::string game_arg = game.string();
    const std::string boost_arg = package_arg(boost);
    const Captured doubled = run({
        "mutator",
        "diff",
        "--game-dir",
        game_arg,
        "--mutator",
        boost_arg,
    });
    expect_status(doubled, oa::tool::exit_done);
    expect_out(
        doubled,
        "# oa-tool mutator diff 1\n"
        "# game: rev31.gp3\n"
        "# mod: none\n"
        "# loadout: builder-boost 1\n"
        "# seed: 0\n"
        "field units/alpha.fbi [unitinfo] workertime: 100 -> 200\tby builder-boost\n"
        "field units/charlie.fbi [unitinfo] workertime: 50 -> 100\tby builder-boost\n"
    );
    const Captured again = run({
        "mutator",
        "diff",
        "--game-dir",
        game_arg,
        "--mutator",
        boost_arg,
    });
    OA_CHECK(again.out == doubled.out);

    const fs::path dumped = scratch.path / "after.txt";
    const Captured with_dump = run({
        "mutator",
        "diff",
        "--game-dir",
        game_arg,
        "--mutator",
        boost_arg,
        "--dump",
        dumped.string(),
    });
    expect_status(with_dump, oa::tool::exit_done);
    OA_CHECK(with_dump.out == doubled.out);
    std::ifstream dump_in(dumped);
    std::string dump_text((std::istreambuf_iterator<char>(dump_in)), {});
    OA_CHECK(dump_text.find("units/alpha.fbi") != std::string::npos);
    OA_CHECK(dump_text.find("workertime") != std::string::npos);
    OA_CHECK(dump_text.find("200") != std::string::npos);

    {
        WorkingDirectory restored(scratch.path);
        const Captured relative = run({
            "mutator",
            "diff",
            "--game-dir",
            "game",
            "--mutator",
            "packages/builder-boost",
        });
        expect_status(relative, oa::tool::exit_done);
        OA_CHECK(relative.out == doubled.out);
    }

    const Captured same = run({
        "mutator",
        "diff",
        "--game-dir",
        game_arg,
        "--mutator",
        boost_arg,
        "--versus",
        package_arg(boost_sm),
    });
    expect_status(same, oa::tool::exit_done);
    expect_has(same, "mutator diff: 0 differences, 0 allowed, 0 unused allowances");
    expect_has_not(same, "left only");

    const Captured tripled_open = run({
        "mutator",
        "diff",
        "--game-dir",
        game_arg,
        "--mutator",
        boost_arg,
        "--versus",
        package_arg(boost_3),
    });
    expect_status(tripled_open, oa::tool::exit_failed);
    expect_has(tripled_open, "left only field units/alpha.fbi [unitinfo] workertime: 100 -> 200");
    expect_has(tripled_open, "right only field units/alpha.fbi [unitinfo] workertime: 100 -> 300");
    expect_has(tripled_open, "left only field units/charlie.fbi [unitinfo] workertime: 50 -> 100");
    expect_has(tripled_open, "right only field units/charlie.fbi [unitinfo] workertime: 50 -> 150");

    const Captured flyers = run({
        "mutator",
        "diff",
        "--game-dir",
        game_arg,
        "--mutator",
        package_arg(no_flyers),
        "--versus",
        package_arg(no_flyers_sm),
    });
    expect_status(flyers, oa::tool::exit_done);
    expect_has(flyers, "mutator diff: 0 differences, 0 allowed, 0 unused allowances");

    const Captured flyers_alone = run({
        "mutator",
        "diff",
        "--game-dir",
        game_arg,
        "--mutator",
        package_arg(no_flyers),
    });
    expect_status(flyers_alone, oa::tool::exit_done);
    expect_has(flyers_alone, "field guis/alpha1.gui [gadget1] grayedout: 0 -> 1\n");
    expect_has(flyers_alone, "field guis/alpha1.gui [gadget1][common] commonattribs: 4 -> 0\n");
    expect_has_not(flyers_alone, "[gadget1@2]");

    const Captured flyers_name = run({
        "mutator",
        "diff",
        "--game-dir",
        game_arg,
        "--mutator",
        package_arg(no_flyers),
        "--versus",
        package_arg(no_flyers_noname),
    });
    expect_status(flyers_name, oa::tool::exit_done);
    expect_has(flyers_name, "mutator diff: 0 differences, 0 allowed, 0 unused allowances");

    const Captured dropped = run({
        "mutator",
        "diff",
        "--game-dir",
        game_arg,
        "--mutator",
        package_arg(no_flyers_sm),
    });
    expect_status(dropped, oa::tool::exit_done);
    expect_has(dropped, "unit dropped BRAVO copyright");
    expect_has(dropped, "[unitinfo] copyright:");
    expect_has(dropped, "[unitinfo] version:");

    const sha256::Digest digest = sha256::digest_of(delta);
    const auto hex = sha256::to_hex(digest);
    const std::string hash(hex.begin(), hex.end());
    const Captured masked = run({
        "mutator",
        "diff",
        "--game-dir",
        game_arg,
        "--mutator",
        package_arg(extra),
    });
    expect_status(masked, oa::tool::exit_done);
    expect_has(masked, "file added objects3d/delta.3do " + hash + "\tby extra-model\n");
    expect_has(masked, "file hidden units/charlie.fbi\tby extra-model\n");
    expect_has(masked, "unit dropped CHARLIE hidden\tby extra-model\n");
    expect_has(masked, "menu removed ALPHA 1 2 CHARLIE\n");

    const Captured mapped = run({
        "mutator",
        "diff",
        "--game-dir",
        game_arg,
        "--map",
        "isle",
        "--mutator",
        package_arg(turbo),
    });
    expect_status(mapped, oa::tool::exit_done);
    expect_has(mapped, "# map: isle");
    expect_has(mapped, "aiprofile:");
    expect_has(mapped, "TURBO");

    const Captured shouted = run({
        "mutator",
        "diff",
        "--game-dir",
        game_arg,
        "--map",
        "ISLE",
        "--mutator",
        package_arg(turbo),
    });
    expect_status(shouted, oa::tool::exit_done);
    OA_CHECK(shouted.out == mapped.out);

    const Captured unmapped = run({
        "mutator",
        "diff",
        "--game-dir",
        game_arg,
        "--mutator",
        package_arg(turbo),
    });
    expect_status(unmapped, oa::tool::exit_done);
    expect_has_not(unmapped, "aiprofile");
    expect_has_not(unmapped, "isle.ota");
    expect_has_not(unmapped, "# map:");

    const Captured empty = run({"mutator", "diff", "--game-dir", game_arg});
    expect_status(empty, oa::tool::exit_done);
    expect_out(
        empty,
        "# oa-tool mutator diff 1\n"
        "# game: rev31.gp3\n"
        "# mod: none\n"
        "# loadout: none\n"
        "# seed: 0\n"
    );

    const Captured modded = run({
        "mutator",
        "diff",
        "--game-dir",
        game_arg,
        "--mod-dir",
        (scratch.path / "ridge-mod").string(),
    });
    expect_status(modded, oa::tool::exit_done);
    expect_has(modded, "# mod: ridge-mod ");

    const fs::path allow = scratch.path / "allow.txt";
    write_text(
        allow,
        "# doubled and tripled worker time\n"
        "left only field units/*.fbi [unitinfo] workertime:*\n"
        "right only field units/*.fbi [unitinfo] workertime:*\n"
    );
    const Captured tripled = run({
        "mutator",
        "diff",
        "--game-dir",
        game_arg,
        "--mutator",
        boost_arg,
        "--versus",
        package_arg(boost_3),
        "--allow",
        allow.string(),
    });
    expect_status(tripled, oa::tool::exit_done);
    expect_has(tripled, "mutator diff: 0 differences, ");

    const fs::path bare = scratch.path / "bare.txt";
    write_text(bare, "left only *\n");
    const Captured no_reason = run({
        "mutator",
        "diff",
        "--game-dir",
        game_arg,
        "--mutator",
        boost_arg,
        "--versus",
        package_arg(boost_3),
        "--allow",
        bare.string(),
    });
    expect_status(no_reason, oa::tool::exit_usage);
    expect_err(no_reason, "a pattern needs a reason above it");

    const fs::path unused = scratch.path / "unused.txt";
    write_text(unused, "# nothing like this remains\nleft only no-such-line*\n");
    const Captured unused_run = run({
        "mutator",
        "diff",
        "--game-dir",
        game_arg,
        "--mutator",
        boost_arg,
        "--versus",
        package_arg(boost_sm),
        "--allow",
        unused.string(),
    });
    expect_status(unused_run, oa::tool::exit_failed);
    expect_has(unused_run, "allowed difference matched nothing:");

    const Captured failed = run({
        "mutator",
        "diff",
        "--game-dir",
        game_arg,
        "--mutator",
        package_arg(broken),
    });
    expect_status(failed, oa::tool::exit_failed);
    expect_err(failed, "broken-script");
    expect_has_not(failed, "# oa-tool mutator diff");

    const Captured no_game = run({"mutator", "diff"});
    expect_status(no_game, oa::tool::exit_usage);
    expect_err(no_game, "game-dir");

    const Captured by_id = run({
        "mutator",
        "diff",
        "--game-dir",
        game_arg,
        "--mutator",
        "builder-boost",
    });
    expect_status(by_id, oa::tool::exit_usage);
    expect_err(by_id, "builder-boost");
    expect_err(by_id, "id");

    const Captured speed = run({
        "mutator",
        "diff",
        "--game-dir",
        game_arg,
        "--mutator",
        boost_arg + ":speed=1",
    });
    expect_status(speed, oa::tool::exit_usage);
    expect_err(speed, "speed");

    const fs::path refused_dump = scratch.path / "refused.txt";
    const Captured refused = run({
        "mutator",
        "diff",
        "--game-dir",
        game_arg,
        "--mutator",
        boost_arg + ":speed=1",
        "--dump",
        refused_dump.string(),
    });
    expect_status(refused, oa::tool::exit_usage);
    OA_CHECK(!fs::exists(refused_dump));

    const Captured positional = run({
        "mutator",
        "diff",
        "extra",
        "--game-dir",
        game_arg,
    });
    expect_status(positional, oa::tool::exit_usage);

    const Captured bad_seed = run({
        "mutator",
        "diff",
        "--game-dir",
        game_arg,
        "--seed",
        "12x",
    });
    expect_status(bad_seed, oa::tool::exit_usage);
    expect_err(bad_seed, "seed");

    const Captured allow_alone = run({
        "mutator",
        "diff",
        "--game-dir",
        game_arg,
        "--allow",
        allow.string(),
    });
    expect_status(allow_alone, oa::tool::exit_usage);
    expect_err(allow_alone, "--allow needs --versus");

    const fs::path empty_dir = scratch.path / "empty-game";
    fs::create_directories(empty_dir);
    const Captured empty_game = run({
        "mutator",
        "diff",
        "--game-dir",
        empty_dir.string(),
    });
    expect_status(empty_game, oa::tool::exit_failed);
    OA_CHECK(!empty_game.err.empty());

    const fs::path not_dir = scratch.path / "not-a-dir";
    write_text(not_dir, "nope\n");
    const Captured file_game = run({
        "mutator",
        "diff",
        "--game-dir",
        not_dir.string(),
    });
    expect_status(file_game, oa::tool::exit_failed);
    OA_CHECK(!file_game.err.empty());

    const Captured stacked = run({
        "mutator",
        "diff",
        "--game-dir",
        game_arg,
        "--mutator",
        boost_arg,
        "--mutator",
        package_arg(turbo),
    });
    expect_status(stacked, oa::tool::exit_done);
    expect_has(stacked, "# loadout: builder-boost 1; ai-turbo 1\n");

    const fs::path paced = scratch.path / "packages" / "paced";
    write_text(
        paced / "oamut.yaml",
        manifest("paced", "[\"units\"]") +
            "options:\n  factor:\n    type: choice\n    label: \"Build speed\"\n"
            "    values: [2, 4, 8]\n    default: 2\n"
    );
    write_text(
        paced / "mutator.lua",
        "function mutate(ctx)\n"
        "  return { rules = { where { workertime = range(0, 0) }:change { workertime = 1 } } }\n"
        "end\n"
    );
    const Captured paced_run = run({
        "mutator",
        "diff",
        "--game-dir",
        game_arg,
        "--mutator",
        package_arg(paced) + ":factor=8",
    });
    expect_status(paced_run, oa::tool::exit_done);
    expect_has(paced_run, "# loadout: paced 1 factor=8\n");

    const Captured two_versus = run({
        "mutator",
        "diff",
        "--game-dir",
        game_arg,
        "--mutator",
        boost_arg,
        "--versus",
        package_arg(boost_sm),
        "--versus",
        package_arg(turbo),
    });
    expect_status(two_versus, oa::tool::exit_done);
    expect_has(two_versus, "# versus: builder-boost-sm 1; ai-turbo 1\n");
    expect_has(two_versus, "mutator diff: 0 differences, 0 allowed, 0 unused allowances");

    const fs::path too_new = write_package(
        scratch,
        "too-new",
        "[\"units\"]",
        "function mutate(ctx)\n"
        "  return { rules = { where { canfly = true }:change { version = 4 } } }\n"
        "end\n"
    );
    const Captured versioned = run({
        "mutator",
        "diff",
        "--game-dir",
        game_arg,
        "--mutator",
        package_arg(too_new),
    });
    expect_status(versioned, oa::tool::exit_done);
    expect_has(versioned, "unit dropped BRAVO version");

    const fs::path range_a = write_package(
        scratch,
        "range-a",
        "[\"weapons\"]",
        "function mutate(ctx)\n"
        "  return { rules = { on(\"weapons\", \"tdf\"):where { id = 1 }:change { range = 600 } } "
        "}\n"
        "end\n"
    );
    const fs::path range_b = write_package(
        scratch,
        "range-b",
        "[\"weapons\"]",
        "function mutate(ctx)\n"
        "  return { rules = { on(\"weapons\", \"tdf\"):where { id = 2 }:change { range = 800 } } "
        "}\n"
        "end\n"
    );
    const Captured ranges = run({
        "mutator",
        "diff",
        "--game-dir",
        game_arg,
        "--mutator",
        package_arg(range_a),
        "--mutator",
        package_arg(range_b),
    });
    expect_status(ranges, oa::tool::exit_done);
    expect_has(ranges, "field weapons/test.tdf [gun] range: 300 -> 600\tby range-a\n");
    expect_has(ranges, "field weapons/test.tdf [cannon] range: 400 -> 800\tby range-b\n");
    expect_has_not(ranges, "by range-a,range-b");
    expect_has_not(ranges, "by range-b,range-a");

    const std::string left_note = "x\tLEFT\r\nMORE";
    const std::string right_note = "x\tRIGHT\r\nMORE";
    const fs::path odd_left = write_package(
        scratch,
        "odd-left",
        "[\"files\"]",
        "function mutate(ctx)\n  return { files = { add = { \"files\" } } }\nend\n"
    );
    const fs::path odd_right = write_package(
        scratch,
        "odd-right",
        "[\"files\"]",
        "function mutate(ctx)\n  return { files = { add = { \"files\" } } }\nend\n"
    );
    write_text(
        odd_left / "files" / "weapons" / "note.tdf", "[NOTE]\n{\ntext=" + left_note + ";\n}\n"
    );
    write_text(
        odd_right / "files" / "weapons" / "note.tdf", "[NOTE]\n{\ntext=" + right_note + ";\n}\n"
    );
    const Captured odd = run({
        "mutator",
        "diff",
        "--game-dir",
        game_arg,
        "--mutator",
        package_arg(odd_left),
        "--versus",
        package_arg(odd_right),
    });
    expect_status(odd, oa::tool::exit_failed);
    expect_has(odd, "x\\x09LEFT\\x0D\\x0AMORE");
    expect_has(odd, "x\\x09RIGHT\\x0D\\x0AMORE");

    const std::vector<uint8_t> replaced{4, 5, 6};
    const fs::path new_model = write_package(
        scratch,
        "new-model",
        "[\"files\"]",
        "function mutate(ctx)\n  return { files = { add = { \"files\" } } }\nend\n"
    );
    write_bytes(new_model / "files" / "objects3d" / "bravo.3do", replaced);
    const sha256::Digest old_digest = sha256::digest_of(std::vector<uint8_t>{9, 8, 7});
    const sha256::Digest new_digest = sha256::digest_of(replaced);
    const auto old_hex = sha256::to_hex(old_digest);
    const auto new_hex = sha256::to_hex(new_digest);
    const std::string old_hash(old_hex.begin(), old_hex.end());
    const std::string new_hash(new_hex.begin(), new_hex.end());
    const Captured replaced_run = run({
        "mutator",
        "diff",
        "--game-dir",
        game_arg,
        "--mutator",
        package_arg(new_model),
    });
    expect_status(replaced_run, oa::tool::exit_done);
    expect_has(
        replaced_run,
        "file replaced objects3d/bravo.3do " + old_hash + " " + new_hash + "\tby new-model\n"
    );

    const fs::path new_unit = write_package(
        scratch,
        "new-unit",
        "[\"files\"]",
        "function mutate(ctx)\n  return { files = { add = { \"files\" } } }\nend\n"
    );
    write_text(
        new_unit / "files" / "units" / "DELTA.fbi", fbi("DELTA", "MaxDamage=10;\n", copyright)
    );
    write_text(
        new_unit / "files" / "download" / "delta.tdf",
        "[MENU0]\n{\nUNITMENU=ALPHA;\nMENU=1;\nBUTTON=0;\nUNITNAME=DELTA;\n}\n"
    );
    const Captured added = run({
        "mutator",
        "diff",
        "--game-dir",
        game_arg,
        "--mutator",
        package_arg(new_unit),
    });
    expect_status(added, oa::tool::exit_done);
    expect_has(added, "unit added DELTA");
    expect_has(added, "menu added ALPHA 1 0 DELTA");

    const Captured missing_map = run({
        "mutator",
        "diff",
        "--game-dir",
        game_arg,
        "--map",
        "No Such Map",
    });
    expect_status(missing_map, oa::tool::exit_usage);
    expect_err(missing_map, "map No Such Map is not in this game");

    const Captured help = run({"help", "mutator", "diff"});
    expect_status(help, oa::tool::exit_done);
    OA_CHECK(help.out.find("--versus") != std::string::npos);
    OA_CHECK(help.out.find("--allow") != std::string::npos);
}

/// Reads one number when the whole text is that number.
bool parse_number(std::string_view text, double& value) {
    if (text.empty())
        return false;
    const std::string owned(text);
    char* end = nullptr;
    value = std::strtod(owned.c_str(), &end);
    return end != owned.c_str() && end != nullptr && *end == '\0';
}

/// Builders whose WorkerTime a doubling rule would change, counted from a dump.
std::size_t builders_in_dump(std::string_view dump) {
    std::size_t count = 0;
    std::string path;
    bool builder = false;
    bool worker = false;
    double worker_time = 0;
    const auto finish = [&]() {
        if (builder && worker && worker_time >= 1.0 && worker_time <= 1000000.0)
            ++count;
        builder = false;
        worker = false;
        worker_time = 0;
    };
    std::size_t begin = 0;
    while (begin <= dump.size()) {
        auto end = dump.find('\n', begin);
        if (end == std::string_view::npos)
            end = dump.size();
        const std::string_view line = dump.substr(begin, end - begin);
        if (!line.empty() && line.front() != '\t') {
            finish();
            const auto tab = line.find('\t');
            path = std::string(line.substr(0, tab));
        } else if (
            path.starts_with("units/") && path.ends_with(".fbi") &&
            path.find('/', 6) == std::string::npos
        ) {
            const std::string_view body = line.substr(1);
            const auto mark = body.find(" = ");
            if (mark != std::string_view::npos) {
                const std::string_view key = body.substr(0, mark);
                const std::string_view value = body.substr(mark + 3);
                double number = 0;
                if (key == "[unitinfo] builder" && parse_number(value, number) && number == 1.0)
                    builder = true;
                if (key == "[unitinfo] workertime" && parse_number(value, number)) {
                    worker = true;
                    worker_time = number;
                }
            }
        }
        if (end == dump.size())
            break;
        begin = end + 1;
    }
    finish();
    return count;
}

/// Every change line is a doubled WorkerTime, and the count matches the dump.
void installed_case() {
    const fs::path game = oa::test::require_game_directory("a loadout diffed against the game");
    Scratch scratch;
    const fs::path boost = write_package(
        scratch,
        "builder-boost",
        "[\"units\"]",
        "function mutate(ctx)\n"
        "  return {\n"
        "    rules = {\n"
        "      where { builder = true, workertime = range(1, 1000000) }:change {\n"
        "        workertime = scale(2)\n"
        "      }\n"
        "    }\n"
        "  }\n"
        "end\n"
    );
    const fs::path boost_sm = write_package(
        scratch,
        "builder-boost-sm",
        "[\"units\"]",
        "function mutate(ctx)\n"
        "  return { rules = { sm \"rules/boost.sm\" } }\n"
        "end\n"
    );
    write_text(
        boost_sm / "rules" / "boost.sm",
        "[*group*]\n{\n[Needs]\n{\nbuilder=1,1;\nworkertime=1,1000000;\n}\n"
        "[Changes]\n{\nworkertime=*2;\n}\n}\n"
    );
    const fs::path dumped = scratch.path / "base.txt";
    const Captured base = run({
        "mutator",
        "diff",
        "--game-dir",
        game.string(),
        "--dump",
        dumped.string(),
    });
    expect_status(base, oa::tool::exit_done);
    std::ifstream dump_in(dumped);
    const std::string dump_text((std::istreambuf_iterator<char>(dump_in)), {});
    const std::size_t expected = builders_in_dump(dump_text);
    OA_CHECK(expected > 0);

    const Captured doubled = run({
        "mutator",
        "diff",
        "--game-dir",
        game.string(),
        "--mutator",
        boost.string(),
    });
    expect_status(doubled, oa::tool::exit_done);
    std::size_t seen = 0;
    std::size_t begin = 0;
    while (begin <= doubled.out.size()) {
        auto end = doubled.out.find('\n', begin);
        if (end == std::string::npos)
            end = doubled.out.size();
        const std::string line = doubled.out.substr(begin, end - begin);
        if (!line.empty() && line.front() != '#') {
            const auto tab = line.find('\t');
            const std::string body = line.substr(0, tab);
            constexpr std::string_view prefix = "field units/";
            constexpr std::string_view middle = ".fbi [unitinfo] workertime: ";
            const auto mid = body.find(middle);
            if (!body.starts_with(prefix) || mid == std::string::npos ||
                body.find('/', prefix.size()) < mid) {
                std::fprintf(stderr, "unexpected change: %s\n", line.c_str());
                OA_CHECK(false);
            } else {
                const std::string_view rest = std::string_view(body).substr(mid + middle.size());
                const auto arrow = rest.find(" -> ");
                double old_value = 0;
                double new_value = 0;
                const bool numbers = arrow != std::string_view::npos &&
                                     parse_number(rest.substr(0, arrow), old_value) &&
                                     parse_number(rest.substr(arrow + 4), new_value);
                if (!numbers) {
                    std::fprintf(stderr, "unparsed change: %s\n", line.c_str());
                    OA_CHECK(false);
                } else {
                    OA_CHECK(std::abs(new_value - old_value * 2.0) <= 1e-4);
                    ++seen;
                }
            }
        }
        if (end == doubled.out.size())
            break;
        begin = end + 1;
    }
    if (seen != expected)
        std::fprintf(stderr, "doubled %zu builders, dump counted %zu\n", seen, expected);
    OA_CHECK(seen == expected);

    const Captured compared = run({
        "mutator",
        "diff",
        "--game-dir",
        game.string(),
        "--mutator",
        boost.string(),
        "--versus",
        boost_sm.string(),
    });
    expect_status(compared, oa::tool::exit_done);
    expect_has(compared, "mutator diff: 0 differences, 0 allowed, 0 unused allowances");

    const fs::path drop_hook = write_package(
        scratch,
        "drop-metal",
        "[\"units\"]",
        "function mutate(ctx)\n"
        "  return {\n"
        "    units = function(units, ctx)\n"
        "      units:where { makesmetal = range(1, 127) }:remove()\n"
        "    end\n"
        "  }\n"
        "end\n"
    );
    const fs::path drop_sm = write_package(
        scratch,
        "drop-metal-sm",
        "[\"units\"]",
        "function mutate(ctx)\n"
        "  return { rules = { sm \"rules/drop.sm\" } }\n"
        "end\n"
    );
    write_text(
        drop_sm / "rules" / "drop.sm",
        "[*group*]\n{\n[Needs]\n{\nmakesmetal=1,127;\n}\n[Changes]\n{\nCopyright=\"\";\n"
        "Version=4;\n}\n}\n"
    );
    const Captured metal = run({
        "mutator",
        "diff",
        "--game-dir",
        game.string(),
        "--mutator",
        drop_hook.string(),
        "--versus",
        drop_sm.string(),
    });
    expect_status(metal, oa::tool::exit_done);
    expect_has(metal, "mutator diff: 0 differences, 0 allowed, 0 unused allowances");

    // The wind generators' build buttons sit in gadgets a build page repeats.
    const fs::path wind_hook = write_package(
        scratch,
        "drop-wind",
        "[\"units\"]",
        "function mutate(ctx)\n"
        "  return {\n"
        "    units = function(units, ctx)\n"
        "      units:where { windgenerator = range(1, 1000) }:remove()\n"
        "    end\n"
        "  }\n"
        "end\n"
    );
    const fs::path wind_sm = write_package(
        scratch,
        "drop-wind-sm",
        "[\"units\"]",
        "function mutate(ctx)\n"
        "  return { rules = { sm \"rules/drop.sm\" } }\n"
        "end\n"
    );
    write_text(
        wind_sm / "rules" / "drop.sm",
        "[*group*]\n{\n[Needs]\n{\nwindgenerator=1,1000;\n}\n[Changes]\n{\nCopyright=\"\";\n"
        "Version=4;\n}\n}\n"
    );
    const Captured wind = run({
        "mutator",
        "diff",
        "--game-dir",
        game.string(),
        "--mutator",
        wind_hook.string(),
        "--versus",
        wind_sm.string(),
    });
    expect_status(wind, oa::tool::exit_done);
    expect_has(wind, "mutator diff: 0 differences, 0 allowed, 0 unused allowances");
}

} // namespace

int main(int argc, char** argv) {
    if (oa::test::game_data_requested(argc, argv))
        installed_case();
    else
        fixture_cases();
    return oa::test::check_exit_status();
}
