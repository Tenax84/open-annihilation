// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Lua spellings of rule blocks. A block is userdata the script can keep:
// its selector, needs and changes live in the userdata and its uservalues,
// which have no finalizer. The rule record is built when the plan is read.
// A Lua error is raised only after every C++ value in the frame is gone,
// because a Lua error leaves destructors unrun. The message waits in a
// buffer that outlives the frame.
#include "oa/data/mutators/rules_lua.hpp"

#include "oa/data/mutators/collections.hpp"
#include "oa/data/script_host/sorted.hpp"

#include "lua.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
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
constexpr const char* kEmptyMeta = "oa.mutators.empty";

constexpr int kValueName = 1;
constexpr int kValueFolder = 2;
constexpr int kValueExtension = 3;
constexpr int kValueNeeds = 4;
constexpr int kValueChanges = 5;
constexpr int kRuleValues = 5;

constexpr std::string_view kDepthMarker = "<alldepth2>";

/// One change value built by set, scale, lock, remove or append.
///
/// The text, when there is any, is uservalue 1. A number change leaves that
/// uservalue nil. Empty text stored here is an assignment of empty text.
/// A removal leaves the uservalue nil.
struct ChangeBox {
    sm::ChangeKind change_kind{sm::ChangeKind::assign};
    double value{0};
    double low{0};
    double high{0};
};

/// One rule block the script can hold. Nothing here owns heap memory.
///
/// Uservalue 1 is the record name, 2 the folder, 3 the extension, 4 the
/// needs table and 5 the changes table. A missing folder or extension means
/// the units folder and the fbi suffix. `selector_chosen` is set only by
/// `where`, `all` and `named`, so `on` can still come first.
struct RuleBits {
    uint8_t complete{0};
    uint8_t selector_chosen{0};
    sm::Selector selector{sm::Selector::all};
    uint32_t line{0};
    uint32_t change_line{0};
};

/// The message the next raise reports. It outlives the frame that filled it.
std::string& parked_reason() {
    static std::string text;
    return text;
}

/// Canonical folder and extension, parked so the frame can end before Lua copies them.
std::string& held_text(int slot) {
    static std::string texts[2];
    return texts[slot == 0 ? 0 : 1];
}

/// Needs gathered for one block. Static so a Lua error cannot drop the buffer.
std::vector<sm::Need>& need_scratch() {
    static std::vector<sm::Need> needs;
    return needs;
}

/// Changes gathered for one block. Static so a Lua error cannot drop the buffer.
std::vector<sm::Change>& change_scratch() {
    static std::vector<sm::Change> changes;
    return changes;
}

void release_scratch() {
    need_scratch().clear();
    change_scratch().clear();
}

