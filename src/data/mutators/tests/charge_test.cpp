// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Collections and rule forms charge their C++ walks against the phase's
// instruction allowance. A walk the script asks for stops when that
// allowance is spent. A small plan stays inside a fixed bound and spends
// the same count on every run. The units are the made-up mod Ridge.

#include "collections_sides.hpp"

#include "oa/data/defs/layout.hpp"
#include "oa/data/mutators/collections.hpp"
#include "oa/data/mutators/rules_lua.hpp"
#include "oa/data/script_host/script_host.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/test/check.hpp"
#include "oa/test/scratch_directory.hpp"

#include "lua.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace mutators = oa::data::mutators;
namespace host = oa::data::script_host;

/// One hundredth of the default allowance. A handful of rule blocks, or one
/// collection where and one pairs, stays under this. The review's 4,000-key
/// where loop does not.
constexpr uint64_t k_plan_bound = 500'000;

/// Small enough that forty walks of a weapon collection pass it only when
/// each walk is charged like one `pairs`.
constexpr uint64_t k_walk_allowance = 100'000;

/// Long enough that a charged walk stops on the instruction allowance on a
/// busy machine. The count itself does not read the clock.
constexpr std::chrono::milliseconds k_long_limit{60'000};

constexpr std::string_view k_review_where = R"(
local t = {}
for i = 1, 4000 do
  t["f" .. i] = 1
end
for n = 1, 2000 do
  where(t)
end
return true
)";

constexpr std::string_view k_trimmed_where = R"(
where { a = 1, [" a"] = 2 }
)";

constexpr std::string_view k_trimmed_change = R"(
all():change { a = 1, [" a"] = 2 }
)";

constexpr std::string_view k_small_plan = R"(
return {
  where { range = range(1, 2) }:change { damage = set(2) },
  where { name = "ridge" }:change { note = set "kept" },
  all():change { marker = set(1) },
  named("ridgecom"):change { health = scale(2) },
}
)";

constexpr std::string_view k_weapon_walk = R"(
local n = 0
for i = 1, 40 do
  for _ in pairs(weapons) do
    n = n + 1
  end
end
return n
)";

constexpr std::string_view k_small_weapons = R"(
local picked = weapons:where { range = range(1, 2) }
local n = 0
for _ in pairs(weapons) do
  n = n + 1
end
return n == 2 and #picked == 2
)";

/// The review's uncharged 200,000 reads fit in this allowance. Charging each
/// side or table access stops the loop.
constexpr uint64_t k_field_allowance = 3'000'000;

constexpr std::string_view k_side_reads = R"(
for i = 1, 200000 do
  if sides[1].commander == nil then
    error("commander")
  end
end
return true
)";

constexpr std::string_view k_table_reads = R"(
for i = 1, 200000 do
  if tables.los.TABLE2.line1 == nil then
    error("line")
  end
end
return true
)";

constexpr std::string_view k_side_pairs = R"(
local n = 0
for i = 1, 40 do
  for _ in pairs(sides) do
    n = n + 1
  end
end
return n
)";

constexpr std::string_view k_charge_sides = R"([SIDE0]
{
name=Ridge;
commander=RGCOM;
}
[SIDE1]
{
name=Other;
commander=RGLAB;
}
)";

constexpr std::string_view k_charge_los = R"([TABLE2]
{
line1=kept;
}
[RADAR]
{
range=1;
}
)";

struct Package {
    std::map<std::string, std::vector<uint8_t>> files;
};

struct FlagRead {
    bool saw{false};
    bool value{false};
};

struct PlanRead {
    bool saw{false};
    mutators::PlanRules plan;
};

bool read_package(void* context, std::string_view path, std::vector<uint8_t>& bytes) {
    auto* package = static_cast<Package*>(context);
    const auto found = package->files.find(std::string(path));
    if (found == package->files.end())
        return false;
    bytes = found->second;
    return true;
}

void put(Package& package, std::string_view source) {
    package.files["mutator.lua"] = std::vector<uint8_t>(source.begin(), source.end());
}

void install_rules(void*, lua_State* state) {
    mutators::install_rule_forms(state);
}

struct WeaponsBind {
    mutators::Collections* collections{nullptr};
};

void install_weapons(void* context, lua_State* state) {
    auto* bind = static_cast<WeaponsBind*>(context);
    mutators::install_selection_forms(state);
    bind->collections->push_weapons(state);
    lua_setglobal(state, "weapons");
}

void read_flag(void* context, lua_State* state, int results) {
    auto* flag = static_cast<FlagRead*>(context);
    flag->saw = true;
    flag->value =
        results == 1 && lua_type(state, -1) == LUA_TBOOLEAN && lua_toboolean(state, -1) != 0;
}

