// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Rewrites the chosen map's OTA for one match. The view the script sees is
// built for that call only. A write that the text writer turns back into the
// same bytes is not served. Start positions stay in the file and are never
// part of the view.
#include "oa/data/mutators/maps.hpp"

#include "oa/data/mutators/rules_lua.hpp"
#include "oa/data/script_host/sorted.hpp"
#include "oa/formats/oamod.hpp"
#include "oa/formats/tdf/edit.hpp"
#include "oa/formats/tdf/number.hpp"
#include "oa/formats/tdf/write.hpp"

#include "lua.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::data::mutators {
namespace {

namespace host = oa::data::script_host;
namespace tdf = oa::formats::tdf;
namespace defs = oa::data::defs;
namespace sm = oa::formats::soft_mutator;
namespace oamod = oa::formats::oamod;
namespace oamut = oa::formats::oamut;

constexpr const char* kMatchMeta = "oa.mutators.mapmatch";
constexpr const char* kMapMeta = "oa.mutators.map";
constexpr const char* kHeaderMeta = "oa.mutators.mapheader";
constexpr const char* kSchemasMeta = "oa.mutators.mapschemas";
constexpr const char* kSchemaMeta = "oa.mutators.mapschema";
constexpr const char* kUnitsMeta = "oa.mutators.mapunits";
constexpr const char* kFeaturesMeta = "oa.mutators.mapfeatures";
constexpr const char* kUnitMeta = "oa.mutators.mapunit";
constexpr const char* kFeatureMeta = "oa.mutators.mapfeature";
constexpr int kSchemaLimit = 64;
constexpr int kGlobLimit = 256;
constexpr std::size_t kFeatureNameLimit = 127;
constexpr std::size_t kErrorLimit = 640;

constexpr uint8_t kGlob = 0;
constexpr uint8_t kPattern = 1;

constexpr uint8_t kBoxMap = 0;
constexpr uint8_t kBoxHeader = 1;
constexpr uint8_t kBoxSchemas = 2;
constexpr uint8_t kBoxSchema = 3;
constexpr uint8_t kBoxUnits = 4;
constexpr uint8_t kBoxFeatures = 5;
constexpr uint8_t kBoxUnit = 6;
constexpr uint8_t kBoxFeature = 7;
constexpr uint8_t kBoxFiltered = 8;

enum class KeyClass : uint8_t {
    header_whole,
    header_bit,
    header_real,
    header_frozen,
    schema_profile,
    schema_resource,
    schema_metal,
    schema_frozen,
    neither
};

struct KeyRow {
    const char* folded{};
    const char* spelling{};
    KeyClass kind{};
};

constexpr KeyRow k_keys[] = {
    {"minwindspeed", "minwindspeed", KeyClass::header_whole},
    {"maxwindspeed", "maxwindspeed", KeyClass::header_whole},
    {"gravity", "gravity", KeyClass::header_whole},
    {"waterdamage", "waterdamage", KeyClass::header_whole},
    {"lavaworld", "lavaworld", KeyClass::header_bit},
    {"waterdoesdamage", "waterdoesdamage", KeyClass::header_bit},
    {"nosealeveltrigger", "nosealeveltrigger", KeyClass::header_bit},
    {"tidalstrength", "tidalstrength", KeyClass::header_real},
    {"killmul", "killmul", KeyClass::header_real},
    {"timemul", "timemul", KeyClass::header_real},
    {"missionname", "missionname", KeyClass::header_frozen},
    {"missiondescription", "missiondescription", KeyClass::header_frozen},
    {"planet", "planet", KeyClass::header_frozen},
    {"numplayers", "numplayers", KeyClass::header_frozen},
    {"memory", "memory", KeyClass::header_frozen},
    {"size", "size", KeyClass::header_frozen},
    {"brief", "brief", KeyClass::header_frozen},
    {"narration", "narration", KeyClass::header_frozen},
    {"missionhint", "missionhint", KeyClass::header_frozen},
    {"glamour", "glamour", KeyClass::header_frozen},
    {"useonlyunits", "UseOnlyUnits", KeyClass::header_frozen},
    {"mapping", "mapping", KeyClass::header_frozen},
    {"lineofsight", "lineofsight", KeyClass::header_frozen},
    {"maxunits", "maxunits", KeyClass::header_frozen},
    {"aiprofile", "aiprofile", KeyClass::schema_profile},
    {"humanmetal", "HumanMetal", KeyClass::schema_resource},
    {"humanenergy", "HumanEnergy", KeyClass::schema_resource},
    {"computermetal", "ComputerMetal", KeyClass::schema_resource},
    {"computerenergy", "ComputerEnergy", KeyClass::schema_resource},
    {"surfacemetal", "SurfaceMetal", KeyClass::schema_metal},
    {"type", "type", KeyClass::schema_frozen},
    {"index", "index", KeyClass::schema_frozen},
    {"players", "players", KeyClass::schema_frozen},
};

struct MatchBits {
    uint8_t kind{kGlob};
};

struct Box {
    void* work{};
    uint8_t kind{kBoxMap};
    int32_t schema{-1};
    int32_t item{-1};
};

struct MapWork {
    oa::AssetStore* store{};
    const defs::DataLayout* layout{};
    const ChosenMap* chosen{};
    const std::vector<JournalEntry>* earlier{};
    tdf::OwnedDocument document;
    uint16_t mutator{0};
    char error[kErrorLimit]{};
    std::size_t error_len{0};
    std::vector<JournalEntry> entries;
    std::vector<Conflict> conflicts;
    std::vector<std::string> features;
    bool features_ready{false};
    bool features_failed{false};
};

struct ApplyBox {
    MapWork* work{};
    const MapScriptContext* context{};
    int ref{LUA_NOREF};
    bool function_entry{false};
};

enum class ValueKind : uint8_t { number, text, scale };

struct ValueAction {
    ValueKind kind{ValueKind::number};
    double number{0};
    double low{0};
    double high{0};
    bool clamp{false};
    std::string text;
};

struct ChangeItem {
    std::string raw;
    std::string folded;
    ValueAction action;
};

void set_error(MapWork* work, std::string_view text) noexcept {
    if (work == nullptr)
        return;
    const std::size_t count = std::min(text.size(), kErrorLimit - 1);
    if (count > 0)
        std::memcpy(work->error, text.data(), count);
    work->error[count] = '\0';
    work->error_len = count;
}

std::string fold_copy(std::string_view text) {
    std::string folded(text);
    for (char& piece : folded) {
        if (piece >= 'A' && piece <= 'Z')
            piece = static_cast<char>(piece - 'A' + 'a');
    }
    return folded;
}

bool same_text(std::string_view left, std::string_view right) {
    if (left.size() != right.size())
        return false;
    for (std::size_t offset = 0; offset < left.size(); ++offset) {
        unsigned char lhs = static_cast<unsigned char>(left[offset]);
        unsigned char rhs = static_cast<unsigned char>(right[offset]);
        if (lhs >= 'A' && lhs <= 'Z')
            lhs = static_cast<unsigned char>(lhs - 'A' + 'a');
        if (rhs >= 'A' && rhs <= 'Z')
            rhs = static_cast<unsigned char>(rhs - 'A' + 'a');
        if (lhs != rhs)
            return false;
    }
    return true;
}

bool starts_with_nocase(std::string_view text, std::string_view prefix) {
    if (text.size() < prefix.size())
        return false;
    return same_text(text.substr(0, prefix.size()), prefix);
}

const KeyRow* key_row(std::string_view folded) {
    for (const KeyRow& row : k_keys) {
        if (folded == row.folded)
            return &row;
    }
    return nullptr;
}

bool header_class(KeyClass kind) noexcept {
    return kind == KeyClass::header_whole || kind == KeyClass::header_bit ||
           kind == KeyClass::header_real || kind == KeyClass::header_frozen;
}

bool schema_class(KeyClass kind) noexcept {
    return kind == KeyClass::schema_profile || kind == KeyClass::schema_resource ||
           kind == KeyClass::schema_metal || kind == KeyClass::schema_frozen;
}

std::string_view trim_ends(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.remove_suffix(1);
    return text;
}

bool fully_numeric(std::string_view text, double& number) {
    if (text.empty())
        return false;
    std::size_t cursor = 0;
    if (text[cursor] == '+' || text[cursor] == '-')
        ++cursor;
    bool digit = false;
    bool dot = false;
    for (; cursor < text.size(); ++cursor) {
        const char piece = text[cursor];
        if (piece >= '0' && piece <= '9') {
            digit = true;
            continue;
        }
        if (piece == '.' && !dot) {
            dot = true;
            continue;
        }
        return false;
    }
    if (!digit)
        return false;
    const std::string copy(text);
    number = tdf::parse_double(copy.c_str());
    return std::isfinite(number);
}

bool whole_number(double value, double& integral) {
    if (!std::isfinite(value))
        return false;
    if (std::modf(value, &integral) != 0.0)
        return false;
    return true;
}

bool glob_match(std::string_view pattern, std::string_view text) {
    std::size_t pattern_at = 0;
    std::size_t text_at = 0;
    std::size_t star = std::string_view::npos;
    std::size_t mark = 0;
    while (text_at < text.size()) {
        if (pattern_at < pattern.size() &&
            (pattern[pattern_at] == '?' ||
             same_text(pattern.substr(pattern_at, 1), text.substr(text_at, 1)))) {
            ++pattern_at;
            ++text_at;
            continue;
        }
        if (pattern_at < pattern.size() && pattern[pattern_at] == '*') {
            star = pattern_at;
            ++pattern_at;
            mark = text_at;
            continue;
        }
        if (star != std::string_view::npos) {
            pattern_at = star + 1;
            ++mark;
            text_at = mark;
            continue;
        }
        return false;
    }
    while (pattern_at < pattern.size() && pattern[pattern_at] == '*')
        ++pattern_at;
    return pattern_at == pattern.size();
}

void ensure_match_meta(lua_State* state) {
    if (luaL_newmetatable(state, kMatchMeta) == 0) {
        lua_pop(state, 1);
        return;
    }
    lua_pushboolean(state, 0);
    lua_setfield(state, -2, "__metatable");
    lua_pop(state, 1);
}

void push_match(lua_State* state, uint8_t kind, const char* text, std::size_t length) {
    ensure_match_meta(state);
    void* memory = lua_newuserdatauv(state, sizeof(MatchBits), 1);
    auto* bits = new (memory) MatchBits();
    bits->kind = kind;
    luaL_setmetatable(state, kMatchMeta);
    lua_pushlstring(state, text, length);
    lua_setiuservalue(state, -2, 1);
}

int form_glob(lua_State* state) {
    std::size_t length = 0;
    const char* text = luaL_checklstring(state, 1, &length);
    if (length > static_cast<std::size_t>(kGlobLimit)) {
        lua_pushliteral(state, "a glob is at most 256 bytes");
        return lua_error(state);
    }
    push_match(state, kGlob, text, length);
    return 1;
}

int form_pattern(lua_State* state) {
    std::size_t length = 0;
    const char* text = luaL_checklstring(state, 1, &length);
    if (length > sm::max_text_bytes) {
        lua_pushliteral(state, "text is longer than 1024 bytes");
        return lua_error(state);
    }
    push_match(state, kPattern, text, length);
    return 1;
}

MapWork* work_of(lua_State* state, int index) {
    const auto* box = static_cast<const Box*>(luaL_testudata(state, index, kMapMeta));
    if (box == nullptr)
        box = static_cast<const Box*>(luaL_testudata(state, index, kHeaderMeta));
    if (box == nullptr)
        box = static_cast<const Box*>(luaL_testudata(state, index, kSchemasMeta));
    if (box == nullptr)
        box = static_cast<const Box*>(luaL_testudata(state, index, kSchemaMeta));
    if (box == nullptr)
        box = static_cast<const Box*>(luaL_testudata(state, index, kUnitsMeta));
    if (box == nullptr)
        box = static_cast<const Box*>(luaL_testudata(state, index, kFeaturesMeta));
    if (box == nullptr)
        box = static_cast<const Box*>(luaL_testudata(state, index, kUnitMeta));
    if (box == nullptr)
        box = static_cast<const Box*>(luaL_testudata(state, index, kFeatureMeta));
    if (box == nullptr || box->work == nullptr)
        return nullptr;
    return static_cast<MapWork*>(box->work);
}

const Box* box_of(lua_State* state, int index, const char* meta) {
    return static_cast<const Box*>(luaL_testudata(state, index, meta));
}

void ensure_box_meta(
    lua_State* state,
    const char* name,
    lua_CFunction index,
    lua_CFunction newindex,
    lua_CFunction length
) {
    if (luaL_newmetatable(state, name) == 0) {
        lua_pop(state, 1);
        return;
    }
    lua_pushcfunction(state, index);
    lua_setfield(state, -2, "__index");
    if (newindex != nullptr) {
        lua_pushcfunction(state, newindex);
        lua_setfield(state, -2, "__newindex");
    }
    if (length != nullptr) {
        lua_pushcfunction(state, length);
        lua_setfield(state, -2, "__len");
    }
    lua_pushboolean(state, 0);
    lua_setfield(state, -2, "__metatable");
    lua_pop(state, 1);
}

void push_box(
    lua_State* state, MapWork* work, uint8_t kind, const char* meta, int32_t schema, int32_t item
) {
    void* memory = lua_newuserdatauv(state, sizeof(Box), kind == kBoxFiltered ? 1 : 0);
    auto* box = new (memory) Box();
    box->work = work;
    box->kind = kind;
    box->schema = schema;
    box->item = item;
    luaL_setmetatable(state, meta);
}

tdf::Block* root_block(MapWork* work) {
    return tdf::editable_root(work->document.get());
}

tdf::Block* header_block(MapWork* work) {
    return tdf::editable_child(root_block(work), "GlobalHeader");
}

tdf::Block* schema_block(MapWork* work, int number) {
    // Schemas are children of GlobalHeader. The session loader looks up
    // "Schema N" from that block, and stops at the first missing number.
    tdf::Block* header = header_block(work);
    if (header == nullptr)
        return nullptr;
    const std::string name = "Schema " + std::to_string(number);
    return tdf::editable_child(header, name);
}

int schema_count(MapWork* work) {
    int count = 0;
    for (int number = 0; number < kSchemaLimit; ++number) {
        if (schema_block(work, number) == nullptr)
            break;
        ++count;
    }
    return count;
}

int start_positions(const tdf::Block* schema) {
    const tdf::Block* specials = tdf::find_child(schema, "specials");
    int count = 0;
    for (uint32_t child = 0; child < tdf::child_count(specials); ++child) {
        const char* what = tdf::find_value(tdf::child_at(specials, child), "specialwhat");
        if (what != nullptr && starts_with_nocase(what, "StartPos"))
            ++count;
    }
    return count;
}

std::string map_record(const ChosenMap& chosen) {
    return "map:" + chosen.key;
}

std::string schema_record(const ChosenMap& chosen, int number) {
    return map_record(chosen) + "/Schema " + std::to_string(number);
}

void note_conflict(MapWork* work, const JournalEntry& entry) {
    const auto consider = [&](const JournalEntry& prior) {
        if (prior.change != Change::set || prior.field != entry.field ||
            prior.record != entry.record || prior.block != entry.block || prior.mutators.empty() ||
            entry.mutators.empty())
            return;
        if (prior.mutators.front() == entry.mutators.front())
            return;
        Conflict conflict;
        conflict.severity = Severity::note;
        conflict.kind = ConflictKind::same_field_hooked;
        conflict.mutators.push_back(prior.mutators.front());
        conflict.mutators.push_back(entry.mutators.front());
        conflict.record = entry.record;
        conflict.field = entry.field;
        conflict.path = entry.path;
        work->conflicts.push_back(std::move(conflict));
    };
    for (const JournalEntry& prior : work->entries)
        consider(prior);
    if (work->earlier != nullptr) {
        for (const JournalEntry& prior : *work->earlier)
            consider(prior);
    }
}

bool remember(MapWork* work, JournalEntry entry) {
    try {
        note_conflict(work, entry);
        work->entries.push_back(std::move(entry));
        return true;
    } catch (const std::bad_alloc&) {
        set_error(work, "out of memory");
        return false;
    }
}

bool store_text(
    MapWork* work,
    tdf::Block* block,
    std::string_view spelling,
    std::string_view written,
    std::string_view record,
    std::string_view block_name
) {
    const std::string key(spelling);
    const char* existing = tdf::find_value(block, key.c_str());
    if (existing != nullptr && written == existing)
        return true;
    const tdf::EditStatus status = tdf::set_value(work->document.get(), block, spelling, written);
    if (status != tdf::EditStatus::ok) {
        set_error(work, "the chosen map could not be written");
        return false;
    }
    JournalEntry entry;
    entry.mutators.push_back(work->mutator);
    entry.phase = Phase::hooks;
    entry.collection = Collection::other;
    entry.change = Change::set;
    entry.record = std::string(record);
    entry.block = std::string(block_name);
    entry.field = fold_copy(spelling);
    entry.path = work->chosen->ota_path;
    if (existing != nullptr)
        entry.old_text = existing;
    entry.new_text = std::string(written);
    return remember(work, std::move(entry));
}

void push_stored(lua_State* state, const char* text) {
    if (text == nullptr) {
        lua_pushnil(state);
        return;
    }
    double number = 0;
    if (fully_numeric(text, number))
        lua_pushnumber(state, number);
    else
        lua_pushstring(state, text);
}

std::string header_limit_message(const KeyRow& row) {
    std::string message = "map.header.";
    message += row.folded;
    if (row.kind == KeyClass::header_bit)
        message += " must be 0 or 1";
    else if (row.kind == KeyClass::header_real)
        message += " must be a number from 0 to 1000";
    else
        message += " must be a whole number from 0 to 32767";
    return message;
}

bool number_in_header(const KeyRow& row, double value, double& accepted) {
    if (row.kind == KeyClass::header_real) {
        if (!std::isfinite(value) || value < 0.0 || value > 1000.0)
            return false;
        accepted = value;
        return true;
    }
    double integral = 0;
    if (!whole_number(value, integral))
        return false;
    if (row.kind == KeyClass::header_bit)
        return integral == 0.0 || integral == 1.0 ? (accepted = integral, true) : false;
    if (integral < 0.0 || integral > 32767.0)
        return false;
    accepted = integral;
    return true;
}

bool read_action(
    lua_State* state, int index, const KeyRow* row, ValueAction& action, std::string& problem
) {
    index = lua_absindex(state, index);
    sm::ChangeKind change = sm::ChangeKind::assign;
    double value = 0;
    double low = 0;
    double high = 0;
    if (rule_change_at(state, index, &change, &value, &low, &high)) {
        if (change == sm::ChangeKind::scale || change == sm::ChangeKind::scale_clamp) {
            if (row != nullptr && row->kind == KeyClass::schema_profile) {
                problem = "map.schemas.aiprofile cannot be scaled";
                return false;
            }
            action.kind = ValueKind::scale;
            action.number = value;
            action.low = low;
            action.high = high;
            action.clamp = change == sm::ChangeKind::scale_clamp;
            return true;
        }
        if (change == sm::ChangeKind::assign) {
            action.kind = ValueKind::number;
            action.number = value;
            return true;
        }
        if (change == sm::ChangeKind::assign_text) {
            if (lua_getiuservalue(state, index, 1) == LUA_TSTRING) {
                std::size_t length = 0;
                const char* text = lua_tolstring(state, -1, &length);
                action.text.assign(text != nullptr ? text : "", length);
            }
            lua_pop(state, 1);
            action.kind = ValueKind::text;
            return true;
        }
        problem = "the maps change must be a value or a scale";
        return false;
    }
    if (lua_type(state, index) == LUA_TBOOLEAN) {
        action.kind = ValueKind::number;
        action.number = lua_toboolean(state, index) != 0 ? 1.0 : 0.0;
        return true;
    }
    if (lua_type(state, index) == LUA_TNUMBER) {
        action.kind = ValueKind::number;
        action.number = lua_tonumber(state, index);
        if (!std::isfinite(action.number)) {
            problem = "a maps value must be finite";
            return false;
        }
        return true;
    }
    if (lua_type(state, index) == LUA_TSTRING) {
        std::size_t length = 0;
        const char* text = lua_tolstring(state, index, &length);
        action.kind = ValueKind::text;
        action.text.assign(text != nullptr ? text : "", length);
        return true;
    }
    problem = "the maps change must be a value or a scale";
    return false;
}

std::string type_problem(const KeyRow& row) {
    if (row.kind == KeyClass::schema_profile)
        return "map.schemas.aiprofile must be text";
    if (row.kind == KeyClass::schema_resource) {
        std::string message = "map.schemas.";
        message += row.folded;
        message += " must be a whole number from 0 to 1000000";
        return message;
    }
    if (row.kind == KeyClass::schema_metal)
        return "map.schemas.surfacemetal must be a whole number from 0 to 255";
    return header_limit_message(row);
}

bool action_fits(
    const KeyRow& row, const ValueAction& action, std::string& problem, bool scaling = false
) {
    if (row.kind == KeyClass::header_frozen) {
        problem = "map.header.";
        problem += row.folded;
        problem += " cannot change";
        return false;
    }
    if (row.kind == KeyClass::schema_frozen) {
        problem = "schema.";
        problem += row.folded;
        problem += " cannot change";
        return false;
    }
    if (row.kind == KeyClass::schema_profile) {
        if (scaling || action.kind == ValueKind::scale) {
            problem = "map.schemas.aiprofile cannot be scaled";
            return false;
        }
        if (action.kind != ValueKind::text) {
            problem = type_problem(row);
            return false;
        }
        return true;
    }
    if (action.kind == ValueKind::text) {
        problem = type_problem(row);
        return false;
    }
    if (action.kind == ValueKind::number) {
        double accepted = 0;
        bool ok = false;
        if (header_class(row.kind))
            ok = number_in_header(row, action.number, accepted);
        else if (row.kind == KeyClass::schema_metal) {
            double integral = 0;
            ok = whole_number(action.number, integral) && integral >= 0.0 && integral <= 255.0;
        } else {
            double integral = 0;
            ok = whole_number(action.number, integral) && integral >= 0.0 && integral <= 1000000.0;
        }
        if (!ok) {
            problem = type_problem(row);
            return false;
        }
    }
    return true;
}

bool profile_text(std::string_view text, std::string& problem) {
    if (text.empty() || text.size() > 31) {
        problem = "aiprofile must be 1 to 31 letters, digits, _ or -";
        return false;
    }
    for (const char piece : text) {
        const bool letter = (piece >= 'A' && piece <= 'Z') || (piece >= 'a' && piece <= 'z');
        const bool digit = piece >= '0' && piece <= '9';
        if (!letter && !digit && piece != '_' && piece != '-') {
            problem = "aiprofile must be 1 to 31 letters, digits, _ or -";
            return false;
        }
    }
    return true;
}

bool profile_present(MapWork* work, std::string_view name) {
    const std::size_t ai = static_cast<std::size_t>(defs::DataDirectory::ai);
    const std::string path = work->layout->directories[ai] + "/" + std::string(name) + ".txt";
    oa::ResourceFile* file = work->store->open(path);
    if (file == nullptr) {
        set_error(work, path + " is not there");
        return false;
    }
    oa::AssetStore::close(file);
    return true;
}

bool apply_on_block(
    MapWork* work,
    tdf::Block* block,
    const KeyRow& row,
    const ValueAction& action,
    std::string_view record,
    std::string_view block_name
) {
    if (block == nullptr) {
        set_error(work, header_class(row.kind) ? "the map has no header" : "the map has no schema");
        return false;
    }
    std::string problem;
    if (!action_fits(row, action, problem)) {
        set_error(work, problem);
        return false;
    }
    if (action.kind == ValueKind::scale) {
        const char* existing = tdf::find_value(block, row.spelling);
        double current = 0;
        if (existing == nullptr || !fully_numeric(existing, current)) {
            std::string message = header_class(row.kind) ? "map.header." : "map.schemas.";
            message += row.folded;
            message += " has no number";
            set_error(work, message);
            return false;
        }
        double scaled = current * action.number;
        if (action.clamp) {
            if (scaled < action.low)
                scaled = action.low;
            if (scaled > action.high)
                scaled = action.high;
        }
        ValueAction assigned;
        assigned.kind = ValueKind::number;
        assigned.number = scaled;
        return apply_on_block(work, block, row, assigned, record, block_name);
    }
    if (row.kind == KeyClass::schema_profile) {
        if (!profile_text(action.text, problem) || !profile_present(work, action.text)) {
            if (work->error_len == 0)
                set_error(work, problem);
            return false;
        }
        return store_text(work, block, row.spelling, action.text, record, block_name);
    }
    double accepted = action.number;
    if (header_class(row.kind)) {
        if (!number_in_header(row, action.number, accepted)) {
            set_error(work, header_limit_message(row));
            return false;
        }
    }
    const std::optional<std::string> spelled = tdf::format_number(accepted);
    if (!spelled.has_value()) {
        set_error(work, "the chosen map could not be written");
        return false;
    }
    return store_text(work, block, row.spelling, *spelled, record, block_name);
}

bool apply_header(MapWork* work, const KeyRow& row, const ValueAction& action) {
    return apply_on_block(work, header_block(work), row, action, map_record(*work->chosen), {});
}

bool apply_schema(MapWork* work, int number, const KeyRow& row, const ValueAction& action) {
    return apply_on_block(
        work, schema_block(work, number), row, action, schema_record(*work->chosen, number), {}
    );
}

bool apply_every_schema(MapWork* work, const KeyRow& row, const ValueAction& action) {
    const int count = schema_count(work);
    for (int number = 0; number < count; ++number) {
        if (!apply_schema(work, number, row, action))
            return false;
    }
    return true;
}

bool load_features(MapWork* work) {
    if (work->features_ready)
        return !work->features_failed;
    work->features_ready = true;
    try {
        const std::vector<std::string> paths =
            work->store->list_effective_recursive("features", "tdf", true);
        for (const std::string& path : paths) {
            const oa::AssetData data = work->store->read(path);
            const std::string text(data.bytes.begin(), data.bytes.end());
            tdf::OwnedDocument parsed;
            if (!parsed.parse(text, nullptr) || parsed.root() == nullptr)
                continue;
            for (uint32_t child = 0; child < tdf::child_count(parsed.root()); ++child) {
                const tdf::Block* section = tdf::child_at(parsed.root(), child);
                if (section == nullptr || section->name == nullptr || section->name[0] == '\0')
                    continue;
                work->features.emplace_back(section->name);
            }
        }
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception&) {
        work->features_failed = true;
        set_error(work, "the features could not be read");
        return false;
    }
    return true;
}

bool feature_known(MapWork* work, std::string_view name) {
    if (!load_features(work))
        return false;
    for (const std::string& have : work->features) {
        if (same_text(have, name))
            return true;
    }
    set_error(work, "the feature " + std::string(name) + " is not there");
    return false;
}

bool unit_known(MapWork* work, std::string_view name) {
    const std::size_t units = static_cast<std::size_t>(defs::DataDirectory::units);
    const std::string path = work->layout->directories[units] + "/" + std::string(name) + "." +
                             work->layout->unit_extension;
    oa::ResourceFile* file = work->store->open(path);
    if (file == nullptr) {
        set_error(work, "the unit " + std::string(name) + " is not there");
        return false;
    }
    oa::AssetStore::close(file);
    return true;
}

tdf::Block* placed_parent(MapWork* work, int schema, bool features) {
    tdf::Block* schema_node = schema_block(work, schema);
    if (schema_node == nullptr)
        return nullptr;
    if (features)
        return tdf::editable_child(schema_node, "features");
    return tdf::editable_child(schema_node, work->layout->map_units_section);
}

tdf::Block* placed_child(MapWork* work, int schema, bool features, int item) {
    tdf::Block* parent = placed_parent(work, schema, features);
    if (parent == nullptr || item < 0)
        return nullptr;
    return tdf::editable_child(parent, static_cast<uint32_t>(item));
}

bool write_placed(
    MapWork* work,
    int schema,
    bool features,
    int item,
    std::string_view key,
    std::string_view written
) {
    tdf::Block* child = placed_child(work, schema, features, item);
    if (child == nullptr) {
        set_error(work, features ? "the map has no such feature" : "the map has no such unit");
        return false;
    }
    const char* spelling = key.data();
    if (same_text(key, "unitname"))
        spelling = "UnitName";
    else if (same_text(key, "featurename"))
        spelling = "Featurename";
    else if (same_text(key, "xpos"))
        spelling = "XPos";
    else if (same_text(key, "zpos"))
        spelling = "ZPos";
    else if (same_text(key, "player"))
        spelling = "Player";
    const std::string block_name = std::to_string(item);
    return store_text(
        work, child, spelling, written, schema_record(*work->chosen, schema), block_name
    );
}

bool remove_placed(MapWork* work, int schema, bool features, int item) {
    tdf::Block* parent = placed_parent(work, schema, features);
    tdf::Block* child = placed_child(work, schema, features, item);
    if (parent == nullptr || child == nullptr) {
        set_error(work, features ? "the map has no such feature" : "the map has no such unit");
        return false;
    }
    const char* field = features ? "featurename" : "unitname";
    const char* existing = tdf::find_value(child, field);
    const tdf::EditStatus status =
        tdf::remove_child(work->document.get(), parent, static_cast<uint32_t>(item));
    if (status != tdf::EditStatus::ok) {
        set_error(work, "the chosen map could not be written");
        return false;
    }
    JournalEntry entry;
    entry.mutators.push_back(work->mutator);
    entry.phase = Phase::hooks;
    entry.collection = Collection::other;
    entry.change = Change::removed;
    entry.record = schema_record(*work->chosen, schema);
    entry.block = std::to_string(item);
    entry.field = field;
    entry.path = work->chosen->ota_path;
    if (existing != nullptr)
        entry.old_text = existing;
    return remember(work, std::move(entry));
}

bool assign_placed(
    MapWork* work,
    int schema,
    bool features,
    int item,
    std::string_view key,
    const ValueAction& action
) {
    const std::string folded = fold_copy(key);
    if (features) {
        if (folded != "featurename") {
            set_error(work, "a placed feature's " + folded + " cannot change");
            return false;
        }
        if (action.kind != ValueKind::text) {
            set_error(work, "a placed feature's featurename must be text");
            return false;
        }
        if (action.text.size() > kFeatureNameLimit) {
            set_error(work, "a feature name must be shorter than 128 bytes");
            return false;
        }
        if (!feature_known(work, action.text))
            return false;
        return write_placed(work, schema, true, item, folded, action.text);
    }
    if (folded == "unitname") {
        if (action.kind != ValueKind::text) {
            set_error(work, "a placed unit's unitname must be text");
            return false;
        }
        if (!unit_known(work, action.text))
            return false;
        return write_placed(work, schema, false, item, folded, action.text);
    }
    if (folded != "xpos" && folded != "zpos" && folded != "player") {
        set_error(work, "a placed unit's " + folded + " cannot change");
        return false;
    }
    if (action.kind != ValueKind::number) {
        set_error(
            work,
            folded == "player"
                ? "a placed unit's player must be a whole number from 1 to 255"
                : "a placed unit's " + folded + " must be a whole number from 0 to 32767"
        );
        return false;
    }
    double integral = 0;
    const bool player = folded == "player";
    const bool ok =
        whole_number(action.number, integral) &&
        (player ? integral >= 1.0 && integral <= 255.0 : integral >= 0.0 && integral <= 32767.0);
    if (!ok) {
        set_error(
            work,
            player ? "a placed unit's player must be a whole number from 1 to 255"
                   : "a placed unit's " + folded + " must be a whole number from 0 to 32767"
        );
        return false;
    }
    const std::optional<std::string> spelled = tdf::format_number(integral);
    if (!spelled.has_value()) {
        set_error(work, "the chosen map could not be written");
        return false;
    }
    return write_placed(work, schema, false, item, folded, *spelled);
}

std::string mission_name(MapWork* work) {
    const char* written = tdf::find_value(header_block(work), "missionname");
    if (written != nullptr && written[0] != '\0')
        return written;
    return work->chosen->key;
}

bool where_text(MapWork* work, std::string_view field, std::string& text, bool& present) {
    const std::string folded = fold_copy(field);
    present = false;
    if (folded == "name") {
        text = mission_name(work);
        present = true;
        return true;
    }
    if (folded == "file") {
        text = work->chosen->key;
        present = true;
        return true;
    }
    if (folded == "pack") {
        text = work->chosen->pack;
        present = true;
        return true;
    }
    const char* written = tdf::find_value(header_block(work), folded.c_str());
    if (written == nullptr)
        return true;
    text = written;
    present = true;
    return true;
}

bool contains_text(std::string_view hay, std::string_view needle) {
    if (needle.empty())
        return true;
    const std::string folded_hay = fold_copy(hay);
    const std::string folded_needle = fold_copy(needle);
    return folded_hay.find(folded_needle) != std::string::npos;
}

enum class Tri : uint8_t { no, yes, stop };

Tri pattern_find(lua_State* state, MapWork* work, std::string_view pattern, std::string_view text) {
    lua_getglobal(state, "string");
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        set_error(work, "string.find is not there");
        return Tri::stop;
    }
    lua_getfield(state, -1, "find");
    lua_remove(state, -2);
    if (!lua_isfunction(state, -1)) {
        lua_pop(state, 1);
        set_error(work, "string.find is not there");
        return Tri::stop;
    }
    lua_pushlstring(state, text.data(), text.size());
    lua_pushlstring(state, pattern.data(), pattern.size());
    if (lua_pcall(state, 2, 1, 0) != LUA_OK) {
        std::size_t length = 0;
        const char* message = lua_tolstring(state, -1, &length);
        set_error(
            work,
            message != nullptr ? std::string_view(message, length)
                               : std::string_view("a pattern is not valid")
        );
        lua_pop(state, 1);
        return Tri::stop;
    }
    const bool found = !lua_isnil(state, -1);
    lua_pop(state, 1);
    return found ? Tri::yes : Tri::no;
}

