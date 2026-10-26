// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The libraries a script may use, and the replacements that keep them inside
// the sandbox. `math` is the engine's library. `pairs` walks keys in a fixed
// order.
//
// next, rawequal, tonumber, warn and setmetatable, string.byte, the table
// functions move, insert, remove, concat, pack, unpack and sort, and
// utf8.char, utf8.len, utf8.codepoint, utf8.offset and utf8.codes follow
// lbaselib.c, lstrlib.c, ltablib.c and lutf8lib.c of Lua 5.4.9.
// Copyright (C) 1994-2026 Lua.org, PUC-Rio.
// The licence is the MIT licence; the text is licenses/lua-LICENSE.txt.
// Each copy counts its work against the phase allowance and refuses a table
// range no table can hold. Being called from the script itself, each one
// names itself and the script's line in its errors as Lua's own does.
// string.char stays Lua's own: the string cap refuses a result that would
// pass it.

#include "internal.hpp"

#include <cctype>
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
const char rawset_key = 0;
const char metatable_guard_key = 0;
const char xpcall_key = 0;

static_assert(string_length_limit == 65536);
static_assert(string_rep_limit == string_length_limit);
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

// string.byte positions, from lstrlib.c. A negative index counts back from
// the end. The initial position clips to at least 1; the final one clips to
// the closed interval from 0 through the length.
std::size_t byte_position(lua_Integer pos, std::size_t length) {
    if (pos > 0)
        return static_cast<std::size_t>(pos);
    if (pos == 0)
        return 1;
    if (pos < -static_cast<lua_Integer>(length))
        return 1;
    return length + static_cast<std::size_t>(pos) + 1;
}

std::size_t byte_end(lua_State* state, int argument, lua_Integer fallback, std::size_t length) {
    const lua_Integer pos = luaL_optinteger(state, argument, fallback);
    if (pos > static_cast<lua_Integer>(length))
        return length;
    if (pos >= 0)
        return static_cast<std::size_t>(pos);
    if (pos < -static_cast<lua_Integer>(length))
        return 0;
    return length + static_cast<std::size_t>(pos) + 1;
}

constexpr int tab_read = 1;
constexpr int tab_write = 2;
constexpr int tab_length = 4;

constexpr const char* concat_limit_message =
    "table.concat would make a string of more than 65536 bytes";

// A table call asked for more elements than the memory cap could hold.
// luaL_error does not return; the attribute is omitted because this build
// does not see that on the Lua declaration.
void refuse_range(lua_State* state, const char* function, uint64_t count) {
    char number[32];
    char limit_text[32];
    append_u64(number, number + sizeof number, count);
    append_u64(
        limit_text, limit_text + sizeof limit_text, static_cast<uint64_t>(table_range_limit)
    );
    char message[160];
    char* cursor = message;
    char* end = message + sizeof message;
    cursor = append_text(cursor, end, function);
    cursor = append_text(cursor, end, ": a range of ");
    cursor = append_text(cursor, end, number);
    cursor = append_text(cursor, end, " elements is past the most of ");
    append_text(cursor, end, limit_text);
    luaL_error(state, "%s", message);
}

int check_field(lua_State* state, const char* key, int depth) {
    lua_pushstring(state, key);
    return lua_rawget(state, -depth) != LUA_TNIL;
}

// A value is usable when it is a table or its metatable has the operations
// `what` names. Anything else raises Lua's own "table expected" error.
void check_table(lua_State* state, int argument, int what) {
    if (lua_type(state, argument) != LUA_TTABLE) {
        int depth = 1;
        if (lua_getmetatable(state, argument) &&
            (!(what & tab_read) || check_field(state, "__index", ++depth)) &&
            (!(what & tab_write) || check_field(state, "__newindex", ++depth)) &&
            (!(what & tab_length) || check_field(state, "__len", ++depth))) {
            lua_pop(state, depth);
        } else {
            luaL_checktype(state, argument, LUA_TTABLE);
        }
    }
}

lua_Integer table_length(lua_State* state, int argument, int what) {
    check_table(state, argument, what | tab_length);
    return luaL_len(state, argument);
}

void add_concat_field(lua_State* state, luaL_Buffer* buffer, lua_Integer index) {
    lua_geti(state, 1, index);
    if (!lua_isstring(state, -1)) {
        luaL_error(
            state,
            "invalid value (%s) at index %I in table for 'concat'",
            luaL_typename(state, -1),
            static_cast<lua_Integer>(index)
        );
    }
    std::size_t field_length = 0;
    lua_tolstring(state, -1, &field_length);
    reject_long_piece(state, luaL_bufflen(buffer), field_length, concat_limit_message);
    luaL_addvalue(buffer);
}

