// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A mutator's hacks: reading the plan's table, merging two mutators, the
// notes Developer Mode and a mod that leaves a hack off produce, and the
// values the resolver plays.

#include "oa/data/mod_profile.hpp"
#include "oa/data/mod_profile/registry.hpp"
#include "oa/data/mutators/hacks.hpp"
#include "oa/test/check.hpp"

#include "lua.hpp"

#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using oa::data::mod_profile::HackOverride;
using oa::data::mod_profile::MatchOption;
using oa::data::mod_profile::ResolveOptions;
using oa::data::mod_profile::ResolveResult;
using oa::data::mutators::HackMerge;
using oa::data::mutators::HackNote;
using oa::data::mutators::MutatorHack;

constexpr std::string_view head = "oamod: 1\n"
                                  "id: {id}\n"
                                  "name: t\n"
                                  "version: \"1\"\n"
                                  "requires: {base: ta-3.1c, catalogue: 1}\n"
                                  "author: {name: unknown}\n"
                                  "packaging: {revision: 1, date: 2026-10-04, packager: t}\n";

std::string profile_text(std::string_view id, std::string_view body) {
    std::string text{head};
    text.replace(text.find("{id}"), 4, id);
    return text + std::string{body};
}

ResolveResult resolve(std::string_view text, ResolveOptions options) {
    options.accept_unimplemented_hacks = true;
    return oa::data::mod_profile::resolve_profile(
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(text.data()), text.size()},
        "test.oamod",
        options
    );
}

const oa::data::mod_profile::Value*
at(const oa::data::mod_profile::Resolution& resolution,
   std::initializer_list<std::string_view> keys) {
    const oa::data::mod_profile::Value* value = &resolution.effective;
    for (const std::string_view key : keys) {
        if (value == nullptr)
            return nullptr;
        value = oa::data::mod_profile::find_member(*value, key);
    }
    return value;
}

std::string source_of(const oa::data::mod_profile::Resolution& resolution, std::string_view path) {
    for (const oa::data::mod_profile::Provenance& provenance : resolution.provenance) {
        if (provenance.path == path)
            return provenance.source;
    }
    return {};
}

/// A fresh Lua state with one table at the top.
struct LuaTable {
    lua_State* state{luaL_newstate()};

    LuaTable() { lua_newtable(state); }

    ~LuaTable() { lua_close(state); }

    LuaTable(const LuaTable&) = delete;
    LuaTable& operator=(const LuaTable&) = delete;
};

std::string
read_top(LuaTable& lua, std::string_view mutator, bool layer, std::vector<MutatorHack>& out) {
    return oa::data::mutators::read_hacks(lua.state, -1, mutator, layer, out);
}

