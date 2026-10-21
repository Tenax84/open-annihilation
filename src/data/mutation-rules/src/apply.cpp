// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// One mutator's rules, applied to the documents as they stand. Sources run
// in plan order. Blocks and changes run in file order. Each need reads the
// records the earlier rules left, and each change is written before the next
// one is read. A lock sets the value and marks the field so a later mutator
// leaves it alone.
#include "oa/data/mutation_rules/apply.hpp"

#include "oa/formats/tdf/edit.hpp"
#include "oa/formats/tdf/number.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::data::mutation_rules {
namespace tdf = oa::formats::tdf;
namespace sm = oa::formats::soft_mutator;
namespace defs = oa::data::defs;

namespace {

/// Base directory names, in `DataDirectory` order. A rule folder whose first
/// segment is one of these means the active layout's folder of that kind.
constexpr const char* k_base_directories[] = {
    "units",
    "weapons",
    "gamedata",
    "ai",
    "guis",
    "unitpics",
    "download",
};

static_assert(
    sizeof(k_base_directories) / sizeof(k_base_directories[0]) == defs::data_directory_count
);

/// One field a change is about to write.
struct Place {
    size_t doc_index{};
    uint32_t section_index{};
    bool inner{};
    uint32_t child_index{};
    TargetKind kind{};
    std::string path;
    std::string record;
    std::string block;
    std::string key;
};

/// One record of a document: a unit's `[UNITINFO]`, or a top-level section.
struct RecordHit {
    uint32_t section_index{};
    std::string record;
};

/// Folds one byte to lower case.
///
/// @param byte a byte of a name
/// @return the folded byte
char fold_ascii(char byte) {
    if (byte >= 'A' && byte <= 'Z')
        return static_cast<char>(byte - 'A' + 'a');
    return byte;
}

/// Reports whether two texts are the same without regard to ASCII case.
///
/// @param left a text
/// @param right another text
/// @return true when they fold to the same bytes
bool same_text(std::string_view left, std::string_view right) {
    if (left.size() != right.size())
        return false;
    for (size_t offset = 0; offset < left.size(); ++offset) {
        if (fold_ascii(left[offset]) != fold_ascii(right[offset]))
            return false;
    }
    return true;
}

/// Copies a text folded to lower case.
///
/// @param text a name
/// @return the folded copy
std::string fold_copy(std::string_view text) {
    std::string out;
    out.resize(text.size());
    for (size_t offset = 0; offset < text.size(); ++offset)
        out[offset] = fold_ascii(text[offset]);
    return out;
}

/// Copies a path folded to lower case, with '\\' written as '/'.
///
/// @param text a document path
/// @return the folded copy
std::string fold_path(std::string_view text) {
    std::string out;
    out.resize(text.size());
    for (size_t offset = 0; offset < text.size(); ++offset) {
        const char byte = text[offset] == '\\' ? '/' : text[offset];
        out[offset] = fold_ascii(byte);
    }
    return out;
}

/// Compares two texts by their folded bytes only.
///
/// Two spellings that differ only in ASCII case compare equal.
///
/// @param left a text
/// @param right another text
/// @return negative when `left` sorts first, positive when `right` does, and
///         zero when both fold to the same bytes
int compare_folded(std::string_view left, std::string_view right) {
    const size_t count = left.size() < right.size() ? left.size() : right.size();
    for (size_t offset = 0; offset < count; ++offset) {
        const char left_fold = fold_ascii(left[offset]);
        const char right_fold = fold_ascii(right[offset]);
        if (left_fold < right_fold)
            return -1;
        if (left_fold > right_fold)
            return 1;
    }
    if (left.size() != right.size())
        return left.size() < right.size() ? -1 : 1;
    return 0;
}

/// Orders two texts by their folded bytes, then by the raw bytes.
///
/// @param left a text
/// @param right another text
/// @return true when `left` sorts first
bool less_text(std::string_view left, std::string_view right) {
    const int folded = compare_folded(left, right);
    if (folded != 0)
        return folded < 0;
    return left < right;
}

/// Reports whether a field text starts as a TDF number.
///
/// After spaces and tabs, an optional sign, then either a digit or a point
/// and at least one digit. A lone point or a sign with nothing after it is
/// not a number. An exponent after the digits is allowed because the TDF
/// reader accepts it.
///
/// @param text a field value, or null
/// @return true when a factor can read the text
bool starts_as_number(const char* text) {
    if (text == nullptr)
        return false;
    const auto* cursor = reinterpret_cast<const unsigned char*>(text);
    while (*cursor == ' ' || *cursor == '\t')
        ++cursor;
    if (*cursor == '+' || *cursor == '-')
        ++cursor;
    if (*cursor == '.') {
        ++cursor;
        return *cursor >= '0' && *cursor <= '9';
    }
    return *cursor >= '0' && *cursor <= '9';
}

/// Reports whether the field already holds this number.
///
/// @param current the field text, or null when the field is absent
/// @param value the number a change would write
/// @return true when the text starts as that number
bool same_number(const char* current, double value) {
    return current != nullptr && starts_as_number(current) && tdf::parse_double(current) == value;
}

/// Replaces a rule folder's first segment when it names a base directory.
///
/// @param folder the rule folder, already lower case with '/' separators
/// @param layout the layout whose directory names replace the base names
/// @return the folder documents are listed from
std::string resolve_folder(std::string_view folder, const defs::DataLayout& layout) {
    const auto slash = folder.find('/');
    const std::string_view first =
        slash == std::string_view::npos ? folder : folder.substr(0, slash);
    const std::string_view rest =
        slash == std::string_view::npos ? std::string_view{} : folder.substr(slash);
    for (size_t directory_index = 0; directory_index < defs::data_directory_count;
         ++directory_index) {
        if (first == k_base_directories[directory_index]) {
            std::string resolved = layout.directories[directory_index];
            resolved.append(rest.data(), rest.size());
            return resolved;
        }
    }
    return std::string(folder);
}

/// Replaces the suffix `fbi` with the layout's unit extension.
///
/// @param extension the rule suffix, already lower case
/// @param layout the layout
/// @return the suffix documents are listed with
std::string resolve_extension(std::string_view extension, const defs::DataLayout& layout) {
    if (extension == "fbi")
        return layout.unit_extension;
    return std::string(extension);
}

/// Classifies a resolved target.
///
/// Units are tested before weapons, so a layout that gives both directories
/// the same name still tells a unit file from a weapon file by its suffix.
/// Features use the rule folder, which is not one of the seven base names.
///
/// @param rule_folder the rule folder, lower case
/// @param resolved_folder the layout folder
/// @param resolved_extension the layout suffix
/// @param layout the layout
/// @return the kind
TargetKind kind_of(
    std::string_view rule_folder,
    std::string_view resolved_folder,
    std::string_view resolved_extension,
    const defs::DataLayout& layout
) {
    const size_t units = static_cast<size_t>(defs::DataDirectory::units);
    const size_t weapons = static_cast<size_t>(defs::DataDirectory::weapons);
    const size_t gamedata = static_cast<size_t>(defs::DataDirectory::gamedata);
    if (same_text(resolved_folder, layout.directories[units]) &&
        same_text(resolved_extension, layout.unit_extension))
        return TargetKind::units;
    if (same_text(resolved_folder, layout.directories[weapons]))
        return TargetKind::weapons;
    if (rule_folder == "features" || rule_folder.starts_with("features/"))
        return TargetKind::features;
    if (same_text(resolved_folder, layout.directories[gamedata]) &&
        same_text(resolved_extension, "sidedata.tdf"))
        return TargetKind::sidedata;
    return TargetKind::other;
}

/// Returns one top-level section.
///
/// @param documents the set
/// @param doc_index the document
/// @param section_index the section's position
/// @return the section, or null
const tdf::Block* section_at(DocumentSet& documents, size_t doc_index, uint32_t section_index) {
    return tdf::child_at(documents.at(doc_index).document.root(), section_index);
}

/// The block a change reads, or null when it is no longer there.
///
/// @param documents the set
/// @param doc_index the document
/// @param section_index the record's section
/// @param inner true when the field is inside a child block
/// @param child_index that child's position
/// @return the block, or null
const tdf::Block* live_block(
    DocumentSet& documents,
    size_t doc_index,
    uint32_t section_index,
    bool inner,
    uint32_t child_index
) {
    const tdf::Block* section = section_at(documents, doc_index, section_index);
    if (section == nullptr || !inner)
        return section;
    return tdf::child_at(section, child_index);
}

/// Lists the records of one document.
///
/// A unit file contributes its first `[UNITINFO]` section, named by `UnitName`
/// when that key is written. Any other file contributes every top-level section,
/// named by the section.
///
/// @param loaded the document
/// @param kind the target kind
/// @return the records in section order
std::vector<RecordHit> records_of(const LoadedDocument& loaded, TargetKind kind) {
    std::vector<RecordHit> hits;
    const tdf::Block* root = loaded.document.root();
    const uint32_t sections = tdf::child_count(root);
    if (kind == TargetKind::units) {
        for (uint32_t section_index = 0; section_index < sections; ++section_index) {
            const tdf::Block* section = tdf::child_at(root, section_index);
            if (section == nullptr || section->name == nullptr)
                continue;
            if (!same_text(section->name, "UNITINFO"))
                continue;
            RecordHit hit;
            hit.section_index = section_index;
            const char* unit_name = tdf::find_value(section, "UnitName");
            hit.record = unit_name != nullptr ? unit_name : section->name;
            hits.push_back(std::move(hit));
            break;
        }
        return hits;
    }
    for (uint32_t section_index = 0; section_index < sections; ++section_index) {
        const tdf::Block* section = tdf::child_at(root, section_index);
        if (section == nullptr || section->name == nullptr)
            continue;
        RecordHit hit;
        hit.section_index = section_index;
        hit.record = section->name;
        hits.push_back(std::move(hit));
    }
    return hits;
}

/// Reports whether a block selects a record. Needs read the record as it stands.
///
/// @param documents the set
/// @param doc_index the document
/// @param hit the record
/// @param kind the target kind
/// @param rule the block
/// @param layout the layout, for the movement-class file
/// @return true when the selector matches
bool selected(
    DocumentSet& documents,
    size_t doc_index,
    const RecordHit& hit,
    TargetKind kind,
    const sm::Block& rule,
    const defs::DataLayout& layout
) {
    if (rule.selector == sm::Selector::all)
        return true;
    if (rule.selector == sm::Selector::name)
        return same_text(hit.record, rule.name);
    const tdf::Block* section = section_at(documents, doc_index, hit.section_index);
    for (const sm::Need& need : rule.needs) {
        if (!need_met(section, kind, documents.at(doc_index).path, layout, need))
            return false;
    }
    return true;
}

/// The spelling of a key the block already holds, or null.
///
/// @param target the block, or null
/// @param key_text the field name
/// @return the stored spelling, or null when the key is absent
const char* property_spelling(const tdf::Block* target, std::string_view key_text) {
    if (target == nullptr)
        return nullptr;
    const uint32_t keys = tdf::property_count(target);
    for (uint32_t key_index = 0; key_index < keys; ++key_index) {
        const char* got = tdf::property_key_at(target, static_cast<int32_t>(key_index));
        if (got != nullptr && same_text(got, key_text))
            return got;
    }
    return nullptr;
}

/// The state's id for one field, with its four names folded.
///
/// @param place the field
/// @return the id
FieldId field_id(const Place& place) {
    FieldId id;
    id.path = fold_path(place.path);
    id.record = fold_copy(place.record);
    id.block = fold_copy(place.block);
    id.key = fold_copy(place.key);
    return id;
}

/// Orders writes by path, record, block and key, compared after folding.
///
/// Two writes of one field compare equal whichever spelling of the key each
/// used, so the stable sort keeps them in the order they were applied.
///
/// @param left a write
/// @param right another write
/// @return true when `left` sorts first
bool write_before(const FieldWrite& left, const FieldWrite& right) {
    int order = compare_folded(left.path, right.path);
    if (order == 0)
        order = compare_folded(left.record, right.record);
    if (order == 0)
        order = compare_folded(left.block, right.block);
    if (order == 0)
        order = compare_folded(left.key, right.key);
    return order < 0;
}

/// Orders notes by path, record, key, kind, line, source and message.
///
/// @param left a note
/// @param right another note
/// @return true when `left` sorts first
bool note_before(const RuleNote& left, const RuleNote& right) {
    if (less_text(left.path, right.path))
        return true;
    if (less_text(right.path, left.path))
        return false;
    if (less_text(left.record, right.record))
        return true;
    if (less_text(right.record, left.record))
        return false;
    if (less_text(left.key, right.key))
        return true;
    if (less_text(right.key, left.key))
        return false;
    if (left.kind != right.kind)
        return static_cast<uint8_t>(left.kind) < static_cast<uint8_t>(right.kind);
    if (left.line != right.line)
        return left.line < right.line;
    if (less_text(left.source, right.source))
        return true;
    if (less_text(right.source, left.source))
        return false;
    return less_text(left.message, right.message);
}

/// One note about a field this change touched.
///
/// @param kind which note
/// @param mutator_names the mutators involved, in list order
/// @param source_name the rule source
/// @param line the change's line
/// @param place the field
/// @param message what happened
/// @return the note
RuleNote make_note(
    NoteKind kind,
    std::vector<std::string> mutator_names,
    const std::string& source_name,
    uint32_t line,
    const Place& place,
    std::string message
) {
    RuleNote note;
    note.kind = kind;
    note.mutators = std::move(mutator_names);
    note.source = source_name;
    note.line = line;
    note.path = place.path;
    note.record = place.record;
    note.key = place.key;
    note.message = std::move(message);
    return note;
}

/// A note that names only the mutator of this step.
///
/// @param kind which note
/// @param mutator the mutator
/// @param source_name the rule source
/// @param line the change's line
/// @param place the field
/// @param message what happened
/// @return the note
RuleNote solo_note(
    NoteKind kind,
    const std::string& mutator,
    const std::string& source_name,
    uint32_t line,
    const Place& place,
    std::string message
) {
    std::vector<std::string> names;
    names.push_back(mutator);
    return make_note(kind, std::move(names), source_name, line, place, std::move(message));
}

/// Writes one new text or removal, when the check and the editor allow it.
///
/// @param documents the set
/// @param options the field check
/// @param result the step result
/// @param place the field; its strings outlive the check
/// @param removal true when the field is removed
/// @param text the new text; ignored for a removal
/// @param mutator the mutator writing the field
/// @param current the field's text before this change, or null
/// @return true when the document changed
bool commit_write(
    DocumentSet& documents,
    const ApplyOptions& options,
    ApplyResult& result,
    const Place& place,
    bool removal,
    const std::optional<std::string>& text,
    const std::string& mutator,
    const char* current
) {
    std::optional<std::string> proposed;
    if (!removal && text.has_value())
        proposed = text;
    bool warned = false;
    std::string warning;
    if (options.check) {
        FieldRef ref;
        ref.kind = place.kind;
        ref.path = place.path;
        ref.record = place.record;
        ref.block = place.block;
        ref.key = place.key;
        const CheckResult verdict = options.check(ref, proposed);
        if (verdict.outcome == CheckOutcome::refuse) {
            RuleNote refused;
            refused.kind = NoteKind::check_refused;
            refused.mutators.push_back(mutator);
            refused.path = place.path;
            refused.record = place.record;
            refused.key = place.key;
            refused.message = verdict.reason;
            result.notes.push_back(std::move(refused));
            return false;
        }
        if (verdict.outcome == CheckOutcome::warn) {
            warned = true;
            warning = verdict.reason;
        }
    }
    tdf::Document* document = documents.at(place.doc_index).document.get();
    tdf::Block* root = tdf::editable_root(document);
    tdf::Block* section = tdf::editable_child(root, place.section_index);
    tdf::Block* target = section;
    if (place.inner)
        target = section != nullptr ? tdf::editable_child(section, place.child_index) : nullptr;
    if (target == nullptr || (!removal && !text.has_value())) {
        RuleNote refused;
        refused.kind = NoteKind::check_refused;
        refused.mutators.push_back(mutator);
        refused.path = place.path;
        refused.record = place.record;
        refused.key = place.key;
        refused.message = "the record is no longer in the document";
        result.notes.push_back(std::move(refused));
        return false;
    }
    // The editor keeps the old bytes, but the write must record its own copy.
    const std::optional<std::string> previous =
        current != nullptr ? std::optional<std::string>(std::string(current)) : std::nullopt;
    const tdf::EditStatus edit_status = removal
                                            ? tdf::remove_key(document, target, place.key)
                                            : tdf::set_value(document, target, place.key, *text);
    if (edit_status != tdf::EditStatus::ok) {
        RuleNote refused;
        refused.kind = NoteKind::check_refused;
        refused.mutators.push_back(mutator);
        refused.path = place.path;
        refused.record = place.record;
        refused.key = place.key;
        refused.message = tdf::edit_status_message(edit_status);
        result.notes.push_back(std::move(refused));
        return false;
    }
    FieldWrite done;
    done.mutators.push_back(mutator);
    done.path = place.path;
    done.record = place.record;
    done.block = place.block;
    done.key = place.key;
    done.old_text = previous;
    done.new_text = removal ? std::nullopt : text;
    result.writes.push_back(std::move(done));
    if (warned) {
        RuleNote caution;
        caution.kind = NoteKind::check_warned;
        caution.mutators.push_back(mutator);
        caution.path = place.path;
        caution.record = place.record;
        caution.key = place.key;
        caution.message = std::move(warning);
        result.notes.push_back(std::move(caution));
    }
    return true;
}

/// Notes a replacement and records the writer, then the lock when this change locks.
///
/// @param state lock marks and last writers
/// @param result the step result
/// @param place the field
/// @param id the field's folded id
/// @param mutator the mutator that wrote
/// @param source_name the rule source
/// @param line the change's line
/// @param replaces true when this change replaces a value rather than scaling or appending
/// @param locks true when this change locks the field
void remember_write(
    ApplyState& state,
    ApplyResult& result,
    const Place& place,
    const FieldId& id,
    const std::string& mutator,
    const std::string& source_name,
    uint32_t line,
    bool replaces,
    bool locks
) {
    const auto earlier = state.writers.find(id);
    if (earlier == state.writers.end()) {
        state.writers.emplace(id, mutator);
    } else {
        if (replaces && earlier->second != mutator) {
            std::vector<std::string> names;
            names.push_back(earlier->second);
            names.push_back(mutator);
            std::string message = mutator;
            message += " replaced the value ";
            message += earlier->second;
            message += " wrote";
            result.notes.push_back(make_note(
                NoteKind::value_replaced,
                std::move(names),
                source_name,
                line,
                place,
                std::move(message)
            ));
        }
        earlier->second = mutator;
    }
    if (locks)
        state.locks.insert_or_assign(id, mutator);
}

/// Writes a finite number, or notes that it cannot be written.
///
/// A number equal to the field's current number is left unwritten. A lock
/// still marks the field. A clamp has already been applied by the caller, and
/// only to a finite product.
///
/// @param documents the set
/// @param state lock marks and last writers
/// @param options the field check
/// @param result the step result
/// @param place the field
/// @param id the field's folded id
/// @param current the field text, or null
/// @param value the number to write
/// @param mutator the mutator
/// @param source_name the rule source
/// @param line the change's line
/// @param replaces true when an assignment or a lock replaces a value
/// @param locks true when the change locks the field
void write_number(
    DocumentSet& documents,
    ApplyState& state,
    const ApplyOptions& options,
    ApplyResult& result,
    const Place& place,
    const FieldId& id,
    const char* current,
    double value,
    const std::string& mutator,
    const std::string& source_name,
    uint32_t line,
    bool replaces,
    bool locks
) {
    if (!std::isfinite(value)) {
        result.notes.push_back(solo_note(
            NoteKind::not_finite,
            mutator,
            source_name,
            line,
            place,
            "the value is not finite, so it was not written"
        ));
        return;
    }
    const std::optional<std::string> spelled = tdf::format_number(value);
    if (!spelled.has_value()) {
        result.notes.push_back(solo_note(
            NoteKind::not_finite,
            mutator,
            source_name,
            line,
            place,
            "the value is not finite, so it was not written"
        ));
        return;
    }
    if (same_number(current, value)) {
        if (locks)
            state.locks.insert_or_assign(id, mutator);
        return;
    }
    if (!commit_write(documents, options, result, place, false, spelled, mutator, current))
        return;
    remember_write(state, result, place, id, mutator, source_name, line, replaces, locks);
}

/// Applies one change to one field of one record.
///
/// @param documents the set
/// @param state lock marks and last writers
/// @param options the field check
/// @param result the step result
/// @param mutator the mutator
/// @param source_name the rule source
/// @param doc_index the document
/// @param hit the record
/// @param kind the target kind
/// @param inner true when the field is inside a child block
/// @param child_index that child's position
/// @param block_spell the child block's name; empty for a key of the record
/// @param change the change
void apply_change(
    DocumentSet& documents,
    ApplyState& state,
    const ApplyOptions& options,
    ApplyResult& result,
    const std::string& mutator,
    const std::string& source_name,
    size_t doc_index,
    const RecordHit& hit,
    TargetKind kind,
    bool inner,
    uint32_t child_index,
    const std::string& block_spell,
    const sm::Change& change
) {
    const tdf::Block* target =
        live_block(documents, doc_index, hit.section_index, inner, child_index);
    Place place;
    place.doc_index = doc_index;
    place.section_index = hit.section_index;
    place.inner = inner;
    place.child_index = inner ? child_index : 0;
    place.kind = kind;
    place.path = documents.at(doc_index).path;
    place.record = hit.record;
    place.block = block_spell;
    place.key = change.field;
    if (target == nullptr) {
        result.notes.push_back(solo_note(
            NoteKind::check_refused,
            mutator,
            source_name,
            change.line,
            place,
            "the record is no longer in the document"
        ));
        return;
    }
    const char* existing = property_spelling(target, change.field);
    if (existing != nullptr)
        place.key = existing;
    const char* current = tdf::find_value(target, place.key.c_str());
    const FieldId id = field_id(place);
    const auto locker = state.locks.find(id);
    if (locker != state.locks.end() && locker->second != mutator) {
        std::vector<std::string> names;
        names.push_back(locker->second);
        names.push_back(mutator);
        std::string message = locker->second;
        message += " locked ";
        message += place.key;
        message += ", so ";
        message += mutator;
        message += " left it alone";
        result.notes.push_back(make_note(
            NoteKind::field_locked,
            std::move(names),
            source_name,
            change.line,
            place,
            std::move(message)
        ));
        return;
    }

    const bool scale =
        change.kind == sm::ChangeKind::scale || change.kind == sm::ChangeKind::scale_clamp;
    if (scale) {
        if (current == nullptr) {
            result.notes.push_back(solo_note(
                NoteKind::factor_on_absent_field,
                mutator,
                source_name,
                change.line,
                place,
                "a factor did nothing because the field is absent"
            ));
            return;
        }
        if (!starts_as_number(current)) {
            result.notes.push_back(solo_note(
                NoteKind::not_a_number,
                mutator,
                source_name,
                change.line,
                place,
                "the field text is not a number"
            ));
            return;
        }
        double value = tdf::parse_double(current) * change.value;
        if (!std::isfinite(value)) {
            result.notes.push_back(solo_note(
                NoteKind::not_finite,
                mutator,
                source_name,
                change.line,
                place,
                "the value is not finite, so it was not written"
            ));
            return;
        }
        if (change.kind == sm::ChangeKind::scale_clamp) {
            if (value < change.low)
                value = change.low;
            else if (value > change.high)
                value = change.high;
        }
        write_number(
            documents,
            state,
            options,
            result,
            place,
            id,
            current,
            value,
            mutator,
            source_name,
            change.line,
            false,
            false
        );
        return;
    }

    if (change.kind == sm::ChangeKind::assign || change.kind == sm::ChangeKind::meta) {
        write_number(
            documents,
            state,
            options,
            result,
            place,
            id,
            current,
            change.value,
            mutator,
            source_name,
            change.line,
            true,
            change.kind == sm::ChangeKind::meta
        );
        return;
    }

    if (change.kind == sm::ChangeKind::remove) {
        if (current == nullptr)
            return;
        if (!commit_write(documents, options, result, place, true, std::nullopt, mutator, current))
            return;
        remember_write(state, result, place, id, mutator, source_name, change.line, true, false);
        return;
    }

    if (change.kind == sm::ChangeKind::assign_text) {
        if (current != nullptr && change.text == current)
            return;
        const std::optional<std::string> text = change.text;
        if (!commit_write(documents, options, result, place, false, text, mutator, current))
            return;
        remember_write(state, result, place, id, mutator, source_name, change.line, true, false);
        return;
    }

    std::string built = current != nullptr ? std::string(current) : std::string();
    built += change.text;
    if (current == nullptr && built.empty())
        return;
    if (current != nullptr && built == current)
        return;
    const std::optional<std::string> text = std::move(built);
    if (!commit_write(documents, options, result, place, false, text, mutator, current))
        return;
    remember_write(state, result, place, id, mutator, source_name, change.line, false, false);
}

/// Applies a depth change to each key of each block directly inside the record.
///
/// Two spellings of one key are one field. The reader keeps the newest first,
/// and that is the value that is changed.
///
/// @param documents the set
/// @param state lock marks and last writers
/// @param options the field check
/// @param result the step result
/// @param mutator the mutator
/// @param source_name the rule source
/// @param doc_index the document
/// @param hit the record
/// @param kind the target kind
/// @param change the depth change
void apply_depth(
    DocumentSet& documents,
    ApplyState& state,
    const ApplyOptions& options,
    ApplyResult& result,
    const std::string& mutator,
    const std::string& source_name,
    size_t doc_index,
    const RecordHit& hit,
    TargetKind kind,
    const sm::Change& change
) {
    const tdf::Block* section = section_at(documents, doc_index, hit.section_index);
    if (section == nullptr)
        return;

    struct Item {
        uint32_t child_index{};
        std::string block;
        std::string key;
    };

    std::vector<Item> items;
    const uint32_t children = tdf::child_count(section);
    for (uint32_t child_index = 0; child_index < children; ++child_index) {
        const tdf::Block* inner = tdf::child_at(section, child_index);
        if (inner == nullptr)
            continue;
        const std::string block_spell = inner->name != nullptr ? inner->name : "";
        const uint32_t key_count = tdf::property_count(inner);
        std::vector<std::string> taken_keys;
        taken_keys.reserve(key_count);
        for (uint32_t key_index = 0; key_index < key_count; ++key_index) {
            const char* key_text = tdf::property_key_at(inner, static_cast<int32_t>(key_index));
            if (key_text == nullptr)
                continue;
            bool repeated = false;
            for (const std::string& earlier : taken_keys) {
                if (same_text(earlier, key_text)) {
                    repeated = true;
                    break;
                }
            }
            if (repeated)
                continue;
            taken_keys.emplace_back(key_text);
            Item item;
            item.child_index = child_index;
            item.block = block_spell;
            item.key = key_text;
            items.push_back(std::move(item));
        }
    }
    for (const Item& item : items) {
        sm::Change one = change;
        one.all_depth2 = false;
        one.field = item.key;
        apply_change(
            documents,
            state,
            options,
            result,
            mutator,
            source_name,
            doc_index,
            hit,
            kind,
            true,
            item.child_index,
            item.block,
            one
        );
    }
}

} // namespace

