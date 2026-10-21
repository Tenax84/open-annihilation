// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Clones a unit or a weapon onto a new name. The unit keeps the source model
// by an explicit Objectname and the layer serves the source script, picture
// and build pages under the new name. A weapon takes the lowest free id from
// 1 to 255. Download entries are not copied. Download-linked button art
// (anims/<unit>_gadget.gaf) is not copied: its sequences are named after the
// built units, so the file under the new name would draw nothing.
#include "clone.hpp"

#include "oa/data/field_schema.hpp"
#include "oa/data/mutators/collections.hpp"
#include "oa/data/script_host/fixed.hpp"
#include "oa/data/script_host/sorted.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/formats/tdf/edit.hpp"
#include "oa/formats/tdf/number.hpp"
#include "oa/formats/tdf/write.hpp"

#include "lua.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::data::mutators {
namespace tdf = oa::formats::tdf;
namespace fields = oa::data::field_schema;
namespace rules = oa::data::mutation_rules;
namespace host = oa::data::script_host;
namespace defs = oa::data::defs;

/// Reaches the collections' own lists. Defined here so the registration in
/// collections.cpp stays the method lookup.
struct CloneAccess {
    static rules::DocumentSet& documents(Collections& self) noexcept { return *self.documents_; }

    static const defs::DataLayout* layout_ptr(const Collections& self) noexcept {
        return self.layout_;
    }

    static bool active(const Collections& self) noexcept { return self.active_; }

    static const std::string& mutator(const Collections& self) noexcept { return self.mutator_; }

    static const WriteGrant& grant(const Collections& self) noexcept { return self.grant_; }

    static std::vector<rules::FieldWrite>& writes(Collections& self) noexcept {
        return self.writes_;
    }

    static std::vector<HookNote>& notes(Collections& self) noexcept { return self.notes_; }

    static oa::AssetStore* store(Collections& self) noexcept { return self.store_; }

    static std::vector<CloneRecord>& clones(Collections& self) noexcept { return self.clones_; }

    static std::vector<ServedCopy>& served(Collections& self) noexcept { return self.served_; }
};

namespace {

/// The deepest build page the game probes before it stops.
constexpr uint32_t k_page_limit = 4096;

/// How a script value is held until the clone has been checked.
enum class HeldKind : uint8_t { nil_value, boolean, number, text, fixed_value };

/// One field from the clone's table, in the order it will be written.
struct HeldField {
    std::string key;
    HeldKind kind = HeldKind::nil_value;
    bool flag = false;
    double number = 0;
    std::string text;
    int64_t count = 0;
};

/// One section the collection would list.
struct Listed {
    std::size_t doc_index = 0;
    tdf::Block* section = nullptr;
    uint32_t ordinal = 0;
    std::string name;
    std::string path;
};

/// Bytes read for one served copy, kept until the clone has succeeded.
struct PendingCopy {
    std::string path;
    std::string source_path;
    std::shared_ptr<const std::vector<uint8_t>> bytes;
};

/// Puts the Lua stack back when the clone returns.
struct StackTop {
    lua_State* state = nullptr;
    int top = 0;

    ~StackTop() {
        if (state != nullptr)
            lua_settop(state, top);
    }
};

/// The layout the collections read, or the layout in force.
const defs::DataLayout& layout_of(const Collections& self) noexcept {
    if (CloneAccess::layout_ptr(self) != nullptr)
        return *CloneAccess::layout_ptr(self);
    return defs::data_layout();
}

unsigned char fold_byte(unsigned char byte) noexcept {
    if (byte >= 'A' && byte <= 'Z')
        return static_cast<unsigned char>(byte - 'A' + 'a');
    return byte;
}

bool same_text(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (fold_byte(static_cast<unsigned char>(left[index])) !=
            fold_byte(static_cast<unsigned char>(right[index])))
            return false;
    }
    return true;
}

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

bool same_path(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        char left_byte = left[index];
        char right_byte = right[index];
        if (left_byte == '\\')
            left_byte = '/';
        if (right_byte == '\\')
            right_byte = '/';
        if (fold_byte(static_cast<unsigned char>(left_byte)) !=
            fold_byte(static_cast<unsigned char>(right_byte)))
            return false;
    }
    return true;
}

bool is_separator(unsigned char byte) noexcept {
    return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r';
}