// utf8, from lutf8lib.c. The largest code point a lax decode accepts.
constexpr unsigned utf8_max_code = 0x7FFFFFFFu;
constexpr unsigned utf8_max_unicode = 0x10FFFFu;
constexpr const char* utf8_invalid = "invalid UTF-8 code";

bool utf8_continuation(unsigned char byte) {
    return (byte & 0xC0u) == 0x80u;
}

lua_Integer utf8_position(lua_Integer pos, std::size_t length) {
    if (pos >= 0)
        return pos;
    if ((std::size_t{0} - static_cast<std::size_t>(pos)) > length)
        return 0;
    return static_cast<lua_Integer>(length) + pos + 1;
}

// Decodes one sequence. Returns null when the bytes are not well formed.
// `strict` also rejects values past U+10FFFF and the surrogate range.
const char* utf8_decode(const char* text, unsigned* value, bool strict) {
    static const unsigned limits[] = {~0u, 0x80u, 0x800u, 0x10000u, 0x200000u, 0x4000000u};
    unsigned lead = static_cast<unsigned char>(text[0]);
    unsigned decoded = 0;
    if (lead < 0x80u) {
        decoded = lead;
    } else if (lead >= 0xFEu) {
        return nullptr;
    } else {
        int count = 0;
        for (; (lead & 0x40u) != 0; lead <<= 1) {
            const unsigned continuation = static_cast<unsigned char>(text[++count]);
            if (!utf8_continuation(static_cast<unsigned char>(continuation)))
                return nullptr;
            decoded = (decoded << 6) | (continuation & 0x3Fu);
        }
        decoded |= (lead & 0x7Fu) << (count * 5);
        if (decoded > utf8_max_code || decoded < limits[count])
            return nullptr;
        text += count;
    }
    if (strict) {
        if (decoded > utf8_max_unicode || (0xD800u <= decoded && decoded <= 0xDFFFu))
            return nullptr;
    }
    if (value != nullptr)
        *value = decoded;
    return text + 1;
}

// utf8.char's one code point, from lutf8lib.c.
void push_utf8_char(lua_State* state, int argument) {
    const auto code = static_cast<lua_Unsigned>(luaL_checkinteger(state, argument));
    luaL_argcheck(state, code <= utf8_max_code, argument, "value out of range");
    lua_pushfstring(state, "%U", static_cast<long>(code));
}

constexpr const char* space_characters = " \f\n\r\t\v";

// tonumber's reading of an integer in `base`, from lbaselib.c. Returns where
// the reading stopped, after trailing spaces, or null when the text is not a
// numeral in that base.
const char* text_to_integer(const char* text, int base, lua_Integer* value) {
    lua_Unsigned number = 0;
    bool negative = false;
    text += std::strspn(text, space_characters);
    if (*text == '-') {
        ++text;
        negative = true;
    } else if (*text == '+') {
        ++text;
    }
    if (!std::isalnum(static_cast<unsigned char>(*text)))
        return nullptr;
    do {
        const auto byte = static_cast<unsigned char>(*text);
        const int digit = std::isdigit(byte) ? *text - '0' : (std::toupper(byte) - 'A') + 10;
        if (digit >= base)
            return nullptr;
        number = number * static_cast<lua_Unsigned>(base) + static_cast<lua_Unsigned>(digit);
        ++text;
    } while (std::isalnum(static_cast<unsigned char>(*text)));
    text += std::strspn(text, space_characters);
    *value = static_cast<lua_Integer>(negative ? (0u - number) : number);
    return text;
}

// table.sort, from ltablib.c. The pivot salt starts at 0, so the first
// partitions use the middle element. After an unbalanced cut it becomes
// ~0u, the same fixed value the interpreter uses instead of the clock.
using SortIndex = unsigned int;
constexpr unsigned sort_random_limit = 100u;

void set_pair(lua_State* state, SortIndex first, SortIndex second) {
    lua_seti(state, 1, static_cast<lua_Integer>(first));
    lua_seti(state, 1, static_cast<lua_Integer>(second));
}

int sort_less(lua_State* state, int left, int right) {
    // Charged before the comparison, including when the comparator is C.
    charge(state, comparison_charge(state, left, right));
    if (lua_isnil(state, 2))
        return lua_compare(state, left, right, LUA_OPLT);
    lua_pushvalue(state, 2);
    lua_pushvalue(state, left - 1);
    lua_pushvalue(state, right - 2);
    lua_call(state, 2, 1);
    const int result = lua_toboolean(state, -1);
    lua_pop(state, 1);
    return result;
}