std::optional<std::vector<uint8_t>> read_no_file(std::string_view, std::string*) {
    return std::nullopt;
}

void read_plan(void* context, lua_State* state, int results) {
    auto* box = static_cast<PlanRead*>(context);
    box->saw = true;
    if (results != 1 || !lua_istable(state, -1)) {
        box->plan.error = "the chunk did not return one value";
        return;
    }
    box->plan = mutators::read_plan_rules(state, -1, read_no_file, "mutator.lua");
}

std::size_t block_count(const mutators::PlanRules& plan) {
    std::size_t count = 0;
    for (const auto& source : plan.sources)
        count += source.rules.blocks.size();
    return count;
}

std::string weapons_text(int sections) {
    std::string text;
    for (int index = 0; index < sections; ++index) {
        text += "[RIDGEW";
        if (index < 10)
            text += "0";
        text += std::to_string(index);
        text += "]\n{\nrange=1;\n}\n";
    }
    return text;
}

struct RulesRun {
    bool opened{false};
    bool ok{false};
    uint64_t used{0};
    host::ScriptError error;
    FlagRead flag;
    PlanRead plan;
};

RulesRun run_rules(
    std::string_view source,
    uint64_t allowance,
    std::chrono::milliseconds time_limit,
    bool read_blocks
) {
    RulesRun run;
    Package package;
    put(package, source);
    host::OpenOptions options;
    options.package_id = "ridge";
    options.random_seed = 0;
    options.time_limit = time_limit;
    options.profile = host::mutator_profile();
    options.profile.budget.instructions = allowance;
    options.profile.install = install_rules;
    options.sources.context = &package;
    options.sources.read = read_package;
    host::ScriptError open_error;
    auto sandbox = host::Sandbox::open(options, open_error);
    if (!sandbox.has_value()) {
        std::fprintf(stderr, "open: %s\n", open_error.message.c_str());
        run.error = open_error;
        return run;
    }
    run.opened = true;
    host::CallHooks hooks;
    if (read_blocks) {
        hooks.context = &run.plan;
        hooks.read = read_plan;
    } else {
        hooks.context = &run.flag;
        hooks.read = read_flag;
    }
    run.ok = sandbox->run_file("mutator.lua", "apply", &hooks, run.error);
    run.used = sandbox->instructions_used();
    return run;
}

void expect_budget(const RulesRun& run, uint64_t allowance, const char* label) {
    const std::string message =
        "the script ran past its budget of " + std::to_string(allowance) + " instructions";
    if (!run.opened || run.ok || run.error.kind != host::ErrorKind::instruction_budget ||
        run.error.message != message) {
        std::fprintf(
            stderr,
            "%s: opened %d ok %d kind %d message %s\n",
            label,
            run.opened ? 1 : 0,
            run.ok ? 1 : 0,
            static_cast<int>(run.error.kind),
            run.error.message.c_str()
        );
    }
    OA_CHECK(run.opened);
    OA_CHECK(!run.ok);
    OA_CHECK(run.error.kind == host::ErrorKind::instruction_budget);
    OA_CHECK(run.error.message == message);
}

void where_on_a_large_table_stops() {
    const RulesRun run = run_rules(k_review_where, k_plan_bound, k_long_limit, false);
    expect_budget(run, k_plan_bound, "where loop");
}

void trimmed_fields_are_refused(std::string_view source, const char* label) {
    const RulesRun run =
        run_rules(source, host::default_instruction_budget, host::default_time_limit, false);
    const bool names_both = run.error.message.find("field \"a\"") != std::string::npos &&
                            run.error.message.find("field \" a\"") != std::string::npos &&
                            run.error.message.find("name the same field") != std::string::npos;
    if (!run.opened || run.ok || run.error.kind != host::ErrorKind::runtime || !names_both) {
        std::fprintf(
            stderr,
            "%s: opened %d ok %d kind %d message %s\n",
            label,
            run.opened ? 1 : 0,
            run.ok ? 1 : 0,
            static_cast<int>(run.error.kind),
            run.error.message.c_str()
        );
    }
    OA_CHECK(run.opened);
    OA_CHECK(!run.ok);
    OA_CHECK(run.error.kind == host::ErrorKind::runtime);
    OA_CHECK(names_both);
}