std::string_view trim_view(std::string_view text) noexcept {
    while (!text.empty() && is_separator(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    while (!text.empty() && is_separator(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);
    return text;
}

std::string file_stem(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    const std::string_view filename =
        slash == std::string_view::npos ? path : path.substr(slash + 1);
    const std::size_t dot = filename.rfind('.');
    if (dot == std::string_view::npos || dot == 0)
        return std::string(filename);
    return std::string(filename.substr(0, dot));
}

bool listed_before(const Listed& left, const Listed& right) noexcept {
    const int names = compare_folded(left.name, right.name);
    if (names != 0)
        return names < 0;
    const int paths = compare_folded(left.path, right.path);
    if (paths != 0)
        return paths < 0;
    return left.ordinal < right.ordinal;
}

std::string request_error(
    bool weapons, std::string_view source, std::string_view name, std::string_view detail
) {
    std::string text = weapons ? "weapons:clone(\"" : "units:clone(\"";
    text.append(source);
    text.append("\", \"");
    text.append(name);
    text.append("\"): ");
    text.append(detail);
    return text;
}

std::string no_free_id(std::string_view source, std::string_view name) {
    std::string text = "no free weapon id: 1 to 255 are all used (cloning ";
    text.append(source);
    text.append(" as ");
    text.append(name);
    text.append(")");
    return text;
}

/// A name of 1 to 31 ASCII letters, digits, '_' or '-'.
bool name_ok(std::string_view name) noexcept {
    if (name.empty() || name.size() > 31)
        return false;
    for (std::size_t index = 0; index < name.size(); ++index) {
        const unsigned char byte = static_cast<unsigned char>(name[index]);
        const bool letter = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z');
        const bool digit = byte >= '0' && byte <= '9';
        if (!letter && !digit && byte != '_' && byte != '-')
            return false;
    }
    return true;
}

const defs::DataLayout& layout_now(const Collections& self) noexcept {
    return layout_of(self);
}

std::string folder_of(const Collections& self, defs::DataDirectory directory) {
    const defs::DataLayout& layout = layout_now(self);
    return layout.directories[static_cast<std::size_t>(directory)];
}

/// Lists live unit or weapon records in collection order.
std::vector<Listed> list_named(Collections& self, bool weapons) {
    rules::DocumentSet& documents = CloneAccess::documents(self);
    const std::string folder = weapons ? folder_of(self, defs::DataDirectory::weapons)
                                       : folder_of(self, defs::DataDirectory::units);
    const std::string suffix = weapons ? std::string("tdf") : layout_now(self).unit_extension;
    const std::vector<std::size_t> docs = documents.documents_in(folder, suffix, false);
    std::vector<Listed> records;
    for (const std::size_t doc_index : docs) {
        rules::LoadedDocument& loaded = documents.at(doc_index);
        tdf::Block* root = tdf::editable_root(loaded.document.get());
        if (root == nullptr)
            continue;
        const uint32_t sections = tdf::child_count(root);
        if (!weapons) {
            for (uint32_t section = 0; section < sections; ++section) {
                tdf::Block* block = tdf::editable_child(root, section);
                if (block == nullptr || block->name == nullptr)
                    continue;
                if (tdf::compare_nocase(block->name, "UNITINFO") != 0)
                    continue;
                Listed listed;
                listed.doc_index = doc_index;
                listed.section = block;
                listed.path = loaded.path;
                const char* unit_name = tdf::find_value(block, "UnitName");
                if (unit_name != nullptr && unit_name[0] != '\0')
                    listed.name = unit_name;
                else
                    listed.name = file_stem(loaded.path);
                records.push_back(std::move(listed));
                break;
            }
            continue;
        }
        std::vector<std::string> seen;
        for (uint32_t section = 0; section < sections; ++section) {
            tdf::Block* block = tdf::editable_child(root, section);
            if (block == nullptr || block->name == nullptr)
                continue;
            Listed listed;
            listed.doc_index = doc_index;
            listed.section = block;
            listed.path = loaded.path;
            listed.name = block->name;
            uint32_t ordinal = 0;
            for (const std::string& earlier : seen) {
                if (same_text(earlier, listed.name))
                    ++ordinal;
            }
            listed.ordinal = ordinal;
            seen.push_back(listed.name);
            records.push_back(std::move(listed));
        }
    }
    std::sort(records.begin(), records.end(), listed_before);
    return records;
}

const Listed* find_named(const std::vector<Listed>& records, std::string_view name) {
    for (const Listed& listed : records) {
        if (same_text(listed.name, name))
            return &listed;
    }
    return nullptr;
}

bool ensure_grant(const Collections& self, bool weapons, std::string& error) {
    if (!CloneAccess::active(self)) {
        error = "no mutator has begun";
        return false;
    }
    const WriteGrant& grant = CloneAccess::grant(self);
    if (weapons ? grant.weapons : grant.units)
        return true;
    error = "this mutator does not declare the ";
    error.append(weapons ? "weapons" : "units");
    error.append(" layer");
    return false;
}

uint32_t count_mine_writes(const Collections& self) {
    const std::string& who = CloneAccess::mutator(self);
    uint32_t count = 0;
    for (const rules::FieldWrite& write : self.writes()) {
        bool mine = false;
        for (const std::string& name : write.mutators) {
            if (name == who)
                mine = true;
        }
        if (!mine)
            continue;
        if (count == mutator_write_limit)
            return count;
        ++count;
    }
    return count;
}

uint32_t count_mine_notes(const Collections& self) {
    const std::string& who = CloneAccess::mutator(self);
    uint32_t count = 0;
    for (const HookNote& note : self.notes()) {
        bool mine = false;
        for (const std::string& name : note.mutators) {
            if (name == who)
                mine = true;
        }
        if (!mine)
            continue;
        if (count == mutator_note_limit)
            return count;
        ++count;
    }
    return count;
}

bool room_for(
    const Collections& self, uint32_t more_writes, uint32_t more_notes, std::string& error
) {
    const uint32_t writes = count_mine_writes(self);
    const uint32_t notes = count_mine_notes(self);
    if (more_writes > 0 && writes > mutator_write_limit - more_writes) {
        error = "this mutator has changed too many fields";
        return false;
    }
    if (more_notes > 0 && notes > mutator_note_limit - more_notes) {
        error = "this mutator has made too many notes";
        return false;
    }
    return true;
}

std::string
prefixed(bool weapons, std::string_view record, std::string_view key, std::string_view detail) {
    std::string text = weapons ? "weapons " : "units ";
    text.append(record);
    text.push_back(' ');
    text.append(key);
    text.append(": ");
    text.append(detail);
    return text;
}

/// Reads the fields table. Keys are sorted before any document changes.
bool read_fields(
    lua_State* state,
    int index,
    bool weapons,
    std::string_view source,
    std::string_view name,
    std::vector<HeldField>& fields_out,
    std::string& error
) {
    lua_pushnil(state);
    while (lua_next(state, index) != 0) {
        if (lua_type(state, -2) != LUA_TSTRING) {
            error = request_error(weapons, source, name, "a field name must be text");
            return false;
        }
        std::size_t length = 0;
        const char* key_text = lua_tolstring(state, -2, &length);
        HeldField held;
        held.key.assign(key_text != nullptr ? key_text : "", length);
        const int type = lua_type(state, -1);
        if (type == LUA_TNIL) {
            held.kind = HeldKind::nil_value;
        } else if (type == LUA_TBOOLEAN) {
            held.kind = HeldKind::boolean;
            held.flag = lua_toboolean(state, -1) != 0;
        } else if (type == LUA_TNUMBER) {
            held.kind = HeldKind::number;
            held.number = lua_tonumber(state, -1);
        } else if (type == LUA_TSTRING) {
            std::size_t text_length = 0;
            const char* text = lua_tolstring(state, -1, &text_length);
            held.kind = HeldKind::text;
            held.text.assign(text != nullptr ? text : "", text_length);
        } else if (type == LUA_TUSERDATA && host::to_fixed(state, -1, &held.count)) {
            held.kind = HeldKind::fixed_value;
        } else {
            const char* type_name = lua_typename(state, type);
            std::string detail = "a ";
            detail.append(type_name != nullptr ? type_name : "value");
            detail.append(" cannot be stored");
            error = request_error(weapons, source, name, detail);
            return false;
        }
        fields_out.push_back(std::move(held));
        lua_pop(state, 1);
    }
    std::sort(
        fields_out.begin(), fields_out.end(), [](const HeldField& left, const HeldField& right) {
            return host::name_less(left.key, right.key);
        }
    );
    return true;
}

/// Checks every field the way a typed write would, before the clone adds anything.
bool precheck_fields(
    bool weapons,
    std::string_view source,
    std::string_view name,
    const std::vector<HeldField>& held,
    std::string& error
) {
    const fields::RecordKind schema =
        weapons ? fields::RecordKind::weapon : fields::RecordKind::unit;
    for (const HeldField& field : held) {
        const std::string_view key = trim_view(field.key);
        if (key.empty()) {
            error = request_error(
                weapons, source, name, prefixed(weapons, name, field.key, "the field name is empty")
            );
            return false;
        }
        if (weapons && same_text(key, "id")) {
            error = request_error(weapons, source, name, "ID is set by the clone");
            return false;
        }
        if (field.kind == HeldKind::nil_value)
            continue;
        fields::Check check;
        if (field.kind == HeldKind::number) {
            check = fields::check_number(schema, key, field.number);
            if (check.verdict != fields::Verdict::refuse && !std::isfinite(field.number)) {
                const char* word = std::isnan(field.number) ? "NaN" : "infinity";
                check.verdict = fields::Verdict::refuse;
                check.reason =
                    std::string(word) +
                    " is not a finite number; a value that is not finite cannot be stored";
            }
            if (check.verdict != fields::Verdict::refuse &&
                !tdf::format_number(field.number).has_value()) {
                check.verdict = fields::Verdict::refuse;
                check.reason = "a value that is not finite cannot be stored";
            }
        } else if (field.kind == HeldKind::boolean) {
            check = fields::check_number(schema, key, field.flag ? 1.0 : 0.0);
        } else if (field.kind == HeldKind::fixed_value) {
            const double as_number = static_cast<double>(field.count) / 65536.0;
            check = fields::check_number(schema, key, as_number);
        } else {
            check = fields::check_text(schema, key, trim_view(field.text));
        }
        if (check.verdict == fields::Verdict::refuse) {
            error =
                request_error(weapons, source, name, prefixed(weapons, name, key, check.reason));
            return false;
        }
    }
    return true;
}

struct FieldNow {
    bool present = false;
    std::string key;
    std::optional<std::string> text;
};

FieldNow field_now(const tdf::Block* block, std::string_view key) {
    FieldNow now;
    const std::string_view trimmed = trim_view(key);
    now.key.assign(trimmed);
    if (block == nullptr)
        return now;
    const uint32_t count = tdf::property_count(block);
    for (uint32_t index = 0; index < count; ++index) {
        const char* stored = tdf::property_key_at(block, static_cast<int32_t>(index));
        if (stored == nullptr || !same_text(stored, trimmed))
            continue;
        now.present = true;
        now.key = stored;
        const char* value = tdf::property_value_at(block, static_cast<int32_t>(index));
        now.text =
            value != nullptr ? std::optional<std::string>(value) : std::optional<std::string>("");
        break;
    }
    return now;
}

bool apply_field(
    Collections& self,
    bool weapons,
    rules::LoadedDocument& loaded,
    tdf::Block* block,
    std::string_view record,
    const HeldField& field,
    std::string& error
) {
    const std::string_view trimmed = trim_view(field.key);
    const fields::RecordKind schema =
        weapons ? fields::RecordKind::weapon : fields::RecordKind::unit;
    const FieldNow before = field_now(block, trimmed);
    if (field.kind == HeldKind::nil_value) {
        if (!before.present)
            return true;
        if (!room_for(self, 1, 0, error))
            return false;
        const tdf::EditStatus status = tdf::remove_key(loaded.document.get(), block, before.key);
        if (status != tdf::EditStatus::ok) {
            error = tdf::edit_status_message(status);
            return false;
        }
        rules::FieldWrite write;
        write.mutators.push_back(CloneAccess::mutator(self));
        write.path = loaded.path;
        write.record = std::string(record);
        write.key = before.key;
        write.old_text = before.text;
        CloneAccess::writes(self).push_back(std::move(write));
        return true;
    }

    fields::Check check;
    std::string text;
    if (field.kind == HeldKind::number) {
        check = fields::check_number(schema, trimmed, field.number);
        const std::optional<std::string> spelled = tdf::format_number(field.number);
        if (!spelled.has_value()) {
            error = "a value that is not finite cannot be stored";
            return false;
        }
        text = *spelled;
    } else if (field.kind == HeldKind::boolean) {
        check = fields::check_number(schema, trimmed, field.flag ? 1.0 : 0.0);
        text = field.flag ? "1" : "0";
    } else if (field.kind == HeldKind::fixed_value) {
        const double as_number = static_cast<double>(field.count) / 65536.0;
        check = fields::check_number(schema, trimmed, as_number);
        char spelled[host::fixed_text_bytes];
        if (!host::fixed_text(field.count, spelled, sizeof spelled)) {
            error = "a value that is not finite cannot be stored";
            return false;
        }
        text.assign(spelled);
    } else {
        const std::string_view body = trim_view(field.text);
        check = fields::check_text(schema, trimmed, body);
        text.assign(body);
    }
    if (check.verdict == fields::Verdict::refuse) {
        error = prefixed(weapons, record, trimmed, check.reason);
        return false;
    }
    if (before.present && before.text.has_value() && *before.text == text)
        return true;
    uint32_t notes = 0;
    if (check.verdict == fields::Verdict::warn)
        notes = 1;
    if (!room_for(self, 1, notes, error))
        return false;
    const std::string write_key = before.present ? before.key : std::string(trimmed);
    const tdf::EditStatus status = tdf::set_value(loaded.document.get(), block, write_key, text);
    if (status != tdf::EditStatus::ok) {
        error = tdf::edit_status_message(status);
        return false;
    }
    const FieldNow after = field_now(block, trimmed);
    rules::FieldWrite write;
    write.mutators.push_back(CloneAccess::mutator(self));
    write.path = loaded.path;
    write.record = std::string(record);
    write.key = after.present ? after.key : write_key;
    write.old_text = before.present ? before.text : std::nullopt;
    write.new_text = after.present ? after.text : std::optional<std::string>(text);
    CloneAccess::writes(self).push_back(std::move(write));
    if (check.verdict == fields::Verdict::warn) {
        HookNote note;
        note.kind = HookNoteKind::check_warned;
        note.mutators.push_back(CloneAccess::mutator(self));
        note.path = loaded.path;
        note.record = std::string(record);
        note.key = write.key;
        note.message = check.reason;
        CloneAccess::notes(self).push_back(std::move(note));
    }
    return true;
}

bool copy_properties(
    tdf::Document* dest_doc, tdf::Block* dest, const tdf::Block* source, std::string& error
) {
    const uint32_t count = source->property_count;
    std::vector<uint32_t> places(count);
    for (uint32_t index = 0; index < count; ++index)
        places[index] = index;
    std::sort(places.begin(), places.end(), [&](uint32_t left, uint32_t right) {
        return source->properties[left].order < source->properties[right].order;
    });
    for (const uint32_t place : places) {
        const tdf::Property& property = source->properties[place];
        if (property.key == nullptr)
            continue;
        const char* value = property.value != nullptr ? property.value : "";
        const tdf::EditStatus status = tdf::set_value(dest_doc, dest, property.key, value);
        if (status != tdf::EditStatus::ok) {
            error = tdf::edit_status_message(status);
            return false;
        }
    }
    return true;
}

bool copy_children(
    tdf::Document* dest_doc, tdf::Block* dest, const tdf::Block* source, std::string& error
) {
    const uint32_t count = tdf::child_count(source);
    for (uint32_t index = 0; index < count; ++index) {
        const tdf::Block* child = tdf::child_at(source, index);
        if (child == nullptr || child->name == nullptr)
            continue;
        tdf::EditStatus status = tdf::EditStatus::ok;
        tdf::Block* made = tdf::add_child(dest_doc, dest, child->name, &status);
        if (made == nullptr) {
            error = tdf::edit_status_message(status);
            return false;
        }
        if (!copy_properties(dest_doc, made, child, error))
            return false;
        if (!copy_children(dest_doc, made, child, error))
            return false;
    }
    return true;
}

bool strip_canbuild(tdf::Document* document, tdf::Block* block, std::string& error) {
    if (block == nullptr)
        return true;
    const uint32_t count = tdf::child_count(block);
    for (uint32_t index = count; index > 0; --index) {
        tdf::Block* child = tdf::editable_child(block, index - 1);
        if (child != nullptr && child->name != nullptr &&
            tdf::compare_nocase(child->name, "CANBUILD") == 0) {
            const tdf::EditStatus status = tdf::remove_child(document, block, index - 1);
            if (status != tdf::EditStatus::ok) {
                error = tdf::edit_status_message(status);
                return false;
            }
            continue;
        }
        if (!strip_canbuild(document, child, error))
            return false;
    }
    return true;
}

std::shared_ptr<const std::vector<uint8_t>> lookup_bytes(Collections& self, std::string_view path) {
    const std::vector<ServedCopy>& served = CloneAccess::served(self);
    for (std::size_t index = served.size(); index > 0; --index) {
        if (same_path(served[index - 1].path, path))
            return served[index - 1].bytes;
    }
    oa::AssetStore* store = CloneAccess::store(self);
    if (store == nullptr)
        return nullptr;
    std::optional<std::vector<uint8_t>> loaded = store->load_file_contents(path);
    if (!loaded.has_value() || loaded->empty())
        return nullptr;
    return std::make_shared<const std::vector<uint8_t>>(std::move(*loaded));
}

bool path_taken(Collections& self, std::string_view path) {
    if (CloneAccess::documents(self).find(path) != nullptr)
        return true;
    oa::AssetStore* store = CloneAccess::store(self);
    if (store == nullptr)
        return false;
    return store->file_size(path) > 0;
}

void remember_copy(
    std::vector<PendingCopy>& pending,
    std::string path,
    std::string source_path,
    const std::shared_ptr<const std::vector<uint8_t>>& bytes
) {
    if (bytes == nullptr || bytes->empty())
        return;
    PendingCopy copy;
    copy.path = std::move(path);
    copy.source_path = std::move(source_path);
    copy.bytes = bytes;
    pending.push_back(std::move(copy));
}

std::string join_path(std::string_view folder, std::string_view name) {
    std::string path(folder);
    if (!path.empty() && path.back() != '/')
        path.push_back('/');
    path.append(name);
    return path;
}

/// Reads the script, picture and build pages the game would, under the new name.
void collect_unit_copies(
    Collections& self,
    std::string_view source,
    std::string_view name,
    std::vector<PendingCopy>& pending
) {
    const std::string pictures = folder_of(self, defs::DataDirectory::unitpics);
    const std::string pages = folder_of(self, defs::DataDirectory::guis);
    const std::string source_script = join_path("scripts", std::string(source) + ".COB");
    const std::string new_script = join_path("scripts", std::string(name) + ".COB");
    remember_copy(pending, new_script, source_script, lookup_bytes(self, source_script));
    const std::string source_pic = join_path(pictures, std::string(source) + ".PCX");
    const std::string new_pic = join_path(pictures, std::string(name) + ".PCX");
    remember_copy(pending, new_pic, source_pic, lookup_bytes(self, source_pic));

    std::vector<std::string> served_pages;
    const auto consider_page = [&](const std::string& page) {
        const std::string source_page = join_path(pages, std::string(source) + page + ".GUI");
        const std::string new_page = join_path(pages, std::string(name) + page + ".GUI");
        const std::shared_ptr<const std::vector<uint8_t>> bytes = lookup_bytes(self, source_page);
        if (bytes == nullptr || bytes->empty())
            return false;
        remember_copy(pending, new_page, source_page, bytes);
        served_pages.push_back(page);
        return true;
    };
    (void)consider_page("0");
    for (uint32_t page = 1; page <= k_page_limit; ++page) {
        if (!consider_page(std::to_string(page)))
            break;
    }
    for (const std::string& page : served_pages) {
        const std::string source_gaf = join_path("anims", std::string(source) + page + ".GAF");
        const std::string new_gaf = join_path("anims", std::string(name) + page + ".GAF");
        remember_copy(pending, new_gaf, source_gaf, lookup_bytes(self, source_gaf));
    }
}

void note_ids(const tdf::Block* block, bool used[256]) {
    if (block == nullptr)
        return;
    const char* text = tdf::find_value(block, "ID");
    if (text != nullptr) {
        const int32_t value = tdf::parse_int(text);
        if (value >= 0 && value <= 255)
            used[static_cast<std::size_t>(value)] = true;
    }
    const uint32_t count = tdf::child_count(block);
    for (uint32_t index = 0; index < count; ++index)
        note_ids(tdf::child_at(block, index), used);
}

void note_document_ids(const rules::LoadedDocument& loaded, bool used[256]) {
    if (loaded.removed)
        return;
    const tdf::Block* root = loaded.document.root();
    note_ids(root, used);
}

/// Every id a live weapon section uses, in the document set and in store files
/// the set does not already hold. An empty or non-numeric id uses 0. An id
/// outside 0..255 uses none.
void collect_used_ids(Collections& self, bool used[256]) {
    rules::DocumentSet& documents = CloneAccess::documents(self);
    const std::string folder = folder_of(self, defs::DataDirectory::weapons);
    const std::vector<std::size_t> docs = documents.documents_in(folder, "tdf", false);
    for (const std::size_t doc_index : docs)
        note_document_ids(documents.at(doc_index), used);
    oa::AssetStore* store = CloneAccess::store(self);
    if (store == nullptr)
        return;
    const std::vector<std::string> names =
        store->list_effective_in_mount_order(folder, ".tdf", true);
    for (const std::string& name : names) {
        if (documents.find(name) != nullptr)
            continue;
        const std::optional<std::vector<uint8_t>> bytes = store->load_file_contents(name);
        if (!bytes.has_value() || bytes->empty())
            continue;
        if (bytes->size() > static_cast<std::size_t>(tdf::max_input_bytes))
            continue;
        tdf::OwnedDocument parsed;
        const std::string_view text(reinterpret_cast<const char*>(bytes->data()), bytes->size());
        if (!parsed.parse(text, nullptr))
            continue;
        note_ids(parsed.root(), used);
    }
}

int lowest_free_id(const bool used[256]) {
    for (int id = 1; id <= 255; ++id) {
        if (!used[static_cast<std::size_t>(id)])
            return id;
    }
    return 0;
}

std::string weapon_path(Collections& self, std::string_view name, std::string& error) {
    const std::string folder = folder_of(self, defs::DataDirectory::weapons);
    const std::string first = join_path(folder, std::string(name) + ".tdf");
    if (!path_taken(self, first))
        return first;
    for (int suffix = 2; suffix <= 100000; ++suffix) {
        const std::string candidate =
            join_path(folder, std::string(name) + "-" + std::to_string(suffix) + ".tdf");
        if (!path_taken(self, candidate))
            return candidate;
    }
    error = "no free weapon file name";
    return {};
}

std::string unit_path(const Collections& self, std::string_view name) {
    const std::string folder = folder_of(self, defs::DataDirectory::units);
    std::string file(name);
    file.push_back('.');
    file.append(layout_now(self).unit_extension);
    return join_path(folder, file);
}

tdf::Block* unit_section(rules::LoadedDocument& loaded) {
    tdf::Block* root = tdf::editable_root(loaded.document.get());
    if (root == nullptr)
        return nullptr;
    const uint32_t sections = tdf::child_count(root);
    for (uint32_t index = 0; index < sections; ++index) {
        tdf::Block* block = tdf::editable_child(root, index);
        if (block == nullptr || block->name == nullptr)
            continue;
        if (tdf::compare_nocase(block->name, "UNITINFO") == 0)
            return block;
    }
    return nullptr;
}

std::string effective_model(const tdf::Block* section, std::string_view record_name) {
    const char* object_name = section != nullptr ? tdf::find_value(section, "Objectname") : nullptr;
    if (object_name != nullptr && object_name[0] != '\0')
        return object_name;
    const char* unit_name = section != nullptr ? tdf::find_value(section, "UnitName") : nullptr;
    if (unit_name != nullptr && unit_name[0] != '\0')
        return unit_name;
    return std::string(record_name);
}

void publish(
    Collections& self,
    bool weapons,
    std::string name,
    std::string source,
    int weapon_id,
    std::string path,
    std::vector<PendingCopy> pending
) {
    CloneRecord record;
    record.mutator = CloneAccess::mutator(self);
    record.weapon = weapons;
    record.name = name;
    record.source = source;
    record.weapon_id = weapon_id;
    record.path = path;
    if (weapons) {
        record.sentence = "added weapon ";
        record.sentence.append(name);
        record.sentence.append(" (copy of ");
        record.sentence.append(source);
        record.sentence.append(") at id ");
        record.sentence.append(std::to_string(weapon_id));
    } else {
        record.sentence = "added unit ";
        record.sentence.append(name);
        record.sentence.append(" (copy of ");
        record.sentence.append(source);
        record.sentence.append(")");
    }
    CloneAccess::clones(self).push_back(std::move(record));
    for (PendingCopy& copy : pending) {
        ServedCopy served;
        served.mutator = CloneAccess::mutator(self);
        served.sentence = "added path ";
        served.sentence.append(copy.path);
        served.sentence.append(", copy of ");
        served.sentence.append(copy.source_path);
        served.path = std::move(copy.path);
        served.source_path = std::move(copy.source_path);
        served.bytes = std::move(copy.bytes);
        CloneAccess::served(self).push_back(std::move(served));
    }
}

bool set_text(
    rules::LoadedDocument& loaded,
    tdf::Block* block,
    std::string_view key,
    std::string_view value,
    std::string& error
) {
    const tdf::EditStatus status = tdf::set_value(loaded.document.get(), block, key, value);
    if (status != tdf::EditStatus::ok) {
        error = tdf::edit_status_message(status);
        return false;
    }
    return true;
}

} // namespace

int clone_collection(lua_State* state, Collections& collections, bool weapons, std::string& error) {
    StackTop stack;
    stack.state = state;
    stack.top = lua_gettop(state);
    const char* which = weapons ? "weapons:clone" : "units:clone";
    if (lua_gettop(state) < 4 || lua_type(state, 2) != LUA_TSTRING ||
        lua_type(state, 3) != LUA_TSTRING || lua_type(state, 4) != LUA_TTABLE) {
        error = which;
        error.append(" expects the source name, the new name and a fields table");
        return -1;
    }
    std::size_t source_length = 0;
    std::size_t name_length = 0;
    const char* source_text = lua_tolstring(state, 2, &source_length);
    const char* name_text = lua_tolstring(state, 3, &name_length);
    const std::string source(source_text != nullptr ? source_text : "", source_length);
    const std::string name(name_text != nullptr ? name_text : "", name_length);
    if (!ensure_grant(collections, weapons, error))
        return -1;

    const std::vector<Listed> records = list_named(collections, weapons);
    const Listed* found = find_named(records, source);
    if (found == nullptr) {
        error = request_error(
            weapons,
            source,
            name,
            weapons ? "the source is not a weapon" : "the source is not a unit"
        );
        return -1;
    }
    if (find_named(records, name) != nullptr) {
        error = request_error(weapons, source, name, "the new name already exists");
        return -1;
    }
    if (!name_ok(name)) {
        error = request_error(
            weapons, source, name, "the new name is not 1 to 31 letters, digits, '_' or '-'"
        );
        return -1;
    }

    std::vector<HeldField> held;
    if (!read_fields(state, 4, weapons, source, name, held, error))
        return -1;
    if (!precheck_fields(weapons, source, name, held, error))
        return -1;

    int weapon_id = 0;
    if (weapons) {
        bool used[256] = {};
        collect_used_ids(collections, used);
        weapon_id = lowest_free_id(used);
        if (weapon_id == 0) {
            error = no_free_id(source, name);
            return -1;
        }
    }

    const std::string path =
        weapons ? weapon_path(collections, name, error) : unit_path(collections, name);
    if (path.empty()) {
        const std::string detail = error.empty() ? std::string("the file name is not free") : error;
        error = request_error(weapons, source, name, detail);
        return -1;
    }
    if (!weapons && path_taken(collections, path)) {
        error = request_error(weapons, source, name, "the unit file already exists");
        return -1;
    }

    std::vector<PendingCopy> pending;
    if (!weapons)
        collect_unit_copies(collections, source, name, pending);

    rules::DocumentSet& documents = CloneAccess::documents(collections);
    std::string body;
    const tdf::Block* source_section = found->section;
    if (!weapons) {
        rules::LoadedDocument& source_doc = documents.at(found->doc_index);
        const std::optional<std::string> written = tdf::write_text(source_doc.document.get());
        if (!written.has_value()) {
            error = request_error(weapons, source, name, "the source could not be written");
            return -1;
        }
        body = *written;
    } else {
        body.push_back('[');
        body.append(name);
        body.append("]\n{\n}\n");
    }
    std::string add_error;
    if (!documents.add(path, body, &add_error)) {
        error = request_error(
            weapons, source, name, add_error.empty() ? "the file could not be added" : add_error
        );
        return -1;
    }
    rules::LoadedDocument* loaded = documents.find(path);
    if (loaded == nullptr) {
        error = request_error(weapons, source, name, "the file could not be added");
        return -1;
    }
    loaded->added = true;
    const auto abandon = [&]() { loaded->removed = true; };

    if (!weapons) {
        tdf::Block* section = unit_section(*loaded);
        if (section == nullptr) {
            abandon();
            error = request_error(weapons, source, name, "the copy has no unit");
            return -1;
        }
        if (!strip_canbuild(
                loaded->document.get(), tdf::editable_root(loaded->document.get()), error
            ) ||
            !set_text(*loaded, section, "UnitName", name, error) ||
            !set_text(
                *loaded, section, "Objectname", effective_model(source_section, found->name), error
            )) {
            const std::string detail = error;
            abandon();
            error = request_error(weapons, source, name, detail);
            return -1;
        }
        for (const HeldField& field : held) {
            if (!apply_field(collections, weapons, *loaded, section, name, field, error)) {
                const std::string detail = error;
                abandon();
                error = request_error(weapons, source, name, detail);
                return -1;
            }
        }
    } else {
        tdf::Block* root = tdf::editable_root(loaded->document.get());
        tdf::Block* section = root != nullptr ? tdf::editable_child(root, 0) : nullptr;
        if (section == nullptr || source_section == nullptr) {
            abandon();
            error = request_error(weapons, source, name, "the copy has no weapon");
            return -1;
        }
        if (!copy_properties(loaded->document.get(), section, source_section, error) ||
            !copy_children(loaded->document.get(), section, source_section, error) ||
            !set_text(*loaded, section, "ID", std::to_string(weapon_id), error)) {
            const std::string detail = error;
            abandon();
            error = request_error(weapons, source, name, detail);
            return -1;
        }
        for (const HeldField& field : held) {
            if (!apply_field(collections, weapons, *loaded, section, name, field, error)) {
                const std::string detail = error;
                abandon();
                error = request_error(weapons, source, name, detail);
                return -1;
            }
        }
    }

    publish(collections, weapons, name, source, weapon_id, path, std::move(pending));
    return 0;
}

} // namespace oa::data::mutators
