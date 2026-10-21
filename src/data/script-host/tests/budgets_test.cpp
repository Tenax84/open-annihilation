// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Instruction and memory allowances. A phase that spends its allowance, or
// passes the memory cap, fails the sandbox even when the script catches the
// error. The allowance starts again on the next phase.

#include "oa/data/script_host/script_host.hpp"
#include "oa/test/check.hpp"

#include <cstdint>
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

host::Profile test_profile() {
    host::Budget budget;
    budget.instructions = 2'000'000;
    budget.memory_bytes = std::size_t{8} << 20;
    return host::Profile{"test", budget, nullptr, nullptr};
}

std::optional<host::Sandbox>
open_sample(Package& package, host::ScriptError& error, host::Profile profile = test_profile()) {
    host::OpenOptions options;
    options.package_id = "sample";
    options.profile = profile;
    options.sources.context = &package;
    options.sources.read = read_package;
    auto sandbox = host::Sandbox::open(options, error);
    OA_CHECK(sandbox.has_value());
    return sandbox;
}

bool run_source(
    host::Sandbox& sandbox,
    Package& package,
    std::string_view source,
    host::ScriptError& error,
    const char* phase = "tick"
) {
    put(package, "mutator.lua", source);
    return sandbox.run_file("mutator.lua", phase, nullptr, error);
}

void instruction_loop_stops(host::Profile profile, uint64_t allowance) {
    Package package;
    host::ScriptError error;
    auto sandbox = open_sample(package, error, profile);
    if (!sandbox)
        return;
    OA_CHECK(!run_source(*sandbox, package, "while true do end\n", error));
    OA_CHECK(error.kind == host::ErrorKind::instruction_budget);
    const std::string message =
        "the script ran past its budget of " + std::to_string(allowance) + " instructions";
    OA_CHECK(error.message == message);
    const uint64_t used = sandbox->instructions_used();
    OA_CHECK(used >= allowance);
    OA_CHECK(used <= allowance + static_cast<uint64_t>(host::budget_check_interval));
}

void caught_loops_still_stop() {
    const char* sources[] = {
        "while true do pcall(function() while true do end end) end\n",
        "while true do xpcall(function() while true do end end, function() while true do end end) "
        "end\n",
        "while true do xpcall(function() while true do end end, "
        "function() while true do pcall(function() while true do end end) end end) end\n",
    };
    for (const char* source : sources) {
        Package package;
        host::ScriptError error;
        auto sandbox = open_sample(package, error);
        if (!sandbox)
            continue;
        OA_CHECK(!run_source(*sandbox, package, source, error));
        OA_CHECK(error.kind == host::ErrorKind::instruction_budget);
        OA_CHECK(error.message == "the script ran past its budget of 2000000 instructions");
    }
}

std::string counted_loop(uint64_t count) {
    return "local i = 0\nwhile i < " + std::to_string(count) + " do\n  i = i + 1\nend\nreturn i\n";
}

uint64_t find_half_budget() {
    constexpr uint64_t allowance = 2'000'000;
    uint64_t count = 10000;
    for (int attempt = 0; attempt < 24; ++attempt) {
        Package package;
        host::ScriptError error;
        auto sandbox = open_sample(package, error);
        if (!sandbox)
            return 0;
        const bool ran = run_source(*sandbox, package, counted_loop(count), error);
        const uint64_t used = sandbox->instructions_used();
        if (ran && used > allowance / 2 && used < allowance)
            return count;
        if (!ran || used >= allowance) {
            count = count > 1 ? count / 2 : 1;
            continue;
        }
        if (used == 0) {
            count = count > allowance / 10 ? allowance / 2 : count * 10;
            continue;
        }
        const uint64_t target = allowance * 3 / 5;
        uint64_t scaled = count * target / used;
        if (scaled <= count)
            scaled = count + count / 5 + 1;
        if (scaled > allowance)
            scaled = allowance;
        count = scaled;
    }
    return 0;
}

void each_phase_has_its_own_allowance() {
    const uint64_t count = find_half_budget();
    OA_CHECK(count != 0);
    if (count == 0)
        return;
    Package package;
    host::ScriptError error;
    auto sandbox = open_sample(package, error);
    if (!sandbox)
        return;
    constexpr uint64_t allowance = 2'000'000;
    OA_CHECK(run_source(*sandbox, package, counted_loop(count), error, "first"));
    const uint64_t first = sandbox->instructions_used();
    OA_CHECK(run_source(*sandbox, package, counted_loop(count), error, "second"));
    const uint64_t second = sandbox->instructions_used();
    OA_CHECK(first > allowance / 2 && first < allowance);
    OA_CHECK(second > allowance / 2 && second < allowance);
}

void memory_cap_stops_the_phase(const char* source) {
    Package package;
    host::ScriptError error;
    auto sandbox = open_sample(package, error);
    if (!sandbox)
        return;
    OA_CHECK(!run_source(*sandbox, package, source, error));
    OA_CHECK(error.kind == host::ErrorKind::memory_budget);
    OA_CHECK(error.message == "the script used more than its 8 MiB of memory");
    OA_CHECK(sandbox->memory_in_use() <= std::size_t{8} << 20);
}

void a_required_loop_is_reported_at_the_module() {
    Package package;
    put(package, "lua/spin.lua", "while true do end\n");
    host::ScriptError error;
    auto sandbox = open_sample(package, error);
    if (!sandbox)
        return;
    put(package, "mutator.lua", "require(\"spin\")\n");
    OA_CHECK(!sandbox->run_file("mutator.lua", "tick", nullptr, error));
    OA_CHECK(error.kind == host::ErrorKind::instruction_budget);
    OA_CHECK(error.chunk == "lua/spin.lua");
    OA_CHECK(error.message == "the script ran past its budget of 2000000 instructions");
}

} // namespace

int main() {
    instruction_loop_stops(test_profile(), 2'000'000);
    caught_loops_still_stop();
    each_phase_has_its_own_allowance();
    memory_cap_stops_the_phase(
        "local kept = {}\nfor i = 1, 200 do\n  kept[i] = string.rep(\"x\", 65536)\nend\n"
    );
    memory_cap_stops_the_phase(
        "local kept = {}\npcall(function()\n  for i = 1, 200 do\n    kept[i] = string.rep(\"x\", "
        "65536)\n  end\nend)\n"
    );
    a_required_loop_is_reported_at_the_module();
    instruction_loop_stops(host::mutator_profile(), host::default_instruction_budget);
    return oa::test::check_exit_status();
}