Tri match_value(lua_State* state, MapWork* work, std::string_view field, int value_index) {
    std::string text;
    bool present = false;
    where_text(work, field, text, present);
    if (map_match_value(state, value_index)) {
        if (!present)
            return Tri::no;
        const auto* bits =
            static_cast<const MatchBits*>(luaL_testudata(state, value_index, kMatchMeta));
        std::string pattern;
        if (lua_getiuservalue(state, value_index, 1) == LUA_TSTRING) {
            std::size_t length = 0;
            const char* raw = lua_tolstring(state, -1, &length);
            pattern.assign(raw != nullptr ? raw : "", length);
        }
        lua_pop(state, 1);
        if (bits != nullptr && bits->kind == kPattern)
            return pattern_find(state, work, pattern, text);
        return glob_match(pattern, text) ? Tri::yes : Tri::no;
    }
    sm::Need need;
    std::string reason;
    if (!need_from_lua(state, value_index, field, need, &reason)) {
        set_error(work, reason);
        return Tri::stop;
    }
    bool met = false;
    if (need.kind == sm::NeedKind::contains && need.text.empty()) {
        met = present;
    } else if (!present) {
        met = false;
    } else if (need.kind == sm::NeedKind::range) {
        double number = 0;
        met = fully_numeric(text, number) && need.low <= number && number <= need.high;
    } else if (need.kind == sm::NeedKind::equals) {
        met = same_text(text, need.text);
    } else {
        met = contains_text(text, need.text);
    }
    if (need.negated)
        met = !met;
    return met ? Tri::yes : Tri::no;
}

