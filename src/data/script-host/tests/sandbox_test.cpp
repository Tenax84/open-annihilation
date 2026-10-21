// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The sandbox: which libraries a script can see, require limited to the
// package's lua/ files, errors that name the chunk and the line, and two
// profiles that do not share a state.

#include "lua.hpp"
#include "oa/data/script_host/script_host.hpp"
#include "oa/test/check.hpp"

#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

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

void put(Package& package, std::string path, std::string_view source) {
    package.files[std::move(path)] = std::vector<uint8_t>(source.begin(), source.end());
}

std::optional<host::Sandbox> open_sample(
    Package& package, host::ScriptError& error, host::Profile profile = host::mutator_profile()
) {
    host::OpenOptions options;
    options.package_id = "sample";
    options.profile = profile;
    options.sources.context = &package;
    options.sources.read = read_package;
    auto sandbox = host::Sandbox::open(options, error);
    OA_CHECK(sandbox.has_value());
    return sandbox;
}

bool run_source(
    host::Sandbox& sandbox,
    Package& package,
    const char* path,
    std::string_view source,
    const char* phase,
    host::ScriptError& error,
    const host::CallHooks* hooks = nullptr
) {
    put(package, path, source);
    return sandbox.run_file(path, phase, hooks, error);
}

/// Standard output or standard error diverted into a temporary file.
struct StreamCapture {
    FILE* file = nullptr;
    FILE* stream = nullptr;
    int saved = -1;

