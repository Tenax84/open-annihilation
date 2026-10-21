// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Collections and rule forms charge their C++ walks against the phase's
// instruction allowance. A walk the script asks for stops when that
// allowance is spent. A small plan stays inside a fixed bound and spends
// the same count on every run. The units are the made-up mod Ridge.

#include "oa/data/mutators/collections.hpp"
#include "oa/data/mutators/rules_lua.hpp"
#include "oa/data/script_host/script_host.hpp"
#include "oa/test/check.hpp"

#include "lua.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
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

} // namespace

int main() {
    where_on_a_large_table_stops();
    collection_walk_stops();
    legitimate_rules_stay_small_and_match();
    legitimate_collection_stays_small_and_matches();
    trimmed_fields_are_refused(k_trimmed_where, "trimmed where");
    trimmed_fields_are_refused(k_trimmed_change, "trimmed change");
    return oa::test::check_exit_status();
}
