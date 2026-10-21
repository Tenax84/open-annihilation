// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A string of 65536 bytes is allowed. Every way of making a longer one ends
// the phase with string_length, at the line that made it.

#include "lua.hpp"
#include "oa/data/script_host/script_host.hpp"
#include "oa/test/check.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

host::Profile mutator() {
    return host::mutator_profile();
}

std::optional<host::Sandbox>
open_with(Package& package, host::ScriptError& error, host::Profile profile = mutator()) {
    host::OpenOptions options;
    options.package_id = "sample";
    options.profile = profile;
    options.sources.context = &package;
    options.sources.read = read_package;
    auto sandbox = host::Sandbox::open(options, error);
    OA_CHECK(sandbox.has_value());
    return sandbox;
}

bool names_limit(const host::ScriptError& error) {
    return error.message.find("65537") != std::string::npos ||
           error.message.find("65536") != std::string::npos;
}

void expect_refused(std::string_view source, const char* chunk, int line) {
    Package package;
    host::ScriptError error;
    auto sandbox = open_with(package, error);
    if (!sandbox)
        return;
    put(package, "mutator.lua", source);
    const bool ran = sandbox->run_file("mutator.lua", "tick", nullptr, error);
    OA_CHECK(!ran);
    OA_CHECK(error.kind == host::ErrorKind::string_length);
    OA_CHECK(error.chunk == chunk);
    OA_CHECK(error.line == line);
    OA_CHECK(names_limit(error));
}

struct Number {
    lua_Integer value = -1;
    bool saw = false;
};

void read_number(void* context, lua_State* state, int results) {
    auto* out = static_cast<Number*>(context);
    out->saw = results >= 1 && lua_isinteger(state, -1);
    if (out->saw)
        out->value = lua_tointeger(state, -1);
}

void a_full_string_is_kept() {
    Package package;
    host::ScriptError error;
    auto sandbox = open_with(package, error);
    if (!sandbox)
        return;
    Number number;
    host::CallHooks hooks;
    hooks.context = &number;
    hooks.read = read_number;
    const char* source = "local s = string.rep(\"a\", 65536)\n"
                         "local up = #string.upper(s)\n"
                         "local rev = #string.reverse(s)\n"
                         "local same = #(s .. \"\")\n"
                         "return up + rev + same\n";
    put(package, "mutator.lua", source);
    const bool ran = sandbox->run_file("mutator.lua", "tick", &hooks, error);
    OA_CHECK(ran);
    OA_CHECK(number.saw);
    OA_CHECK(number.value == 65536 * 3);
}

void each_full_route_is_kept() {
    Package package;
    host::ScriptError error;
    auto sandbox = open_with(package, error);
    if (!sandbox)
        return;
    Number number;
    host::CallHooks hooks;
    hooks.context = &number;
    hooks.read = read_number;
    const char* source = "local a = string.rep(\"x\", 30000)\n"
                         "local b = string.rep(\"y\", 35536)\n"
                         "local joined = a .. b\n"
                         "local formatted = string.format(\"%s%s\", a, b)\n"
                         "local concatenated = table.concat({a, b})\n"
                         "local codes = {}\n"
                         "for i = 1, 65536 do\n"
                         "  codes[i] = 120\n"
                         "end\n"
                         "local from_char = string.char(table.unpack(codes))\n"
                         "local from_utf = utf8.char(table.unpack(codes))\n"
                         "local replaced = string.rep(\"x\", 65536):gsub(\"z\", \"y\")\n"
                         "local shown = tostring(setmetatable({}, {\n"
                         "  __tostring = function()\n"
                         "    return string.rep(\"q\", 65536)\n"
                         "  end\n"
                         "}))\n"
                         "return #joined + #formatted + #concatenated + #from_char + #from_utf +"
                         " #replaced + #shown\n";
    put(package, "mutator.lua", source);
    const bool ran = sandbox->run_file("mutator.lua", "tick", &hooks, error);
    OA_CHECK(ran);
    OA_CHECK(number.saw);
    OA_CHECK(number.value == 65536 * 7);
}

