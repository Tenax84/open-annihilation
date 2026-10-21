// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// One spelling for every float, no place in memory in script text, and no
// way for a script to make a weak table.

#include "lua.hpp"
#include "oa/data/script_host/script_host.hpp"
#include "oa/test/check.hpp"

#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

extern "C" int oa_script_host_number_text(char* buffer, std::size_t size, double value);

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

std::optional<host::Sandbox> open_box(Package& package, host::ScriptError& error) {
    host::OpenOptions options;
    options.package_id = "sample";
    options.profile = host::mutator_profile();
    options.sources.context = &package;
    options.sources.read = read_package;
    auto sandbox = host::Sandbox::open(options, error);
    OA_CHECK(sandbox.has_value());
    return sandbox;
}

bool run_source(
    host::Sandbox& sandbox,
    Package& package,
    const char* source,
    host::ScriptError& error,
    const host::CallHooks* hooks = nullptr
) {
    package.files["mutator.lua"] = std::vector<uint8_t>(source, source + std::strlen(source));
    return sandbox.run_file("mutator.lua", "plan", hooks, error);
}

struct Texts {
    std::string value[16];
    int count = -1;
};

void read_texts(void* context, lua_State* state, int results) {
    auto* out = static_cast<Texts*>(context);
    out->count = results;
    for (int index = 0; index < results && index < 16; ++index) {
        const int slot = -results + index;
        if (lua_isstring(state, slot) != 0)
            out->value[static_cast<std::size_t>(index)] = lua_tostring(state, slot);
    }
}

struct Flag {
    bool value = false;
    bool present = false;
};

void read_flag(void* context, lua_State* state, int results) {
    auto* out = static_cast<Flag*>(context);
    out->present = results == 1 && lua_isboolean(state, -1) != 0;
    out->value = out->present && lua_toboolean(state, -1) != 0;
}

void expect_text(const std::string& got, const char* wanted) {
    OA_CHECK(got == wanted);
    if (got != wanted)
        std::fprintf(stderr, "text: got [%s] wanted [%s]\n", got.c_str(), wanted);
}

void number_text_is_the_same_everywhere() {
    Package package;
    host::ScriptError error;
    auto sandbox = open_box(package, error);
    if (!sandbox)
        return;
    const char* source =
        "local named = setmetatable({}, { __tostring = function() return 'shown' end })\n"
        "local box = setmetatable({}, { __name = 'box' })\n"
        "return tostring(0/0), tostring(-(0/0)), tostring(1/0), tostring(-1/0),\n"
        "  tostring(-0.0), tostring(0.1), tostring(1/3), tostring(100.0),\n"
        "  tostring(1e100), tostring(2^53), 0.1 .. '',\n"
        "  tostring({}), tostring(function() end), tostring(named), tostring(box)\n";
    Texts texts;
    host::CallHooks hooks;
    hooks.context = &texts;
    hooks.read = read_texts;
    OA_CHECK(run_source(*sandbox, package, source, error, &hooks));
    if (!error.message.empty())
        std::fprintf(stderr, "number text: %s\n", error.message.c_str());
    OA_CHECK(texts.count == 15);
    if (texts.count != 15)
        return;
    expect_text(texts.value[0], "nan");
    expect_text(texts.value[1], "nan");
    expect_text(texts.value[2], "inf");
    expect_text(texts.value[3], "-inf");
    expect_text(texts.value[4], "-0.0");
    expect_text(texts.value[5], "0.1");
    expect_text(texts.value[6], "0.33333333333333");
    expect_text(texts.value[7], "100.0");
    expect_text(texts.value[8], "1e+100");
    expect_text(texts.value[9], "9.007199254741e+15");
    expect_text(texts.value[10], "0.1");
    expect_text(texts.value[11], "table: hidden");
    expect_text(texts.value[12], "function: hidden");
    expect_text(texts.value[13], "shown");
    expect_text(texts.value[14], "box: hidden");
}