bool map_where_holds(lua_State* state, MapWork* work, int table, bool& holds) {
    holds = true;
    table = lua_absindex(state, table);
    lua_pushnil(state);
    while (lua_next(state, table) != 0) {
        if (lua_type(state, -2) != LUA_TSTRING) {
            lua_pop(state, 2);
            set_error(work, "a maps where key must be text");
            return false;
        }
        std::size_t length = 0;
        const char* raw = lua_tolstring(state, -2, &length);
        const std::string field(trim_ends(std::string_view(raw != nullptr ? raw : "", length)));
        const Tri matched = match_value(state, work, field, lua_gettop(state));
        lua_pop(state, 1);
        if (matched == Tri::stop) {
            lua_pop(state, 1);
            return false;
        }
        if (matched == Tri::no) {
            lua_pop(state, 1);
            holds = false;
            return true;
        }
    }
    return true;
}

bool collect_changes(
    lua_State* state,
    int table,
    std::vector<ChangeItem>& items,
    std::string& problem,
    bool scaling = false
) {
    table = lua_absindex(state, table);
    lua_pushnil(state);
    while (lua_next(state, table) != 0) {
        if (lua_type(state, -2) != LUA_TSTRING) {
            problem = "a maps change key must be text";
            lua_pop(state, 2);
            return false;
        }
        std::size_t length = 0;
        const char* raw_text = lua_tolstring(state, -2, &length);
        ChangeItem item;
        item.raw.assign(trim_ends(std::string_view(raw_text != nullptr ? raw_text : "", length)));
        item.folded = fold_copy(item.raw);
        const KeyRow* row = key_row(item.folded);
        if (row == nullptr) {
            problem = "the maps change \"" + item.raw + "\" is not a header or schema setting";
            lua_pop(state, 2);
            return false;
        }
        if (!read_action(state, -1, row, item.action, problem) ||
            !action_fits(*row, item.action, problem, scaling)) {
            lua_pop(state, 2);
            return false;
        }
        items.push_back(std::move(item));
        lua_pop(state, 1);
    }
    std::sort(items.begin(), items.end(), [](const ChangeItem& left, const ChangeItem& right) {
        return host::name_less(left.raw, right.raw);
    });
    return true;
}

