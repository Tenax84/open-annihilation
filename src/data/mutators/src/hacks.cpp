// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Reads a plan's hacks table against the registry and merges the records.

#include "oa/data/mutators/hacks.hpp"

#include "oa/data/mod_profile.hpp"
#include "oa/data/mod_profile/registry.hpp"
#include "oa/data/mod_profile/value.hpp"

#include "lua.hpp"

#include <algorithm>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::data::mutators {
namespace {

namespace registry = oa::data::mod_profile::registry;

using registry::Adjustable;
using registry::Entry;
using registry::Parameter;
using registry::ValueType;

/// The registry entry a parameter path names. Parameter names have no dot.
std::string entry_id(std::string_view path) {
    const std::size_t dot = path.rfind('.');
    if (dot == std::string_view::npos)
        return std::string{path};
    return std::string{path.substr(0, dot)};
}

/// The profile text of one Lua scalar, or none when the value is not one.
///
/// An integer is written with std::to_string. lua_tostring would change a
/// number's type in the state, so it is not used.
std::optional<std::string> scalar_text(lua_State* state, int index) {
    const int value = lua_absindex(state, index);
    if (lua_isboolean(state, value) != 0)
        return lua_toboolean(state, value) != 0 ? std::string{"true"} : std::string{"false"};
    if (lua_isinteger(state, value) != 0)
        return std::to_string(lua_tointeger(state, value));
    if (lua_type(state, value) == LUA_TNUMBER) {
        std::ostringstream stream;
        stream << lua_tonumber(state, value);
        return stream.str();
    }
    if (lua_type(state, value) == LUA_TSTRING) {
        std::size_t length = 0;
        const char* text = lua_tolstring(state, value, &length);
        if (text == nullptr)
            return std::string{};
        return std::string{text, length};
    }
    return std::nullopt;
}

/// The parameter a scalar sets.
///
/// The registry's shorthand names one parameter. A marked entry with no
/// shorthand and exactly one parameter adjustable per match, such as
/// limits.units-per-player, takes a scalar as that parameter. true and false
/// are still refused when that parameter is not a boolean.
int scalar_parameter(const Entry& entry) {
    if (entry.shorthand >= 0)
        return entry.shorthand;
    const std::span<const Parameter> parameters = registry::parameters_of(entry);
    int match = -1;
    int count = 0;
    for (std::size_t slot = 0; slot < parameters.size(); ++slot) {
        if (parameters[slot].adjustable != Adjustable::match)
            continue;
        match = static_cast<int>(slot);
        ++count;
    }
    if (count == 1)
        return match;
    return -1;
}

std::string adjustable_word(Adjustable adjustable) {
    if (adjustable == Adjustable::fixed)
        return "fixed";
    if (adjustable == Adjustable::install)
        return "install";
    return "match";
}

/// Refuses a parameter that is not adjustable per match, or a value the
/// registry refuses, and otherwise appends one record.
std::string take_parameter(
    std::string_view mutator,
    const std::string& id,
    std::string_view name,
    const std::string& raw,
    const Parameter& parameter,
    std::vector<MutatorHack>& made
) {
    if (parameter.adjustable != Adjustable::match) {
        return "hacks: " + id +
               ": only parameters the registry marks adjustable per match can be set (" +
               std::string{name} + " is " + adjustable_word(parameter.adjustable) + ")";
    }
    oa::data::mod_profile::MatchOption option;
    option.path = id + "." + std::string{name};
    option.value = raw;
    const oa::data::mod_profile::MatchOptionReading reading =
        oa::data::mod_profile::match_option_reading(option);
    if (reading.problem.has_value())
        return "hacks: " + id + ": " + *reading.problem;
    made.push_back(
        MutatorHack{
            std::string{mutator}, option.path, reading.normal.empty() ? raw : reading.normal
        }
    );
    return {};
}

/// Reads one entry of the hacks table.
std::string read_entry(
    lua_State* state,
    int index,
    std::string_view mutator,
    const std::string& id,
    std::vector<MutatorHack>& made
) {
    const int value = lua_absindex(state, index);
    const Entry* entry = registry::find_entry(id);
    if (entry == nullptr) {
        oa::data::mod_profile::MatchOption option;
        option.path = id;
        option.value = "0";
        const std::optional<std::string> problem =
            oa::data::mod_profile::match_option_problem(option);
        return "hacks: " + id + ": " +
               (problem.has_value() ? *problem : std::string{"unknown parameter"});
    }
    if (!entry->mutator) {
        return "hacks: " + id +
               ": a mutator cannot set this hack (the registry does not open it to mutators)";
    }
    if (lua_istable(state, value) != 0) {
        if (lua_getmetatable(state, value) != 0) {
            lua_pop(state, 1);
            return "hacks: " + id + ": a parameter table must be plain, not one with a metatable";
        }
        lua_pushnil(state);
        while (lua_next(state, value) != 0) {
            if (lua_type(state, -2) != LUA_TSTRING) {
                lua_pop(state, 2);
                return "hacks: " + id + ": a parameter name must be text";
            }
            std::size_t length = 0;
            const char* key = lua_tolstring(state, -2, &length);
            const std::string name = key != nullptr ? std::string{key, length} : std::string{};
            const std::optional<std::string> raw = scalar_text(state, -1);
            if (!raw.has_value()) {
                lua_pop(state, 2);
                return "hacks: " + id + ": a parameter value must be a scalar";
            }
            const int parameter = registry::find_parameter(*entry, name);
            std::string problem;
            if (parameter < 0) {
                oa::data::mod_profile::MatchOption option;
                option.path = id + "." + name;
                option.value = *raw;
                const std::optional<std::string> refused =
                    oa::data::mod_profile::match_option_problem(option);
                problem = "hacks: " + id + ": " +
                          (refused.has_value() ? *refused : std::string{"unknown parameter"});
            } else {
                const Parameter& spec =
                    registry::parameters_of(*entry)[static_cast<std::size_t>(parameter)];
                problem = take_parameter(mutator, id, name, *raw, spec, made);
            }
            if (!problem.empty()) {
                lua_pop(state, 2);
                return problem;
            }
            lua_pop(state, 1);
        }
        return {};
    }
    const std::optional<std::string> raw = scalar_text(state, value);
    if (!raw.has_value())
        return "hacks: " + id + ": a parameter value must be a scalar";
    const int target = scalar_parameter(*entry);
    if (target < 0) {
        if (lua_isboolean(state, value) != 0)
            return "hacks: " + id + ": a mutator sets a parameter, not true or false";
        return "hacks: " + id + ": the entry has no shorthand parameter";
    }
    const Parameter& parameter = registry::parameters_of(*entry)[static_cast<std::size_t>(target)];
    if (lua_isboolean(state, value) != 0 && parameter.value.type != ValueType::boolean)
        return "hacks: " + id + ": a mutator sets a parameter, not true or false";
    return take_parameter(mutator, id, parameter.name, *raw, parameter, made);
}

/// Reports whether a resolve error names this parameter or its entry.
bool names_hack(const oa::data::mod_profile::Diagnostic& diagnostic, std::string_view path) {
    if (diagnostic.path == "match " + std::string{path})
        return true;
    return diagnostic.path == entry_id(path);
}

} // namespace