void legitimate_rules_stay_small_and_match() {
    const RulesRun first = run_rules(k_small_plan, k_plan_bound, host::default_time_limit, true);
    const RulesRun second = run_rules(k_small_plan, k_plan_bound, host::default_time_limit, true);
    if (!first.ok || !first.plan.saw || !first.plan.plan.ok() || block_count(first.plan.plan) != 4)
        std::fprintf(stderr, "plan: %s\n", first.plan.plan.error.c_str());
    if (!second.ok || first.used != second.used || first.used > k_plan_bound)
        std::fprintf(
            stderr,
            "plan charge: %llu %llu\n",
            static_cast<unsigned long long>(first.used),
            static_cast<unsigned long long>(second.used)
        );
    OA_CHECK(first.opened && second.opened);
    OA_CHECK(first.ok && second.ok);
    OA_CHECK(first.plan.saw && second.plan.saw);
    OA_CHECK(first.plan.plan.ok() && second.plan.plan.ok());
    OA_CHECK(block_count(first.plan.plan) == 4);
    OA_CHECK(block_count(second.plan.plan) == 4);
    OA_CHECK(first.used > 0);
    OA_CHECK(first.used <= k_plan_bound);
    OA_CHECK(first.used == second.used);
}

struct WeaponsWorld {
    oa::data::mutation_rules::DocumentSet documents;
    mutators::Collections collections;
    std::optional<host::Sandbox> sandbox;

    WeaponsWorld() : collections(documents) {}
};

struct WeaponsRun {
    bool opened{false};
    bool ok{false};
    uint64_t used{0};
    host::ScriptError error;
    FlagRead flag;
};

WeaponsRun run_weapons(
    std::string_view documents,
    std::string_view source,
    uint64_t allowance,
    std::chrono::milliseconds time_limit,
    bool read_result
) {
    WeaponsRun run;
    WeaponsWorld world;
    std::string failure;
    if (!world.documents.add("Weapons/ridge.tdf", documents, &failure)) {
        std::fprintf(stderr, "weapons: %s\n", failure.c_str());
        return run;
    }
    Package package;
    put(package, source);
    WeaponsBind bind;
    bind.collections = &world.collections;
    host::OpenOptions options;
    options.package_id = "ridge";
    options.random_seed = 0;
    options.time_limit = time_limit;
    options.profile = host::mutator_profile();
    options.profile.budget.instructions = allowance;
    options.profile.context = &bind;
    options.profile.install = install_weapons;
    options.sources.context = &package;
    options.sources.read = read_package;
    host::ScriptError open_error;
    world.sandbox = host::Sandbox::open(options, open_error);
    if (!world.sandbox.has_value()) {
        std::fprintf(stderr, "open: %s\n", open_error.message.c_str());
        run.error = open_error;
        return run;
    }
    run.opened = true;
    host::CallHooks hooks;
    const host::CallHooks* results = nullptr;
    if (read_result) {
        hooks.context = &run.flag;
        hooks.read = read_flag;
        results = &hooks;
    }
    run.ok = world.sandbox->run_file("mutator.lua", "apply", results, run.error);
    run.used = world.sandbox->instructions_used();
    return run;
}

void collection_walk_stops() {
    const std::string text = weapons_text(30);
    const WeaponsRun run = run_weapons(text, k_weapon_walk, k_walk_allowance, k_long_limit, false);
    const std::string message =
        "the script ran past its budget of " + std::to_string(k_walk_allowance) + " instructions";
    if (!run.opened || run.ok || run.error.kind != host::ErrorKind::instruction_budget ||
        run.error.message != message) {
        std::fprintf(
            stderr,
            "collection walk: opened %d ok %d kind %d message %s\n",
            run.opened ? 1 : 0,
            run.ok ? 1 : 0,
            static_cast<int>(run.error.kind),
            run.error.message.c_str()
        );
    }
    OA_CHECK(run.opened);
    OA_CHECK(!run.ok);
    OA_CHECK(run.error.kind == host::ErrorKind::instruction_budget);
    OA_CHECK(run.error.message == message);
}

void legitimate_collection_stays_small_and_matches() {
    constexpr std::string_view text = R"([RIDGE_A]
{
range=1;
}
[RIDGE_B]
{
range=2;
}
)";
    const WeaponsRun first =
        run_weapons(text, k_small_weapons, k_plan_bound, host::default_time_limit, true);
    const WeaponsRun second =
        run_weapons(text, k_small_weapons, k_plan_bound, host::default_time_limit, true);
    if (!first.ok || !first.flag.value || !second.flag.value || first.used != second.used ||
        first.used > k_plan_bound) {
        std::fprintf(
            stderr,
            "small weapons: ok %d %d flag %d %d used %llu %llu message %s\n",
            first.ok ? 1 : 0,
            second.ok ? 1 : 0,
            first.flag.value ? 1 : 0,
            second.flag.value ? 1 : 0,
            static_cast<unsigned long long>(first.used),
            static_cast<unsigned long long>(second.used),
            first.error.message.c_str()
        );
    }
    OA_CHECK(first.opened && second.opened);
    OA_CHECK(first.ok && second.ok);
    OA_CHECK(first.flag.saw && second.flag.saw);
    OA_CHECK(first.flag.value && second.flag.value);
    OA_CHECK(first.used > 0);
    OA_CHECK(first.used <= k_plan_bound);
    OA_CHECK(first.used == second.used);
}