bool apply_change_list(MapWork* work, const std::vector<ChangeItem>& items, int only_schema) {
    for (const ChangeItem& item : items) {
        const KeyRow* row = key_row(item.folded);
        if (row == nullptr) {
            set_error(
                work, "the maps change \"" + item.raw + "\" is not a header or schema setting"
            );
            return false;
        }
        if (header_class(row->kind)) {
            if (!apply_header(work, *row, item.action))
                return false;
            continue;
        }
        if (only_schema >= 0) {
            if (!apply_schema(work, only_schema, *row, item.action))
                return false;
            continue;
        }
        if (!apply_every_schema(work, *row, item.action))
            return false;
    }
    return true;
}

std::string schema_field_text(MapWork* work, int number, std::string_view field, bool& present) {
    present = false;
    const std::string folded = fold_copy(field);
    tdf::Block* schema = schema_block(work, number);
    if (schema == nullptr)
        return {};
    if (folded == "index") {
        present = true;
        return std::to_string(number);
    }
    if (folded == "players") {
        present = true;
        return std::to_string(start_positions(schema));
    }
    const char* written = tdf::find_value(schema, folded.c_str());
    if (written == nullptr)
        return {};
    present = true;
    return written;
}

bool schema_need_holds(MapWork* work, int number, const std::vector<sm::Need>& needs) {
    for (const sm::Need& need : needs) {
        bool present = false;
        const std::string text = schema_field_text(work, number, need.field, present);
        bool met = false;
        if (need.kind == sm::NeedKind::contains && need.text.empty())
            met = present;
        else if (!present)
            met = false;
        else if (need.kind == sm::NeedKind::range) {
            double value = 0;
            met = fully_numeric(text, value) && need.low <= value && value <= need.high;
        } else if (need.kind == sm::NeedKind::equals)
            met = same_text(text, need.text);
        else
            met = contains_text(text, need.text);
        if (need.negated)
            met = !met;
        if (!met)
            return false;
    }
    return true;
}

