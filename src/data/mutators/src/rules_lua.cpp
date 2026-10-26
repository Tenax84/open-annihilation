// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Lua spellings of rule blocks. A block is userdata the script can keep:
// its selector lives in the userdata and its name, target, needs and
// changes in its uservalues, which have no finalizer. `where` and `:change`
// check the table they are given and keep a copy of it that only the block
// can reach, so a later change to the script's table changes no block. The
// rule record is built from those copies when the plan is read.
//
// Each form checks with C++ values first and builds in Lua only after they
// are gone, because a Lua error leaves destructors unrun. A refusal's text
// waits in a fixed buffer in the form's own frame until it is raised.
#include "oa/data/mutators/rules_lua.hpp"

#include "oa/data/mutators/collections.hpp"
#include "oa/data/mutators/maps.hpp"
#include "oa/data/script_host/fixed.hpp"
#include "oa/data/script_host/script_host.hpp"
#include "oa/data/script_host/sorted.hpp"

#include "lua.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
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

/// The longest refusal a form raises: a field name at the text limit and the
/// words around it. A longer one is cut.
constexpr std::size_t kRefusalBytes = sm::max_text_bytes + 256;

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
/// needs and 5 the changes. The needs and changes are copies of the tables
/// the script passed, made when `where` and `:change` were called. A missing
/// folder or extension means the units folder and the fbi suffix.
/// `selector_chosen` is set only by `where`, `all` and `named`, so `on` can
/// still come first. `need_count` is the number of needs in uservalue 4.
struct RuleBits {
    uint8_t complete{0};
    uint8_t selector_chosen{0};
    sm::Selector selector{sm::Selector::all};
    uint32_t line{0};
    uint32_t change_line{0};
    uint32_t need_count{0};
};

/// Why a form refused, kept in the form's own frame.
///
/// It has no destructor, so the form can raise it after every C++ value of
/// its check is gone.
struct Refusal {
    std::array<char, kRefusalBytes> text{};
    std::size_t length{0};
};

/// What a form's check found, for the part of the form that builds in Lua.
///
/// It has no destructor. Its texts point into strings on the form's stack.
/// `table_slot` is the slot of the needs or changes table, and `count` the
/// number of entries the check read from it.
struct FormPlan {
    RuleBits bits{};
    bool has_prior{false};
    int table_slot{0};
    uint32_t count{0};
    const char* first_text{nullptr};
    std::size_t first_length{0};
    const char* second_text{nullptr};
    std::size_t second_length{0};
};

/// A form's check: fills the plan, or gives the reason it refuses. It does not raise.
using FormCheck = bool (*)(lua_State*, FormPlan&, std::string&);

void keep_refusal(Refusal& refusal, std::string_view reason) noexcept {
    const std::size_t length = std::min(reason.size(), refusal.text.size());
    if (length != 0)
        std::memcpy(refusal.text.data(), reason.data(), length);
    refusal.length = length;
}

/// Runs a form's check. Its C++ values end with this call; a refusal is copied out.
bool run_check(lua_State* state, FormCheck check, FormPlan& plan, Refusal& refusal) {
    bool ok = false;
    bool memory_failed = false;
    try {
        std::string reason;
        ok = check(state, plan, reason);
        if (!ok)
            keep_refusal(refusal, reason);
    } catch (const std::bad_alloc&) {
        memory_failed = true;
    }
    if (memory_failed) {
        keep_refusal(refusal, "out of memory");
        return false;
    }
    return ok;
}