void format_keeps_finite_text_and_refuses_the_rest() {
    Package package;
    host::ScriptError error;
    auto sandbox = open_box(package, error);
    if (!sandbox)
        return;
    const char* source =
        "return ('%5.2f'):format(3.14159), ('%d'):format(3.0), ('%q'):format(1/0),\n"
        "  ('%q'):format(0/0), ('%%%s'):format(1), ('%%p'):format()\n";
    Texts texts;
    host::CallHooks hooks;
    hooks.context = &texts;
    hooks.read = read_texts;
    OA_CHECK(run_source(*sandbox, package, source, error, &hooks));
    if (!error.message.empty())
        std::fprintf(stderr, "format text: %s\n", error.message.c_str());
    OA_CHECK(texts.count == 6);
    if (texts.count != 6)
        return;
    expect_text(texts.value[0], " 3.14");
    expect_text(texts.value[1], "3");
    expect_text(texts.value[2], "1e9999");
    expect_text(texts.value[3], "(0/0)");
    expect_text(texts.value[4], "%1");
    expect_text(texts.value[5], "%p");

    Package same = package;
    host::ScriptError hex;
    auto hex_box = open_box(same, hex);
    if (!hex_box)
        return;
    Flag equal;
    host::CallHooks flag_hooks;
    flag_hooks.context = &equal;
    flag_hooks.read = read_flag;
    OA_CHECK(
        run_source(*hex_box, same, "return tonumber(('%a'):format(0.1)) == 0.1\n", hex, &flag_hooks)
    );
    OA_CHECK(equal.present);
    OA_CHECK(equal.value);

    // The refusals sit inside the charged copy of string.format, so each is
    // raised at the script's own line, a method call's included.
    struct Refusal {
        const char* source;
        const char* message;
        int line;
    };

    const char* const address = "string.format: %p would show a memory address";
    const Refusal refusals[] = {
        {"string.format('%p', {})\n", address, 1},
        {"local t = {}\nreturn ('%-3p'):format(t)\n", address, 2},
        {"string.format('%.3f', 0/0)\n",
         "string.format: cannot write nan or an infinity with %f; test the number first, or use "
         "tostring",
         1},
        {"('%g'):format(1/0)\n",
         "string.format: cannot write nan or an infinity with %g; test the number first, or use "
         "tostring",
         1},
        {"return ('%a'):format(-(0/0))\n",
         "string.format: cannot write nan or an infinity with %a; test the number first, or use "
         "tostring",
         1},
        {"local x = 1\nreturn string.format('%d %5.1E', x, -1/0)\n",
         "string.format: cannot write nan or an infinity with %E; test the number first, or use "
         "tostring",
         2},
    };
    for (const Refusal& refusal : refusals) {
        Package own;
        host::ScriptError refused;
        auto box = open_box(own, refused);
        if (!box)
            continue;
        OA_CHECK(!run_source(*box, own, refusal.source, refused));
        OA_CHECK(refused.kind == host::ErrorKind::runtime);
        OA_CHECK(refused.line == refusal.line);
        OA_CHECK(refused.message == refusal.message);
        if (refused.message != refusal.message || refused.line != refusal.line) {
            std::fprintf(
                stderr, "format refuse: line %d [%s]\n", refused.line, refused.message.c_str()
            );
        }
    }
}

void pack_is_absent() {
    Package package;
    host::ScriptError error;
    auto sandbox = open_box(package, error);
    if (!sandbox)
        return;
    Flag absent;
    host::CallHooks hooks;
    hooks.context = &absent;
    hooks.read = read_flag;
    const char* source =
        "return string.pack == nil and string.unpack == nil and string.packsize == nil\n";
    OA_CHECK(run_source(*sandbox, package, source, error, &hooks));
    OA_CHECK(absent.present);
    OA_CHECK(absent.value);
}

void direct_number_text_truncates_like_snprintf() {
    const double value = 1.0 / 3.0;
    char full[64];
    const int length = oa_script_host_number_text(full, sizeof full, value);
    OA_CHECK(length > 1);
    OA_CHECK(length < static_cast<int>(sizeof full));
    char library[64];
    const int library_length = std::snprintf(library, sizeof library, "%.14g", value);
    OA_CHECK(length == library_length);
    OA_CHECK(std::strcmp(full, library) == 0);

    char small[64];
    const int again = oa_script_host_number_text(small, static_cast<std::size_t>(length), value);
    OA_CHECK(again == length);
    OA_CHECK(std::strlen(small) == static_cast<std::size_t>(length - 1));
    OA_CHECK(std::memcmp(small, full, static_cast<std::size_t>(length - 1)) == 0);
    OA_CHECK(small[length - 1] == '\0');

    const double negative_nan = std::copysign(std::numeric_limits<double>::quiet_NaN(), -1.0);
    char special[8];
    OA_CHECK(oa_script_host_number_text(special, sizeof special, negative_nan) == 3);
    OA_CHECK(std::strcmp(special, "nan") == 0);
    OA_CHECK(
        oa_script_host_number_text(
            special, sizeof special, std::numeric_limits<double>::infinity()
        ) == 3
    );
    OA_CHECK(std::strcmp(special, "inf") == 0);
    OA_CHECK(
        oa_script_host_number_text(
            special, sizeof special, -std::numeric_limits<double>::infinity()
        ) == 4
    );
    OA_CHECK(std::strcmp(special, "-inf") == 0);
}

