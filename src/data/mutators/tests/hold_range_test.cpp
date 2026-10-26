// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Two multipliers past the 16-bit end of turn rate and worker time, which
// the game reads unsigned. The published unit stores 65535, the dry run
// notes each field once, and the verdict stays a pass. A value that still
// fits is left as scaled. Further cases hold each unit and weapon field at
// the range the game reads it in, and leave a value inside that range.
#include "oa/core/feature_def.h"
#include "oa/data/defs/categories.hpp"
#include "oa/data/defs/layout.hpp"
#include "oa/data/defs/unit_def_loader.hpp"
#include "oa/data/defs/weapons.hpp"
#include "oa/data/mutators/dry_run.hpp"
#include "oa/data/mutators/pipeline.hpp"
#include "oa/formats/oamut/manifest.hpp"
#include "oa/formats/tdf.hpp"
#include "oa/sim/map_runtime/feature_defs.hpp"
#include "oa/test/check.hpp"
#include "oa/test/scratch_directory.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#ifndef OA_MUTATORS_TEST_DATA
#error OA_MUTATORS_TEST_DATA is the fixture folder
#endif

namespace {

namespace mutators = oa::data::mutators;
namespace defs = oa::data::defs;
namespace fs = std::filesystem;

const fs::path k_ridge{OA_MUTATORS_TEST_DATA "/ridge"};

void write_text(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    OA_CHECK(out.good());
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    OA_CHECK(out.good());
}

void write_package(
    const fs::path& folder,
    std::string_view id,
    std::string_view lua,
    std::string_view layers = "\"units\""
) {
    std::string yaml = "oamut: 1\nid: ";
    yaml.append(id);
    yaml += "\nname: \"";
    yaml.append(id);
    yaml += "\"\nversion: \"1.0\"\nauthor:\n  name: \"Ridge Maker\"\n"
            "tags: [\"gameplay\"]\nworks-with: [\"*\"]\nlayers: [";
    yaml.append(layers);
    yaml += "]\n";
    write_text(folder / "oamut.yaml", yaml);
    write_text(folder / "mutator.lua", lua);
}

mutators::PipelineMutator load_mutator(const fs::path& package) {
    mutators::PipelineMutator mutator;
    mutator.package = package;
    std::ifstream in(package / "oamut.yaml", std::ios::binary);
    OA_CHECK(in.good());
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<uint8_t> bytes(text.begin(), text.end());
    std::vector<oa::formats::oamut::Problem> problems;
    OA_CHECK(oa::formats::oamut::read_manifest(bytes, mutator.manifest, problems));
    return mutator;
}

std::string layer_text(const oa::MutatorLayerSpec& spec, std::string_view name) {
    for (const oa::MutatorLayerEntry& entry : spec.entries) {
        if (entry.contents == nullptr)
            continue;
        bool named = false;
        if (entry.path.size() >= name.size()) {
            named = true;
            for (std::size_t index = 0; index < name.size(); ++index) {
                char left = entry.path[entry.path.size() - name.size() + index];
                char right = name[index];
                if (left >= 'A' && left <= 'Z')
                    left = static_cast<char>(left - 'A' + 'a');
                if (right >= 'A' && right <= 'Z')
                    right = static_cast<char>(right - 'A' + 'a');
                if (left != right)
                    named = false;
            }
        }
        if (!named)
            continue;
        return std::string(entry.contents->begin(), entry.contents->end());
    }
    return {};
}

struct MemoryFiles {
    std::string text;

