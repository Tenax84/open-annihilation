// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Charged library calls against Lua's own results, then the loops and ranges
// that have to stop a phase. The clock is only in this test.

#include "lua.hpp"
#include "oa/data/script_host/script_host.hpp"
#include "oa/test/check.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace host = oa::data::script_host;

namespace {

struct Case {
    std::string name;
    std::string body;
};

struct Package {
    std::map<std::string, std::vector<uint8_t>> files;
};

struct ReadResult {
    std::vector<char> text;
    std::size_t length = 0;
    bool saw = false;
    bool overflow = false;
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

void* counting_allocate(void* /*user*/, void* pointer, size_t /*old_size*/, size_t new_size) {
    if (new_size == 0) {
        std::free(pointer);
        return nullptr;
    }
    return std::realloc(pointer, new_size);
}

void read_result(void* context, lua_State* state, int results) {
    auto* out = static_cast<ReadResult*>(context);
    out->saw = true;
    out->length = 0;
    out->overflow = false;
    if (results < 1 || lua_type(state, -1) != LUA_TSTRING)
        return;
    std::size_t length = 0;
    const char* text = lua_tolstring(state, -1, &length);
    if (text == nullptr)
        return;
    if (length >= out->text.size()) {
        out->overflow = true;
        return;
    }
    std::memcpy(out->text.data(), text, length);
    out->length = length;
}

host::Profile compare_profile() {
    host::Budget budget;
    budget.instructions = 2'000'000;
    budget.memory_bytes = std::size_t{8} << 20;
    return host::Profile{"test", budget, nullptr, nullptr};
}

std::optional<host::Sandbox>
open_with(Package& package, host::ScriptError& error, host::Profile profile) {
    host::OpenOptions options;
    options.package_id = "sample";
    options.profile = profile;
    options.sources.context = &package;
    options.sources.read = read_package;
    auto sandbox = host::Sandbox::open(options, error);
    OA_CHECK(sandbox.has_value());
    return sandbox;
}

const char* preamble = R"lua(
local function show(...)
  local count = select("#", ...)
  local parts = {}
  for index = 1, count do
    local value = select(index, ...)
    local kind = type(value)
    if kind == "string" then
      parts[index] = "string:" .. string.format("%q", value)
    elseif kind == "number" then
      parts[index] = "number:" .. string.format("%.17g", value)
    elseif kind == "boolean" then
      if value then
        parts[index] = "boolean:true"
      else
        parts[index] = "boolean:false"
      end
    elseif kind == "nil" then
      parts[index] = "nil"
    else
      parts[index] = kind
    end
  end
  return table.concat(parts, "\t")
end
local ok, result = pcall(function()
)lua";

const char* postamble = R"lua(
end)
if not ok then
  return "error\t" .. tostring(result)
end
return "values\t" .. result
)lua";

std::string script_for(const std::string& body) {
    return std::string(preamble) + body + postamble;
}

std::string message_body(std::string_view text) {
    const auto colon = text.find(':');
    if (colon == std::string_view::npos || colon == 0)
        return std::string(text);
    std::size_t index = colon + 1;
    if (index >= text.size() || text[index] < '0' || text[index] > '9')
        return std::string(text);
    while (index < text.size() && text[index] >= '0' && text[index] <= '9')
        ++index;
    if (index + 1 < text.size() && text[index] == ':' && text[index + 1] == ' ')
        return std::string(text.substr(index + 2));
    return std::string(text);
}

std::string canonical(std::string text) {
    constexpr std::string_view prefix = "error\t";
    if (text.compare(0, prefix.size(), prefix.data(), prefix.size()) != 0)
        return text;
    return std::string(prefix) + message_body(std::string_view(text).substr(prefix.size()));
}

void add_case(std::vector<Case>& cases, std::string name, std::string body) {
    cases.push_back(Case{std::move(name), std::move(body)});
}