bool read_schema_needs(lua_State* state, MapWork* work, int table, std::vector<sm::Need>& needs) {
    table = lua_absindex(state, table);
    lua_pushnil(state);
    while (lua_next(state, table) != 0) {
        if (lua_type(state, -2) != LUA_TSTRING) {
            lua_pop(state, 2);
            set_error(work, "a maps where key must be text");
            return false;
        }
        std::size_t length = 0;
        const char* raw = lua_tolstring(state, -2, &length);
        const std::string field(trim_ends(std::string_view(raw != nullptr ? raw : "", length)));
        sm::Need need;
        std::string reason;
        if (!need_from_lua(state, -1, field, need, &reason)) {
            lua_pop(state, 2);
            set_error(work, reason);
            return false;
        }
        needs.push_back(std::move(need));
        lua_pop(state, 1);
    }
    return true;
}

void push_schema(lua_State* state, MapWork* work, int number) {
    push_box(state, work, kBoxSchema, kSchemaMeta, static_cast<int32_t>(number), -1);
}

void push_filtered(lua_State* state, MapWork* work, const std::vector<int>& numbers) {
    push_box(state, work, kBoxFiltered, kSchemasMeta, -1, -1);
    lua_createtable(state, static_cast<int>(numbers.size()), 0);
    for (std::size_t slot = 0; slot < numbers.size(); ++slot) {
        lua_pushinteger(state, numbers[slot]);
        lua_rawseti(state, -2, static_cast<lua_Integer>(slot) + 1);
    }
    lua_setiuservalue(state, -2, 1);
}

bool filtered_numbers(lua_State* state, int index, std::vector<int>& numbers) {
    const Box* box = box_of(state, index, kSchemasMeta);
    if (box == nullptr)
        return false;
    if (box->kind == kBoxSchemas) {
        MapWork* work = static_cast<MapWork*>(box->work);
        const int count = schema_count(work);
        for (int number = 0; number < count; ++number)
            numbers.push_back(number);
        return true;
    }
    if (lua_getiuservalue(state, index, 1) != LUA_TTABLE) {
        lua_pop(state, 1);
        return true;
    }
    const lua_Unsigned count = lua_rawlen(state, -1);
    for (lua_Unsigned slot = 1; slot <= count; ++slot) {
        lua_rawgeti(state, -1, static_cast<lua_Integer>(slot));
        const int number = static_cast<int>(lua_tointeger(state, -1));
        lua_pop(state, 1);
        numbers.push_back(number);
    }
    lua_pop(state, 1);
    return true;
}