SortIndex sort_partition(lua_State* state, SortIndex lo, SortIndex up) {
    SortIndex low_index = lo;
    SortIndex high_index = up - 1;
    for (;;) {
        for (;;) {
            lua_geti(state, 1, static_cast<lua_Integer>(++low_index));
            if (!sort_less(state, -1, -2))
                break;
            if (low_index == up - 1)
                luaL_error(state, "invalid order function for sorting");
            lua_pop(state, 1);
        }
        for (;;) {
            lua_geti(state, 1, static_cast<lua_Integer>(--high_index));
            if (!sort_less(state, -3, -1))
                break;
            if (high_index < low_index)
                luaL_error(state, "invalid order function for sorting");
            lua_pop(state, 1);
        }
        if (high_index < low_index) {
            lua_pop(state, 1);
            set_pair(state, up - 1, low_index);
            return low_index;
        }
        set_pair(state, low_index, high_index);
    }
}

SortIndex choose_pivot(SortIndex lo, SortIndex up, unsigned salt) {
    const SortIndex quarter = (up - lo) / 4;
    return salt % (quarter * 2) + (lo + quarter);
}

void aux_sort(lua_State* state, SortIndex lo, SortIndex up, unsigned salt) {
    while (lo < up) {
        lua_geti(state, 1, static_cast<lua_Integer>(lo));
        lua_geti(state, 1, static_cast<lua_Integer>(up));
        if (sort_less(state, -1, -2))
            set_pair(state, lo, up);
        else
            lua_pop(state, 2);
        if (up - lo == 1)
            return;
        SortIndex pivot = 0;
        if (up - lo < sort_random_limit || salt == 0)
            pivot = (lo + up) / 2;
        else
            pivot = choose_pivot(lo, up, salt);
        lua_geti(state, 1, static_cast<lua_Integer>(pivot));
        lua_geti(state, 1, static_cast<lua_Integer>(lo));
        if (sort_less(state, -2, -1))
            set_pair(state, pivot, lo);
        else {
            lua_pop(state, 1);
            lua_geti(state, 1, static_cast<lua_Integer>(up));
            if (sort_less(state, -1, -2))
                set_pair(state, pivot, up);
            else
                lua_pop(state, 2);
        }
        if (up - lo == 2)
            return;
        lua_geti(state, 1, static_cast<lua_Integer>(pivot));
        lua_pushvalue(state, -1);
        lua_geti(state, 1, static_cast<lua_Integer>(up - 1));
        set_pair(state, pivot, up - 1);
        pivot = sort_partition(state, lo, up);
        SortIndex smaller = 0;
        if (pivot - lo < up - pivot) {
            aux_sort(state, lo, pivot - 1, salt);
            smaller = pivot - lo;
            lo = pivot + 1;
        } else {
            aux_sort(state, pivot + 1, up, salt);
            smaller = up - pivot;
            up = pivot - 1;
        }
        if ((up - lo) / 128 > smaller)
            salt = ~0u;
    }
}

/// Replaces the metatable at `index` with a copy whose own metatable refuses
/// `__mode`.
///
/// The caller's table stays theirs, so a later write to it, including one
/// through a table shared by several values, cannot make this value weak.
/// `__index` still points at the table it pointed at. The copy costs
/// metatable_copy_charge, and each field copied one unit, so a class set on
/// thousands of values stays cheap. No object with a destructor may be alive
/// across this call.
///
/// @param state the sandbox state
/// @param index stack index of the caller's metatable, a table
void replace_with_guarded_copy(lua_State* state, int index) {
    index = lua_absindex(state, index);
    charge(state, metatable_copy_charge);
    lua_newtable(state);
    const int copy = lua_gettop(state);
    lua_pushnil(state);
    while (lua_next(state, index) != 0) {
        charge(state, 1);
        lua_pushvalue(state, -2);
        lua_insert(state, -2);
        lua_rawset(state, copy);
    }
    lua_rawgetp(state, LUA_REGISTRYINDEX, &metatable_guard_key);
    lua_setmetatable(state, copy);
    lua_replace(state, index);
}