void c_collation_orders_strings() {
    Package package;
    host::ScriptError error;
    auto sandbox = open_box(package, error);
    if (!sandbox)
        return;
    Flag before;
    host::CallHooks hooks;
    hooks.context = &before;
    hooks.read = read_flag;
    OA_CHECK(run_source(*sandbox, package, "return 'a' < 'B'\n", error, &hooks));
    OA_CHECK(before.present);
    OA_CHECK(!before.value);
}

/// Puts one locale category back as it was when the case began.
struct LocaleRestore {
    int category;
    std::string saved;

    explicit LocaleRestore(int which) : category(which) {
        const char* now = std::setlocale(category, nullptr);
        saved = now != nullptr ? now : "C";
    }

    ~LocaleRestore() { std::setlocale(category, saved.c_str()); }
};

/// Sets `category` to the first of a few common locales the system has, and
/// checks that a sandbox then refuses to open, naming `words` and the locale.
void another_locale_is_refused(int category, const char* words) {
    LocaleRestore restore(category);
    const char* names[] = {"de_DE.UTF-8", "fr_FR.UTF-8", "en_US.UTF-8", "en_GB.UTF-8"};
    const char* applied = nullptr;
    for (const char* name : names) {
        applied = std::setlocale(category, name);
        if (applied != nullptr)
            break;
    }
    if (applied == nullptr) {
        std::fprintf(
            stderr,
            "data-script-host-text: no %s locale other than C is installed; skipping that case\n",
            words
        );
        return;
    }
    const std::string name = applied;
    Package package;
    host::ScriptError error;
    host::OpenOptions options;
    options.package_id = "sample";
    options.profile = host::mutator_profile();
    options.sources.context = &package;
    options.sources.read = read_package;
    auto sandbox = host::Sandbox::open(options, error);
    OA_CHECK(!sandbox.has_value());
    OA_CHECK(error.kind == host::ErrorKind::runtime);
    OA_CHECK(error.phase == "open");
    OA_CHECK(error.package_id == "sample");
    const std::string expected =
        std::string("the C library's ") + words + " locale is " + name + "; scripts need C";
    OA_CHECK(error.message == expected);
    if (error.message != expected)
        std::fprintf(stderr, "%s: [%s]\n", words, error.message.c_str());
}

// String order follows the collation locale.
void another_collation_is_refused() {
    another_locale_is_refused(LC_COLLATE, "collation");
}

// Reading and writing number text follows the numeric locale: in de_DE,
// 0.5 would not read as a number.
void another_numeric_locale_is_refused() {
    another_locale_is_refused(LC_NUMERIC, "numeric");
}

// upper, lower and the pattern classes follow the character-type locale for
// bytes past 127.
void another_character_type_locale_is_refused() {
    another_locale_is_refused(LC_CTYPE, "character-type");
}

/// Once each category is C again, the same options open a sandbox.
void c_locales_open_again() {
    Package package;
    host::ScriptError error;
    auto sandbox = open_box(package, error);
    OA_CHECK(sandbox.has_value());
    if (!sandbox)
        return;
    Flag half;
    host::CallHooks hooks;
    hooks.context = &half;
    hooks.read = read_flag;
    OA_CHECK(run_source(
        *sandbox,
        package,
        "return tonumber('0.5') == 0.5 and tostring(0.5) == '0.5'\n",
        error,
        &hooks
    ));
    OA_CHECK(half.present);
    OA_CHECK(half.value);
}

int count_entries(lua_State* state) {
    const int index = lua_absindex(state, -1);
    int count = 0;
    lua_pushnil(state);
    while (lua_next(state, index) != 0) {
        ++count;
        lua_pop(state, 1);
    }
    return count;
}

void collect_garbage(void* /*context*/, lua_State* state) {
    lua_gc(state, LUA_GCCOLLECT);
    lua_gc(state, LUA_GCCOLLECT);
}

void read_case(lua_State* state, int slot, int* entries, int* other_entries, int* refused) {
    lua_rawgeti(state, -1, slot);
    lua_getfield(state, -1, "refused");
    *refused = lua_toboolean(state, -1);
    lua_pop(state, 1);
    lua_getfield(state, -1, "probe");
    *entries = count_entries(state);
    lua_pop(state, 1);
    lua_getfield(state, -1, "other");
    *other_entries = count_entries(state);
    lua_pop(state, 1);
    lua_getfield(state, -1, "mode_set");
    OA_CHECK(lua_isnil(state, -1) != 0);
    lua_pop(state, 2);
}