std::string read_hacks(
    lua_State* state,
    int index,
    std::string_view mutator,
    bool declares_layer,
    std::vector<MutatorHack>& out
) {
    out.clear();
    const int table = lua_absindex(state, index);
    if (lua_istable(state, table) == 0)
        return "hacks: hacks must be a table";
    if (lua_getmetatable(state, table) != 0) {
        lua_pop(state, 1);
        return "hacks: hacks must be a plain table, not one with a metatable";
    }
    std::vector<MutatorHack> made;
    lua_pushnil(state);
    while (lua_next(state, table) != 0) {
        if (lua_type(state, -2) != LUA_TSTRING) {
            lua_pop(state, 2);
            out.clear();
            return "hacks: a hacks key must be text";
        }
        std::size_t length = 0;
        const char* key = lua_tolstring(state, -2, &length);
        const std::string id = key != nullptr ? std::string{key, length} : std::string{};
        if (!declares_layer) {
            lua_pop(state, 2);
            out.clear();
            return "hacks: " + id + ": the manifest does not declare the hacks layer";
        }
        const std::string problem = read_entry(state, -1, mutator, id, made);
        if (!problem.empty()) {
            lua_pop(state, 2);
            out.clear();
            return problem;
        }
        lua_pop(state, 1);
    }
    if (!declares_layer) {
        out.clear();
        return "hacks: the manifest does not declare the hacks layer";
    }
    std::sort(made.begin(), made.end(), [](const MutatorHack& left, const MutatorHack& right) {
        return left.path < right.path;
    });
    out = std::move(made);
    return {};
}

