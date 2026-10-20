// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// One pass over every mutator's rule records. Needs are decided on the
// documents as they stand. Each mutator's changes then fold into one
// contribution per field, the contributions combine, and the writes happen
// last, so no change is visible to a need.
#include "oa/data/mutation_rules/apply.hpp"

#include "oa/formats/tdf/edit.hpp"
#include "oa/formats/tdf/number.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
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

/// How one mutator's text contribution writes a field.
enum class TextOp : uint8_t { none, assign, append, remove };

/// One mutator's folded contribution to one field.
struct Contrib {
    bool used{};
    bool numeric{};
    bool has_meta{};
    double meta{};
    bool has_assignment{};
    double assignment{};
    bool has_factor{};
    double factor{1};
    bool has_clamp{};
    double clamp_low{};
    double clamp_high{};
    TextOp text_op{};
    std::string text;
    std::string source;
    uint32_t line{};
};

/// Identity of a field while contributions are collected.
struct FieldKey {
    size_t doc_index{};
    uint32_t section_index{};
    bool inner{};
    uint32_t child_index{};
    std::string block_fold;
    std::string key_fold;

    /// Orders keys so a map can find a field again.
    ///
    /// @param other another key
    /// @return true when this key sorts first
    bool operator<(const FieldKey& other) const {
        if (doc_index != other.doc_index)
            return doc_index < other.doc_index;
        if (section_index != other.section_index)
            return section_index < other.section_index;
        if (inner != other.inner)
            return !inner && other.inner;
        if (child_index != other.child_index)
            return child_index < other.child_index;
        if (block_fold != other.block_fold)
            return block_fold < other.block_fold;
        return key_fold < other.key_fold;
    }
};

/// One field any mutator touched, with a contribution slot per mutator.
struct Slot {
    bool ready{};
    size_t doc_index{};
    uint32_t section_index{};
    bool inner{};
    uint32_t child_index{};
    TargetKind kind{};
    std::string path;
    std::string record;
    std::string block;
    std::string key;
    std::vector<Contrib> contribs;
};

/// A write decided by the algebra, not yet applied.
struct Planned {
    size_t doc_index{};
    uint32_t section_index{};
    bool inner{};
    uint32_t child_index{};
    TargetKind kind{};
    std::string path;
    std::string record;
    std::string block;
    std::string key;
    bool removal{};
    std::optional<std::string> old_text;
    std::optional<std::string> new_text;
    std::vector<std::string> mutators;
    std::vector<RuleNote> when_written;
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

/// Orders two texts by their folded bytes, then by the raw bytes.
///
/// @param left a text
/// @param right another text
/// @return true when `left` sorts first
bool less_text(std::string_view left, std::string_view right) {
    const size_t count = left.size() < right.size() ? left.size() : right.size();
    for (size_t offset = 0; offset < count; ++offset) {
        const char left_fold = fold_ascii(left[offset]);
        const char right_fold = fold_ascii(right[offset]);
        if (left_fold < right_fold)
            return true;
        if (left_fold > right_fold)
            return false;
    }
    if (left.size() != right.size())
        return left.size() < right.size();
    return left < right;
}

/// Reports whether a field text starts as a TDF number.
///
/// After spaces and tabs, an optional sign and at least one digit. A leading
/// point is not a number here, and an exponent after the digits is allowed
/// because the TDF reader accepts it.
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
    return *cursor >= '0' && *cursor <= '9';
}