void no_route_makes_a_weak_table() {
    Package package;
    host::ScriptError error;
    auto sandbox = open_box(package, error);
    if (!sandbox)
        return;
    const char* source = "local cases = {}\n"
                         "local function attempt(route, mode)\n"
                         "  local key = {}\n"
                         "  local value = {}\n"
                         "  local mt = {}\n"
                         "  local probe = setmetatable({}, mt)\n"
                         "  local other = setmetatable({}, mt)\n"
                         "  local installed = getmetatable(probe)\n"
                         "  local target = (route == 'shared') and mt or installed\n"
                         "  local ok, err = pcall(function()\n"
                         "    if route == 'rawset' then rawset(target, '__mode', mode)\n"
                         "    else target.__mode = mode end\n"
                         "  end)\n"
                         "  probe[key] = value\n"
                         "  other[key] = value\n"
                         "  probe[1] = 'keep'\n"
                         "  other[1] = 'keep'\n"
                         "  local refused = (not ok) and type(err) == 'string'\n"
                         "    and string.find(err, 'the key __mode is refused', 1, true) ~= nil\n"
                         "  cases[#cases + 1] = {\n"
                         "    route = route, mode = mode, refused = refused,\n"
                         "    probe = probe, other = other,\n"
                         "    mode_set = rawget(getmetatable(probe), '__mode'),\n"
                         "  }\n"
                         "end\n"
                         "for _, mode in ipairs({'k', 'v', 'kv'}) do\n"
                         "  attempt('after', mode)\n"
                         "  attempt('rawset', mode)\n"
                         "  attempt('shared', mode)\n"
                         "end\n"
                         "held = cases\n";
    OA_CHECK(run_source(*sandbox, package, source, error));
    if (!error.message.empty())
        std::fprintf(stderr, "weak setup: %s\n", error.message.c_str());
    if (!error.message.empty())
        return;
    OA_CHECK(sandbox->run_host("collect", nullptr, collect_garbage, error));

    struct Walk {
        int count = 0;
        bool strong = true;
    };

    Walk walk;
    auto read_cases = [](void* context, lua_State* state) {
        auto* out = static_cast<Walk*>(context);
        lua_getglobal(state, "held");
        const int count = static_cast<int>(lua_rawlen(state, -1));
        out->count = count;
        for (int slot = 1; slot <= count; ++slot) {
            int entries = 0;
            int other_entries = 0;
            int refused = 0;
            read_case(state, slot, &entries, &other_entries, &refused);
            lua_rawgeti(state, -1, slot);
            lua_getfield(state, -1, "route");
            const char* route = lua_tostring(state, -1);
            const bool shared = route != nullptr && std::strcmp(route, "shared") == 0;
            lua_pop(state, 2);
            if (entries != 2 || other_entries != 2)
                out->strong = false;
            if (shared && refused != 0)
                out->strong = false;
            if (!shared && refused == 0)
                out->strong = false;
        }
        lua_pop(state, 1);
    };
    OA_CHECK(sandbox->run_host("read", &walk, read_cases, error));
    OA_CHECK(walk.count == 9);
    OA_CHECK(walk.strong);

    Package ordinary;
    host::ScriptError ordinary_error;
    auto ordinary_box = open_box(ordinary, ordinary_error);
    if (!ordinary_box)
        return;
    const char* methods =
        "local calls = 0\n"
        "local methods = {}\n"
        "local value = setmetatable({}, {\n"
        "  __index = methods,\n"
        "  __newindex = function(table, key, item) rawset(table, key, item); calls = calls + 1 "
        "end,\n"
        "  __call = function() return 'called' end,\n"
        "  __tostring = function() return 'shown' end,\n"
        "})\n"
        "methods.number = 3\n"
        "value.extra = 1\n"
        "return value.number, value(), tostring(value), calls, value.extra\n";
    Texts texts;
    host::CallHooks hooks;
    hooks.context = &texts;
    hooks.read = read_texts;
    OA_CHECK(run_source(*ordinary_box, ordinary, methods, ordinary_error, &hooks));
    if (!ordinary_error.message.empty())
        std::fprintf(stderr, "ordinary: %s\n", ordinary_error.message.c_str());
    OA_CHECK(texts.count == 5);
    if (texts.count != 5)
        return;
    expect_text(texts.value[0], "3");
    expect_text(texts.value[1], "called");
    expect_text(texts.value[2], "shown");
    expect_text(texts.value[3], "1");
    expect_text(texts.value[4], "1");
}

} // namespace

int main() {
    number_text_is_the_same_everywhere();
    format_keeps_finite_text_and_refuses_the_rest();
    pack_is_absent();
    direct_number_text_truncates_like_snprintf();
    c_collation_orders_strings();
    another_collation_is_refused();
    another_numeric_locale_is_refused();
    another_character_type_locale_is_refused();
    c_locales_open_again();
    no_route_makes_a_weak_table();
    return oa::test::check_exit_status();
}
