// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A real-time limit stops a phase the instruction count cannot see. The
// allowance here is huge, so only the clock can end the phase. The clock in
// this file measures the test; the sandbox has its own.

#include "lua.hpp"
#include "oa/data/script_host/script_host.hpp"
#include "oa/test/check.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace host = oa::data::script_host;

namespace {

struct Package {
    std::map<std::string, std::vector<uint8_t>> files;
};

struct Number {
    lua_Integer value = 0;
    bool saw = false;
};

bool read_package(void* context, std::string_view path, std::vector<uint8_t>& bytes) {
    auto* package = static_cast<Package*>(context);
    const auto found = package->files.find(std::string(path));
    if (found == package->files.end())
        return false;
    bytes = found->second;
    return true;
}

void put(Package& package, std::string path, std::string_view source) {
    package.files[std::move(path)] = std::vector<uint8_t>(source.begin(), source.end());
}

void read_number(void* context, lua_State* state, int results) {
    auto* out = static_cast<Number*>(context);
    out->saw = false;
    if (results < 1 || !lua_isinteger(state, -1))
        return;
    out->value = lua_tointeger(state, -1);
    out->saw = true;
}

host::Profile clock_profile() {
    host::Budget budget;
    budget.instructions = std::numeric_limits<uint64_t>::max();
    budget.memory_bytes = host::default_memory_budget;
    return host::Profile{"test", budget, nullptr, nullptr};
}

std::optional<host::Sandbox>
open_limited(Package& package, host::ScriptError& error, std::chrono::milliseconds limit) {
    host::OpenOptions options;
    options.package_id = "sample";
    options.profile = clock_profile();
    options.time_limit = limit;
    options.sources.context = &package;
    options.sources.read = read_package;
    auto sandbox = host::Sandbox::open(options, error);
    OA_CHECK(sandbox.has_value());
    return sandbox;
}

std::string expected_message(const char* phase, std::chrono::milliseconds limit) {
    const auto length = limit.count();
    long long number = length;
    const char* unit = "milliseconds";
    if (length > 0 && length % 1000 == 0) {
        number = length / 1000;
        unit = number == 1 ? "second" : "seconds";
    } else if (length == 1) {
        unit = "millisecond";
    }
    return std::string(phase) + " ran for more than " + std::to_string(number) + " " + unit +
           "; its work is too large or never ends";
}

int64_t expect_stopped(
    const char* name, const char* source, std::chrono::milliseconds limit, const char* phase
) {
    Package package;
    host::ScriptError error;
    auto sandbox = open_limited(package, error, limit);
    if (!sandbox)
        return 0;
    put(package, "mutator.lua", source);
    const auto started = std::chrono::steady_clock::now();
    const bool ran = sandbox->run_file("mutator.lua", phase, nullptr, error);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started
    );
    const auto overshoot = elapsed - limit;
    std::printf(
        "time-limit %s elapsed %lld ms overshoot %lld ms\n",
        name,
        static_cast<long long>(elapsed.count()),
        static_cast<long long>(overshoot.count())
    );
    OA_CHECK(!ran);
    OA_CHECK(error.kind == host::ErrorKind::time_limit);
    OA_CHECK(error.phase == phase);
    OA_CHECK(error.chunk == "mutator.lua");
    OA_CHECK(error.line > 0);
    OA_CHECK(error.message == expected_message(phase, limit));
    OA_CHECK(elapsed >= limit);
    OA_CHECK(elapsed < limit + std::chrono::seconds(1));
    OA_CHECK(sandbox->instructions_used() < clock_profile().budget.instructions);

    host::ScriptError again;
    OA_CHECK(!sandbox->run_file("mutator.lua", "later", nullptr, again));
    OA_CHECK(again.kind == host::ErrorKind::time_limit);
    OA_CHECK(again.phase == phase);
    return overshoot.count();
}

const char* tight_loop = "while true do end\n";

const char* zero_filled = "local a = string.rep(\"\\0\", 65536)\n"
                          "local b = string.rep(\"\\0\", 65536)\n"
                          "while true do\n"
                          "  if a < b then\n"
                          "    break\n"
                          "  end\n"
                          "end\n";

const char* wide_call = "local t = {}\n"
                        "for i = 1, 300000 do\n"
                        "  t[i] = 0\n"
                        "end\n"
                        "local function f(...)\n"
                        "  while true do\n"
                        "    math.max(...)\n"
                        "  end\n"
                        "end\n"
                        "f(table.unpack(t))\n";