void test_reading() {
    {
        LuaTable lua;
        lua_newtable(lua.state);
        lua_pushinteger(lua.state, 4);
        lua_setfield(lua.state, -2, "min");
        lua_pushinteger(lua.state, 12);
        lua_setfield(lua.state, -2, "max");
        lua_setfield(lua.state, -2, "console.game-speed-range");
        std::vector<MutatorHack> out;
        OA_CHECK(read_top(lua, "steady-pace", true, out).empty());
        OA_CHECK(out.size() == 2);
        if (out.size() == 2) {
            OA_CHECK(out[0].path == "console.game-speed-range.max");
            OA_CHECK(out[0].value == "12");
            OA_CHECK(out[0].mutator == "steady-pace");
            OA_CHECK(out[1].path == "console.game-speed-range.min");
            OA_CHECK(out[1].value == "4");
        }
    }
    {
        LuaTable lua;
        lua_pushinteger(lua.state, 400);
        lua_setfield(lua.state, -2, "limits.units-per-player");
        std::vector<MutatorHack> out;
        OA_CHECK(read_top(lua, "ridge", true, out).empty());
        OA_CHECK(out.size() == 1);
        if (out.size() == 1) {
            OA_CHECK(out[0].path == "limits.units-per-player.default");
            OA_CHECK(out[0].value == "400");
        }
    }
    {
        LuaTable lua;
        lua_newtable(lua.state);
        lua_pushstring(lua.state, "15 # open-annihilation: injected");
        lua_setfield(lua.state, -2, "max");
        lua_setfield(lua.state, -2, "console.game-speed-range");
        std::vector<MutatorHack> out;
        OA_CHECK(read_top(lua, "steady-pace", true, out).empty());
        OA_CHECK(out.size() == 1);
        if (out.size() == 1)
            OA_CHECK(out[0].value == "15");
    }
    {
        LuaTable lua;
        lua_pushboolean(lua.state, 1);
        lua_setfield(lua.state, -2, "console.game-speed-range");
        std::vector<MutatorHack> out;
        const std::string problem = read_top(lua, "steady-pace", true, out);
        OA_CHECK(
            problem ==
            "hacks: console.game-speed-range: a mutator sets a parameter, not true or false"
        );
        OA_CHECK(out.empty());
    }
    {
        LuaTable lua;
        lua_newtable(lua.state);
        lua_pushboolean(lua.state, 1);
        lua_setfield(lua.state, -2, "available");
        lua_setfield(lua.state, -2, "setup.commander-warp");
        std::vector<MutatorHack> out;
        const std::string problem = read_top(lua, "steady-pace", true, out);
        OA_CHECK(
            problem ==
            "hacks: setup.commander-warp: a mutator cannot set this hack (the registry does not "
            "open it to mutators)"
        );
        OA_CHECK(out.empty());
    }
    {
        LuaTable lua;
        lua_newtable(lua.state);
        lua_pushboolean(lua.state, 1);
        lua_setfield(lua.state, -2, "syncon");
        lua_setfield(lua.state, -2, "console.game-speed-range");
        std::vector<MutatorHack> out;
        const std::string problem = read_top(lua, "steady-pace", true, out);
        OA_CHECK(
            problem ==
            "hacks: console.game-speed-range: only parameters the registry marks adjustable per "
            "match can be set (syncon is fixed)"
        );
        OA_CHECK(out.empty());
    }
    {
        LuaTable lua;
        lua_pushinteger(lua.state, 1);
        lua_setfield(lua.state, -2, "nope.nope");
        std::vector<MutatorHack> out;
        const std::string problem = read_top(lua, "steady-pace", true, out);
        OA_CHECK(problem == "hacks: nope.nope: unknown parameter");
        OA_CHECK(out.empty());
    }
    {
        LuaTable lua;
        lua_newtable(lua.state);
        lua_pushinteger(lua.state, 30);
        lua_setfield(lua.state, -2, "min");
        lua_setfield(lua.state, -2, "console.game-speed-range");
        std::vector<MutatorHack> out;
        const std::string problem = read_top(lua, "steady-pace", true, out);
        OA_CHECK(problem == "hacks: console.game-speed-range: 30 is outside [0, 20]");
        OA_CHECK(out.empty());
    }
    {
        LuaTable lua;
        lua_newtable(lua.state);
        lua_pushinteger(lua.state, 15);
        lua_setfield(lua.state, -2, "max");
        lua_setfield(lua.state, -2, "console.game-speed-range");
        std::vector<MutatorHack> out;
        const std::string problem = read_top(lua, "steady-pace", false, out);
        OA_CHECK(
            problem ==
            "hacks: console.game-speed-range: the manifest does not declare the hacks layer"
        );
        OA_CHECK(out.empty());
    }
}

void test_merging() {
    const std::vector<MutatorHack> hacks = {
        MutatorHack{"steady-pace", "console.game-speed-range.min", "2"},
        MutatorHack{"fast-pace", "console.game-speed-range.min", "8"},
        MutatorHack{"fast-pace", "console.game-speed-range.max", "15"},
    };
    const HackMerge merged = oa::data::mutators::merge_hacks(hacks);
    OA_CHECK(merged.hacks.size() == 2);
    if (merged.hacks.size() == 2) {
        OA_CHECK(merged.hacks[0].path == "console.game-speed-range.max");
        OA_CHECK(merged.hacks[0].value == "15");
        OA_CHECK(merged.hacks[1].path == "console.game-speed-range.min");
        OA_CHECK(merged.hacks[1].value == "8");
        OA_CHECK(merged.hacks[1].mutator == "fast-pace");
    }
    OA_CHECK(merged.notes.size() == 1);
    if (!merged.notes.empty()) {
        OA_CHECK(merged.notes[0].entry == "console.game-speed-range");
        OA_CHECK(
            merged.notes[0].text ==
            "steady-pace and fast-pace both set console.game-speed-range.min; fast-pace's value "
            "plays"
        );
    }
}

const HackNote* note_for(const std::vector<HackNote>& notes, std::string_view entry) {
    for (const HackNote& note : notes) {
        if (note.entry == entry)
            return &note;
    }
    return nullptr;
}