void add_cases(std::vector<Case>& cases) {
    const char* rows[][2] = {
        {"byte-one", "return show(string.byte(\"ABC\", 2))"},
        {"byte-range", "return show(string.byte(\"ABC\", 1, 3))"},
        {"byte-negative", "return show(string.byte(\"ABC\", -1))"},
        {"byte-negative-range", "return show(string.byte(\"ABC\", -3, -2))"},
        {"byte-empty-range", "return show(string.byte(\"ABC\", 3, 2))"},
        {"byte-past-end", "return show(string.byte(\"ABC\", 1, 10))"},
        {"byte-zero", "return show(string.byte(\"ABC\", 0))"},
        {"byte-empty-string", "return show(string.byte(\"\", 1, 1))"},
        {"byte-high", "return show(string.byte(string.char(0, 127, 128, 255), 1, 4))"},
        {"byte-default", "return show(string.byte(\"Z\"))"},
        {"byte-only-end", "return show(string.byte(\"ABC\", 2, 2))"},
        {"byte-type", "return show(string.byte(\"ABC\", \"no\"))"},

        {"move-forward",
         "local t = {10, 20, 30, 40}\ntable.move(t, 1, 2, 3)\nreturn show(t[1], t[2], t[3], t[4])"},
        {"move-backward",
         "local t = {10, 20, 30, 40}\ntable.move(t, 3, 4, 2)\nreturn show(t[1], t[2], t[3], t[4])"},
        {"move-other",
         "local a = {1, 2, 3}\nlocal b = {}\ntable.move(a, 1, 3, 2, b)\nreturn show(b[2], b[3], "
         "b[4])"},
        {"move-empty", "local t = {1, 2}\ntable.move(t, 2, 1, 1)\nreturn show(t[1], t[2])"},
        {"move-one", "local t = {7}\ntable.move(t, 1, 1, 2)\nreturn show(t[1], t[2])"},
        {"move-same-spot", "local t = {4, 5}\ntable.move(t, 1, 2, 1)\nreturn show(t[1], t[2])"},
        {"move-not-table", "return show(table.move(1, 1, 1, 1))"},
        {"move-too-many", "return show(table.move({}, 0, 9223372036854775807, 1))"},
        {"move-wrap", "return show(table.move({1}, 1, 2, 9223372036854775807))"},
        {"move-negative", "local t = {9}\ntable.move(t, -1, -1, 1)\nreturn show(t[1])"},

        {"insert-end", "local t = {1, 2}\ntable.insert(t, 3)\nreturn show(t[1], t[2], t[3])"},
        {"insert-middle",
         "local t = {1, 2, 3}\ntable.insert(t, 2, 9)\nreturn show(t[1], t[2], t[3], t[4])"},
        {"insert-front", "local t = {1, 2}\ntable.insert(t, 1, 0)\nreturn show(t[1], t[2], t[3])"},
        {"insert-empty", "local t = {}\ntable.insert(t, \"a\")\nreturn show(t[1])"},
        {"insert-bounds", "return show(table.insert({1}, 3, 1))"},
        {"insert-zero", "return show(table.insert({1}, 0, 1))"},
        {"insert-argc", "return show(table.insert({1}, 1, 1, 1))"},
        {"insert-type", "return show(table.insert(1, 1))"},

        {"remove-last", "local t = {1, 2, 3}\nreturn show(table.remove(t), t[1], t[2], t[3])"},
        {"remove-middle", "local t = {1, 2, 3}\nreturn show(table.remove(t, 2), t[1], t[2], t[3])"},
        {"remove-front", "local t = {1, 2, 3}\nreturn show(table.remove(t, 1), t[1], t[2], t[3])"},
        {"remove-only", "local t = {8}\nreturn show(table.remove(t), t[1])"},
        {"remove-empty", "return show(table.remove({}))"},
        {"remove-bounds", "return show(table.remove({1, 2}, 4))"},
        {"remove-past-end", "local t = {1}\nreturn show(table.remove(t, 2), t[1], t[2])"},

        {"concat-empty", "return show(table.concat({}))"},
        {"concat-one", "return show(table.concat({\"ab\"}))"},
        {"concat-sep", "return show(table.concat({\"a\", \"b\", \"c\"}, \"-\"))"},
        {"concat-range", "return show(table.concat({\"a\", \"b\", \"c\", \"d\"}, \",\", 2, 3))"},
        {"concat-numbers", "return show(table.concat({1, 2, 3}, \":\"))"},
        {"concat-empty-range", "return show(table.concat({\"a\", \"b\"}, \",\", 2, 1))"},
        {"concat-invalid", "return show(table.concat({1, {}, 3}))"},
        {"concat-hole", "return show(table.concat({\"a\", nil, \"c\"}))"},

        {"unpack-all", "return show(table.unpack({10, 20, 30}))"},
        {"unpack-range", "return show(table.unpack({10, 20, 30, 40}, 2, 3))"},
        {"unpack-empty", "return show(table.unpack({1, 2}, 2, 1))"},
        {"unpack-from", "return show(table.unpack({7, 8, 9}, 3))"},
        {"unpack-hole", "return show(table.unpack({1, nil, 3}))"},
        {"unpack-none", "return show(table.unpack({}))"},

        {"sort-reverse",
         "local t = {3, 1, 4, 2}\ntable.sort(t)\nreturn show(t[1], t[2], t[3], t[4])"},
        {"sort-sorted",
         "local t = {1, 2, 3, 4}\ntable.sort(t)\nreturn show(t[1], t[2], t[3], t[4])"},
        {"sort-duplicates",
         "local t = {2, 2, 1, 1, 2}\ntable.sort(t)\nreturn show(t[1], t[2], t[3], t[4], t[5])"},
        {"sort-empty", "local t = {}\ntable.sort(t)\nreturn show(#t)"},
        {"sort-one", "local t = {5}\ntable.sort(t)\nreturn show(t[1])"},
        {"sort-comparator",
         "local t = {1, 3, 2}\ntable.sort(t, function(a, b) return a > b end)\nreturn show(t[1], "
         "t[2], t[3])"},
        {"sort-strings",
         "local t = {\"b\", \"a\", \"c\"}\ntable.sort(t)\nreturn show(t[1], t[2], t[3])"},
        {"sort-invalid",
         "local t = {1, 2, 3}\ntable.sort(t, function() return true end)\nreturn show(t[1])"},

        {"len-ascii", "return show(utf8.len(\"abc\"))"},
        {"len-multibyte", "return show(utf8.len(string.char(0xC3, 0xA9)))"},
        {"len-mixed", "return show(utf8.len(\"a\" .. string.char(0xC3, 0xA9) .. \"b\"))"},
        {"len-empty", "return show(utf8.len(\"\"))"},
        {"len-invalid", "return show(utf8.len(string.char(0x80)))"},
        {"len-range", "return show(utf8.len(\"abcd\", 2, 3))"},
        {"len-lax", "return show(utf8.len(string.char(0xED, 0xA0, 0x80), 1, -1, true))"},

        {"codepoint-ascii", "return show(utf8.codepoint(\"AB\", 1, 2))"},
        {"codepoint-one", "return show(utf8.codepoint(string.char(0xC3, 0xA9)))"},
        {"codepoint-range", "return show(utf8.codepoint(\"abcd\", 2, 3))"},
        {"codepoint-empty", "return show(utf8.codepoint(\"abc\", 3, 2))"},
        {"codepoint-invalid", "return show(utf8.codepoint(string.char(0x80)))"},

        {"offset-next", "return show(utf8.offset(\"abc\", 2))"},
        {"offset-back", "return show(utf8.offset(\"abc\", -1))"},
        {"offset-zero", "return show(utf8.offset(string.char(0xC3, 0xA9), 0, 2))"},
        {"offset-end", "return show(utf8.offset(\"abc\", 4))"},
        {"offset-missing", "return show(utf8.offset(\"abc\", 5))"},
        {"offset-continuation", "return show(utf8.offset(string.char(0xC3, 0xA9), 1, 2))"},

        {"codes-ascii",
         "local parts = {}\nfor position, code in utf8.codes(\"ab\") do\n  parts[#parts + 1] = "
         "show(position, code)\nend\nreturn table.concat(parts, \"|\")"},
        {"codes-multibyte",
         "local text = \"a\" .. string.char(0xC3, 0xA9)\nlocal parts = {}\nfor position, code in "
         "utf8.codes(text) do\n  parts[#parts + 1] = show(position, code)\nend\nreturn "
         "table.concat(parts, \"|\")"},
        {"codes-empty",
         "local n = 0\nfor position, code in utf8.codes(\"\") do\n  n = n + 1\nend\nreturn "
         "show(n)"},
        {"codes-invalid",
         "local n = 0\nfor position, code in utf8.codes(string.char(0x80)) do\n  n = n + "
         "1\nend\nreturn show(n)"},
    };
    for (const char* const* row : rows)
        add_case(cases, row[0], row[1]);
}