const char* caught_loop = "while true do\n"
                          "  pcall(function()\n"
                          "    while true do end\n"
                          "  end)\n"
                          "end\n";

const char* handled_loop = "while true do\n"
                           "  xpcall(function()\n"
                           "    while true do end\n"
                           "  end, function()\n"
                           "    while true do end\n"
                           "  end)\n"
                           "end\n";

const char* plain_work = "local total = 0\n"
                         "for i = 1, 5000 do\n"
                         "  total = total + i\n"
                         "end\n"
                         "return total\n";

bool run_plain(host::Sandbox& sandbox, Number& number, uint64_t* used) {
    host::ScriptError error;
    host::CallHooks hooks;
    hooks.context = &number;
    hooks.read = read_number;
    const auto started = std::chrono::steady_clock::now();
    const bool ran = sandbox.run_file("mutator.lua", "plan", &hooks, error);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started
    );
    OA_CHECK(ran);
    OA_CHECK(error.kind == host::ErrorKind::none);
    OA_CHECK(number.saw);
    OA_CHECK(number.value == 12502500);
    OA_CHECK(elapsed < std::chrono::seconds(2));
    if (used != nullptr)
        *used = sandbox.instructions_used();
    return ran && number.saw;
}

void a_short_script_is_unchanged() {
    const auto limit = std::chrono::milliseconds(300);
    Package short_package;
    host::ScriptError short_error;
    put(short_package, "mutator.lua", plain_work);
    auto shortened = open_limited(short_package, short_error, limit);
    Package full_package;
    host::ScriptError full_error;
    put(full_package, "mutator.lua", plain_work);
    auto full = open_limited(full_package, full_error, host::default_time_limit);
    if (!shortened || !full)
        return;
    Number short_number;
    Number full_number;
    uint64_t short_used = 0;
    uint64_t full_used = 0;
    if (!run_plain(*shortened, short_number, &short_used))
        return;
    if (!run_plain(*full, full_number, &full_used))
        return;
    OA_CHECK(short_number.value == full_number.value);
    OA_CHECK(short_used == full_used);
    OA_CHECK(short_used > 0);
}

std::string busy_compares(int repeats) {
    return "local a = string.rep(\"\\0\", 65536)\n"
           "local b = string.rep(\"\\0\", 65536)\n"
           "local n = 0\n"
           "for i = 1, " +
           std::to_string(repeats) +
           " do\n"
           "  if not (a < b) then\n"
           "    n = n + 1\n"
           "  end\n"
           "end\n"
           "return n\n";
}

bool run_busy(
    host::Sandbox& sandbox, Package& package, int repeats, std::chrono::milliseconds* elapsed
) {
    host::ScriptError error;
    Number number;
    host::CallHooks hooks;
    hooks.context = &number;
    hooks.read = read_number;
    put(package, "busy.lua", busy_compares(repeats));
    const auto started = std::chrono::steady_clock::now();
    const bool ran = sandbox.run_file("busy.lua", "plan", &hooks, error);
    *elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started
    );
    if (!ran) {
        OA_CHECK(error.kind == host::ErrorKind::time_limit);
        return false;
    }
    OA_CHECK(number.saw);
    OA_CHECK(number.value == repeats);
    return true;
}

