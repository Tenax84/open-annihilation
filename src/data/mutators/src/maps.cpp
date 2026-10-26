// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Rewrites the chosen map's OTA for one match. The view the script sees is
// built for that call only. A write that the text writer turns back into the
// same bytes is not served. Start positions stay in the file and are never
// part of the view.
//
// Lua is built as C, so a raise skips every C++ destructor between it and
// the protected call. Each callback charges its access before any C++ object
// exists, does its C++ work inside a try block on storage the turn owns, and
// charges, pushes or raises only once that work has returned. A refusal waits
// in the turn's fixed error buffer until then, and no exception leaves a
// callback.
#include "oa/data/mutators/maps.hpp"

#include "incompatible.hpp"
#include "oa/data/mutators/collections.hpp"
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
#include <map>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
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
constexpr std::size_t kGlobLimit = 256;
constexpr std::size_t kFeatureNameLimit = 127;
constexpr std::size_t kErrorLimit = 640;
/// Longest key the view compares. A longer key names nothing.
constexpr std::size_t kKeyLimit = 256;
/// One field read or write: the lookup and the field check around it, as
/// the unit collections charge a record access.
constexpr uint64_t kFieldCharge = 8;
/// Most entries one `where`, `:set` or `:scale` table may hold.
constexpr std::size_t kEntryLimit = sm::max_statements;

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

/// One view handle. The work outlives every handle of its turn.
struct Box {
    void* work{};
    uint8_t kind{kBoxMap};
    int32_t schema{-1};
    int32_t item{-1};
};

static_assert(std::is_trivially_destructible_v<Box>);

enum class ValueKind : uint8_t { number, text, scale };

/// One value a script wrote. Text points into a Lua string that the
/// callback's arguments, or the table being read, keep alive.
struct ValueAction {
    ValueKind kind{ValueKind::number};
    double number{0};
    double low{0};
    double high{0};
    bool clamp{false};
    const char* text{};
    std::size_t text_length{0};
};

static_assert(std::is_trivially_destructible_v<ValueAction>);

/// One key of a change table, with its value. The key points into the
/// table's own key string.
struct ChangeItem {
    const char* raw{};
    std::size_t raw_length{0};
    const KeyRow* row{};
    ValueAction action;
};

static_assert(std::is_trivially_copyable_v<ChangeItem>);
static_assert(std::is_trivially_destructible_v<ChangeItem>);

/// Everything one maps turn owns. A callback computes into these members,
/// so nothing it built is a local when it charges, pushes or raises.
struct MapWork {
    oa::AssetStore* store{};
    /// The documents as the mutators above left them. Null only while a
    /// plan's list is checked, when no placed name is looked up.
    Collections* collections{};
    const defs::DataLayout* layout{};
    const ChosenMap* chosen{};
    tdf::OwnedDocument document;
    uint16_t mutator{0};
    /// The error a callback raises once its C++ work has returned.
    char error[kErrorLimit]{};
    std::size_t error_len{0};
    /// Units the C++ work owes the allowance, charged at the next settle.
    uint64_t bill{0};
    /// Set once a write passed `mutator_write_limit`. The turn then fails,
    /// even when the script caught the error.
    bool over_limit{false};
    /// Stack index of the change table the table form is reading.
    int table{0};
    std::vector<JournalEntry> entries;
    std::vector<ChangeItem> items;
    std::vector<sm::Need> needs;
    sm::Need staging;
    std::string reason;
    std::vector<int32_t> numbers;
    /// A placed name that waits for its lookup. It points into the value
    /// the callback was given.
    const char* lookup_name{};
    std::size_t lookup_length{0};
    bool lookup_feature{false};
};

struct ApplyBox {
    MapWork* work{};
    const MapScriptContext* context{};
    DeclaredStop* stop{};
    int ref{LUA_NOREF};
    bool function_entry{false};
};

/// What a read pushes once its C++ work has returned.
enum class EmitKind : uint8_t { nil, integer, number, text, box, function };

struct Emit {
    EmitKind kind{EmitKind::nil};
    lua_Integer integer{0};
    double number{0};
    const char* text{};
    std::size_t length{0};
    uint8_t box_kind{kBoxMap};
    const char* meta{};
    int32_t schema{-1};
    int32_t item{-1};
    lua_CFunction function{};
};

static_assert(std::is_trivially_destructible_v<Emit>);

enum class Tri : uint8_t { no, yes, stop };

extern "C" {
static int form_glob(lua_State* state);
static int form_pattern(lua_State* state);
static int map_index(lua_State* state);
static int map_newindex(lua_State* state);
static int header_index(lua_State* state);
static int header_newindex(lua_State* state);
static int schemas_index(lua_State* state);
static int schemas_newindex(lua_State* state);
static int schemas_len(lua_State* state);
static int schemas_each(lua_State* state);
static int schemas_where(lua_State* state);
static int schemas_set(lua_State* state);
static int schemas_scale(lua_State* state);
static int schema_index(lua_State* state);
static int schema_newindex(lua_State* state);
static int placed_index(lua_State* state);
static int placed_newindex(lua_State* state);
static int placed_len(lua_State* state);
static int placed_remove(lua_State* state);
static int item_index(lua_State* state);
static int item_newindex(lua_State* state);
static int apply_table_form(lua_State* state);
}

// Text. Nothing here allocates.

char fold_byte(char piece) noexcept {
    if (piece >= 'A' && piece <= 'Z')
        return static_cast<char>(piece - 'A' + 'a');
    return piece;
}

bool same_text(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size())
        return false;
    for (std::size_t offset = 0; offset < left.size(); ++offset) {
        if (fold_byte(left[offset]) != fold_byte(right[offset]))
            return false;
    }
    return true;
}

bool starts_with_nocase(std::string_view text, std::string_view prefix) noexcept {
    if (text.size() < prefix.size())
        return false;
    return same_text(text.substr(0, prefix.size()), prefix);
}

bool contains_nocase(std::string_view hay, std::string_view needle) noexcept {
    if (needle.empty())
        return true;
    if (needle.size() > hay.size())
        return false;
    for (std::size_t start = 0; start + needle.size() <= hay.size(); ++start) {
        if (same_text(hay.substr(start, needle.size()), needle))
            return true;
    }
    return false;
}

std::string_view trim_ends(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.remove_suffix(1);
    return text;
}

bool has_nul(std::string_view text) noexcept {
    return text.find('\0') != std::string_view::npos;
}