void each_route_is_refused() {
    expect_refused(
        "local a = string.rep(\"x\", 40000)\n"
        "local b = string.rep(\"y\", 25537)\n"
        "return a .. b\n",
        "mutator.lua",
        3
    );
    expect_refused(
        "local a = string.rep(\"x\", 30000)\n"
        "local b = string.rep(\"y\", 30000)\n"
        "local c = string.rep(\"z\", 5537)\n"
        "return a .. b .. c\n",
        "mutator.lua",
        4
    );
    expect_refused("return string.rep(\"a\", 65537)\n", "mutator.lua", 1);
    expect_refused(
        "local a = string.rep(\"x\", 40000)\n"
        "local b = string.rep(\"y\", 25537)\n"
        "return string.format(\"%s%s\", a, b)\n",
        "mutator.lua",
        3
    );
    expect_refused(
        "local a = string.rep(\"x\", 40000)\n"
        "local b = string.rep(\"y\", 25537)\n"
        "return table.concat({a, b})\n",
        "mutator.lua",
        3
    );
    expect_refused(
        "local codes = {}\n"
        "for i = 1, 65537 do\n"
        "  codes[i] = 120\n"
        "end\n"
        "return string.char(table.unpack(codes))\n",
        "mutator.lua",
        5
    );
    expect_refused(
        "local codes = {}\n"
        "for i = 1, 65537 do\n"
        "  codes[i] = 120\n"
        "end\n"
        "return utf8.char(table.unpack(codes))\n",
        "mutator.lua",
        5
    );
    expect_refused("return (\"x\"):rep(40000):gsub(\"x\", \"yy\")\n", "mutator.lua", 1);
    expect_refused(
        "local a = string.rep(\"x\", 40000)\n"
        "local b = string.rep(\"y\", 25537)\n"
        "local t = setmetatable({}, {\n"
        "  __tostring = function()\n"
        "    return a .. b\n"
        "  end\n"
        "})\n"
        "return tostring(t)\n",
        "mutator.lua",
        5
    );
}

std::string long_literal() {
    std::string source = "return [[";
    source.append(65537, 'x');
    source += "]]\n";
    return source;
}

void a_literal_is_refused() {
    Package package;
    host::ScriptError error;
    auto sandbox = open_with(package, error);
    if (!sandbox)
        return;
    put(package, "mutator.lua", long_literal());
    const bool ran = sandbox->run_file("mutator.lua", "tick", nullptr, error);
    OA_CHECK(!ran);
    OA_CHECK(error.kind == host::ErrorKind::string_length);
    OA_CHECK(error.chunk == "mutator.lua");
    OA_CHECK(error.line == 0);
    OA_CHECK(error.message.find("65537") != std::string::npos);
}

void a_required_literal_is_refused() {
    Package package;
    put(package, "lua/big.lua", long_literal());
    host::ScriptError error;
    auto sandbox = open_with(package, error);
    if (!sandbox)
        return;
    put(package, "mutator.lua", "require(\"big\")\n");
    const bool ran = sandbox->run_file("mutator.lua", "tick", nullptr, error);
    OA_CHECK(!ran);
    OA_CHECK(error.kind == host::ErrorKind::string_length);
    OA_CHECK(error.chunk == "lua/big.lua");
    OA_CHECK(error.line == 0);
    OA_CHECK(error.message.find("65537") != std::string::npos);
}

char host_bytes[65537];

void push_long(void* /*context*/, lua_State* state) {
    std::memset(host_bytes, 'q', sizeof host_bytes);
    lua_pushlstring(state, host_bytes, sizeof host_bytes);
}

void a_host_push_is_refused() {
    Package package;
    host::ScriptError error;
    auto sandbox = open_with(package, error);
    if (!sandbox)
        return;
    const bool ran = sandbox->run_host("push", nullptr, push_long, error);
    OA_CHECK(!ran);
    OA_CHECK(error.kind == host::ErrorKind::string_length);
    OA_CHECK(error.chunk.empty());
    OA_CHECK(error.line == 0);
    OA_CHECK(error.message.find("65537") != std::string::npos);
    OA_CHECK(error.message.find("65536") != std::string::npos);
}