int schemas_len(lua_State* state) {
    host::charge_work(state, host::element_charge);
    std::vector<int> numbers;
    if (!filtered_numbers(state, 1, numbers)) {
        lua_pushinteger(state, 0);
        return 1;
    }
    lua_pushinteger(state, static_cast<lua_Integer>(numbers.size()));
    return 1;
}

int schemas_where(lua_State* state) {
    host::charge_work(state, host::table_walk_charge);
    MapWork* work = work_of(state, 1);
    if (work == nullptr) {
        lua_pushliteral(state, "the map is not there");
        return lua_error(state);
    }
    if (!lua_istable(state, 2)) {
        lua_pushliteral(state, "where expects a table");
        return lua_error(state);
    }
    bool ready = false;
    {
        std::vector<sm::Need> needs;
        std::vector<int> source;
        std::vector<int> kept;
        ready = read_schema_needs(state, work, 2, needs) && filtered_numbers(state, 1, source);
        if (ready) {
            for (const int number : source) {
                if (schema_need_holds(work, number, needs))
                    kept.push_back(number);
            }
            push_filtered(state, work, kept);
        }
    }
    if (!ready) {
        lua_pushlstring(state, work->error, work->error_len);
        return lua_error(state);
    }
    return 1;
}

bool apply_schema_table(lua_State* state, MapWork* work, bool factors) {
    std::vector<ChangeItem> items;
    std::string problem;
    if (!collect_changes(state, 2, items, problem, factors)) {
        set_error(work, problem);
        return false;
    }
    if (factors) {
        for (ChangeItem& item : items) {
            if (item.action.kind == ValueKind::number)
                item.action.kind = ValueKind::scale;
        }
    }
    std::vector<int> numbers;
    if (!filtered_numbers(state, 1, numbers))
        return false;
    for (const int number : numbers) {
        if (!apply_change_list(work, items, number))
            return false;
    }
    return true;
}

int schemas_set(lua_State* state) {
    host::charge_work(state, host::table_walk_charge);
    MapWork* work = work_of(state, 1);
    if (work == nullptr) {
        lua_pushliteral(state, "the map is not there");
        return lua_error(state);
    }
    if (!lua_istable(state, 2)) {
        lua_pushliteral(state, "set expects a table");
        return lua_error(state);
    }
    const bool applied = apply_schema_table(state, work, false);
    if (!applied) {
        lua_pushlstring(state, work->error, work->error_len);
        return lua_error(state);
    }
    return 0;
}

int schemas_scale(lua_State* state) {
    host::charge_work(state, host::table_walk_charge);
    MapWork* work = work_of(state, 1);
    if (work == nullptr) {
        lua_pushliteral(state, "the map is not there");
        return lua_error(state);
    }
    if (!lua_istable(state, 2)) {
        lua_pushliteral(state, "scale expects a table");
        return lua_error(state);
    }
    const bool applied = apply_schema_table(state, work, true);
    if (!applied) {
        lua_pushlstring(state, work->error, work->error_len);
        return lua_error(state);
    }
    return 0;
}

int schemas_each(lua_State* state) {
    host::charge_work(state, host::element_charge);
    MapWork* work = work_of(state, 1);
    if (work == nullptr) {
        lua_pushliteral(state, "the map is not there");
        return lua_error(state);
    }
    if (!lua_isfunction(state, 2)) {
        lua_pushliteral(state, "each expects a function");
        return lua_error(state);
    }
    int count = 0;
    {
        std::vector<int> numbers;
        if (!filtered_numbers(state, 1, numbers)) {
            lua_pushliteral(state, "the map is not there");
            return lua_error(state);
        }
        count = static_cast<int>(numbers.size());
        lua_createtable(state, count, 0);
        for (int slot = 0; slot < count; ++slot) {
            lua_pushinteger(state, numbers[static_cast<std::size_t>(slot)]);
            lua_rawseti(state, -2, static_cast<lua_Integer>(slot) + 1);
        }
    }
    for (int slot = 1; slot <= count; ++slot) {
        lua_pushvalue(state, 2);
        lua_rawgeti(state, 3, slot);
        const int number = static_cast<int>(lua_tointeger(state, -1));
        lua_pop(state, 1);
        push_schema(state, work, number);
        lua_call(state, 1, 0);
    }
    return 0;
}

int schemas_index(lua_State* state) {
    host::charge_work(state, host::element_charge);
    MapWork* work = work_of(state, 1);
    if (work == nullptr) {
        lua_pushnil(state);
        return 1;
    }
    if (lua_type(state, 2) == LUA_TSTRING) {
        std::size_t length = 0;
        const char* text = lua_tolstring(state, 2, &length);
        const std::string_view name(text != nullptr ? text : "", length);
        if (name == "each")
            lua_pushcfunction(state, schemas_each);
        else if (name == "where")
            lua_pushcfunction(state, schemas_where);
        else if (name == "set")
            lua_pushcfunction(state, schemas_set);
        else if (name == "scale")
            lua_pushcfunction(state, schemas_scale);
        else
            lua_pushnil(state);
        return 1;
    }
    std::vector<int> numbers;
    if (!filtered_numbers(state, 1, numbers)) {
        lua_pushnil(state);
        return 1;
    }
    if (lua_type(state, 2) != LUA_TNUMBER) {
        lua_pushnil(state);
        return 1;
    }
    const lua_Integer slot = lua_tointeger(state, 2);
    if (slot < 1 || static_cast<std::size_t>(slot) > numbers.size()) {
        lua_pushnil(state);
        return 1;
    }
    push_schema(state, work, numbers[static_cast<std::size_t>(slot) - 1]);
    return 1;
}

int schemas_newindex(lua_State* state) {
    lua_pushliteral(state, "a schema cannot be added");
    return lua_error(state);
}

int placed_len(lua_State* state) {
    host::charge_work(state, host::element_charge);
    const Box* box = box_of(state, 1, kUnitsMeta);
    bool features = false;
    if (box == nullptr) {
        box = box_of(state, 1, kFeaturesMeta);
        features = true;
    }
    if (box == nullptr || box->work == nullptr) {
        lua_pushinteger(state, 0);
        return 1;
    }
    tdf::Block* parent = placed_parent(static_cast<MapWork*>(box->work), box->schema, features);
    lua_pushinteger(state, static_cast<lua_Integer>(tdf::child_count(parent)));
    return 1;
}

int placed_index(lua_State* state) {
    host::charge_work(state, host::element_charge);
    const Box* units = box_of(state, 1, kUnitsMeta);
    const bool features = units == nullptr;
    const Box* box = features ? box_of(state, 1, kFeaturesMeta) : units;
    if (box == nullptr || lua_type(state, 2) != LUA_TNUMBER) {
        lua_pushnil(state);
        return 1;
    }
    const lua_Integer slot = lua_tointeger(state, 2);
    MapWork* work = static_cast<MapWork*>(box->work);
    tdf::Block* parent = placed_parent(work, box->schema, features);
    if (slot < 1 || parent == nullptr || static_cast<uint32_t>(slot) > tdf::child_count(parent)) {
        lua_pushnil(state);
        return 1;
    }
    push_box(
        state,
        work,
        features ? kBoxFeature : kBoxUnit,
        features ? kFeatureMeta : kUnitMeta,
        box->schema,
        static_cast<int32_t>(slot) - 1
    );
    return 1;
}

int placed_newindex(lua_State* state) {
    const Box* box = box_of(state, 1, kFeaturesMeta);
    if (box != nullptr)
        lua_pushliteral(state, "a placed feature cannot be added");
    else
        lua_pushliteral(state, "a placed unit cannot be added");
    return lua_error(state);
}

int placed_remove(lua_State* state) {
    host::charge_work(state, host::element_charge);
    const Box* unit = box_of(state, 1, kUnitMeta);
    const bool features = unit == nullptr;
    const Box* box = features ? box_of(state, 1, kFeatureMeta) : unit;
    if (box == nullptr || box->work == nullptr) {
        lua_pushliteral(state, "the map is not there");
        return lua_error(state);
    }
    MapWork* work = static_cast<MapWork*>(box->work);
    if (!remove_placed(work, box->schema, features, box->item)) {
        lua_pushlstring(state, work->error, work->error_len);
        return lua_error(state);
    }
    return 0;
}

