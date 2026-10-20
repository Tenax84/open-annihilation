// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The libraries a script may use, and the replacements that keep them inside
// the sandbox. The maths library is left closed; a later change adds it.

#include "internal.hpp"

#include <climits>
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

extern "C" {
static int host_print(lua_State* state) {
    const int count = lua_gettop(state);
    luaL_Buffer buffer;
    luaL_buffinit(state, &buffer);
    for (int index = 1; index <= count; ++index) {
        if (index > 1)
            luaL_addchar(&buffer, '\t');
        std::size_t length = 0;
        const char* text = luaL_tolstring(state, index, &length);
        luaL_addlstring(&buffer, text, length);
        lua_pop(state, 1);
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

static int host_string_rep(lua_State* state) {
    std::size_t length = 0;
    luaL_checklstring(state, 1, &length);
    const lua_Integer copies = luaL_checkinteger(state, 2);
    std::size_t separator_length = 0;
    luaL_optlstring(state, 3, "", &separator_length);

    bool too_large_for_lua = false;
    bool over_limit = false;
    std::size_t total = 0;
    if (copies > 0) {
        const auto count64 = static_cast<uint64_t>(copies);
        const auto size_max = static_cast<uint64_t>(std::numeric_limits<std::size_t>::max());
        if (count64 > size_max ||
            separator_length > std::numeric_limits<std::size_t>::max() - length) {
            too_large_for_lua = true;
        } else {
            const std::size_t count = static_cast<std::size_t>(count64);
            const std::size_t unit = length + separator_length;
            constexpr std::size_t lua_max = static_cast<std::size_t>(INT_MAX);
            if (unit > 0 && (count > lua_max || unit > lua_max / count)) {
                too_large_for_lua = true;
            } else {
                total = count * unit - separator_length;
                over_limit = total > string_rep_limit;
            }
        }
    }
    if (!too_large_for_lua && over_limit) {
        char number[32];
        append_u64(number, number + sizeof number, static_cast<uint64_t>(total));
        char message[128];
        char* cursor = message;
        char* end = message + sizeof message;
        cursor = append_text(cursor, end, "string.rep would make a string of ");
        cursor = append_text(cursor, end, number);
        append_text(cursor, end, " bytes; the most is 1048576");
        return luaL_error(state, "%s", message);
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

    install_require(state);
}

} // namespace oa::data::script_host
