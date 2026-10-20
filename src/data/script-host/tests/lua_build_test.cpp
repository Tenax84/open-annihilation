// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The vendored Lua build: a text chunk returns its result and frees every
// allocation, the libraries the build compiles write the same number text
// everywhere and the others are absent, three states walk one table in one
// order, and a precompiled chunk is refused.

#include "lua.hpp"
#include "oa/test/check.hpp"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct AllocationCount {
    std::size_t live_bytes = 0;
    std::size_t live_blocks = 0;
};

// Counts the bytes a Lua state still holds. Lua reports the old size when
// it reallocates or frees, and a zero new size frees the block.
void* counting_allocate(void* user_data, void* pointer, size_t old_size, size_t new_size) {
    auto* count = static_cast<AllocationCount*>(user_data);
    if (new_size == 0) {
        if (pointer != nullptr) {
            if (old_size <= count->live_bytes && count->live_blocks > 0) {
                count->live_bytes -= old_size;
                --count->live_blocks;
            }
            std::free(pointer);
        }
        return nullptr;
    }
    void* allocated = std::realloc(pointer, new_size);
    if (allocated == nullptr)
        return nullptr;
    if (pointer == nullptr) {
        count->live_bytes += new_size;
        ++count->live_blocks;
    } else if (old_size <= count->live_bytes) {
        count->live_bytes = count->live_bytes - old_size + new_size;
    }
    return allocated;
}

lua_State* open_state(AllocationCount& count) {
    lua_State* state = lua_newstate(counting_allocate, &count);
    OA_CHECK(state != nullptr);
    return state;
}

void close_state(lua_State* state, AllocationCount& count) {
    if (state != nullptr)
        lua_close(state);
    OA_CHECK(count.live_bytes == 0);
    OA_CHECK(count.live_blocks == 0);
}

void test_chunk_returns_forty_two() {
    AllocationCount count;
    lua_State* state = open_state(count);
    if (state == nullptr)
        return;
    const char* source = "return 6 * 7";
    const int loaded = luaL_loadbufferx(state, source, std::strlen(source), source, "t");
    OA_CHECK(loaded == LUA_OK);
    if (loaded == LUA_OK) {
        const int called = lua_pcall(state, 0, 1, 0);
        OA_CHECK(called == LUA_OK);
        if (called == LUA_OK) {
            OA_CHECK(lua_isinteger(state, -1));
            OA_CHECK(lua_tointeger(state, -1) == 42);
        }
    }
    close_state(state, count);
}

// Opens the four libraries this build compiles, into the global table.
void open_kept_libraries(lua_State* state) {
    luaL_requiref(state, LUA_GNAME, luaopen_base, 1);
    lua_pop(state, 1);
    luaL_requiref(state, LUA_STRLIBNAME, luaopen_string, 1);
    lua_pop(state, 1);
    luaL_requiref(state, LUA_TABLIBNAME, luaopen_table, 1);
    lua_pop(state, 1);
    luaL_requiref(state, LUA_UTF8LIBNAME, luaopen_utf8, 1);
    lua_pop(state, 1);
}

std::string string_result(lua_State* state, const char* source) {
    const int loaded = luaL_loadbufferx(state, source, std::strlen(source), source, "t");
    OA_CHECK(loaded == LUA_OK);
    if (loaded != LUA_OK) {
        lua_pop(state, 1);
        return {};
    }
    const int called = lua_pcall(state, 0, 1, 0);
    OA_CHECK(called == LUA_OK);
    if (called != LUA_OK) {
        lua_pop(state, 1);
        return {};
    }
    const char* text = lua_tostring(state, -1);
    OA_CHECK(text != nullptr);
    std::string value = text == nullptr ? std::string() : std::string(text);
    lua_pop(state, 1);
    return value;
}

bool boolean_result(lua_State* state, const char* source) {
    const int loaded = luaL_loadbufferx(state, source, std::strlen(source), source, "t");
    OA_CHECK(loaded == LUA_OK);
    if (loaded != LUA_OK) {
        lua_pop(state, 1);
        return false;
    }
    const int called = lua_pcall(state, 0, 1, 0);
    OA_CHECK(called == LUA_OK);
    if (called != LUA_OK) {
        lua_pop(state, 1);
        return false;
    }
    OA_CHECK(lua_isboolean(state, -1));
    const bool value = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
    return value;
}

