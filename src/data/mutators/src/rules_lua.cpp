// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Lua spellings of rule blocks. A block is userdata: where, all, named and
// on build one that is not finished, and change finishes it. Needs and
// changes are stored in the order of their field names after ASCII
// lower-casing, so a table's insertion order never becomes part of the
// record. Raising happens only after the C++ values in the frame are done
// with, because a Lua error leaves those destructors unrun.
#include "oa/data/mutators/rules_lua.hpp"

#include "oa/data/mutators/collections.hpp"
#include "oa/data/script_host/sorted.hpp"

#include "lua.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::data::mutators {
namespace {

namespace sm = oa::formats::soft_mutator;
namespace host = oa::data::script_host;

constexpr const char* kRuleMeta = "oa.mutators.rule";
constexpr const char* kChangeMeta = "oa.mutators.rulechange";
constexpr const char* kSourceMeta = "oa.mutators.rulesource";

constexpr std::string_view kDepthMarker = "<alldepth2>";

/// One change value built by set, scale, lock, remove or append.
///
/// The text, when there is any, is uservalue 1. A number change leaves that
/// uservalue nil.
struct ChangeBox {
    sm::ChangeKind change_kind{sm::ChangeKind::assign};
    double value{0};
    double low{0};
    double high{0};
};

/// One rule block. Finished blocks are what the plan list may hold.
struct RuleBox {
    bool complete{false};
    sm::Block block{};
};

int raise_reason(lua_State* state, std::string& reason) {
    if (reason.empty())
        reason = "a rule was refused";
    lua_pushlstring(state, reason.data(), reason.size());
    reason.clear();
    return lua_error(state);
}

/// The line the host reports for the Lua call that entered the current function.
uint32_t call_line(lua_State* state) {
    lua_Debug activation;
    if (lua_getstack(state, 1, &activation) == 0)
        return 0;
    if (lua_getinfo(state, "l", &activation) == 0)
        return 0;
    if (activation.currentline <= 0)
        return 0;
    return static_cast<uint32_t>(activation.currentline);
}

bool depth_marker(std::string_view field) noexcept {
    if (field.size() != kDepthMarker.size())
        return false;
    for (std::size_t index = 0; index < field.size(); ++index) {
        unsigned char byte = static_cast<unsigned char>(field[index]);
        if (byte >= 'A' && byte <= 'Z')
            byte = static_cast<unsigned char>(byte - 'A' + 'a');
        if (byte != static_cast<unsigned char>(kDepthMarker[index]))
            return false;
    }
    return true;
}

std::string_view trim_ends(std::string_view text) noexcept {
    auto blank = [](unsigned char byte) noexcept {
        return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r';
    };
    while (!text.empty() && blank(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    while (!text.empty() && blank(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);
    return text;
}

std::string folded_copy(std::string_view text) {
    std::string folded;
    folded.reserve(text.size());
    for (const char raw : text) {
        const auto byte = static_cast<unsigned char>(raw);
        char piece = byte == '\\' ? '/' : static_cast<char>(byte);
        if (piece >= 'A' && piece <= 'Z')
            piece = static_cast<char>(piece - 'A' + 'a');
        folded.push_back(piece);
    }
    return folded;
}

extern "C" {

static int refuse_assign(lua_State* state) {
    return luaL_error(state, "a rule value cannot be assigned");
}

static int rule_index(lua_State* state);
static int rule_collect(lua_State* state);
static int form_where(lua_State* state);
static int form_all(lua_State* state);
static int form_named(lua_State* state);
static int form_on(lua_State* state);
static int form_change(lua_State* state);
static int form_set(lua_State* state);
static int form_scale(lua_State* state);
static int form_lock(lua_State* state);
static int form_remove(lua_State* state);
static int form_append(lua_State* state);
static int form_sm(lua_State* state);

} // extern "C"

void ensure_meta(lua_State* state, const char* name, lua_CFunction index, lua_CFunction collect) {
    if (luaL_newmetatable(state, name) == 0) {
        lua_pop(state, 1);
        return;
    }
    if (index != nullptr) {
        lua_pushcfunction(state, index);
        lua_setfield(state, -2, "__index");
    }
    lua_pushcfunction(state, refuse_assign);
    lua_setfield(state, -2, "__newindex");
    if (collect != nullptr) {
        lua_pushcfunction(state, collect);
        lua_setfield(state, -2, "__gc");
    }
    lua_pushboolean(state, 0);
    lua_setfield(state, -2, "__metatable");
    lua_pop(state, 1);
}

void ensure_rule_meta(lua_State* state) {
    ensure_meta(state, kRuleMeta, rule_index, rule_collect);
    ensure_meta(state, kChangeMeta, nullptr, nullptr);
    ensure_meta(state, kSourceMeta, nullptr, nullptr);
}

RuleBox* rule_at(lua_State* state, int index) {
    return static_cast<RuleBox*>(luaL_testudata(state, index, kRuleMeta));
}

void push_rule(lua_State* state, sm::Block block) {
    ensure_rule_meta(state);
    void* memory = lua_newuserdatauv(state, sizeof(RuleBox), 0);
    auto* box = new (memory) RuleBox();
    box->block = std::move(block);
    luaL_setmetatable(state, kRuleMeta);
}

void push_change(
    lua_State* state,
    sm::ChangeKind kind,
    double value,
    double low,
    double high,
    const char* text,
    std::size_t length
) {
    ensure_rule_meta(state);
    void* memory = lua_newuserdatauv(state, sizeof(ChangeBox), 1);
    auto* box = static_cast<ChangeBox*>(memory);
    box->change_kind = kind;
    box->value = value;
    box->low = low;
    box->high = high;
    if (text != nullptr)
        lua_pushlstring(state, text, length);
    else
        lua_pushnil(state);
    lua_setiuservalue(state, -2, 1);
    luaL_setmetatable(state, kChangeMeta);
}

bool text_within_limit(std::string_view text, std::string& reason) {
    if (text.size() <= sm::max_text_bytes)
        return true;
    reason = "text is longer than 1024 bytes";
    return false;
}

bool collect_keys(
    lua_State* state, int index, std::vector<std::string>& keys, std::string& reason
) {
    index = lua_absindex(state, index);
    lua_pushnil(state);
    while (lua_next(state, index) != 0) {
        if (lua_type(state, -2) != LUA_TSTRING) {
            lua_pop(state, 2);
            reason = "a field name must be a string";
            return false;
        }
        std::size_t length = 0;
        const char* text = lua_tolstring(state, -2, &length);
        keys.emplace_back(text != nullptr ? text : "", length);
        lua_pop(state, 1);
    }
    host::sort_names(keys);
    return true;
}

bool lookup_field(
    lua_State* state,
    int table_index,
    const std::string& raw,
    std::string& field,
    std::string& reason
) {
    const std::string_view trimmed = trim_ends(raw);
    if (trimmed.empty()) {
        reason = "a field name is empty";
        return false;
    }
    if (!text_within_limit(trimmed, reason))
        return false;
    field.assign(trimmed);
    lua_pushlstring(state, raw.data(), raw.size());
    lua_rawget(state, table_index);
    return true;
}

bool fill_needs(lua_State* state, int table_index, sm::Block& block, std::string& reason) {
    table_index = lua_absindex(state, table_index);
    std::vector<std::string> keys;
    if (!collect_keys(state, table_index, keys, reason))
        return false;
    if (keys.size() > sm::max_statements) {
        reason = "too many statements";
        return false;
    }
    block.needs.reserve(keys.size());
    for (const std::string& raw : keys) {
        std::string field;
        if (!lookup_field(state, table_index, raw, field, reason))
            return false;
        sm::Need need;
        if (!need_from_lua(state, -1, field, need, &reason)) {
            lua_pop(state, 1);
            return false;
        }
        lua_pop(state, 1);
        if (!text_within_limit(need.text, reason))
            return false;
        need.line = block.line;
        block.needs.push_back(std::move(need));
    }
    return true;
}

bool read_change_value(
    lua_State* state, int index, std::string_view field, sm::Change& change, std::string& reason
) {
    change.field.assign(field);
    change.all_depth2 = depth_marker(field);
    change.line = 0;
    const int type = lua_type(state, index);
    if (type == LUA_TNUMBER) {
        const double value = lua_tonumber(state, index);
        if (!std::isfinite(value)) {
            reason = std::string(field) + " is not a finite number";
            return false;
        }
        change.kind = sm::ChangeKind::assign;
        change.value = value;
        return true;
    }
    if (type == LUA_TBOOLEAN) {
        change.kind = sm::ChangeKind::assign;
        change.value = lua_toboolean(state, index) != 0 ? 1 : 0;
        return true;
    }
    if (type == LUA_TSTRING) {
        std::size_t length = 0;
        const char* text = lua_tolstring(state, index, &length);
        if (length == 0) {
            change.kind = sm::ChangeKind::remove;
            return true;
        }
        if (!text_within_limit(std::string_view(text != nullptr ? text : "", length), reason))
            return false;
        change.kind = sm::ChangeKind::assign_text;
        change.text.assign(text != nullptr ? text : "", length);
        return true;
    }
    const auto* box = static_cast<const ChangeBox*>(luaL_testudata(state, index, kChangeMeta));
    if (box == nullptr) {
        reason = std::string(field) + " is not a number, a boolean, a string or a change";
        return false;
    }
    change.kind = box->change_kind;
    change.value = box->value;
    change.low = box->low;
    change.high = box->high;
    if (lua_getiuservalue(state, index, 1) == LUA_TSTRING) {
        std::size_t length = 0;
        const char* text = lua_tolstring(state, -1, &length);
        if (!text_within_limit(std::string_view(text != nullptr ? text : "", length), reason)) {
            lua_pop(state, 1);
            return false;
        }
        change.text.assign(text != nullptr ? text : "", length);
    }
    lua_pop(state, 1);
    return true;
}

bool fill_changes(
    lua_State* state,
    int table_index,
    uint32_t line,
    std::vector<sm::Change>& changes,
    std::string& reason
) {
    table_index = lua_absindex(state, table_index);
    std::vector<std::string> keys;
    if (!collect_keys(state, table_index, keys, reason))
        return false;
    if (keys.size() > sm::max_statements) {
        reason = "too many statements";
        return false;
    }
    changes.reserve(keys.size());
    for (const std::string& raw : keys) {
        std::string field;
        if (!lookup_field(state, table_index, raw, field, reason))
            return false;
        sm::Change change;
        const bool read = read_change_value(state, -1, field, change, reason);
        lua_pop(state, 1);
        if (!read)
            return false;
        change.line = line;
        changes.push_back(std::move(change));
    }
    return true;
}

bool build_where(lua_State* state, std::string& reason) {
    const RuleBox* prior = rule_at(state, 1);
    int table_index = 1;
    if (prior != nullptr) {
        if (prior->complete) {
            reason = "a rule already has its changes";
            return false;
        }
        table_index = 2;
    }
    if (!lua_istable(state, table_index)) {
        reason = "where expects a table";
        return false;
    }
    sm::Block block;
    if (prior != nullptr)
        block.target = prior->block.target;
    block.selector = sm::Selector::group;
    block.line = call_line(state);
    if (!fill_needs(state, table_index, block, reason))
        return false;
    push_rule(state, std::move(block));
    return true;
}

bool build_all(lua_State* state, std::string& reason) {
    const RuleBox* prior = rule_at(state, 1);
    if (lua_gettop(state) >= 1 && prior == nullptr) {
        reason = "all takes no arguments";
        return false;
    }
    if (prior != nullptr && prior->complete) {
        reason = "a rule already has its changes";
        return false;
    }
    sm::Block block;
    if (prior != nullptr)
        block.target = prior->block.target;
    block.selector = sm::Selector::all;
    block.line = call_line(state);
    push_rule(state, std::move(block));
    return true;
}

bool build_named(lua_State* state, std::string& reason) {
    const RuleBox* prior = rule_at(state, 1);
    const int name_index = prior != nullptr ? 2 : 1;
    if (prior != nullptr && prior->complete) {
        reason = "a rule already has its changes";
        return false;
    }
    if (lua_type(state, name_index) != LUA_TSTRING) {
        reason = "named expects a name";
        return false;
    }
    std::size_t length = 0;
    const char* text = lua_tolstring(state, name_index, &length);
    const std::string_view name = trim_ends(std::string_view(text != nullptr ? text : "", length));
    if (name.empty()) {
        reason = "named expects a name";
        return false;
    }
    if (!text_within_limit(name, reason))
        return false;
    sm::Block block;
    if (prior != nullptr)
        block.target = prior->block.target;
    block.selector = sm::Selector::name;
    block.name.assign(name);
    block.line = call_line(state);
    push_rule(state, std::move(block));
    return true;
}

bool canonical_target(
    lua_State* state, int folder_index, int extension_index, sm::Target& target, std::string& reason
) {
    if (lua_type(state, folder_index) != LUA_TSTRING ||
        lua_type(state, extension_index) != LUA_TSTRING) {
        reason = "on expects a folder and an extension";
        return false;
    }
    std::size_t folder_length = 0;
    std::size_t extension_length = 0;
    const char* folder_text = lua_tolstring(state, folder_index, &folder_length);
    const char* extension_text = lua_tolstring(state, extension_index, &extension_length);
    std::string folder = folded_copy(
        trim_ends(std::string_view(folder_text != nullptr ? folder_text : "", folder_length))
    );
    std::string extension = folded_copy(trim_ends(
        std::string_view(extension_text != nullptr ? extension_text : "", extension_length)
    ));
    if (!extension.empty() && extension.front() == '.')
        extension.erase(extension.begin());
    if (folder.empty() || extension.empty()) {
        reason = "on expects a folder and an extension";
        return false;
    }
    if (!text_within_limit(folder, reason) || !text_within_limit(extension, reason))
        return false;
    target.folder = std::move(folder);
    target.extension = std::move(extension);
    return true;
}

bool build_on(lua_State* state, std::string& reason) {
    const RuleBox* prior = rule_at(state, 1);
    const int folder_index = prior != nullptr ? 2 : 1;
    const int extension_index = prior != nullptr ? 3 : 2;
    if (prior != nullptr && prior->complete) {
        reason = "a rule already has its changes";
        return false;
    }
    sm::Block block;
    if (prior != nullptr) {
        block.target = prior->block.target;
        block.selector = prior->block.selector;
        block.name = prior->block.name;
        block.needs = prior->block.needs;
        block.line = prior->block.line;
    } else {
        block.selector = sm::Selector::all;
        block.line = call_line(state);
    }
    if (!canonical_target(state, folder_index, extension_index, block.target, reason))
        return false;
    push_rule(state, std::move(block));
    return true;
}

bool build_change(lua_State* state, std::string& reason) {
    RuleBox* box = rule_at(state, 1);
    if (box == nullptr) {
        reason = "change expects a rule";
        return false;
    }
    if (box->complete) {
        reason = "change was already called";
        return false;
    }
    if (!lua_istable(state, 2)) {
        reason = "change expects a table";
        return false;
    }
    const uint32_t line = call_line(state);
    std::vector<sm::Change> changes;
    if (!fill_changes(state, 2, line, changes, reason))
        return false;
    if (box->block.needs.size() > sm::max_statements ||
        changes.size() > static_cast<std::size_t>(sm::max_statements) ||
        box->block.needs.size() + changes.size() > sm::max_statements) {
        reason = "too many statements";
        return false;
    }
    box->block.changes = std::move(changes);
    box->complete = true;
    return true;
}

int enter_form(lua_State* state, bool (*build)(lua_State*, std::string&)) {
    std::string reason;
    bool ok = false;
    try {
        ok = build(state, reason);
    } catch (const std::bad_alloc&) {
        ok = false;
        try {
            reason = "out of memory";
        } catch (const std::bad_alloc&) {
            return luaL_error(state, "out of memory");
        }
    }
    if (!ok)
        return raise_reason(state, reason);
    return 1;
}

extern "C" {

static int rule_collect(lua_State* state) {
    auto* box = static_cast<RuleBox*>(lua_touserdata(state, 1));
    if (box != nullptr)
        box->~RuleBox();
    return 0;
}

static int rule_index(lua_State* state) {
    if (rule_at(state, 1) == nullptr)
        return luaL_error(state, "expected a rule");
    if (lua_type(state, 2) != LUA_TSTRING) {
        lua_pushnil(state);
        return 1;
    }
    std::size_t length = 0;
    const char* text = lua_tolstring(state, 2, &length);
    const std::string_view key(text != nullptr ? text : "", length);
    if (key == "change") {
        lua_pushcfunction(state, form_change);
        return 1;
    }
    if (key == "where") {
        lua_pushcfunction(state, form_where);
        return 1;
    }
    if (key == "all") {
        lua_pushcfunction(state, form_all);
        return 1;
    }
    if (key == "named") {
        lua_pushcfunction(state, form_named);
        return 1;
    }
    if (key == "on") {
        lua_pushcfunction(state, form_on);
        return 1;
    }
    lua_pushnil(state);
    return 1;
}

static int form_where(lua_State* state) {
    return enter_form(state, build_where);
}

static int form_all(lua_State* state) {
    return enter_form(state, build_all);
}

static int form_named(lua_State* state) {
    return enter_form(state, build_named);
}

static int form_on(lua_State* state) {
    return enter_form(state, build_on);
}

static int form_change(lua_State* state) {
    const int result = enter_form(state, build_change);
    if (result != 1)
        return result;
    lua_settop(state, 1);
    return 1;
}

static int form_set(lua_State* state) {
    const int type = lua_type(state, 1);
    if (type == LUA_TNUMBER) {
        const double value = lua_tonumber(state, 1);
        if (!std::isfinite(value))
            return luaL_error(state, "set expects a finite number");
        push_change(state, sm::ChangeKind::assign, value, 0, 0, nullptr, 0);
        return 1;
    }
    if (type == LUA_TBOOLEAN) {
        const double value = lua_toboolean(state, 1) != 0 ? 1 : 0;
        push_change(state, sm::ChangeKind::assign, value, 0, 0, nullptr, 0);
        return 1;
    }
    if (type == LUA_TSTRING) {
        std::size_t length = 0;
        const char* text = lua_tolstring(state, 1, &length);
        if (length > sm::max_text_bytes)
            return luaL_error(state, "text is longer than 1024 bytes");
        if (length == 0)
            push_change(state, sm::ChangeKind::remove, 0, 0, 0, nullptr, 0);
        else
            push_change(state, sm::ChangeKind::assign_text, 0, 0, 0, text, length);
        return 1;
    }
    return luaL_error(state, "set expects a number, a boolean or a string");
}

static int form_scale(lua_State* state) {
    const int count = lua_gettop(state);
    if (count != 1 && count != 3)
        return luaL_error(state, "scale expects a factor, or a factor and two bounds");
    for (int slot = 1; slot <= count; ++slot) {
        if (lua_type(state, slot) != LUA_TNUMBER || !std::isfinite(lua_tonumber(state, slot)))
            return luaL_error(state, "scale expects a finite number");
    }
    const double factor = lua_tonumber(state, 1);
    if (count == 1) {
        push_change(state, sm::ChangeKind::scale, factor, 0, 0, nullptr, 0);
        return 1;
    }
    const double low = lua_tonumber(state, 2);
    const double high = lua_tonumber(state, 3);
    if (low > high)
        return luaL_error(state, "scale bounds are out of order");
    push_change(state, sm::ChangeKind::scale_clamp, factor, low, high, nullptr, 0);
    return 1;
}

static int form_lock(lua_State* state) {
    if (lua_type(state, 1) != LUA_TNUMBER || !std::isfinite(lua_tonumber(state, 1)))
        return luaL_error(state, "lock expects a number");
    push_change(state, sm::ChangeKind::meta, lua_tonumber(state, 1), 0, 0, nullptr, 0);
    return 1;
}

static int form_remove(lua_State* state) {
    push_change(state, sm::ChangeKind::remove, 0, 0, 0, nullptr, 0);
    return 1;
}

static int form_append(lua_State* state) {
    if (lua_type(state, 1) != LUA_TSTRING)
        return luaL_error(state, "append expects a string");
    std::size_t length = 0;
    const char* text = lua_tolstring(state, 1, &length);
    if (length > sm::max_text_bytes)
        return luaL_error(state, "text is longer than 1024 bytes");
    push_change(state, sm::ChangeKind::append, 0, 0, 0, text != nullptr ? text : "", length);
    return 1;
}

static int form_sm(lua_State* state) {
    if (lua_type(state, 1) != LUA_TSTRING)
        return luaL_error(state, "sm expects a path");
    std::size_t length = 0;
    lua_tolstring(state, 1, &length);
    if (length == 0)
        return luaL_error(state, "sm expects a path");
    ensure_rule_meta(state);
    lua_newuserdatauv(state, 1, 1);
    lua_pushvalue(state, 1);
    lua_setiuservalue(state, -2, 1);
    luaL_setmetatable(state, kSourceMeta);
    return 1;
}

} // extern "C"

std::string outside_reason(std::string_view path) {
    if (path.empty() || path.front() == '/' || path.front() == '\\')
        return "is outside the package";
    if (path.size() >= 2 &&
        ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
        path[1] == ':')
        return "is outside the package";
    std::size_t start = 0;
    for (std::size_t index = 0; index <= path.size(); ++index) {
        if (index != path.size() && path[index] != '/' && path[index] != '\\')
            continue;
        const std::string_view segment = path.substr(start, index - start);
        if (segment == "..")
            return "is outside the package";
        start = index + 1;
    }
    return {};
}

std::string slash_path(std::string_view path) {
    std::string normalized;
    normalized.reserve(path.size());
    for (char piece : path)
        normalized.push_back(piece == '\\' ? '/' : piece);
    return normalized;
}

std::string rules_slot(lua_Integer slot) {
    return "rules[" + std::to_string(static_cast<long long>(slot)) + "]";
}

const char* lua_type_name(lua_State* state, int index) {
    const char* name = luaL_typename(state, index);
    return name != nullptr ? name : "value";
}

struct StackGuard {
    lua_State* state{nullptr};
    int top{0};

    ~StackGuard() {
        if (state != nullptr)
            lua_settop(state, top);
    }
};

PlanRules assemble(
    lua_State* state, int rules_index, const PackageReader& read, std::string_view entry_name
) {
    PlanRules result;
    const int table = lua_absindex(state, rules_index);
    if (!lua_istable(state, table)) {
        result.error = "rules is not a list";
        return result;
    }
    oa::data::mutation_rules::RuleSource pending;
    bool pending_open = false;
    auto flush = [&]() {
        if (!pending_open)
            return;
        result.sources.push_back(std::move(pending));
        pending = {};
        pending_open = false;
    };
    for (lua_Integer slot = 1;; ++slot) {
        if (lua_rawgeti(state, table, slot) == LUA_TNIL) {
            lua_pop(state, 1);
            break;
        }
        const int value = lua_gettop(state);
        if (const RuleBox* box = rule_at(state, value); box != nullptr) {
            if (!box->complete) {
                result.error = rules_slot(slot) + " is not a complete rule";
                return result;
            }
            if (!pending_open) {
                pending.name.assign(entry_name);
                pending_open = true;
            }
            if (pending.rules.blocks.size() >= sm::max_blocks) {
                result.error = "too many blocks";
                return result;
            }
            pending.rules.blocks.push_back(box->block);
            lua_pop(state, 1);
            continue;
        }
        if (luaL_testudata(state, value, kSourceMeta) != nullptr) {
            flush();
            if (lua_getiuservalue(state, value, 1) != LUA_TSTRING) {
                lua_pop(state, 1);
                result.error = rules_slot(slot) + " is not a rule: a userdata";
                return result;
            }
            std::size_t length = 0;
            const char* text = lua_tolstring(state, -1, &length);
            const std::string raw(text != nullptr ? text : "", length);
            lua_pop(state, 1);
            const std::string problem = outside_reason(raw);
            if (!problem.empty()) {
                result.error = rules_slot(slot) + ": sm path \"" + raw + "\" " + problem;
                return result;
            }
            const std::string path = slash_path(raw);
            std::string read_error;
            std::optional<std::vector<uint8_t>> bytes;
            if (read)
                bytes = read(path, &read_error);
            if (!bytes) {
                result.error = "cannot read \"" + path + "\"";
                if (!read_error.empty())
                    result.error += ": " + read_error;
                return result;
            }
            const sm::ParseResult parsed = sm::parse_rules(*bytes);
            bool failed = false;
            for (const sm::Diagnostic& item : parsed.diagnostics) {
                result.diagnostics.push_back(item);
                result.diagnostic_sources.push_back(path);
                if (!failed && item.severity == sm::Severity::error) {
                    result.error = path + ":" + std::to_string(item.line) + ": " + item.message;
                    failed = true;
                }
            }
            if (failed)
                return result;
            oa::data::mutation_rules::RuleSource source;
            source.name = path;
            source.rules = parsed.rules;
            result.sources.push_back(std::move(source));
            lua_pop(state, 1);
            continue;
        }
        result.error = rules_slot(slot) + " is not a rule: a " + lua_type_name(state, value);
        return result;
    }
    flush();
    return result;
}

} // namespace

void install_rule_forms(lua_State* state) {
    install_selection_forms(state);
    ensure_rule_meta(state);
    lua_pushcfunction(state, form_where);
    lua_setglobal(state, "where");
    lua_pushcfunction(state, form_all);
    lua_setglobal(state, "all");
    lua_pushcfunction(state, form_named);
    lua_setglobal(state, "named");
    lua_pushcfunction(state, form_on);
    lua_setglobal(state, "on");
    lua_pushcfunction(state, form_set);
    lua_setglobal(state, "set");
    lua_pushcfunction(state, form_scale);
    lua_setglobal(state, "scale");
    lua_pushcfunction(state, form_lock);
    lua_setglobal(state, "lock");
    lua_pushcfunction(state, form_remove);
    lua_setglobal(state, "remove");
    lua_pushcfunction(state, form_append);
    lua_setglobal(state, "append");
    lua_pushcfunction(state, form_sm);
    lua_setglobal(state, "sm");
}

PlanRules read_plan_rules(
    lua_State* state, int rules_index, const PackageReader& read, std::string_view entry_name
) {
    try {
        const StackGuard guard{state, lua_gettop(state)};
        return assemble(state, rules_index, read, entry_name);
    } catch (const std::bad_alloc&) {
        PlanRules result;
        result.error = "out of memory";
        return result;
    }
}

} // namespace oa::data::mutators