struct SidesBind {
    mutators::Collections* collections{nullptr};
    const oa::AssetStore* store{nullptr};
    const oa::data::defs::DataLayout* layout{nullptr};
    const std::string* variant{nullptr};
};

void install_sides(void* context, lua_State* state) {
    auto* bind = static_cast<SidesBind*>(context);
    bind->collections->begin_mutator("ridge", {});
    lua_newtable(state);
    mutators::set_context_sides(
        state, -1, *bind->collections, *bind->store, *bind->layout, *bind->variant
    );
    lua_getfield(state, -1, "sides");
    lua_setglobal(state, "sides");
    lua_getfield(state, -1, "tables");
    lua_setglobal(state, "tables");
    lua_pop(state, 1);
}

struct SidesWorld {
    std::filesystem::path scratch;
    oa::AssetStore store;
    std::string variant;
    oa::data::mutation_rules::DocumentSet documents;
    mutators::Collections collections;
    std::optional<host::Sandbox> sandbox;

    SidesWorld()
        : scratch(oa::test::make_scratch_directory("D04-charge")), store(scratch),
          collections(documents, &oa::data::defs::data_layout()) {}
};

struct SidesRun {
    bool opened{false};
    bool ok{false};
    host::ScriptError error;
};

SidesRun run_sides(std::string_view source, uint64_t allowance) {
    SidesRun run;
    SidesWorld world;
    std::string failure;
    if (!world.documents.add("gamedata/sidedata.tdf", k_charge_sides, &failure) ||
        !world.documents.add("gamedata/los.tdf", k_charge_los, &failure)) {
        std::fprintf(stderr, "sides documents: %s\n", failure.c_str());
        std::error_code remove_error;
        std::filesystem::remove_all(world.scratch, remove_error);
        return run;
    }
    Package package;
    put(package, source);
    SidesBind bind;
    bind.collections = &world.collections;
    bind.store = &world.store;
    bind.layout = &oa::data::defs::data_layout();
    bind.variant = &world.variant;
    host::OpenOptions options;
    options.package_id = "ridge";
    options.random_seed = 0;
    options.time_limit = k_long_limit;
    options.profile = host::mutator_profile();
    options.profile.budget.instructions = allowance;
    options.profile.context = &bind;
    options.profile.install = install_sides;
    options.sources.context = &package;
    options.sources.read = read_package;
    host::ScriptError open_error;
    world.sandbox = host::Sandbox::open(options, open_error);
    if (!world.sandbox.has_value()) {
        std::fprintf(stderr, "open: %s\n", open_error.message.c_str());
        run.error = open_error;
        std::error_code remove_error;
        std::filesystem::remove_all(world.scratch, remove_error);
        return run;
    }
    run.opened = true;
    run.ok = world.sandbox->run_file("mutator.lua", "apply", nullptr, run.error);
    std::error_code remove_error;
    std::filesystem::remove_all(world.scratch, remove_error);
    return run;
}

void expect_sides_budget(const SidesRun& run, uint64_t allowance, const char* label) {
    const std::string message =
        "the script ran past its budget of " + std::to_string(allowance) + " instructions";
    if (!run.opened || run.ok || run.error.kind != host::ErrorKind::instruction_budget ||
        run.error.message != message) {
        std::fprintf(
            stderr,
            "%s: opened %d ok %d kind %d message %s\n",
            label,
            run.opened ? 1 : 0,
            run.ok ? 1 : 0,
            static_cast<int>(run.error.kind),
            run.error.message.c_str()
        );
    }
    OA_CHECK(run.opened);
    OA_CHECK(!run.ok);
    OA_CHECK(run.error.kind == host::ErrorKind::instruction_budget);
    OA_CHECK(run.error.message == message);
}

struct UnitsBind {
    mutators::Collections* collections{nullptr};
};

void install_units(void* context, lua_State* state) {
    auto* bind = static_cast<UnitsBind*>(context);
    mutators::install_selection_forms(state);
    bind->collections->push_units(state);
    lua_setglobal(state, "units");
    mutators::WriteGrant grant;
    grant.units = true;
    bind->collections->begin_mutator("ridge-clone", grant);
    bind->collections->set_clash_mode(oa::formats::oamut::ClashMode::add);
    bind->collections->set_clash_active(true);
}