lua_State* open_raw() {
    lua_State* state = lua_newstate(counting_allocate, nullptr);
    OA_CHECK(state != nullptr);
    if (state == nullptr)
        return nullptr;
    luaL_requiref(state, "_G", luaopen_base, 1);
    lua_pop(state, 1);
    luaL_requiref(state, "string", luaopen_string, 1);
    lua_pop(state, 1);
    luaL_requiref(state, "table", luaopen_table, 1);
    lua_pop(state, 1);
    luaL_requiref(state, "utf8", luaopen_utf8, 1);
    lua_pop(state, 1);
    return state;
}

std::string run_raw(lua_State* state, const std::string& source, bool* ran) {
    lua_settop(state, 0);
    if (luaL_loadbufferx(state, source.data(), source.size(), "@case", "t") != LUA_OK) {
        *ran = false;
        const char* text = lua_tostring(state, -1);
        std::string message = text != nullptr ? text : "load failed";
        lua_settop(state, 0);
        return message;
    }
    if (lua_pcall(state, 0, 1, 0) != LUA_OK) {
        *ran = false;
        const char* text = lua_tostring(state, -1);
        std::string message = text != nullptr ? text : "call failed";
        lua_settop(state, 0);
        return message;
    }
    *ran = true;
    std::size_t length = 0;
    const char* text = lua_tolstring(state, -1, &length);
    std::string result = text != nullptr ? std::string(text, length) : std::string();
    lua_settop(state, 0);
    return result;
}

