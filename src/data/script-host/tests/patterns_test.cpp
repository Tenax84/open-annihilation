// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// string.find, string.match, string.gmatch and string.gsub against Lua's own
// functions, then a match that spends the instruction allowance.

#include "lua.hpp"
#include "oa/data/script_host/script_host.hpp"
#include "oa/test/check.hpp"

#include <cctype>
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

/// Copies the chunk's one string result. Does not allocate.
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

host::Profile pattern_profile() {
    host::Budget budget;
    budget.instructions = 2'000'000;
    budget.memory_bytes = std::size_t{8} << 20;
    return host::Profile{"test", budget, nullptr, nullptr};
}

std::optional<host::Sandbox> open_sample(Package& package, host::ScriptError& error) {
    host::OpenOptions options;
    options.package_id = "sample";
    options.profile = pattern_profile();
    options.sources.context = &package;
    options.sources.read = read_package;
    return host::Sandbox::open(options, error);
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
local function collect(...)
  local rows = {}
  local iterator = string.gmatch(...)
  while true do
    local values = {iterator()}
    if values[1] == nil then break end
    rows[#rows + 1] = show(table.unpack(values))
  end
  return table.concat(rows, "|")
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

/// Drops one "chunk:line: " prefix so two states can use different chunk names.
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

void add_named(std::vector<Case>& cases, std::string name, std::string body) {
    add_case(cases, std::move(name), std::move(body));
}

void add_class_sweeps_named(std::vector<Case>& cases) {
    const char* letters = "acdglpsuwxz";
    for (const char* letter = letters; *letter != '\0'; ++letter) {
        const char lower[] = {'%', *letter, '\0'};
        const char upper[] = {
            '%', static_cast<char>(std::toupper(static_cast<unsigned char>(*letter))), '\0'
        };
        const char* forms[] = {lower, upper};
        const char* which[] = {"class", "complement"};
        for (int form = 0; form < 2; ++form) {
            std::string body = std::string(
                                   "local parts = {}\n"
                                   "local pattern = [["
                               ) +
                               forms[form] +
                               "]]\n"
                               "for byte = 0, 255 do\n"
                               "  local subject = string.char(byte)\n"
                               "  parts[#parts + 1] = show(string.find(subject, pattern))\n"
                               "  parts[#parts + 1] = show(string.match(subject, pattern))\n"
                               "end\n"
                               "return table.concat(parts, \"\\n\")\n";
            add_named(
                cases,
                std::string(which[form]) + "-" + forms[form][1] + "-every-byte",
                std::move(body)
            );
        }
    }
}

void add_grid(std::vector<Case>& cases) {
    const char* subjects[] = {
        "\"\"", "\"a\"", "\"aaa\"", "\"abc\"", "\"b\"", "\"a1\"", "\" a\"", "\"a b c\""
    };
    const char* patterns[] = {
        "\".\"",
        "\"a*\"",
        "\"a+\"",
        "\"a-\"",
        "\"a?\"",
        "\".*\"",
        "\".+\"",
        "\".-\"",
        "\".?\"",
        "\"%a*\"",
        "\"%d+\"",
        "\"%w-\"",
        "\"[a-c]\"",
        "\"[^a-c]\"",
        "\"[%d]\"",
        "\"[%a%d]\"",
        "\"[a-c%d]\"",
        "\"[^%w]\""
    };
    for (const char* subject : subjects) {
        for (const char* pattern : patterns) {
            const std::string call = std::string("string.find(") + subject + ", " + pattern + ")";
            add_named(
                cases,
                std::string("grid-find-") + subject + "-" + pattern,
                "return show(" + call + ")"
            );
            const std::string match_call =
                std::string("string.match(") + subject + ", " + pattern + ")";
            add_named(
                cases,
                std::string("grid-match-") + subject + "-" + pattern,
                "return show(" + match_call + ")"
            );
        }
    }
}

void add_explicit(std::vector<Case>& cases) {
    const struct Row {
        const char* name;
        const char* body;
    } rows[] = {
        {"anchor-start", "return show(string.find(\"abc\", \"^a\"))"},
        {"anchor-start-miss", "return show(string.find(\"abc\", \"^b\"))"},
        {"anchor-end", "return show(string.find(\"abc\", \"c$\"))"},
        {"anchor-end-miss", "return show(string.find(\"abc\", \"b$\"))"},
        {"anchor-both", "return show(string.match(\"abc\", \"^abc$\"))"},
        {"anchor-empty", "return show(string.match(\"abc\", \"^\"))"},
        {"anchor-dollar-only", "return show(string.match(\"abc\", \"$\"))"},
        {"anchor-gsub", "return show(string.gsub(\"aaa\", \"^a\", \"b\"))"},
        {"capture-nested", "return show(string.match(\"abc\", \"(a(b)c)\"))"},
        {"capture-empty", "return show(string.match(\"abc\", \"a()b\"))"},
        {"capture-whole", "return show(string.match(\"abc\", \"(a)\"))"},
        {"capture-find", "return show(string.find(\"abc\", \"(a)(b)\"))"},
        {"capture-two", "return show(string.match(\"abcd\", \"(b)(c)\"))"},
        {"position-only", "return show(string.match(\"abc\", \"()\"))"},
        {"position-middle", "return show(string.match(\"abc\", \"a()c\"))"},
        {"position-find", "return show(string.find(\"abc\", \"()b()\"))"},
        {"position-gsub", "return show(string.gsub(\"abc\", \"()b\", \"[%0]\"))"},
        {"backref-repeat", "return show(string.match(\"abcabc\", \"(abc)%1\"))"},
        {"backref-pair", "return show(string.match(\"abab\", \"(a)(b)%1%2\"))"},
        {"backref-miss", "return show(string.match(\"abba\", \"(a)(b)%1\"))"},
        {"backref-digit", "return show(string.match(\"aaa\", \"(a)%1\"))"},
        {"balance-nested", "return show(string.match(\"a(b(c)d)e\", \"%b()\"))"},
        {"balance-simple", "return show(string.match(\"(a)\", \"%b()\"))"},
        {"balance-miss", "return show(string.match(\"a\", \"%b()\"))"},
        {"balance-open", "return show(string.match(\"((\", \"%b()\"))"},
        {"balance-find", "return show(string.find(\"x(y)z\", \"%b()\"))"},
        {"balance-other", "return show(string.match(\"<a<b>c>\", \"%b<>\"))"},
        {"frontier-start", "return show(string.find(\"a\", \"%f[%a]\"))"},
        {"frontier-space", "return show(string.find(\" a\", \"%f[%a]\"))"},
        {"frontier-digit", "return show(string.match(\"a1\", \"%f[%d]\"))"},
        {"frontier-word", "return show(string.find(\"word\", \"%f[%w]\"))"},
        {"frontier-inside", "return show(string.find(\"ab\", \"%f[%w]\", 2))"},
        {"frontier-miss", "return show(string.find(\"ab\", \"%f[%d]\"))"},
        {"init-two", "return show(string.find(\"abc\", \"b\", 2))"},
        {"init-negative", "return show(string.find(\"abc\", \"b\", -2))"},
        {"init-zero", "return show(string.find(\"abc\", \"a\", 0))"},
        {"init-past", "return show(string.find(\"abc\", \"a\", 5))"},
        {"init-far", "return show(string.find(\"abc\", \"a\", -10))"},
        {"init-match", "return show(string.match(\"abc\", \"c\", -1))"},
        {"init-match-past", "return show(string.match(\"abc\", \"a\", 4))"},
        {"plain-dot", "return show(string.find(\"a.b\", \".\", 1, true))"},
        {"plain-dot-pattern", "return show(string.find(\"a.b\", \".\"))"},
        {"plain-plus", "return show(string.find(\"a+b\", \"+\", 1, true))"},
        {"plain-word", "return show(string.find(\"hello\", \"ell\"))"},
        {"plain-init", "return show(string.find(\"hello\", \"l\", 4, true))"},
        {"plain-nul", "return show(string.find(\"a\\0b\", \"\\0\", 1, true))"},
        {"plain-false", "return show(string.find(\"a.b\", \".\", 1, false))"},
        {"plain-truthy", "return show(string.find(\"a+b\", \"+\", 1, 1))"},
        {"gmatch-words", "return collect(\"a b c\", \"%a+\")"},
        {"gmatch-init", "return collect(\"abcde\", \"%a\", 3)"},
        {"gmatch-init-negative", "return collect(\"abcde\", \"%a\", -2)"},
        {"gmatch-init-zero", "return collect(\"abc\", \"%a\", 0)"},
        {"gmatch-init-past", "return collect(\"abc\", \"%a\", 100)"},
        {"gmatch-anchor", "return collect(\"abc\", \"^a\")"},
        {"gmatch-anchor-class", "return collect(\"abc\", \"^%a+\")"},
        {"gmatch-caret-literal", "return collect(\"^abc\", \"%^a\")"},
        {"gmatch-two", "return collect(\"a1b2\", \"(%a)(%d)\")"},
        {"gmatch-empty", "return collect(\"abc\", \"\")"},
        {"gsub-literal", "return show(string.gsub(\"abc\", \"b\", \"B\"))"},
        {"gsub-percent-zero", "return show(string.gsub(\"ab cd\", \"%a+\", \"[%0]\"))"},
        {"gsub-percent-one", "return show(string.gsub(\"ab\", \"(%a)\", \"%1%1\"))"},
        {"gsub-percent-percent", "return show(string.gsub(\"ab\", \"b\", \"%%\"))"},
        {"gsub-number", "return show(string.gsub(\"a\", \"a\", 5))"},
        {"gsub-table", "return show(string.gsub(\"ab\", \".\", {a = \"A\", b = \"B\"}))"},
        {"gsub-table-miss", "return show(string.gsub(\"ab\", \".\", {a = \"A\"}))"},
        {"gsub-table-false", "return show(string.gsub(\"ab\", \".\", {a = false, b = \"B\"}))"},
        {"gsub-function-false",
         "return show(string.gsub(\"ab\", \".\", function() return false end))"},
        {"gsub-function-nil", "return show(string.gsub(\"ab\", \".\", function() return nil end))"},
        {"gsub-function-text",
         "return show(string.gsub(\"ab\", \".\", function(ch) return ch:upper() end))"},
        {"gsub-function-number", "return show(string.gsub(\"a\", \"a\", function() return 1 end))"},
        {"gsub-limit", "return show(string.gsub(\"aaaa\", \"a\", \"b\", 2))"},
        {"gsub-limit-zero", "return show(string.gsub(\"aaaa\", \"a\", \"b\", 0))"},
        {"gsub-limit-negative", "return show(string.gsub(\"aaaa\", \"a\", \"b\", -1))"},
        {"gsub-empty-pattern", "return show(string.gsub(\"ab\", \"\", \"-\"))"},
        {"method-find", "return show((\"abc\"):find(\"b\"))"},
        {"method-match", "return show((\"abc\"):match(\"b\"))"},
        {"method-gmatch", "return collect((\"a b\"), \"%a\")"},
        {"method-gsub", "return show((\"ab\"):gsub(\"b\", \"B\"))"},
        {"empty-find", "return show(string.find(\"\", \"\"))"},
        {"empty-subject", "return show(string.find(\"\", \"a\"))"},
        {"empty-pattern", "return show(string.find(\"a\", \"\"))"},
        {"set-escaped", "return show(string.match(\"a]b\", \"[%]]\"))"},
        {"set-dash", "return show(string.match(\"a-b\", \"[%-]\"))"},
        {"set-dot", "return show(string.match(\"a.b\", \"[.]\"))"},
        {"error-ends-percent", "return show(string.find(\"a\", \"%\"))"},
        {"error-missing-bracket", "return show(string.find(\"a\", \"[a\"))"},
        {"error-percent-in-set", "return show(string.find(\"a\", \"[%\"))"},
        {"error-frontier", "return show(string.find(\"a\", \"%f%a\"))"},
        {"error-frontier-end", "return show(string.match(\"a\", \"%f\"))"},
        {"error-balance", "return show(string.find(\"a\", \"%b\"))"},
        {"error-balance-one", "return show(string.find(\"a\", \"%b(\"))"},
        {"error-capture-index", "return show(string.match(\"abc\", \"%1\"))"},
        {"error-capture-zero", "return show(string.match(\"abc\", \"%0\"))"},
        {"error-pattern-capture", "return show(string.match(\"abc\", \")\"))"},
        {"error-unfinished", "return show(string.match(\"abc\", \"(a\"))"},
        {"error-too-many",
         "return show(string.match(\"a\", string.rep(\"(\", 40) .. \"a\" .. string.rep(\")\", "
         "40)))"},
        {"error-too-complex", "return show(string.match(\"\", string.rep(\"a?\", 250)))"},
        {"error-replacement-boolean",
         "return show(string.gsub(\"a\", \"a\", function() return true end))"},
        {"error-replacement-table",
         "return show(string.gsub(\"a\", \"a\", function() return {} end))"},
        {"error-replacement-percent", "return show(string.gsub(\"a\", \"a\", \"%z\"))"},
        {"error-replacement-trailing", "return show(string.gsub(\"a\", \"a\", \"%\"))"},
        {"error-gsub-type", "return show(string.gsub(\"a\", \"a\", true))"},
        {"dot-middle", "return show(string.match(\"abc\", \"a.c\"))"},
        {"dot-find", "return show(string.find(\"abc\", \".\"))"},
        {"quantifier-star", "return show(string.match(\"aaa\", \"a*\"))"},
        {"quantifier-plus", "return show(string.match(\"aaa\", \"a+\"))"},
        {"quantifier-lazy", "return show(string.match(\"aaa\", \"a-a\"))"},
        {"quantifier-optional", "return show(string.match(\"ab\", \"a?b\"))"},
        {"quantifier-optional-empty", "return show(string.match(\"b\", \"a?b\"))"},
    };

    for (const Row& row : rows)
        add_case(cases, row.name, row.body);
}

std::vector<Case> all_cases() {
    std::vector<Case> cases;
    cases.reserve(400);
    add_class_sweeps_named(cases);
    add_grid(cases);
    add_explicit(cases);
    return cases;
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
    OA_CHECK(cases.size() >= 150);
    Package package;
    host::ScriptError error;
    auto sandbox = open_sample(package, error);
    OA_CHECK(sandbox.has_value());
    if (!sandbox)
        return;
    lua_State* raw = open_raw();
    if (raw == nullptr)
        return;
    for (const Case& row : cases) {
        if (sandbox->failed()) {
            sandbox = open_sample(package, error);
            if (!sandbox)
                break;
        }
        const std::string source = script_for(row.body);
        bool raw_ran = false;
        const std::string raw_text = run_raw(raw, source, &raw_ran);
        ReadResult read;
        read.text.resize(512 * 1024);
        host::CallHooks hooks;
        hooks.context = &read;
        hooks.read = read_result;
        put(package, "mutator.lua", source);
        const bool ran = sandbox->run_file("mutator.lua", "patterns", &hooks, error);
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

void budget_stops_a_backtracking_match() {
    const char* sources[] = {
        "string.find((\"a\"):rep(30000), (\"a*\"):rep(12) .. \"b\")\n",
        "pcall(function() string.find((\"a\"):rep(30000), (\"a*\"):rep(12) .. \"b\") end)\n"
        "return \"carried on\"\n",
    };
    for (const char* source : sources) {
        Package package;
        host::ScriptError error;
        auto sandbox = open_sample(package, error);
        OA_CHECK(sandbox.has_value());
        if (!sandbox)
            return;
        put(package, "mutator.lua", source);
        const auto started = std::chrono::steady_clock::now();
        const bool ran = sandbox->run_file("mutator.lua", "patterns", nullptr, error);
        const auto elapsed = std::chrono::steady_clock::now() - started;
        OA_CHECK(!ran);
        OA_CHECK(error.kind == host::ErrorKind::instruction_budget);
        OA_CHECK(error.message == "the script ran past its budget of 2000000 instructions");
        OA_CHECK(sandbox->instructions_used() >= 2'000'000);
        OA_CHECK(elapsed < std::chrono::seconds(5));
    }
}

void a_long_match_stays_inside_the_allowance() {
    Package package;
    host::ScriptError error;
    auto sandbox = open_sample(package, error);
    OA_CHECK(sandbox.has_value());
    if (!sandbox)
        return;
    lua_State* raw = open_raw();
    if (raw == nullptr)
        return;
    const char* body = "return show(((\"x \"):rep(30000)):gsub(\"%s+\", \"_\"))";
    const std::string source = script_for(body);
    bool raw_ran = false;
    const std::string raw_text = run_raw(raw, source, &raw_ran);
    ReadResult read;
    read.text.resize(512 * 1024);
    host::CallHooks hooks;
    hooks.context = &read;
    hooks.read = read_result;
    put(package, "mutator.lua", source);
    const bool ran = sandbox->run_file("mutator.lua", "patterns", &hooks, error);
    OA_CHECK(ran);
    OA_CHECK(raw_ran);
    OA_CHECK(read.saw);
    if (ran && raw_ran && read.saw) {
        const std::string sandbox_text(read.text.data(), read.length);
        OA_CHECK(canonical(sandbox_text) == canonical(raw_text));
    }
    const uint64_t with_match = sandbox->instructions_used();

    Package plain;
    host::ScriptError plain_error;
    auto other = open_sample(plain, plain_error);
    OA_CHECK(other.has_value());
    if (!other) {
        lua_close(raw);
        return;
    }
    put(plain, "mutator.lua", "return #((\"x \"):rep(30000))\n");
    OA_CHECK(other->run_file("mutator.lua", "patterns", nullptr, plain_error));
    OA_CHECK(with_match > other->instructions_used());
    lua_close(raw);
}

} // namespace

int main() {
    const std::vector<Case> cases = all_cases();
    compare_cases(cases);
    budget_stops_a_backtracking_match();
    a_long_match_stays_inside_the_allowance();
    return oa::test::check_exit_status();
}
