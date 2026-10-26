// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// ctx.incompatible, shared by the pipeline and the maps hook.
// A Lua callback must not allocate or throw: the reason is copied into a
// fixed buffer, and one_line_reason runs only after the call has returned.
#pragma once

#include "oa/data/mutators/verdicts.hpp"

#include "lua.hpp"

#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>

namespace oa::data::mutators {

/// The flag a script sets by calling `ctx.incompatible`. It outlives `pcall`.
struct DeclaredStop {
    /// True once the script called `ctx.incompatible`.
    bool raised{false};
    /// The reason bytes, not yet trimmed. Not a C string the script owns.
    char reason[1024]{};
    /// How many of those bytes were copied.
    std::size_t length{0};
};

/// The reason an empty `ctx.incompatible` is stored as.
inline constexpr std::string_view incompatible_default_reason = "incompatible with this mod";

extern "C" {

/// Lua closure: copies the reason, then raises so a normal script stops.
inline int incompatible_callback(lua_State* state) {
    auto* stop = static_cast<DeclaredStop*>(lua_touserdata(state, lua_upvalueindex(1)));
    if (stop != nullptr && !stop->raised) {
        stop->raised = true;
        stop->length = 0;
        stop->reason[0] = '\0';
        if (lua_type(state, 1) == LUA_TSTRING) {
            std::size_t length = 0;
            const char* text = lua_tolstring(state, 1, &length);
            if (text != nullptr && length > 0) {
                constexpr std::size_t cap = sizeof(DeclaredStop::reason) - 1;
                if (length > cap)
                    length = cap;
                std::memcpy(stop->reason, text, length);
                stop->reason[length] = '\0';
                stop->length = length;
            }
        }
    }
    lua_pushliteral(state, "incompatible");
    return lua_error(state);
}

} // extern "C"

/// Installs `ctx.incompatible` on the table at `ctx`. `stop` outlives the call.
inline void set_incompatible(lua_State* state, int ctx, DeclaredStop* stop) {
    lua_pushlightuserdata(state, stop);
    lua_pushcclosure(state, incompatible_callback, 1);
    lua_setfield(state, ctx, "incompatible");
}

/// The stored reason: one line, or the default when the script gave none.
inline std::string declared_reason(const DeclaredStop& stop) {
    const std::string line = one_line_reason(std::string_view(stop.reason, stop.length));
    if (line.empty())
        return std::string(incompatible_default_reason);
    return line;
}

} // namespace oa::data::mutators
