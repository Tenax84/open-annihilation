// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// pairs walks number, string and boolean keys in one order, and the host
// helpers hand names and keys over in that order.

#include "lua.hpp"
#include "oa/data/script_host/script_host.hpp"
#include "oa/data/script_host/sorted.hpp"
#include "oa/test/check.hpp"

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

struct Texts {
    int count = -1;
    char value[12][400]{};
};

void read_texts(void* context, lua_State* state, int results) {
    auto* texts = static_cast<Texts*>(context);
    texts->count = results;
    for (int index = 0; index < results && index < 12; ++index) {
        std::size_t length = 0;
        const char* value = lua_tolstring(state, -results + index, &length);
        if (value == nullptr || length >= sizeof texts->value[index])
            continue;
        std::memcpy(texts->value[index], value, length);
        texts->value[index][length] = '\0';
    }
}

void expect_text(const char* got, const char* expected) {
    if (std::strcmp(got, expected) != 0)
        std::fprintf(stderr, "pairs text\n  got %s\n  expected %s\n", got, expected);
    OA_CHECK(std::strcmp(got, expected) == 0);
}

void pairs_walks_in_sorted_order() {
    Package package;
    host::ScriptError error;
    host::OpenOptions options;
    options.package_id = "sample";
    options.profile = host::mutator_profile();
    options.sources.context = &package;
    options.sources.read = read_package;
    auto sandbox = host::Sandbox::open(options, error);
    OA_CHECK(sandbox.has_value());
    if (!sandbox)
        return;
    const char* source =
        "local function show(t)\n"
        "  local parts = {}\n"
        "  for k, v in pairs(t) do\n"
        "    parts[#parts + 1] = type(k) .. ':' .. tostring(k) .. '=' .. tostring(v)\n"
        "  end\n"
        "  return table.concat(parts, ',')\n"
        "end\n"
        "local function build(order)\n"
        "  local t = {}\n"
        "  for i = 1, #order do t[order[i]] = order[i] end\n"
        "  return t\n"
        "end\n"
        "local first = build({2, 'b', -3, 'A', true, 1.5, false, 'a'})\n"
        "local second = build({'a', false, 1.5, 'A', true, -3, 'b', 2})\n"
        "second[2] = nil\n"
        "second[2] = 2\n"
        "local during = {}\n"
        "during[1], during[2], during[3] = 'a', 'b', 'c'\n"
        "local seen = {}\n"
        "for k, v in pairs(during) do\n"
        "  seen[#seen + 1] = tostring(k) .. '=' .. v\n"
        "  if k == 1 then during[2] = nil; during[3] = 'C'; during[4] = 'no' end\n"
        "end\n"
        "local big = {}\n"
        "big[9007199254740992.0] = 'float'\n"
        "big[9007199254740993] = 'int'\n"
        "local letters = {B = 1, a = 2}\n"
        "local empty = 0\n"
        "for k, v in pairs({}) do empty = empty + 1 end\n"
        "local function caught(make)\n"
        "  local t = {}\n"
        "  t[make()] = 1\n"
        "  local ok, err = pcall(function() for k, v in pairs(t) do end end)\n"
        "  return ok and 'ok' or err\n"
        "end\n"
        "local function step(state, key)\n"
        "  if key == nil then return 'Ridge', 1 end\n"
        "end\n"
        "local object = setmetatable({}, {__pairs = function() return step, nil, nil end})\n"
        "object[{}] = true\n"
        "local iter, state, key = pairs(object)\n"
        "local name, value = iter(state, key)\n"
        "local joined = {}\n"
        "for i, v in ipairs({'a', 'b', 'c'}) do joined[#joined + 1] = v end\n"
        "local nxt = (next({}) == nil and next({7}) ~= nil) and 'yes' or 'no'\n"
        "return show(first), show(second), table.concat(seen, ','), show(big), show(letters),\n"
        "  tostring(empty), caught(function() return {} end),\n"
        "  caught(function() return function() end end),\n"
        "  name .. '=' .. tostring(value), table.concat(joined, ','), nxt\n";
    package.files["mutator.lua"] = std::vector<uint8_t>(source, source + std::strlen(source));
    Texts texts;
    host::CallHooks hooks;
    hooks.context = &texts;
    hooks.read = read_texts;
    OA_CHECK(sandbox->run_file("mutator.lua", "plan", &hooks, error));
    if (!error.message.empty())
        std::fprintf(stderr, "pairs error %s\n", error.message.c_str());
    OA_CHECK(texts.count == 11);
    const char* mixed = "number:-3=-3,number:1.5=1.5,number:2=2,string:A=A,string:a=a,string:b=b,"
                        "boolean:false=false,boolean:true=true";
    expect_text(texts.value[0], mixed);
    expect_text(texts.value[1], mixed);
    expect_text(texts.value[2], "1=a,3=C");
    expect_text(texts.value[3], "number:9007199254740992=float,number:9007199254740993=int");
    expect_text(texts.value[4], "string:B=1,string:a=2");
    expect_text(texts.value[5], "0");
    OA_CHECK(
        std::strstr(
            texts.value[6],
            "pairs: this table has keys of type table, which have no fixed order; "
            "keep such values in a sequence and walk it with ipairs"
        ) != nullptr
    );
    OA_CHECK(
        std::strstr(
            texts.value[7],
            "pairs: this table has keys of type function, which have no fixed order; "
            "keep such values in a sequence and walk it with ipairs"
        ) != nullptr
    );
    expect_text(texts.value[8], "Ridge=1");
    expect_text(texts.value[9], "a,b,c");
    expect_text(texts.value[10], "yes");
}