HackMerge merge_hacks(std::span<const MutatorHack> hacks) {
    struct Winner {
        MutatorHack hack;
        std::string earlier;
        bool overwritten{false};
    };

    std::vector<Winner> winners;
    for (const MutatorHack& hack : hacks) {
        Winner* found = nullptr;
        for (Winner& winner : winners) {
            if (winner.hack.path == hack.path)
                found = &winner;
        }
        if (found == nullptr) {
            winners.push_back(Winner{hack, {}, false});
            continue;
        }
        if (!found->overwritten)
            found->earlier = found->hack.mutator;
        found->overwritten = true;
        found->hack = hack;
    }
    HackMerge merged;
    for (const Winner& winner : winners) {
        merged.hacks.push_back(winner.hack);
        if (!winner.overwritten)
            continue;
        HackNote note;
        note.entry = entry_id(winner.hack.path);
        note.earlier = winner.earlier;
        note.mutator = winner.hack.mutator;
        note.text = winner.earlier + " and " + winner.hack.mutator + " both set " +
                    winner.hack.path + "; " + winner.hack.mutator + "'s value plays";
        merged.notes.push_back(std::move(note));
    }
    std::sort(
        merged.hacks.begin(),
        merged.hacks.end(),
        [](const MutatorHack& left, const MutatorHack& right) { return left.path < right.path; }
    );
    std::sort(
        merged.notes.begin(), merged.notes.end(), [](const HackNote& left, const HackNote& right) {
            return left.entry < right.entry;
        }
    );
    return merged;
}

std::vector<HackNote> developer_conflicts(
    std::span<const MutatorHack> hacks,
    std::span<const oa::data::mod_profile::HackOverride> overrides,
    std::span<const std::string> profile_on
) {
    struct Seen {
        std::string entry;
        std::string mutator;
    };

    std::vector<Seen> seen;
    for (const MutatorHack& hack : hacks) {
        const std::string entry = entry_id(hack.path);
        Seen* found = nullptr;
        for (Seen& item : seen) {
            if (item.entry == entry)
                found = &item;
        }
        if (found == nullptr)
            seen.push_back(Seen{entry, hack.mutator});
        else
            found->mutator = hack.mutator;
    }
    std::vector<HackNote> notes;
    for (const Seen& item : seen) {
        const oa::data::mod_profile::HackOverride* matched = nullptr;
        for (const oa::data::mod_profile::HackOverride& one : overrides) {
            if (one.hack == item.entry)
                matched = &one;
        }
        bool on = false;
        for (const std::string& id : profile_on) {
            if (id == item.entry)
                on = true;
        }
        HackNote note;
        note.entry = item.entry;
        note.mutator = item.mutator;
        if (matched != nullptr && !matched->on) {
            note.text = "Developer Mode turns " + item.entry + " off; " + item.mutator +
                        " turns it on for this game";
        } else if (matched != nullptr) {
            note.text = "Developer Mode also sets " + item.entry;
        } else if (!on) {
            note.text = "the mod leaves " + item.entry + " off; " + item.mutator +
                        " turns it on for this game";
        } else {
            continue;
        }
        notes.push_back(std::move(note));
    }
    return notes;
}

std::vector<HackCheckError> hack_faults(
    std::span<const oa::data::mod_profile::Diagnostic> errors, std::span<const MutatorHack> hacks
) {
    std::vector<std::string> ids;
    for (const MutatorHack& hack : hacks) {
        if (std::find(ids.begin(), ids.end(), hack.mutator) == ids.end())
            ids.push_back(hack.mutator);
    }
    std::vector<HackCheckError> faults;
    for (const std::string& id : ids) {
        bool found = false;
        for (const oa::data::mod_profile::Diagnostic& diagnostic : errors) {
            for (const MutatorHack& hack : hacks) {
                if (hack.mutator != id || !names_hack(diagnostic, hack.path))
                    continue;
                faults.push_back(
                    HackCheckError{
                        id,
                        diagnostic.path,
                        "hacks: " + entry_id(hack.path) + ": " + diagnostic.message
                    }
                );
                found = true;
                break;
            }
            if (found)
                break;
        }
    }
    if (!faults.empty())
        return faults;
    // No error names a hack, so none of them can be told apart from the rest.
    const std::string fallback =
        errors.empty() ? std::string{"the hacks were refused"} : errors.front().message;
    for (const std::string& id : ids) {
        for (const MutatorHack& hack : hacks) {
            if (hack.mutator != id)
                continue;
            const std::string entry = entry_id(hack.path);
            faults.push_back(HackCheckError{id, entry, "hacks: " + entry + ": " + fallback});
            break;
        }
    }
    return faults;
}

} // namespace oa::data::mutators
