// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Lua collections over one document set. A handle remembers the section it
// was given and looks that section up again on every access. The name stored
// in the handle is for messages. Iteration follows the folded name, then the
// document path, then the ordinal of that name in its document. It does not
// follow Lua's table order. Name lookup uses an index that a removal builds
// again; a change to UnitName moves only that unit's entry. Each index entry
// carries its section's handle slot. A mutator that passes the write or note
// limit fails.
#include "oa/data/mutators/collections.hpp"

#include "oa/data/field_schema.hpp"
#include "oa/data/script_host/script_host.hpp"
#include "oa/data/script_host/sorted.hpp"
#include "oa/formats/tdf/edit.hpp"
#include "oa/formats/tdf/number.hpp"

#include "lua.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace oa::data::mutators {

/// Indexes, section identities and the storage used while a hook runs.
struct CollectionsState {
    /// Which collection a view or a record belongs to.
    enum class Kind : uint8_t { units, weapons, features };

    /// A value a script asked to store.
    enum class StoredKind : uint8_t { nil_value, boolean, number, text };

    /// One section a handle can name: its document and its block.
    struct Identity {
        uint32_t doc_index = 0;
        oa::formats::tdf::Block* section = nullptr;
    };

    /// Orders identities for the slot lookup. Nothing walks this order.
    struct IdentityLess {
        bool operator()(const Identity& left, const Identity& right) const noexcept {
            if (left.doc_index != right.doc_index)
                return left.doc_index < right.doc_index;
            return std::less<const oa::formats::tdf::Block*>{}(left.section, right.section);
        }
    };

    /// The section each slot names. A slot is never reused.
    std::vector<Identity> identities;

    /// The slot of each section in `identities`, found without a scan.
    std::map<Identity, uint32_t, IdentityLess> slots;

    struct Rec {
        uint32_t doc_index = 0;
        oa::formats::tdf::Block* section = nullptr;
        uint32_t ordinal = 0;
        uint32_t slot = 0; ///< the section's slot in `identities`
        std::string name;
        std::string path;
    };

    struct Index {
        bool ready = false;
        std::vector<Rec> records;
    };

    Index indexes[3]{};

    struct WriterKey {
        std::string path;
        std::string record;
        std::string block;
        std::string key;

        bool operator<(const WriterKey& other) const {
            if (path != other.path)
                return path < other.path;
            if (record != other.record)
                return record < other.record;
            if (block != other.block)
                return block < other.block;
            return key < other.key;
        }
    };

    std::map<WriterKey, std::vector<std::string>> writers;

    struct Tally {
        std::string mutator;
        uint32_t writes = 0;
        uint32_t notes = 0;
    };

    std::vector<Tally> tallies;

    struct Live {
        Kind kind = Kind::units;
        bool damage = false;
        uint32_t doc_index = 0;
        uint32_t section_index = 0;
        std::string name;
        std::string path;
        oa::formats::tdf::Document* document = nullptr;
        oa::formats::tdf::Block* section = nullptr;
        oa::formats::tdf::Block* block = nullptr;
    };

    struct Handle {
        Kind kind = Kind::units;
        bool damage = false;
        uint32_t slot = 0;
        std::string name;
    };

    struct Stored {
        StoredKind kind = StoredKind::nil_value;
        bool flag = false;
        double number = 0;
        std::string text;
    };

    struct Assignment {
        std::string key;
        Stored value;
    };

    struct Factor {
        std::string key;
        double number = 0;
    };

    struct Prepared {
        oa::data::field_schema::Value::Kind kind = oa::data::field_schema::Value::Kind::absent;
        int64_t integer = 0;
        double number = 0;
        bool flag = false;
        std::string text;
    };

    std::vector<oa::formats::soft_mutator::Need> needs;
    oa::formats::soft_mutator::Need staging;
    std::vector<uint32_t> matched;
    std::vector<Handle> handles;
    std::vector<Assignment> assignments;
    std::vector<Factor> factors;
    std::vector<std::string> keys;
    std::vector<Live> lives;
    Live live;
    Stored stored;
    Prepared prepared;

    /// The next Lua error, held until the trampoline can raise it.
    std::string pending_error;
};

/// The bindings reach the journal through here. Callers do not use this type.
struct CollectionsAccess {
    static oa::data::mutation_rules::DocumentSet& documents(Collections& self) noexcept {
        return *self.documents_;
    }

    static const oa::data::defs::DataLayout* layout_ptr(const Collections& self) noexcept {
        return self.layout_;
    }

    static bool active(const Collections& self) noexcept { return self.active_; }

    static const std::string& mutator(const Collections& self) noexcept { return self.mutator_; }

    static const WriteGrant& grant(const Collections& self) noexcept { return self.grant_; }

    static const std::vector<oa::data::mutation_rules::FieldWrite>&
    writes(const Collections& self) noexcept {
        return self.writes_;
    }

    static std::vector<oa::data::mutation_rules::FieldWrite>& writes(Collections& self) noexcept {
        return self.writes_;
    }

    static std::vector<RecordRemoval>& removals(Collections& self) noexcept {
        return self.removals_;
    }

    static std::vector<HookNote>& notes(Collections& self) noexcept { return self.notes_; }

    static CollectionsState& work(Collections& self) noexcept { return *self.state_; }
};