void a_caught_refusal_does_not_continue() {
    Package package;
    host::ScriptError error;
    auto sandbox = open_with(package, error);
    if (!sandbox)
        return;
    const char* source = "big = string.rep(\"x\", 65536)\n"
                         "caught = false\n"
                         "steps = 0\n"
                         "caught = not pcall(function()\n"
                         "  return big .. \"x\"\n"
                         "end)\n"
                         "while steps < 100000 do\n"
                         "  steps = steps + 1\n"
                         "end\n";
    put(package, "mutator.lua", source);
    const bool ran = sandbox->run_file("mutator.lua", "tick", nullptr, error);
    OA_CHECK(!ran);
    OA_CHECK(error.kind == host::ErrorKind::string_length);
    OA_CHECK(error.chunk == "mutator.lua");
    OA_CHECK(error.line == 5);
    OA_CHECK(error.message.find("65537") != std::string::npos);
    lua_State* state = sandbox->state();
    lua_getglobal(state, "caught");
    OA_CHECK(lua_toboolean(state, -1) == 1);
    lua_getglobal(state, "steps");
    OA_CHECK(lua_isinteger(state, -1));
    OA_CHECK(lua_tointeger(state, -1) <= host::budget_check_interval);
}

void print_cuts_a_long_line() {
    Package package;
    host::ScriptError error;
    auto sandbox = open_with(package, error);
    if (!sandbox)
        return;
    const char* source = "local big = string.rep(\"x\", 40000)\n"
                         "print(big, big)\n";
    put(package, "mutator.lua", source);
    const bool ran = sandbox->run_file("mutator.lua", "tick", nullptr, error);
    OA_CHECK(ran);
    const auto lines = sandbox->printed();
    OA_CHECK(lines.size() == 1);
    OA_CHECK(lines[0].size() == host::max_printed_bytes);
    OA_CHECK(!lines[0].empty() && lines[0][0] == 'x');
    OA_CHECK(lines[0].find("left out") == std::string::npos);
}

void shorter_strings_still_hit_memory() {
    Package package;
    host::ScriptError error;
    host::Budget budget;
    budget.instructions = host::default_instruction_budget;
    budget.memory_bytes = std::size_t{1} << 20;
    auto sandbox = open_with(package, error, host::Profile{"tight", budget, nullptr, nullptr});
    if (!sandbox)
        return;
    const char* source = "local kept = {}\n"
                         "for i = 1, 40 do\n"
                         "  kept[i] = string.rep(\"x\", 65536)\n"
                         "end\n";
    put(package, "mutator.lua", source);
    const bool ran = sandbox->run_file("mutator.lua", "tick", nullptr, error);
    OA_CHECK(!ran);
    OA_CHECK(error.kind == host::ErrorKind::memory_budget);
    OA_CHECK(error.message == "the script used more than its 1 MiB of memory");
}

void two_sandboxes_measure_one_header() {
    Package first_package;
    Package second_package;
    host::ScriptError first_error;
    host::ScriptError second_error;
    auto first = open_with(first_package, first_error);
    auto second = open_with(second_package, second_error);
    if (!first || !second)
        return;
    const std::size_t header = first->string_header_bytes();
    std::printf("string header %zu bytes\n", header);
    OA_CHECK(header > 0);
    OA_CHECK(header == second->string_header_bytes());
}

} // namespace

int main() {
    two_sandboxes_measure_one_header();
    a_full_string_is_kept();
    each_full_route_is_kept();
    each_route_is_refused();
    a_literal_is_refused();
    a_required_literal_is_refused();
    a_host_push_is_refused();
    a_caught_refusal_does_not_continue();
    print_cuts_a_long_line();
    shorter_strings_still_hit_memory();
    return oa::test::check_exit_status();
}