void names_sort_without_case_then_by_bytes() {
    OA_CHECK(host::name_less("ARMCOM", "armpw"));
    OA_CHECK(host::name_less("armpw", "CORAK"));
    OA_CHECK(!host::name_less("CORAK", "ARMCOM"));
    OA_CHECK(host::name_less("ARM", "arm"));
    OA_CHECK(!host::name_less("arm", "ARM"));
    OA_CHECK(!host::name_less("ARM", "ARM"));
    OA_CHECK(host::name_less("ARM", "ARMCOM"));
    OA_CHECK(host::name_less("a", "B"));
    std::vector<std::string> names{"CORAK", "armpw", "ARMCOM", "arm", "ARM"};
    host::sort_names(names);
    OA_CHECK(names.size() == 5);
    OA_CHECK(names[0] == "ARM");
    OA_CHECK(names[1] == "arm");
    OA_CHECK(names[2] == "ARMCOM");
    OA_CHECK(names[3] == "armpw");
    OA_CHECK(names[4] == "CORAK");
}

struct NameList {
    const std::vector<std::string>* names = nullptr;
    int count = -1;
    char text[6][16]{};
};

void push_sorted_names(void* context, lua_State* state) {
    auto* list = static_cast<NameList*>(context);
    host::push_names(state, *list->names);
    list->count = static_cast<int>(lua_rawlen(state, -1));
    for (int index = 1; index <= list->count && index < 6; ++index) {
        std::size_t length = 0;
        lua_rawgeti(state, -1, index);
        const char* value = lua_tolstring(state, -1, &length);
        if (value != nullptr && length < sizeof list->text[index]) {
            std::memcpy(list->text[index], value, length);
            list->text[index][length] = '\0';
        }
        lua_pop(state, 1);
    }
}

struct KeyList {
    int count = -1;
    int kind[8]{};
    double number[8]{};
    char text[8][8]{};
    int flag[8]{};
};