void compare_cases(const std::vector<Case>& cases) {
    OA_CHECK(cases.size() >= 60);
    Package package;
    host::ScriptError error;
    auto sandbox = open_with(package, error, compare_profile());
    if (!sandbox)
        return;
    lua_State* raw = open_raw();
    if (raw == nullptr)
        return;
    for (const Case& row : cases) {
        if (sandbox->failed()) {
            sandbox = open_with(package, error, compare_profile());
            if (!sandbox)
                break;
        }
        const std::string source = script_for(row.body);
        bool raw_ran = false;
        const std::string raw_text = run_raw(raw, source, &raw_ran);
        ReadResult read;
        read.text.resize(64 * 1024);
        host::CallHooks hooks;
        hooks.context = &read;
        hooks.read = read_result;
        put(package, "mutator.lua", source);
        const bool ran = sandbox->run_file("mutator.lua", "c-work", &hooks, error);
        if (!ran || !raw_ran || !read.saw || read.overflow) {
            std::fprintf(
                stderr,
                "case %s sandbox %d raw %d saw %d overflow %d kind %d message %s\n",
                row.name.c_str(),
                ran ? 1 : 0,
                raw_ran ? 1 : 0,
                read.saw ? 1 : 0,
                read.overflow ? 1 : 0,
                static_cast<int>(error.kind),
                error.message.c_str()
            );
            OA_CHECK(ran && raw_ran && read.saw && !read.overflow);
            continue;
        }
        const std::string sandbox_text(read.text.data(), read.length);
        if (canonical(sandbox_text) != canonical(raw_text)) {
            std::fprintf(
                stderr,
                "case %s\n sandbox [%s]\n raw [%s]\n",
                row.name.c_str(),
                canonical(sandbox_text).c_str(),
                canonical(raw_text).c_str()
            );
            OA_CHECK(canonical(sandbox_text) == canonical(raw_text));
        }
    }
    lua_close(raw);
}

bool message_has(const host::ScriptError& error, const char* needle) {
    return error.message.find(needle) != std::string::npos;
}

void expect_budget(const char* source) {
    Package package;
    host::ScriptError error;
    auto sandbox = open_with(package, error, host::mutator_profile());
    if (!sandbox)
        return;
    put(package, "mutator.lua", source);
    const auto started = std::chrono::steady_clock::now();
    const bool ran = sandbox->run_file("mutator.lua", "tick", nullptr, error);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    OA_CHECK(!ran);
    OA_CHECK(error.kind == host::ErrorKind::instruction_budget);
    OA_CHECK(message_has(error, "50000000"));
    OA_CHECK(sandbox->instructions_used() >= host::default_instruction_budget);
    OA_CHECK(elapsed < std::chrono::seconds(5));
}

void expect_range(const char* source, const char* function, const char* count) {
    Package package;
    host::ScriptError error;
    auto sandbox = open_with(package, error, host::mutator_profile());
    if (!sandbox)
        return;
    put(package, "mutator.lua", source);
    const auto started = std::chrono::steady_clock::now();
    const bool ran = sandbox->run_file("mutator.lua", "tick", nullptr, error);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    OA_CHECK(!ran);
    OA_CHECK(error.kind == host::ErrorKind::runtime);
    OA_CHECK(message_has(error, function));
    OA_CHECK(message_has(error, "past the most of"));
    OA_CHECK(message_has(error, count));
    OA_CHECK(elapsed < std::chrono::seconds(5));
}

