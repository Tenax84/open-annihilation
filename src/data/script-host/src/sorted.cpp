// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/data/script_host/sorted.hpp"

#include "internal.hpp"

#include <algorithm>
#include <climits>
#include <cstring>

namespace oa::data::script_host {
namespace {

unsigned char fold_letter(unsigned char byte) noexcept {
    if (byte >= 'A' && byte <= 'Z')
        return static_cast<unsigned char>(byte - 'A' + 'a');
    return byte;
}

int key_category(int type) noexcept {
    if (type == LUA_TNUMBER)
        return 0;
    if (type == LUA_TSTRING)
        return 1;
    return 2;
}

bool key_less(lua_State* state, int left, int right) {
    const int left_type = lua_type(state, left);
    const int right_type = lua_type(state, right);
    const int left_category = key_category(left_type);
    const int right_category = key_category(right_type);
    if (left_category != right_category) {
        charge(state, comparison_unit);
        return left_category < right_category;
    }
    if (left_type == LUA_TNUMBER || left_type == LUA_TSTRING) {
        charge(state, comparison_charge(state, left, right));
        return lua_compare(state, left, right, LUA_OPLT) != 0;
    }
    charge(state, comparison_unit);
    return lua_toboolean(state, left) == 0 && lua_toboolean(state, right) != 0;
}

void swap_keys(lua_State* state, int sequence, lua_Integer left, lua_Integer right) {
    if (left == right)
        return;
    lua_rawgeti(state, sequence, left);
    lua_rawgeti(state, sequence, right);
    lua_rawseti(state, sequence, left);
    lua_rawseti(state, sequence, right);
}

bool less_at(lua_State* state, int sequence, lua_Integer left, lua_Integer right) {
    lua_rawgeti(state, sequence, left);
    lua_rawgeti(state, sequence, right);
    const bool less = key_less(state, -2, -1);
    lua_pop(state, 2);
    return less;
}

void sift_down(lua_State* state, int sequence, lua_Integer start, lua_Integer end) {
    lua_Integer root = start;
    for (;;) {
        const lua_Integer child = root * 2;
        if (child > end)
            return;
        lua_Integer chosen = child;
        if (child + 1 <= end && less_at(state, sequence, child, child + 1))
            chosen = child + 1;
        if (!less_at(state, sequence, root, chosen))
            return;
        swap_keys(state, sequence, root, chosen);
        root = chosen;
    }
}

void sort_keys(lua_State* state, int sequence, lua_Integer count) {
    if (count < 2)
        return;
    luaL_checkstack(state, 4, "pairs");
    for (lua_Integer start = count / 2; start >= 1; --start)
        sift_down(state, sequence, start, count);
    for (lua_Integer end = count; end > 1; --end) {
        swap_keys(state, sequence, 1, end);
        sift_down(state, sequence, 1, end - 1);
    }
}

extern "C" {
static int pairs_step(lua_State* state) {
    for (;;) {
        const lua_Integer slot = lua_tointeger(state, lua_upvalueindex(3)) + 1;
        lua_pushinteger(state, slot);
        lua_replace(state, lua_upvalueindex(3));
        if (lua_rawgeti(state, lua_upvalueindex(2), slot) == LUA_TNIL)
            return 1;
        lua_pushvalue(state, -1);
        if (lua_rawget(state, lua_upvalueindex(1)) == LUA_TNIL) {
            lua_pop(state, 2);
            continue;
        }
        return 2;
    }
}
} // extern "C"

} // namespace

bool name_less(std::string_view left, std::string_view right) noexcept {
    const std::size_t count = left.size() < right.size() ? left.size() : right.size();
    for (std::size_t index = 0; index < count; ++index) {
        const auto left_byte = static_cast<unsigned char>(left[index]);
        const auto right_byte = static_cast<unsigned char>(right[index]);
        const unsigned char left_folded = fold_letter(left_byte);
        const unsigned char right_folded = fold_letter(right_byte);
        if (left_folded != right_folded)
            return left_folded < right_folded;
    }
    if (left.size() != right.size())
        return left.size() < right.size();
    for (std::size_t index = 0; index < count; ++index) {
        const auto left_byte = static_cast<unsigned char>(left[index]);
        const auto right_byte = static_cast<unsigned char>(right[index]);
        if (left_byte != right_byte)
            return left_byte < right_byte;
    }
    return false;
}

void sort_names(std::vector<std::string>& names) {
    std::sort(names.begin(), names.end(), name_less);
}

void push_names(lua_State* state, std::span<const std::string> names) {
    const int count = static_cast<int>(names.size());
    lua_createtable(state, count, 0);
    for (int index = 0; index < count; ++index) {
        const std::string& name = names[static_cast<std::size_t>(index)];
        lua_pushlstring(state, name.data(), name.size());
        lua_rawseti(state, -2, static_cast<lua_Integer>(index) + 1);
    }
}

int push_sorted_keys(lua_State* state, int index) {
    // One walk can visit every slot an emptied table still holds.
    charge(state, scan_call_charge);
    index = lua_absindex(state, index);
    luaL_checktype(state, index, LUA_TTABLE);
    luaL_checkstack(state, 4, "pairs");
    lua_newtable(state);
    const int sequence = lua_gettop(state);
    lua_Integer count = 0;
    lua_pushnil(state);
    while (lua_next(state, index) != 0) {
        const int key_type = lua_type(state, -2);
        if (key_type != LUA_TNUMBER && key_type != LUA_TSTRING && key_type != LUA_TBOOLEAN) {
            const char* type_name = lua_typename(state, key_type);
            return luaL_error(
                state,
                "pairs: this table has keys of type %s, which have no fixed order; "
                "keep such values in a sequence and walk it with ipairs",
                type_name
            );
        }
        lua_pop(state, 1);
        lua_pushvalue(state, -1);
        ++count;
        lua_rawseti(state, sequence, count);
    }
    sort_keys(state, sequence, count);
    if (count > static_cast<lua_Integer>(INT_MAX))
        return luaL_error(state, "pairs: too many keys");
    return static_cast<int>(count);
}

extern "C" {
static int ordered_pairs(lua_State* state) {
    luaL_checkany(state, 1);
    if (luaL_getmetafield(state, 1, "__pairs") != LUA_TNIL) {
        lua_pushvalue(state, 1);
        lua_call(state, 1, 3);
        return 3;
    }
    luaL_checktype(state, 1, LUA_TTABLE);
    lua_pushvalue(state, 1);
    push_sorted_keys(state, 1);
    lua_pushinteger(state, 0);
    lua_pushcclosure(state, pairs_step, 3);
    lua_pushvalue(state, 1);
    lua_pushnil(state);
    return 3;
}
} // extern "C"

void install_pairs(lua_State* state) {
    lua_pushcfunction(state, ordered_pairs);
    lua_setglobal(state, "pairs");
}

} // namespace oa::data::script_host