/// Raises the parked message. Call it only after C++ objects in the frame are gone.
int raise_parked(lua_State* state) {
    std::string& text = parked_reason();
    if (text.empty())
        lua_pushliteral(state, "a rule was refused");
    else
        lua_pushlstring(state, text.data(), text.size());
    text.clear();
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
    for (std::size_t byte_index = 0; byte_index < field.size(); ++byte_index) {
        unsigned char byte = static_cast<unsigned char>(field[byte_index]);
        if (byte >= 'A' && byte <= 'Z')
            byte = static_cast<unsigned char>(byte - 'A' + 'a');
        if (byte != static_cast<unsigned char>(kDepthMarker[byte_index]))
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

const char* selector_word(const RuleBits& bits) noexcept {
    if (bits.selector_chosen == 0)
        return "on";
    if (bits.selector == sm::Selector::all)
        return "all";
    if (bits.selector == sm::Selector::name)
        return "named";
    return "where";
}

extern "C" {

static int refuse_assign(lua_State* state) {
    return luaL_error(state, "a rule value cannot be assigned");
}

static int rule_index(lua_State* state);
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

void ensure_meta(lua_State* state, const char* name, lua_CFunction index) {
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
    lua_pushboolean(state, 0);
    lua_setfield(state, -2, "__metatable");
    lua_pop(state, 1);
}

void ensure_rule_meta(lua_State* state) {
    ensure_meta(state, kRuleMeta, rule_index);
    ensure_meta(state, kChangeMeta, nullptr);
    ensure_meta(state, kSourceMeta, nullptr);
    ensure_meta(state, kEmptyMeta, nullptr);
}

RuleBits* rule_at(lua_State* state, int slot) {
    return static_cast<RuleBits*>(luaL_testudata(state, slot, kRuleMeta));
}

bool empty_at(lua_State* state, int slot) {
    return luaL_testudata(state, slot, kEmptyMeta) != nullptr;
}

RuleBits* place_rule(lua_State* state, const RuleBits& bits) {
    ensure_rule_meta(state);
    void* memory = lua_newuserdatauv(state, sizeof(RuleBits), kRuleValues);
    auto* box = new (memory) RuleBits(bits);
    luaL_setmetatable(state, kRuleMeta);
    return box;
}

void copy_values(lua_State* state, int from, int to) {
    for (int slot = 1; slot <= kRuleValues; ++slot) {
        lua_getiuservalue(state, from, slot);
        lua_setiuservalue(state, to, slot);
    }
}

void set_value_from(lua_State* state, int rule, int value_slot, int from) {
    lua_pushvalue(state, from);
    lua_setiuservalue(state, rule, value_slot);
}

void clear_value(lua_State* state, int rule, int value_slot) {
    lua_pushnil(state);
    lua_setiuservalue(state, rule, value_slot);
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
    auto* box = new (memory) ChangeBox();
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

/// True when the key cannot be a field name. `reason` says why.
bool reject_key(std::string_view raw, std::string& reason) {
    const std::string_view field = trim_ends(raw);
    if (field.empty()) {
        reason = "a field name is empty";
        return true;
    }
    if (field.size() > sm::max_text_bytes) {
        reason = "text is longer than 1024 bytes";
        return true;
    }
    return false;
}

bool read_change_value(
    lua_State* state, int slot, std::string_view field, sm::Change& change, std::string& reason
) {
    change.field.assign(field);
    change.all_depth2 = depth_marker(field);
    change.line = 0;
    const int type = lua_type(state, slot);
    if (type == LUA_TNUMBER) {
        const double value = lua_tonumber(state, slot);
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
        change.value = lua_toboolean(state, slot) != 0 ? 1 : 0;
        return true;
    }
    if (type == LUA_TSTRING) {
        std::size_t length = 0;
        const char* text = lua_tolstring(state, slot, &length);
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
    if (empty_at(state, slot)) {
        change.kind = sm::ChangeKind::assign_text;
        return true;
    }
    const auto* box = static_cast<const ChangeBox*>(luaL_testudata(state, slot, kChangeMeta));
    if (box == nullptr) {
        reason = std::string(field) + " is not a number, a boolean, a string or a change";
        return false;
    }
    change.kind = box->change_kind;
    change.value = box->value;
    change.low = box->low;
    change.high = box->high;
    if (lua_getiuservalue(state, slot, 1) == LUA_TSTRING) {
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

bool gather_needs(lua_State* state, int table, uint32_t line, std::string& reason) {
    auto& needs = need_scratch();
    needs.clear();
    table = lua_absindex(state, table);
    lua_pushnil(state);
    while (lua_next(state, table) != 0) {
        if (lua_type(state, -2) != LUA_TSTRING) {
            lua_pop(state, 2);
            needs.clear();
            reason = "a field name must be a string";
            return false;
        }
        std::size_t length = 0;
        const char* text = lua_tolstring(state, -2, &length);
        const std::string_view raw(text != nullptr ? text : "", length);
        if (reject_key(raw, reason)) {
            lua_pop(state, 2);
            needs.clear();
            return false;
        }
        const std::string_view field = trim_ends(raw);
        sm::Need need;
        const bool accepted = need_from_lua(state, -1, field, need, &reason);
        if (!accepted || !text_within_limit(need.text, reason)) {
            lua_pop(state, 2);
            needs.clear();
            return false;
        }
        need.line = line;
        needs.push_back(std::move(need));
        lua_pop(state, 1);
    }
    std::sort(needs.begin(), needs.end(), [](const sm::Need& first, const sm::Need& second) {
        return host::name_less(first.field, second.field);
    });
    return true;
}

bool gather_changes(lua_State* state, int table, uint32_t line, std::string& reason) {
    auto& changes = change_scratch();
    changes.clear();
    table = lua_absindex(state, table);
    lua_pushnil(state);
    while (lua_next(state, table) != 0) {
        if (lua_type(state, -2) != LUA_TSTRING) {
            lua_pop(state, 2);
            changes.clear();
            reason = "a field name must be a string";
            return false;
        }
        std::size_t length = 0;
        const char* text = lua_tolstring(state, -2, &length);
        const std::string_view raw(text != nullptr ? text : "", length);
        if (reject_key(raw, reason)) {
            lua_pop(state, 2);
            changes.clear();
            return false;
        }
        const std::string_view field = trim_ends(raw);
        sm::Change change;
        const bool accepted = read_change_value(state, -1, field, change, reason);
        if (!accepted) {
            lua_pop(state, 2);
            changes.clear();
            return false;
        }
        change.line = line;
        changes.push_back(std::move(change));
        lua_pop(state, 1);
    }
    std::sort(
        changes.begin(), changes.end(), [](const sm::Change& first, const sm::Change& second) {
            return host::name_less(first.field, second.field);
        }
    );
    return true;
}

bool within_statements(std::size_t needs, std::size_t changes, std::string& reason) {
    if (needs > sm::max_statements || changes > sm::max_statements ||
        needs + changes > sm::max_statements) {
        reason = "too many statements";
        return false;
    }
    return true;
}

bool refuse_finished(const RuleBits* prior, std::string& reason) {
    if (prior == nullptr || prior->complete == 0)
        return false;
    reason = "a rule already has its changes";
    return true;
}

bool refuse_second_selector(
    lua_State* state, const RuleBits* prior, const char* word, std::string& reason
) {
    if (prior == nullptr || prior->selector_chosen == 0)
        return false;
    const uint32_t line = call_line(state);
    reason =
        std::string(word) + " on line " + std::to_string(line) + ": a rule already has a selector";
    return true;
}

bool build_where(lua_State* state, std::string& reason) {
    const RuleBits* prior = rule_at(state, 1);
    const int table_slot = prior != nullptr ? 2 : 1;
    if (refuse_finished(prior, reason) || refuse_second_selector(state, prior, "where", reason))
        return false;
    if (!lua_istable(state, table_slot)) {
        reason = "where expects a table";
        return false;
    }
    const uint32_t line = call_line(state);
    if (!gather_needs(state, table_slot, line, reason) ||
        !within_statements(need_scratch().size(), 0, reason)) {
        release_scratch();
        return false;
    }
    release_scratch();
    RuleBits bits;
    if (prior != nullptr)
        bits = *prior;
    bits.complete = 0;
    bits.selector_chosen = 1;
    bits.selector = sm::Selector::group;
    bits.line = line;
    bits.change_line = 0;
    place_rule(state, bits);
    const int created = lua_gettop(state);
    if (prior != nullptr)
        copy_values(state, 1, created);
    set_value_from(state, created, kValueNeeds, table_slot);
    clear_value(state, created, kValueName);
    clear_value(state, created, kValueChanges);
    return true;
}

bool build_all(lua_State* state, std::string& reason) {
    const RuleBits* prior = rule_at(state, 1);
    if (lua_gettop(state) >= 1 && prior == nullptr) {
        reason = "all takes no arguments";
        return false;
    }
    if (refuse_finished(prior, reason) || refuse_second_selector(state, prior, "all", reason))
        return false;
    RuleBits bits;
    if (prior != nullptr)
        bits = *prior;
    bits.complete = 0;
    bits.selector_chosen = 1;
    bits.selector = sm::Selector::all;
    bits.line = call_line(state);
    bits.change_line = 0;
    place_rule(state, bits);
    const int created = lua_gettop(state);
    if (prior != nullptr)
        copy_values(state, 1, created);
    clear_value(state, created, kValueName);
    clear_value(state, created, kValueNeeds);
    clear_value(state, created, kValueChanges);
    return true;
}

bool build_named(lua_State* state, std::string& reason) {
    const RuleBits* prior = rule_at(state, 1);
    const int name_slot = prior != nullptr ? 2 : 1;
    if (refuse_finished(prior, reason) || refuse_second_selector(state, prior, "named", reason))
        return false;
    if (lua_type(state, name_slot) != LUA_TSTRING) {
        reason = "named expects a name";
        return false;
    }
    std::size_t length = 0;
    const char* text = lua_tolstring(state, name_slot, &length);
    const std::string_view name = trim_ends(std::string_view(text != nullptr ? text : "", length));
    if (name.empty()) {
        reason = "named expects a name";
        return false;
    }
    if (!text_within_limit(name, reason))
        return false;
    const uint32_t line = call_line(state);
    RuleBits bits;
    if (prior != nullptr)
        bits = *prior;
    bits.complete = 0;
    bits.selector_chosen = 1;
    bits.selector = sm::Selector::name;
    bits.line = line;
    bits.change_line = 0;
    place_rule(state, bits);
    const int created = lua_gettop(state);
    if (prior != nullptr)
        copy_values(state, 1, created);
    lua_pushlstring(state, name.data(), name.size());
    lua_setiuservalue(state, created, kValueName);
    clear_value(state, created, kValueNeeds);
    clear_value(state, created, kValueChanges);
    return true;
}

bool canonical_target(lua_State* state, int folder_slot, int extension_slot, std::string& reason) {
    if (lua_type(state, folder_slot) != LUA_TSTRING ||
        lua_type(state, extension_slot) != LUA_TSTRING) {
        reason = "on expects a folder and an extension";
        return false;
    }
    std::size_t folder_length = 0;
    std::size_t extension_length = 0;
    const char* folder_text = lua_tolstring(state, folder_slot, &folder_length);
    const char* extension_text = lua_tolstring(state, extension_slot, &extension_length);
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
    held_text(0) = std::move(folder);
    held_text(1) = std::move(extension);
    return true;
}

bool build_on(lua_State* state, std::string& reason) {
    const RuleBits* prior = rule_at(state, 1);
    const int folder_slot = prior != nullptr ? 2 : 1;
    const int extension_slot = prior != nullptr ? 3 : 2;
    if (refuse_finished(prior, reason))
        return false;
    if (!canonical_target(state, folder_slot, extension_slot, reason))
        return false;
    RuleBits bits;
    if (prior != nullptr) {
        bits = *prior;
    } else {
        bits.selector = sm::Selector::all;
        bits.line = call_line(state);
    }
    bits.complete = 0;
    bits.change_line = 0;
    place_rule(state, bits);
    const int created = lua_gettop(state);
    if (prior != nullptr)
        copy_values(state, 1, created);
    lua_pushlstring(state, held_text(0).data(), held_text(0).size());
    lua_setiuservalue(state, created, kValueFolder);
    lua_pushlstring(state, held_text(1).data(), held_text(1).size());
    lua_setiuservalue(state, created, kValueExtension);
    held_text(0).clear();
    held_text(1).clear();
    return true;
}

bool build_change(lua_State* state, std::string& reason) {
    RuleBits* box = rule_at(state, 1);
    if (box == nullptr) {
        reason = "change expects a rule";
        return false;
    }
    if (box->complete != 0) {
        reason = "change was already called";
        return false;
    }
    if (!lua_istable(state, 2)) {
        reason = "change expects a table";
        return false;
    }
    const uint32_t line = call_line(state);
    const int needs_type = lua_getiuservalue(state, 1, kValueNeeds);
    const bool has_needs = needs_type == LUA_TTABLE;
    if (needs_type != LUA_TNIL && !has_needs) {
        lua_pop(state, 1);
        reason = "where expects a table";
        return false;
    }
    bool needs_ok = true;
    if (has_needs)
        needs_ok = gather_needs(state, -1, box->line, reason);
    lua_pop(state, 1);
    if (!needs_ok) {
        release_scratch();
        return false;
    }
    if (!has_needs)
        need_scratch().clear();
    const std::size_t need_count = need_scratch().size();
    if (!gather_changes(state, 2, line, reason) ||
        !within_statements(need_count, change_scratch().size(), reason)) {
        release_scratch();
        return false;
    }
    release_scratch();
    set_value_from(state, 1, kValueChanges, 2);
    box->complete = 1;
    box->change_line = line;
    return true;
}

int enter_form(lua_State* state, bool (*build)(lua_State*, std::string&)) {
    bool ok = false;
    bool memory_failed = false;
    try {
        std::string reason;
        ok = build(state, reason);
        if (!ok)
            parked_reason() = std::move(reason);
    } catch (const std::bad_alloc&) {
        ok = false;
        memory_failed = true;
    }
    if (memory_failed) {
        try {
            parked_reason() = "out of memory";
        } catch (const std::bad_alloc&) {
            lua_pushliteral(state, "out of memory");
            return lua_error(state);
        }
    }
    if (ok)
        return 1;
    return raise_parked(state);
}

extern "C" {

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
    if (empty_at(state, 1)) {
        push_change(state, sm::ChangeKind::assign_text, 0, 0, 0, "", 0);
        return 1;
    }
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
    for (std::size_t cursor = 0; cursor <= path.size(); ++cursor) {
        if (cursor != path.size() && path[cursor] != '/' && path[cursor] != '\\')
            continue;
        const std::string_view segment = path.substr(start, cursor - start);
        if (segment == "..")
            return "is outside the package";
        start = cursor + 1;
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

const char* lua_type_name(lua_State* state, int slot) {
    const char* name = luaL_typename(state, slot);
    return name != nullptr ? name : "value";
}

std::string take_value_text(lua_State* state, int rule, int value_slot) {
    std::string text;
    if (lua_getiuservalue(state, rule, value_slot) == LUA_TSTRING) {
        std::size_t length = 0;
        const char* raw = lua_tolstring(state, -1, &length);
        text.assign(raw != nullptr ? raw : "", length);
    }
    lua_pop(state, 1);
    return text;
}

bool take_block(
    lua_State* state, int rule, const RuleBits& bits, sm::Block& block, std::string& reason
) {
    release_scratch();
    block.selector = bits.selector;
    block.line = bits.line;
    block.target.folder = take_value_text(state, rule, kValueFolder);
    block.target.extension = take_value_text(state, rule, kValueExtension);
    if (block.target.folder.empty())
        block.target.folder = "units";
    if (block.target.extension.empty())
        block.target.extension = "fbi";
    if (bits.selector == sm::Selector::name)
        block.name = take_value_text(state, rule, kValueName);
    const int needs_type = lua_getiuservalue(state, rule, kValueNeeds);
    if (needs_type == LUA_TTABLE) {
        if (!gather_needs(state, -1, bits.line, reason)) {
            lua_pop(state, 1);
            release_scratch();
            return false;
        }
    } else if (needs_type != LUA_TNIL) {
        lua_pop(state, 1);
        reason = "where expects a table";
        release_scratch();
        return false;
    }
    lua_pop(state, 1);
    const int changes_type = lua_getiuservalue(state, rule, kValueChanges);
    if (changes_type != LUA_TTABLE) {
        lua_pop(state, 1);
        reason = "change expects a table";
        release_scratch();
        return false;
    }
    if (!gather_changes(state, -1, bits.change_line, reason)) {
        lua_pop(state, 1);
        release_scratch();
        return false;
    }
    lua_pop(state, 1);
    if (!within_statements(need_scratch().size(), change_scratch().size(), reason)) {
        release_scratch();
        return false;
    }
    block.needs = std::move(need_scratch());
    block.changes = std::move(change_scratch());
    return true;
}

struct StackGuard {
    lua_State* state{nullptr};
    int top{0};

    ~StackGuard() {
        if (state != nullptr)
            lua_settop(state, top);
    }
};

struct CachedFile {
    sm::RuleFile rules;
    std::vector<sm::Diagnostic> diagnostics;
};

std::string read_failure(const std::string& path, const char* detail) {
    std::string message = "cannot read \"" + path + "\"";
    if (detail != nullptr && detail[0] != '\0') {
        message += ": ";
        message += detail;
    }
    return message;
}

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
    uint32_t kept_blocks = 0;
    std::map<std::string, CachedFile> cached;
    auto flush = [&]() -> bool {
        if (!pending_open)
            return true;
        if (result.sources.size() >= max_plan_sources) {
            result.error = "too many sources";
            return false;
        }
        kept_blocks += static_cast<uint32_t>(pending.rules.blocks.size());
        result.sources.push_back(std::move(pending));
        pending = {};
        pending_open = false;
        return true;
    };
    for (lua_Integer slot = 1;; ++slot) {
        if (lua_rawgeti(state, table, slot) == LUA_TNIL) {
            lua_pop(state, 1);
            break;
        }
        const int value = lua_gettop(state);
        if (const RuleBits* bits = rule_at(state, value); bits != nullptr) {
            if (bits->complete == 0) {
                result.error = rules_slot(slot) + ": the " + selector_word(*bits) + " on line " +
                               std::to_string(bits->line) + " has no change";
                return result;
            }
            if (!pending_open) {
                pending.name.assign(entry_name);
                pending_open = true;
            }
            const std::size_t used =
                static_cast<std::size_t>(kept_blocks) + pending.rules.blocks.size();
            if (used >= sm::max_blocks) {
                result.error = "too many blocks";
                return result;
            }
            sm::Block block;
            std::string block_error;
            if (!take_block(state, value, *bits, block, block_error)) {
                result.error = block_error.empty() ? "a rule was refused" : std::move(block_error);
                return result;
            }
            pending.rules.blocks.push_back(std::move(block));
            lua_pop(state, 1);
            continue;
        }
        if (luaL_testudata(state, value, kSourceMeta) != nullptr) {
            if (!flush())
                return result;
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
            const bool known = cached.find(path) != cached.end();
            if (!known) {
                std::string read_error;
                std::optional<std::vector<uint8_t>> bytes;
                try {
                    if (read)
                        bytes = read(path, &read_error);
                } catch (const std::exception& ex) {
                    result.error = read_failure(path, ex.what());
                    return result;
                } catch (...) {
                    result.error = read_failure(path, nullptr);
                    return result;
                }
                if (!bytes) {
                    result.error =
                        read_failure(path, read_error.empty() ? nullptr : read_error.c_str());
                    return result;
                }
                sm::ParseResult parsed = sm::parse_rules(*bytes);
                bool failed = false;
                for (const sm::Diagnostic& item : parsed.diagnostics) {
                    if (!failed && item.severity == sm::Severity::error) {
                        result.error = path + ":" + std::to_string(item.line) + ": " + item.message;
                        failed = true;
                    }
                }
                if (failed) {
                    for (const sm::Diagnostic& item : parsed.diagnostics) {
                        result.diagnostics.push_back(item);
                        result.diagnostic_sources.push_back(path);
                    }
                    return result;
                }
                CachedFile stored;
                stored.rules = std::move(parsed.rules);
                stored.diagnostics = std::move(parsed.diagnostics);
                cached.emplace(path, std::move(stored));
            }
            const CachedFile& kept = cached.at(path);
            if (static_cast<std::size_t>(kept_blocks) + kept.rules.blocks.size() > sm::max_blocks) {
                result.error = "too many blocks";
                return result;
            }
            if (result.sources.size() >= max_plan_sources) {
                result.error = "too many sources";
                return result;
            }
            if (!known) {
                for (const sm::Diagnostic& item : kept.diagnostics) {
                    result.diagnostics.push_back(item);
                    result.diagnostic_sources.push_back(path);
                }
            }
            kept_blocks += static_cast<uint32_t>(kept.rules.blocks.size());
            oa::data::mutation_rules::RuleSource source;
            source.name = path;
            source.rules = kept.rules;
            result.sources.push_back(std::move(source));
            lua_pop(state, 1);
            continue;
        }
        result.error = rules_slot(slot) + " is not a rule: a " + lua_type_name(state, value);
        return result;
    }
    if (!flush())
        return result;
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
    void* memory = lua_newuserdatauv(state, 1, 0);
    *static_cast<unsigned char*>(memory) = 0;
    luaL_setmetatable(state, kEmptyMeta);
    lua_setglobal(state, "empty");
}

PlanRules read_plan_rules(
    lua_State* state, int rules_index, const PackageReader& read, std::string_view entry_name
) {
    try {
        const StackGuard restore{state, lua_gettop(state)};
        return assemble(state, rules_index, read, entry_name);
    } catch (const std::bad_alloc&) {
        PlanRules failed;
        failed.error = "out of memory";
        return failed;
    } catch (const std::exception& ex) {
        PlanRules failed;
        failed.error = "cannot read";
        if (ex.what() != nullptr && ex.what()[0] != '\0') {
            failed.error += ": ";
            failed.error += ex.what();
        }
        return failed;
    } catch (...) {
        PlanRules failed;
        failed.error = "cannot read";
        return failed;
    }
}

} // namespace oa::data::mutators