void review_and_replaced_calls_stop() {
    // A string of 1 MiB cannot be built, so the original upper loop is the
    // same call on a string of the new cap.
    Package package;
    host::ScriptError error;
    auto sandbox = open_with(package, error, host::mutator_profile());
    if (sandbox) {
        put(package, "mutator.lua", "string.rep(\"x\", 1048576)\n");
        const auto started = std::chrono::steady_clock::now();
        const bool ran = sandbox->run_file("mutator.lua", "tick", nullptr, error);
        const auto elapsed = std::chrono::steady_clock::now() - started;
        OA_CHECK(!ran);
        OA_CHECK(error.kind == host::ErrorKind::string_length);
        OA_CHECK(message_has(error, "1048576"));
        OA_CHECK(message_has(error, "65536"));
        OA_CHECK(elapsed < std::chrono::seconds(5));
    }

    expect_budget(
        "local s = string.rep(\"x\", 65536)\nwhile true do\n  local u = s:upper()\nend\n"
    );
    expect_range("table.move({}, 1, 0x7ffffffffffffffe, 2)\n", "table.move", "9223372036854775806");
    expect_range(
        "local t = setmetatable({}, { __len = function() return 1 << 62 end })\n"
        "table.insert(t, 1, \"x\")\n",
        "table.insert",
        "4611686018427387904"
    );
    expect_range(
        "local t = setmetatable({}, { __len = function() return 1 << 62 end })\n"
        "table.remove(t, 1)\n",
        "table.remove",
        "4611686018427387903"
    );
    expect_range("table.concat({}, \"\", 1, 1 << 62)\n", "table.concat", "4611686018427387904");
    expect_range("table.unpack({}, 1, 1 << 62)\n", "table.unpack", "4611686018427387904");
    expect_budget(
        "local s = string.rep(\"x\", 8192)\nwhile true do\n  string.byte(s, 1, -1)\nend\n"
    );
    expect_budget(
        "local t = {}\nfor i = 1, 128 do\n  t[i] = 129 - i\nend\nwhile true do\n  "
        "table.sort(t)\nend\n"
    );
    expect_budget("local s = string.rep(\"x\", 65536)\nwhile true do\n  utf8.len(s)\nend\n");
    expect_budget(
        "local s = string.rep(\"x\", 8192)\nwhile true do\n  utf8.codepoint(s, 1, -1)\nend\n"
    );
    expect_budget(
        "local s = string.rep(\"x\", 65536)\nwhile true do\n  utf8.offset(s, 65536)\nend\n"
    );
    expect_budget(
        "local s = string.rep(\"x\", 65536)\nwhile true do\n  for position, code in "
        "utf8.codes(s) do end\nend\n"
    );
}

struct TextResult {
    char text[16]{};
    std::size_t length = 0;
    bool saw = false;
};

void read_text(void* context, lua_State* state, int results) {
    auto* out = static_cast<TextResult*>(context);
    out->saw = results >= 1;
    out->length = 0;
    if (!out->saw || lua_type(state, -1) != LUA_TSTRING)
        return;
    std::size_t length = 0;
    const char* text = lua_tolstring(state, -1, &length);
    if (text == nullptr || length >= sizeof out->text)
        return;
    std::memcpy(out->text, text, length);
    out->length = length;
}

void a_mutator_rewrites_tables() {
    Package package;
    host::ScriptError error;
    auto sandbox = open_with(package, error, host::mutator_profile());
    if (!sandbox)
        return;
    const char* source = "local rows = {}\n"
                         "for i = 1, 400 do\n"
                         "  rows[i] = { text = \"Ridge\" }\n"
                         "end\n"
                         "for i = 1, 400 do\n"
                         "  local text = rows[i].text\n"
                         "  text = string.upper(text)\n"
                         "  text = string.lower(text)\n"
                         "  text = string.reverse(text)\n"
                         "  rows[i].text = text\n"
                         "end\n"
                         "return rows[1].text\n";
    TextResult text;
    host::CallHooks hooks;
    hooks.context = &text;
    hooks.read = read_text;
    put(package, "mutator.lua", source);
    const bool ran = sandbox->run_file("mutator.lua", "tick", &hooks, error);
    OA_CHECK(ran);
    OA_CHECK(text.saw);
    OA_CHECK(text.length == 5);
    OA_CHECK(std::memcmp(text.text, "egdir", 5) == 0);
    OA_CHECK(sandbox->instructions_used() >= 400);
}

} // namespace

int main() {
    std::vector<Case> cases;
    add_cases(cases);
    compare_cases(cases);
    review_and_replaced_calls_stop();
    a_mutator_rewrites_tables();
    return oa::test::check_exit_status();
}