struct UnitsWorld {
    oa::data::mutation_rules::DocumentSet documents;
    mutators::Collections collections;
    std::optional<host::Sandbox> sandbox;

    UnitsWorld() : collections(documents) {}
};

struct UnitsRun {
    bool opened{false};
    bool ok{false};
    uint64_t used{0};
    host::ScriptError error;
};

UnitsRun run_units(std::string_view documents, std::string_view source, uint64_t allowance) {
    UnitsRun run;
    UnitsWorld world;
    std::string failure;
    if (!world.documents.add("units/RGBOT.FBI", documents, &failure)) {
        std::fprintf(stderr, "units: %s\n", failure.c_str());
        return run;
    }
    Package package;
    put(package, source);
    UnitsBind bind;
    bind.collections = &world.collections;
    host::OpenOptions options;
    options.package_id = "ridge";
    options.random_seed = 0;
    options.time_limit = k_long_limit;
    options.profile = host::mutator_profile();
    options.profile.budget.instructions = allowance;
    options.profile.context = &bind;
    options.profile.install = install_units;
    options.sources.context = &package;
    options.sources.read = read_package;
    host::ScriptError open_error;
    world.sandbox = host::Sandbox::open(options, open_error);
    if (!world.sandbox.has_value()) {
        std::fprintf(stderr, "open: %s\n", open_error.message.c_str());
        run.error = open_error;
        return run;
    }
    run.opened = true;
    run.ok = world.sandbox->run_file("mutator.lua", "apply", nullptr, run.error);
    run.used = world.sandbox->instructions_used();
    return run;
}

constexpr std::string_view k_clone_names = R"(
for i = 1, 40 do
  units:clone("RGBOT", "RGBOT", {})
end
return true
)";

/// Small enough that forty Add-mode clones onto one name pass it only when
/// each private-name lookup is left uncharged.
constexpr uint64_t k_clone_allowance = 8'000;

void add_mode_clone_lookups_are_charged() {
    const std::string unit = "[UNITINFO]\n{\nUnitName=RGBOT;\n}\n";
    const UnitsRun wide = run_units(unit, k_clone_names, k_plan_bound);
    if (!wide.opened || !wide.ok || wide.used <= k_clone_allowance) {
        std::fprintf(
            stderr,
            "clone charge: opened %d ok %d used %llu kind %d message %s\n",
            wide.opened ? 1 : 0,
            wide.ok ? 1 : 0,
            static_cast<unsigned long long>(wide.used),
            static_cast<int>(wide.error.kind),
            wide.error.message.c_str()
        );
    }
    OA_CHECK(wide.opened);
    OA_CHECK(wide.ok);
    OA_CHECK(wide.used > k_clone_allowance);
    OA_CHECK(wide.used <= k_plan_bound);

    const UnitsRun tight = run_units(unit, k_clone_names, k_clone_allowance);
    const std::string message =
        "the script ran past its budget of " + std::to_string(k_clone_allowance) + " instructions";
    if (!tight.opened || tight.ok || tight.error.kind != host::ErrorKind::instruction_budget ||
        tight.error.message != message) {
        std::fprintf(
            stderr,
            "clone budget: opened %d ok %d kind %d message %s\n",
            tight.opened ? 1 : 0,
            tight.ok ? 1 : 0,
            static_cast<int>(tight.error.kind),
            tight.error.message.c_str()
        );
    }
    OA_CHECK(tight.opened);
    OA_CHECK(!tight.ok);
    OA_CHECK(tight.error.kind == host::ErrorKind::instruction_budget);
    OA_CHECK(tight.error.message == message);
}

void side_and_table_walks_stop() {
    expect_sides_budget(
        run_sides(k_side_reads, k_field_allowance), k_field_allowance, "side reads"
    );
    expect_sides_budget(
        run_sides(k_table_reads, k_field_allowance), k_field_allowance, "table reads"
    );
    expect_sides_budget(run_sides(k_side_pairs, k_walk_allowance), k_walk_allowance, "side pairs");
}

} // namespace

int main() {
    where_on_a_large_table_stops();
    collection_walk_stops();
    legitimate_rules_stay_small_and_match();
    legitimate_collection_stays_small_and_matches();
    trimmed_fields_are_refused(k_trimmed_where, "trimmed where");
    trimmed_fields_are_refused(k_trimmed_change, "trimmed change");
    side_and_table_walks_stop();
    add_mode_clone_lookups_are_charged();
    return oa::test::check_exit_status();
}
