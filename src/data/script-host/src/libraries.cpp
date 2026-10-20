// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The libraries a script may use, and the replacements that keep them inside
// the sandbox. `math` is the engine's library. `pairs` walks keys in a fixed
// order.

#include "internal.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <string>

namespace oa::data::script_host {
namespace {

using Control = HostDetail::Control;

const char string_rep_key = 0;
const char setmetatable_key = 0;
const char rawset_key = 0;
const char xpcall_key = 0;

static_assert(string_rep_limit == 1048576);
static_assert(max_source_bytes == 1048576);

std::string omission_note(std::size_t lines) {
    if (lines == 1)
        return "1 line left out";
    return std::to_string(lines) + " lines left out";
}

void pop_printed_line(Control& control) {
    control.printed_bytes -= control.printed.back().size();
    control.printed.pop_back();
}

void place_omission(Control& control) {
    if (control.omission_stored && !control.printed.empty()) {
        pop_printed_line(control);
        control.omission_stored = false;
    }
    for (;;) {
        std::string note = omission_note(control.lines_left_out == 0 ? 1 : control.lines_left_out);
        const bool lines_ok = control.printed.size() + 1 <= max_printed_lines;
        const bool bytes_ok = note.size() <= max_printed_bytes &&
                              control.printed_bytes <= max_printed_bytes - note.size();
        if (control.printed.empty() || (lines_ok && bytes_ok)) {
            control.printed_bytes += note.size();
            control.printed.push_back(std::move(note));
            control.omission_stored = true;
            return;
        }
        pop_printed_line(control);
        ++control.lines_left_out;
    }
}

bool retain_printed_line(Control& control, const char* text, std::size_t length) {
    if (text == nullptr) {
        text = "";
        length = 0;
    }
    if (control.print_closed) {
        ++control.lines_left_out;
        place_omission(control);
        return true;
    }
    if (control.printed_bytes <= max_printed_bytes && control.printed.size() < max_printed_lines &&
        length <= max_printed_bytes - control.printed_bytes) {
        control.printed.emplace_back(text, length);
        control.printed_bytes += length;
        return true;
    }
    control.print_closed = true;
    control.lines_left_out = 1;
    place_omission(control);
    return true;
}

bool keep_printed_line(Control& control, const char* text, std::size_t length) {
    try {
        return retain_printed_line(control, text, length);
    } catch (const std::bad_alloc&) {
        return false;
    }
}

// True when `count` copies of `length` plus the separators between them fit in
// a uint64. `total` is that size. An empty result is 0.
bool rep_total(uint64_t count, std::size_t length, std::size_t separator_length, uint64_t* total) {
    const auto bytes = static_cast<uint64_t>(length);
    const auto separator = static_cast<uint64_t>(separator_length);
    if (bytes != 0 && count > std::numeric_limits<uint64_t>::max() / bytes)
        return false;
    uint64_t size = count * bytes;
    const uint64_t gaps = count - 1;
    if (separator != 0 && gaps > std::numeric_limits<uint64_t>::max() / separator)
        return false;
    const uint64_t gap_bytes = gaps * separator;
    if (size > std::numeric_limits<uint64_t>::max() - gap_bytes)
        return false;
    *total = size + gap_bytes;
    return true;
}

extern "C" {
static int host_print(lua_State* state) {
    const int count = lua_gettop(state);
    luaL_Buffer buffer;
    luaL_buffinit(state, &buffer);
    for (int index = 1; index <= count; ++index) {
        if (index > 1)
            luaL_addchar(&buffer, '\t');
        // luaL_addvalue is the buffer call that takes the value on top.
        // Popping that slot by hand frees the buffer once it grows.
        luaL_tolstring(state, index, nullptr);
        luaL_addvalue(&buffer);
    }
    luaL_pushresult(&buffer);
    std::size_t length = 0;
    const char* text = lua_tolstring(state, -1, &length);
    Control* control = control_from(state);
    if (!keep_printed_line(*control, text, length)) {
        control->allocation_refused = true;
        return luaL_error(state, "not enough memory");
    }
    return 0;
}

static int rep_refused(lua_State* state, uint64_t bytes) {
    char number[32];
    append_u64(number, number + sizeof number, bytes);
    char message[128];
    char* cursor = message;
    char* end = message + sizeof message;
    cursor = append_text(cursor, end, "string.rep would make a string of ");
    cursor = append_text(cursor, end, number);
    append_text(cursor, end, " bytes; the most is 1048576");
    return luaL_error(state, "%s", message);
}

static int host_string_rep(lua_State* state) {
    std::size_t length = 0;
    const char* text = luaL_checklstring(state, 1, &length);
    const lua_Integer copies = luaL_checkinteger(state, 2);
    std::size_t separator_length = 0;
    const char* separator = luaL_optlstring(state, 3, "", &separator_length);

    // Lua's own rep loops `copies` times even when every copy is empty.
    if (copies <= 0 || (length == 0 && separator_length == 0)) {
        lua_pushliteral(state, "");
        return 1;
    }

    const auto count = static_cast<uint64_t>(copies);
    const auto limit = static_cast<uint64_t>(string_rep_limit);
    uint64_t total = 0;
    const bool fits = rep_total(count, length, separator_length, &total);
    if (fits && total == 0) {
        lua_pushliteral(state, "");
        return 1;
    }
    if (!fits || total > limit) {
        // A count past the cap must not reach Lua: its size check misses an
        // empty copy and then loops. A size that does not fit is past the cap.
        if (!fits)
            return rep_refused(state, std::numeric_limits<uint64_t>::max());
        return rep_refused(state, total);
    }
    if (count > limit) {
        // The result is within the cap (an empty text plus a short separator)
        // but the count is not. Build it here instead of calling Lua.
        luaL_Buffer built;
        char* cursor = luaL_buffinitsize(state, &built, static_cast<std::size_t>(total));
        for (uint64_t gap = 1; gap < count; ++gap) {
            if (separator_length > 0) {
                std::memcpy(cursor, separator, separator_length);
                cursor += separator_length;
            }
        }
        if (length > 0)
            std::memcpy(cursor, text, length);
        luaL_pushresultsize(&built, static_cast<std::size_t>(total));
        return 1;
    }

    lua_rawgetp(state, LUA_REGISTRYINDEX, &string_rep_key);
    lua_insert(state, 1);
    const int arguments = lua_gettop(state) - 1;
    lua_call(state, arguments, 1);
    return 1;
}

static int host_setmetatable(lua_State* state) {
    const int metatable_type = lua_type(state, 2);
    luaL_checktype(state, 1, LUA_TTABLE);
    luaL_argexpected(
        state, metatable_type == LUA_TNIL || metatable_type == LUA_TTABLE, 2, "nil or table"
    );
    if (metatable_type == LUA_TTABLE) {
        lua_pushliteral(state, "__gc");
        lua_rawget(state, 2);
        if (!lua_isnil(state, -1))
            return luaL_error(state, "setmetatable: a metatable with __gc is refused");
        lua_pop(state, 1);
        lua_pushliteral(state, "__mode");
        lua_rawget(state, 2);
        if (!lua_isnil(state, -1))
            return luaL_error(state, "setmetatable: a metatable with __mode is refused");
        lua_pop(state, 1);
    }
    lua_rawgetp(state, LUA_REGISTRYINDEX, &setmetatable_key);
    lua_pushvalue(state, 1);
    lua_pushvalue(state, 2);
    lua_call(state, 2, 1);
    return 1;
}

static int host_rawset(lua_State* state) {
    luaL_checktype(state, 1, LUA_TTABLE);
    luaL_checkany(state, 2);
    luaL_checkany(state, 3);
    if (lua_type(state, 2) == LUA_TSTRING) {
        std::size_t length = 0;
        const char* key = lua_tolstring(state, 2, &length);
        if (length == 4 && std::memcmp(key, "__gc", 4) == 0)
            return luaL_error(state, "rawset: the key __gc is refused");
        if (length == 6 && std::memcmp(key, "__mode", 6) == 0)
            return luaL_error(state, "rawset: the key __mode is refused");
    }
    lua_rawgetp(state, LUA_REGISTRYINDEX, &rawset_key);
    lua_pushvalue(state, 1);
    lua_pushvalue(state, 2);
    lua_pushvalue(state, 3);
    lua_call(state, 3, 1);
    return 1;
}

// Once the allowance is spent, or the sandbox has failed, the script's handler
// must not run: a looping handler would never return the error to the host.
static int host_xpcall_handler(lua_State* state) {
    const Control* control = control_from(state);
    if (control->allowance_spent || control->sandbox_failed)
        return 1;
    const int count = lua_gettop(state);
    lua_pushvalue(state, lua_upvalueindex(1));
    lua_insert(state, 1);
    lua_call(state, count, LUA_MULTRET);
    return lua_gettop(state);
}

static int host_xpcall(lua_State* state) {
    luaL_checktype(state, 2, LUA_TFUNCTION);
    lua_pushvalue(state, 2);
    lua_pushcclosure(state, host_xpcall_handler, 1);
    lua_replace(state, 2);
    lua_rawgetp(state, LUA_REGISTRYINDEX, &xpcall_key);
    lua_insert(state, 1);
    const int arguments = lua_gettop(state) - 1;
    lua_call(state, arguments, LUA_MULTRET);
    return lua_gettop(state);
}
} // extern "C"

} // namespace

void open_libraries(lua_State* state) {
    luaL_requiref(state, "_G", luaopen_base, 1);
    lua_pop(state, 1);
    luaL_requiref(state, "string", luaopen_string, 1);
    lua_pop(state, 1);
    luaL_requiref(state, "table", luaopen_table, 1);
    lua_pop(state, 1);
    luaL_requiref(state, "utf8", luaopen_utf8, 1);
    lua_pop(state, 1);
    open_math_library(state);

    const char* const removed[] = {"load", "loadfile", "dofile", "collectgarbage"};
    for (const char* name : removed) {
        lua_pushnil(state);
        lua_setglobal(state, name);
    }

    lua_getglobal(state, "string");
    lua_getfield(state, -1, "rep");
    lua_rawsetp(state, LUA_REGISTRYINDEX, &string_rep_key);
    lua_pushnil(state);
    lua_setfield(state, -2, "dump");
    lua_pushcfunction(state, host_string_rep);
    lua_setfield(state, -2, "rep");
    // The string metatable's __index is this table, so ("x"):find uses these.
    install_patterns(state);
    lua_pop(state, 1);

    lua_getglobal(state, "setmetatable");
    lua_rawsetp(state, LUA_REGISTRYINDEX, &setmetatable_key);
    lua_pushcfunction(state, host_setmetatable);
    lua_setglobal(state, "setmetatable");

    lua_getglobal(state, "rawset");
    lua_rawsetp(state, LUA_REGISTRYINDEX, &rawset_key);
    lua_pushcfunction(state, host_rawset);
    lua_setglobal(state, "rawset");

    lua_pushcfunction(state, host_print);
    lua_setglobal(state, "print");

    install_pairs(state);

    lua_getglobal(state, "xpcall");
    lua_rawsetp(state, LUA_REGISTRYINDEX, &xpcall_key);
    lua_pushcfunction(state, host_xpcall);
    lua_setglobal(state, "xpcall");

    install_require(state);
}

} // namespace oa::data::script_host