void test_developer_mode() {
    const std::vector<MutatorHack> hacks = {
        MutatorHack{"steady-pace", "console.game-speed-range.min", "4"}
    };
    const std::vector<std::string> on{"console.game-speed-range"};
    const std::vector<HackNote> quiet = oa::data::mutators::developer_conflicts(hacks, {}, on);
    OA_CHECK(quiet.empty());

    const std::vector<HackOverride> also{HackOverride{"console.game-speed-range", true, {}}};
    const std::vector<HackNote> overlap = oa::data::mutators::developer_conflicts(hacks, also, on);
    const HackNote* also_note = note_for(overlap, "console.game-speed-range");
    OA_CHECK(also_note != nullptr);
    if (also_note != nullptr)
        OA_CHECK(also_note->text == "Developer Mode also sets console.game-speed-range");

    const std::vector<HackNote> left = oa::data::mutators::developer_conflicts(hacks, {}, {});
    const HackNote* left_note = note_for(left, "console.game-speed-range");
    OA_CHECK(left_note != nullptr);
    if (left_note != nullptr) {
        OA_CHECK(
            left_note->text ==
            "the mod leaves console.game-speed-range off; steady-pace turns it on for this game"
        );
    }

    const std::vector<HackOverride> closed{HackOverride{"console.game-speed-range", false, {}}};
    const std::vector<HackNote> off = oa::data::mutators::developer_conflicts(hacks, closed, on);
    const HackNote* off_note = note_for(off, "console.game-speed-range");
    OA_CHECK(off_note != nullptr);
    if (off_note != nullptr) {
        OA_CHECK(
            off_note->text ==
            "Developer Mode turns console.game-speed-range off; steady-pace turns it on for this "
            "game"
        );
    }
}

void test_resolver() {
    const std::string on = profile_text("ridge", "hacks:\n  console.game-speed-range: true\n");
    ResolveOptions options{};
    MatchOption option;
    option.path = "console.game-speed-range.min";
    option.value = "4";
    option.source = "steady-pace";
    option.turn_on = true;
    options.match = {option};
    const ResolveResult played = resolve(on, options);
    OA_CHECK(played.resolution.has_value());
    if (played.resolution) {
        const oa::data::mod_profile::Value* min =
            at(*played.resolution, {"hacks", "console.game-speed-range", "min"});
        OA_CHECK(min != nullptr && oa::data::mod_profile::canonical_json(*min) == "4");
        OA_CHECK(
            source_of(*played.resolution, "hacks.console.game-speed-range.min") ==
            "match steady-pace"
        );
    }

    const std::string off = profile_text("plain", "");
    const ResolveResult over_mod = resolve(off, options);
    OA_CHECK(over_mod.resolution.has_value());
    if (over_mod.resolution) {
        OA_CHECK(
            source_of(*over_mod.resolution, "hacks.console.game-speed-range.min") ==
            "match steady-pace (turned on)"
        );
    }
    const std::vector<MutatorHack> pace{
        MutatorHack{"steady-pace", "console.game-speed-range.min", "4"}
    };
    const std::vector<HackNote> left = oa::data::mutators::developer_conflicts(pace, {}, {});
    OA_CHECK(!left.empty() && left[0].text.find("console.game-speed-range") != std::string::npos);

    ResolveOptions closed = options;
    closed.overrides = {HackOverride{"console.game-speed-range", false, {}}};
    const ResolveResult over_mode = resolve(off, closed);
    OA_CHECK(over_mode.resolution.has_value());
    if (over_mode.resolution) {
        const oa::data::mod_profile::Value* min =
            at(*over_mode.resolution, {"hacks", "console.game-speed-range", "min"});
        OA_CHECK(min != nullptr && oa::data::mod_profile::canonical_json(*min) == "4");
        OA_CHECK(
            source_of(*over_mode.resolution, "hacks.console.game-speed-range.min") ==
            "match steady-pace (turned on)"
        );
    }
    const std::vector<HackOverride> mode{HackOverride{"console.game-speed-range", false, {}}};
    const std::vector<HackNote> turned = oa::data::mutators::developer_conflicts(pace, mode, {});
    OA_CHECK(
        !turned.empty() && turned[0].text.find("console.game-speed-range") != std::string::npos
    );
    OA_CHECK(
        !turned.empty() &&
        turned[0].text ==
            "Developer Mode turns console.game-speed-range off; steady-pace turns it on for this "
            "game"
    );
}

} // namespace

int main() {
    test_reading();
    test_merging();
    test_developer_mode();
    test_resolver();
    return oa::test::check_exit_status();
}