    /// Sends `target` into a temporary file.
    ///
    /// @param target stdout or stderr
    /// @return false when the file or the diversion could not be made
    bool begin(FILE* target) {
        stream = target;
#ifdef _MSC_VER
        if (tmpfile_s(&file) != 0)
            file = nullptr;
#else
        file = std::tmpfile();
#endif
        if (file == nullptr)
            return false;
#ifdef _WIN32
        const int from = _fileno(stream);
        const int into = _fileno(file);
        saved = _dup(from);
        if (saved < 0 || _dup2(into, from) != 0) {
#else
        const int from = fileno(stream);
        const int into = fileno(file);
        saved = dup(from);
        if (saved < 0 || dup2(into, from) < 0) {
#endif
            if (saved >= 0) {
#ifdef _WIN32
                _close(saved);
#else
                close(saved);
#endif
                saved = -1;
            }
            std::fclose(file);
            file = nullptr;
            return false;
        }
        return true;
    }

    /// Puts the stream back.
    void end() {
        if (saved < 0)
            return;
        std::fflush(stream);
#ifdef _WIN32
        _dup2(saved, _fileno(stream));
        _close(saved);
#else
        dup2(saved, fileno(stream));
        close(saved);
#endif
        saved = -1;
    }

    /// The text written since begin.
    ///
    /// @return the captured text; empty when begin did not succeed
    std::string text() {
        if (stream != nullptr)
            std::fflush(stream);
        if (file == nullptr)
            return {};
        std::fflush(file);
        std::rewind(file);
        std::string value;
        char buffer[256];
        while (std::fgets(buffer, static_cast<int>(sizeof buffer), file) != nullptr)
            value += buffer;
        return value;
    }

    ~StreamCapture() {
        end();
        if (file != nullptr)
            std::fclose(file);
    }
};

struct Sum {
    int value = 0;
    int results = -1;
};

int push_add(void* /*context*/, lua_State* state) {
    lua_getglobal(state, "add");
    lua_pushinteger(state, 20);
    lua_pushinteger(state, 22);
    return 2;
}

void read_add(void* context, lua_State* state, int results) {
    auto* sum = static_cast<Sum*>(context);
    sum->results = results;
    if (results == 1 && lua_isinteger(state, -1))
        sum->value = static_cast<int>(lua_tointeger(state, -1));
}

void call_reads_the_result() {
    Package package;
    host::ScriptError error;
    auto sandbox = open_sample(package, error);
    if (!sandbox)
        return;
    const bool defined = run_source(
        *sandbox,
        package,
        "mutator.lua",
        "function add(left, right)\n  return left + right\nend\n",
        "plan",
        error
    );
    OA_CHECK(defined);
    Sum sum;
    host::CallHooks hooks;
    hooks.context = &sum;
    hooks.push = push_add;
    hooks.read = read_add;
    OA_CHECK(sandbox->call("plan", hooks, error));
    OA_CHECK(sum.results == 1);
    OA_CHECK(sum.value == 42);
}

void read_absent(void* /*context*/, lua_State* state, int results) {
    OA_CHECK(results == 10);
    for (int index = 0; index < results; ++index)
        OA_CHECK(lua_isnil(state, -results + index));
}

void read_present(void* /*context*/, lua_State* state, int results) {
    OA_CHECK(results == 8);
    OA_CHECK(results >= 1 && std::strcmp(lua_tostring(state, -8), "AB") == 0);
    OA_CHECK(results >= 2 && std::strcmp(lua_tostring(state, -7), "ab") == 0);
    OA_CHECK(results >= 3 && std::strcmp(lua_tostring(state, -6), "A") == 0);
    OA_CHECK(results >= 4 && lua_toboolean(state, -5) == 1);
    OA_CHECK(results >= 5 && lua_tointeger(state, -4) == 7);
    OA_CHECK(results >= 6 && std::strcmp(lua_tostring(state, -3), "b") == 0);
    OA_CHECK(results >= 7 && std::strcmp(lua_tostring(state, -2), "function") == 0);
    OA_CHECK(results >= 8 && std::strcmp(lua_tostring(state, -1), "table") == 0);
}

void libraries_match_the_sandbox() {
    Package package;
    host::ScriptError error;
    auto sandbox = open_sample(package, error);
    if (!sandbox)
        return;
    host::CallHooks absent;
    absent.read = read_absent;
    const bool closed = run_source(
        *sandbox,
        package,
        "mutator.lua",
        "return load, loadfile, dofile, collectgarbage, string.dump, io, os, debug, package, "
        "coroutine\n",
        "plan",
        error,
        &absent
    );
    OA_CHECK(closed);
    host::CallHooks present;
    present.read = read_present;
    const bool open = run_source(
        *sandbox,
        package,
        "present.lua",
        "local upper = string.upper(\"ab\")\n"
        "local joined = table.concat({\"a\", \"b\"})\n"
        "local letter = utf8.char(65)\n"
        "local ok, value = pcall(function() return 7 end)\n"
        "local selected = select(2, \"a\", \"b\", \"c\")\n"
        "local listed = ipairs({})\n"
        "return upper, joined, letter, ok, value, selected, type(listed), type(math)\n",
        "plan",
        error,
        &present
    );
    OA_CHECK(open);
}

void read_length(void* context, lua_State* state, int results) {
    auto* length = static_cast<int*>(context);
    if (results == 1 && lua_isinteger(state, -1))
        *length = static_cast<int>(lua_tointeger(state, -1));
}

void string_rep_obeys_the_limit() {
    Package package;
    host::ScriptError error;
    auto sandbox = open_sample(package, error);
    if (!sandbox)
        return;
    int length = 0;
    host::CallHooks hooks;
    hooks.context = &length;
    hooks.read = read_length;
    const bool built = run_source(
        *sandbox,
        package,
        "mutator.lua",
        "return #string.rep(\"ab\", 1000, \",\")\n",
        "plan",
        error,
        &hooks
    );
    OA_CHECK(built);
    OA_CHECK(length == 2999);

    struct Repeated {
        std::string first;
        std::string second;
        int results = 0;
    } repeated;

    host::CallHooks empty_rep;
    empty_rep.context = &repeated;
    empty_rep.read = [](void* context, lua_State* state, int results) {
        auto* out = static_cast<Repeated*>(context);
        out->results = results;
        if (results >= 1 && lua_isstring(state, -results))
            out->first = lua_tostring(state, -results);
        if (results >= 2 && lua_isstring(state, -results + 1))
            out->second = lua_tostring(state, -results + 1);
    };
    const bool empty = run_source(
        *sandbox,
        package,
        "empty.lua",
        "return string.rep(\"\", 0x7fffffffffffffff), string.rep(\"\", 0x7fffffffffffffff, \"\")\n",
        "plan",
        error,
        &empty_rep
    );
    OA_CHECK(empty);
    OA_CHECK(repeated.results == 2);
    OA_CHECK(repeated.first.empty());
    OA_CHECK(repeated.second.empty());

    int capped = 0;
    host::CallHooks cap_hooks;
    cap_hooks.context = &capped;
    cap_hooks.read = read_length;
    const bool at_cap = run_source(
        *sandbox,
        package,
        "cap.lua",
        "return #string.rep(\"a\", 65536)\n",
        "plan",
        error,
        &cap_hooks
    );
    OA_CHECK(at_cap);
    OA_CHECK(capped == 65536);

    const char* sources[] = {
        "local marker = 1\nstring.rep(\"a\", 65537)\n",
        "local marker = 1\nstring.rep(\"a\", 32769, \"b\")\n",
    };
    for (const char* source : sources) {
        Package own;
        host::ScriptError refused;
        auto limited = open_sample(own, refused);
        if (!limited)
            continue;
        OA_CHECK(!run_source(*limited, own, "mutator.lua", source, "plan", refused));
        OA_CHECK(refused.kind == host::ErrorKind::string_length);
        OA_CHECK(refused.chunk == "mutator.lua");
        OA_CHECK(refused.line == 2);
        OA_CHECK(
            refused.message == "string.rep would make a string of 65537 bytes; the most is 65536"
        );
    }
}

void metatable_keys_are_refused() {
    const char* sources[] = {
        "local function reclaim() end\nsetmetatable({}, { __gc = reclaim })\n",
        "setmetatable({}, { __mode = \"k\" })\n",
        "local value = {}\nrawset(value, \"__gc\", function() end)\n",
        "local value = {}\nrawset(value, \"__mode\", \"v\")\n",
    };
    const char* messages[] = {
        "setmetatable: a metatable with __gc is refused",
        "setmetatable: a metatable with __mode is refused",
        "rawset: the key __gc is refused",
        "rawset: the key __mode is refused",
    };
    const int lines[] = {2, 1, 2, 2};
    for (int index = 0; index < 4; ++index) {
        Package package;
        host::ScriptError error;
        auto sandbox = open_sample(package, error);
        if (!sandbox)
            continue;
        OA_CHECK(!run_source(*sandbox, package, "mutator.lua", sources[index], "plan", error));
        OA_CHECK(error.kind == host::ErrorKind::runtime);
        OA_CHECK(error.line == lines[index]);
        OA_CHECK(error.message == messages[index]);
    }

    Package package;
    host::ScriptError error;
    auto sandbox = open_sample(package, error);
    if (!sandbox)
        return;
    int value = 0;
    host::CallHooks hooks;
    hooks.context = &value;
    hooks.read = read_length;
    const bool indexed = run_source(
        *sandbox,
        package,
        "mutator.lua",
        "local value = setmetatable({}, { __index = { number = 3 }, __tostring = function() return "
        "\"shown\" end })\n"
        "return value.number\n",
        "plan",
        error,
        &hooks
    );
    OA_CHECK(indexed);
    OA_CHECK(value == 3);

    std::string shown;
    host::CallHooks text;
    text.context = &shown;
    text.read = [](void* context, lua_State* state, int results) {
        auto* out = static_cast<std::string*>(context);
        if (results == 1 && lua_isstring(state, -1))
            *out = lua_tostring(state, -1);
    };
    const bool named = run_source(
        *sandbox,
        package,
        "shown.lua",
        "local value = setmetatable({}, { __tostring = function() return \"shown\" end })\n"
        "return tostring(value)\n",
        "plan",
        error,
        &text
    );
    OA_CHECK(named);
    OA_CHECK(shown == "shown");

    Package locked_package;
    host::ScriptError locked;
    auto protected_table = open_sample(locked_package, locked);
    if (!protected_table)
        return;
    OA_CHECK(!run_source(
        *protected_table,
        locked_package,
        "mutator.lua",
        "local value = {}\nsetmetatable(value, { __metatable = \"locked\" })\nsetmetatable(value, "
        "{})\n",
        "plan",
        locked
    ));
    OA_CHECK(locked.kind == host::ErrorKind::runtime);
    OA_CHECK(locked.line == 3);
    OA_CHECK(locked.message == "cannot change a protected metatable");
}

bool prints_match(const host::Sandbox& sandbox, std::size_t count, const char* last) {
    const auto lines = sandbox.printed();
    if (lines.size() != count)
        return false;
    if (count == 0)
        return true;
    return lines[count - 1] == last;
}

void print_keeps_lines_and_writes_nothing() {
    StreamCapture out;
    StreamCapture err;
    if (!out.begin(stdout) || !err.begin(stderr)) {
        out.end();
        err.end();
        OA_CHECK(false);
        return;
    }

    bool tabs = false;
    bool empty = false;
    bool full = false;
    bool line_cap = false;
    bool byte_cap = false;
    bool note_fits = false;
    {
        Package package;
        host::ScriptError error;
        auto sandbox = open_sample(package, error);
        if (sandbox) {
            tabs = run_source(
                *sandbox, package, "mutator.lua", "print(\"a\", \"b\", 3)\nprint()\n", "plan", error
            );
            const auto lines = sandbox->printed();
            tabs = tabs && lines.size() == 2 && lines[0] == "a\tb\t3" && lines[1].empty();
            empty = lines.size() == 2 && lines[1].empty();
        }
    }
    {
        Package package;
        host::ScriptError error;
        auto sandbox = open_sample(package, error);
        if (sandbox) {
            std::string source;
            for (int line = 0; line < 256; ++line)
                source += "print(\"x\")\n";
            full = run_source(*sandbox, package, "mutator.lua", source, "plan", error);
            full = full && prints_match(*sandbox, 256, "x");
        }
    }
    {
        Package package;
        host::ScriptError error;
        auto sandbox = open_sample(package, error);
        if (sandbox) {
            std::string source;
            for (int line = 0; line < 257; ++line)
                source += "print(\"x\")\n";
            line_cap = run_source(*sandbox, package, "mutator.lua", source, "plan", error);
            const auto lines = sandbox->printed();
            line_cap = line_cap && lines.size() == 256 && lines[0] == "x" && lines[254] == "x" &&
                       lines[255] == "2 lines left out";
        }
    }
    {
        Package package;
        host::ScriptError error;
        auto sandbox = open_sample(package, error);
        if (sandbox) {
            const bool ran = run_source(
                *sandbox,
                package,
                "mutator.lua",
                "print(string.rep(\"x\", 16384))\nprint(\"y\")\n",
                "plan",
                error
            );
            const auto lines = sandbox->printed();
            byte_cap = ran && lines.size() == 1 && lines[0] == "2 lines left out";
        }
    }
    {
        Package package;
        host::ScriptError error;
        auto sandbox = open_sample(package, error);
        if (sandbox) {
            const bool ran = run_source(
                *sandbox,
                package,
                "mutator.lua",
                "for i = 1, 100 do print(\"x\") end\nprint(string.rep(\"y\", 20000))\n",
                "plan",
                error
            );
            const auto lines = sandbox->printed();
            note_fits =
                ran && lines.size() == 101 && lines[0] == "x" && lines[100] == "1 line left out";
        }
    }
    bool long_text = false;
    {
        Package package;
        host::ScriptError error;
        auto sandbox = open_sample(package, error);
        if (sandbox) {
            const bool ran = run_source(
                *sandbox,
                package,
                "mutator.lua",
                "print((\"y\"):rep(2000))\n"
                "print((\"y\"):rep(2000), (\"z\"):rep(2000), (\"w\"):rep(2000))\n",
                "plan",
                error
            );
            const auto lines = sandbox->printed();
            const std::string y(2000, 'y');
            const std::string joined =
                y + '\t' + std::string(2000, 'z') + '\t' + std::string(2000, 'w');
            long_text = ran && lines.size() == 2 && lines[0] == y && lines[1] == joined;
        }
    }

    out.end();
    err.end();
    OA_CHECK(out.text().empty());
    OA_CHECK(err.text().empty());
    OA_CHECK(tabs);
    OA_CHECK(empty);
    OA_CHECK(full);
    OA_CHECK(line_cap);
    OA_CHECK(byte_cap);
    OA_CHECK(note_fits);
    OA_CHECK(long_text);
}

void require_finds_and_caches() {
    Package package;
    put(package, "lua/util.lua", "return 5\n");
    put(package, "lua/a/b.lua", "return \"ab\"\n");
    put(package, "lua/once.lua", "calls_once = (calls_once or 0) + 1\nreturn false\n");
    put(package, "lua/nilmod.lua", "calls_nil = (calls_nil or 0) + 1\n");
    host::ScriptError error;
    auto sandbox = open_sample(package, error);
    if (!sandbox)
        return;

    struct Loaded {
        int util = 0;
        std::string path;
        bool once_first = true;
        bool once_second = true;
        bool nil_first = false;
        bool nil_second = false;
        int calls_once = 0;
        int calls_nil = 0;
        int results = 0;
    } loaded;

    host::CallHooks hooks;
    hooks.context = &loaded;
    hooks.read = [](void* context, lua_State* state, int results) {
        auto* out = static_cast<Loaded*>(context);
        out->results = results;
        if (results != 8)
            return;
        if (lua_isinteger(state, -8))
            out->util = static_cast<int>(lua_tointeger(state, -8));
        if (lua_isstring(state, -7))
            out->path = lua_tostring(state, -7);
        out->once_first = lua_toboolean(state, -6) != 0;
        out->once_second = lua_toboolean(state, -5) != 0;
        out->nil_first = lua_toboolean(state, -4) != 0;
        out->nil_second = lua_toboolean(state, -3) != 0;
        if (lua_isinteger(state, -2))
            out->calls_once = static_cast<int>(lua_tointeger(state, -2));
        if (lua_isinteger(state, -1))
            out->calls_nil = static_cast<int>(lua_tointeger(state, -1));
    };
    const bool ran = run_source(
        *sandbox,
        package,
        "mutator.lua",
        "local util = require(\"util\")\n"
        "local path = require(\"a.b\")\n"
        "local once_first = require(\"once\")\n"
        "local once_second = require(\"once\")\n"
        "local nil_first = require(\"nilmod\")\n"
        "local nil_second = require(\"nilmod\")\n"
        "return util, path, once_first, once_second, nil_first, nil_second, calls_once, "
        "calls_nil\n",
        "plan",
        error,
        &hooks
    );
    OA_CHECK(ran);
    OA_CHECK(loaded.results == 8);
    OA_CHECK(loaded.util == 5);
    OA_CHECK(loaded.path == "ab");
    OA_CHECK(loaded.once_first == false);
    OA_CHECK(loaded.once_second == false);
    OA_CHECK(loaded.nil_first);
    OA_CHECK(loaded.nil_second);
    OA_CHECK(loaded.calls_once == 1);
    OA_CHECK(loaded.calls_nil == 1);
}

void require_refuses_a_bad_name(const char* name, bool full_message) {
    Package package;
    host::ScriptError error;
    auto sandbox = open_sample(package, error);
    if (!sandbox)
        return;
    const std::string source = std::string("require(\"") + name + "\")\n";
    OA_CHECK(!run_source(*sandbox, package, "mutator.lua", source, "plan", error));
    OA_CHECK(error.kind == host::ErrorKind::module_name);
    const std::string expected =
        std::string("require: \"") + name +
        "\" is not a module name: lower-case letters, digits and _ in parts joined by dots";
    if (full_message)
        OA_CHECK(error.message == expected);
    else
        OA_CHECK(error.message.find(name) != std::string::npos);
}

int append_dump(lua_State*, const void* data, size_t size, void* user) {
    auto* dumped = static_cast<std::vector<uint8_t>*>(user);
    const auto* bytes = static_cast<const uint8_t*>(data);
    dumped->insert(dumped->end(), bytes, bytes + size);
    return 0;
}

std::vector<uint8_t> precompiled_chunk() {
    std::vector<uint8_t> dumped;
    lua_State* state = luaL_newstate();
    OA_CHECK(state != nullptr);
    if (state == nullptr)
        return dumped;
    const char* source = "return 1";
    const int loaded = luaL_loadbufferx(state, source, std::strlen(source), source, "t");
    OA_CHECK(loaded == LUA_OK);
    if (loaded == LUA_OK)
        OA_CHECK(lua_dump(state, append_dump, &dumped, 0) == LUA_OK);
    lua_close(state);
    OA_CHECK(!dumped.empty() && dumped[0] == 0x1b);
    return dumped;
}

void require_reports_missing_loop_and_bytecode() {
    {
        Package package;
        host::ScriptError error;
        auto sandbox = open_sample(package, error);
        if (sandbox) {
            OA_CHECK(!run_source(
                *sandbox, package, "mutator.lua", "require(\"missing\")\n", "plan", error
            ));
            OA_CHECK(error.kind == host::ErrorKind::module_missing);
            OA_CHECK(error.message == "require: the package holds no lua/missing.lua");
        }
    }
    {
        Package package;
        put(package, "lua/a.lua", "require(\"b\")\n");
        put(package, "lua/b.lua", "require(\"a\")\n");
        host::ScriptError error;
        auto sandbox = open_sample(package, error);
        if (sandbox) {
            OA_CHECK(
                !run_source(*sandbox, package, "mutator.lua", "require(\"a\")\n", "plan", error)
            );
            OA_CHECK(error.kind == host::ErrorKind::module_loop);
            OA_CHECK(error.message == "require: lua/a.lua needs lua/b.lua, which needs lua/a.lua");
        }
    }
    {
        Package package;
        put(package, "lua/a.lua", "require(\"a\")\n");
        host::ScriptError error;
        auto sandbox = open_sample(package, error);
        if (sandbox) {
            OA_CHECK(
                !run_source(*sandbox, package, "mutator.lua", "require(\"a\")\n", "plan", error)
            );
            OA_CHECK(error.kind == host::ErrorKind::module_loop);
            OA_CHECK(error.message == "require: lua/a.lua needs lua/a.lua");
        }
    }
    {
        Package package;
        package.files["lua/byte.lua"] = precompiled_chunk();
        host::ScriptError error;
        auto sandbox = open_sample(package, error);
        if (sandbox) {
            OA_CHECK(
                !run_source(*sandbox, package, "mutator.lua", "require(\"byte\")\n", "plan", error)
            );
            OA_CHECK(error.kind == host::ErrorKind::precompiled);
            OA_CHECK(
                error.message == "lua/byte.lua is precompiled Lua; packages hold Lua source only"
            );
        }
    }
    {
        Package package;
        host::ScriptError error;
        auto sandbox = open_sample(package, error);
        if (sandbox) {
            const bool ran = run_source(
                *sandbox,
                package,
                "mutator.lua",
                "pcall(require, \"missing\")\nerror(\"boom\")\n",
                "plan",
                error
            );
            OA_CHECK(!ran);
            OA_CHECK(error.kind == host::ErrorKind::runtime);
            OA_CHECK(error.message == "boom");
        }
    }
    {
        Package package;
        for (int index = 1; index <= 33; ++index) {
            const std::string path = "lua/d" + std::to_string(index) + ".lua";
            std::string source = "return true\n";
            if (index < 33)
                source = "require(\"d" + std::to_string(index + 1) + "\")\n";
            put(package, path, source);
        }
        host::ScriptError error;
        auto sandbox = open_sample(package, error);
        if (sandbox) {
            OA_CHECK(
                !run_source(*sandbox, package, "mutator.lua", "require(\"d1\")\n", "plan", error)
            );
            OA_CHECK(error.kind == host::ErrorKind::runtime);
            OA_CHECK(error.message == "require: modules are nested more than 32 deep");
        }
    }
    {
        Package package;
        for (int index = 1; index <= 32; ++index) {
            const std::string path = "lua/e" + std::to_string(index) + ".lua";
            std::string source = "return true\n";
            if (index < 32)
                source = "require(\"e" + std::to_string(index + 1) + "\")\n";
            put(package, path, source);
        }
        host::ScriptError error;
        auto sandbox = open_sample(package, error);
        if (sandbox)
            OA_CHECK(
                run_source(*sandbox, package, "mutator.lua", "require(\"e1\")\n", "plan", error)
            );
    }
}

void require_clears_a_caught_failure() {
    Package package;
    put(package, "lua/bad.lua", "error(\"bad failed\")\n");
    put(package, "lua/good.lua", "return 4\n");
    for (int index = 1; index <= 33; ++index)
        put(package, "lua/m" + std::to_string(index) + ".lua", "error(\"missed\")\n");
    host::ScriptError error;
    auto sandbox = open_sample(package, error);
    if (!sandbox)
        return;

    struct Caught {
        std::string first;
        std::string second;
        int caught = 0;
        int good = 0;
        int results = 0;
    } caught;

    host::CallHooks hooks;
    hooks.context = &caught;
    hooks.read = [](void* context, lua_State* state, int results) {
        auto* out = static_cast<Caught*>(context);
        out->results = results;
        if (results < 4)
            return;
        if (lua_isstring(state, -4))
            out->first = lua_tostring(state, -4);
        if (lua_isstring(state, -3))
            out->second = lua_tostring(state, -3);
        if (lua_isinteger(state, -2))
            out->caught = static_cast<int>(lua_tointeger(state, -2));
        if (lua_isinteger(state, -1))
            out->good = static_cast<int>(lua_tointeger(state, -1));
    };
    const bool ran = run_source(
        *sandbox,
        package,
        "mutator.lua",
        "local function failed_text(name)\n"
        "  local ok, err = pcall(require, name)\n"
        "  if ok then return \"ran\" end\n"
        "  return err\n"
        "end\n"
        "local first = failed_text(\"bad\")\n"
        "local second = failed_text(\"bad\")\n"
        "local caught = 0\n"
        "for i = 1, 33 do\n"
        "  local ok, err = pcall(require, \"m\" .. i)\n"
        "  if not ok and err == \"lua/m\" .. i .. \".lua:1: missed\" then\n"
        "    caught = caught + 1\n"
        "  end\n"
        "end\n"
        "return first, second, caught, require(\"good\")\n",
        "plan",
        error,
        &hooks
    );
    OA_CHECK(ran);
    OA_CHECK(caught.results == 4);
    OA_CHECK(caught.first == "lua/bad.lua:1: bad failed");
    OA_CHECK(caught.second == "lua/bad.lua:1: bad failed");
    OA_CHECK(caught.caught == 33);
    OA_CHECK(caught.good == 4);
}

void errors_name_the_chunk_and_the_line() {
    Package package;
    host::ScriptError error;
    auto sandbox = open_sample(package, error);
    if (!sandbox)
        return;
    OA_CHECK(!run_source(
        *sandbox, package, "mutator.lua", "local value = 1\n\nerror(\"boom\")\n", "plan", error
    ));
    OA_CHECK(error.kind == host::ErrorKind::runtime);
    OA_CHECK(error.phase == "plan");
    OA_CHECK(error.package_id == "sample");
    OA_CHECK(error.chunk == "mutator.lua");
    OA_CHECK(error.line == 3);
    OA_CHECK(error.message == "boom");
    OA_CHECK(host::describe(error) == "sample: plan: mutator.lua:3: boom");

    Package util_package;
    put(util_package, "lua/util.lua", "local value = 1\n\n\n\nmissing_global()\n");
    host::ScriptError util_error;
    auto util = open_sample(util_package, util_error);
    if (!util)
        return;
    OA_CHECK(
        !run_source(*util, util_package, "mutator.lua", "require(\"util\")\n", "plan", util_error)
    );
    OA_CHECK(util_error.kind == host::ErrorKind::runtime);
    OA_CHECK(util_error.chunk == "lua/util.lua");
    OA_CHECK(util_error.line == 5);

    Package table_package;
    host::ScriptError table_error;
    auto table_box = open_sample(table_package, table_error);
    if (!table_box)
        return;
    OA_CHECK(
        !run_source(*table_box, table_package, "mutator.lua", "error({})\n", "plan", table_error)
    );
    OA_CHECK(table_error.kind == host::ErrorKind::runtime);
    OA_CHECK(table_error.message == "(error object is a table value)");

    Package syntax_package;
    host::ScriptError syntax;
    auto broken = open_sample(syntax_package, syntax);
    if (!broken)
        return;
    OA_CHECK(!run_source(
        *broken, syntax_package, "mutator.lua", "local value = 1\nreturn (", "plan", syntax
    ));
    OA_CHECK(syntax.kind == host::ErrorKind::syntax);
    OA_CHECK(syntax.chunk == "mutator.lua");
    OA_CHECK(syntax.line == 2);
}

int push_counted(void* context, lua_State* /*state*/) {
    auto* pushes = static_cast<int*>(context);
    *pushes += 1;
    return 0;
}

void a_failed_sandbox_runs_nothing_later() {
    Package package;
    host::ScriptError error;
    auto sandbox = open_sample(package, error);
    if (!sandbox)
        return;
    OA_CHECK(!run_source(*sandbox, package, "mutator.lua", "error(\"boom\")\n", "plan", error));
    const host::ScriptError first = error;
    int pushes = 0;
    host::CallHooks hooks;
    hooks.context = &pushes;
    hooks.push = push_counted;
    host::ScriptError later;
    OA_CHECK(!sandbox->call("again", hooks, later));
    OA_CHECK(pushes == 0);
    OA_CHECK(later.kind == first.kind);
    OA_CHECK(later.message == first.message);
    OA_CHECK(later.chunk == first.chunk);
    OA_CHECK(later.line == first.line);
    OA_CHECK(later.phase == first.phase);
    OA_CHECK(sandbox->failed());
}

void compile_check_does_not_run() {
    host::ScriptError error;
    const char* fine = "error(\"nope\")\n";
    const auto fine_bytes = std::vector<uint8_t>(fine, fine + std::strlen(fine));
    OA_CHECK(host::compile_check(fine_bytes, "mutator.lua", error));

    const char* broken = "local value = 1\nreturn (";
    const auto broken_bytes = std::vector<uint8_t>(broken, broken + std::strlen(broken));
    OA_CHECK(!host::compile_check(broken_bytes, "mutator.lua", error));
    OA_CHECK(error.kind == host::ErrorKind::syntax);
    OA_CHECK(error.phase == "compile");
    OA_CHECK(error.chunk == "mutator.lua");
    OA_CHECK(error.line == 2);

    const auto dumped = precompiled_chunk();
    OA_CHECK(!host::compile_check(dumped, "byte.lua", error));
    OA_CHECK(error.kind == host::ErrorKind::precompiled);
    OA_CHECK(error.message == "byte.lua is precompiled Lua; packages hold Lua source only");

    Package package;
    host::ScriptError opened;
    auto sandbox = open_sample(package, opened);
    if (!sandbox)
        return;
    OA_CHECK(run_source(*sandbox, package, "mutator.lua", "print(\"kept\")\n", "plan", opened));
    StreamCapture out;
    OA_CHECK(out.begin(stdout));
    const char* printing = "print(\"hidden\")\n";
    const auto printing_bytes = std::vector<uint8_t>(printing, printing + std::strlen(printing));
    const bool accepted = host::compile_check(printing_bytes, "other.lua", error);
    out.end();
    OA_CHECK(accepted);
    OA_CHECK(out.text().empty());
    const auto lines = sandbox->printed();
    OA_CHECK(lines.size() == 1 && lines[0] == "kept");
}

extern "C" int make_box(lua_State* state) {
    auto* box = static_cast<int*>(lua_newuserdatauv(state, sizeof(int), 0));
    *box = static_cast<int>(luaL_checkinteger(state, 1));
    luaL_setmetatable(state, "sample.box");
    return 1;
}

extern "C" int box_text(lua_State* state) {
    const auto* box = static_cast<const int*>(luaL_checkudata(state, 1, "sample.box"));
    char text[32];
    std::snprintf(text, sizeof text, "%d", *box);
    lua_pushstring(state, text);
    return 1;
}

void register_box(void* /*context*/, lua_State* state) {
    luaL_newmetatable(state, "sample.box");
    lua_pushcfunction(state, box_text);
    lua_setfield(state, -2, "__tostring");
    lua_pop(state, 1);
    lua_pushcfunction(state, make_box);
    lua_setglobal(state, "make_box");
}

void fail_host(void* /*context*/, lua_State* state) {
    luaL_error(state, "host failed");
}

void run_host_registers_and_can_fail() {
    Package package;
    host::ScriptError error;
    auto sandbox = open_sample(package, error);
    if (!sandbox)
        return;
    OA_CHECK(sandbox->run_host("host", nullptr, register_box, error));
    std::string shown;
    host::CallHooks hooks;
    hooks.context = &shown;
    hooks.read = [](void* context, lua_State* state, int results) {
        auto* out = static_cast<std::string*>(context);
        if (results == 1 && lua_isstring(state, -1))
            *out = lua_tostring(state, -1);
    };
    const bool ran = run_source(
        *sandbox, package, "mutator.lua", "return tostring(make_box(7))\n", "plan", error, &hooks
    );
    OA_CHECK(ran);
    OA_CHECK(shown == "7");

    Package failing;
    host::ScriptError host_error;
    auto broken = open_sample(failing, host_error);
    if (!broken)
        return;
    OA_CHECK(!broken->run_host("host", nullptr, fail_host, host_error));
    OA_CHECK(host_error.kind == host::ErrorKind::runtime);
    OA_CHECK(host_error.message == "host failed");
    OA_CHECK(host_error.line == 0);
    OA_CHECK(host_error.chunk.empty());
    OA_CHECK(host::describe(host_error) == "sample: host: host failed");
}

void install_from_test(void* /*context*/, lua_State* state) {
    lua_pushinteger(state, 1);
    lua_setglobal(state, "from_test");
}

void read_global(void* context, lua_State* state, int results) {
    auto* value = static_cast<int*>(context);
    *value = -1;
    if (results == 1 && lua_isnil(state, -1))
        *value = 0;
    else if (results == 1 && lua_isinteger(state, -1))
        *value = static_cast<int>(lua_tointeger(state, -1));
}

void profiles_do_not_share_a_state() {
    host::Budget budget;
    budget.instructions = 2'000'000;
    budget.memory_bytes = std::size_t{8} << 20;
    host::Profile profile{"test", budget, nullptr, install_from_test};

    Package test_package;
    host::ScriptError test_error;
    auto test_box = open_sample(test_package, test_error, profile);
    if (!test_box)
        return;
    int from_test = -1;
    host::CallHooks hooks;
    hooks.context = &from_test;
    hooks.read = read_global;
    OA_CHECK(run_source(
        *test_box, test_package, "mutator.lua", "return from_test\n", "plan", test_error, &hooks
    ));
    OA_CHECK(from_test == 1);

    Package mutator_package;
    host::ScriptError mutator_error;
    auto mutator = open_sample(mutator_package, mutator_error);
    if (!mutator)
        return;
    int absent = -1;
    hooks.context = &absent;
    OA_CHECK(run_source(
        *mutator,
        mutator_package,
        "mutator.lua",
        "return from_test\n",
        "plan",
        mutator_error,
        &hooks
    ));
    OA_CHECK(absent == 0);
}

void a_move_keeps_one_state() {
    Package package;
    host::ScriptError error;
    auto sandbox = open_sample(package, error);
    if (!sandbox)
        return;
    host::Sandbox moved = std::move(*sandbox);
    OA_CHECK(sandbox->state() == nullptr);
    host::ScriptError moved_from;
    OA_CHECK(!sandbox->run_file("mutator.lua", "plan", nullptr, moved_from));
    OA_CHECK(moved_from.kind == host::ErrorKind::runtime);
    OA_CHECK(moved_from.message == "the sandbox has been moved from");
    OA_CHECK(run_source(moved, package, "mutator.lua", "return 3\n", "plan", error));
    OA_CHECK(moved.state() != nullptr);
}

bool throwing_read(void* /*context*/, std::string_view /*path*/, std::vector<uint8_t>& /*bytes*/) {
    throw std::runtime_error("nope");
}

void files_are_refused_before_lua() {
    Package package;
    package.files["big.lua"] =
        std::vector<uint8_t>(host::max_source_bytes + 1, static_cast<uint8_t>('a'));
    host::ScriptError error;
    auto sandbox = open_sample(package, error);
    if (!sandbox)
        return;
    OA_CHECK(!sandbox->run_file("big.lua", "plan", nullptr, error));
    OA_CHECK(error.kind == host::ErrorKind::source_too_large);
    OA_CHECK(error.message == "big.lua is 1048577 bytes of source; the most is 1048576");

    Package binary;
    binary.files["byte.lua"] = {0x1b, static_cast<uint8_t>('L')};
    host::ScriptError binary_error;
    auto binary_box = open_sample(binary, binary_error);
    if (!binary_box)
        return;
    OA_CHECK(!binary_box->run_file("byte.lua", "plan", nullptr, binary_error));
    OA_CHECK(binary_error.kind == host::ErrorKind::precompiled);
    OA_CHECK(binary_error.message == "byte.lua is precompiled Lua; packages hold Lua source only");

    Package missing;
    host::ScriptError missing_error;
    auto missing_box = open_sample(missing, missing_error);
    if (!missing_box)
        return;
    OA_CHECK(!missing_box->run_file("gone.lua", "plan", nullptr, missing_error));
    OA_CHECK(missing_error.kind == host::ErrorKind::runtime);
    OA_CHECK(missing_error.message == "gone.lua: the package holds no such file");

    Package throwing;
    host::ScriptError throw_error;
    host::OpenOptions options;
    options.package_id = "sample";
    options.profile = host::mutator_profile();
    options.sources.read = throwing_read;
    auto throw_box = host::Sandbox::open(options, throw_error);
    OA_CHECK(throw_box.has_value());
    if (!throw_box)
        return;
    OA_CHECK(!throw_box->run_file("mutator.lua", "plan", nullptr, throw_error));
    OA_CHECK(throw_error.kind == host::ErrorKind::runtime);
    OA_CHECK(throw_error.message == "mutator.lua: the package file could not be read");
}

} // namespace

int main() {
    call_reads_the_result();
    libraries_match_the_sandbox();
    string_rep_obeys_the_limit();
    metatable_keys_are_refused();
    print_keeps_lines_and_writes_nothing();
    require_finds_and_caches();
    require_refuses_a_bad_name("Util", true);
    require_refuses_a_bad_name("../x", true);
    require_refuses_a_bad_name("a..b", true);
    require_refuses_a_bad_name("", true);
    const std::string long_name(65, 'a');
    require_refuses_a_bad_name(long_name.c_str(), true);
    require_reports_missing_loop_and_bytecode();
    require_clears_a_caught_failure();
    errors_name_the_chunk_and_the_line();
    a_failed_sandbox_runs_nothing_later();
    compile_check_does_not_run();
    run_host_registers_and_can_fail();
    profiles_do_not_share_a_state();
    a_move_keeps_one_state();
    files_are_refused_before_lua();
    return oa::test::check_exit_status();
}
