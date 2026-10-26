// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// require finds modules only in the package's own lua/ files. The name is
// matched byte for byte, so a file system that folds case cannot change which
// file loads.

#include "internal.hpp"

#include <cstring>

namespace oa::data::script_host {
namespace {

using Control = HostDetail::Control;

constexpr const char* module_table_key = "oa.script_host.modules";

static_assert(max_require_depth == 32);
static_assert(max_module_name_bytes == 64);

bool valid_module_name(const char* name, std::size_t length) {
    if (name == nullptr || length == 0 || length > max_module_name_bytes)
        return false;
    bool in_part = false;
    for (std::size_t index = 0; index < length; ++index) {
        const unsigned char symbol = static_cast<unsigned char>(name[index]);
        if (symbol == '.') {
            if (!in_part)
                return false;
            in_part = false;
            continue;
        }
        const bool letter = symbol >= 'a' && symbol <= 'z';
        const bool digit = symbol >= '0' && symbol <= '9';
        if (!letter && !digit && symbol != '_')
            return false;
        in_part = true;
    }
    return in_part;
}

void write_invalid_name(Control* control, const char* name, std::size_t length) {
    const std::size_t shown = length < 96 ? length : 96;
    char* cursor = control->scratch;
    char* end = control->scratch + sizeof control->scratch;
    cursor = append_text(cursor, end, "require: \"");
    cursor = append_bytes(cursor, end, name, shown);
    append_text(
        cursor,
        end,
        "\" is not a module name: lower-case letters, digits and _ in parts joined by dots"
    );
}

bool write_module_path(Control* control, const char* name, std::size_t length) {
    char* cursor = control->module_path;
    char* end = control->module_path + sizeof control->module_path;
    cursor = append_text(cursor, end, "lua/");
    for (std::size_t index = 0; index < length; ++index) {
        if (cursor + 1 >= end)
            return false;
        *cursor++ = name[index] == '.' ? '/' : name[index];
    }
    cursor = append_text(cursor, end, ".lua");
    if (cursor + 1 >= end && control->module_path[sizeof control->module_path - 1] != '\0')
        return false;
    char* at = control->chunk_at_path;
    char* at_end = control->chunk_at_path + sizeof control->chunk_at_path;
    at = append_text(at, at_end, "@");
    append_text(at, at_end, control->module_path);
    return true;
}

void write_missing(Control* control) {
    char* cursor = control->scratch;
    char* end = control->scratch + sizeof control->scratch;
    cursor = append_text(cursor, end, "require: the package holds no ");
    append_text(cursor, end, control->module_path);
}

void write_too_large(Control* control, std::size_t size) {
    char number[32];
    append_u64(number, number + sizeof number, static_cast<uint64_t>(size));
    char* cursor = control->scratch;
    char* end = control->scratch + sizeof control->scratch;
    cursor = append_text(cursor, end, control->module_path);
    cursor = append_text(cursor, end, " is ");
    cursor = append_text(cursor, end, number);
    append_text(cursor, end, " bytes of source; the most is 1048576");
}

void write_precompiled(Control* control) {
    char* cursor = control->scratch;
    char* end = control->scratch + sizeof control->scratch;
    cursor = append_text(cursor, end, control->module_path);
    append_text(cursor, end, " is precompiled Lua; packages hold Lua source only");
}

void write_cycle(Control* control) {
    char* cursor = control->scratch;
    char* end = control->scratch + sizeof control->scratch;
    cursor = append_text(cursor, end, "require: ");
    if (control->loading_count > 0)
        cursor = append_text(cursor, end, control->loading_paths[0]);
    for (int index = 1; index < control->loading_count; ++index) {
        cursor = append_text(cursor, end, index == 1 ? " needs " : ", which needs ");
        cursor = append_text(cursor, end, control->loading_paths[index]);
    }
    cursor = append_text(cursor, end, control->loading_count <= 1 ? " needs " : ", which needs ");
    append_text(cursor, end, control->module_path);
}

bool path_is_loading(const Control* control) {
    for (int index = 0; index < control->loading_count; ++index) {
        if (std::strcmp(control->loading_paths[index], control->module_path) == 0)
            return true;
    }
    return false;
}

int read_package_file(Control* control) {
    try {
        control->read_buffer.clear();
        if (control->sources.read == nullptr)
            return 0;
        const bool found = control->sources.read(
            control->sources.context, std::string_view{control->module_path}, control->read_buffer
        );
        return found ? 1 : 0;
    } catch (...) {
        return -1;
    }
}

void mark_loading(Control* control) {
    copy_bounded(
        control->loading_paths[control->loading_count],
        path_buffer_bytes,
        control->module_path,
        std::strlen(control->module_path)
    );
    ++control->loading_count;
}

// How many host_require frames are still on the stack, including this call.
// A caught error leaves its mark behind; the next require drops marks whose
// frames are gone.
int require_frames(lua_State* state, lua_CFunction require_function) {
    int found = 0;
    lua_Debug activation;
    int walked = 0;
    for (int level = 0; lua_getstack(state, level, &activation) != 0; ++level) {
        ++walked;
        lua_getinfo(state, "f", &activation);
        const bool is_require =
            lua_iscfunction(state, -1) != 0 && lua_tocfunction(state, -1) == require_function;
        lua_pop(state, 1);
        if (is_require)
            ++found;
    }
    charge(state, static_cast<uint64_t>(walked));
    return found;
}

extern "C" {
static int host_require(lua_State* state) {
    Control* control = control_from(state);
    if (control->require_function != nullptr) {
        const int parents = require_frames(state, control->require_function) - 1;
        if (parents >= 0 && control->loading_count > parents)
            control->loading_count = parents;
    }
    std::size_t name_length = 0;
    const char* name = luaL_checklstring(state, 1, &name_length);
    if (!valid_module_name(name, name_length)) {
        write_invalid_name(control, name, name_length);
        control->raised_kind = ErrorKind::module_name;
        return luaL_error(state, "%s", control->scratch);
    }

    if (lua_getfield(state, LUA_REGISTRYINDEX, module_table_key) != LUA_TTABLE) {
        lua_pop(state, 1);
        lua_newtable(state);
        lua_pushvalue(state, -1);
        lua_setfield(state, LUA_REGISTRYINDEX, module_table_key);
    }
    lua_pushvalue(state, 1);
    if (lua_rawget(state, -2) != LUA_TNIL) {
        lua_remove(state, -2);
        return 1;
    }
    lua_pop(state, 2);

    if (!write_module_path(control, name, name_length)) {
        write_invalid_name(control, name, name_length);
        control->raised_kind = ErrorKind::module_name;
        return luaL_error(state, "%s", control->scratch);
    }
    if (path_is_loading(control)) {
        write_cycle(control);
        control->raised_kind = ErrorKind::module_loop;
        return luaL_error(state, "%s", control->scratch);
    }
    if (control->loading_count >= max_require_depth) {
        control->raised_kind = ErrorKind::runtime;
        return luaL_error(state, "require: modules are nested more than 32 deep");
    }

    const int read_result = read_package_file(control);
    if (read_result < 0) {
        control->raised_kind = ErrorKind::runtime;
        return luaL_error(state, "require: the package file could not be read");
    }
    if (read_result == 0) {
        write_missing(control);
        control->raised_kind = ErrorKind::module_missing;
        return luaL_error(state, "%s", control->scratch);
    }
    if (control->read_buffer.size() > max_source_bytes) {
        write_too_large(control, control->read_buffer.size());
        control->raised_kind = ErrorKind::source_too_large;
        return luaL_error(state, "%s", control->scratch);
    }
    if (!control->read_buffer.empty() && control->read_buffer[0] == 0x1b) {
        write_precompiled(control);
        control->raised_kind = ErrorKind::precompiled;
        return luaL_error(state, "%s", control->scratch);
    }

    mark_loading(control);
    const char* bytes = control->read_buffer.empty()
                            ? ""
                            : reinterpret_cast<const char*>(control->read_buffer.data());
    control->active_chunk_name = control->chunk_at_path;
    control->loading_chunk = true;
    const int loaded =
        luaL_loadbufferx(state, bytes, control->read_buffer.size(), control->chunk_at_path, "t");
    control->loading_chunk = false;
    if (loaded != LUA_OK) {
        --control->loading_count;
        if (loaded == LUA_ERRMEM) {
            if (!control->string_refused)
                control->allocation_refused = true;
            return lua_error(state);
        }
        remember_load_error(control, state);
        return lua_error(state);
    }

    lua_pushvalue(state, 1);
    lua_call(state, 1, 1);
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        lua_pushboolean(state, 1);
    }
    lua_getfield(state, LUA_REGISTRYINDEX, module_table_key);
    lua_pushvalue(state, 1);
    lua_pushvalue(state, -3);
    lua_rawset(state, -3);
    lua_pop(state, 1);
    --control->loading_count;
    return 1;
}
} // extern "C"

} // namespace

void install_require(lua_State* state) {
    HostDetail::Control* control = control_from(state);
    control->require_function = host_require;
    lua_newtable(state);
    lua_setfield(state, LUA_REGISTRYINDEX, module_table_key);
    lua_pushcfunction(state, host_require);
    lua_setglobal(state, "require");
}

} // namespace oa::data::script_host