/// Raises a refusal. Call it only when no C++ value in the frame has a destructor.
int raise_refusal(lua_State* state, const Refusal& refusal) {
    if (refusal.length == 0)
        lua_pushliteral(state, "a rule was refused");
    else
        lua_pushlstring(state, refusal.text.data(), refusal.length);
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

/// One byte of a target: ASCII capitals lowered and `\` read as `/`.
char folded_byte(char raw) noexcept {
    const auto byte = static_cast<unsigned char>(raw);
    if (byte == '\\')
        return '/';
    if (byte >= 'A' && byte <= 'Z')
        return static_cast<char>(byte - 'A' + 'a');
    return raw;
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

void clear_value(lua_State* state, int rule, int value_slot) {
    lua_pushnil(state);
    lua_setiuservalue(state, rule, value_slot);
}

/// Charges one `where` or `:change` table: a small flat cost, then one unit
/// per entry, up to `limit` plus one.
///
/// Key bytes are charged only when every key is visited. A table past the
/// limit charges the entry count alone, so the count does not follow
/// `lua_next`'s order. May raise. No C++ object with a destructor is alive.
void charge_script_table(lua_State* state, int table, std::size_t limit) {
    host::charge_work(state, host::table_walk_charge);
    table = lua_absindex(state, table);
    lua_pushnil(state);
    std::size_t visited = 0;
    uint64_t key_units = 0;
    bool past_limit = false;
    while (lua_next(state, table) != 0) {
        ++visited;
        if (visited > limit) {
            past_limit = true;
            lua_pop(state, 1);
            lua_pop(state, 1);
            break;
        }
        if (lua_type(state, -2) == LUA_TSTRING) {
            std::size_t length = 0;
            lua_tolstring(state, -2, &length);
            const uint64_t units = host::units_for_bytes(length);
            if (key_units > UINT64_MAX - units)
                key_units = UINT64_MAX;
            else
                key_units += units;
        }
        lua_pop(state, 1);
    }
    if (host::element_charge != 0 && visited > UINT64_MAX / host::element_charge)
        host::charge_work(state, UINT64_MAX);
    else
        host::charge_work(state, host::element_charge * static_cast<uint64_t>(visited));
    if (!past_limit)
        host::charge_work(state, key_units);
}

/// Pushes a new table holding every pair of the table at `from`, read raw.
///
/// The keys and values are the same Lua values. Need and change values
/// cannot be changed, so the new table keeps what the form was given.
///
/// @param count number of pairs, used to size the table
void push_raw_copy(lua_State* state, int from, uint32_t count) {
    from = lua_absindex(state, from);
    host::charge_work(state, host::element_charge * static_cast<uint64_t>(count));
    lua_createtable(state, 0, static_cast<int>(std::min<uint32_t>(count, sm::max_statements)));
    const int private_copy = lua_gettop(state);
    lua_pushnil(state);
    uint64_t key_units = 0;
    while (lua_next(state, from) != 0) {
        if (lua_type(state, -2) == LUA_TSTRING) {
            std::size_t length = 0;
            lua_tolstring(state, -2, &length);
            const uint64_t units = host::units_for_bytes(length);
            if (key_units > UINT64_MAX - units)
                key_units = UINT64_MAX;
            else
                key_units += units;
        }
        lua_pushvalue(state, -2);
        lua_insert(state, -2);
        lua_rawset(state, private_copy);
    }
    host::charge_work(state, key_units);
}

/// Pushes `length` bytes of `text` with ASCII capitals lowered and `\` read as `/`.
void push_folded(lua_State* state, const char* text, std::size_t length) {
    host::charge_work(state, host::units_for_bytes(length));
    luaL_Buffer buffer;
    char* bytes = luaL_buffinitsize(state, &buffer, length);
    for (std::size_t byte_index = 0; byte_index < length; ++byte_index)
        bytes[byte_index] = folded_byte(text[byte_index]);
    luaL_pushresultsize(&buffer, length);
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

/// Reads the key at -2 as a field name, trimmed. False with `reason` when it cannot be one.
bool field_at(lua_State* state, std::string_view& field, std::string& reason) {
    if (lua_type(state, -2) != LUA_TSTRING) {
        reason = "a field name must be a string";
        return false;
    }
    std::size_t length = 0;
    const char* text = lua_tolstring(state, -2, &length);
    const std::string_view raw(text != nullptr ? text : "", length);
    if (reject_key(raw, reason))
        return false;
    field = trim_ends(raw);
    return true;
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

/// Reads the value at -1 as the need on `field`.
bool read_need(lua_State* state, std::string_view field, sm::Need& need, std::string& reason) {
    return need_from_lua(state, -1, field, need, &reason) && text_within_limit(need.text, reason);
}

/// Like `read_need`, except a map glob or pattern is left for the maps hook.
///
/// A units rule reads the same table again through `read_need` and refuses it.
bool read_where_need(
    lua_State* state, std::string_view field, sm::Need& need, std::string& reason
) {
    if (map_match_value(state, -1))
        return true;
    return read_need(state, field, need, reason);
}

/// Reads the value at -1 as the change to `field`.
bool read_change(
    lua_State* state, std::string_view field, sm::Change& change, std::string& reason
) {
    return read_change_value(state, -1, field, change, reason);
}

/// Walks a needs or changes table, read raw. Does not raise.
///
/// Every key must be a field name, and `read_entry` checks each value. The
/// walk stops with "too many statements" when the table holds more than
/// `limit` entries. `into`, when not null, receives each entry with `line`.
///
/// @param[out] count number of entries read
/// @return false with `reason` when an entry is refused
template <typename Entry, typename ReadEntry>
bool walk_table(
    lua_State* state,
    int table,
    uint32_t line,
    std::size_t limit,
    std::vector<Entry>* into,
    uint32_t& count,
    std::string& reason,
    ReadEntry read_entry
) {
    count = 0;
    table = lua_absindex(state, table);
    lua_pushnil(state);
    // Trimmed field to the first spelling that trimmed to it. Two keys that
    // trim to one field have no order of their own; the walk would otherwise
    // keep whichever Lua visited first.
    std::map<std::string, std::string, std::less<>> seen_fields;
    while (lua_next(state, table) != 0) {
        std::string_view raw;
        if (lua_type(state, -2) == LUA_TSTRING) {
            std::size_t raw_length = 0;
            const char* raw_text = lua_tolstring(state, -2, &raw_length);
            raw = std::string_view(raw_text != nullptr ? raw_text : "", raw_length);
        }
        std::string_view field;
        bool accepted = field_at(state, field, reason);
        if (accepted && count >= limit) {
            reason = "too many statements";
            accepted = false;
        }
        if (accepted) {
            const auto earlier = seen_fields.find(field);
            if (earlier != seen_fields.end()) {
                reason = "field \"";
                reason += earlier->second;
                reason += "\" and field \"";
                reason += raw;
                reason += "\" name the same field";
                accepted = false;
            }
        }
        Entry entry;
        if (accepted)
            accepted = read_entry(state, field, entry, reason);
        if (!accepted) {
            lua_pop(state, 2);
            return false;
        }
        seen_fields.emplace(std::string(field), std::string(raw));
        entry.line = line;
        if (into != nullptr)
            into->push_back(std::move(entry));
        ++count;
        lua_pop(state, 1);
    }
    return true;
}

/// How many changes a block with `need_count` needs may hold.
std::size_t change_limit(uint32_t need_count) noexcept {
    return need_count < sm::max_statements ? sm::max_statements - need_count : 0;
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

/// Starts the plan of a form that chooses a selector: the prior block's bits, unfinished.
void choose_selector(FormPlan& plan, const RuleBits* prior, sm::Selector selector, uint32_t line) {
    plan.has_prior = prior != nullptr;
    if (prior != nullptr)
        plan.bits = *prior;
    plan.bits.complete = 0;
    plan.bits.selector_chosen = 1;
    plan.bits.selector = selector;
    plan.bits.line = line;
    plan.bits.change_line = 0;
    plan.bits.need_count = 0;
}

bool check_where(lua_State* state, FormPlan& plan, std::string& reason) {
    const RuleBits* prior = rule_at(state, 1);
    const int table_slot = prior != nullptr ? 2 : 1;
    if (refuse_finished(prior, reason) || refuse_second_selector(state, prior, "where", reason))
        return false;
    if (!lua_istable(state, table_slot)) {
        reason = "where expects a table";
        return false;
    }
    const uint32_t line = call_line(state);
    uint32_t count = 0;
    if (!walk_table<sm::Need>(
            state, table_slot, line, sm::max_statements, nullptr, count, reason, read_where_need
        ))
        return false;
    choose_selector(plan, prior, sm::Selector::group, line);
    plan.bits.need_count = count;
    plan.table_slot = table_slot;
    plan.count = count;
    return true;
}

int place_where(lua_State* state, const FormPlan& plan) {
    push_raw_copy(state, plan.table_slot, plan.count);
    const int needs = lua_gettop(state);
    place_rule(state, plan.bits);
    const int created = lua_gettop(state);
    if (plan.has_prior)
        copy_values(state, 1, created);
    lua_pushvalue(state, needs);
    lua_setiuservalue(state, created, kValueNeeds);
    clear_value(state, created, kValueName);
    clear_value(state, created, kValueChanges);
    return 1;
}

bool check_all(lua_State* state, FormPlan& plan, std::string& reason) {
    const RuleBits* prior = rule_at(state, 1);
    if (lua_gettop(state) >= 1 && prior == nullptr) {
        reason = "all takes no arguments";
        return false;
    }
    if (refuse_finished(prior, reason) || refuse_second_selector(state, prior, "all", reason))
        return false;
    choose_selector(plan, prior, sm::Selector::all, call_line(state));
    return true;
}

int place_all(lua_State* state, const FormPlan& plan) {
    place_rule(state, plan.bits);
    const int created = lua_gettop(state);
    if (plan.has_prior)
        copy_values(state, 1, created);
    clear_value(state, created, kValueName);
    clear_value(state, created, kValueNeeds);
    clear_value(state, created, kValueChanges);
    return 1;
}

bool check_named(lua_State* state, FormPlan& plan, std::string& reason) {
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
    choose_selector(plan, prior, sm::Selector::name, call_line(state));
    plan.first_text = name.data();
    plan.first_length = name.size();
    return true;
}

int place_named(lua_State* state, const FormPlan& plan) {
    place_rule(state, plan.bits);
    const int created = lua_gettop(state);
    if (plan.has_prior)
        copy_values(state, 1, created);
    lua_pushlstring(state, plan.first_text, plan.first_length);
    lua_setiuservalue(state, created, kValueName);
    clear_value(state, created, kValueNeeds);
    clear_value(state, created, kValueChanges);
    return 1;
}

/// The trimmed text of a string argument, or an empty view when it is not a string.
std::string_view trimmed_argument(lua_State* state, int slot) {
    if (lua_type(state, slot) != LUA_TSTRING)
        return {};
    std::size_t length = 0;
    const char* text = lua_tolstring(state, slot, &length);
    return trim_ends(std::string_view(text != nullptr ? text : "", length));
}

bool check_on(lua_State* state, FormPlan& plan, std::string& reason) {
    const RuleBits* prior = rule_at(state, 1);
    const int folder_slot = prior != nullptr ? 2 : 1;
    const int extension_slot = prior != nullptr ? 3 : 2;
    if (refuse_finished(prior, reason))
        return false;
    const std::string_view folder = trimmed_argument(state, folder_slot);
    std::string_view extension = trimmed_argument(state, extension_slot);
    if (!extension.empty() && extension.front() == '.')
        extension.remove_prefix(1);
    if (folder.empty() || extension.empty()) {
        reason = "on expects a folder and an extension";
        return false;
    }
    if (!text_within_limit(folder, reason) || !text_within_limit(extension, reason))
        return false;
    plan.has_prior = prior != nullptr;
    if (prior != nullptr) {
        plan.bits = *prior;
    } else {
        plan.bits.selector = sm::Selector::all;
        plan.bits.line = call_line(state);
    }
    plan.bits.complete = 0;
    plan.bits.change_line = 0;
    plan.first_text = folder.data();
    plan.first_length = folder.size();
    plan.second_text = extension.data();
    plan.second_length = extension.size();
    return true;
}

int place_on(lua_State* state, const FormPlan& plan) {
    place_rule(state, plan.bits);
    const int created = lua_gettop(state);
    if (plan.has_prior)
        copy_values(state, 1, created);
    push_folded(state, plan.first_text, plan.first_length);
    lua_setiuservalue(state, created, kValueFolder);
    push_folded(state, plan.second_text, plan.second_length);
    lua_setiuservalue(state, created, kValueExtension);
    return 1;
}

bool check_change(lua_State* state, FormPlan& plan, std::string& reason) {
    const RuleBits* box = rule_at(state, 1);
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
    uint32_t count = 0;
    if (!walk_table<sm::Change>(
            state, 2, line, change_limit(box->need_count), nullptr, count, reason, read_change
        ))
        return false;
    plan.bits = *box;
    plan.bits.complete = 1;
    plan.bits.change_line = line;
    plan.table_slot = 2;
    plan.count = count;
    return true;
}

int place_change(lua_State* state, const FormPlan& plan) {
    push_raw_copy(state, plan.table_slot, plan.count);
    lua_setiuservalue(state, 1, kValueChanges);
    auto* box = static_cast<RuleBits*>(lua_touserdata(state, 1));
    box->complete = 1;
    box->change_line = plan.bits.change_line;
    lua_settop(state, 1);
    return 1;
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
    const RuleBits* prior = rule_at(state, 1);
    const int table_slot = prior != nullptr ? 2 : 1;
    if (lua_istable(state, table_slot))
        charge_script_table(state, table_slot, sm::max_statements);
    Refusal refusal;
    FormPlan plan;
    if (!run_check(state, check_where, plan, refusal))
        return raise_refusal(state, refusal);
    return place_where(state, plan);
}

static int form_all(lua_State* state) {
    Refusal refusal;
    FormPlan plan;
    if (!run_check(state, check_all, plan, refusal))
        return raise_refusal(state, refusal);
    return place_all(state, plan);
}

static int form_named(lua_State* state) {
    const RuleBits* prior = rule_at(state, 1);
    const int name_slot = prior != nullptr ? 2 : 1;
    if (lua_type(state, name_slot) == LUA_TSTRING) {
        std::size_t length = 0;
        lua_tolstring(state, name_slot, &length);
        host::charge_work(state, host::units_for_bytes(length));
    }
    Refusal refusal;
    FormPlan plan;
    if (!run_check(state, check_named, plan, refusal))
        return raise_refusal(state, refusal);
    return place_named(state, plan);
}

static int form_on(lua_State* state) {
    Refusal refusal;
    FormPlan plan;
    if (!run_check(state, check_on, plan, refusal))
        return raise_refusal(state, refusal);
    return place_on(state, plan);
}

static int form_change(lua_State* state) {
    const RuleBits* box = rule_at(state, 1);
    if (box != nullptr && lua_istable(state, 2))
        charge_script_table(state, 2, change_limit(box->need_count));
    Refusal refusal;
    FormPlan plan;
    if (!run_check(state, check_change, plan, refusal))
        return raise_refusal(state, refusal);
    return place_change(state, plan);
}

/// A fixed value as `count / 65536`, or a finite Lua number.
static bool rule_number(lua_State* state, int slot, double* number) {
    int64_t count = 0;
    if (host::to_fixed(state, slot, &count)) {
        *number = static_cast<double>(count) / 65536.0;
        return true;
    }
    if (lua_type(state, slot) != LUA_TNUMBER)
        return false;
    *number = lua_tonumber(state, slot);
    return std::isfinite(*number);
}

static int form_set(lua_State* state) {
    if (empty_at(state, 1)) {
        push_change(state, sm::ChangeKind::assign_text, 0, 0, 0, "", 0);
        return 1;
    }
    int64_t count = 0;
    if (host::to_fixed(state, 1, &count)) {
        push_change(
            state, sm::ChangeKind::assign, static_cast<double>(count) / 65536.0, 0, 0, nullptr, 0
        );
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
        host::charge_work(state, host::units_for_bytes(length));
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
    double numbers[3] = {};
    for (int slot = 1; slot <= count; ++slot) {
        host::charge_work(state, host::element_charge);
        if (!rule_number(state, slot, &numbers[slot - 1]))
            return luaL_error(state, "scale expects a finite number");
    }
    const double factor = numbers[0];
    if (count == 1) {
        push_change(state, sm::ChangeKind::scale, factor, 0, 0, nullptr, 0);
        return 1;
    }
    const double low = numbers[1];
    const double high = numbers[2];
    if (low > high)
        return luaL_error(state, "scale bounds are out of order");
    push_change(state, sm::ChangeKind::scale_clamp, factor, low, high, nullptr, 0);
    return 1;
}

static int form_lock(lua_State* state) {
    double number = 0;
    if (!rule_number(state, 1, &number))
        return luaL_error(state, "lock expects a number");
    push_change(state, sm::ChangeKind::meta, number, 0, 0, nullptr, 0);
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
    host::charge_work(state, host::units_for_bytes(length));
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
    host::charge_work(state, host::units_for_bytes(length));
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

/// Names a block in a plan error: its place in the list, the call that started it and that call's line.
std::string block_label(lua_Integer slot, const RuleBits& bits) {
    return rules_slot(slot) + ": the " + selector_word(bits) + " on line " +
           std::to_string(bits.line);
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

/// Builds the record of a finished block from the copies it keeps.
///
/// Called when the plan is read, outside any Lua call; nothing here raises.
bool take_block(
    lua_State* state, int rule, const RuleBits& bits, sm::Block& block, std::string& reason
) {
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
    std::vector<sm::Need> needs;
    uint32_t need_count = 0;
    const int needs_type = lua_getiuservalue(state, rule, kValueNeeds);
    bool ok = true;
    if (needs_type == LUA_TTABLE) {
        ok = walk_table<sm::Need>(
            state, -1, bits.line, sm::max_statements, &needs, need_count, reason, read_need
        );
    } else if (needs_type != LUA_TNIL) {
        reason = "where expects a table";
        ok = false;
    }
    lua_pop(state, 1);
    if (!ok)
        return false;
    std::vector<sm::Change> changes;
    uint32_t change_count = 0;
    if (lua_getiuservalue(state, rule, kValueChanges) == LUA_TTABLE) {
        ok = walk_table<sm::Change>(
            state,
            -1,
            bits.change_line,
            change_limit(need_count),
            &changes,
            change_count,
            reason,
            read_change
        );
    } else {
        reason = "change expects a table";
        ok = false;
    }
    lua_pop(state, 1);
    if (!ok)
        return false;
    std::sort(needs.begin(), needs.end(), [](const sm::Need& first, const sm::Need& second) {
        return host::name_less(first.field, second.field);
    });
    std::sort(
        changes.begin(), changes.end(), [](const sm::Change& first, const sm::Change& second) {
            return host::name_less(first.field, second.field);
        }
    );
    block.needs = std::move(needs);
    block.changes = std::move(changes);
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
                result.error = block_label(slot, *bits) + " has no change";
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
                result.error = block_label(slot, *bits) + ": " +
                               (block_error.empty() ? "a rule was refused" : block_error);
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

bool finished_where_rule(lua_State* state, int index) {
    const RuleBits* bits = rule_at(state, index);
    return bits != nullptr && bits->complete != 0 && bits->selector_chosen != 0 &&
           bits->selector == sm::Selector::group;
}

void push_rule_needs(lua_State* state, int rule) {
    rule = lua_absindex(state, rule);
    if (rule_at(state, rule) == nullptr) {
        lua_pushnil(state);
        return;
    }
    lua_getiuservalue(state, rule, kValueNeeds);
}

void push_rule_changes(lua_State* state, int rule) {
    rule = lua_absindex(state, rule);
    if (rule_at(state, rule) == nullptr) {
        lua_pushnil(state);
        return;
    }
    lua_getiuservalue(state, rule, kValueChanges);
}

bool rule_change_at(
    lua_State* state,
    int index,
    oa::formats::soft_mutator::ChangeKind* kind,
    double* value,
    double* low,
    double* high
) {
    const auto* box = static_cast<const ChangeBox*>(luaL_testudata(state, index, kChangeMeta));
    if (box == nullptr)
        return false;
    if (kind != nullptr)
        *kind = box->change_kind;
    if (value != nullptr)
        *value = box->value;
    if (low != nullptr)
        *low = box->low;
    if (high != nullptr)
        *high = box->high;
    return true;
}

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