namespace {

namespace tdf = oa::formats::tdf;
namespace fields = oa::data::field_schema;
namespace rules = oa::data::mutation_rules;
namespace sm = oa::formats::soft_mutator;
namespace host = oa::data::script_host;
namespace defs = oa::data::defs;

using Kind = CollectionsState::Kind;
using StoredKind = CollectionsState::StoredKind;

constexpr const char* kNeedMeta = "oa.mutators.need";
constexpr const char* kCollectionMeta = "oa.mutators.collection";
constexpr const char* kSetMeta = "oa.mutators.set";
constexpr const char* kRecordMeta = "oa.mutators.record";

/// Numbers and flags of a need. The compared text is uservalue 1, so the
/// userdata has nothing to destroy.
struct NeedBits {
    uint8_t kind = 0;
    uint8_t negated = 0;
    double low = 0;
    double high = 0;
};

static_assert(std::is_trivially_copyable_v<NeedBits>);
static_assert(std::is_trivially_destructible_v<NeedBits>);

/// A collection or a set. A set keeps its records in uservalue 1.
struct ViewBox {
    Collections* owner = nullptr;
    Kind kind = Kind::units;
};

/// A record handle. The name's bytes follow the struct in the same userdata.
struct RecordBox {
    Collections* owner = nullptr;
    Kind kind = Kind::units;
    uint8_t damage = 0;
    uint32_t slot = 0;
    uint32_t name_size = 0;
};

static_assert(std::is_trivially_copyable_v<RecordBox>);
static_assert(std::is_standard_layout_v<RecordBox>);

using Rec = CollectionsState::Rec;
using Live = CollectionsState::Live;
using Handle = CollectionsState::Handle;
using Stored = CollectionsState::Stored;
using Assignment = CollectionsState::Assignment;
using Factor = CollectionsState::Factor;
using Prepared = CollectionsState::Prepared;
using Identity = CollectionsState::Identity;
using WriterKey = CollectionsState::WriterKey;

/// The key and text currently stored for one field.
struct FieldNow {
    bool present = false;
    std::string key;
    std::optional<std::string> text;
};

/// One section a set will remove, in snapshot order.
struct SectionDrop {
    uint32_t doc_index = 0;
    uint32_t section_index = 0;
    bool journal_document = false;
    std::string name;
    std::string path;
};

CollectionsState& working(Collections& self) noexcept {
    return CollectionsAccess::work(self);
}

/// The error the next raise on this collections object reports.
std::string& pending_of(Collections& self) noexcept {
    return working(self).pending_error;
}

/// Reports whether a byte is a space, a tab or a line break.
bool is_separator(unsigned char byte) noexcept {
    return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r';
}

/// Trims spaces, tabs and line breaks from both ends.
std::string_view trim_view(std::string_view text) noexcept {
    while (!text.empty() && is_separator(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    while (!text.empty() && is_separator(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);
    return text;
}

/// Folds one ASCII letter to lower case.
unsigned char fold_byte(unsigned char byte) noexcept {
    if (byte >= 'A' && byte <= 'Z')
        return static_cast<unsigned char>(byte - 'A' + 'a');
    return byte;
}

/// Copies text with ASCII letters folded. Other bytes stay as they are.
std::string fold_copy(std::string_view text) {
    std::string folded(text);
    for (char& byte : folded)
        byte = static_cast<char>(fold_byte(static_cast<unsigned char>(byte)));
    return folded;
}

/// Compares two texts after ASCII lower-casing. A shorter prefix comes first.
int compare_folded(std::string_view left, std::string_view right) noexcept {
    const std::size_t count = std::min(left.size(), right.size());
    for (std::size_t index = 0; index < count; ++index) {
        const unsigned char left_byte = fold_byte(static_cast<unsigned char>(left[index]));
        const unsigned char right_byte = fold_byte(static_cast<unsigned char>(right[index]));
        if (left_byte < right_byte)
            return -1;
        if (left_byte > right_byte)
            return 1;
    }
    if (left.size() < right.size())
        return -1;
    if (left.size() > right.size())
        return 1;
    return 0;
}

/// Reports whether two texts match without regard to ASCII case.
bool same_text(std::string_view left, std::string_view right) noexcept {
    return compare_folded(left, right) == 0;
}

/// Orders records by folded name, then folded path, then ordinal.
bool rec_before(const Rec& left, const Rec& right) noexcept {
    const int names = compare_folded(left.name, right.name);
    if (names != 0)
        return names < 0;
    const int paths = compare_folded(left.path, right.path);
    if (paths != 0)
        return paths < 0;
    return left.ordinal < right.ordinal;
}

/// The collection's layer name, as a grant error spells it.
const char* layer_name(Kind kind) noexcept {
    if (kind == Kind::units)
        return "units";
    if (kind == Kind::weapons)
        return "weapons";
    return "features";
}

/// The schema a need on this collection uses.
rules::TargetKind target_kind(Kind kind) noexcept {
    if (kind == Kind::units)
        return rules::TargetKind::units;
    if (kind == Kind::weapons)
        return rules::TargetKind::weapons;
    return rules::TargetKind::features;
}

/// The typed schema of a record, or nothing for a feature.
std::optional<fields::RecordKind> schema_of(const Live& live) noexcept {
    if (live.kind == Kind::features)
        return std::nullopt;
    if (live.damage)
        return fields::RecordKind::weapon_damage;
    if (live.kind == Kind::weapons)
        return fields::RecordKind::weapon;
    return fields::RecordKind::unit;
}

/// The file name at the end of a path.
std::string file_name(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    if (slash == std::string_view::npos)
        return std::string(path);
    return std::string(path.substr(slash + 1));
}

/// The file name without its last extension.
std::string file_stem(std::string_view path) {
    const std::string filename = file_name(path);
    const std::size_t dot = filename.rfind('.');
    if (dot == std::string::npos || dot == 0)
        return filename;
    return filename.substr(0, dot);
}

/// "<name> was removed".
std::string removed_message(std::string_view name) {
    std::string text;
    text.reserve(name.size() + 12);
    text.append(name);
    text.append(" was removed");
    return text;
}

/// A script error that names the collection, the record and the field.
std::string
prefixed(Kind kind, std::string_view record, std::string_view key, std::string_view detail) {
    std::string text;
    text.reserve(detail.size() + record.size() + key.size() + 16);
    text.append(layer_name(kind));
    text.push_back(' ');
    text.append(record);
    text.push_back(' ');
    text.append(key);
    text.append(": ");
    text.append(detail);
    return text;
}

/// Reports whether field text starts as a TDF number.
///
/// After spaces and tabs, an optional sign and at least one digit. A leading
/// point is not a number here.
bool starts_as_number(const char* text) noexcept {
    if (text == nullptr)
        return false;
    const auto* cursor = reinterpret_cast<const unsigned char*>(text);
    while (*cursor == ' ' || *cursor == '\t')
        ++cursor;
    if (*cursor == '+' || *cursor == '-')
        ++cursor;
    return *cursor >= '0' && *cursor <= '9';
}

/// The name stored after a record handle.
std::string_view name_of(const RecordBox* box) noexcept {
    const auto* bytes = reinterpret_cast<const char*>(box) + sizeof(RecordBox);
    return std::string_view(bytes, box->name_size);
}

/// Charges one key-collecting walk. May raise. No C++ destructor is alive.
void charge_scan(lua_State* state) {
    host::charge_work(state, host::scan_call_charge);
}

/// Charges `visits` elements in one step. May raise. No C++ destructor is alive.
void charge_visits(lua_State* state, uint64_t visits) {
    if (host::element_charge != 0 && visits > UINT64_MAX / host::element_charge)
        visits = UINT64_MAX / host::element_charge;
    host::charge_work(state, visits * host::element_charge);
}

/// Charges the bytes of one key or name. May raise. No C++ destructor is alive.
void charge_text(lua_State* state, std::size_t bytes) {
    host::charge_work(state, host::units_for_bytes(bytes));
}

/// `left * right`, or the largest count when the product does not fit.
uint64_t saturating_product(std::size_t left, std::size_t right) {
    const uint64_t wide_left = static_cast<uint64_t>(left);
    const uint64_t wide_right = static_cast<uint64_t>(right);
    if (wide_left != 0 && wide_right > UINT64_MAX / wide_left)
        return UINT64_MAX;
    return wide_left * wide_right;
}

/// The layout the collections read, or the layout in force.
const defs::DataLayout& layout_of(const Collections& self) noexcept {
    if (CollectionsAccess::layout_ptr(self) != nullptr)
        return *CollectionsAccess::layout_ptr(self);
    return defs::data_layout();
}

/// True when the current mutator may change this collection.
bool ensure_grant(const Collections& self, Kind kind, std::string& reason) {
    if (!CollectionsAccess::active(self)) {
        reason = "no mutator has begun";
        return false;
    }
    const WriteGrant& grant = CollectionsAccess::grant(self);
    const bool allowed = kind == Kind::units     ? grant.units
                         : kind == Kind::weapons ? grant.weapons
                                                 : grant.features;
    if (allowed)
        return true;
    reason = "this mutator does not declare the ";
    reason.append(layer_name(kind));
    reason.append(" layer");
    return false;
}

/// Installs a protected metatable. An existing one is left as it is.
void ensure_meta(
    lua_State* state,
    const char* name,
    lua_CFunction index,
    lua_CFunction newindex,
    lua_CFunction length,
    lua_CFunction pairs,
    lua_CFunction collect
) {
    if (luaL_newmetatable(state, name) == 0) {
        lua_pop(state, 1);
        return;
    }
    if (index != nullptr) {
        lua_pushcfunction(state, index);
        lua_setfield(state, -2, "__index");
    }
    if (newindex != nullptr) {
        lua_pushcfunction(state, newindex);
        lua_setfield(state, -2, "__newindex");
    }
    if (length != nullptr) {
        lua_pushcfunction(state, length);
        lua_setfield(state, -2, "__len");
    }
    if (pairs != nullptr) {
        lua_pushcfunction(state, pairs);
        lua_setfield(state, -2, "__pairs");
    }
    if (collect != nullptr) {
        lua_pushcfunction(state, collect);
        lua_setfield(state, -2, "__gc");
    }
    lua_pushboolean(state, 0);
    lua_setfield(state, -2, "__metatable");
    lua_pop(state, 1);
}

/// Raises the owner's pending error. Call it only after C++ objects in the
/// frame are gone, and never from inside a catch block.
int raise_pending(lua_State* state, Collections& owner) {
    std::string& text = pending_of(owner);
    lua_pushlstring(state, text.data(), text.size());
    text.clear();
    return lua_error(state);
}

/// The unit name a section has now: UnitName, or the file stem.
std::string unit_name_of(const tdf::Block* section, std::string_view path) {
    const char* unit_name = section != nullptr ? tdf::find_value(section, "UnitName") : nullptr;
    if (unit_name != nullptr && unit_name[0] != '\0')
        return unit_name;
    return file_stem(path);
}

/// Forgets a built index. The next lookup builds it again.
void mark_dirty(Collections& self, Kind kind) {
    working(self).indexes[static_cast<std::size_t>(kind)].ready = false;
}

/// Remembers a section and returns its slot. The same block keeps its slot.
///
/// The lookup is a map search, so a walk costs the same however many
/// sections were remembered before it.
uint32_t remember(Collections& self, uint32_t doc_index, tdf::Block* section) {
    CollectionsState& scratch = working(self);
    const Identity identity{doc_index, section};
    const auto known = scratch.slots.find(identity);
    if (known != scratch.slots.end())
        return known->second;
    if (scratch.identities.size() >= static_cast<std::size_t>(std::numeric_limits<uint32_t>::max()))
        throw std::bad_alloc();
    const auto slot = static_cast<uint32_t>(scratch.identities.size());
    const auto placed = scratch.slots.emplace(identity, slot).first;
    try {
        scratch.identities.push_back(identity);
    } catch (const std::bad_alloc&) {
        scratch.slots.erase(placed);
        throw;
    }
    return slot;
}

/// Lists the records of one collection, in order.
std::vector<Rec>
list_records(rules::DocumentSet& documents, const defs::DataLayout& layout, Kind kind) {
    std::string folder;
    std::string suffix;
    bool recursive = false;
    if (kind == Kind::units) {
        folder = layout.directories[static_cast<std::size_t>(defs::DataDirectory::units)];
        suffix = layout.unit_extension;
    } else if (kind == Kind::weapons) {
        folder = layout.directories[static_cast<std::size_t>(defs::DataDirectory::weapons)];
        suffix = "tdf";
    } else {
        folder = "features";
        suffix = "tdf";
        recursive = true;
    }
    const std::vector<std::size_t> docs = documents.documents_in(folder, suffix, recursive);
    std::vector<Rec> records;
    for (const std::size_t doc_index : docs) {
        if (doc_index > static_cast<std::size_t>(std::numeric_limits<uint32_t>::max()))
            continue;
        rules::LoadedDocument& loaded = documents.at(doc_index);
        tdf::Document* document = loaded.document.get();
        tdf::Block* root = tdf::editable_root(document);
        if (root == nullptr)
            continue;
        const uint32_t sections = tdf::child_count(root);
        if (kind == Kind::units) {
            for (uint32_t section = 0; section < sections; ++section) {
                tdf::Block* block = tdf::editable_child(root, section);
                if (block == nullptr || block->name == nullptr)
                    continue;
                if (tdf::compare_nocase(block->name, "UNITINFO") != 0)
                    continue;
                Rec rec;
                rec.doc_index = static_cast<uint32_t>(doc_index);
                rec.section = block;
                rec.ordinal = 0;
                rec.path = loaded.path;
                rec.name = unit_name_of(block, loaded.path);
                records.push_back(std::move(rec));
                break;
            }
            continue;
        }
        std::vector<std::string> seen_names;
        for (uint32_t section = 0; section < sections; ++section) {
            tdf::Block* block = tdf::editable_child(root, section);
            if (block == nullptr || block->name == nullptr)
                continue;
            Rec rec;
            rec.doc_index = static_cast<uint32_t>(doc_index);
            rec.section = block;
            rec.path = loaded.path;
            rec.name = block->name;
            uint32_t ordinal = 0;
            for (const std::string& earlier : seen_names) {
                if (same_text(earlier, rec.name))
                    ++ordinal;
            }
            rec.ordinal = ordinal;
            seen_names.push_back(rec.name);
            records.push_back(std::move(rec));
        }
    }
    std::sort(records.begin(), records.end(), rec_before);
    return records;
}

/// The records of one collection, each with its slot. A removal builds them
/// again; a UnitName change moves one entry (`rekey_if_renamed`).
const std::vector<Rec>& ensure_index(Collections& self, Kind kind) {
    CollectionsState::Index& index = working(self).indexes[static_cast<std::size_t>(kind)];
    if (index.ready)
        return index.records;
    std::vector<Rec> built =
        list_records(CollectionsAccess::documents(self), layout_of(self), kind);
    for (Rec& rec : built)
        rec.slot = remember(self, rec.doc_index, rec.section);
    index.records.swap(built);
    index.ready = true;
    return index.records;
}

/// The first record of this name, in collection order, or null.
const Rec* find_named(Collections& self, Kind kind, std::string_view key) {
    const std::vector<Rec>& records = ensure_index(self, kind);
    const auto found = std::lower_bound(
        records.begin(), records.end(), key, [](const Rec& rec, std::string_view sought) {
            return compare_folded(rec.name, sought) < 0;
        }
    );
    if (found == records.end() || !same_text(found->name, key))
        return nullptr;
    return &(*found);
}

/// Pushes a record handle. The name is copied into the userdata.
void push_record(
    lua_State* state,
    Collections* owner,
    Kind kind,
    bool damage,
    uint32_t slot,
    std::string_view name
) {
    charge_text(state, name.size());
    const std::size_t bytes = sizeof(RecordBox) + name.size();
    void* memory = lua_newuserdatauv(state, bytes, 0);
    auto* box = static_cast<RecordBox*>(memory);
    box->owner = owner;
    box->kind = kind;
    box->damage = damage ? static_cast<uint8_t>(1) : static_cast<uint8_t>(0);
    box->slot = slot;
    box->name_size = static_cast<uint32_t>(name.size());
    if (!name.empty())
        std::memcpy(reinterpret_cast<char*>(memory) + sizeof(RecordBox), name.data(), name.size());
    luaL_setmetatable(state, kRecordMeta);
}

/// Pushes a value prepared on the collections object.
void push_prepared(lua_State* state, const Prepared& value) {
    switch (value.kind) {
    case fields::Value::Kind::absent:
        lua_pushnil(state);
        return;
    case fields::Value::Kind::integer:
        if (value.integer >= static_cast<int64_t>(std::numeric_limits<lua_Integer>::min()) &&
            value.integer <= static_cast<int64_t>(std::numeric_limits<lua_Integer>::max()))
            lua_pushinteger(state, static_cast<lua_Integer>(value.integer));
        else
            lua_pushnumber(state, static_cast<double>(value.integer));
        return;
    case fields::Value::Kind::number:
        lua_pushnumber(state, value.number);
        return;
    case fields::Value::Kind::boolean:
        lua_pushboolean(state, value.flag ? 1 : 0);
        return;
    case fields::Value::Kind::text:
        lua_pushlstring(state, value.text.data(), value.text.size());
        return;
    }
}

/// Reads a Lua value into `stored`.
bool read_stored(lua_State* state, int index, Stored& stored, std::string& reason) {
    const int type = lua_type(state, index);
    if (type == LUA_TNIL) {
        stored.kind = StoredKind::nil_value;
        return true;
    }
    if (type == LUA_TBOOLEAN) {
        stored.kind = StoredKind::boolean;
        stored.flag = lua_toboolean(state, index) != 0;
        return true;
    }
    if (type == LUA_TNUMBER) {
        stored.kind = StoredKind::number;
        stored.number = lua_tonumber(state, index);
        return true;
    }
    if (type == LUA_TSTRING) {
        std::size_t length = 0;
        const char* text = lua_tolstring(state, index, &length);
        stored.kind = StoredKind::text;
        stored.text.assign(text != nullptr ? text : "", length);
        return true;
    }
    reason = "a ";
    const char* type_name = lua_typename(state, type);
    reason.append(type_name != nullptr ? type_name : "value");
    reason.append(" cannot be stored");
    return false;
}

/// Finds the handle's section again. A missing section sets `reason`.
bool resolve(
    Collections& self,
    const RecordBox& box,
    std::string_view message_name,
    Live& live,
    std::string& reason
) {
    CollectionsState& scratch = working(self);
    if (box.slot >= scratch.identities.size()) {
        reason = removed_message(message_name);
        return false;
    }
    const Identity& identity = scratch.identities[box.slot];
    rules::DocumentSet& documents = CollectionsAccess::documents(self);
    if (identity.section == nullptr ||
        identity.doc_index >= static_cast<uint32_t>(documents.size())) {
        reason = removed_message(message_name);
        return false;
    }
    rules::LoadedDocument& loaded = documents.at(identity.doc_index);
    if (loaded.removed) {
        reason = removed_message(message_name);
        return false;
    }
    tdf::Document* document = loaded.document.get();
    tdf::Block* root = tdf::editable_root(document);
    if (root == nullptr) {
        reason = removed_message(message_name);
        return false;
    }
    const uint32_t sections = tdf::child_count(root);
    uint32_t found_index = sections;
    for (uint32_t section = 0; section < sections; ++section) {
        if (tdf::editable_child(root, section) == identity.section) {
            found_index = section;
            break;
        }
    }
    if (found_index == sections) {
        reason = removed_message(message_name);
        return false;
    }
    live.kind = box.kind;
    live.damage = box.damage != 0;
    live.doc_index = identity.doc_index;
    live.section_index = found_index;
    live.path = loaded.path;
    live.document = document;
    live.section = identity.section;
    live.block = identity.section;
    if (live.kind == Kind::units)
        live.name = unit_name_of(identity.section, loaded.path);
    else if (identity.section->name != nullptr)
        live.name = identity.section->name;
    else
        live.name.clear();
    if (live.damage)
        live.block = tdf::editable_child(identity.section, "DAMAGE");
    return true;
}

/// The stored spelling and text of one key, or the trimmed spelling.
FieldNow field_now(const tdf::Block* block, std::string_view key) {
    const std::string_view trimmed = trim_view(key);
    FieldNow now;
    now.key.assign(trimmed);
    if (block == nullptr)
        return now;
    const uint32_t count = tdf::property_count(block);
    for (uint32_t index = 0; index < count; ++index) {
        const auto place = static_cast<int32_t>(index);
        const char* stored = tdf::property_key_at(block, place);
        if (stored == nullptr || !same_text(stored, trimmed))
            continue;
        now.present = true;
        now.key = stored;
        const char* value = tdf::property_value_at(block, place);
        now.text =
            value != nullptr ? std::optional<std::string>(value) : std::optional<std::string>("");
        break;
    }
    return now;
}

/// The tally for this mutator name, counted across the collections object's life.
std::size_t tally_index(Collections& self) {
    CollectionsState& scratch = working(self);
    const std::string& mutator = CollectionsAccess::mutator(self);
    for (std::size_t index = 0; index < scratch.tallies.size(); ++index) {
        if (scratch.tallies[index].mutator == mutator)
            return index;
    }
    scratch.tallies.push_back(CollectionsState::Tally{mutator, 0, 0});
    return scratch.tallies.size() - 1;
}

/// Refuses a write or a note that would pass the per-mutator limit.
bool room_for(Collections& self, uint32_t more_writes, uint32_t more_notes, std::string& reason) {
    const CollectionsState::Tally& tally = working(self).tallies[tally_index(self)];
    if (more_writes > 0 && tally.writes > mutator_write_limit - more_writes) {
        reason = "this mutator has changed too many fields";
        return false;
    }
    if (more_notes > 0 && tally.notes > mutator_note_limit - more_notes) {
        reason = "this mutator has made too many notes";
        return false;
    }
    return true;
}

/// Counts a write and its notes against the mutator's limit.
void commit_tally(Collections& self, uint32_t more_writes, uint32_t more_notes) {
    CollectionsState::Tally& tally = working(self).tallies[tally_index(self)];
    tally.writes += more_writes;
    tally.notes += more_notes;
}

/// Names every other mutator that already wrote this field, in first-seen order.
std::vector<std::string> earlier_writers(
    Collections& self,
    std::string_view path,
    std::string_view record,
    std::string_view block,
    std::string_view key
) {
    WriterKey writer_key;
    writer_key.path = fold_copy(path);
    writer_key.record = fold_copy(record);
    writer_key.block = fold_copy(block);
    writer_key.key = fold_copy(key);
    std::vector<std::string> earlier;
    const auto found = working(self).writers.find(writer_key);
    if (found == working(self).writers.end())
        return earlier;
    const std::string& current = CollectionsAccess::mutator(self);
    for (const std::string& name : found->second) {
        if (name != current)
            earlier.push_back(name);
    }
    return earlier;
}

/// Records that this mutator wrote the field, keeping the first-seen order.
void remember_writer(
    Collections& self,
    std::string_view path,
    std::string_view record,
    std::string_view block,
    std::string_view key
) {
    WriterKey writer_key;
    writer_key.path = fold_copy(path);
    writer_key.record = fold_copy(record);
    writer_key.block = fold_copy(block);
    writer_key.key = fold_copy(key);
    std::vector<std::string>& names = working(self).writers[writer_key];
    const std::string& current = CollectionsAccess::mutator(self);
    for (const std::string& name : names) {
        if (name == current)
            return;
    }
    names.push_back(current);
}

/// Records one write, then a warning or an overwrite when either applies.
void journal_write(
    Collections& self,
    const Live& live,
    const std::string& key,
    std::optional<std::string> old_text,
    std::optional<std::string> new_text,
    const fields::Check& check,
    const std::vector<std::string>& earlier
) {
    const std::string block = live.damage ? "DAMAGE" : "";
    rules::FieldWrite write;
    write.mutators.push_back(CollectionsAccess::mutator(self));
    write.path = live.path;
    write.record = live.name;
    write.block = block;
    write.key = key;
    write.old_text = std::move(old_text);
    write.new_text = std::move(new_text);
    CollectionsAccess::writes(self).push_back(std::move(write));
    uint32_t notes = 0;
    if (check.verdict == fields::Verdict::warn) {
        HookNote note;
        note.kind = HookNoteKind::check_warned;
        note.mutators.push_back(CollectionsAccess::mutator(self));
        note.path = live.path;
        note.record = live.name;
        note.key = key;
        note.message = check.reason;
        CollectionsAccess::notes(self).push_back(std::move(note));
        ++notes;
    }
    if (!earlier.empty()) {
        HookNote note;
        note.kind = HookNoteKind::hook_overwrote;
        note.mutators = earlier;
        note.mutators.push_back(CollectionsAccess::mutator(self));
        note.path = live.path;
        note.record = live.name;
        note.key = key;
        note.message = CollectionsAccess::mutator(self);
        note.message.append(" replaced the value written by ");
        note.message.append(earlier[0]);
        for (std::size_t index = 1; index < earlier.size(); ++index) {
            note.message.append(" and ");
            note.message.append(earlier[index]);
        }
        CollectionsAccess::notes(self).push_back(std::move(note));
        ++notes;
    }
    remember_writer(self, live.path, live.name, block, key);
    commit_tally(self, 1, notes);
}

/// Moves a renamed unit's entry to its place under the new name.
///
/// `live` was resolved before the write, so `live.name` is the name the
/// index holds. The other entries keep their order, and the result is the
/// order a rebuild would give. An entry that cannot be found, or a failure
/// part way, leaves the index to be built again on the next lookup.
void rekey_if_renamed(Collections& self, const Live& live, std::string_view key) {
    if (live.kind != Kind::units || live.damage || !same_text(key, "UnitName"))
        return;
    CollectionsState::Index& index = working(self).indexes[static_cast<std::size_t>(Kind::units)];
    if (!index.ready)
        return;
    index.ready = false;
    std::string renamed;
    try {
        renamed = unit_name_of(live.section, live.path);
    } catch (const std::bad_alloc&) {
        return;
    }
    std::vector<Rec>& records = index.records;
    auto entry = std::lower_bound(
        records.begin(),
        records.end(),
        std::string_view(live.name),
        [](const Rec& rec, std::string_view sought) { return compare_folded(rec.name, sought) < 0; }
    );
    while (entry != records.end() && same_text(entry->name, live.name) &&
           (entry->doc_index != live.doc_index || entry->section != live.section))
        ++entry;
    if (entry == records.end() || !same_text(entry->name, live.name))
        return;
    entry->name.swap(renamed);
    if (entry != records.begin() && rec_before(*entry, *(entry - 1))) {
        const auto place = std::upper_bound(records.begin(), entry, *entry, rec_before);
        std::rotate(place, entry, entry + 1);
    } else {
        const auto place = std::lower_bound(entry + 1, records.end(), *entry, rec_before);
        std::rotate(entry, entry + 1, place);
    }
    index.ready = true;
}

/// Writes one stored value. The grant has already been checked.
bool write_stored(
    Collections& self, Live& live, std::string_view key, const Stored& stored, std::string& reason
) {
    const std::string trimmed(trim_view(key));
    const FieldNow before = field_now(live.block, trimmed);
    const std::string block_name = live.damage ? "DAMAGE" : "";
    if (stored.kind == StoredKind::nil_value) {
        if (!before.present || live.block == nullptr)
            return true;
        const std::vector<std::string> earlier =
            earlier_writers(self, live.path, live.name, block_name, before.key);
        const uint32_t notes = earlier.empty() ? 0 : 1;
        if (!room_for(self, 1, notes, reason))
            return false;
        const tdf::EditStatus status = tdf::remove_key(live.document, live.block, before.key);
        if (status != tdf::EditStatus::ok) {
            reason = prefixed(live.kind, live.name, trimmed, tdf::edit_status_message(status));
            return false;
        }
        journal_write(self, live, before.key, before.text, std::nullopt, {}, earlier);
        rekey_if_renamed(self, live, trimmed);
        return true;
    }

    fields::Check check;
    std::string text;
    const std::optional<fields::RecordKind> schema = schema_of(live);
    if (stored.kind == StoredKind::number) {
        if (!std::isfinite(stored.number)) {
            if (schema.has_value())
                check = fields::check_number(*schema, trimmed, stored.number);
            if (check.verdict == fields::Verdict::refuse)
                reason = prefixed(live.kind, live.name, trimmed, check.reason);
            else {
                const char* word = std::isnan(stored.number) ? "NaN" : "infinity";
                reason = prefixed(
                    live.kind,
                    live.name,
                    trimmed,
                    std::string(word) +
                        " is not a finite number; a value that is not finite cannot be stored"
                );
            }
            return false;
        }
        if (schema.has_value())
            check = fields::check_number(*schema, trimmed, stored.number);
        if (check.verdict == fields::Verdict::refuse) {
            reason = prefixed(live.kind, live.name, trimmed, check.reason);
            return false;
        }
        const std::optional<std::string> spelled = tdf::format_number(stored.number);
        if (!spelled.has_value()) {
            reason = prefixed(
                live.kind, live.name, trimmed, "a value that is not finite cannot be stored"
            );
            return false;
        }
        text = *spelled;
    } else if (stored.kind == StoredKind::boolean) {
        const double number = stored.flag ? 1.0 : 0.0;
        if (schema.has_value())
            check = fields::check_number(*schema, trimmed, number);
        if (check.verdict == fields::Verdict::refuse) {
            reason = prefixed(live.kind, live.name, trimmed, check.reason);
            return false;
        }
        text = stored.flag ? "1" : "0";
    } else {
        const std::string_view body = trim_view(stored.text);
        if (schema.has_value())
            check = fields::check_text(*schema, trimmed, body);
        if (check.verdict == fields::Verdict::refuse) {
            reason = prefixed(live.kind, live.name, trimmed, check.reason);
            return false;
        }
        text.assign(body);
    }

    if (before.present && before.text.has_value() && *before.text == text)
        return true;

    const std::string& journal_lookup = before.present ? before.key : trimmed;
    const std::vector<std::string> earlier =
        earlier_writers(self, live.path, live.name, block_name, journal_lookup);
    uint32_t notes = earlier.empty() ? 0 : 1;
    if (check.verdict == fields::Verdict::warn)
        ++notes;
    if (!room_for(self, 1, notes, reason))
        return false;

    bool created = false;
    if (live.damage && live.block == nullptr) {
        tdf::EditStatus created_status = tdf::EditStatus::ok;
        live.block = tdf::add_child(live.document, live.section, "DAMAGE", &created_status);
        if (live.block == nullptr) {
            reason =
                prefixed(live.kind, live.name, trimmed, tdf::edit_status_message(created_status));
            return false;
        }
        created = true;
    }
    const std::string write_key = before.present ? before.key : trimmed;
    const tdf::EditStatus status = tdf::set_value(live.document, live.block, write_key, text);
    if (status != tdf::EditStatus::ok) {
        if (created) {
            const uint32_t count = tdf::child_count(live.section);
            if (count > 0 &&
                tdf::remove_child(live.document, live.section, count - 1) == tdf::EditStatus::ok)
                live.block = nullptr;
        }
        reason = prefixed(live.kind, live.name, trimmed, tdf::edit_status_message(status));
        return false;
    }
    const FieldNow after = field_now(live.block, trimmed);
    const std::string journal_key = after.present ? after.key : write_key;
    const std::optional<std::string> new_text =
        after.present ? after.text : std::optional<std::string>(text);
    journal_write(
        self,
        live,
        journal_key,
        before.present ? before.text : std::nullopt,
        new_text,
        check,
        earlier
    );
    rekey_if_renamed(self, live, trimmed);
    return true;
}

/// Drops download sections that name `unit_name`, and journals each one.
bool remove_download(Collections& self, std::string_view unit_name, std::string& reason) {
    const defs::DataLayout& layout = layout_of(self);
    const std::string& folder =
        layout.directories[static_cast<std::size_t>(defs::DataDirectory::download)];
    const std::vector<std::size_t> docs =
        CollectionsAccess::documents(self).documents_in(folder, "tdf", false);
    for (const std::size_t doc_index : docs) {
        rules::LoadedDocument& loaded = CollectionsAccess::documents(self).at(doc_index);
        tdf::Document* document = loaded.document.get();
        tdf::Block* root = tdf::editable_root(document);
        if (root == nullptr)
            continue;
        const uint32_t count = tdf::child_count(root);
        std::vector<uint32_t> matched;
        std::vector<std::string> names;
        bool first_matched = false;
        for (uint32_t index = 0; index < count; ++index) {
            tdf::Block* section = tdf::editable_child(root, index);
            if (section == nullptr)
                continue;
            const char* unit = tdf::find_value(section, "UNITNAME");
            const char* menu = tdf::find_value(section, "UNITMENU");
            const bool names_unit = (unit != nullptr && same_text(unit, unit_name)) ||
                                    (menu != nullptr && same_text(menu, unit_name));
            if (!names_unit)
                continue;
            matched.push_back(index);
            names.emplace_back(section->name != nullptr ? section->name : "");
            if (index == 0)
                first_matched = true;
        }
        for (std::size_t index = matched.size(); index > 0; --index) {
            const tdf::EditStatus status = tdf::remove_child(document, root, matched[index - 1]);
            if (status != tdf::EditStatus::ok) {
                reason =
                    prefixed(Kind::units, unit_name, "UNITNAME", tdf::edit_status_message(status));
                return false;
            }
        }
        for (const std::string& section_name : names) {
            RecordRemoval removal;
            removal.mutator = CollectionsAccess::mutator(self);
            removal.kind = RemovalKind::download_entry;
            removal.name = section_name;
            removal.path = loaded.path;
            CollectionsAccess::removals(self).push_back(std::move(removal));
        }
        if (tdf::child_count(root) == 0) {
            loaded.removed = true;
            RecordRemoval removal;
            removal.mutator = CollectionsAccess::mutator(self);
            removal.kind = RemovalKind::document;
            removal.name = file_name(loaded.path);
            removal.path = loaded.path;
            CollectionsAccess::removals(self).push_back(std::move(removal));
        } else if (first_matched) {
            if (!room_for(self, 0, 1, reason))
                return false;
            const tdf::Block* first = tdf::child_at(root, 0);
            const char* now = first != nullptr ? tdf::find_value(first, "UNITNAME") : nullptr;
            HookNote note;
            note.kind = HookNoteKind::download_first_entry_changed;
            note.mutators.push_back(CollectionsAccess::mutator(self));
            note.path = loaded.path;
            note.record = std::string(unit_name);
            note.key = "UNITNAME";
            note.message = "removing the first entry leaves ";
            if (now != nullptr && now[0] != '\0')
                note.message.append(now);
            else
                note.message.append("an entry with no UNITNAME");
            note.message.append(" as the unit this file marks");
            CollectionsAccess::notes(self).push_back(std::move(note));
            commit_tally(self, 0, 1);
        }
    }
    return true;
}

/// Removes one unit document and the download entries that name it.
bool remove_unit(Collections& self, const Live& live, std::string& reason) {
    if (live.doc_index >= CollectionsAccess::documents(self).size()) {
        reason = removed_message(live.name);
        return false;
    }
    rules::LoadedDocument& loaded = CollectionsAccess::documents(self).at(live.doc_index);
    if (loaded.removed) {
        reason = removed_message(live.name);
        return false;
    }
    loaded.removed = true;
    mark_dirty(self, Kind::units);
    RecordRemoval removal;
    removal.mutator = CollectionsAccess::mutator(self);
    removal.kind = RemovalKind::unit;
    removal.name = live.name;
    removal.path = live.path;
    CollectionsAccess::removals(self).push_back(std::move(removal));
    return remove_download(self, live.name, reason);
}

/// Removes weapon or feature sections. Indices are taken before any removal.
bool remove_sections(
    Collections& self, Kind kind, const std::vector<Live>& lives, std::string& reason
) {
    mark_dirty(self, kind);
    std::vector<SectionDrop> drops;
    drops.reserve(lives.size());
    for (const Live& live : lives) {
        SectionDrop drop;
        drop.doc_index = live.doc_index;
        drop.section_index = live.section_index;
        drop.name = live.name;
        drop.path = live.path;
        drops.push_back(std::move(drop));
    }
    std::vector<uint32_t> doc_ids;
    for (const SectionDrop& drop : drops) {
        bool seen = false;
        for (const uint32_t doc_index : doc_ids) {
            if (doc_index == drop.doc_index)
                seen = true;
        }
        if (!seen)
            doc_ids.push_back(drop.doc_index);
    }
    for (const uint32_t doc_index : doc_ids) {
        if (doc_index >= CollectionsAccess::documents(self).size())
            continue;
        const uint32_t children =
            tdf::child_count(CollectionsAccess::documents(self).at(doc_index).document.root());
        uint32_t removing = 0;
        std::size_t last = 0;
        bool any = false;
        for (std::size_t index = 0; index < drops.size(); ++index) {
            if (drops[index].doc_index != doc_index)
                continue;
            ++removing;
            last = index;
            any = true;
        }
        if (any && removing == children)
            drops[last].journal_document = true;
    }
    std::vector<std::size_t> order(drops.size());
    for (std::size_t index = 0; index < order.size(); ++index)
        order[index] = index;
    std::sort(order.begin(), order.end(), [&](std::size_t left, std::size_t right) {
        const SectionDrop& a = drops[left];
        const SectionDrop& b = drops[right];
        if (a.doc_index != b.doc_index)
            return a.doc_index < b.doc_index;
        return a.section_index > b.section_index;
    });
    for (const std::size_t index : order) {
        const SectionDrop& drop = drops[index];
        if (drop.doc_index >= CollectionsAccess::documents(self).size()) {
            reason = removed_message(drop.name);
            return false;
        }
        rules::LoadedDocument& loaded = CollectionsAccess::documents(self).at(drop.doc_index);
        tdf::Block* root = tdf::editable_root(loaded.document.get());
        const tdf::EditStatus status =
            tdf::remove_child(loaded.document.get(), root, drop.section_index);
        if (status != tdf::EditStatus::ok) {
            reason = prefixed(kind, drop.name, "", tdf::edit_status_message(status));
            return false;
        }
    }
    for (const uint32_t doc_index : doc_ids) {
        if (doc_index >= CollectionsAccess::documents(self).size())
            continue;
        rules::LoadedDocument& loaded = CollectionsAccess::documents(self).at(doc_index);
        if (tdf::child_count(loaded.document.root()) == 0)
            loaded.removed = true;
    }
    for (const SectionDrop& drop : drops) {
        RecordRemoval removal;
        removal.mutator = CollectionsAccess::mutator(self);
        removal.kind = kind == Kind::weapons ? RemovalKind::weapon : RemovalKind::feature;
        removal.name = drop.name;
        removal.path = drop.path;
        CollectionsAccess::removals(self).push_back(std::move(removal));
        if (!drop.journal_document)
            continue;
        RecordRemoval document;
        document.mutator = CollectionsAccess::mutator(self);
        document.kind = RemovalKind::document;
        document.name = file_name(drop.path);
        document.path = drop.path;
        CollectionsAccess::removals(self).push_back(std::move(document));
    }
    return true;
}

/// Removes every live record. The grant has already been checked.
bool remove_lives(
    Collections& self, Kind kind, const std::vector<Live>& lives, std::string& reason
) {
    if (kind == Kind::units) {
        for (const Live& live : lives) {
            if (!remove_unit(self, live, reason))
                return false;
        }
        return true;
    }
    return remove_sections(self, kind, lives, reason);
}

/// Copies a record userdata into a handle the removal path can keep.
Handle handle_from(const RecordBox& box) {
    Handle handle;
    handle.kind = box.kind;
    handle.damage = box.damage != 0;
    handle.slot = box.slot;
    handle.name = std::string(name_of(&box));
    return handle;
}

/// The record userdata at `index`, or null.
const RecordBox* record_at(lua_State* state, int index) {
    return static_cast<const RecordBox*>(luaL_testudata(state, index, kRecordMeta));
}

const ViewBox* view_at(lua_State* state, int index, const char* meta) {
    return static_cast<const ViewBox*>(luaL_testudata(state, index, meta));
}

/// True when `key` is one of the four record methods, spelled exactly.
bool record_method(std::string_view key) noexcept {
    return key == "remove" || key == "get" || key == "set" || key == "keys";
}

/// Copies the records of a set, or the one record a method was called on.
bool collect_handles(lua_State* state, Collections& owner, int index) {
    CollectionsState& scratch = working(owner);
    scratch.handles.clear();
    if (const RecordBox* box = record_at(state, index)) {
        try {
            scratch.handles.push_back(handle_from(*box));
        } catch (const std::bad_alloc&) {
            scratch.pending_error = "out of memory";
            return false;
        }
        return true;
    }
    if (view_at(state, index, kSetMeta) == nullptr) {
        scratch.pending_error = "remove expects a record or a set";
        return false;
    }
    if (lua_getiuservalue(state, index, 1) != LUA_TTABLE) {
        lua_pop(state, 1);
        scratch.pending_error = "the set has no records";
        return false;
    }
    const int sequence = lua_absindex(state, -1);
    const lua_Unsigned length = lua_rawlen(state, sequence);
    charge_scan(state);
    for (lua_Unsigned slot = 1; slot <= length; ++slot) {
        charge_visits(state, 1);
        lua_rawgeti(state, sequence, static_cast<lua_Integer>(slot));
        const RecordBox* box = record_at(state, -1);
        if (box == nullptr) {
            lua_pop(state, 2);
            scratch.pending_error = "the set holds a value that is not a record";
            return false;
        }
        try {
            scratch.handles.push_back(handle_from(*box));
        } catch (const std::bad_alloc&) {
            lua_pop(state, 2);
            scratch.pending_error = "out of memory";
            return false;
        }
        lua_pop(state, 1);
    }
    lua_pop(state, 1);
    return true;
}

/// Reads the host's key sequence into the collections object. Keys stay raw.
bool read_keys(lua_State* state, Collections& self, int key_count, const char* type_error) {
    CollectionsState& scratch = working(self);
    scratch.keys.clear();
    const int sequence = lua_absindex(state, -1);
    charge_scan(state);
    for (int slot = 1; slot <= key_count; ++slot) {
        charge_visits(state, 1);
        lua_rawgeti(state, sequence, slot);
        if (lua_type(state, -1) != LUA_TSTRING) {
            lua_pop(state, 1);
            scratch.pending_error = type_error;
            return false;
        }
        std::size_t length = 0;
        const char* text = lua_tolstring(state, -1, &length);
        charge_text(state, length);
        try {
            scratch.keys.emplace_back(text != nullptr ? text : "", length);
        } catch (const std::bad_alloc&) {
            lua_pop(state, 1);
            scratch.pending_error = "out of memory";
            return false;
        }
        lua_pop(state, 1);
    }
    return true;
}

/// The owner of the collection or set at index 1, or null.
Collections* view_owner(lua_State* state, const char* meta) {
    const ViewBox* box = view_at(state, 1, meta);
    if (box == nullptr)
        return nullptr;
    return box->owner;
}

/// The owner of the collection or the set at index 1, or null.
Collections* any_view_owner(lua_State* state) {
    if (Collections* owner = view_owner(state, kSetMeta))
        return owner;
    return view_owner(state, kCollectionMeta);
}

/// Builds a set from the field-name sequence `where` pushed. The collection
/// is at index 1 and the needs table at index 2.
int where_from_keys(lua_State* state, Collections& owner, Kind kind, int key_count) {
    if (!read_keys(state, owner, key_count, "where: a field name must be a string"))
        return -1;
    CollectionsState& scratch = working(owner);
    scratch.needs.clear();
    const int table = 2;
    for (std::size_t index = 0; index < scratch.keys.size(); ++index) {
        charge_visits(state, 1);
        const std::string& field = scratch.keys[index];
        charge_text(state, field.size());
        lua_pushlstring(state, field.data(), field.size());
        lua_rawget(state, table);
        if (!need_from_lua(state, -1, trim_view(field), scratch.staging, &scratch.pending_error)) {
            lua_pop(state, 1);
            return -1;
        }
        try {
            scratch.needs.push_back(std::move(scratch.staging));
        } catch (const std::bad_alloc&) {
            lua_pop(state, 1);
            scratch.pending_error = "out of memory";
            return -1;
        }
        lua_pop(state, 1);
    }
    const std::vector<Rec>* indexed = nullptr;
    try {
        indexed = &ensure_index(owner, kind);
        scratch.matched.clear();
    } catch (const std::bad_alloc&) {
        scratch.pending_error = "out of memory";
        return -1;
    }
    if (indexed == nullptr) {
        scratch.pending_error = "out of memory";
        return -1;
    }
    const std::vector<Rec>& records = *indexed;
    // The match loops are C++. Charge them before any string is built, so a
    // spent allowance raises with no destructor alive.
    charge_scan(state);
    charge_visits(state, static_cast<uint64_t>(records.size()));
    charge_visits(state, saturating_product(records.size(), scratch.needs.size()));
    try {
        for (std::size_t index = 0; index < records.size(); ++index) {
            bool keep = true;
            for (const sm::Need& need : scratch.needs) {
                if (!rules::need_met(records[index].section, target_kind(kind), need)) {
                    keep = false;
                    break;
                }
            }
            if (!keep)
                continue;
            if (index > static_cast<std::size_t>(std::numeric_limits<uint32_t>::max())) {
                scratch.pending_error = "too many records";
                return -1;
            }
            if (records[index].name.size() >
                static_cast<std::size_t>(std::numeric_limits<uint32_t>::max())) {
                scratch.pending_error = "a record name is too long";
                return -1;
            }
            scratch.matched.push_back(static_cast<uint32_t>(index));
        }
    } catch (const std::bad_alloc&) {
        scratch.pending_error = "out of memory";
        return -1;
    }
    if (scratch.matched.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        scratch.pending_error = "too many records";
        return -1;
    }
    lua_createtable(state, static_cast<int>(scratch.matched.size()), 0);
    for (std::size_t index = 0; index < scratch.matched.size(); ++index) {
        const Rec& rec = records[scratch.matched[index]];
        push_record(state, &owner, kind, false, rec.slot, rec.name);
        lua_rawseti(state, -2, static_cast<lua_Integer>(index) + 1);
    }
    auto* box = static_cast<ViewBox*>(lua_newuserdatauv(state, sizeof(ViewBox), 1));
    box->owner = &owner;
    box->kind = kind;
    luaL_setmetatable(state, kSetMeta);
    lua_pushvalue(state, -2);
    lua_setiuservalue(state, -2, 1);
    return 1;
}

/// Pushes the records of the collection at index 1, or the snapshot the set
/// at index 1 already holds.
int push_members(lua_State* state, Collections& owner) {
    CollectionsState& scratch = working(owner);
    if (view_at(state, 1, kSetMeta) != nullptr) {
        if (lua_getiuservalue(state, 1, 1) != LUA_TTABLE) {
            lua_pop(state, 1);
            scratch.pending_error = "the set has no records";
            return -1;
        }
        return 1;
    }
    const ViewBox* collection = view_at(state, 1, kCollectionMeta);
    if (collection == nullptr) {
        scratch.pending_error = "expected a collection";
        return -1;
    }
    const Kind kind = collection->kind;
    try {
        const std::vector<Rec>& records = ensure_index(owner, kind);
        for (const Rec& rec : records) {
            if (rec.name.size() > static_cast<std::size_t>(std::numeric_limits<uint32_t>::max())) {
                scratch.pending_error = "a record name is too long";
                return -1;
            }
        }
    } catch (const std::bad_alloc&) {
        scratch.pending_error = "out of memory";
        return -1;
    }
    const std::vector<Rec>& records = ensure_index(owner, kind);
    if (records.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        scratch.pending_error = "too many records";
        return -1;
    }
    charge_scan(state);
    charge_visits(state, static_cast<uint64_t>(records.size()));
    lua_createtable(state, static_cast<int>(records.size()), 0);
    for (std::size_t index = 0; index < records.size(); ++index) {
        push_record(state, &owner, kind, false, records[index].slot, records[index].name);
        lua_rawseti(state, -2, static_cast<lua_Integer>(index) + 1);
    }
    return 1;
}

/// Resolves a handle without a userdata name tail. The message uses `handle.name`.
bool resolve_handle(Collections& self, const Handle& handle, Live& live, std::string& reason);

/// Applies the prepared assignments. Nothing here calls into Lua.
bool apply_assignments(lua_State* state, std::string& reason) {
    const ViewBox* set = view_at(state, 1, kSetMeta);
    if (set == nullptr || set->owner == nullptr) {
        reason = "set expects a set";
        return false;
    }
    if (!ensure_grant(*set->owner, set->kind, reason))
        return false;
    CollectionsState& scratch = working(*set->owner);
    for (const Handle& handle : scratch.handles) {
        Live live;
        if (!resolve_handle(*set->owner, handle, live, reason))
            return false;
    }
    for (const Assignment& assignment : scratch.assignments) {
        for (const Handle& handle : scratch.handles) {
            Live live;
            if (!resolve_handle(*set->owner, handle, live, reason))
                return false;
            if (!write_stored(*set->owner, live, assignment.key, assignment.value, reason))
                return false;
        }
    }
    return true;
}

/// Resolves a handle without a userdata name tail. The message uses `handle.name`.
bool resolve_handle(Collections& self, const Handle& handle, Live& live, std::string& reason) {
    RecordBox box{};
    box.owner = &self;
    box.kind = handle.kind;
    box.damage = handle.damage ? static_cast<uint8_t>(1) : static_cast<uint8_t>(0);
    box.slot = handle.slot;
    box.name_size = static_cast<uint32_t>(handle.name.size());
    return resolve(self, box, handle.name, live, reason);
}

/// Scales the present numeric fields of the set at index 1.
bool apply_scale(lua_State* state, std::string& reason) {
    const ViewBox* set = view_at(state, 1, kSetMeta);
    if (set == nullptr || set->owner == nullptr) {
        reason = "scale expects a set";
        return false;
    }
    if (!ensure_grant(*set->owner, set->kind, reason))
        return false;
    CollectionsState& scratch = working(*set->owner);
    for (const Handle& handle : scratch.handles) {
        Live live;
        if (!resolve_handle(*set->owner, handle, live, reason))
            return false;
    }
    for (const Factor& factor : scratch.factors) {
        for (const Handle& handle : scratch.handles) {
            Live live;
            if (!resolve_handle(*set->owner, handle, live, reason))
                return false;
            const FieldNow before = field_now(live.block, factor.key);
            if (!before.present || !before.text.has_value())
                continue;
            if (!starts_as_number(before.text->c_str())) {
                if (!room_for(*set->owner, 0, 1, reason))
                    return false;
                HookNote note;
                note.kind = HookNoteKind::not_a_number;
                note.mutators.push_back(CollectionsAccess::mutator(*set->owner));
                note.path = live.path;
                note.record = live.name;
                note.key = before.key;
                note.message = "the field text is not a number";
                CollectionsAccess::notes(*set->owner).push_back(std::move(note));
                commit_tally(*set->owner, 0, 1);
                continue;
            }
            const double current = tdf::parse_double(before.text->c_str());
            Stored stored;
            stored.kind = StoredKind::number;
            stored.number = current * factor.number;
            if (!write_stored(*set->owner, live, factor.key, stored, reason))
                return false;
        }
    }
    return true;
}

/// Removes the handles collected for the value at index 1.
bool remove_collected(Collections& self, Kind kind, std::string& reason) {
    if (!ensure_grant(self, kind, reason))
        return false;
    CollectionsState& scratch = working(self);
    for (const Handle& handle : scratch.handles) {
        if (handle.damage) {
            reason = "a damage block cannot be removed";
            return false;
        }
    }
    scratch.lives.clear();
    for (const Handle& handle : scratch.handles) {
        Live live;
        if (!resolve_handle(self, handle, live, reason))
            return false;
        if (live.damage) {
            reason = "a damage block cannot be removed";
            return false;
        }
        scratch.lives.push_back(std::move(live));
    }
    return remove_lives(self, kind, scratch.lives, reason);
}

/// Fills the prepared read. The trampoline pushes it after this returns.
int prepare_read(const RecordBox* box, std::string_view key) {
    std::string reason;
    Live live;
    if (!resolve(*box->owner, *box, name_of(box), live, reason)) {
        pending_of(*box->owner) = std::move(reason);
        return -1;
    }
    const std::optional<fields::RecordKind> schema = schema_of(live);
    const fields::FieldSpec* spec = nullptr;
    if (schema.has_value())
        spec = fields::find_field(*schema, key);
    const std::string owned(key);
    const char* text = nullptr;
    if (live.block != nullptr)
        text = tdf::find_value(live.block, owned.c_str());
    const fields::Value value = fields::read_value(spec, text);
    Prepared& prepared = working(*box->owner).prepared;
    prepared.kind = value.kind;
    prepared.integer = value.integer;
    prepared.number = value.number;
    prepared.flag = value.boolean;
    prepared.text = std::move(value.text);
    return 1;
}

/// Assigns one field. The value is already on the stack at index 3.
int assign_record(lua_State* state, const RecordBox* box, std::string_view key) {
    std::string reason;
    if (!ensure_grant(*box->owner, box->kind, reason)) {
        pending_of(*box->owner) = std::move(reason);
        return -1;
    }
    if (!read_stored(state, 3, working(*box->owner).stored, reason)) {
        pending_of(*box->owner) = prefixed(box->kind, name_of(box), key, reason);
        return -1;
    }
    Live live;
    if (!resolve(*box->owner, *box, name_of(box), live, reason)) {
        pending_of(*box->owner) = std::move(reason);
        return -1;
    }
    if (!write_stored(*box->owner, live, key, working(*box->owner).stored, reason)) {
        pending_of(*box->owner) = std::move(reason);
        return -1;
    }
    return 0;
}

/// Collects a record's keys into the collections object, in name order.
///
/// The charges sit outside every `try`: a spent allowance raises, and a
/// raise leaves a destructor unrun.
int prepare_keys(lua_State* state, const RecordBox* box) {
    const tdf::Block* block = nullptr;
    try {
        std::string reason;
        Live live;
        if (!resolve(*box->owner, *box, name_of(box), live, reason)) {
            pending_of(*box->owner) = std::move(reason);
            return -1;
        }
        block = live.block;
    } catch (const std::bad_alloc&) {
        pending_of(*box->owner) = "out of memory";
        return -1;
    }
    CollectionsState& scratch = working(*box->owner);
    scratch.keys.clear();
    const uint32_t property_count = block != nullptr ? tdf::property_count(block) : 0;
    charge_scan(state);
    charge_visits(state, property_count);
    for (uint32_t scan_index = 0; scan_index < property_count; ++scan_index) {
        const char* stored = tdf::property_key_at(block, static_cast<int32_t>(scan_index));
        if (stored == nullptr)
            continue;
        charge_text(state, std::strlen(stored));
    }
    try {
        for (uint32_t copy_index = 0; copy_index < property_count; ++copy_index) {
            const char* stored = tdf::property_key_at(block, static_cast<int32_t>(copy_index));
            if (stored == nullptr)
                continue;
            scratch.keys.emplace_back(stored);
        }
    } catch (const std::bad_alloc&) {
        pending_of(*box->owner) = "out of memory";
        return -1;
    }
    host::charge_work(state, host::comparison_unit * static_cast<uint64_t>(scratch.keys.size()));
    try {
        std::stable_sort(scratch.keys.begin(), scratch.keys.end(), host::name_less);
    } catch (const std::bad_alloc&) {
        pending_of(*box->owner) = "out of memory";
        return -1;
    }
    return 1;
}

/// Reports whether the record userdata still names a live section.
bool still_there(const RecordBox* box) {
    if (box == nullptr || box->owner == nullptr)
        return false;
    Live live;
    std::string reason;
    return resolve(*box->owner, *box, name_of(box), live, reason);
}

/// Writes the reason a need was refused. Returns false.
bool refuse_need(std::string* error, std::string_view field) {
    if (error == nullptr)
        return false;
    try {
        if (field.empty())
            *error = "a need must be a number, a boolean, a string or a form";
        else
            *error = std::string(field) + " is not a number, a boolean, a string or a form";
    } catch (const std::bad_alloc&) {
        try {
            *error = "out of memory";
        } catch (const std::bad_alloc&) {
        }
    }
    return false;
}

// Trampolines. A raising call happens here, after every C++ object in the frame has gone.
extern "C" {

static int form_range(lua_State* state);
static int form_is(lua_State* state);
static int form_has(lua_State* state);
static int form_absent(lua_State* state);
static int form_not(lua_State* state);
static int pairs_next(lua_State* state);
static int collection_where(lua_State* state);
static int collection_each(lua_State* state);
static int set_assign(lua_State* state);
static int set_scale(lua_State* state);
static int set_remove(lua_State* state);
static int record_call_remove(lua_State* state);
static int record_call_get(lua_State* state);
static int record_call_set(lua_State* state);
static int record_call_keys(lua_State* state);
static int collection_index(lua_State* state);
static int collection_newindex(lua_State* state);
static int collection_len(lua_State* state);
static int view_pairs(lua_State* state);
static int set_index(lua_State* state);
static int set_newindex(lua_State* state);
static int set_len(lua_State* state);
static int record_index(lua_State* state);
static int record_newindex(lua_State* state);

static int form_range(lua_State* state) {
    if (lua_gettop(state) < 2 || lua_type(state, 1) != LUA_TNUMBER ||
        lua_type(state, 2) != LUA_TNUMBER)
        return luaL_error(state, "range expects two numbers");
    void* memory = lua_newuserdatauv(state, sizeof(NeedBits), 1);
    auto* bits = static_cast<NeedBits*>(memory);
    bits->kind = static_cast<uint8_t>(sm::NeedKind::range);
    bits->negated = 0;
    bits->low = lua_tonumber(state, 1);
    bits->high = lua_tonumber(state, 2);
    lua_pushliteral(state, "");
    lua_setiuservalue(state, -2, 1);
    luaL_setmetatable(state, kNeedMeta);
    return 1;
}

static int form_is(lua_State* state) {
    if (lua_gettop(state) < 1 || lua_type(state, 1) != LUA_TSTRING)
        return luaL_error(state, "is expects a string");
    void* memory = lua_newuserdatauv(state, sizeof(NeedBits), 1);
    auto* bits = static_cast<NeedBits*>(memory);
    bits->kind = static_cast<uint8_t>(sm::NeedKind::equals);
    bits->negated = 0;
    bits->low = 0;
    bits->high = 0;
    lua_pushvalue(state, 1);
    lua_setiuservalue(state, -2, 1);
    luaL_setmetatable(state, kNeedMeta);
    return 1;
}

static int form_has(lua_State* state) {
    if (lua_gettop(state) < 1 || lua_type(state, 1) != LUA_TSTRING)
        return luaL_error(state, "has expects a string");
    void* memory = lua_newuserdatauv(state, sizeof(NeedBits), 1);
    auto* bits = static_cast<NeedBits*>(memory);
    bits->kind = static_cast<uint8_t>(sm::NeedKind::contains);
    bits->negated = 0;
    bits->low = 0;
    bits->high = 0;
    lua_pushvalue(state, 1);
    lua_setiuservalue(state, -2, 1);
    luaL_setmetatable(state, kNeedMeta);
    return 1;
}

static int form_absent(lua_State* state) {
    void* memory = lua_newuserdatauv(state, sizeof(NeedBits), 1);
    auto* bits = static_cast<NeedBits*>(memory);
    bits->kind = static_cast<uint8_t>(sm::NeedKind::contains);
    bits->negated = 1;
    bits->low = 0;
    bits->high = 0;
    lua_pushliteral(state, "");
    lua_setiuservalue(state, -2, 1);
    luaL_setmetatable(state, kNeedMeta);
    return 1;
}

static int form_not(lua_State* state) {
    const auto* existing = static_cast<const NeedBits*>(luaL_testudata(state, 1, kNeedMeta));
    if (existing == nullptr)
        return luaL_error(state, "not_ expects a need");
    if (existing->negated != 0)
        return luaL_error(state, "not_ of a not_");
    if (lua_getiuservalue(state, 1, 1) != LUA_TSTRING) {
        lua_pop(state, 1);
        lua_pushliteral(state, "");
    }
    void* memory = lua_newuserdatauv(state, sizeof(NeedBits), 1);
    auto* bits = static_cast<NeedBits*>(memory);
    bits->kind = existing->kind;
    bits->negated = 1;
    bits->low = existing->low;
    bits->high = existing->high;
    lua_pushvalue(state, -2);
    lua_setiuservalue(state, -2, 1);
    luaL_setmetatable(state, kNeedMeta);
    lua_replace(state, -2);
    return 1;
}

static int pairs_next(lua_State* state) {
    const bool skip = lua_toboolean(state, lua_upvalueindex(3)) != 0;
    for (;;) {
        charge_visits(state, 1);
        lua_settop(state, 0);
        const lua_Integer slot = lua_tointeger(state, lua_upvalueindex(2)) + 1;
        lua_pushinteger(state, slot);
        lua_replace(state, lua_upvalueindex(2));
        if (lua_rawgeti(state, lua_upvalueindex(1), slot) != LUA_TUSERDATA)
            return 1;
        const auto* box = static_cast<const RecordBox*>(lua_touserdata(state, -1));
        if (box == nullptr || box->owner == nullptr)
            return luaL_error(state, "pairs: the record is missing");
        if (skip) {
            bool alive = false;
            bool failed = false;
            try {
                alive = still_there(box);
            } catch (const std::bad_alloc&) {
                pending_of(*box->owner) = "out of memory";
                failed = true;
            }
            if (failed)
                return raise_pending(state, *box->owner);
            if (!alive) {
                lua_pop(state, 1);
                continue;
            }
        }
        if (box->name_size == 0)
            lua_pushliteral(state, "");
        else {
            const char* bytes = reinterpret_cast<const char*>(box) + sizeof(RecordBox);
            lua_pushlstring(state, bytes, box->name_size);
        }
        lua_insert(state, -2);
        return 2;
    }
}

static int collection_index(lua_State* state) {
    const ViewBox* collection = view_at(state, 1, kCollectionMeta);
    if (collection == nullptr || collection->owner == nullptr)
        return luaL_error(state, "expected a collection");
    if (lua_type(state, 2) != LUA_TSTRING) {
        lua_pushnil(state);
        return 1;
    }
    std::size_t length = 0;
    const char* text = lua_tolstring(state, 2, &length);
    const std::string_view key(text != nullptr ? text : "", length);
    if (key == "where") {
        lua_pushcfunction(state, collection_where);
        return 1;
    }
    if (key == "each") {
        lua_pushcfunction(state, collection_each);
        return 1;
    }
    Collections& owner = *collection->owner;
    const Rec* found = nullptr;
    bool failed = false;
    try {
        found = find_named(owner, collection->kind, key);
        if (found != nullptr &&
            found->name.size() > static_cast<std::size_t>(std::numeric_limits<uint32_t>::max())) {
            pending_of(owner) = "a record name is too long";
            failed = true;
        }
    } catch (const std::bad_alloc&) {
        pending_of(owner) = "out of memory";
        failed = true;
    }
    if (failed)
        return raise_pending(state, owner);
    if (found == nullptr) {
        lua_pushnil(state);
        return 1;
    }
    push_record(state, &owner, collection->kind, false, found->slot, found->name);
    return 1;
}

static int collection_newindex(lua_State* state) {
    (void)state;
    return luaL_error(state, "a collection cannot be assigned");
}

static int collection_len(lua_State* state) {
    const ViewBox* collection = view_at(state, 1, kCollectionMeta);
    if (collection == nullptr || collection->owner == nullptr)
        return luaL_error(state, "expected a collection");
    std::size_t count = 0;
    bool failed = false;
    try {
        count = ensure_index(*collection->owner, collection->kind).size();
    } catch (const std::bad_alloc&) {
        pending_of(*collection->owner) = "out of memory";
        failed = true;
    }
    if (failed)
        return raise_pending(state, *collection->owner);
    if (count > static_cast<std::size_t>(std::numeric_limits<lua_Integer>::max()))
        return luaL_error(state, "too many records");
    lua_pushinteger(state, static_cast<lua_Integer>(count));
    return 1;
}

static int view_pairs(lua_State* state) {
    Collections* owner = any_view_owner(state);
    if (owner == nullptr)
        return luaL_error(state, "expected a collection");
    const bool skip = view_at(state, 1, kCollectionMeta) != nullptr;
    const int built = push_members(state, *owner);
    if (built < 0)
        return raise_pending(state, *owner);
    lua_pushinteger(state, 0);
    lua_pushboolean(state, skip ? 1 : 0);
    lua_pushcclosure(state, pairs_next, 3);
    lua_pushnil(state);
    lua_pushnil(state);
    return 3;
}

static int collection_each(lua_State* state) {
    if (!lua_isfunction(state, 2))
        return luaL_error(state, "each expects a function");
    Collections* owner = any_view_owner(state);
    if (owner == nullptr)
        return luaL_error(state, "expected a collection");
    const bool skip = view_at(state, 1, kCollectionMeta) != nullptr;
    const int built = push_members(state, *owner);
    if (built < 0)
        return raise_pending(state, *owner);
    const int sequence = lua_gettop(state);
    const lua_Unsigned length = lua_rawlen(state, sequence);
    if (length > static_cast<lua_Unsigned>(std::numeric_limits<lua_Integer>::max()))
        return luaL_error(state, "each: too many records");
    const auto last = static_cast<lua_Integer>(length);
    charge_scan(state);
    charge_visits(state, static_cast<uint64_t>(last));
    for (lua_Integer index = 1; index <= last; ++index) {
        if (skip) {
            lua_rawgeti(state, sequence, index);
            const auto* box = static_cast<const RecordBox*>(lua_touserdata(state, -1));
            bool alive = false;
            bool failed = false;
            try {
                alive = still_there(box);
            } catch (const std::bad_alloc&) {
                pending_of(*owner) = "out of memory";
                failed = true;
            }
            if (failed)
                return raise_pending(state, *owner);
            lua_pop(state, 1);
            if (!alive)
                continue;
        }
        lua_pushvalue(state, 2);
        lua_rawgeti(state, sequence, index);
        lua_call(state, 1, 0);
    }
    return 0;
}

static int collection_where(lua_State* state) {
    const ViewBox* collection = view_at(state, 1, kCollectionMeta);
    if (collection == nullptr || collection->owner == nullptr)
        return luaL_error(state, "where expects a collection");
    if (!lua_istable(state, 2))
        return luaL_error(state, "where expects a table");
    Collections& owner = *collection->owner;
    const Kind kind = collection->kind;
    const int key_count = host::push_sorted_keys(state, 2);
    const int result = where_from_keys(state, owner, kind, key_count);
    if (result < 0)
        return raise_pending(state, owner);
    return result;
}

static int set_index(lua_State* state) {
    if (view_at(state, 1, kSetMeta) == nullptr)
        return luaL_error(state, "expected a set");
    if (lua_type(state, 2) != LUA_TSTRING) {
        lua_pushnil(state);
        return 1;
    }
    std::size_t length = 0;
    const char* text = lua_tolstring(state, 2, &length);
    const std::string_view key(text != nullptr ? text : "", length);
    if (key == "each") {
        lua_pushcfunction(state, collection_each);
        return 1;
    }
    if (key == "set") {
        lua_pushcfunction(state, set_assign);
        return 1;
    }
    if (key == "scale") {
        lua_pushcfunction(state, set_scale);
        return 1;
    }
    if (key == "remove") {
        lua_pushcfunction(state, set_remove);
        return 1;
    }
    lua_pushnil(state);
    return 1;
}

static int set_newindex(lua_State* state) {
    (void)state;
    return luaL_error(state, "a set cannot be assigned");
}

static int set_len(lua_State* state) {
    if (view_at(state, 1, kSetMeta) == nullptr)
        return luaL_error(state, "expected a set");
    if (lua_getiuservalue(state, 1, 1) != LUA_TTABLE) {
        lua_pop(state, 1);
        lua_pushinteger(state, 0);
        return 1;
    }
    const lua_Unsigned length = lua_rawlen(state, -1);
    lua_pop(state, 1);
    lua_pushinteger(state, static_cast<lua_Integer>(length));
    return 1;
}

static int set_assign(lua_State* state) {
    if (view_at(state, 1, kSetMeta) == nullptr)
        return luaL_error(state, "set expects a set");
    if (!lua_istable(state, 2))
        return luaL_error(state, "set expects a table");
    Collections* owner = view_owner(state, kSetMeta);
    if (owner == nullptr)
        return luaL_error(state, "set expects a set");
    const int key_count = host::push_sorted_keys(state, 2);
    if (!read_keys(state, *owner, key_count, "a field name must be a string"))
        return raise_pending(state, *owner);
    if (!collect_handles(state, *owner, 1))
        return raise_pending(state, *owner);
    CollectionsState& scratch = working(*owner);
    scratch.assignments.clear();
    const ViewBox* set = view_at(state, 1, kSetMeta);
    const Kind kind = set != nullptr ? set->kind : Kind::units;
    const int table = 2;
    for (std::size_t index = 0; index < scratch.keys.size(); ++index) {
        charge_visits(state, 1);
        const std::string& raw = scratch.keys[index];
        charge_text(state, raw.size());
        lua_pushlstring(state, raw.data(), raw.size());
        lua_rawget(state, table);
        bool stored_ok = false;
        {
            std::string reason;
            Assignment assignment;
            try {
                stored_ok = read_stored(state, -1, assignment.value, reason);
                if (stored_ok) {
                    assignment.key.assign(trim_view(raw));
                    scratch.assignments.push_back(std::move(assignment));
                } else {
                    const std::string_view record = scratch.handles.empty()
                                                        ? std::string_view("record")
                                                        : std::string_view(scratch.handles[0].name);
                    pending_of(*owner) = prefixed(kind, record, trim_view(raw), reason);
                }
            } catch (const std::bad_alloc&) {
                pending_of(*owner) = "out of memory";
                stored_ok = false;
            }
        }
        lua_pop(state, 1);
        if (!stored_ok)
            return raise_pending(state, *owner);
    }
    charge_visits(state, static_cast<uint64_t>(scratch.handles.size()));
    charge_visits(state, saturating_product(scratch.handles.size(), scratch.assignments.size()));
    bool ok = false;
    try {
        std::string reason;
        ok = apply_assignments(state, reason);
        if (!ok)
            pending_of(*owner) = std::move(reason);
    } catch (const std::bad_alloc&) {
        pending_of(*owner) = "out of memory";
        ok = false;
    }
    if (!ok)
        return raise_pending(state, *owner);
    return 0;
}

static int set_scale(lua_State* state) {
    if (view_at(state, 1, kSetMeta) == nullptr)
        return luaL_error(state, "scale expects a set");
    if (!lua_istable(state, 2))
        return luaL_error(state, "scale expects a table");
    Collections* owner = view_owner(state, kSetMeta);
    if (owner == nullptr)
        return luaL_error(state, "scale expects a set");
    const int key_count = host::push_sorted_keys(state, 2);
    if (!read_keys(state, *owner, key_count, "a field name must be a string"))
        return raise_pending(state, *owner);
    if (!collect_handles(state, *owner, 1))
        return raise_pending(state, *owner);
    CollectionsState& scratch = working(*owner);
    scratch.factors.clear();
    const ViewBox* set = view_at(state, 1, kSetMeta);
    const Kind kind = set != nullptr ? set->kind : Kind::units;
    const int table = 2;
    const std::string_view record = scratch.handles.empty()
                                        ? std::string_view("record")
                                        : std::string_view(scratch.handles[0].name);
    for (std::size_t index = 0; index < scratch.keys.size(); ++index) {
        charge_visits(state, 1);
        const std::string& raw = scratch.keys[index];
        charge_text(state, raw.size());
        lua_pushlstring(state, raw.data(), raw.size());
        lua_rawget(state, table);
        const bool number = lua_type(state, -1) == LUA_TNUMBER;
        const double factor = number ? lua_tonumber(state, -1) : 0;
        lua_pop(state, 1);
        const std::string_view key = trim_view(raw);
        if (!number) {
            try {
                pending_of(*owner) = prefixed(kind, record, key, "scale expects a number");
            } catch (const std::bad_alloc&) {
                pending_of(*owner) = "out of memory";
            }
            return raise_pending(state, *owner);
        }
        bool failed = false;
        try {
            Factor stored;
            stored.key.assign(key);
            stored.number = factor;
            scratch.factors.push_back(std::move(stored));
        } catch (const std::bad_alloc&) {
            pending_of(*owner) = "out of memory";
            failed = true;
        }
        if (failed)
            return raise_pending(state, *owner);
    }
    charge_visits(state, static_cast<uint64_t>(scratch.handles.size()));
    charge_visits(state, saturating_product(scratch.handles.size(), scratch.factors.size()));
    bool ok = false;
    try {
        std::string reason;
        ok = apply_scale(state, reason);
        if (!ok)
            pending_of(*owner) = std::move(reason);
    } catch (const std::bad_alloc&) {
        pending_of(*owner) = "out of memory";
        ok = false;
    }
    if (!ok)
        return raise_pending(state, *owner);
    return 0;
}

static int set_remove(lua_State* state) {
    const ViewBox* set = view_at(state, 1, kSetMeta);
    const RecordBox* record = record_at(state, 1);
    if (set == nullptr && record == nullptr)
        return luaL_error(state, "remove expects a record or a set");
    Collections* owner = set != nullptr ? set->owner : record->owner;
    const Kind kind = set != nullptr ? set->kind : record->kind;
    if (owner == nullptr)
        return luaL_error(state, "remove expects a record or a set");
    if (!collect_handles(state, *owner, 1))
        return raise_pending(state, *owner);
    // `remove_collected` walks the handles twice. Charge that before the
    // caller's reason string exists.
    charge_visits(state, saturating_product(working(*owner).handles.size(), 2));
    bool ok = false;
    try {
        std::string reason;
        ok = remove_collected(*owner, kind, reason);
        if (!ok)
            pending_of(*owner) = std::move(reason);
    } catch (const std::bad_alloc&) {
        pending_of(*owner) = "out of memory";
        ok = false;
    }
    if (!ok)
        return raise_pending(state, *owner);
    return 0;
}

static int record_index(lua_State* state) {
    const RecordBox* box = record_at(state, 1);
    if (box == nullptr || box->owner == nullptr)
        return luaL_error(state, "expected a record");
    if (lua_type(state, 2) != LUA_TSTRING) {
        lua_pushnil(state);
        return 1;
    }
    std::size_t length = 0;
    const char* text = lua_tolstring(state, 2, &length);
    const std::string_view key = trim_view(std::string_view(text != nullptr ? text : "", length));
    if (key == "remove") {
        lua_pushcfunction(state, record_call_remove);
        return 1;
    }
    if (key == "get") {
        lua_pushcfunction(state, record_call_get);
        return 1;
    }
    if (key == "set") {
        lua_pushcfunction(state, record_call_set);
        return 1;
    }
    if (key == "keys") {
        lua_pushcfunction(state, record_call_keys);
        return 1;
    }
    if (box->kind == Kind::weapons && box->damage == 0 && same_text(key, "damage")) {
        if (box->name_size > static_cast<std::size_t>(std::numeric_limits<uint32_t>::max()))
            return luaL_error(state, "a record name is too long");
        push_record(state, box->owner, box->kind, true, box->slot, name_of(box));
        return 1;
    }
    int result = 0;
    try {
        result = prepare_read(box, key);
    } catch (const std::bad_alloc&) {
        pending_of(*box->owner) = "out of memory";
        result = -1;
    }
    if (result < 0)
        return raise_pending(state, *box->owner);
    push_prepared(state, working(*box->owner).prepared);
    return 1;
}

static int record_newindex(lua_State* state) {
    const RecordBox* box = record_at(state, 1);
    if (box == nullptr || box->owner == nullptr)
        return luaL_error(state, "expected a record");
    if (lua_type(state, 2) != LUA_TSTRING)
        return luaL_error(state, "a field name must be a string");
    std::size_t length = 0;
    const char* text = lua_tolstring(state, 2, &length);
    const std::string_view key = trim_view(std::string_view(text != nullptr ? text : "", length));
    if (record_method(key))
        return luaL_error(state, "a record method cannot be assigned");
    int result = 0;
    try {
        result = assign_record(state, box, key);
    } catch (const std::bad_alloc&) {
        pending_of(*box->owner) = "out of memory";
        result = -1;
    }
    if (result < 0)
        return raise_pending(state, *box->owner);
    return 0;
}

static int record_call_remove(lua_State* state) {
    return set_remove(state);
}

static int record_call_get(lua_State* state) {
    const RecordBox* box = record_at(state, 1);
    if (box == nullptr || box->owner == nullptr)
        return luaL_error(state, "get expects a record");
    if (lua_type(state, 2) != LUA_TSTRING)
        return luaL_error(state, "get expects a field name");
    std::size_t length = 0;
    const char* text = lua_tolstring(state, 2, &length);
    const std::string_view key = trim_view(std::string_view(text != nullptr ? text : "", length));
    int result = 0;
    try {
        result = prepare_read(box, key);
    } catch (const std::bad_alloc&) {
        pending_of(*box->owner) = "out of memory";
        result = -1;
    }
    if (result < 0)
        return raise_pending(state, *box->owner);
    push_prepared(state, working(*box->owner).prepared);
    return 1;
}

static int record_call_set(lua_State* state) {
    const RecordBox* box = record_at(state, 1);
    if (box == nullptr || box->owner == nullptr)
        return luaL_error(state, "set expects a record");
    if (lua_type(state, 2) != LUA_TSTRING)
        return luaL_error(state, "set expects a field name");
    if (lua_gettop(state) < 3 || lua_type(state, 3) == LUA_TNONE)
        return luaL_error(state, "set expects a value");
    std::size_t length = 0;
    const char* text = lua_tolstring(state, 2, &length);
    const std::string_view key = trim_view(std::string_view(text != nullptr ? text : "", length));
    int result = 0;
    try {
        result = assign_record(state, box, key);
    } catch (const std::bad_alloc&) {
        pending_of(*box->owner) = "out of memory";
        result = -1;
    }
    if (result < 0)
        return raise_pending(state, *box->owner);
    return 0;
}

static int record_call_keys(lua_State* state) {
    const RecordBox* box = record_at(state, 1);
    if (box == nullptr || box->owner == nullptr)
        return luaL_error(state, "keys expects a record");
    const int result = prepare_keys(state, box);
    if (result < 0)
        return raise_pending(state, *box->owner);
    const std::vector<std::string>& keys = working(*box->owner).keys;
    if (keys.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        pending_of(*box->owner) = "too many keys";
        return raise_pending(state, *box->owner);
    }
    lua_createtable(state, static_cast<int>(keys.size()), 0);
    for (std::size_t index = 0; index < keys.size(); ++index) {
        lua_pushlstring(state, keys[index].data(), keys[index].size());
        lua_rawseti(state, -2, static_cast<lua_Integer>(index) + 1);
    }
    return 1;
}

} // extern "C"

/// Installs the four protected metatables.
void ensure_all(lua_State* state) {
    ensure_meta(state, kNeedMeta, nullptr, nullptr, nullptr, nullptr, nullptr);
    ensure_meta(
        state,
        kCollectionMeta,
        collection_index,
        collection_newindex,
        collection_len,
        view_pairs,
        nullptr
    );
    ensure_meta(state, kSetMeta, set_index, set_newindex, set_len, view_pairs, nullptr);
    ensure_meta(state, kRecordMeta, record_index, record_newindex, nullptr, nullptr, nullptr);
}

/// Pushes one collection view.
void push_view(lua_State* state, Collections* owner, Kind kind) {
    ensure_all(state);
    auto* box = static_cast<ViewBox*>(lua_newuserdatauv(state, sizeof(ViewBox), 0));
    box->owner = owner;
    box->kind = kind;
    luaL_setmetatable(state, kCollectionMeta);
}

} // namespace

bool need_from_lua(
    lua_State* state, int index, std::string_view field, sm::Need& need, std::string* error
) {
    const int type = lua_type(state, index);
    const NeedBits* bits = nullptr;
    const char* text_ptr = nullptr;
    std::size_t text_length = 0;
    bool have_text = false;
    bool pop_text = false;
    sm::NeedKind kind = sm::NeedKind::range;
    bool negated = false;
    double low = 0;
    double high = 0;
    if (type == LUA_TNUMBER) {
        kind = sm::NeedKind::range;
        low = lua_tonumber(state, index);
        high = low;
    } else if (type == LUA_TBOOLEAN) {
        kind = sm::NeedKind::range;
        low = 1;
        high = 1;
        negated = lua_toboolean(state, index) == 0;
    } else if (type == LUA_TSTRING) {
        kind = sm::NeedKind::equals;
        text_ptr = lua_tolstring(state, index, &text_length);
        have_text = true;
    } else if (type == LUA_TUSERDATA) {
        bits = static_cast<const NeedBits*>(luaL_testudata(state, index, kNeedMeta));
        if (bits == nullptr)
            return refuse_need(error, field);
        const int value_type = lua_getiuservalue(state, index, 1);
        pop_text = true;
        if (value_type == LUA_TSTRING) {
            text_ptr = lua_tolstring(state, -1, &text_length);
            have_text = true;
        }
        kind = static_cast<sm::NeedKind>(bits->kind);
        negated = bits->negated != 0;
        low = bits->low;
        high = bits->high;
    } else {
        return refuse_need(error, field);
    }
    try {
        sm::Need built;
        built.field.assign(field);
        built.kind = kind;
        built.negated = negated;
        built.low = low;
        built.high = high;
        if (have_text && text_ptr != nullptr)
            built.text.assign(text_ptr, text_length);
        if (pop_text)
            lua_pop(state, 1);
        need = std::move(built);
        return true;
    } catch (const std::bad_alloc&) {
        if (pop_text)
            lua_pop(state, 1);
        if (error != nullptr) {
            try {
                *error = "out of memory";
            } catch (const std::bad_alloc&) {
            }
        }
        return false;
    }
}

void install_selection_forms(lua_State* state) {
    ensure_all(state);
    lua_pushcfunction(state, form_range);
    lua_setglobal(state, "range");
    lua_pushcfunction(state, form_is);
    lua_setglobal(state, "is");
    lua_pushcfunction(state, form_has);
    lua_setglobal(state, "has");
    lua_pushcfunction(state, form_absent);
    lua_setglobal(state, "absent");
    lua_pushcfunction(state, form_not);
    lua_setglobal(state, "not_");
}

Collections::Collections(rules::DocumentSet& documents, const defs::DataLayout* layout)
    : documents_(&documents), layout_(layout), state_(std::make_unique<CollectionsState>()) {
}

Collections::~Collections() = default;

void Collections::begin_mutator(std::string mutator, WriteGrant grant) {
    mutator_ = std::move(mutator);
    grant_ = grant;
    active_ = true;
}

void Collections::end_mutator() {
    active_ = false;
    mutator_.clear();
    grant_ = {};
}

void Collections::push_units(lua_State* state) {
    push_view(state, this, Kind::units);
}

void Collections::push_weapons(lua_State* state) {
    push_view(state, this, Kind::weapons);
}

void Collections::push_features(lua_State* state) {
    push_view(state, this, Kind::features);
}

void Collections::set_context_collections(lua_State* state, int ctx_index) {
    const int absolute = lua_absindex(state, ctx_index);
    push_units(state);
    lua_setfield(state, absolute, "units");
    push_weapons(state);
    lua_setfield(state, absolute, "weapons");
    push_features(state);
    lua_setfield(state, absolute, "features");
}

std::span<const rules::FieldWrite> Collections::writes() const {
    return writes_;
}

std::span<const RecordRemoval> Collections::removals() const {
    return removals_;
}

std::span<const HookNote> Collections::notes() const {
    return notes_;
}

} // namespace oa::data::mutators