void check_absent(lua_State* state, const char* name) {
    OA_CHECK(lua_getglobal(state, name) == LUA_TNIL);
    lua_pop(state, 1);
}

void test_libraries_and_numbers() {
    AllocationCount count;
    lua_State* state = open_state(count);
    if (state == nullptr)
        return;
    open_kept_libraries(state);
    OA_CHECK(
        string_result(state, "return string.format(\"%5.1f|%d|%s\", 2.26, 1 << 40, 0.1)") ==
        "  2.3|1099511627776|0.1"
    );
    OA_CHECK(string_result(state, "return tostring(2^63)") == "9.2233720368548e+18");
    OA_CHECK(boolean_result(state, "return tonumber(\"0x1.8p1\") == 3.0"));
    OA_CHECK(boolean_result(state, "return tonumber(string.format(\"%a\", 0.1)) == 0.1"));
    check_absent(state, "math");
    check_absent(state, "io");
    check_absent(state, "os");
    check_absent(state, "package");
    check_absent(state, "debug");
    check_absent(state, "coroutine");
    close_state(state, count);
}

constexpr const char* table_keys[] = {
    "amber", "basin", "cedar", "delta", "ember", "flint",  "grove",  "haven", "inlet",  "juniper",
    "kelp",  "ledge", "maple", "north", "otter", "pebble", "quartz", "ridge", "spruce", "timber",
};

std::vector<std::string> key_order(lua_State* state) {
    lua_newtable(state);
    for (const char* key : table_keys) {
        lua_pushstring(state, key);
        lua_pushboolean(state, 1);
        lua_settable(state, -3);
    }
    std::vector<std::string> order;
    lua_pushnil(state);
    while (lua_next(state, -2) != 0) {
        const char* key = lua_tostring(state, -2);
        OA_CHECK(lua_type(state, -2) == LUA_TSTRING);
        OA_CHECK(key != nullptr);
        if (key != nullptr)
            order.emplace_back(key);
        lua_pop(state, 1);
    }
    lua_pop(state, 1);
    return order;
}

void test_tables_walk_in_one_order() {
    AllocationCount counts[4] = {};
    // The second state is unused and left open, so the three states that
    // walk the table cannot be the same block.
    lua_State* first = open_state(counts[0]);
    lua_State* unused = open_state(counts[1]);
    lua_State* second = open_state(counts[2]);
    lua_State* third = open_state(counts[3]);
    OA_CHECK(first != nullptr && unused != nullptr && second != nullptr && third != nullptr);
    OA_CHECK(first != second && second != third && first != third);
    if (first != nullptr && second != nullptr && third != nullptr) {
        const std::vector<std::string> order = key_order(first);
        OA_CHECK(order.size() == 20);
        OA_CHECK(key_order(second) == order);
        OA_CHECK(key_order(third) == order);
    }
    close_state(third, counts[3]);
    close_state(second, counts[2]);
    close_state(unused, counts[1]);
    close_state(first, counts[0]);
}

int append_dump(lua_State*, const void* data, size_t size, void* user_data) {
    auto* dumped = static_cast<std::string*>(user_data);
    dumped->append(static_cast<const char*>(data), size);
    return 0;
}

void test_precompiled_chunk_is_refused() {
    AllocationCount count;
    lua_State* state = open_state(count);
    if (state == nullptr)
        return;
    const char* source = "return 6 * 7";
    const int loaded = luaL_loadbufferx(state, source, std::strlen(source), source, "t");
    OA_CHECK(loaded == LUA_OK);
    if (loaded == LUA_OK) {
        std::string dumped;
        OA_CHECK(lua_dump(state, append_dump, &dumped, 0) == LUA_OK);
        lua_pop(state, 1);
        const int binary = luaL_loadbufferx(state, dumped.data(), dumped.size(), source, "t");
        OA_CHECK(binary == LUA_ERRSYNTAX);
        if (binary != LUA_OK) {
            const char* message = lua_tostring(state, -1);
            OA_CHECK(message != nullptr && std::strstr(message, "binary chunk") != nullptr);
            lua_pop(state, 1);
        }
    }
    close_state(state, count);
}

} // namespace

int main() {
    test_chunk_returns_forty_two();
    test_libraries_and_numbers();
    test_tables_walk_in_one_order();
    test_precompiled_chunk_is_refused();
    return oa::test::check_exit_status();
}