std::vector<sm::Target> targets_of(std::span<const MutatorRules> mutators) {
    std::vector<sm::Target> found;
    for (const MutatorRules& rules : mutators) {
        for (const RuleSource& source : rules.sources) {
            for (const sm::Block& rule : source.rules.blocks) {
                bool seen = false;
                for (const sm::Target& have : found) {
                    if (have == rule.target) {
                        seen = true;
                        break;
                    }
                }
                if (!seen)
                    found.push_back(rule.target);
            }
        }
    }
    return found;
}

ApplyResult apply_step(
    const MutatorRules& rules,
    DocumentSet& documents,
    ApplyState& state,
    const ApplyOptions& options
) {
    const defs::DataLayout& layout =
        options.layout != nullptr ? *options.layout : defs::data_layout();
    ApplyResult result;
    for (const RuleSource& source : rules.sources) {
        for (const sm::Block& rule : source.rules.blocks) {
            const std::string resolved_folder = resolve_folder(rule.target.folder, layout);
            const std::string resolved_extension = resolve_extension(rule.target.extension, layout);
            const TargetKind kind =
                kind_of(rule.target.folder, resolved_folder, resolved_extension, layout);
            const std::vector<size_t> docs =
                documents.documents_in(resolved_folder, resolved_extension, false);
            bool matched = false;
            for (const size_t doc_index : docs) {
                const std::vector<RecordHit> hits = records_of(documents.at(doc_index), kind);
                for (const RecordHit& hit : hits) {
                    if (!selected(documents, doc_index, hit, kind, rule, layout))
                        continue;
                    matched = true;
                    for (const sm::Change& change : rule.changes) {
                        if (change.all_depth2) {
                            apply_depth(
                                documents,
                                state,
                                options,
                                result,
                                rules.mutator,
                                source.name,
                                doc_index,
                                hit,
                                kind,
                                change
                            );
                            continue;
                        }
                        apply_change(
                            documents,
                            state,
                            options,
                            result,
                            rules.mutator,
                            source.name,
                            doc_index,
                            hit,
                            kind,
                            false,
                            0,
                            {},
                            change
                        );
                    }
                }
            }
            if (!matched) {
                RuleNote missed;
                missed.kind = NoteKind::matched_nothing;
                missed.mutators.push_back(rules.mutator);
                missed.source = source.name;
                missed.line = rule.line;
                missed.message = "the block matched no record";
                result.notes.push_back(std::move(missed));
            }
        }
    }
    std::stable_sort(result.writes.begin(), result.writes.end(), write_before);
    std::stable_sort(result.notes.begin(), result.notes.end(), note_before);
    return result;
}

} // namespace oa::data::mutation_rules