extern "C" {
static int host_print(lua_State* state) {
    const int count = lua_gettop(state);
    // One past the kept line, so a long value is cut before it becomes a string.
    constexpr std::size_t ceiling = max_printed_bytes + 1;
    luaL_Buffer buffer;
    luaL_buffinit(state, &buffer);
    std::size_t kept = 0;
    for (int index = 1; index <= count && kept < ceiling; ++index) {
        if (index > 1) {
            luaL_addchar(&buffer, '\t');
            ++kept;
        }
        luaL_tolstring(state, index, nullptr);
        std::size_t length = 0;
        const char* text = lua_tolstring(state, -1, &length);
        const std::size_t room = ceiling - kept;
        const std::size_t take = length < room ? length : room;
        // The text sits above the buffer. addlstring would treat that slot
        // as the buffer's box the first time the line grows past its first
        // block, and the pop would free the box the buffer still uses.
        if (take == length) {
            luaL_addvalue(&buffer);
        } else {
            lua_pushlstring(state, text != nullptr ? text : "", take);
            lua_remove(state, -2);
            luaL_addvalue(&buffer);
        }
        kept += take;
    }
    luaL_pushresult(&buffer);
    std::size_t length = 0;
    const char* text = lua_tolstring(state, -1, &length);
    if (length > max_printed_bytes)
        length = max_printed_bytes;
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
    append_text(cursor, end, " bytes; the most is 65536");
    raise_string_limit(state, bytes, message);
    return 0;
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

// The installed metatable's own metatable. Writing __mode would make the
// table weak, and a weak table drops entries at different times on each machine.
static int host_mode_newindex(lua_State* state) {
    luaL_checktype(state, 1, LUA_TTABLE);
    if (lua_type(state, 2) == LUA_TSTRING) {
        std::size_t length = 0;
        const char* key = lua_tolstring(state, 2, &length);
        if (length == 6 && std::memcmp(key, "__mode", 6) == 0)
            return luaL_error(state, "the key __mode is refused");
    }
    lua_rawgetp(state, LUA_REGISTRYINDEX, &rawset_key);
    lua_pushvalue(state, 1);
    lua_pushvalue(state, 2);
    lua_pushvalue(state, 3);
    lua_call(state, 3, 0);
    return 0;
}

// setmetatable, from lbaselib.c. The __gc and __mode refusals sit in this
// function, and so does the protected-metatable check, so each error names
// the script's chunk and line as Lua's own does. The check comes before the
// metatable is replaced with its guarded copy, in Lua's own order, so a
// protected table is refused before any copy is made or charged.
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
    if (luaL_getmetafield(state, 1, "__metatable") != LUA_TNIL)
        return luaL_error(state, "cannot change a protected metatable");
    if (metatable_type == LUA_TTABLE)
        replace_with_guarded_copy(state, 2);
    lua_settop(state, 2);
    lua_setmetatable(state, 1);
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

// Once the allowance is spent, the real-time limit is past, or the sandbox
// has failed, the script's handler must not run: a looping handler would
// never return the error to the host.
static int host_xpcall_handler(lua_State* state) {
    const Control* control = control_from(state);
    if (control->string_refused || control->allowance_spent || control->time_expired ||
        control->sandbox_failed)
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

static int host_string_byte(lua_State* state) {
    std::size_t length = 0;
    const char* text = luaL_checklstring(state, 1, &length);
    const lua_Integer first = luaL_optinteger(state, 2, 1);
    const std::size_t start = byte_position(first, length);
    const std::size_t finish = byte_end(state, 3, first, length);
    if (start > finish)
        return 0;
    if (finish - start >= static_cast<std::size_t>(INT_MAX))
        return luaL_error(state, "string slice too long");
    const int count = static_cast<int>(finish - start) + 1;
    luaL_checkstack(state, count, "string slice too long");
    charge(state, static_cast<uint64_t>(count));
    for (int index = 0; index < count; ++index) {
        const auto byte =
            static_cast<unsigned char>(text[start + static_cast<std::size_t>(index) - 1]);
        lua_pushinteger(state, static_cast<lua_Integer>(byte));
    }
    return count;
}

static int host_table_insert(lua_State* state) {
    lua_Integer hole = table_length(state, 1, tab_read | tab_write);
    hole = luaL_intop(+, hole, 1);
    lua_Integer pos = 0;
    switch (lua_gettop(state)) {
    case 2:
        pos = hole;
        break;
    case 3: {
        pos = luaL_checkinteger(state, 2);
        luaL_argcheck(
            state,
            static_cast<lua_Unsigned>(pos) - 1u < static_cast<lua_Unsigned>(hole),
            2,
            "position out of bounds"
        );
        if (hole > pos) {
            const lua_Unsigned shifted =
                static_cast<lua_Unsigned>(hole) - static_cast<lua_Unsigned>(pos);
            if (shifted > table_range_limit)
                refuse_range(state, "table.insert", static_cast<uint64_t>(shifted));
            charge(state, static_cast<uint64_t>(shifted));
            for (lua_Integer index = hole; index > pos; --index) {
                lua_geti(state, 1, index - 1);
                lua_seti(state, 1, index);
            }
        }
        break;
    }
    default:
        return luaL_error(state, "wrong number of arguments to 'insert'");
    }
    lua_seti(state, 1, pos);
    return 0;
}

static int host_table_remove(lua_State* state) {
    const lua_Integer size = table_length(state, 1, tab_read | tab_write);
    lua_Integer pos = luaL_optinteger(state, 2, size);
    if (pos != size) {
        luaL_argcheck(
            state,
            static_cast<lua_Unsigned>(pos) - 1u <= static_cast<lua_Unsigned>(size),
            2,
            "position out of bounds"
        );
    }
    lua_Unsigned shifted = 0;
    if (pos < size)
        shifted = static_cast<lua_Unsigned>(size) - static_cast<lua_Unsigned>(pos);
    if (shifted > table_range_limit)
        refuse_range(state, "table.remove", static_cast<uint64_t>(shifted));
    lua_geti(state, 1, pos);
    if (shifted > 0)
        charge(state, static_cast<uint64_t>(shifted));
    lua_Integer cursor = pos;
    for (; cursor < size; ++cursor) {
        lua_geti(state, 1, cursor + 1);
        lua_seti(state, 1, cursor);
    }
    lua_pushnil(state);
    lua_seti(state, 1, cursor);
    return 1;
}

static int host_table_move(lua_State* state) {
    const lua_Integer from = luaL_checkinteger(state, 2);
    const lua_Integer last = luaL_checkinteger(state, 3);
    const lua_Integer destination = luaL_checkinteger(state, 4);
    const int target = !lua_isnoneornil(state, 5) ? 5 : 1;
    check_table(state, 1, tab_read);
    check_table(state, target, tab_write);
    if (last >= from) {
        luaL_argcheck(
            state, from > 0 || last < LUA_MAXINTEGER + from, 3, "too many elements to move"
        );
        const lua_Integer count = last - from + 1;
        luaL_argcheck(
            state, destination <= LUA_MAXINTEGER - count + 1, 4, "destination wrap around"
        );
        if (static_cast<lua_Unsigned>(count) > table_range_limit)
            refuse_range(state, "table.move", static_cast<uint64_t>(count));
        charge(state, static_cast<uint64_t>(count));
        const bool forwards = destination > last || destination <= from ||
                              (target != 1 && !lua_compare(state, 1, target, LUA_OPEQ));
        if (forwards) {
            for (lua_Integer index = 0; index < count; ++index) {
                lua_geti(state, 1, from + index);
                lua_seti(state, target, destination + index);
            }
        } else {
            for (lua_Integer index = count - 1; index >= 0; --index) {
                lua_geti(state, 1, from + index);
                lua_seti(state, target, destination + index);
            }
        }
    }
    lua_pushvalue(state, target);
    return 1;
}

static int host_table_concat(lua_State* state) {
    lua_Integer last = table_length(state, 1, tab_read);
    std::size_t separator_length = 0;
    const char* separator = luaL_optlstring(state, 2, "", &separator_length);
    lua_Integer start = luaL_optinteger(state, 3, 1);
    last = luaL_optinteger(state, 4, last);
    uint64_t fields = 0;
    if (start <= last) {
        const lua_Unsigned span =
            static_cast<lua_Unsigned>(last) - static_cast<lua_Unsigned>(start);
        if (span >= table_range_limit)
            refuse_range(state, "table.concat", static_cast<uint64_t>(span) + 1);
        fields = static_cast<uint64_t>(span) + 1;
    }
    if (fields > 0)
        charge(state, fields);
    luaL_Buffer buffer;
    luaL_buffinit(state, &buffer);
    for (; start < last; ++start) {
        add_concat_field(state, &buffer, start);
        reject_long_piece(state, luaL_bufflen(&buffer), separator_length, concat_limit_message);
        luaL_addlstring(&buffer, separator, separator_length);
    }
    if (start == last)
        add_concat_field(state, &buffer, start);
    luaL_pushresult(&buffer);
    return 1;
}

static int host_table_unpack(lua_State* state) {
    const lua_Integer start = luaL_optinteger(state, 2, 1);
    lua_Integer cursor = start;
    const lua_Integer last = luaL_opt(state, luaL_checkinteger, 3, luaL_len(state, 1));
    if (cursor > last)
        return 0;
    const lua_Unsigned span = static_cast<lua_Unsigned>(last) - static_cast<lua_Unsigned>(cursor);
    const uint64_t count = static_cast<uint64_t>(span) + 1;
    if (count > table_range_limit)
        refuse_range(state, "table.unpack", count);
    if (span >= static_cast<lua_Unsigned>(INT_MAX) ||
        !lua_checkstack(state, static_cast<int>(span + 1)))
        return luaL_error(state, "too many results to unpack");
    charge(state, count);
    for (; cursor < last; ++cursor)
        lua_geti(state, 1, cursor);
    lua_geti(state, 1, last);
    return static_cast<int>(count);
}

static int host_table_sort(lua_State* state) {
    const lua_Integer length = table_length(state, 1, tab_read | tab_write);
    if (length > 1) {
        luaL_argcheck(state, length < INT_MAX, 1, "array too big");
        if (static_cast<lua_Unsigned>(length) > table_range_limit)
            refuse_range(state, "table.sort", static_cast<uint64_t>(length));
        if (!lua_isnoneornil(state, 2))
            luaL_checktype(state, 2, LUA_TFUNCTION);
        lua_settop(state, 2);
        aux_sort(state, 1, static_cast<SortIndex>(length), 0);
    }
    return 0;
}

static int host_utf8_len(lua_State* state) {
    std::size_t length = 0;
    const char* text = luaL_checklstring(state, 1, &length);
    lua_Integer posi = utf8_position(luaL_optinteger(state, 2, 1), length);
    lua_Integer posj = utf8_position(luaL_optinteger(state, 3, -1), length);
    const int lax = lua_toboolean(state, 4);
    luaL_argcheck(
        state,
        1 <= posi && --posi <= static_cast<lua_Integer>(length),
        2,
        "initial position out of bounds"
    );
    luaL_argcheck(
        state, --posj < static_cast<lua_Integer>(length), 3, "final position out of bounds"
    );
    if (posi <= posj) {
        const auto bytes = static_cast<std::size_t>(posj - posi) + 1;
        charge(state, units_for_bytes(bytes));
    }
    lua_Integer count = 0;
    while (posi <= posj) {
        const char* next = utf8_decode(text + posi, nullptr, lax == 0);
        if (next == nullptr) {
            luaL_pushfail(state);
            lua_pushinteger(state, posi + 1);
            return 2;
        }
        posi = static_cast<lua_Integer>(next - text);
        ++count;
    }
    lua_pushinteger(state, count);
    return 1;
}

static int host_utf8_codepoint(lua_State* state) {
    std::size_t length = 0;
    const char* text = luaL_checklstring(state, 1, &length);
    const lua_Integer posi = utf8_position(luaL_optinteger(state, 2, 1), length);
    const lua_Integer pose = utf8_position(luaL_optinteger(state, 3, posi), length);
    const int lax = lua_toboolean(state, 4);
    luaL_argcheck(state, posi >= 1, 2, "out of bounds");
    luaL_argcheck(state, pose <= static_cast<lua_Integer>(length), 3, "out of bounds");
    if (posi > pose)
        return 0;
    if (pose - posi >= INT_MAX)
        return luaL_error(state, "string slice too long");
    const int room = static_cast<int>(pose - posi) + 1;
    luaL_checkstack(state, room, "string slice too long");
    int produced = 0;
    const char* cursor = text + (posi - 1);
    const char* finish = text + pose;
    for (; cursor < finish;) {
        unsigned code = 0;
        cursor = utf8_decode(cursor, &code, lax == 0);
        if (cursor == nullptr)
            return luaL_error(state, utf8_invalid);
        lua_pushinteger(state, static_cast<lua_Integer>(code));
        ++produced;
    }
    charge(state, static_cast<uint64_t>(produced));
    return produced;
}

static int host_utf8_offset(lua_State* state) {
    std::size_t length = 0;
    const char* text = luaL_checklstring(state, 1, &length);
    lua_Integer step = luaL_checkinteger(state, 2);
    lua_Integer origin = step >= 0 ? 1 : static_cast<lua_Integer>(length) + 1;
    origin = utf8_position(luaL_optinteger(state, 3, origin), length);
    luaL_argcheck(
        state,
        1 <= origin && --origin <= static_cast<lua_Integer>(length),
        3,
        "position out of bounds"
    );
    std::size_t scanned = 0;
    if (step == 0) {
        while (origin > 0 && utf8_continuation(static_cast<unsigned char>(text[origin]))) {
            --origin;
            ++scanned;
        }
    } else {
        if (utf8_continuation(static_cast<unsigned char>(text[origin])))
            return luaL_error(state, "initial position is a continuation byte");
        if (step < 0) {
            while (step < 0 && origin > 0) {
                do {
                    --origin;
                    ++scanned;
                } while (origin > 0 && utf8_continuation(static_cast<unsigned char>(text[origin])));
                ++step;
            }
        } else {
            --step;
            while (step > 0 && origin < static_cast<lua_Integer>(length)) {
                do {
                    ++origin;
                    ++scanned;
                } while (utf8_continuation(static_cast<unsigned char>(text[origin])));
                --step;
            }
        }
    }
    charge(state, units_for_bytes(scanned));
    if (step == 0)
        lua_pushinteger(state, origin + 1);
    else
        luaL_pushfail(state);
    return 1;
}

static int host_utf8_codes_aux(lua_State* state, int strict) {
    std::size_t length = 0;
    const char* text = luaL_checklstring(state, 1, &length);
    const lua_Unsigned start = static_cast<lua_Unsigned>(lua_tointeger(state, 2));
    lua_Unsigned index = start;
    if (index < length) {
        while (utf8_continuation(static_cast<unsigned char>(text[index])))
            ++index;
    }
    if (index >= length) {
        std::size_t scanned = 0;
        if (start < length && index > start)
            scanned = static_cast<std::size_t>(index - start);
        charge(state, units_for_bytes(scanned));
        return 0;
    }
    unsigned code = 0;
    const char* next = utf8_decode(text + index, &code, strict != 0);
    if (next == nullptr || utf8_continuation(static_cast<unsigned char>(*next))) {
        std::size_t scanned = 1;
        if (index > start)
            scanned = static_cast<std::size_t>(index - start) + 1;
        charge(state, units_for_bytes(scanned));
        return luaL_error(state, utf8_invalid);
    }
    charge(state, units_for_bytes(static_cast<std::size_t>(next - (text + start))));
    lua_pushinteger(state, static_cast<lua_Integer>(index) + 1);
    lua_pushinteger(state, static_cast<lua_Integer>(code));
    return 2;
}

// Two light functions, as Lua's own utf8.codes does. A new closure each
// call would not compare equal to the next one.
static int host_utf8_codes_strict(lua_State* state) {
    return host_utf8_codes_aux(state, 1);
}

static int host_utf8_codes_lax(lua_State* state) {
    return host_utf8_codes_aux(state, 0);
}

static int host_utf8_codes(lua_State* state) {
    const int lax = lua_toboolean(state, 2);
    const char* text = luaL_checkstring(state, 1);
    luaL_argcheck(state, !utf8_continuation(static_cast<unsigned char>(text[0])), 1, utf8_invalid);
    lua_pushcfunction(state, lax != 0 ? host_utf8_codes_lax : host_utf8_codes_strict);
    lua_pushvalue(state, 1);
    lua_pushinteger(state, 0);
    return 3;
}

// next, from lbaselib.c. One walk can visit every slot an emptied table
// still holds, so the call costs scan_call_charge.
static int host_next(lua_State* state) {
    luaL_checktype(state, 1, LUA_TTABLE);
    charge(state, scan_call_charge);
    lua_settop(state, 2);
    if (lua_next(state, 1) != 0)
        return 2;
    lua_pushnil(state);
    return 1;
}

// rawequal, from lbaselib.c. Two strings are charged one unit per
// bytes_per_charge of their two lengths, which their comparison may read.
static int host_rawequal(lua_State* state) {
    luaL_checkany(state, 1);
    luaL_checkany(state, 2);
    if (lua_type(state, 1) == LUA_TSTRING && lua_type(state, 2) == LUA_TSTRING) {
        std::size_t left_length = 0;
        std::size_t right_length = 0;
        lua_tolstring(state, 1, &left_length);
        lua_tolstring(state, 2, &right_length);
        charge(state, units_for_bytes(left_length + right_length));
    }
    lua_pushboolean(state, lua_rawequal(state, 1, 2));
    return 1;
}

// tonumber, from lbaselib.c. Reading a string of digits is charged one unit
// per bytes_per_charge of its length, and at least one unit.
static int host_tonumber(lua_State* state) {
    std::size_t text_length = 0;
    if (lua_type(state, 1) == LUA_TSTRING)
        lua_tolstring(state, 1, &text_length);
    charge(state, text_length == 0 ? 1 : units_for_bytes(text_length));
    if (lua_isnoneornil(state, 2)) {
        if (lua_type(state, 1) == LUA_TNUMBER) {
            lua_settop(state, 1);
            return 1;
        }
        std::size_t length = 0;
        const char* text = lua_tolstring(state, 1, &length);
        if (text != nullptr && lua_stringtonumber(state, text) == length + 1)
            return 1;
        luaL_checkany(state, 1);
    } else {
        lua_Integer value = 0;
        const lua_Integer base = luaL_checkinteger(state, 2);
        luaL_checktype(state, 1, LUA_TSTRING);
        std::size_t length = 0;
        const char* text = lua_tolstring(state, 1, &length);
        luaL_argcheck(state, 2 <= base && base <= 36, 2, "base out of range");
        if (text_to_integer(text, static_cast<int>(base), &value) == text + length) {
            lua_pushinteger(state, value);
            return 1;
        }
    }
    luaL_pushfail(state);
    return 1;
}

// warn, from lbaselib.c. Every argument is checked before any is passed on,
// so an error cannot leave a warning unfinished.
static int host_warn(lua_State* state) {
    const int count = lua_gettop(state);
    charge(state, argument_unit * static_cast<uint64_t>(count > 1 ? count : 1));
    luaL_checkstring(state, 1);
    for (int index = 2; index <= count; ++index)
        luaL_checkstring(state, index);
    for (int index = 1; index < count; ++index)
        lua_warning(state, lua_tostring(state, index), 1);
    lua_warning(state, lua_tostring(state, count), 0);
    return 0;
}

// utf8.char, from lutf8lib.c. One unit per code point.
static int host_utf8_char(lua_State* state) {
    const int count = lua_gettop(state);
    charge(state, count <= 0 ? 1 : static_cast<uint64_t>(count));
    if (count == 1) {
        push_utf8_char(state, 1);
        return 1;
    }
    luaL_Buffer buffer;
    luaL_buffinit(state, &buffer);
    for (int index = 1; index <= count; ++index) {
        push_utf8_char(state, index);
        luaL_addvalue(&buffer);
    }
    luaL_pushresult(&buffer);
    return 1;
}

// table.pack, from ltablib.c. One unit per element.
static int host_table_pack(lua_State* state) {
    const int count = lua_gettop(state);
    charge(state, static_cast<uint64_t>(count));
    lua_createtable(state, count, 1);
    lua_insert(state, 1);
    for (int index = count; index >= 1; --index)
        lua_seti(state, 1, index);
    lua_pushinteger(state, count);
    lua_setfield(state, 1, "n");
    return 1;
}
} // extern "C"

void install_c_work(lua_State* state) {
    lua_getglobal(state, "string");
    lua_pushcfunction(state, host_string_byte);
    lua_setfield(state, -2, "byte");
    lua_pop(state, 1);

    lua_getglobal(state, "table");
    lua_pushcfunction(state, host_table_concat);
    lua_setfield(state, -2, "concat");
    lua_pushcfunction(state, host_table_insert);
    lua_setfield(state, -2, "insert");
    lua_pushcfunction(state, host_table_move);
    lua_setfield(state, -2, "move");
    lua_pushcfunction(state, host_table_pack);
    lua_setfield(state, -2, "pack");
    lua_pushcfunction(state, host_table_remove);
    lua_setfield(state, -2, "remove");
    lua_pushcfunction(state, host_table_sort);
    lua_setfield(state, -2, "sort");
    lua_pushcfunction(state, host_table_unpack);
    lua_setfield(state, -2, "unpack");
    lua_pop(state, 1);

    lua_getglobal(state, "utf8");
    lua_pushcfunction(state, host_utf8_char);
    lua_setfield(state, -2, "char");
    lua_pushcfunction(state, host_utf8_offset);
    lua_setfield(state, -2, "offset");
    lua_pushcfunction(state, host_utf8_codepoint);
    lua_setfield(state, -2, "codepoint");
    lua_pushcfunction(state, host_utf8_len);
    lua_setfield(state, -2, "len");
    lua_pushcfunction(state, host_utf8_codes);
    lua_setfield(state, -2, "codes");
    lua_pop(state, 1);
}

void replace_global(lua_State* state, const char* name, lua_CFunction host) {
    lua_pushcfunction(state, host);
    lua_setglobal(state, name);
}

} // namespace

void open_libraries(lua_State* state) {
    luaL_requiref(state, "_G", luaopen_base, 1);
    lua_pop(state, 1);
    replace_global(state, "next", host_next);
    replace_global(state, "rawequal", host_rawequal);
    replace_global(state, "tonumber", host_tonumber);
    replace_global(state, "warn", host_warn);
    luaL_requiref(state, "string", luaopen_string, 1);
    lua_pop(state, 1);
    luaL_requiref(state, "table", luaopen_table, 1);
    lua_pop(state, 1);
    luaL_requiref(state, "utf8", luaopen_utf8, 1);
    lua_pop(state, 1);
    open_math_library(state);
    open_fixed_library(state);

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
    // Sizes and alignment differ between 32-bit and 64-bit builds, and between
    // Windows and the rest, so a script cannot pack bytes itself.
    lua_pushnil(state);
    lua_setfield(state, -2, "pack");
    lua_pushnil(state);
    lua_setfield(state, -2, "unpack");
    lua_pushnil(state);
    lua_setfield(state, -2, "packsize");
    lua_pushcfunction(state, host_string_rep);
    lua_setfield(state, -2, "rep");
    // The string metatable's __index is this table, so ("x"):find and
    // ("%d"):format use these.
    install_format(state);
    install_patterns(state);
    lua_pop(state, 1);

    lua_pushcfunction(state, host_setmetatable);
    lua_setglobal(state, "setmetatable");

    lua_getglobal(state, "rawset");
    lua_rawsetp(state, LUA_REGISTRYINDEX, &rawset_key);
    lua_pushcfunction(state, host_rawset);
    lua_setglobal(state, "rawset");

    lua_newtable(state);
    lua_pushcfunction(state, host_mode_newindex);
    lua_setfield(state, -2, "__newindex");
    lua_pushboolean(state, 1);
    lua_setfield(state, -2, "__metatable");
    lua_rawsetp(state, LUA_REGISTRYINDEX, &metatable_guard_key);

    lua_pushcfunction(state, host_print);
    lua_setglobal(state, "print");

    install_pairs(state);

    lua_getglobal(state, "xpcall");
    lua_rawsetp(state, LUA_REGISTRYINDEX, &xpcall_key);
    lua_pushcfunction(state, host_xpcall);
    lua_setglobal(state, "xpcall");

    install_require(state);
    install_c_work(state);
}

} // namespace oa::data::script_host