/// Spells a finite number, or `0` when the number has no spelling.
///
/// Clamp notes name ranges that were finite when the rule was read.
///
/// @param number a bound or a combined value
/// @return the TDF spelling
std::string spell_or_zero(double number) {
    const std::optional<std::string> spelled = tdf::format_number(number);
    if (spelled.has_value())
        return *spelled;
    return "0";
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

/// Reports whether a block selects a record. Needs read the record as found.
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

/// Clears the numeric half of a contribution.
///
/// @param contrib the contribution
void clear_number(Contrib& contrib) {
    contrib.numeric = false;
    contrib.has_meta = false;
    contrib.meta = 0;
    contrib.has_assignment = false;
    contrib.assignment = 0;
    contrib.has_factor = false;
    contrib.factor = 1;
    contrib.has_clamp = false;
    contrib.clamp_low = 0;
    contrib.clamp_high = 0;
}

/// Folds one numeric change into a contribution.
///
/// A meta value replaces the numeric state and later numeric changes do not
/// compose with it. An assignment resets factors and clamps. A scale multiplies.
/// A clamped scale also joins the clamp, taking the lower low and the higher high.
///
/// @param contrib the contribution
/// @param change the change
/// @return false when a meta value already occupies the contribution and this change is not a meta value
bool apply_number(Contrib& contrib, const sm::Change& change) {
    if (change.kind == sm::ChangeKind::meta) {
        clear_number(contrib);
        contrib.numeric = true;
        contrib.has_meta = true;
        contrib.meta = change.value;
        return true;
    }
    if (contrib.has_meta)
        return false;
    if (change.kind == sm::ChangeKind::assign) {
        contrib.has_assignment = true;
        contrib.assignment = change.value;
        contrib.has_factor = false;
        contrib.factor = 1;
        contrib.has_clamp = false;
        contrib.clamp_low = 0;
        contrib.clamp_high = 0;
        return true;
    }
    if (!contrib.has_factor) {
        contrib.has_factor = true;
        contrib.factor = change.value;
    } else {
        contrib.factor *= change.value;
    }
    if (change.kind == sm::ChangeKind::scale_clamp) {
        if (!contrib.has_clamp) {
            contrib.has_clamp = true;
            contrib.clamp_low = change.low;
            contrib.clamp_high = change.high;
        } else {
            if (change.low < contrib.clamp_low)
                contrib.clamp_low = change.low;
            if (change.high > contrib.clamp_high)
                contrib.clamp_high = change.high;
        }
    }
    return true;
}

/// Folds one text change into a contribution.
///
/// An assignment replaces the text. A removal clears it. An append after a
/// removal becomes an assignment of the appended text; after an assignment it
/// extends that assignment; otherwise it stays an append.
///
/// @param contrib the contribution
/// @param change the change
void apply_text(Contrib& contrib, const sm::Change& change) {
    if (change.kind == sm::ChangeKind::assign_text) {
        contrib.text_op = TextOp::assign;
        contrib.text = change.text;
        return;
    }
    if (change.kind == sm::ChangeKind::remove) {
        contrib.text_op = TextOp::remove;
        contrib.text.clear();
        return;
    }
    if (contrib.text_op == TextOp::remove) {
        contrib.text_op = TextOp::assign;
        contrib.text = change.text;
        return;
    }
    if (contrib.text_op == TextOp::assign) {
        contrib.text += change.text;
        return;
    }
    contrib.text_op = TextOp::append;
    contrib.text += change.text;
}

/// Folds one change into one mutator's contribution, discarding the other kind.
///
/// @param contrib the contribution
/// @param change the change
/// @param source_name the rule source the change came from
void fold_one(Contrib& contrib, const sm::Change& change, const std::string& source_name) {
    const bool as_text = change.kind == sm::ChangeKind::assign_text ||
                         change.kind == sm::ChangeKind::remove ||
                         change.kind == sm::ChangeKind::append;
    bool took = false;
    if (as_text) {
        clear_number(contrib);
        apply_text(contrib, change);
        took = true;
    } else {
        if (contrib.text_op != TextOp::none) {
            contrib.text_op = TextOp::none;
            contrib.text.clear();
        }
        contrib.numeric = true;
        took = apply_number(contrib, change);
    }
    contrib.used = true;
    if (took) {
        contrib.source = source_name;
        contrib.line = change.line;
    }
}

/// The field's current text, or null when the key is absent.
///
/// @param documents the set
/// @param slot the field
/// @return the text, or null
const char* current_of(DocumentSet& documents, const Slot& slot) {
    const tdf::Block* section = section_at(documents, slot.doc_index, slot.section_index);
    if (section == nullptr)
        return nullptr;
    const tdf::Block* target = section;
    if (slot.inner) {
        target = tdf::child_at(section, slot.child_index);
        if (target == nullptr)
            return nullptr;
    }
    return tdf::find_value(target, slot.key.c_str());
}

/// Orders slots by path, record, block and key.
///
/// @param left a field
/// @param right another field
/// @return true when `left` sorts first
bool slot_before(const Slot* left, const Slot* right) {
    if (less_text(left->path, right->path))
        return true;
    if (less_text(right->path, left->path))
        return false;
    if (less_text(left->record, right->record))
        return true;
    if (less_text(right->record, left->record))
        return false;
    if (less_text(left->block, right->block))
        return true;
    if (less_text(right->block, left->block))
        return false;
    return less_text(left->key, right->key);
}

/// Orders writes by path, record, block and key.
///
/// @param left a write
/// @param right another write
/// @return true when `left` sorts first
bool write_before(const FieldWrite& left, const FieldWrite& right) {
    if (less_text(left.path, right.path))
        return true;
    if (less_text(right.path, left.path))
        return false;
    if (less_text(left.record, right.record))
        return true;
    if (less_text(right.record, left.record))
        return false;
    if (less_text(left.block, right.block))
        return true;
    if (less_text(right.block, left.block))
        return false;
    return less_text(left.key, right.key);
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

/// A note about one field.
///
/// @param kind which note
/// @param mutator_names the mutators involved, in list order
/// @param slot the field
/// @param message what happened
/// @return the note, with no single source
RuleNote field_note(
    NoteKind kind, std::vector<std::string> mutator_names, const Slot& slot, std::string message
) {
    RuleNote note;
    note.kind = kind;
    note.mutators = std::move(mutator_names);
    note.path = slot.path;
    note.record = slot.record;
    note.key = slot.key;
    note.message = std::move(message);
    return note;
}

/// Fills a note's source when exactly one contribution is involved.
///
/// @param note the note
/// @param contribs every mutator's contribution
/// @param involved the mutator positions on the note
void fill_one_source(
    RuleNote& note, const std::vector<Contrib>& contribs, const std::vector<size_t>& involved
) {
    if (involved.size() != 1)
        return;
    const Contrib& contrib = contribs[involved[0]];
    note.source = contrib.source;
    note.line = contrib.line;
}

/// The mutator names at the given positions.
///
/// @param involved mutator positions, in list order
/// @param mutators the mutator list
/// @return the names
std::vector<std::string>
names_of(const std::vector<size_t>& involved, std::span<const MutatorRules> mutators) {
    std::vector<std::string> names;
    names.reserve(involved.size());
    for (const size_t mutator_index : involved)
        names.push_back(mutators[mutator_index].mutator);
    return names;
}

/// Combines one field's contributions into a planned write and its notes.
///
/// @param slot the field
/// @param current the field's text, or null when it is absent
/// @param mutators the mutator list
/// @param planned writes still to apply
/// @param notes notes that do not wait on the write
void combine_slot(
    const Slot& slot,
    const char* current,
    std::span<const MutatorRules> mutators,
    std::vector<Planned>& planned,
    std::vector<RuleNote>& notes
) {
    std::vector<size_t> metas;
    std::vector<size_t> plain;
    std::vector<size_t> text_ones;
    std::vector<size_t> appends;
    std::vector<size_t> non_appends;
    for (size_t mutator_index = 0; mutator_index < slot.contribs.size(); ++mutator_index) {
        const Contrib& contrib = slot.contribs[mutator_index];
        if (!contrib.used)
            continue;
        if (contrib.numeric && contrib.has_meta) {
            metas.push_back(mutator_index);
            continue;
        }
        if (contrib.numeric &&
            (contrib.has_assignment || contrib.has_factor || contrib.has_clamp)) {
            plain.push_back(mutator_index);
            continue;
        }
        if (contrib.text_op == TextOp::none)
            continue;
        text_ones.push_back(mutator_index);
        if (contrib.text_op == TextOp::append)
            appends.push_back(mutator_index);
        else
            non_appends.push_back(mutator_index);
    }

    const bool number_wins = !metas.empty() || !plain.empty();
    if (number_wins && !text_ones.empty()) {
        RuleNote dropped = field_note(
            NoteKind::text_cancelled_by_number,
            names_of(text_ones, mutators),
            slot,
            "a number change dropped the text changes"
        );
        fill_one_source(dropped, slot.contribs, text_ones);
        notes.push_back(std::move(dropped));
    }

    auto old_text = [&]() {
        std::optional<std::string> previous;
        if (current != nullptr)
            previous = std::string(current);
        return previous;
    };

    if (!metas.empty()) {
        double sum = 0;
        for (const size_t mutator_index : metas)
            sum += slot.contribs[mutator_index].meta;
        const double result_number = sum / static_cast<double>(metas.size());
        if (!std::isfinite(result_number)) {
            RuleNote failed = field_note(
                NoteKind::not_finite,
                names_of(metas, mutators),
                slot,
                "the combined value is not finite"
            );
            fill_one_source(failed, slot.contribs, metas);
            notes.push_back(std::move(failed));
            return;
        }
        const std::optional<std::string> spelled = tdf::format_number(result_number);
        if (!spelled.has_value()) {
            notes.push_back(field_note(
                NoteKind::not_finite,
                names_of(metas, mutators),
                slot,
                "the combined value is not finite"
            ));
            return;
        }
        if (current != nullptr && starts_as_number(current) &&
            tdf::parse_double(current) == result_number)
            return;
        Planned write;
        write.doc_index = slot.doc_index;
        write.section_index = slot.section_index;
        write.inner = slot.inner;
        write.child_index = slot.child_index;
        write.kind = slot.kind;
        write.path = slot.path;
        write.record = slot.record;
        write.block = slot.block;
        write.key = slot.key;
        write.old_text = old_text();
        write.new_text = *spelled;
        write.mutators = names_of(metas, mutators);
        planned.push_back(std::move(write));
        return;
    }

    if (!plain.empty()) {
        bool creates = false;
        for (const size_t mutator_index : plain) {
            if (slot.contribs[mutator_index].has_assignment)
                creates = true;
        }
        if (!creates && current == nullptr) {
            RuleNote absent = field_note(
                NoteKind::factor_on_absent_field,
                names_of(plain, mutators),
                slot,
                "a factor did nothing because the field is absent"
            );
            fill_one_source(absent, slot.contribs, plain);
            notes.push_back(std::move(absent));
            return;
        }
        if (!creates && !starts_as_number(current)) {
            RuleNote skipped = field_note(
                NoteKind::not_a_number,
                names_of(plain, mutators),
                slot,
                "the field text is not a number"
            );
            fill_one_source(skipped, slot.contribs, plain);
            notes.push_back(std::move(skipped));
            return;
        }
        double base = 0;
        uint32_t assign_count = 0;
        if (creates) {
            double sum = 0;
            for (const size_t mutator_index : plain) {
                const Contrib& contrib = slot.contribs[mutator_index];
                if (!contrib.has_assignment)
                    continue;
                sum += contrib.assignment;
                ++assign_count;
            }
            base = sum / static_cast<double>(assign_count);
        } else {
            base = tdf::parse_double(current);
        }
        double product = 1;
        bool any_clamp = false;
        double bound_low = 0;
        double bound_high = 0;
        std::vector<size_t> writers;
        for (const size_t mutator_index : plain) {
            const Contrib& contrib = slot.contribs[mutator_index];
            if (contrib.has_factor)
                product *= contrib.factor;
            if (contrib.has_clamp) {
                if (!any_clamp) {
                    any_clamp = true;
                    bound_low = contrib.clamp_low;
                    bound_high = contrib.clamp_high;
                } else {
                    if (contrib.clamp_low < bound_low)
                        bound_low = contrib.clamp_low;
                    if (contrib.clamp_high > bound_high)
                        bound_high = contrib.clamp_high;
                }
            }
            if (contrib.has_assignment || contrib.has_factor || contrib.has_clamp)
                writers.push_back(mutator_index);
        }
        double result_number = base * product;
        if (!std::isfinite(result_number)) {
            RuleNote failed = field_note(
                NoteKind::not_finite,
                names_of(writers, mutators),
                slot,
                "the combined value is not finite"
            );
            fill_one_source(failed, slot.contribs, writers);
            notes.push_back(std::move(failed));
            return;
        }
        if (any_clamp) {
            if (result_number < bound_low)
                result_number = bound_low;
            else if (result_number > bound_high)
                result_number = bound_high;
        }
        if (!std::isfinite(result_number)) {
            notes.push_back(field_note(
                NoteKind::not_finite,
                names_of(writers, mutators),
                slot,
                "the combined value is not finite"
            ));
            return;
        }
        const std::optional<std::string> spelled = tdf::format_number(result_number);
        if (!spelled.has_value()) {
            notes.push_back(field_note(
                NoteKind::not_finite,
                names_of(writers, mutators),
                slot,
                "the combined value is not finite"
            ));
            return;
        }
        if (current != nullptr && starts_as_number(current) &&
            tdf::parse_double(current) == result_number)
            return;
        Planned write;
        write.doc_index = slot.doc_index;
        write.section_index = slot.section_index;
        write.inner = slot.inner;
        write.child_index = slot.child_index;
        write.kind = slot.kind;
        write.path = slot.path;
        write.record = slot.record;
        write.block = slot.block;
        write.key = slot.key;
        write.old_text = old_text();
        write.new_text = *spelled;
        write.mutators = names_of(writers, mutators);
        if (any_clamp) {
            for (const size_t mutator_index : plain) {
                const Contrib& contrib = slot.contribs[mutator_index];
                if (!contrib.has_clamp)
                    continue;
                if (result_number >= contrib.clamp_low && result_number <= contrib.clamp_high)
                    continue;
                RuleNote widened;
                widened.kind = NoteKind::clamp_widened;
                widened.mutators.push_back(mutators[mutator_index].mutator);
                widened.source = contrib.source;
                widened.line = contrib.line;
                widened.path = slot.path;
                widened.record = slot.record;
                widened.key = slot.key;
                widened.message = mutators[mutator_index].mutator;
                widened.message += " clamped ";
                widened.message += spell_or_zero(contrib.clamp_low);
                widened.message += "..";
                widened.message += spell_or_zero(contrib.clamp_high);
                widened.message += "; the value ";
                widened.message += *spelled;
                widened.message += " used ";
                widened.message += spell_or_zero(bound_low);
                widened.message += "..";
                widened.message += spell_or_zero(bound_high);
                write.when_written.push_back(std::move(widened));
            }
        }
        planned.push_back(std::move(write));
        return;
    }

    if (appends.empty() && non_appends.size() > 1) {
        RuleNote cancelled = field_note(
            NoteKind::text_assignments_cancel,
            names_of(non_appends, mutators),
            slot,
            "text assignments cancel, so the field is unchanged"
        );
        fill_one_source(cancelled, slot.contribs, non_appends);
        notes.push_back(std::move(cancelled));
        return;
    }
    if (!appends.empty() && !non_appends.empty()) {
        RuleNote lost = field_note(
            NoteKind::text_assignment_lost_to_append,
            names_of(non_appends, mutators),
            slot,
            "an assignment or removal lost to an append"
        );
        fill_one_source(lost, slot.contribs, non_appends);
        notes.push_back(std::move(lost));
    }

    if (!appends.empty()) {
        std::string built = current != nullptr ? std::string(current) : std::string{};
        for (const size_t mutator_index : appends)
            built += slot.contribs[mutator_index].text;
        if (current != nullptr && built == current)
            return;
        if (current == nullptr && built.empty())
            return;
        Planned write;
        write.doc_index = slot.doc_index;
        write.section_index = slot.section_index;
        write.inner = slot.inner;
        write.child_index = slot.child_index;
        write.kind = slot.kind;
        write.path = slot.path;
        write.record = slot.record;
        write.block = slot.block;
        write.key = slot.key;
        write.old_text = old_text();
        write.new_text = std::move(built);
        write.mutators = names_of(appends, mutators);
        planned.push_back(std::move(write));
        return;
    }

    if (non_appends.size() != 1)
        return;
    const size_t mutator_index = non_appends[0];
    const Contrib& contrib = slot.contribs[mutator_index];
    if (contrib.text_op == TextOp::remove) {
        if (current == nullptr)
            return;
        Planned write;
        write.doc_index = slot.doc_index;
        write.section_index = slot.section_index;
        write.inner = slot.inner;
        write.child_index = slot.child_index;
        write.kind = slot.kind;
        write.path = slot.path;
        write.record = slot.record;
        write.block = slot.block;
        write.key = slot.key;
        write.removal = true;
        write.old_text = old_text();
        write.mutators = names_of(non_appends, mutators);
        planned.push_back(std::move(write));
        return;
    }
    if (current != nullptr && contrib.text == current)
        return;
    Planned write;
    write.doc_index = slot.doc_index;
    write.section_index = slot.section_index;
    write.inner = slot.inner;
    write.child_index = slot.child_index;
    write.kind = slot.kind;
    write.path = slot.path;
    write.record = slot.record;
    write.block = slot.block;
    write.key = slot.key;
    write.old_text = old_text();
    write.new_text = contrib.text;
    write.mutators = names_of(non_appends, mutators);
    planned.push_back(std::move(write));
}

/// Applies one planned write, recording a refusal or a warning.
///
/// @param documents the set
/// @param write the planned write; its strings outlive the check
/// @param options the field check
/// @param result the pass result
void commit_write(
    DocumentSet& documents, Planned& write, const ApplyOptions& options, ApplyResult& result
) {
    std::optional<std::string> proposed;
    if (!write.removal && write.new_text.has_value())
        proposed = write.new_text;
    bool warned = false;
    std::string warning;
    if (options.check) {
        FieldRef ref;
        ref.kind = write.kind;
        ref.path = write.path;
        ref.record = write.record;
        ref.block = write.block;
        ref.key = write.key;
        const CheckResult verdict = options.check(ref, proposed);
        if (verdict.outcome == CheckOutcome::refuse) {
            RuleNote refused;
            refused.kind = NoteKind::check_refused;
            refused.mutators = write.mutators;
            refused.path = write.path;
            refused.record = write.record;
            refused.key = write.key;
            refused.message = verdict.reason;
            result.notes.push_back(std::move(refused));
            return;
        }
        if (verdict.outcome == CheckOutcome::warn) {
            warned = true;
            warning = verdict.reason;
        }
    }
    tdf::Document* document = documents.at(write.doc_index).document.get();
    tdf::Block* root = tdf::editable_root(document);
    tdf::Block* section = tdf::editable_child(root, write.section_index);
    tdf::Block* target = section;
    if (write.inner)
        target = section != nullptr ? tdf::editable_child(section, write.child_index) : nullptr;
    if (target == nullptr || (!write.removal && !write.new_text.has_value())) {
        RuleNote refused;
        refused.kind = NoteKind::check_refused;
        refused.mutators = write.mutators;
        refused.path = write.path;
        refused.record = write.record;
        refused.key = write.key;
        refused.message = "the record is no longer in the document";
        result.notes.push_back(std::move(refused));
        return;
    }
    const tdf::EditStatus status =
        write.removal ? tdf::remove_key(document, target, write.key)
                      : tdf::set_value(document, target, write.key, *write.new_text);
    if (status != tdf::EditStatus::ok) {
        RuleNote refused;
        refused.kind = NoteKind::check_refused;
        refused.mutators = write.mutators;
        refused.path = write.path;
        refused.record = write.record;
        refused.key = write.key;
        refused.message = tdf::edit_status_message(status);
        result.notes.push_back(std::move(refused));
        return;
    }
    FieldWrite done;
    done.mutators = write.mutators;
    done.path = write.path;
    done.record = write.record;
    done.block = write.block;
    done.key = write.key;
    done.old_text = std::move(write.old_text);
    done.new_text = write.removal ? std::nullopt : write.new_text;
    result.writes.push_back(std::move(done));
    for (RuleNote& extra : write.when_written)
        result.notes.push_back(std::move(extra));
    if (warned) {
        RuleNote caution;
        caution.kind = NoteKind::check_warned;
        caution.mutators = result.writes.back().mutators;
        caution.path = result.writes.back().path;
        caution.record = result.writes.back().record;
        caution.key = result.writes.back().key;
        caution.message = std::move(warning);
        result.notes.push_back(std::move(caution));
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

ApplyResult apply_rules(
    std::span<const MutatorRules> mutators, DocumentSet& documents, const ApplyOptions& options
) {
    const defs::DataLayout& layout =
        options.layout != nullptr ? *options.layout : defs::data_layout();
    ApplyResult result;
    std::map<FieldKey, Slot> slots;

    auto ensure = [&](const FieldKey& key,
                      size_t doc_index,
                      const RecordHit& hit,
                      TargetKind kind,
                      std::string block_spell,
                      std::string key_spell) -> Slot& {
        Slot& slot = slots[key];
        if (!slot.ready) {
            slot.ready = true;
            slot.doc_index = doc_index;
            slot.section_index = hit.section_index;
            slot.inner = key.inner;
            slot.child_index = key.child_index;
            slot.kind = kind;
            slot.path = documents.at(doc_index).path;
            slot.record = hit.record;
            slot.block = std::move(block_spell);
            slot.key = std::move(key_spell);
            slot.contribs.resize(mutators.size());
        }
        return slot;
    };

    auto fold_field = [&](size_t mutator_index,
                          const std::string& source_name,
                          size_t doc_index,
                          const RecordHit& hit,
                          TargetKind kind,
                          bool inner,
                          uint32_t child_index,
                          std::string block_spell,
                          const tdf::Block* target,
                          const sm::Change& change) {
        FieldKey key;
        key.doc_index = doc_index;
        key.section_index = hit.section_index;
        key.inner = inner;
        key.child_index = inner ? child_index : 0;
        key.block_fold = fold_copy(block_spell);
        key.key_fold = fold_copy(change.field);
        std::string key_spell = change.field;
        const char* existing = property_spelling(target, change.field);
        // A key the record already holds keeps the spelling the reader sees
        // (the newest case variant). A key it lacks takes, across every
        // change that names it, the spelling that sorts first by raw bytes,
        // so the written text does not depend on mutator order.
        const bool created = existing == nullptr;
        if (!created)
            key_spell = existing;
        Slot& slot =
            ensure(key, doc_index, hit, kind, std::move(block_spell), std::move(key_spell));
        if (created && change.field < slot.key)
            slot.key = change.field;
        if (mutator_index < slot.contribs.size())
            fold_one(slot.contribs[mutator_index], change, source_name);
    };

    for (size_t mutator_index = 0; mutator_index < mutators.size(); ++mutator_index) {
        const MutatorRules& rules = mutators[mutator_index];
        for (const RuleSource& source : rules.sources) {
            for (const sm::Block& rule : source.rules.blocks) {
                const std::string resolved_folder = resolve_folder(rule.target.folder, layout);
                const std::string resolved_extension =
                    resolve_extension(rule.target.extension, layout);
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
                        const tdf::Block* section =
                            section_at(documents, doc_index, hit.section_index);
                        for (const sm::Change& change : rule.changes) {
                            if (change.all_depth2) {
                                const uint32_t children = tdf::child_count(section);
                                for (uint32_t child_index = 0; child_index < children;
                                     ++child_index) {
                                    const tdf::Block* inner = tdf::child_at(section, child_index);
                                    if (inner == nullptr)
                                        continue;
                                    const std::string block_spell =
                                        inner->name != nullptr ? inner->name : "";
                                    const uint32_t key_count = tdf::property_count(inner);
                                    // Case variants are one field. The reader
                                    // keeps the newest, so the change is folded
                                    // once, not once per spelling.
                                    std::vector<std::string_view> taken_keys;
                                    taken_keys.reserve(key_count);
                                    for (uint32_t key_index = 0; key_index < key_count;
                                         ++key_index) {
                                        const char* key_text = tdf::property_key_at(
                                            inner, static_cast<int32_t>(key_index)
                                        );
                                        if (key_text == nullptr)
                                            continue;
                                        bool repeated = false;
                                        for (const std::string_view earlier : taken_keys) {
                                            if (same_text(earlier, key_text)) {
                                                repeated = true;
                                                break;
                                            }
                                        }
                                        if (repeated)
                                            continue;
                                        taken_keys.emplace_back(key_text);
                                        sm::Change one = change;
                                        one.all_depth2 = false;
                                        one.field = key_text;
                                        fold_field(
                                            mutator_index,
                                            source.name,
                                            doc_index,
                                            hit,
                                            kind,
                                            true,
                                            child_index,
                                            block_spell,
                                            inner,
                                            one
                                        );
                                    }
                                }
                                continue;
                            }
                            fold_field(
                                mutator_index,
                                source.name,
                                doc_index,
                                hit,
                                kind,
                                false,
                                0,
                                {},
                                section,
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
    }

    std::vector<Slot*> ordered;
    ordered.reserve(slots.size());
    for (auto& entry : slots)
        ordered.push_back(&entry.second);
    // Ties keep map order, which already includes the section index, so two
    // sections of the same name stay in file order on every standard library.
    std::stable_sort(ordered.begin(), ordered.end(), slot_before);

    std::vector<Planned> waiting;
    for (const Slot* slot : ordered) {
        const char* current = current_of(documents, *slot);
        combine_slot(*slot, current, mutators, waiting, result.notes);
    }
    for (Planned& write : waiting)
        commit_write(documents, write, options, result);

    std::stable_sort(result.writes.begin(), result.writes.end(), write_before);
    std::stable_sort(result.notes.begin(), result.notes.end(), note_before);
    return result;
}

} // namespace oa::data::mutation_rules