    defs::Files services() {
        return defs::Files{
            this,
            [](void* context, const char*, uint8_t** data, uint32_t* size, bool* archived) {
                const auto* self = static_cast<const MemoryFiles*>(context);
                *data = static_cast<uint8_t*>(std::malloc(self->text.size() + 1));
                OA_CHECK(*data != nullptr);
                if (*data == nullptr)
                    return false;
                std::memcpy(*data, self->text.data(), self->text.size());
                (*data)[self->text.size()] = 0;
                *size = static_cast<uint32_t>(self->text.size());
                *archived = false;
                return true;
            },
            [](void*, uint8_t* data) { std::free(data); },
            [](void*, const char*) { return true; },
            [](void*, const char*, const char*, void (*)(void*, const char*), void*) {},
        };
    }
};

int16_t loaded_field(std::string_view fbi, bool turn) {
    MemoryFiles files;
    files.text = std::string{fbi};
    defs::Files services = files.services();
    defs::CategoryRegistry categories;
    defs::category_registry_init(&categories);
    oa::UnitDef unit{};
    defs::UnitDefSources sources;
    sources.language = "";
    sources.categories = &categories;
    const bool loaded = defs::load_unit_def(&services, "units/FAST.FBI", unit, sources);
    const int16_t value = turn ? unit.turn_rate : unit.worker_time;
    defs::category_registry_clear(&categories);
    OA_CHECK(loaded);
    return value;
}

void test_stacked_turn_and_worker_are_held() {
    const fs::path scratch = oa::test::make_scratch_directory("V19-hold");
    write_text(
        scratch / "units" / "FAST.FBI",
        "[UNITINFO]\n{\nUnitName=FAST;\nCommander=1;\nBMcode=1;\nTurnRate=100;\nWorkerTime=100;\n}"
        "\n"
    );
    write_text(
        scratch / "units" / "SLOW.FBI",
        "[UNITINFO]\n{\nUnitName=SLOW;\nCommander=1;\nBMcode=1;\nTurnRate=10;\nWorkerTime=10;\n}\n"
    );
    write_text(scratch / "objects3d" / "FAST.3DO", " ");
    write_text(scratch / "objects3d" / "SLOW.3DO", " ");
    write_text(scratch / "scripts" / "FAST.COB", " ");
    write_text(scratch / "scripts" / "SLOW.COB", " ");
    constexpr std::string_view lua = "function mutate(ctx)\n"
                                     "  return {\n"
                                     "    units = function(units, ctx)\n"
                                     "      units:where { unitname = \"FAST\" }:scale { turnrate "
                                     "= 30, workertime = 30 }\n"
                                     "      units:where { unitname = \"SLOW\" }:scale { turnrate "
                                     "= 30, workertime = 30 }\n"
                                     "    end\n"
                                     "  }\n"
                                     "end\n";
    write_package(scratch / "wide", "ridge-hold-wide", lua);
    write_package(scratch / "again", "ridge-hold-again", lua);

    mutators::PipelineInput input;
    input.mutators = {load_mutator(scratch / "wide"), load_mutator(scratch / "again")};
    input.mod.id = "ridge";
    input.mod.version = "1.0";
    input.mod.game = "ridge";
    input.engine_version = "0.8.5";
    input.layout = &defs::data_layout();
    input.seed = 1;

    mutators::DryRunJob job;
    job.loose_roots = {scratch, k_ridge};
    job.mod_label = "Ridge 1.0";
    job.input = input;
    const std::atomic<bool> cancel{false};
    const mutators::VerdictRecord verdict = mutators::run_dry_run(job, cancel);
    OA_CHECK(verdict.verdict == mutators::Verdict::compatible);
    OA_CHECK(verdict.reason.empty());

    oa::AssetStore store(std::vector<fs::path>{scratch, k_ridge});
    const mutators::PipelineResult result = mutators::run_pipeline(store, input);
    if (store.mutator_layer_mounted())
        store.unmount_mutator_layer();
    OA_CHECK(result.refusal.empty());
    OA_CHECK(result.mutators.size() == 2);
    if (result.mutators.size() == 2) {
        OA_CHECK(result.mutators[0].outcome == mutators::Outcome::applied);
        OA_CHECK(result.mutators[1].outcome == mutators::Outcome::applied);
    }

    int turn_notes = 0;
    int worker_notes = 0;
    for (const mutators::Conflict& conflict : result.report.conflicts) {
        if (conflict.kind != mutators::ConflictKind::value_held)
            continue;
        const std::string line = mutators::describe(conflict, {});
        if (conflict.record == "FAST" && conflict.field == "turnrate") {
            OA_CHECK(line == "FAST: turnrate 90000 held at 65535");
            ++turn_notes;
        }
        if (conflict.record == "FAST" && conflict.field == "workertime") {
            OA_CHECK(line == "FAST: workertime 90000 held at 65535");
            ++worker_notes;
        }
        OA_CHECK(conflict.record != "SLOW");
    }
    OA_CHECK(turn_notes == 1);
    OA_CHECK(worker_notes == 1);

    const std::string fast = layer_text(result.layer, "FAST.FBI");
    const std::string slow = layer_text(result.layer, "SLOW.FBI");
    OA_CHECK(fast.find("65535") != std::string::npos);
    OA_CHECK(slow.find("9000") != std::string::npos);
    OA_CHECK(static_cast<uint16_t>(loaded_field(fast, true)) == 65535u);
    OA_CHECK(static_cast<uint16_t>(loaded_field(fast, false)) == 65535u);
    OA_CHECK(loaded_field(slow, true) == 9000);
    OA_CHECK(loaded_field(slow, false) == 9000);
}

/// Runs one package's hooks over the scratch game and the ridge fixture.
mutators::PipelineResult run_hooks(
    const fs::path& scratch,
    std::string_view id,
    std::string_view lua,
    std::string_view layers = "\"units\", \"weapons\""
) {
    write_package(scratch / id, id, lua, layers);
    mutators::PipelineInput input;
    input.mutators = {load_mutator(scratch / id)};
    input.mod.id = "ridge";
    input.mod.version = "1.0";
    input.mod.game = "ridge";
    input.engine_version = "0.8.5";
    input.layout = &defs::data_layout();
    input.seed = 1;
    oa::AssetStore store(std::vector<fs::path>{scratch, k_ridge});
    mutators::PipelineResult result = mutators::run_pipeline(store, input);
    if (store.mutator_layer_mounted())
        store.unmount_mutator_layer();
    OA_CHECK(result.refusal.empty());
    OA_CHECK(result.mutators.size() == 1);
    if (result.mutators.size() == 1) {
        OA_CHECK(result.mutators[0].outcome == mutators::Outcome::applied);
        if (result.mutators[0].outcome != mutators::Outcome::applied)
            std::fprintf(stderr, "%s\n", result.mutators[0].message.c_str());
    }
    return result;
}

/// The held notes, as the dry run words them.
std::vector<std::string> held_lines(const mutators::PipelineResult& result) {
    std::vector<std::string> lines;
    for (const mutators::Conflict& conflict : result.report.conflicts) {
        if (conflict.kind == mutators::ConflictKind::value_held)
            lines.push_back(mutators::describe(conflict, {}));
    }
    return lines;
}

bool has_line(const std::vector<std::string>& lines, std::string_view line) {
    for (const std::string& each : lines) {
        if (each == line)
            return true;
    }
    return false;
}

// A mod's own value past a field's range loads as it always has, wrapped,
// when a mutator changes another field of the same file. Twelve packaged
// mods ship such values (buildangle 65536, healtime 900000, a weapon file's
// sprayangle 70000 or default damage 99999); holding them would change the
// mod's units in every mutated match.
void test_own_values_are_left() {
    const fs::path scratch = oa::test::make_scratch_directory("V19-own");
    write_text(
        scratch / "units" / "OWN.FBI",
        "[UNITINFO]\n{\nUnitName=OWN;\nCommander=1;\nBMcode=1;\nTurnRate=100;\n"
        "HealTime=900000;\nTransportCapacity=255;\nBuildAngle=65536;\n}\n"
    );
    write_text(scratch / "objects3d" / "OWN.3DO", " ");
    write_text(scratch / "scripts" / "OWN.COB", " ");
    write_text(
        scratch / "weapons" / "OWN.TDF",
        "[OWN_GUN]\n{\nID=3;\nreloadtime=1;\nburst=36000;\n[DAMAGE]\n{\ndefault=99999;\n}\n}\n"
        "[OWN_BLAST]\n{\nID=4;\nsprayangle=70000;\n}\n"
    );
    const mutators::PipelineResult result = run_hooks(
        scratch,
        "ridge-hold-own",
        "function mutate(ctx)\n"
        "  return {\n"
        "    units = function(units, ctx)\n"
        "      units.OWN.turnrate = 90000\n"
        "    end,\n"
        "    weapons = function(weapons, ctx)\n"
        "      weapons.OWN_GUN.reloadtime = 2000\n"
        "    end\n"
        "  }\n"
        "end\n"
    );
    const std::vector<std::string> lines = held_lines(result);
    OA_CHECK(lines.size() == 2);
    OA_CHECK(has_line(lines, "OWN: turnrate 90000 held at 65535"));
    OA_CHECK(has_line(lines, "OWN_GUN: reloadtime 2000 held at 1820.44"));

    const std::string unit = layer_text(result.layer, "OWN.FBI");
    OA_CHECK(unit.find("65535") != std::string::npos);
    OA_CHECK(unit.find("900000") != std::string::npos);
    OA_CHECK(unit.find("=255;") != std::string::npos);
    OA_CHECK(unit.find("65536") != std::string::npos);
    const std::string weapon = layer_text(result.layer, "OWN.TDF");
    OA_CHECK(weapon.find("reloadtime=1820.44;") != std::string::npos);
    OA_CHECK(weapon.find("36000") != std::string::npos);
    OA_CHECK(weapon.find("99999") != std::string::npos);
    OA_CHECK(weapon.find("70000") != std::string::npos);
}

/// The unit record the loader makes of an FBI's text.
oa::UnitDef loaded_unit(std::string_view fbi) {
    MemoryFiles files;
    files.text = std::string{fbi};
    defs::Files services = files.services();
    defs::CategoryRegistry categories;
    defs::category_registry_init(&categories);
    oa::UnitDef unit{};
    defs::UnitDefSources sources;
    sources.language = "";
    sources.categories = &categories;
    const bool loaded = defs::load_unit_def(&services, "units/TOUGH.FBI", unit, sources);
    defs::category_registry_clear(&categories);
    OA_CHECK(loaded);
    return unit;
}

// Three whole numbers reach the game narrower than their record: health is
// the low 16 bits of maxdamage, a hit's damage is 16 bits whatever the
// [DAMAGE] entry, and the self-destruct countdown is 3 bits. Each is held,
// and so is a value set past the negative end of a signed field (sight
// distance) or below 0 in a field the game reads unsigned (turn rate).
void test_narrower_than_their_record() {
    const fs::path scratch = oa::test::make_scratch_directory("V19-narrow");
    write_text(
        scratch / "units" / "TOUGH.FBI",
        "[UNITINFO]\n{\nUnitName=TOUGH;\nBMcode=1;\nMaxDamage=3000;\nTurnRate=100;\n"
        "SelfDestructCountdown=5;\n}\n"
    );
    write_text(
        scratch / "units" / "FRAIL.FBI",
        "[UNITINFO]\n{\nUnitName=FRAIL;\nBMcode=1;\nMaxDamage=300;\n}\n"
    );
    write_text(scratch / "objects3d" / "TOUGH.3DO", " ");
    write_text(scratch / "objects3d" / "FRAIL.3DO", " ");
    write_text(scratch / "scripts" / "TOUGH.COB", " ");
    write_text(scratch / "scripts" / "FRAIL.COB", " ");
    write_text(
        scratch / "weapons" / "TOUGH.TDF",
        "[TOUGH_GUN]\n{\nID=3;\n[DAMAGE]\n{\ndefault=100;\nTOUGH=2000;\n}\n}\n"
    );
    const mutators::PipelineResult result = run_hooks(
        scratch,
        "ridge-hold-narrow",
        "function mutate(ctx)\n"
        "  return {\n"
        "    units = function(units, ctx)\n"
        "      units:where { unitname = \"TOUGH\" }:scale { maxdamage = 20 }\n"
        "      units.TOUGH.selfdestructcountdown = 10\n"
        "      units.TOUGH.turnrate = -90000\n"
        "      units.TOUGH.sightdistance = -90000\n"
        "      units.FRAIL.maxdamage = -5\n"
        "    end,\n"
        "    weapons = function(weapons, ctx)\n"
        "      weapons.TOUGH_GUN.damage.TOUGH = 40000\n"
        "    end\n"
        "  }\n"
        "end\n"
    );
    const std::vector<std::string> lines = held_lines(result);
    OA_CHECK(lines.size() == 6);
    OA_CHECK(has_line(lines, "TOUGH: maxdamage 60000 held at 32767"));
    OA_CHECK(has_line(lines, "TOUGH: selfdestructcountdown 10 held at 7"));
    OA_CHECK(has_line(lines, "TOUGH: turnrate -90000 held at 0"));
    OA_CHECK(has_line(lines, "TOUGH: sightdistance -90000 held at -32768"));
    OA_CHECK(has_line(lines, "FRAIL: maxdamage -5 held at 1"));
    OA_CHECK(has_line(lines, "TOUGH_GUN: tough 40000 held at 32767"));

    const oa::UnitDef tough = loaded_unit(layer_text(result.layer, "TOUGH.FBI"));
    OA_CHECK(tough.max_damage == 32767u);
    OA_CHECK(tough.turn_rate == 0);
    OA_CHECK(tough.sight_distance == -32768);
    OA_CHECK(
        ((tough.abilities & OA_UNIT_DEF_ABILITY_SELF_DESTRUCT_MASK) >>
         OA_UNIT_DEF_ABILITY_SELF_DESTRUCT_SHIFT) == 7u
    );
    const oa::UnitDef frail = loaded_unit(layer_text(result.layer, "FRAIL.FBI"));
    OA_CHECK(frail.max_damage == 1u);
    const std::string weapon = layer_text(result.layer, "TOUGH.TDF");
    OA_CHECK(weapon.find("TOUGH=32767;") != std::string::npos);
    OA_CHECK(weapon.find("default=100;") != std::string::npos);
}

// A hook that reads a number an earlier mutator's rule stacked past the
// signed 16 bits the loader stores reads it as written, not wrapped: 1000 x
// 40 read back as -25536 and doubled would be held at 0, a commander that
// cannot turn. A mod's own value reads as written too.
void test_script_reads_the_stacked_number() {
    const fs::path scratch = oa::test::make_scratch_directory("V19-read");
    write_text(
        scratch / "units" / "STACK.FBI",
        "[UNITINFO]\n{\nUnitName=STACK;\nCommander=1;\nBMcode=1;\nTurnRate=1000;\n"
        "TransportCapacity=255;\n}\n"
    );
    write_text(scratch / "objects3d" / "STACK.3DO", " ");
    write_text(scratch / "scripts" / "STACK.COB", " ");
    write_package(
        scratch / "forty",
        "ridge-hold-forty",
        "function mutate(ctx)\n"
        "  return { rules = { where { unitname = is(\"STACK\") }:change { turnrate = scale(40) } } "
        "}\n"
        "end\n"
    );
    write_package(
        scratch / "double",
        "ridge-hold-double",
        "function mutate(ctx)\n"
        "  return {\n"
        "    units = function(units, ctx)\n"
        "      units.STACK.sortbias = units.STACK.transportcapacity\n"
        "      units.STACK.turnrate = units.STACK.turnrate * 2\n"
        "    end\n"
        "  }\n"
        "end\n"
    );
    mutators::PipelineInput input;
    input.mutators = {load_mutator(scratch / "forty"), load_mutator(scratch / "double")};
    input.mod.id = "ridge";
    input.mod.version = "1.0";
    input.mod.game = "ridge";
    input.engine_version = "0.8.5";
    input.layout = &defs::data_layout();
    input.seed = 1;
    oa::AssetStore store(std::vector<fs::path>{scratch, k_ridge});
    const mutators::PipelineResult result = mutators::run_pipeline(store, input);
    if (store.mutator_layer_mounted())
        store.unmount_mutator_layer();
    OA_CHECK(result.refusal.empty());
    OA_CHECK(result.mutators.size() == 2);
    for (const mutators::MutatorResult& one : result.mutators) {
        OA_CHECK(one.outcome == mutators::Outcome::applied);
        if (one.outcome != mutators::Outcome::applied)
            std::fprintf(stderr, "%s: %s\n", one.id.c_str(), one.message.c_str());
    }
    const std::vector<std::string> lines = held_lines(result);
    OA_CHECK(lines.size() == 1);
    OA_CHECK(has_line(lines, "STACK: turnrate 80000 held at 65535"));
    const std::string unit = layer_text(result.layer, "STACK.FBI");
    OA_CHECK(static_cast<uint16_t>(loaded_field(unit, true)) == 65535u);
    // The mod's own 255 reads as written, and is left in the file.
    OA_CHECK(unit.find("TransportCapacity=255;") != std::string::npos);
    OA_CHECK(unit.find("sortbias=255;") != std::string::npos);
}

/// Prints the held notes when there are not as many as expected.
void expect_lines(const std::vector<std::string>& lines, std::size_t count) {
    OA_CHECK(lines.size() == count);
    if (lines.size() != count) {
        for (const std::string& line : lines)
            std::fprintf(stderr, "held: %s\n", line.c_str());
    }
}

// The game reads several unit fields as the unsigned form of what the
// loader stores, or narrower. UTASP's resource facility floats at a
// waterline of 255, which the loader keeps as -1 in a signed byte and every
// unit's height reads back as 255. Two mutators stack each field on KEEP to
// a value the game reads as written, which is left, and on PAST and FLYER
// past what it reads, which is held at that end, once each. FLYER flies:
// an aircraft keeps its build distance as a signed 16-bit radius and its
// dogfight run takes 30 ticks of its speed, while braking squares a ground
// unit's speed and doubles its brake rate.
void test_unit_fields_are_held_at_the_range_the_game_reads() {
    const fs::path scratch = oa::test::make_scratch_directory("V19R-units");
    for (const std::string_view name : {"KEEP", "PAST", "FLYER"}) {
        std::string fbi = "[UNITINFO]\n{\nUnitName=";
        fbi.append(name);
        fbi += ";\nBMcode=1;\nWaterLine=17;\nTransportSize=40;\nTransportCapacity=51;\n"
               "MakesMetal=30;\nTurnRate=8000;\nWorkerTime=12000;\nHealTime=10000;\n"
               "KamikazeDistance=8000;\nManeuverLeashLength=13107;\nBuildDistance=10000;\n"
               "AttackRunLength=6000;\nBuildCostEnergy=429496716;\nCloakCost=1000;\n"
               "MaxVelocity=36;\nBrakeRate=3200;\nSightDistance=400;\n";
        if (name == "FLYER")
            fbi += "CanFly=1;\n";
        fbi += "}\n";
        std::string file = std::string{name} + ".FBI";
        write_text(scratch / "units" / file, fbi);
        write_text(scratch / "objects3d" / (std::string{name} + ".3DO"), " ");
        write_text(scratch / "scripts" / (std::string{name} + ".COB"), " ");
    }
    // KEEP ends inside the range the game reads; PAST and FLYER beyond it.
    write_package(
        scratch / "stack",
        "ridge-read-stack",
        "function mutate(ctx)\n"
        "  return {\n"
        "    units = function(units, ctx)\n"
        "      units:where { }:scale { waterline = 3, transportsize = 5, transportcapacity = 5,\n"
        "        makesmetal = 5, turnrate = 5, workertime = 5, healtime = 5,\n"
        "        kamikazedistance = 5, maneuverleashlength = 5, builddistance = 5,\n"
        "        attackrunlength = 5, buildcostenergy = 5, maxvelocity = 5, brakerate = 5 }\n"
        "    end\n"
        "  }\n"
        "end\n"
    );
    write_package(
        scratch / "again",
        "ridge-read-again",
        "function mutate(ctx)\n"
        "  return {\n"
        "    units = function(units, ctx)\n"
        "      units.KEEP.waterline = units.KEEP.waterline * 5\n"
        "      units.KEEP.makesmetal = 150\n"
        "      units.KEEP.attackrunlength = 31807\n"
        "      units.KEEP.buildcostenergy = 2147483583\n"
        "      units.KEEP.maxvelocity = 181\n"
        "      units.KEEP.brakerate = 16000\n"
        "      units.PAST.waterline = 300\n"
        "      units.PAST.transportsize = 256\n"
        "      units.PAST.transportcapacity = 300\n"
        "      units.PAST.makesmetal = -1\n"
        "      units.PAST.turnrate = 70000\n"
        "      units.PAST.workertime = -5\n"
        "      units.PAST.healtime = 70000\n"
        "      units.PAST.kamikazedistance = 70000\n"
        "      units.PAST.maneuverleashlength = -1\n"
        "      units.PAST.builddistance = 70000\n"
        "      units.PAST.attackrunlength = 31808\n"
        "      units.PAST.buildcostenergy = 2147483600\n"
        "      units.PAST.cloakcost = 3000000000\n"
        "      units.PAST.maxvelocity = 200\n"
        "      units.PAST.brakerate = 20000\n"
        "      units.FLYER.builddistance = 40000\n"
        "      units.FLYER.maxvelocity = 2000\n"
        "      units.FLYER.brakerate = 20000\n"
        "    end\n"
        "  }\n"
        "end\n"
    );
    mutators::PipelineInput input;
    input.mutators = {load_mutator(scratch / "stack"), load_mutator(scratch / "again")};
    input.mod.id = "ridge";
    input.mod.version = "1.0";
    input.mod.game = "ridge";
    input.engine_version = "0.8.5";
    input.layout = &defs::data_layout();
    input.seed = 1;
    oa::AssetStore store(std::vector<fs::path>{scratch, k_ridge});
    const mutators::PipelineResult result = mutators::run_pipeline(store, input);
    if (store.mutator_layer_mounted())
        store.unmount_mutator_layer();
    OA_CHECK(result.refusal.empty());
    OA_CHECK(result.mutators.size() == 2);
    for (const mutators::MutatorResult& one : result.mutators) {
        OA_CHECK(one.outcome == mutators::Outcome::applied);
        if (one.outcome != mutators::Outcome::applied)
            std::fprintf(stderr, "%s: %s\n", one.id.c_str(), one.message.c_str());
    }

    const std::vector<std::string> lines = held_lines(result);
    expect_lines(lines, 17);
    for (const std::string& line : lines)
        OA_CHECK(line.find("KEEP:") != 0);
    for (const std::string_view line :
         {"PAST: waterline 300 held at 255",
          "PAST: transportsize 256 held at 255",
          "PAST: transportcapacity 300 held at 255",
          "PAST: makesmetal -1 held at 0",
          "PAST: turnrate 70000 held at 65535",
          "PAST: workertime -5 held at 0",
          "PAST: healtime 70000 held at 65535",
          "PAST: kamikazedistance 70000 held at 65535",
          "PAST: maneuverleashlength -1 held at 0",
          "PAST: builddistance 70000 held at 65535",
          "PAST: attackrunlength 31808 held at 31807",
          "PAST: buildcostenergy 2147483600 held at 2147483583",
          "PAST: cloakcost 3000000000 held at 2147483583",
          "PAST: maxvelocity 200 held at 181.01934",
          "PAST: brakerate 20000 held at 16383.99999",
          "FLYER: builddistance 40000 held at 32767",
          "FLYER: maxvelocity 2000 held at 1092.26667"}) {
        OA_CHECK(has_line(lines, line));
        if (!has_line(lines, line))
            std::fprintf(stderr, "missing: %.*s\n", static_cast<int>(line.size()), line.data());
    }

    // KEEP loads every stacked value as written.
    const oa::UnitDef keep = loaded_unit(layer_text(result.layer, "KEEP.FBI"));
    OA_CHECK(static_cast<uint8_t>(keep.water_line) == 255u);
    OA_CHECK(static_cast<uint8_t>(keep.transport_size) == 200u);
    OA_CHECK(static_cast<uint8_t>(keep.transport_capacity) == 255u);
    OA_CHECK(static_cast<uint8_t>(keep.makes_metal) == 150u);
    OA_CHECK(static_cast<uint16_t>(keep.turn_rate) == 40000u);
    OA_CHECK(static_cast<uint16_t>(keep.worker_time) == 60000u);
    OA_CHECK(static_cast<uint16_t>(keep.heal_time) == 50000u);
    OA_CHECK(static_cast<uint16_t>(keep.kamikaze_distance) == 40000u);
    OA_CHECK(static_cast<uint16_t>(keep.maneuver_leash_length) == 65535u);
    OA_CHECK(static_cast<uint16_t>(keep.build_distance) == 50000u);
    OA_CHECK(keep.attack_run_length == 31807);
    OA_CHECK(keep.build_cost_energy == 2147483520.0F);
    OA_CHECK(keep.cloak_cost == 1000.0F);
    OA_CHECK(keep.max_velocity == 181 * 65536);
    OA_CHECK(keep.brake_rate == 16000 * 65536);
    // PAST and FLYER load each held value as that end.
    const oa::UnitDef past = loaded_unit(layer_text(result.layer, "PAST.FBI"));
    OA_CHECK(static_cast<uint8_t>(past.water_line) == 255u);
    OA_CHECK(static_cast<uint8_t>(past.transport_size) == 255u);
    OA_CHECK(static_cast<uint8_t>(past.transport_capacity) == 255u);
    OA_CHECK(past.makes_metal == 0);
    OA_CHECK(static_cast<uint16_t>(past.turn_rate) == 65535u);
    OA_CHECK(past.worker_time == 0);
    OA_CHECK(static_cast<uint16_t>(past.heal_time) == 65535u);
    OA_CHECK(static_cast<uint16_t>(past.kamikaze_distance) == 65535u);
    OA_CHECK(past.maneuver_leash_length == 0);
    OA_CHECK(static_cast<uint16_t>(past.build_distance) == 65535u);
    OA_CHECK(past.attack_run_length == 31807);
    OA_CHECK(past.build_cost_energy == 2147483520.0F);
    OA_CHECK(past.cloak_cost == 2147483520.0F);
    OA_CHECK(past.max_velocity == 11863283);
    OA_CHECK(past.brake_rate == 1073741823);
    const oa::UnitDef flyer = loaded_unit(layer_text(result.layer, "FLYER.FBI"));
    OA_CHECK(flyer.build_distance == 32767);
    OA_CHECK(flyer.max_velocity == 71582788);
    OA_CHECK(flyer.brake_rate == 20000 * 65536);
}

/// The weapon records the loader makes of one weapon file's text.
std::unique_ptr<defs::WeaponTable> loaded_weapons(const std::string& text) {
    auto table = std::make_unique<defs::WeaponTable>();
    defs::weapon_table_init(table.get());
    OA_CHECK(
        defs::load_weapon_text(
            table.get(), text.data(), static_cast<uint32_t>(text.size()), nullptr
        )
    );
    return table;
}

// The game reads a weapon's burst, spray angle, turn rate, area of effect
// and tick counts as the unsigned form of the signed 16 bits the loader
// stores, accuracy as a 16-bit spread that grows by 2048 on a damaged unit,
// range and coverage as signed 16.16 distances, and the speeds and the
// default damage unsigned. A hook stacks KEEP_GUN's fields to values the
// game reads as written, which are left, and PAST_GUN's past them, which
// are held, once each. An interceptor squares its area of effect, and a
// weapon that does not stockpile reloads up to 120% as long when damaged.
void test_weapon_fields_are_held_at_the_range_the_game_reads() {
    const fs::path scratch = oa::test::make_scratch_directory("V19R-weapons");
    write_text(
        scratch / "weapons" / "READ.TDF",
        "[KEEP_GUN]\n{\nID=3;\nburst=8000;\nsprayangle=12000;\nturnrate=60000;\naccuracy=400;\n"
        "areaofeffect=12000;\nrendertype=4;\nrange=6000;\ncoverage=1000;\nreloadtime=2;\n"
        "weapontimer=3;\nburstrate=0.4;\nduration=0.3;\nrandomdecay=0.5;\nsmokedelay=0.1;\n"
        "flighttime=4;\nholdtime=1;\nweaponvelocity=400;\n[DAMAGE]\n{\ndefault=100;\n}\n}\n"
        "[PAST_GUN]\n{\nID=4;\nburst=8000;\nsprayangle=12000;\nturnrate=60000;\naccuracy=400;\n"
        "areaofeffect=12000;\nrendertype=4;\nrange=6000;\ncoverage=1000;\nreloadtime=2;\n"
        "weapontimer=3;\nburstrate=0.4;\nduration=0.3;\nrandomdecay=0.5;\nsmokedelay=0.1;\n"
        "flighttime=4;\nholdtime=1;\nweaponvelocity=400;\nstartvelocity=100;\n"
        "weaponacceleration=50;\n[DAMAGE]\n{\ndefault=100;\n}\n}\n"
        "[CATCHER]\n{\nID=5;\ninterceptor=1;\nareaofeffect=12000;\n}\n"
        "[STORE]\n{\nID=6;\nstockpile=1;\nreloadtime=2;\n}\n"
    );
    const mutators::PipelineResult result = run_hooks(
        scratch,
        "ridge-read-weapons",
        "function mutate(ctx)\n"
        "  return {\n"
        "    weapons = function(weapons, ctx)\n"
        "      local keep = weapons.KEEP_GUN\n"
        "      keep.burst = 40000\n"
        "      keep.sprayangle = 60000\n"
        "      keep.turnrate = 1800000\n"
        "      keep.accuracy = 63487\n"
        "      keep.areaofeffect = 60000\n"
        "      keep.rendertype = 200\n"
        "      keep.range = 32767\n"
        "      keep.coverage = 32767\n"
        "      keep.reloadtime = 1820\n"
        "      keep.weapontimer = 2000\n"
        "      keep.burstrate = 2000\n"
        "      keep.duration = 2000\n"
        "      keep.randomdecay = 2000\n"
        "      keep.smokedelay = 2000\n"
        "      keep.flighttime = 2000\n"
        "      keep.holdtime = 2000\n"
        "      keep.damage.default = 32767\n"
        "      local past = weapons.PAST_GUN\n"
        "      past.burst = 70000\n"
        "      past.sprayangle = -1\n"
        "      past.turnrate = 3000000\n"
        "      past.accuracy = 64000\n"
        "      past.areaofeffect = 70000\n"
        "      past.rendertype = 300\n"
        "      past.range = 40000\n"
        "      past.coverage = -5\n"
        "      past.reloadtime = 2000\n"
        "      past.weapontimer = -1\n"
        "      past.burstrate = 2200\n"
        "      past.duration = -1\n"
        "      past.randomdecay = 2200\n"
        "      past.smokedelay = -1\n"
        "      past.flighttime = 2200\n"
        "      past.holdtime = -1\n"
        "      past.weaponvelocity = -10\n"
        "      past.startvelocity = -10\n"
        "      past.weaponacceleration = -1\n"
        "      past.damage.default = -5\n"
        "      weapons.CATCHER.areaofeffect = 50000\n"
        "      weapons.STORE.reloadtime = 2000\n"
        "    end\n"
        "  }\n"
        "end\n",
        "\"weapons\""
    );
    const std::vector<std::string> lines = held_lines(result);
    expect_lines(lines, 21);
    for (const std::string& line : lines) {
        OA_CHECK(line.find("KEEP_GUN:") != 0);
        OA_CHECK(line.find("STORE:") != 0);
    }
    for (const std::string_view line :
         {"PAST_GUN: burst 70000 held at 65535",        "PAST_GUN: sprayangle -1 held at 0",
          "PAST_GUN: turnrate 3000000 held at 1966050", "PAST_GUN: accuracy 64000 held at 63487",
          "PAST_GUN: areaofeffect 70000 held at 65535", "PAST_GUN: rendertype 300 held at 255",
          "PAST_GUN: range 40000 held at 32767",        "PAST_GUN: coverage -5 held at 0",
          "PAST_GUN: reloadtime 2000 held at 1820.44",  "PAST_GUN: weapontimer -1 held at 0",
          "PAST_GUN: burstrate 2200 held at 2184.5",    "PAST_GUN: duration -1 held at 0",
          "PAST_GUN: randomdecay 2200 held at 2184.5",  "PAST_GUN: smokedelay -1 held at 0",
          "PAST_GUN: flighttime 2200 held at 2184.5",   "PAST_GUN: holdtime -1 held at 0",
          "PAST_GUN: weaponvelocity -10 held at 0",     "PAST_GUN: startvelocity -10 held at 0",
          "PAST_GUN: weaponacceleration -1 held at 0",  "PAST_GUN: default -5 held at 0",
          "CATCHER: areaofeffect 50000 held at 46340"}) {
        OA_CHECK(has_line(lines, line));
        if (!has_line(lines, line))
            std::fprintf(stderr, "missing: %.*s\n", static_cast<int>(line.size()), line.data());
    }

    const std::unique_ptr<defs::WeaponTable> table =
        loaded_weapons(layer_text(result.layer, "READ.TDF"));
    const oa::WeaponDef& keep = table->defs[3];
    OA_CHECK(static_cast<uint16_t>(keep.burst) == 40000u);
    OA_CHECK(static_cast<uint16_t>(keep.spray_angle) == 60000u);
    OA_CHECK(static_cast<uint16_t>(keep.turn_rate) == 60000u);
    OA_CHECK(static_cast<uint16_t>(keep.accuracy) == 63487u);
    OA_CHECK(static_cast<uint16_t>(keep.area_of_effect) == 60000u);
    OA_CHECK(static_cast<uint8_t>(keep.render_type) == 200u);
    OA_CHECK(keep.range == 32767);
    OA_CHECK(keep.coverage == 32767);
    OA_CHECK(static_cast<uint16_t>(keep.reload_time) == 54600u);
    OA_CHECK(static_cast<uint16_t>(keep.weapon_timer) == 60000u);
    OA_CHECK(static_cast<uint16_t>(keep.burst_rate) == 60000u);
    OA_CHECK(static_cast<uint16_t>(keep.duration) == 60000u);
    OA_CHECK(static_cast<uint16_t>(keep.random_decay) == 60000u);
    OA_CHECK(static_cast<uint16_t>(keep.smoke_delay) == 60000u);
    OA_CHECK(static_cast<uint16_t>(keep.flight_time) == 60000u);
    OA_CHECK(static_cast<uint16_t>(keep.hold_time) == 60000u);
    OA_CHECK(keep.damage_default == 32767);
    const oa::WeaponDef& past = table->defs[4];
    OA_CHECK(static_cast<uint16_t>(past.burst) == 65535u);
    OA_CHECK(past.spray_angle == 0);
    OA_CHECK(static_cast<uint16_t>(past.turn_rate) == 65535u);
    OA_CHECK(static_cast<uint16_t>(past.accuracy) == 63487u);
    OA_CHECK(static_cast<uint16_t>(past.area_of_effect) == 65535u);
    OA_CHECK(static_cast<uint8_t>(past.render_type) == 255u);
    OA_CHECK(past.range == 32767);
    OA_CHECK(past.coverage == 0);
    OA_CHECK(static_cast<uint16_t>(past.reload_time) == 54613u);
    OA_CHECK(past.weapon_timer == 0);
    OA_CHECK(static_cast<uint16_t>(past.burst_rate) == 65535u);
    OA_CHECK(past.duration == 0);
    OA_CHECK(static_cast<uint16_t>(past.random_decay) == 65535u);
    OA_CHECK(past.smoke_delay == 0);
    OA_CHECK(static_cast<uint16_t>(past.flight_time) == 65535u);
    OA_CHECK(past.hold_time == 0);
    OA_CHECK(past.weapon_velocity == 0);
    OA_CHECK(past.start_velocity == 0);
    OA_CHECK(past.weapon_acceleration == 0);
    OA_CHECK(past.damage_default == 0);
    OA_CHECK(static_cast<uint16_t>(table->defs[5].area_of_effect) == 46340u);
    OA_CHECK(static_cast<uint16_t>(table->defs[6].reload_time) == 60000u);
    defs::weapon_table_free(table.get());
}

/// The record the feature loader makes of one section of a feature file's text.
oa::FeatureDef loaded_feature(std::string_view text, std::string_view name) {
    std::vector<oa::formats::tdf::OwnedDocument> documents(1);
    OA_CHECK(documents[0].parse(std::string{text}));
    oa::sim::map_runtime::FeatureDefTable table;
    const oa::sim::map_runtime::FeatureIndexResult loaded =
        oa::sim::map_runtime::load_feature_def(table, documents, name, nullptr);
    OA_CHECK(loaded.ok());
    OA_CHECK(table.defs.size() == 1);
    if (table.defs.empty())
        return oa::FeatureDef{};
    return table.defs[0];
}

// A features hook that scales a tree's metal past 16 bits holds it at 65535,
// which is what the feature loader stores. The dry run notes that once.
void test_scaled_feature_metal_is_held() {
    const fs::path scratch = oa::test::make_scratch_directory("V20-hold");
    write_text(
        scratch / "features" / "trees" / "RIDGE.TDF",
        "[TREE]\n{\nmetal=100;\nenergy=5;\ndamage=10;\nheight=4;\n}\n"
    );
    const mutators::PipelineResult result = run_hooks(
        scratch,
        "ridge-hold-tree",
        "function mutate(ctx)\n"
        "  return {\n"
        "    features = function(features, ctx)\n"
        "      features:where { }:scale { metal = 800 }\n"
        "    end\n"
        "  }\n"
        "end\n",
        "\"features\""
    );
    const std::vector<std::string> lines = held_lines(result);
    OA_CHECK(lines.size() == 1);
    OA_CHECK(has_line(lines, "TREE: metal 80000 held at 65535"));
    const std::string feature = layer_text(result.layer, "features/trees/RIDGE.TDF");
    OA_CHECK(feature.find("metal=65535;") != std::string::npos);
    OA_CHECK(feature.find("80000") == std::string::npos);
    OA_CHECK(loaded_feature(feature, "TREE").metal == 65535.0F);
}

// Two features hooks each scale every feature field by 3. The game reads
// most feature fields narrower than, or as the unsigned form of, what the
// loader stores: hit points and height are unsigned, a fire's spread
// countdown is one byte, a footprint cell's way back to its origin is a byte
// each way, and a metal spot paints its metal as a byte. Each value past the
// range the game reads is held once at its end; one inside it, if past the
// signed range the loader stores, is left as scaled. A value set past the
// low end is held at that end.
void test_stacked_feature_fields_are_held() {
    const fs::path scratch = oa::test::make_scratch_directory("V20-stack");
    write_text(
        scratch / "features" / "ridge" / "SPIRES.TDF",
        "[SPIRE]\n{\nheight=100;\ndamage=5000;\nspreadchance=20;\nsparktime=5;\n"
        "footprintx=40;\nfootprintz=2;\nmetal=1000;\nenergy=10000;\n}\n"
        "[PATCH]\n{\nindestructible=1;\nmetal=50;\n}\n"
        "[QUARRY]\n{\nindestructible=1;\nreclaimable=1;\nmetal=5000;\n}\n"
        "[LOW]\n{\nheight=10;\n}\n"
    );
    constexpr std::string_view scale_all =
        "      features:where { }:scale { height = 3, damage = 3, spreadchance = 3,\n"
        "        sparktime = 3, footprintx = 3, metal = 3, energy = 3 }\n";
    std::string triple = "function mutate(ctx)\n  return {\n"
                         "    features = function(features, ctx)\n";
    triple.append(scale_all);
    triple += "    end\n  }\nend\n";
    std::string again = "function mutate(ctx)\n  return {\n"
                        "    features = function(features, ctx)\n";
    again.append(scale_all);
    again += "      features.LOW.height = -5\n"
             "      features.LOW.damage = -1\n"
             "      features.LOW.sparktime = -1\n"
             "      features.LOW.footprintz = -40000\n"
             "      features.LOW.reproduce = 300\n"
             "    end\n  }\nend\n";
    write_package(scratch / "triple", "ridge-hold-triple", triple, "\"features\"");
    write_package(scratch / "again", "ridge-hold-again", again, "\"features\"");

    mutators::PipelineInput input;
    input.mutators = {load_mutator(scratch / "triple"), load_mutator(scratch / "again")};
    input.mod.id = "ridge";
    input.mod.version = "1.0";
    input.mod.game = "ridge";
    input.engine_version = "0.8.5";
    input.layout = &defs::data_layout();
    input.seed = 1;

    mutators::DryRunJob job;
    job.loose_roots = {scratch, k_ridge};
    job.mod_label = "Ridge 1.0";
    job.input = input;
    const std::atomic<bool> cancel{false};
    const mutators::VerdictRecord verdict = mutators::run_dry_run(job, cancel);
    OA_CHECK(verdict.verdict == mutators::Verdict::compatible);
    OA_CHECK(verdict.reason.empty());

    oa::AssetStore store(std::vector<fs::path>{scratch, k_ridge});
    const mutators::PipelineResult result = mutators::run_pipeline(store, input);
    if (store.mutator_layer_mounted())
        store.unmount_mutator_layer();
    OA_CHECK(result.refusal.empty());
    OA_CHECK(result.mutators.size() == 2);
    for (const mutators::MutatorResult& one : result.mutators) {
        OA_CHECK(one.outcome == mutators::Outcome::applied);
        if (one.outcome != mutators::Outcome::applied)
            std::fprintf(stderr, "%s: %s\n", one.id.c_str(), one.message.c_str());
    }

    const std::vector<std::string> lines = held_lines(result);
    OA_CHECK(lines.size() == 10);
    if (lines.size() != 10) {
        for (const std::string& line : lines)
            std::fprintf(stderr, "held: %s\n", line.c_str());
    }
    OA_CHECK(has_line(lines, "SPIRE: height 900 held at 255"));
    OA_CHECK(has_line(lines, "SPIRE: sparktime 45 held at 8.57"));
    OA_CHECK(has_line(lines, "SPIRE: footprintx 360 held at 256"));
    OA_CHECK(has_line(lines, "SPIRE: energy 90000 held at 65535"));
    OA_CHECK(has_line(lines, "PATCH: metal 450 held at 255"));
    OA_CHECK(has_line(lines, "LOW: height -5 held at 0"));
    OA_CHECK(has_line(lines, "LOW: damage -1 held at 0"));
    OA_CHECK(has_line(lines, "LOW: sparktime -1 held at 0"));
    OA_CHECK(has_line(lines, "LOW: footprintz -40000 held at -32768"));
    OA_CHECK(has_line(lines, "LOW: reproduce 300 held at 255"));

    const std::string text = layer_text(result.layer, "features/ridge/SPIRES.TDF");
    const oa::FeatureDef spire = loaded_feature(text, "SPIRE");
    OA_CHECK(static_cast<uint8_t>(spire.height) == 255u);
    OA_CHECK(static_cast<uint16_t>(spire.damage) == 45000u);
    OA_CHECK(static_cast<uint8_t>(spire.spread_chance) == 180u);
    OA_CHECK(spire.spark_time == 257);
    OA_CHECK(spire.footprint_x == 256);
    OA_CHECK(spire.footprint_z == 2);
    OA_CHECK(spire.metal == 9000.0F);
    OA_CHECK(spire.energy == 65535.0F);
    OA_CHECK(loaded_feature(text, "PATCH").metal == 255.0F);
    OA_CHECK(loaded_feature(text, "QUARRY").metal == 45000.0F);
    const oa::FeatureDef low = loaded_feature(text, "LOW");
    OA_CHECK(low.height == 0);
    OA_CHECK(low.damage == 0);
    OA_CHECK(low.spark_time == 0);
    OA_CHECK(low.footprint_z == -32768);
    OA_CHECK(static_cast<uint8_t>(low.reproduce) == 255u);
}

// A mod's or the game's own feature values past the range the game reads
// stay in the file, and load as they always have, when a hook changes
// another field of the same section. Stock spires stand up to 490 high and
// the gate wrecks carry 171172 metal; mods give wrecks 99999 damage and
// metal spots 300 or 500 metal.
void test_own_feature_values_are_left() {
    const fs::path scratch = oa::test::make_scratch_directory("V20-own");
    write_text(
        scratch / "features" / "rocks" / "RIDGE.TDF",
        "[ROCK]\n{\nmetal=70000;\nenergy=5;\ndamage=99999;\nheight=420;\nsparktime=20;\n"
        "footprintx=300;\nspreadchance=-5;\n}\n"
        "[SPOT]\n{\nindestructible=1;\nmetal=500;\ndamage=32009;\n}\n"
    );
    const mutators::PipelineResult result = run_hooks(
        scratch,
        "ridge-hold-rock",
        "function mutate(ctx)\n"
        "  return {\n"
        "    features = function(features, ctx)\n"
        "      features.ROCK.energy = 40\n"
        "      features.SPOT.damage = 32010\n"
        "    end\n"
        "  }\n"
        "end\n",
        "\"features\""
    );
    const std::vector<std::string> lines = held_lines(result);
    OA_CHECK(lines.empty());
    const std::string feature = layer_text(result.layer, "features/rocks/RIDGE.TDF");
    OA_CHECK(feature.find("energy=40;") != std::string::npos);
    OA_CHECK(feature.find("damage=32010;") != std::string::npos);
    for (const std::string_view own :
         {"metal=70000;",
          "damage=99999;",
          "height=420;",
          "sparktime=20;",
          "footprintx=300;",
          "spreadchance=-5;",
          "metal=500;"})
        OA_CHECK(feature.find(own) != std::string::npos);
    const oa::FeatureDef rock = loaded_feature(feature, "ROCK");
    OA_CHECK(rock.metal == 4464.0F);
    OA_CHECK(static_cast<uint16_t>(rock.damage) == 34463u);
    OA_CHECK(static_cast<uint8_t>(rock.height) == 164u);
    OA_CHECK(rock.spark_time == 600);
    OA_CHECK(loaded_feature(feature, "SPOT").metal == 500.0F);
}

} // namespace

int main() {
    test_stacked_turn_and_worker_are_held();
    test_own_values_are_left();
    test_narrower_than_their_record();
    test_script_reads_the_stacked_number();
    test_unit_fields_are_held_at_the_range_the_game_reads();
    test_weapon_fields_are_held_at_the_range_the_game_reads();
    test_scaled_feature_metal_is_held();
    test_stacked_feature_fields_are_held();
    test_own_feature_values_are_left();
    return oa::test::check_exit_status();
}