/// The row of a key, ignoring ASCII case, or null.
const KeyRow* key_row(std::string_view key) noexcept {
    if (key.size() >= kKeyLimit || has_nul(key))
        return nullptr;
    char folded[kKeyLimit];
    for (std::size_t offset = 0; offset < key.size(); ++offset)
        folded[offset] = fold_byte(key[offset]);
    const std::string_view lowered(folded, key.size());
    for (const KeyRow& row : k_keys) {
        if (lowered == row.folded)
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

/// Reads NUL-terminated text that is a number and nothing else.
bool fully_numeric(const char* text, double& number) noexcept {
    if (text == nullptr || text[0] == '\0')
        return false;
    const char* cursor = text;
    if (*cursor == '+' || *cursor == '-')
        ++cursor;
    bool digit = false;
    bool dot = false;
    for (; *cursor != '\0'; ++cursor) {
        if (*cursor >= '0' && *cursor <= '9') {
            digit = true;
            continue;
        }
        if (*cursor == '.' && !dot) {
            dot = true;
            continue;
        }
        return false;
    }
    if (!digit)
        return false;
    number = tdf::parse_double(text);
    return std::isfinite(number);
}

bool whole_number(double value, double& integral) noexcept {
    if (!std::isfinite(value))
        return false;
    return std::modf(value, &integral) == 0.0;
}

bool glob_match(std::string_view pattern, std::string_view text) noexcept {
    std::size_t pattern_at = 0;
    std::size_t text_at = 0;
    std::size_t star = std::string_view::npos;
    std::size_t mark = 0;
    while (text_at < text.size()) {
        if (pattern_at < pattern.size() &&
            (pattern[pattern_at] == '?' ||
             fold_byte(pattern[pattern_at]) == fold_byte(text[text_at]))) {
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

/// Writes a whole number in decimal, NUL-terminated.
std::size_t spell_whole(int64_t value, char (&out)[24]) noexcept {
    char digits[24];
    std::size_t count = 0;
    const bool negative = value < 0;
    uint64_t rest = negative ? 0 - static_cast<uint64_t>(value) : static_cast<uint64_t>(value);
    do {
        digits[count++] = static_cast<char>('0' + rest % 10);
        rest /= 10;
    } while (rest != 0 && count < sizeof digits);
    std::size_t at = 0;
    if (negative)
        out[at++] = '-';
    while (count > 0 && at + 1 < sizeof out)
        out[at++] = digits[--count];
    out[at] = '\0';
    return at;
}

// Charges. A charge can raise, so it runs only when every C++ object of the
// frames above it is gone.

uint64_t add_units(uint64_t left, uint64_t right) noexcept {
    return right > UINT64_MAX - left ? UINT64_MAX : left + right;
}

/// Units for work over `left` times `right` bytes, at the string rate.
uint64_t product_units(uint64_t left, uint64_t right) noexcept {
    if (left != 0 && right > UINT64_MAX / left)
        return UINT64_MAX / host::bytes_per_charge;
    const uint64_t bytes = left * right;
    return bytes / host::bytes_per_charge + (bytes % host::bytes_per_charge != 0 ? 1 : 0);
}

void add_bill(MapWork& work, uint64_t units) noexcept {
    work.bill = add_units(work.bill, units);
}

/// Charges what the C++ work billed. May raise; no C++ object may be alive.
void settle(lua_State* state, MapWork& work) {
    const uint64_t units = work.bill;
    work.bill = 0;
    if (units != 0)
        host::charge_work(state, units);
}

/// Bytes of a string, or of a `set "text"` value, at `index`. Does not raise.
std::size_t value_bytes(lua_State* state, int index) noexcept {
    index = lua_absindex(state, index);
    std::size_t length = 0;
    if (lua_type(state, index) == LUA_TSTRING) {
        lua_tolstring(state, index, &length);
        return length;
    }
    sm::ChangeKind change = sm::ChangeKind::assign;
    if (!rule_change_at(state, index, &change, nullptr, nullptr, nullptr) ||
        change != sm::ChangeKind::assign_text)
        return 0;
    if (lua_getiuservalue(state, index, 1) == LUA_TSTRING)
        length = static_cast<std::size_t>(lua_rawlen(state, -1));
    lua_pop(state, 1);
    return length;
}

/// Charges one field read: the lookup and the key's bytes. May raise.
void charge_access(lua_State* state) {
    std::size_t key = 0;
    if (lua_type(state, 2) == LUA_TSTRING)
        lua_tolstring(state, 2, &key);
    host::charge_work(state, kFieldCharge + host::units_for_bytes(key));
}

/// Charges one field write: the lookup, the key's bytes and the value's. May raise.
void charge_write(lua_State* state) {
    std::size_t key = 0;
    if (lua_type(state, 2) == LUA_TSTRING)
        lua_tolstring(state, 2, &key);
    const std::size_t value = value_bytes(state, 3);
    host::charge_work(
        state, kFieldCharge + host::units_for_bytes(key) + host::units_for_bytes(value)
    );
}

// The held error. It lives in a fixed buffer, so holding one never allocates.

void hold_clear(MapWork& work) noexcept {
    work.error_len = 0;
    work.error[0] = '\0';
}

void hold_add(MapWork& work, std::string_view text, bool fold = false) noexcept {
    const std::size_t room = kErrorLimit - 1 - work.error_len;
    const std::size_t count = std::min(room, text.size());
    for (std::size_t offset = 0; offset < count; ++offset)
        work.error[work.error_len + offset] = fold ? fold_byte(text[offset]) : text[offset];
    work.error_len += count;
    work.error[work.error_len] = '\0';
}

void hold(MapWork& work, std::string_view text) noexcept {
    hold_clear(work);
    hold_add(work, text);
}

/// Raises the held error. Call it with no C++ object alive, and never from
/// inside a catch block.
int raise_held(lua_State* state, MapWork& work) {
    if (work.error_len == 0)
        hold(work, "the maps hook stopped");
    lua_pushlstring(state, work.error, work.error_len);
    hold_clear(work);
    return lua_error(state);
}

/// One step of a callback's C++ work. It calls no Lua function that can
/// raise and never charges: it bills `work.bill` and holds its refusal.
using Step = bool (*)(lua_State* state, MapWork& work, Emit& emit);

/// Holds an exception's text, after the key the callback was given.
void hold_exception(lua_State* state, MapWork& work, const char* what) noexcept {
    hold_clear(work);
    if (lua_type(state, 2) == LUA_TSTRING) {
        std::size_t length = 0;
        const char* key = lua_tolstring(state, 2, &length);
        hold_add(work, std::string_view(key, length), true);
        hold_add(work, ": ");
    }
    hold_add(work, what != nullptr && what[0] != '\0' ? what : "the maps hook stopped");
}

/// Runs a step, turning any exception into a held error that names the key.
bool guarded(lua_State* state, MapWork& work, Step step, Emit& emit) noexcept {
    try {
        return step(state, work, emit);
    } catch (const std::bad_alloc&) {
        hold_exception(state, work, "out of memory");
    } catch (const std::exception& ex) {
        hold_exception(state, work, ex.what());
    } catch (...) {
        hold_exception(state, work, nullptr);
    }
    return false;
}

// Boxes and metatables.

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

const Box* box_of(lua_State* state, int index, const char* meta) noexcept {
    return static_cast<const Box*>(luaL_testudata(state, index, meta));
}

MapWork* work_in(const Box* box) noexcept {
    return box != nullptr ? static_cast<MapWork*>(box->work) : nullptr;
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

int push_emit(lua_State* state, MapWork& work, const Emit& emit) {
    switch (emit.kind) {
    case EmitKind::integer:
        lua_pushinteger(state, emit.integer);
        break;
    case EmitKind::number:
        lua_pushnumber(state, emit.number);
        break;
    case EmitKind::text:
        lua_pushlstring(state, emit.text, emit.length);
        break;
    case EmitKind::box:
        push_box(state, &work, emit.box_kind, emit.meta, emit.schema, emit.item);
        break;
    case EmitKind::function:
        lua_pushcfunction(state, emit.function);
        break;
    case EmitKind::nil:
    default:
        lua_pushnil(state);
        break;
    }
    return 1;
}

Emit box_emit(uint8_t kind, const char* meta, int32_t schema, int32_t item) noexcept {
    Emit emit;
    emit.kind = EmitKind::box;
    emit.box_kind = kind;
    emit.meta = meta;
    emit.schema = schema;
    emit.item = item;
    return emit;
}

Emit function_emit(lua_CFunction function) noexcept {
    Emit emit;
    emit.kind = EmitKind::function;
    emit.function = function;
    return emit;
}

Emit integer_emit(lua_Integer value) noexcept {
    Emit emit;
    emit.kind = EmitKind::integer;
    emit.integer = value;
    return emit;
}

// The document. Lookups walk the parsed tree and allocate nothing.

tdf::Block* header_block(MapWork& work) noexcept {
    return tdf::editable_child(tdf::editable_root(work.document.get()), "GlobalHeader");
}

/// "Schema N", the spelling the session loader looks up. `number` is not negative.
void schema_name(int number, char (&out)[32]) noexcept {
    char digits[24];
    const std::size_t digits_len = spell_whole(number, digits);
    out[0] = 'S';
    out[1] = 'c';
    out[2] = 'h';
    out[3] = 'e';
    out[4] = 'm';
    out[5] = 'a';
    out[6] = ' ';
    std::memcpy(out + 7, digits, digits_len + 1);
}

tdf::Block* schema_block(MapWork& work, int number) noexcept {
    // Schemas are children of GlobalHeader. The session loader looks up
    // "Schema N" from that block, and stops at the first missing number.
    if (number < 0)
        return nullptr;
    tdf::Block* header = header_block(work);
    if (header == nullptr)
        return nullptr;
    char name[32];
    schema_name(number, name);
    return tdf::editable_child(header, name);
}

int schema_count(MapWork& work) noexcept {
    int count = 0;
    for (int number = 0; number < kSchemaLimit; ++number) {
        if (schema_block(work, number) == nullptr)
            break;
        ++count;
    }
    return count;
}

int start_positions(const tdf::Block* schema) noexcept {
    const tdf::Block* specials = tdf::find_child(schema, "specials");
    int count = 0;
    for (uint32_t child = 0; child < tdf::child_count(specials); ++child) {
        const char* what = tdf::find_value(tdf::child_at(specials, child), "specialwhat");
        if (what != nullptr && starts_with_nocase(what, "StartPos"))
            ++count;
    }
    return count;
}

/// The scenario's `missionname`, or the map key when that is absent.
std::string_view mission_name(MapWork& work) noexcept {
    const char* written = tdf::find_value(header_block(work), "missionname");
    if (written != nullptr && written[0] != '\0')
        return written;
    return work.chosen->key;
}

tdf::Block* placed_parent(MapWork& work, int schema, bool features) noexcept {
    tdf::Block* node = schema_block(work, schema);
    if (node == nullptr)
        return nullptr;
    if (features)
        return tdf::editable_child(node, "features");
    return tdf::editable_child(node, std::string_view(work.layout->map_units_section));
}

tdf::Block* placed_child(MapWork& work, int schema, bool features, int item) noexcept {
    tdf::Block* parent = placed_parent(work, schema, features);
    if (parent == nullptr || item < 0)
        return nullptr;
    return tdf::editable_child(parent, static_cast<uint32_t>(item));
}

/// How many schemas the set at `index` holds. Calls nothing that raises.
lua_Integer set_count(lua_State* state, MapWork& work, const Box& box, int index) noexcept {
    if (box.kind != kBoxFiltered)
        return schema_count(work);
    if (lua_getiuservalue(state, index, 1) != LUA_TTABLE) {
        lua_pop(state, 1);
        return 0;
    }
    const auto count = static_cast<lua_Integer>(lua_rawlen(state, -1));
    lua_pop(state, 1);
    return count;
}

/// The schema number at `slot`, from 1, of the set at `index`.
int set_number(lua_State* state, const Box& box, int index, lua_Integer slot) noexcept {
    if (box.kind != kBoxFiltered)
        return static_cast<int>(slot - 1);
    if (lua_getiuservalue(state, index, 1) != LUA_TTABLE) {
        lua_pop(state, 1);
        return -1;
    }
    lua_rawgeti(state, -1, slot);
    const int number = static_cast<int>(lua_tointeger(state, -1));
    lua_pop(state, 2);
    return number;
}

/// What a stored value reads as: a number when it is one, else its text.
Emit stored_emit(MapWork& work, const char* text) noexcept {
    Emit emit;
    if (text == nullptr)
        return emit;
    const std::size_t length = std::strlen(text);
    add_bill(work, host::units_for_bytes(length));
    double number = 0;
    if (fully_numeric(text, number)) {
        emit.kind = EmitKind::number;
        emit.number = number;
        return emit;
    }
    emit.kind = EmitKind::text;
    emit.text = text;
    emit.length = length;
    return emit;
}

// Refusals.

const char* header_limit_tail(KeyClass kind) noexcept {
    if (kind == KeyClass::header_bit)
        return " must be 0 or 1";
    if (kind == KeyClass::header_real)
        return " must be a number from 0 to 1000";
    return " must be a whole number from 0 to 32767";
}

constexpr const char* k_schema_metal_limit =
    "map.schemas.surfacemetal must be a whole number from 0 to 255";

void hold_header_limit(MapWork& work, const KeyRow& row) noexcept {
    hold(work, "map.header.");
    hold_add(work, row.folded);
    hold_add(work, header_limit_tail(row.kind));
}

void hold_type_problem(MapWork& work, const KeyRow& row) noexcept {
    if (row.kind == KeyClass::schema_profile) {
        hold(work, "map.schemas.aiprofile must be text");
    } else if (row.kind == KeyClass::schema_resource) {
        hold(work, "map.schemas.");
        hold_add(work, row.folded);
        hold_add(work, " must be a whole number from 0 to 1000000");
    } else if (row.kind == KeyClass::schema_metal) {
        hold(work, k_schema_metal_limit);
    } else {
        hold_header_limit(work, row);
    }
}

void hold_not_setting(MapWork& work, std::string_view key, bool fold) noexcept {
    hold(work, "the maps change \"");
    hold_add(work, key, fold);
    hold_add(work, "\" is not a header or schema setting");
}

bool number_in_header(const KeyRow& row, double value, double& accepted) noexcept {
    if (row.kind == KeyClass::header_real) {
        if (!std::isfinite(value) || value < 0.0 || value > 1000.0)
            return false;
        accepted = value;
        return true;
    }
    double integral = 0;
    if (!whole_number(value, integral))
        return false;
    if (row.kind == KeyClass::header_bit) {
        if (integral != 0.0 && integral != 1.0)
            return false;
        accepted = integral;
        return true;
    }
    if (integral < 0.0 || integral > 32767.0)
        return false;
    accepted = integral;
    return true;
}

/// Reads the value at `index`. Calls nothing that raises.
bool read_action(
    lua_State* state, int index, const KeyRow* row, ValueAction& action, MapWork& work
) noexcept {
    index = lua_absindex(state, index);
    sm::ChangeKind change = sm::ChangeKind::assign;
    double value = 0;
    double low = 0;
    double high = 0;
    if (rule_change_at(state, index, &change, &value, &low, &high)) {
        if (change == sm::ChangeKind::scale || change == sm::ChangeKind::scale_clamp) {
            if (row != nullptr && row->kind == KeyClass::schema_profile) {
                hold(work, "map.schemas.aiprofile cannot be scaled");
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
            // The text stays referenced by the value's own slot.
            if (lua_getiuservalue(state, index, 1) == LUA_TSTRING)
                action.text = lua_tolstring(state, -1, &action.text_length);
            lua_pop(state, 1);
            if (action.text == nullptr) {
                action.text = "";
                action.text_length = 0;
            }
            action.kind = ValueKind::text;
            return true;
        }
        hold(work, "the maps change must be a value or a scale");
        return false;
    }
    const int type = lua_type(state, index);
    if (type == LUA_TBOOLEAN) {
        action.kind = ValueKind::number;
        action.number = lua_toboolean(state, index) != 0 ? 1.0 : 0.0;
        return true;
    }
    if (type == LUA_TNUMBER) {
        action.kind = ValueKind::number;
        action.number = lua_tonumber(state, index);
        if (!std::isfinite(action.number)) {
            hold(work, "a maps value must be finite");
            return false;
        }
        return true;
    }
    if (type == LUA_TSTRING) {
        action.kind = ValueKind::text;
        action.text = lua_tolstring(state, index, &action.text_length);
        return true;
    }
    hold(work, "the maps change must be a value or a scale");
    return false;
}

bool action_fits(
    MapWork& work, const KeyRow& row, const ValueAction& action, bool scaling
) noexcept {
    if (row.kind == KeyClass::header_frozen) {
        hold(work, "map.header.");
        hold_add(work, row.folded);
        hold_add(work, " cannot change");
        return false;
    }
    if (row.kind == KeyClass::schema_frozen) {
        hold(work, "schema.");
        hold_add(work, row.folded);
        hold_add(work, " cannot change");
        return false;
    }
    if (row.kind == KeyClass::schema_profile) {
        if (scaling || action.kind == ValueKind::scale) {
            hold(work, "map.schemas.aiprofile cannot be scaled");
            return false;
        }
        if (action.kind != ValueKind::text) {
            hold_type_problem(work, row);
            return false;
        }
        return true;
    }
    if (action.kind == ValueKind::text) {
        hold_type_problem(work, row);
        return false;
    }
    if (action.kind == ValueKind::number) {
        double accepted = 0;
        double integral = 0;
        bool ok = false;
        if (header_class(row.kind))
            ok = number_in_header(row, action.number, accepted);
        else if (row.kind == KeyClass::schema_metal)
            ok = whole_number(action.number, integral) && integral >= 0.0 && integral <= 255.0;
        else
            ok = whole_number(action.number, integral) && integral >= 0.0 && integral <= 1000000.0;
        if (!ok) {
            hold_type_problem(work, row);
            return false;
        }
    }
    return true;
}

bool profile_text(MapWork& work, std::string_view text) noexcept {
    bool ok = !text.empty() && text.size() <= 31;
    for (std::size_t offset = 0; ok && offset < text.size(); ++offset) {
        const char piece = text[offset];
        const bool letter = (piece >= 'A' && piece <= 'Z') || (piece >= 'a' && piece <= 'z');
        const bool digit = piece >= '0' && piece <= '9';
        ok = letter || digit || piece == '_' || piece == '-';
    }
    if (!ok)
        hold(work, "aiprofile must be 1 to 31 letters, digits, _ or -");
    return ok;
}

/// True when `ai/<name>.txt` is in the layered store. Bills one lookup.
bool profile_present(MapWork& work, std::string_view name) {
    const std::size_t ai = static_cast<std::size_t>(defs::DataDirectory::ai);
    std::string path = work.layout->directories[ai];
    path.push_back('/');
    path.append(name.data(), name.size());
    path.append(".txt");
    add_bill(work, kFieldCharge + host::units_for_bytes(path.size()));
    oa::ResourceFile* file = work.store->open(path);
    if (file == nullptr) {
        hold(work, path);
        hold_add(work, " is not there");
        return false;
    }
    oa::AssetStore::close(file);
    return true;
}

bool profile_ok(MapWork& work, const ValueAction& action) {
    const std::string_view name(action.text, action.text_length);
    return profile_text(work, name) && profile_present(work, name);
}

/// Checks each profile in `work.items` once, before any schema is written.
bool check_profiles(MapWork& work) {
    for (std::size_t slot = 0; slot < work.items.size(); ++slot) {
        const ChangeItem item = work.items[slot];
        if (item.row->kind == KeyClass::schema_profile && !profile_ok(work, item.action))
            return false;
    }
    return true;
}

// Writes. Each one is journalled. The count is the mutator's tally on the
// collections, shared with the units, weapons and other hooks.

std::string map_record(const ChosenMap& chosen) {
    return "map:" + chosen.key;
}

std::string schema_record(const ChosenMap& chosen, int number) {
    return map_record(chosen) + "/Schema " + std::to_string(number);
}

std::string fold_copy(std::string_view text) {
    std::string folded(text);
    for (char& piece : folded)
        piece = fold_byte(piece);
    return folded;
}

/// Grows the journal geometrically before an edit, so the push after the
/// edit cannot fail.
void make_room(std::vector<JournalEntry>& entries) {
    if (entries.size() < entries.capacity())
        return;
    entries.reserve(std::max<std::size_t>(16, entries.capacity() * 2));
}

bool room_for_write(MapWork& work) noexcept {
    if (!work.over_limit && work.collections != nullptr && work.collections->allow_field_write())
        return true;
    work.over_limit = true;
    hold(work, "this mutator has changed too many fields");
    return false;
}

void count_write(MapWork& work) noexcept {
    if (work.collections != nullptr)
        work.collections->count_field_write();
}

/// Sets one key and journals it. The row is built before the edit, so an
/// allocation that fails leaves the document as it was.
bool store_text(
    MapWork& work,
    tdf::Block* block,
    const char* spelling,
    std::string_view written,
    int schema,
    int item
) {
    const char* existing = tdf::find_value(block, spelling);
    if (existing != nullptr && written == existing)
        return true;
    if (!room_for_write(work))
        return false;
    JournalEntry entry;
    entry.mutators.push_back(work.mutator);
    entry.phase = Phase::hooks;
    entry.collection = Collection::other;
    entry.change = Change::set;
    entry.record = schema < 0 ? map_record(*work.chosen) : schema_record(*work.chosen, schema);
    if (item >= 0)
        entry.block = std::to_string(item);
    entry.field = fold_copy(spelling);
    entry.path = work.chosen->ota_path;
    if (existing != nullptr)
        entry.old_text = std::string(existing);
    entry.new_text = std::string(written);
    make_room(work.entries);
    const tdf::EditStatus status = tdf::set_value(work.document.get(), block, spelling, written);
    if (status != tdf::EditStatus::ok) {
        hold(work, "the chosen map could not be written");
        return false;
    }
    work.entries.push_back(std::move(entry));
    count_write(work);
    return true;
}

bool apply_on_block(
    MapWork& work, tdf::Block* block, const KeyRow& row, const ValueAction& action, int schema
) {
    if (block == nullptr) {
        hold(work, header_class(row.kind) ? "the map has no header" : "the map has no schema");
        return false;
    }
    if (!action_fits(work, row, action, false))
        return false;
    if (action.kind == ValueKind::scale) {
        const char* existing = tdf::find_value(block, row.spelling);
        double current = 0;
        if (existing == nullptr || !fully_numeric(existing, current)) {
            hold(work, header_class(row.kind) ? "map.header." : "map.schemas.");
            hold_add(work, row.folded);
            hold_add(work, " has no number");
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
        return apply_on_block(work, block, row, assigned, schema);
    }
    if (row.kind == KeyClass::schema_profile) {
        const std::string_view name(action.text, action.text_length);
        if (!profile_text(work, name))
            return false;
        return store_text(work, block, row.spelling, name, schema, -1);
    }
    double accepted = action.number;
    if (header_class(row.kind) && !number_in_header(row, action.number, accepted)) {
        hold_header_limit(work, row);
        return false;
    }
    const std::optional<std::string> spelled = tdf::format_number(accepted);
    if (!spelled.has_value()) {
        hold(work, "the chosen map could not be written");
        return false;
    }
    return store_text(work, block, row.spelling, *spelled, schema, -1);
}

bool apply_header(MapWork& work, const KeyRow& row, const ValueAction& action) {
    return apply_on_block(work, header_block(work), row, action, -1);
}

bool apply_schema(MapWork& work, int number, const KeyRow& row, const ValueAction& action) {
    return apply_on_block(work, schema_block(work, number), row, action, number);
}

/// Applies `work.items`: header keys to the header, schema keys to one
/// schema, or to every schema when `only_schema` is negative. Each block
/// written is billed as one field write plus the value's bytes.
bool apply_items(MapWork& work, int only_schema) {
    for (std::size_t slot = 0; slot < work.items.size(); ++slot) {
        const ChangeItem item = work.items[slot];
        const uint64_t write_units = kFieldCharge + host::units_for_bytes(item.action.text_length);
        if (header_class(item.row->kind)) {
            add_bill(work, write_units);
            if (!apply_header(work, *item.row, item.action))
                return false;
            continue;
        }
        if (only_schema >= 0) {
            add_bill(work, write_units);
            if (!apply_schema(work, only_schema, *item.row, item.action))
                return false;
            continue;
        }
        const int count = schema_count(work);
        for (int number = 0; number < count; ++number) {
            add_bill(work, write_units);
            if (!apply_schema(work, number, *item.row, item.action))
                return false;
        }
    }
    return true;
}

/// Reads a change table into `work.items`, sorted by key. Calls nothing
/// that raises, so a plan's list can be read with it outside a phase.
bool collect_changes(lua_State* state, MapWork& work, int table, bool scaling) {
    work.items.clear();
    table = lua_absindex(state, table);
    lua_pushnil(state);
    while (lua_next(state, table) != 0) {
        if (lua_type(state, -2) != LUA_TSTRING) {
            lua_pop(state, 2);
            hold(work, "a maps change key must be text");
            return false;
        }
        if (work.items.size() >= kEntryLimit) {
            lua_pop(state, 2);
            hold(work, "too many statements");
            return false;
        }
        std::size_t length = 0;
        const char* raw = lua_tolstring(state, -2, &length);
        add_bill(
            work,
            host::element_charge + host::units_for_bytes(length) +
                host::units_for_bytes(value_bytes(state, -1))
        );
        const std::string_view key = trim_ends(std::string_view(raw, length));
        ChangeItem item;
        item.raw = key.data();
        item.raw_length = key.size();
        item.row = key_row(key);
        if (item.row == nullptr) {
            lua_pop(state, 2);
            hold_not_setting(work, key, false);
            return false;
        }
        if (!read_action(state, -1, item.row, item.action, work) ||
            !action_fits(work, *item.row, item.action, scaling)) {
            lua_pop(state, 2);
            return false;
        }
        work.items.push_back(item);
        lua_pop(state, 1);
    }
    std::sort(
        work.items.begin(), work.items.end(), [](const ChangeItem& left, const ChangeItem& right) {
            return host::name_less(
                std::string_view(left.raw, left.raw_length),
                std::string_view(right.raw, right.raw_length)
            );
        }
    );
    return true;
}

bool write_placed(
    MapWork& work,
    int schema,
    bool features,
    int item,
    const char* spelling,
    std::string_view written
) {
    tdf::Block* child = placed_child(work, schema, features, item);
    if (child == nullptr) {
        hold(work, features ? "the map has no such feature" : "the map has no such unit");
        return false;
    }
    return store_text(work, child, spelling, written, schema, item);
}

bool remove_placed(MapWork& work, int schema, bool features, int item) {
    tdf::Block* parent = placed_parent(work, schema, features);
    tdf::Block* child = placed_child(work, schema, features, item);
    if (parent == nullptr || child == nullptr) {
        hold(work, features ? "the map has no such feature" : "the map has no such unit");
        return false;
    }
    if (!room_for_write(work))
        return false;
    const char* field = features ? "featurename" : "unitname";
    const char* existing = tdf::find_value(child, field);
    JournalEntry entry;
    entry.mutators.push_back(work.mutator);
    entry.phase = Phase::hooks;
    entry.collection = Collection::other;
    entry.change = Change::removed;
    entry.record = schema_record(*work.chosen, schema);
    entry.block = std::to_string(item);
    entry.field = field;
    entry.path = work.chosen->ota_path;
    if (existing != nullptr)
        entry.old_text = std::string(existing);
    make_room(work.entries);
    const tdf::EditStatus status =
        tdf::remove_child(work.document.get(), parent, static_cast<uint32_t>(item));
    if (status != tdf::EditStatus::ok) {
        hold(work, "the chosen map could not be written");
        return false;
    }
    work.entries.push_back(std::move(entry));
    count_write(work);
    return true;
}

/// The stored spelling of a placed entry's writable field, or null.
const char* placed_spelling(std::string_view key, bool features) noexcept {
    if (features)
        return same_text(key, "featurename") ? "Featurename" : nullptr;
    if (same_text(key, "unitname"))
        return "UnitName";
    if (same_text(key, "xpos"))
        return "XPos";
    if (same_text(key, "zpos"))
        return "ZPos";
    if (same_text(key, "player"))
        return "Player";
    return nullptr;
}

// Matching. `where` reads the header, the map's names and the schemas.

/// The text a map `where` field reads. `key` holds a header key's copy.
void where_text(
    MapWork& work,
    std::string_view field,
    char (&key)[kKeyLimit],
    std::string_view& text,
    bool& present
) noexcept {
    present = false;
    text = {};
    if (same_text(field, "name")) {
        text = mission_name(work);
        present = true;
        return;
    }
    if (same_text(field, "file")) {
        text = work.chosen->key;
        present = true;
        return;
    }
    if (same_text(field, "pack")) {
        text = work.chosen->pack;
        present = true;
        return;
    }
    if (field.size() >= kKeyLimit || has_nul(field))
        return;
    std::memcpy(key, field.data(), field.size());
    key[field.size()] = '\0';
    const char* written = tdf::find_value(header_block(work), key);
    if (written == nullptr)
        return;
    text = written;
    present = true;
}

/// The text a schema `where` field reads. `digits` holds a number's spelling.
void schema_field_text(
    MapWork& work,
    int number,
    const std::string& field,
    char (&digits)[24],
    std::string_view& text,
    bool& present
) noexcept {
    present = false;
    text = {};
    tdf::Block* schema = schema_block(work, number);
    if (schema == nullptr || has_nul(field))
        return;
    if (same_text(field, "index")) {
        text = std::string_view(digits, spell_whole(number, digits));
        present = true;
        return;
    }
    if (same_text(field, "players")) {
        text = std::string_view(digits, spell_whole(start_positions(schema), digits));
        present = true;
        return;
    }
    const char* written = tdf::find_value(schema, field.c_str());
    if (written == nullptr)
        return;
    text = written;
    present = true;
}

/// Units for comparing `text` with one need: a `has` may try every place.
uint64_t compare_units(const sm::Need& need, std::string_view text) noexcept {
    const uint64_t read = host::units_for_bytes(text.size());
    if (need.kind != sm::NeedKind::contains)
        return read;
    return add_units(read, product_units(text.size(), need.text.size()));
}

/// Whether a need holds for NUL-terminated text.
bool need_holds(const sm::Need& need, bool present, std::string_view text) noexcept {
    bool met = false;
    if (need.kind == sm::NeedKind::contains && need.text.empty()) {
        met = present;
    } else if (!present) {
        met = false;
    } else if (need.kind == sm::NeedKind::range) {
        double number = 0;
        met = fully_numeric(text.data(), number) && need.low <= number && number <= need.high;
    } else if (need.kind == sm::NeedKind::equals) {
        met = same_text(text, need.text);
    } else {
        met = contains_nocase(text, need.text);
    }
    return need.negated ? !met : met;
}

bool schema_needs_hold(MapWork& work, int number) noexcept {
    for (std::size_t slot = 0; slot < work.needs.size(); ++slot) {
        const sm::Need& need = work.needs[slot];
        char digits[24];
        std::string_view text;
        bool present = false;
        schema_field_text(work, number, need.field, digits, text, present);
        add_bill(work, add_units(host::element_charge, compare_units(need, text)));
        if (!need_holds(need, present, text))
            return false;
    }
    return true;
}

/// Matches the map text against the pattern at the top of the stack, with
/// the sandbox's `string.find`, and pops the pattern. May raise; every
/// frame above it holds plain values only.
Tri pattern_find(lua_State* state, MapWork& work, std::string_view text) {
    const int pattern = lua_gettop(state);
    lua_getglobal(state, "string");
    if (!lua_istable(state, -1)) {
        lua_settop(state, pattern - 1);
        hold(work, "string.find is not there");
        return Tri::stop;
    }
    lua_getfield(state, -1, "find");
    lua_remove(state, -2);
    if (!lua_isfunction(state, -1)) {
        lua_settop(state, pattern - 1);
        hold(work, "string.find is not there");
        return Tri::stop;
    }
    lua_pushlstring(state, text.data(), text.size());
    lua_pushvalue(state, pattern);
    if (lua_pcall(state, 2, 1, 0) != LUA_OK) {
        if (lua_type(state, -1) == LUA_TSTRING) {
            std::size_t length = 0;
            const char* message = lua_tolstring(state, -1, &length);
            hold(work, std::string_view(message, length));
        } else {
            hold(work, "a pattern is not valid");
        }
        lua_settop(state, pattern - 1);
        return Tri::stop;
    }
    const bool found = !lua_isnil(state, -1);
    lua_settop(state, pattern - 1);
    return found ? Tri::yes : Tri::no;
}

/// Matches one map `where` field. May raise; holds plain values only.
Tri match_value(lua_State* state, MapWork& work, std::string_view field, int value_index) {
    char key[kKeyLimit];
    std::string_view text;
    bool present = false;
    where_text(work, field, key, text, present);
    if (map_match_value(state, value_index)) {
        if (!present)
            return Tri::no;
        const auto* bits =
            static_cast<const MatchBits*>(luaL_testudata(state, value_index, kMatchMeta));
        if (lua_getiuservalue(state, value_index, 1) != LUA_TSTRING) {
            lua_pop(state, 1);
            return Tri::no;
        }
        if (bits != nullptr && bits->kind == kPattern)
            return pattern_find(state, work, text);
        std::size_t length = 0;
        const char* pattern = lua_tolstring(state, -1, &length);
        host::charge_work(
            state, add_units(host::units_for_bytes(text.size()), product_units(length, text.size()))
        );
        const bool matched = glob_match(std::string_view(pattern, length), text);
        lua_pop(state, 1);
        return matched ? Tri::yes : Tri::no;
    }
    if (!need_from_lua(state, value_index, field, work.staging, &work.reason)) {
        hold(work, work.reason);
        return Tri::stop;
    }
    host::charge_work(state, compare_units(work.staging, text));
    return need_holds(work.staging, present, text) ? Tri::yes : Tri::no;
}

/// Whether every field of a map `where` holds. May raise; plain values only.
Tri where_holds(lua_State* state, MapWork& work, int table) {
    table = lua_absindex(state, table);
    lua_pushnil(state);
    while (lua_next(state, table) != 0) {
        if (lua_type(state, -2) != LUA_TSTRING) {
            lua_pop(state, 2);
            hold(work, "a maps where key must be text");
            return Tri::stop;
        }
        std::size_t length = 0;
        const char* raw = lua_tolstring(state, -2, &length);
        host::charge_work(state, host::element_charge + host::units_for_bytes(length));
        const Tri matched =
            match_value(state, work, trim_ends(std::string_view(raw, length)), lua_gettop(state));
        lua_pop(state, 1);
        if (matched != Tri::yes) {
            lua_pop(state, 1);
            return matched;
        }
    }
    return Tri::yes;
}

// Steps: the C++ work of each callback.

bool map_read(lua_State* state, MapWork& work, Emit& emit) {
    if (lua_type(state, 2) != LUA_TSTRING)
        return true;
    std::size_t length = 0;
    const char* raw = lua_tolstring(state, 2, &length);
    const std::string_view name(raw, length);
    std::string_view text;
    if (name == "name") {
        text = mission_name(work);
    } else if (name == "file") {
        text = work.chosen->key;
    } else if (name == "planet") {
        emit = stored_emit(work, tdf::find_value(header_block(work), "planet"));
        return true;
    } else if (name == "pack") {
        if (work.chosen->pack.empty())
            return true;
        text = work.chosen->pack;
    } else if (name == "header") {
        emit = box_emit(kBoxHeader, kHeaderMeta, -1, -1);
        return true;
    } else if (name == "schemas") {
        emit = box_emit(kBoxSchemas, kSchemasMeta, -1, -1);
        return true;
    } else {
        return true;
    }
    add_bill(work, host::units_for_bytes(text.size()));
    emit.kind = EmitKind::text;
    emit.text = text.data();
    emit.length = text.size();
    return true;
}

bool header_read(lua_State* state, MapWork& work, Emit& emit) {
    if (lua_type(state, 2) != LUA_TSTRING)
        return true;
    std::size_t length = 0;
    const char* raw = lua_tolstring(state, 2, &length);
    if (has_nul(std::string_view(raw, length)))
        return true;
    emit = stored_emit(work, tdf::find_value(header_block(work), raw));
    return true;
}

bool header_write(lua_State* state, MapWork& work, Emit&) {
    if (lua_type(state, 2) != LUA_TSTRING || header_block(work) == nullptr) {
        hold(work, "the map has no header");
        return false;
    }
    std::size_t length = 0;
    const char* raw = lua_tolstring(state, 2, &length);
    const std::string_view key(raw, length);
    const KeyRow* row = key_row(key);
    if (row == nullptr || !header_class(row->kind) || row->kind == KeyClass::header_frozen) {
        hold(work, "map.header.");
        hold_add(work, key, true);
        hold_add(work, " cannot change");
        return false;
    }
    ValueAction action;
    if (!read_action(state, 3, row, action, work))
        return false;
    return apply_header(work, *row, action);
}

bool schemas_read(lua_State* state, MapWork& work, Emit& emit) {
    if (lua_type(state, 2) == LUA_TSTRING) {
        std::size_t length = 0;
        const char* raw = lua_tolstring(state, 2, &length);
        const std::string_view name(raw, length);
        if (name == "each")
            emit = function_emit(schemas_each);
        else if (name == "where")
            emit = function_emit(schemas_where);
        else if (name == "set")
            emit = function_emit(schemas_set);
        else if (name == "scale")
            emit = function_emit(schemas_scale);
        return true;
    }
    const Box* box = box_of(state, 1, kSchemasMeta);
    if (box == nullptr || lua_type(state, 2) != LUA_TNUMBER)
        return true;
    int whole = 0;
    const lua_Integer slot = lua_tointegerx(state, 2, &whole);
    if (whole == 0)
        return true;
    if (slot < 1 || slot > set_count(state, work, *box, 1))
        return true;
    const int number = set_number(state, *box, 1, slot);
    if (number < 0)
        return true;
    emit = box_emit(kBoxSchema, kSchemaMeta, number, -1);
    return true;
}

bool schema_read(lua_State* state, MapWork& work, Emit& emit) {
    const Box* box = box_of(state, 1, kSchemaMeta);
    if (box == nullptr || lua_type(state, 2) != LUA_TSTRING)
        return true;
    std::size_t length = 0;
    const char* raw = lua_tolstring(state, 2, &length);
    const std::string_view key(raw, length);
    tdf::Block* schema = schema_block(work, box->schema);
    if (same_text(key, "index")) {
        emit = integer_emit(box->schema);
        return true;
    }
    if (same_text(key, "players")) {
        emit = integer_emit(start_positions(schema));
        return true;
    }
    if (same_text(key, "units")) {
        emit = box_emit(kBoxUnits, kUnitsMeta, box->schema, -1);
        return true;
    }
    if (same_text(key, "features")) {
        emit = box_emit(kBoxFeatures, kFeaturesMeta, box->schema, -1);
        return true;
    }
    if (has_nul(key))
        return true;
    emit = stored_emit(work, tdf::find_value(schema, raw));
    return true;
}

bool schema_write(lua_State* state, MapWork& work, Emit&) {
    const Box* box = box_of(state, 1, kSchemaMeta);
    if (box == nullptr || lua_type(state, 2) != LUA_TSTRING) {
        hold(work, "a schema field cannot change");
        return false;
    }
    std::size_t length = 0;
    const char* raw = lua_tolstring(state, 2, &length);
    const std::string_view key(raw, length);
    if (same_text(key, "units") || same_text(key, "features")) {
        hold(work, "a placed unit cannot be added");
        return false;
    }
    const KeyRow* row = key_row(key);
    if (row == nullptr || !schema_class(row->kind)) {
        hold_not_setting(work, key, true);
        return false;
    }
    ValueAction action;
    if (!read_action(state, 3, row, action, work))
        return false;
    if (row->kind == KeyClass::schema_profile) {
        if (!action_fits(work, *row, action, false) || !profile_ok(work, action))
            return false;
    }
    return apply_schema(work, box->schema, *row, action);
}

bool placed_read(lua_State* state, MapWork& work, Emit& emit) {
    const Box* units = box_of(state, 1, kUnitsMeta);
    const bool features = units == nullptr;
    const Box* box = features ? box_of(state, 1, kFeaturesMeta) : units;
    if (box == nullptr || lua_type(state, 2) != LUA_TNUMBER)
        return true;
    int whole = 0;
    const lua_Integer slot = lua_tointegerx(state, 2, &whole);
    if (whole == 0)
        return true;
    tdf::Block* parent = placed_parent(work, box->schema, features);
    if (slot < 1 || parent == nullptr || slot > static_cast<lua_Integer>(tdf::child_count(parent)))
        return true;
    emit = box_emit(
        features ? kBoxFeature : kBoxUnit,
        features ? kFeatureMeta : kUnitMeta,
        box->schema,
        static_cast<int32_t>(slot) - 1
    );
    return true;
}

bool item_read(lua_State* state, MapWork& work, Emit& emit) {
    const Box* unit = box_of(state, 1, kUnitMeta);
    const bool features = unit == nullptr;
    const Box* box = features ? box_of(state, 1, kFeatureMeta) : unit;
    if (box == nullptr || lua_type(state, 2) != LUA_TSTRING)
        return true;
    std::size_t length = 0;
    const char* raw = lua_tolstring(state, 2, &length);
    const std::string_view name(raw, length);
    if (name == "remove") {
        emit = function_emit(placed_remove);
        return true;
    }
    if (name == "index") {
        emit = integer_emit(static_cast<lua_Integer>(box->item) + 1);
        return true;
    }
    if (has_nul(name))
        return true;
    emit = stored_emit(
        work, tdf::find_value(placed_child(work, box->schema, features, box->item), raw)
    );
    return true;
}

/// Checks a placed write. A name waits in `work.lookup_name` for the
/// collections; a position is written here.
bool item_write(lua_State* state, MapWork& work, Emit&) {
    work.lookup_name = nullptr;
    const Box* unit = box_of(state, 1, kUnitMeta);
    const bool features = unit == nullptr;
    const Box* box = features ? box_of(state, 1, kFeatureMeta) : unit;
    if (box == nullptr || lua_type(state, 2) != LUA_TSTRING) {
        hold(work, "a placed entry cannot change");
        return false;
    }
    std::size_t length = 0;
    const char* raw = lua_tolstring(state, 2, &length);
    const std::string_view key(raw, length);
    const char* spelling = placed_spelling(key, features);
    if (spelling == nullptr) {
        hold(work, features ? "a placed feature's " : "a placed unit's ");
        hold_add(work, key, true);
        hold_add(work, " cannot change");
        return false;
    }
    ValueAction action;
    if (!read_action(state, 3, nullptr, action, work))
        return false;
    const bool name = features || same_text(key, "unitname");
    if (name) {
        if (action.kind != ValueKind::text) {
            hold(
                work,
                features ? "a placed feature's featurename must be text"
                         : "a placed unit's unitname must be text"
            );
            return false;
        }
        if (features && action.text_length > kFeatureNameLimit) {
            hold(work, "a feature name must be shorter than 128 bytes");
            return false;
        }
        if (placed_child(work, box->schema, features, box->item) == nullptr) {
            hold(work, features ? "the map has no such feature" : "the map has no such unit");
            return false;
        }
        work.lookup_name = action.text;
        work.lookup_length = action.text_length;
        work.lookup_feature = features;
        return true;
    }
    const bool player = same_text(key, "player");
    double integral = 0;
    const bool ok =
        action.kind == ValueKind::number && whole_number(action.number, integral) &&
        (player ? integral >= 1.0 && integral <= 255.0 : integral >= 0.0 && integral <= 32767.0);
    if (!ok) {
        if (player) {
            hold(work, "a placed unit's player must be a whole number from 1 to 255");
        } else {
            hold(work, "a placed unit's ");
            hold_add(work, key, true);
            hold_add(work, " must be a whole number from 0 to 32767");
        }
        return false;
    }
    const std::optional<std::string> spelled = tdf::format_number(integral);
    if (!spelled.has_value()) {
        hold(work, "the chosen map could not be written");
        return false;
    }
    return write_placed(work, box->schema, false, box->item, spelling, *spelled);
}

/// Writes the placed name the collections found.
bool item_write_name(lua_State* state, MapWork& work, Emit&) {
    const Box* unit = box_of(state, 1, kUnitMeta);
    const bool features = unit == nullptr;
    const Box* box = features ? box_of(state, 1, kFeatureMeta) : unit;
    if (box == nullptr || work.lookup_name == nullptr) {
        hold(work, "a placed entry cannot change");
        return false;
    }
    return write_placed(
        work,
        box->schema,
        features,
        box->item,
        features ? "Featurename" : "UnitName",
        std::string_view(work.lookup_name, work.lookup_length)
    );
}

bool remove_step(lua_State* state, MapWork& work, Emit&) {
    const Box* unit = box_of(state, 1, kUnitMeta);
    const bool features = unit == nullptr;
    const Box* box = features ? box_of(state, 1, kFeatureMeta) : unit;
    if (box == nullptr) {
        hold(work, "the map is not there");
        return false;
    }
    return remove_placed(work, box->schema, features, box->item);
}

/// Reads a schema `where` and keeps the numbers that match in `work.numbers`.
bool where_step(lua_State* state, MapWork& work, Emit&) {
    work.needs.clear();
    work.numbers.clear();
    const Box* box = box_of(state, 1, kSchemasMeta);
    if (box == nullptr) {
        hold(work, "the map is not there");
        return false;
    }
    lua_pushnil(state);
    while (lua_next(state, 2) != 0) {
        if (lua_type(state, -2) != LUA_TSTRING) {
            lua_pop(state, 2);
            hold(work, "a maps where key must be text");
            return false;
        }
        if (work.needs.size() >= kEntryLimit) {
            lua_pop(state, 2);
            hold(work, "too many statements");
            return false;
        }
        std::size_t length = 0;
        const char* raw = lua_tolstring(state, -2, &length);
        add_bill(work, host::element_charge + host::units_for_bytes(length));
        const std::string_view field = trim_ends(std::string_view(raw, length));
        if (!need_from_lua(state, -1, field, work.staging, &work.reason)) {
            lua_pop(state, 2);
            hold(work, work.reason);
            return false;
        }
        work.needs.push_back(std::move(work.staging));
        lua_pop(state, 1);
    }
    const lua_Integer count = set_count(state, work, *box, 1);
    for (lua_Integer slot = 1; slot <= count; ++slot) {
        const int number = set_number(state, *box, 1, slot);
        add_bill(work, host::element_charge);
        if (number >= 0 && schema_needs_hold(work, number))
            work.numbers.push_back(number);
    }
    return true;
}

bool change_schemas(lua_State* state, MapWork& work, bool factors) {
    const Box* box = box_of(state, 1, kSchemasMeta);
    if (box == nullptr) {
        hold(work, "the map is not there");
        return false;
    }
    if (!collect_changes(state, work, 2, factors))
        return false;
    if (factors) {
        for (ChangeItem& item : work.items) {
            if (item.action.kind == ValueKind::number)
                item.action.kind = ValueKind::scale;
        }
    }
    if (!check_profiles(work))
        return false;
    const lua_Integer count = set_count(state, work, *box, 1);
    for (lua_Integer slot = 1; slot <= count; ++slot) {
        const int number = set_number(state, *box, 1, slot);
        if (number < 0)
            continue;
        if (!apply_items(work, number))
            return false;
    }
    return true;
}

bool set_step(lua_State* state, MapWork& work, Emit&) {
    return change_schemas(state, work, false);
}

bool scale_step(lua_State* state, MapWork& work, Emit&) {
    return change_schemas(state, work, true);
}

/// Applies the change table at `work.table` of a matched list item.
bool table_change_step(lua_State* state, MapWork& work, Emit&) {
    if (!collect_changes(state, work, work.table, false))
        return false;
    if (!check_profiles(work))
        return false;
    return apply_items(work, -1);
}

// Trampolines. Each charges first, runs its step, settles the bill, and only
// then pushes or raises.

int finish_read(lua_State* state, MapWork* work, Step step) {
    charge_access(state);
    if (work == nullptr) {
        lua_pushnil(state);
        return 1;
    }
    Emit emit;
    const bool ok = guarded(state, *work, step, emit);
    settle(state, *work);
    if (!ok)
        return raise_held(state, *work);
    return push_emit(state, *work, emit);
}

/// Looks a placed name up in the collections, as the mutators above left
/// them. The lookup is charged and can raise: plain values only here.
int lookup_placed(lua_State* state, MapWork& work) {
    if (work.collections == nullptr)
        return 0;
    return work.collections->has_record(
        state, work.lookup_feature, std::string_view(work.lookup_name, work.lookup_length)
    );
}

int finish_write(lua_State* state, MapWork* work, Step step, const char* missing) {
    charge_write(state);
    if (work == nullptr) {
        lua_pushstring(state, missing);
        return lua_error(state);
    }
    // A lookup that raised, and was caught, leaves no name for this write.
    work->lookup_name = nullptr;
    Emit emit;
    const bool ok = guarded(state, *work, step, emit);
    settle(state, *work);
    if (!ok)
        return raise_held(state, *work);
    if (work->lookup_name == nullptr)
        return 0;
    const int found = lookup_placed(state, *work);
    if (found <= 0) {
        if (found < 0) {
            hold(*work, "out of memory");
        } else {
            hold(*work, work->lookup_feature ? "the feature " : "the unit ");
            hold_add(*work, std::string_view(work->lookup_name, work->lookup_length));
            hold_add(*work, " is not there");
        }
        work->lookup_name = nullptr;
        return raise_held(state, *work);
    }
    const bool written = guarded(state, *work, item_write_name, emit);
    work->lookup_name = nullptr;
    settle(state, *work);
    if (!written)
        return raise_held(state, *work);
    return 0;
}

/// Runs a method of the schema set at index 1 on the table at index 2.
int finish_method(lua_State* state, Step step, const char* expects) {
    host::charge_work(state, kFieldCharge);
    MapWork* work = work_in(box_of(state, 1, kSchemasMeta));
    if (work == nullptr) {
        lua_pushliteral(state, "the map is not there");
        return lua_error(state);
    }
    if (!lua_istable(state, 2)) {
        lua_pushstring(state, expects);
        return lua_error(state);
    }
    Emit emit;
    const bool ok = guarded(state, *work, step, emit);
    settle(state, *work);
    if (!ok)
        return raise_held(state, *work);
    return 0;
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
    lua_createtable(state, 0, 4);
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

int push_maps_call(void* context, lua_State* state) {
    auto* box = static_cast<ApplyBox*>(context);
    install_view(state);
    if (box->function_entry) {
        lua_rawgeti(state, LUA_REGISTRYINDEX, box->ref);
        push_box(state, box->work, kBoxMap, kMapMeta, -1, -1);
        push_context(state, box->context);
        if (box->stop != nullptr)
            set_incompatible(state, lua_gettop(state), box->stop);
        return 2;
    }
    lua_pushlightuserdata(state, box);
    lua_pushcclosure(state, apply_table_form, 1);
    return 0;
}

/// Checks a plan's maps list. Runs outside a phase: nothing here raises.
bool list_shape_ok(lua_State* state, int index, MapWork& work) {
    index = lua_absindex(state, index);
    if (lua_getmetatable(state, index) != 0) {
        lua_pop(state, 1);
        hold(work, "maps must be a plain table, not one with a metatable");
        return false;
    }
    const lua_Unsigned count = lua_rawlen(state, index);
    lua_pushnil(state);
    while (lua_next(state, index) != 0) {
        if (lua_type(state, -2) != LUA_TNUMBER) {
            hold(work, "maps must be a list");
            lua_pop(state, 2);
            return false;
        }
        const lua_Integer slot = lua_tointeger(state, -2);
        const lua_Number raw = lua_tonumber(state, -2);
        if (slot < 1 || static_cast<lua_Unsigned>(slot) > count ||
            static_cast<lua_Number>(slot) != raw) {
            hold(work, "maps must be a list");
            lua_pop(state, 2);
            return false;
        }
        if (!finished_where_rule(state, -1)) {
            hold(work, "maps must be a function or a list of where():change()");
            lua_pop(state, 2);
            return false;
        }
        push_rule_changes(state, lua_gettop(state));
        hold_clear(work);
        if (!lua_istable(state, -1) || !collect_changes(state, work, lua_gettop(state), false)) {
            if (work.error_len == 0)
                hold(work, "maps must be a function or a list of where():change()");
            lua_pop(state, 3);
            return false;
        }
        lua_pop(state, 2);
    }
    return true;
}

struct FieldKey {
    std::string record;
    std::string block;
    std::string field;

    bool operator<(const FieldKey& other) const {
        if (record != other.record)
            return record < other.record;
        if (block != other.block)
            return block < other.block;
        return field < other.field;
    }
};

extern "C" {

static int form_glob(lua_State* state) {
    std::size_t length = 0;
    const char* text = luaL_checklstring(state, 1, &length);
    if (length > kGlobLimit) {
        lua_pushliteral(state, "a glob is at most 256 bytes");
        return lua_error(state);
    }
    push_match(state, kGlob, text, length);
    return 1;
}

static int form_pattern(lua_State* state) {
    std::size_t length = 0;
    const char* text = luaL_checklstring(state, 1, &length);
    if (length > sm::max_text_bytes) {
        lua_pushliteral(state, "text is longer than 1024 bytes");
        return lua_error(state);
    }
    push_match(state, kPattern, text, length);
    return 1;
}

static int map_index(lua_State* state) {
    return finish_read(state, work_in(box_of(state, 1, kMapMeta)), map_read);
}

static int map_newindex(lua_State* state) {
    charge_write(state);
    lua_pushliteral(state, "the map cannot change there");
    return lua_error(state);
}

static int header_index(lua_State* state) {
    return finish_read(state, work_in(box_of(state, 1, kHeaderMeta)), header_read);
}

static int header_newindex(lua_State* state) {
    return finish_write(
        state, work_in(box_of(state, 1, kHeaderMeta)), header_write, "the map has no header"
    );
}

static int schemas_index(lua_State* state) {
    return finish_read(state, work_in(box_of(state, 1, kSchemasMeta)), schemas_read);
}

static int schemas_newindex(lua_State* state) {
    charge_write(state);
    lua_pushliteral(state, "a schema cannot be added");
    return lua_error(state);
}

static int schemas_len(lua_State* state) {
    host::charge_work(state, kFieldCharge);
    const Box* box = box_of(state, 1, kSchemasMeta);
    MapWork* work = work_in(box);
    if (work == nullptr) {
        lua_pushinteger(state, 0);
        return 1;
    }
    lua_pushinteger(state, set_count(state, *work, *box, 1));
    return 1;
}

static int schemas_each(lua_State* state) {
    host::charge_work(state, kFieldCharge);
    const Box* box = box_of(state, 1, kSchemasMeta);
    MapWork* work = work_in(box);
    if (work == nullptr) {
        lua_pushliteral(state, "the map is not there");
        return lua_error(state);
    }
    if (!lua_isfunction(state, 2)) {
        lua_pushliteral(state, "each expects a function");
        return lua_error(state);
    }
    const lua_Integer count = set_count(state, *work, *box, 1);
    host::charge_work(state, static_cast<uint64_t>(count) * host::element_charge);
    lua_settop(state, 2);
    if (box->kind == kBoxFiltered) {
        lua_getiuservalue(state, 1, 1);
    } else {
        lua_createtable(state, static_cast<int>(count), 0);
        for (lua_Integer slot = 1; slot <= count; ++slot) {
            lua_pushinteger(state, slot - 1);
            lua_rawseti(state, -2, slot);
        }
    }
    for (lua_Integer slot = 1; slot <= count; ++slot) {
        lua_pushvalue(state, 2);
        lua_rawgeti(state, 3, slot);
        const auto number = static_cast<int32_t>(lua_tointeger(state, -1));
        lua_pop(state, 1);
        push_box(state, work, kBoxSchema, kSchemaMeta, number, -1);
        lua_call(state, 1, 0);
    }
    return 0;
}

static int schemas_where(lua_State* state) {
    host::charge_work(state, kFieldCharge);
    MapWork* work = work_in(box_of(state, 1, kSchemasMeta));
    if (work == nullptr) {
        lua_pushliteral(state, "the map is not there");
        return lua_error(state);
    }
    if (!lua_istable(state, 2)) {
        lua_pushliteral(state, "where expects a table");
        return lua_error(state);
    }
    Emit emit;
    const bool ok = guarded(state, *work, where_step, emit);
    settle(state, *work);
    if (!ok)
        return raise_held(state, *work);
    push_box(state, work, kBoxFiltered, kSchemasMeta, -1, -1);
    const std::size_t kept = work->numbers.size();
    lua_createtable(state, static_cast<int>(kept), 0);
    for (std::size_t slot = 0; slot < kept; ++slot) {
        lua_pushinteger(state, work->numbers[slot]);
        lua_rawseti(state, -2, static_cast<lua_Integer>(slot) + 1);
    }
    lua_setiuservalue(state, -2, 1);
    return 1;
}

static int schemas_set(lua_State* state) {
    return finish_method(state, set_step, "set expects a table");
}

static int schemas_scale(lua_State* state) {
    return finish_method(state, scale_step, "scale expects a table");
}

static int schema_index(lua_State* state) {
    return finish_read(state, work_in(box_of(state, 1, kSchemaMeta)), schema_read);
}

static int schema_newindex(lua_State* state) {
    return finish_write(
        state, work_in(box_of(state, 1, kSchemaMeta)), schema_write, "a schema field cannot change"
    );
}

static int placed_index(lua_State* state) {
    const Box* units = box_of(state, 1, kUnitsMeta);
    const Box* box = units != nullptr ? units : box_of(state, 1, kFeaturesMeta);
    return finish_read(state, work_in(box), placed_read);
}

static int placed_newindex(lua_State* state) {
    charge_write(state);
    if (box_of(state, 1, kFeaturesMeta) != nullptr)
        lua_pushliteral(state, "a placed feature cannot be added");
    else
        lua_pushliteral(state, "a placed unit cannot be added");
    return lua_error(state);
}

static int placed_len(lua_State* state) {
    host::charge_work(state, kFieldCharge);
    const Box* units = box_of(state, 1, kUnitsMeta);
    const bool features = units == nullptr;
    const Box* box = features ? box_of(state, 1, kFeaturesMeta) : units;
    MapWork* work = work_in(box);
    if (work == nullptr) {
        lua_pushinteger(state, 0);
        return 1;
    }
    tdf::Block* parent = placed_parent(*work, box->schema, features);
    lua_pushinteger(state, static_cast<lua_Integer>(tdf::child_count(parent)));
    return 1;
}

static int placed_remove(lua_State* state) {
    host::charge_work(state, kFieldCharge);
    const Box* unit = box_of(state, 1, kUnitMeta);
    MapWork* work = work_in(unit != nullptr ? unit : box_of(state, 1, kFeatureMeta));
    if (work == nullptr) {
        lua_pushliteral(state, "the map is not there");
        return lua_error(state);
    }
    Emit emit;
    const bool ok = guarded(state, *work, remove_step, emit);
    settle(state, *work);
    if (!ok)
        return raise_held(state, *work);
    return 0;
}

static int item_index(lua_State* state) {
    const Box* unit = box_of(state, 1, kUnitMeta);
    const Box* box = unit != nullptr ? unit : box_of(state, 1, kFeatureMeta);
    return finish_read(state, work_in(box), item_read);
}

static int item_newindex(lua_State* state) {
    const Box* unit = box_of(state, 1, kUnitMeta);
    const Box* box = unit != nullptr ? unit : box_of(state, 1, kFeatureMeta);
    return finish_write(state, work_in(box), item_write, "a placed entry cannot change");
}

static int apply_table_form(lua_State* state) {
    auto* apply = static_cast<ApplyBox*>(lua_touserdata(state, lua_upvalueindex(1)));
    if (apply == nullptr || apply->work == nullptr) {
        lua_pushliteral(state, "the map is not there");
        return lua_error(state);
    }
    MapWork& work = *apply->work;
    lua_rawgeti(state, LUA_REGISTRYINDEX, apply->ref);
    const int list = lua_gettop(state);
    const lua_Unsigned count = lua_rawlen(state, list);
    for (lua_Unsigned slot = 1; slot <= count; ++slot) {
        host::charge_work(state, host::element_charge);
        lua_rawgeti(state, list, static_cast<lua_Integer>(slot));
        const int rule = lua_gettop(state);
        push_rule_needs(state, rule);
        const int needs = lua_gettop(state);
        if (!lua_istable(state, needs)) {
            lua_settop(state, list - 1);
            hold(work, "maps must be a function or a list of where():change()");
            return raise_held(state, work);
        }
        const Tri holds = where_holds(state, work, needs);
        if (holds == Tri::stop) {
            lua_settop(state, list - 1);
            return raise_held(state, work);
        }
        if (holds == Tri::yes) {
            push_rule_changes(state, rule);
            if (!lua_istable(state, -1)) {
                lua_settop(state, list - 1);
                hold(work, "the maps change could not be applied");
                return raise_held(state, work);
            }
            work.table = lua_gettop(state);
            Emit emit;
            const bool ok = guarded(state, work, table_change_step, emit);
            settle(state, work);
            if (!ok) {
                lua_settop(state, list - 1);
                return raise_held(state, work);
            }
            lua_pop(state, 1);
        }
        lua_pop(state, 2);
    }
    lua_pop(state, 1);
    return 0;
}

} // extern "C"

} // namespace

std::string map_header_refusal(std::string_view key, const std::optional<std::string>& text) {
    const KeyRow* row = key_row(key);
    if (row == nullptr || !header_class(row->kind)) {
        std::string sentence = "the maps change \"";
        sentence.append(key.data(), key.size());
        sentence += "\" is not a header or schema setting";
        return sentence;
    }
    if (row->kind == KeyClass::header_frozen) {
        std::string sentence = "map.header.";
        sentence += row->folded;
        sentence += " cannot change";
        return sentence;
    }
    std::string sentence = "map.header.";
    sentence += row->folded;
    sentence += header_limit_tail(row->kind);
    if (!text.has_value()) {
        std::string removed = "map.header.";
        removed += row->folded;
        removed += " cannot be removed";
        return removed;
    }
    double number = 0;
    if (!fully_numeric(text->c_str(), number))
        return sentence;
    if (row->kind == KeyClass::header_whole) {
        // A rule may leave a fraction. The range is the one the header uses.
        if (!std::isfinite(number) || number < 0.0 || number > 32767.0)
            return sentence;
        return {};
    }
    double accepted = 0;
    if (!number_in_header(*row, number, accepted))
        return sentence;
    return {};
}

std::string map_schema_metal_refusal(const std::optional<std::string>& text) {
    double number = 0;
    double integral = 0;
    if (!text.has_value() || !fully_numeric(text->c_str(), number) ||
        !whole_number(number, integral) || integral < 0.0 || integral > 255.0)
        return k_schema_metal_limit;
    return {};
}

bool write_map_field(
    tdf::Document* document, tdf::Block* block, const char* spelling, std::string_view text
) {
    if (document == nullptr || block == nullptr || spelling == nullptr)
        return false;
    return tdf::set_value(document, block, spelling, text) == tdf::EditStatus::ok;
}

int map_schema_count(const tdf::Document* document) {
    if (document == nullptr || document->root == nullptr)
        return 0;
    const tdf::Block* header = tdf::find_child(document->root, "GlobalHeader");
    if (header == nullptr)
        return 0;
    int found = 0;
    for (int number = 0; number < kSchemaLimit; ++number) {
        char name[32];
        schema_name(number, name);
        if (tdf::find_child(header, name) == nullptr)
            break;
        ++found;
    }
    return found;
}

tdf::Block* map_schema_block(tdf::Document* document, int number) {
    if (document == nullptr || number < 0 || number >= kSchemaLimit)
        return nullptr;
    tdf::Block* header = tdf::editable_child(tdf::editable_root(document), "GlobalHeader");
    if (header == nullptr)
        return nullptr;
    char name[32];
    schema_name(number, name);
    return tdf::editable_child(header, name);
}

std::string map_schema_label(int number) {
    if (number < 0)
        return {};
    char name[32];
    schema_name(number, name);
    return std::string(name);
}

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
    if (!lua_istable(state, index))
        return "maps must be a function or a list of where():change()";
    MapWork work;
    if (!list_shape_ok(state, index, work))
        return std::string(work.error, work.error_len);
    return {};
}

std::vector<Conflict> map_conflicts(const std::vector<JournalEntry>& rows) {
    std::vector<Conflict> conflicts;
    std::map<FieldKey, std::vector<uint16_t>> writers;
    for (const JournalEntry& row : rows) {
        if (row.change != Change::set || row.mutators.empty())
            continue;
        const uint16_t mutator = row.mutators.front();
        std::vector<uint16_t>& seen = writers[FieldKey{row.record, row.block, row.field}];
        if (std::find(seen.begin(), seen.end(), mutator) != seen.end())
            continue;
        for (const uint16_t earlier : seen) {
            Conflict conflict;
            conflict.severity = Severity::note;
            conflict.kind = ConflictKind::same_field_hooked;
            conflict.mutators.push_back(earlier);
            conflict.mutators.push_back(mutator);
            conflict.record = row.record;
            conflict.field = row.field;
            conflict.path = row.path;
            conflicts.push_back(std::move(conflict));
        }
        seen.push_back(mutator);
    }
    return conflicts;
}

MapsTurn apply_maps(
    host::Sandbox& sandbox,
    bool function_entry,
    int ref,
    const ChosenMap& chosen,
    const std::vector<uint8_t>& current,
    oa::AssetStore& store,
    Collections& collections,
    const defs::DataLayout& layout,
    const MapScriptContext& context,
    uint16_t mutator
) {
    MapsTurn turn;
    try {
        MapWork work;
        work.store = &store;
        work.collections = &collections;
        work.layout = &layout;
        work.chosen = &chosen;
        work.mutator = mutator;
        const std::string text(current.begin(), current.end());
        tdf::ParseError error{};
        if (current.empty() || !work.document.parse(text, &error)) {
            turn.ok = false;
            turn.message = "the chosen map could not be read";
            return turn;
        }
        DeclaredStop stop;
        ApplyBox box;
        box.work = &work;
        box.context = &context;
        box.stop = &stop;
        box.ref = ref;
        box.function_entry = function_entry;
        host::CallHooks hooks;
        hooks.context = &box;
        hooks.push = push_maps_call;
        host::ScriptError script_error;
        const bool called = sandbox.call("maps", hooks, script_error);
        if (stop.raised) {
            // A caught error still ends the turn, and none of its bytes are kept.
            turn.ok = false;
            turn.declared = true;
            turn.from_script = false;
            turn.message = declared_reason(stop);
            return turn;
        }
        if (!called) {
            turn.ok = false;
            turn.from_script = true;
            turn.kind = script_error.kind;
            turn.message = script_error.message;
            return turn;
        }
        if (work.over_limit) {
            // The script caught the error, but the limit fails the mutator.
            turn.ok = false;
            turn.from_script = true;
            turn.kind = host::ErrorKind::runtime;
            turn.message = "this mutator has changed too many fields";
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
    } catch (const std::bad_alloc&) {
        turn = MapsTurn{};
        turn.ok = false;
        turn.message = "out of memory";
    } catch (const std::exception& ex) {
        turn = MapsTurn{};
        turn.ok = false;
        turn.message =
            ex.what() != nullptr && ex.what()[0] != '\0' ? ex.what() : "the maps hook stopped";
    }
    return turn;
}

} // namespace oa::data::mutators