int item_index(lua_State* state) {
    host::charge_work(state, host::element_charge);
    const Box* unit = box_of(state, 1, kUnitMeta);
    const bool features = unit == nullptr;
    const Box* box = features ? box_of(state, 1, kFeatureMeta) : unit;
    if (box == nullptr || box->work == nullptr || lua_type(state, 2) != LUA_TSTRING) {
        lua_pushnil(state);
        return 1;
    }
    std::size_t length = 0;
    const char* text = lua_tolstring(state, 2, &length);
    const std::string_view name(text != nullptr ? text : "", length);
    if (name == "remove") {
        lua_pushcfunction(state, placed_remove);
        return 1;
    }
    MapWork* work = static_cast<MapWork*>(box->work);
    tdf::Block* child = placed_child(work, box->schema, features, box->item);
    if (name == "index") {
        lua_pushinteger(state, static_cast<lua_Integer>(box->item) + 1);
        return 1;
    }
    push_stored(state, tdf::find_value(child, std::string(name).c_str()));
    return 1;
}

int item_newindex(lua_State* state) {
    host::charge_work(state, host::element_charge);
    const Box* unit = box_of(state, 1, kUnitMeta);
    const bool features = unit == nullptr;
    const Box* box = features ? box_of(state, 1, kFeatureMeta) : unit;
    if (box == nullptr || box->work == nullptr || lua_type(state, 2) != LUA_TSTRING) {
        lua_pushliteral(state, "a placed entry cannot change");
        return lua_error(state);
    }
    MapWork* work = static_cast<MapWork*>(box->work);
    std::size_t length = 0;
    const char* text = lua_tolstring(state, 2, &length);
    const std::string key(text != nullptr ? text : "", length);
    ValueAction action;
    std::string problem;
    if (!read_action(state, 3, nullptr, action, problem)) {
        set_error(work, problem);
        lua_pushlstring(state, work->error, work->error_len);
        return lua_error(state);
    }
    if (!assign_placed(work, box->schema, features, box->item, key, action)) {
        lua_pushlstring(state, work->error, work->error_len);
        return lua_error(state);
    }
    return 0;
}

int schema_index(lua_State* state) {
    host::charge_work(state, host::element_charge);
    const Box* box = box_of(state, 1, kSchemaMeta);
    if (box == nullptr || box->work == nullptr || lua_type(state, 2) != LUA_TSTRING) {
        lua_pushnil(state);
        return 1;
    }
    MapWork* work = static_cast<MapWork*>(box->work);
    std::size_t length = 0;
    const char* text = lua_tolstring(state, 2, &length);
    const std::string folded = fold_copy(std::string_view(text != nullptr ? text : "", length));
    tdf::Block* schema = schema_block(work, box->schema);
    if (folded == "index") {
        lua_pushinteger(state, box->schema);
        return 1;
    }
    if (folded == "players") {
        lua_pushinteger(state, start_positions(schema));
        return 1;
    }
    if (folded == "units") {
        push_box(state, work, kBoxUnits, kUnitsMeta, box->schema, -1);
        return 1;
    }
    if (folded == "features") {
        push_box(state, work, kBoxFeatures, kFeaturesMeta, box->schema, -1);
        return 1;
    }
    push_stored(state, tdf::find_value(schema, folded.c_str()));
    return 1;
}

int schema_newindex(lua_State* state) {
    host::charge_work(state, host::element_charge);
    const Box* box = box_of(state, 1, kSchemaMeta);
    if (box == nullptr || box->work == nullptr || lua_type(state, 2) != LUA_TSTRING) {
        lua_pushliteral(state, "a schema field cannot change");
        return lua_error(state);
    }
    MapWork* work = static_cast<MapWork*>(box->work);
    std::size_t length = 0;
    const char* text = lua_tolstring(state, 2, &length);
    const std::string folded = fold_copy(std::string_view(text != nullptr ? text : "", length));
    if (folded == "units" || folded == "features") {
        lua_pushliteral(state, "a placed unit cannot be added");
        return lua_error(state);
    }
    const KeyRow* row = key_row(folded);
    if (row == nullptr || !schema_class(row->kind)) {
        std::string message = "the maps change \"";
        message += folded;
        message += "\" is not a header or schema setting";
        lua_pushlstring(state, message.data(), message.size());
        return lua_error(state);
    }
    ValueAction action;
    std::string problem;
    if (!read_action(state, 3, row, action, problem)) {
        lua_pushlstring(state, problem.data(), problem.size());
        return lua_error(state);
    }
    if (!apply_schema(work, box->schema, *row, action)) {
        lua_pushlstring(state, work->error, work->error_len);
        return lua_error(state);
    }
    return 0;
}

int header_index(lua_State* state) {
    host::charge_work(state, host::element_charge);
    MapWork* work = work_of(state, 1);
    if (work == nullptr || lua_type(state, 2) != LUA_TSTRING) {
        lua_pushnil(state);
        return 1;
    }
    std::size_t length = 0;
    const char* text = lua_tolstring(state, 2, &length);
    const std::string folded = fold_copy(std::string_view(text != nullptr ? text : "", length));
    push_stored(state, tdf::find_value(header_block(work), folded.c_str()));
    return 1;
}

int header_newindex(lua_State* state) {
    host::charge_work(state, host::element_charge);
    MapWork* work = work_of(state, 1);
    if (work == nullptr || lua_type(state, 2) != LUA_TSTRING) {
        lua_pushliteral(state, "the map has no header");
        return lua_error(state);
    }
    if (header_block(work) == nullptr) {
        lua_pushliteral(state, "the map has no header");
        return lua_error(state);
    }
    std::size_t length = 0;
    const char* text = lua_tolstring(state, 2, &length);
    const std::string folded = fold_copy(std::string_view(text != nullptr ? text : "", length));
    const KeyRow* row = key_row(folded);
    if (row == nullptr || !header_class(row->kind) || row->kind == KeyClass::header_frozen) {
        std::string message = "map.header.";
        message += folded;
        message += " cannot change";
        lua_pushlstring(state, message.data(), message.size());
        return lua_error(state);
    }
    ValueAction action;
    std::string problem;
    if (!read_action(state, 3, row, action, problem)) {
        lua_pushlstring(state, problem.data(), problem.size());
        return lua_error(state);
    }
    if (!apply_header(work, *row, action)) {
        lua_pushlstring(state, work->error, work->error_len);
        return lua_error(state);
    }
    return 0;
}

int map_index(lua_State* state) {
    host::charge_work(state, host::element_charge);
    const Box* box = box_of(state, 1, kMapMeta);
    if (box == nullptr || box->work == nullptr || lua_type(state, 2) != LUA_TSTRING) {
        lua_pushnil(state);
        return 1;
    }
    MapWork* work = static_cast<MapWork*>(box->work);
    std::size_t length = 0;
    const char* text = lua_tolstring(state, 2, &length);
    const std::string_view name(text != nullptr ? text : "", length);
    if (name == "name") {
        const std::string shown = mission_name(work);
        lua_pushlstring(state, shown.data(), shown.size());
        return 1;
    }
    if (name == "file") {
        lua_pushlstring(state, work->chosen->key.data(), work->chosen->key.size());
        return 1;
    }
    if (name == "planet") {
        push_stored(state, tdf::find_value(header_block(work), "planet"));
        return 1;
    }
    if (name == "pack") {
        if (work->chosen->pack.empty())
            lua_pushnil(state);
        else
            lua_pushlstring(state, work->chosen->pack.data(), work->chosen->pack.size());
        return 1;
    }
    if (name == "header") {
        push_box(state, work, kBoxHeader, kHeaderMeta, -1, -1);
        return 1;
    }
    if (name == "schemas") {
        push_box(state, work, kBoxSchemas, kSchemasMeta, -1, -1);
        return 1;
    }
    lua_pushnil(state);
    return 1;
}

int map_newindex(lua_State* state) {
    lua_pushliteral(state, "the map cannot change there");
    return lua_error(state);
}

void install_view(lua_State* state) {
    ensure_box_meta(state, kMapMeta, map_index, map_newindex, nullptr);
    ensure_box_meta(state, kHeaderMeta, header_index, header_newindex, nullptr);
    ensure_box_meta(state, kSchemasMeta, schemas_index, schemas_newindex, schemas_len);
    ensure_box_meta(state, kSchemaMeta, schema_index, schema_newindex, nullptr);
    ensure_box_meta(state, kUnitsMeta, placed_index, placed_newindex, placed_len);
    ensure_box_meta(state, kFeaturesMeta, placed_index, placed_newindex, placed_len);
    ensure_box_meta(state, kUnitMeta, item_index, item_newindex, nullptr);
    ensure_box_meta(state, kFeatureMeta, item_index, item_newindex, nullptr);
}