void push_key_sequence(void* context, lua_State* state) {
    auto* list = static_cast<KeyList*>(context);
    lua_newtable(state);
    lua_pushinteger(state, 2);
    lua_pushliteral(state, "i");
    lua_rawset(state, -3);
    lua_pushnumber(state, 1.5);
    lua_pushliteral(state, "f");
    lua_rawset(state, -3);
    lua_pushnumber(state, -3);
    lua_pushliteral(state, "n");
    lua_rawset(state, -3);
    lua_pushliteral(state, "b");
    lua_pushliteral(state, "sb");
    lua_rawset(state, -3);
    lua_pushliteral(state, "A");
    lua_pushliteral(state, "sA");
    lua_rawset(state, -3);
    lua_pushliteral(state, "a");
    lua_pushliteral(state, "sa");
    lua_rawset(state, -3);
    lua_pushboolean(state, 1);
    lua_pushliteral(state, "t");
    lua_rawset(state, -3);
    lua_pushboolean(state, 0);
    lua_pushliteral(state, "f");
    lua_rawset(state, -3);
    list->count = host::push_sorted_keys(state, -1);
    for (int index = 1; index <= list->count && index <= 8; ++index) {
        lua_rawgeti(state, -1, index);
        list->kind[index - 1] = lua_type(state, -1);
        if (lua_type(state, -1) == LUA_TNUMBER)
            list->number[index - 1] = lua_tonumber(state, -1);
        else if (lua_type(state, -1) == LUA_TSTRING) {
            std::size_t length = 0;
            const char* value = lua_tolstring(state, -1, &length);
            if (value != nullptr && length < sizeof list->text[index - 1]) {
                std::memcpy(list->text[index - 1], value, length);
                list->text[index - 1][length] = '\0';
            }
        } else if (lua_type(state, -1) == LUA_TBOOLEAN)
            list->flag[index - 1] = lua_toboolean(state, -1);
        lua_pop(state, 1);
    }
}

void helpers_publish_sorted_sequences() {
    std::vector<std::string> names{"CORAK", "armpw", "ARMCOM"};
    host::sort_names(names);
    Package package;
    host::ScriptError error;
    host::OpenOptions options;
    options.package_id = "sample";
    options.profile = host::mutator_profile();
    options.sources.context = &package;
    options.sources.read = read_package;
    auto sandbox = host::Sandbox::open(options, error);
    OA_CHECK(sandbox.has_value());
    if (!sandbox)
        return;
    NameList published;
    published.names = &names;
    OA_CHECK(sandbox->run_host("plan", &published, push_sorted_names, error));
    OA_CHECK(published.count == 3);
    OA_CHECK(std::strcmp(published.text[1], "ARMCOM") == 0);
    OA_CHECK(std::strcmp(published.text[2], "armpw") == 0);
    OA_CHECK(std::strcmp(published.text[3], "CORAK") == 0);

    KeyList keys;
    OA_CHECK(sandbox->run_host("plan", &keys, push_key_sequence, error));
    OA_CHECK(keys.count == 8);
    OA_CHECK(keys.kind[0] == LUA_TNUMBER && keys.number[0] == -3.0);
    OA_CHECK(keys.kind[1] == LUA_TNUMBER && keys.number[1] == 1.5);
    OA_CHECK(keys.kind[2] == LUA_TNUMBER && keys.number[2] == 2.0);
    OA_CHECK(keys.kind[3] == LUA_TSTRING && std::strcmp(keys.text[3], "A") == 0);
    OA_CHECK(keys.kind[4] == LUA_TSTRING && std::strcmp(keys.text[4], "a") == 0);
    OA_CHECK(keys.kind[5] == LUA_TSTRING && std::strcmp(keys.text[5], "b") == 0);
    OA_CHECK(keys.kind[6] == LUA_TBOOLEAN && keys.flag[6] == 0);
    OA_CHECK(keys.kind[7] == LUA_TBOOLEAN && keys.flag[7] == 1);
}

} // namespace

int main() {
    names_sort_without_case_then_by_bytes();
    pairs_walks_in_sorted_order();
    helpers_publish_sorted_sequences();
    return oa::test::check_exit_status();
}