void the_next_phase_starts_again() {
    const auto limit = std::chrono::milliseconds(400);
    const auto low = limit * 60 / 100;
    const auto high = limit * 80 / 100;
    int repeats = 80;
    bool found = false;
    for (int attempt = 0; attempt < 12 && !found; ++attempt) {
        Package package;
        host::ScriptError error;
        auto sandbox = open_limited(package, error, limit);
        if (!sandbox)
            return;
        std::chrono::milliseconds elapsed{0};
        const bool ran = run_busy(*sandbox, package, repeats, &elapsed);
        if (!ran) {
            repeats = repeats > 1 ? repeats / 2 : 1;
            continue;
        }
        if (elapsed < low) {
            const auto elapsed_count = elapsed.count();
            int64_t scaled = repeats * 2;
            if (elapsed_count > 0)
                scaled = static_cast<int64_t>(repeats) * (limit.count() * 70 / 100) / elapsed_count;
            if (scaled <= repeats)
                scaled = static_cast<int64_t>(repeats) * 2;
            if (scaled > 2000000)
                scaled = 2000000;
            repeats = static_cast<int>(scaled);
            continue;
        }
        if (elapsed > high) {
            repeats = repeats > 1 ? repeats * 2 / 3 : 1;
            if (repeats < 1)
                repeats = 1;
            continue;
        }
        found = true;
    }
    OA_CHECK(found);
    if (!found)
        return;

    bool proved = false;
    for (int pair = 0; pair < 4 && !proved; ++pair) {
        Package package;
        host::ScriptError error;
        auto sandbox = open_limited(package, error, limit);
        if (!sandbox)
            return;
        std::chrono::milliseconds first{0};
        std::chrono::milliseconds second{0};
        const bool first_ran = run_busy(*sandbox, package, repeats, &first);
        if (!first_ran) {
            repeats = repeats > 1 ? repeats * 4 / 5 : 1;
            continue;
        }
        const bool second_ran = run_busy(*sandbox, package, repeats, &second);
        if (!second_ran) {
            repeats = repeats > 1 ? repeats * 4 / 5 : 1;
            continue;
        }
        if (first + second <= limit) {
            const int grown = repeats + repeats / 5 + 1;
            repeats = grown > 2000000 ? 2000000 : grown;
            continue;
        }
        proved = true;
        std::printf(
            "time-limit fresh phases %lld ms then %lld ms, limit %lld ms, repeats %d\n",
            static_cast<long long>(first.count()),
            static_cast<long long>(second.count()),
            static_cast<long long>(limit.count()),
            repeats
        );
    }
    OA_CHECK(proved);
}

void protected_metatable_matches_lua() {
    const char* source = "local value = {}\n"
                         "setmetatable(value, { __metatable = \"locked\" })\n"
                         "setmetatable(value, {})\n";
    Package package;
    host::ScriptError error;
    auto sandbox = open_limited(package, error, host::default_time_limit);
    if (!sandbox)
        return;
    put(package, "mutator.lua", source);
    OA_CHECK(!sandbox->run_file("mutator.lua", "plan", nullptr, error));
    OA_CHECK(error.kind == host::ErrorKind::runtime);
    const std::string sandbox_text =
        error.chunk + ":" + std::to_string(error.line) + ": " + error.message;

    lua_State* raw = luaL_newstate();
    OA_CHECK(raw != nullptr);
    if (raw == nullptr)
        return;
    luaL_requiref(raw, "_G", luaopen_base, 1);
    lua_pop(raw, 1);
    const int loaded = luaL_loadbufferx(raw, source, std::strlen(source), "@mutator.lua", "t");
    OA_CHECK(loaded == LUA_OK);
    if (loaded != LUA_OK) {
        lua_close(raw);
        return;
    }
    const int status = lua_pcall(raw, 0, 0, 0);
    OA_CHECK(status != LUA_OK);
    const char* raw_text = lua_tostring(raw, -1);
    OA_CHECK(raw_text != nullptr);
    if (raw_text != nullptr && sandbox_text != raw_text) {
        std::fprintf(stderr, "sandbox [%s]\nraw [%s]\n", sandbox_text.c_str(), raw_text);
        OA_CHECK(sandbox_text == raw_text);
    } else {
        OA_CHECK(raw_text != nullptr && sandbox_text == raw_text);
    }
    lua_close(raw);
}

} // namespace

int main() {
    static_assert(host::default_time_limit == std::chrono::milliseconds{10000});

    const auto limit = std::chrono::milliseconds(300);
    int64_t worst = 0;

    struct Stop {
        const char* name;
        const char* source;
    };

    const Stop stopping[] = {
        {"tight-loop", tight_loop},
        {"zero-filled", zero_filled},
        {"wide-call", wide_call},
        {"pcall", caught_loop},
        {"xpcall", handled_loop},
    };
    for (const auto& row : stopping) {
        const int64_t overshoot = expect_stopped(row.name, row.source, limit, "plan");
        if (overshoot > worst)
            worst = overshoot;
    }
    const int64_t one_second =
        expect_stopped("one-second", tight_loop, std::chrono::milliseconds(1000), "plan");
    if (one_second > worst)
        worst = one_second;

    a_short_script_is_unchanged();
    the_next_phase_starts_again();
    protected_metatable_matches_lua();

    std::printf("time-limit worst overshoot %lld ms\n", static_cast<long long>(worst));
    return oa::test::check_exit_status();
}