void push_context(lua_State* state, const MapScriptContext* shown) {
    lua_createtable(state, 0, 3);
    const int ctx = lua_gettop(state);
    lua_createtable(state, 0, 3);
    const int mod_table = lua_gettop(state);
    lua_pushlstring(state, shown->mod_id.data(), shown->mod_id.size());
    lua_setfield(state, mod_table, "id");
    lua_pushlstring(state, shown->mod_version.data(), shown->mod_version.size());
    lua_setfield(state, mod_table, "version");
    lua_pushlstring(state, shown->mod_game.data(), shown->mod_game.size());
    lua_setfield(state, mod_table, "game");
    lua_setfield(state, ctx, "mod");
    lua_createtable(state, 0, 1);
    const int engine_table = lua_gettop(state);
    lua_pushlstring(state, shown->engine_version.data(), shown->engine_version.size());
    lua_setfield(state, engine_table, "version");
    lua_setfield(state, ctx, "engine");
    const std::size_t count = shown->options != nullptr ? shown->options->size() : 0;
    const int sized = count > 1000000 ? 1000000 : static_cast<int>(count);
    lua_createtable(state, 0, sized);
    const int options_table = lua_gettop(state);
    if (shown->options != nullptr) {
        for (const LoadoutOption& option : *shown->options) {
            if (option.type == oamut::OptionType::toggle)
                lua_pushboolean(state, option.toggle ? 1 : 0);
            else if (option.type == oamut::OptionType::number || option.value.number)
                lua_pushnumber(state, oamod::number_to_double(option.value.number_value));
            else
                lua_pushlstring(state, option.value.text.data(), option.value.text.size());
            lua_setfield(state, options_table, option.id.c_str());
        }
    }
    lua_setfield(state, ctx, "options");
}

bool run_table_form(lua_State* state, ApplyBox* apply) {
    MapWork* work = apply->work;
    lua_rawgeti(state, LUA_REGISTRYINDEX, apply->ref);
    const int list = lua_gettop(state);
    const lua_Unsigned count = lua_rawlen(state, list);
    for (lua_Unsigned slot = 1; slot <= count; ++slot) {
        lua_rawgeti(state, list, static_cast<lua_Integer>(slot));
        const int rule = lua_gettop(state);
        push_rule_needs(state, rule);
        const int needs = lua_gettop(state);
        bool holds = false;
        if (!lua_istable(state, needs) || !map_where_holds(state, work, needs, holds)) {
            lua_settop(state, list - 1);
            return false;
        }
        if (holds) {
            push_rule_changes(state, rule);
            std::vector<ChangeItem> items;
            std::string problem;
            const bool applied = lua_istable(state, -1) &&
                                 collect_changes(state, lua_gettop(state), items, problem) &&
                                 apply_change_list(work, items, -1);
            if (!applied) {
                if (work->error_len == 0)
                    set_error(
                        work, problem.empty() ? "the maps change could not be applied" : problem
                    );
                lua_settop(state, list - 1);
                return false;
            }
            lua_pop(state, 1);
        }
        lua_pop(state, 2);
    }
    lua_pop(state, 1);
    return true;
}

int apply_table_form(lua_State* state) {
    auto* apply = static_cast<ApplyBox*>(lua_touserdata(state, lua_upvalueindex(1)));
    if (apply == nullptr || apply->work == nullptr) {
        lua_pushliteral(state, "the map is not there");
        return lua_error(state);
    }
    if (!run_table_form(state, apply)) {
        lua_pushlstring(state, apply->work->error, apply->work->error_len);
        return lua_error(state);
    }
    return 0;
}

int push_maps_call(void* context, lua_State* state) {
    auto* box = static_cast<ApplyBox*>(context);
    install_view(state);
    if (box->function_entry) {
        lua_rawgeti(state, LUA_REGISTRYINDEX, box->ref);
        push_box(state, box->work, kBoxMap, kMapMeta, -1, -1);
        push_context(state, box->context);
        return 2;
    }
    lua_pushlightuserdata(state, box);
    lua_pushcclosure(state, apply_table_form, 1);
    return 0;
}

bool list_shape_ok(lua_State* state, int index, std::string& problem) {
    index = lua_absindex(state, index);
    if (lua_getmetatable(state, index) != 0) {
        lua_pop(state, 1);
        problem = "maps must be a plain table, not one with a metatable";
        return false;
    }
    const lua_Unsigned count = lua_rawlen(state, index);
    lua_pushnil(state);
    while (lua_next(state, index) != 0) {
        if (lua_type(state, -2) != LUA_TNUMBER) {
            problem = "maps must be a list";
            lua_pop(state, 2);
            return false;
        }
        const lua_Integer slot = lua_tointeger(state, -2);
        const lua_Number raw = lua_tonumber(state, -2);
        if (slot < 1 || static_cast<lua_Unsigned>(slot) > count ||
            static_cast<lua_Number>(slot) != raw) {
            problem = "maps must be a list";
            lua_pop(state, 2);
            return false;
        }
        if (!finished_where_rule(state, -1)) {
            problem = "maps must be a function or a list of where():change()";
            lua_pop(state, 2);
            return false;
        }
        push_rule_changes(state, lua_gettop(state));
        std::vector<ChangeItem> items;
        if (!lua_istable(state, -1) || !collect_changes(state, lua_gettop(state), items, problem)) {
            if (problem.empty())
                problem = "maps must be a function or a list of where():change()";
            lua_pop(state, 3);
            return false;
        }
        lua_pop(state, 2);
    }
    return true;
}

} // namespace

void install_map_match_forms(lua_State* state) {
    ensure_match_meta(state);
    lua_pushcfunction(state, form_glob);
    lua_setglobal(state, "glob");
    lua_pushcfunction(state, form_pattern);
    lua_setglobal(state, "pattern");
}

bool map_match_value(lua_State* state, int index) {
    return luaL_testudata(state, index, kMatchMeta) != nullptr;
}

std::string maps_list_problem(lua_State* state, int index) {
    std::string problem;
    if (!lua_istable(state, index))
        return "maps must be a function or a list of where():change()";
    if (!list_shape_ok(state, index, problem))
        return problem;
    return {};
}

MapsTurn apply_maps(
    host::Sandbox& sandbox,
    bool function_entry,
    int ref,
    const ChosenMap& chosen,
    const std::vector<uint8_t>& current,
    oa::AssetStore& store,
    const defs::DataLayout& layout,
    const MapScriptContext& context,
    uint16_t mutator,
    const std::vector<JournalEntry>& earlier
) {
    MapsTurn turn;
    MapWork work;
    work.store = &store;
    work.layout = &layout;
    work.chosen = &chosen;
    work.earlier = &earlier;
    work.mutator = mutator;
    const std::string text(current.begin(), current.end());
    tdf::ParseError error{};
    if (current.empty() || !work.document.parse(text, &error)) {
        turn.ok = false;
        turn.message = "the chosen map could not be read";
        return turn;
    }
    ApplyBox box;
    box.work = &work;
    box.context = &context;
    box.ref = ref;
    box.function_entry = function_entry;
    host::CallHooks hooks;
    hooks.context = &box;
    hooks.push = push_maps_call;
    host::ScriptError script_error;
    bool called = false;
    try {
        called = sandbox.call("maps", hooks, script_error);
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& ex) {
        turn.ok = false;
        turn.message = ex.what() != nullptr ? ex.what() : "the maps hook stopped";
        return turn;
    }
    if (!called) {
        turn.ok = false;
        turn.from_script = true;
        turn.kind = script_error.kind;
        turn.message = script_error.message;
        return turn;
    }
    if (!tdf::document_edited(work.document.get()))
        return turn;
    const std::optional<std::string> written = tdf::write_text(work.document.get());
    if (!written.has_value()) {
        turn.ok = false;
        turn.message = "the chosen map could not be written";
        return turn;
    }
    auto bytes = std::make_shared<const std::vector<uint8_t>>(written->begin(), written->end());
    if (*bytes == current)
        return turn;
    turn.changed = true;
    turn.bytes = std::move(bytes);
    turn.entries = std::move(work.entries);
    turn.conflicts = std::move(work.conflicts);
    return turn;
}

} // namespace oa::data::mutators
