// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Journal order, and the stacking notes of the files layer, each mutator's
// rules and its hooks turned into conflicts. The sentences are the log lines.
#include "oa/data/mutators/journal.hpp"

#include "oa/data/languages/interface_text.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

namespace oa::data::mutators {
namespace rules = oa::data::mutation_rules;

namespace {

/// Folds one byte to lower case.
///
/// @param byte a byte of a name
/// @return the folded byte
char fold_ascii(char byte) {
    if (byte >= 'A' && byte <= 'Z')
        return static_cast<char>(byte - 'A' + 'a');
    return byte;
}

/// Copies a text folded to lower case.
///
/// @param text a name
/// @return the folded copy
std::string fold_copy(std::string_view text) {
    std::string out;
    out.resize(text.size());
    for (std::size_t offset = 0; offset < text.size(); ++offset)
        out[offset] = fold_ascii(text[offset]);
    return out;
}

/// Orders two texts by their folded bytes, then by the raw bytes.
///
/// @param left a text
/// @param right another text
/// @return true when `left` sorts first
bool less_text(std::string_view left, std::string_view right) {
    const std::size_t count = left.size() < right.size() ? left.size() : right.size();
    for (std::size_t offset = 0; offset < count; ++offset) {
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

/// The index journal order uses for an entry that names no mutator.
constexpr uint16_t k_no_mutator = 65535;

/// Returns the first mutator index, or `k_no_mutator` when there is none.
///
/// @param entry the entry
/// @return the index
uint16_t first_mutator(const JournalEntry& entry) {
    if (entry.mutators.empty())
        return k_no_mutator;
    return entry.mutators.front();
}

/// Reports whether `left` sorts before `right` in the journal.
///
/// @param left an entry
/// @param right another entry
/// @return true when `left` sorts first
bool entry_before(const JournalEntry& left, const JournalEntry& right) {
    const uint16_t left_mutator = first_mutator(left);
    const uint16_t right_mutator = first_mutator(right);
    if (left_mutator != right_mutator)
        return left_mutator < right_mutator;
    if (left.phase != right.phase)
        return static_cast<uint8_t>(left.phase) < static_cast<uint8_t>(right.phase);
    if (left.path != right.path && less_text(left.path, right.path))
        return true;
    if (left.path != right.path && less_text(right.path, left.path))
        return false;
    if (left.record != right.record && less_text(left.record, right.record))
        return true;
    if (left.record != right.record && less_text(right.record, left.record))
        return false;
    if (left.block != right.block && less_text(left.block, right.block))
        return true;
    if (left.block != right.block && less_text(right.block, left.block))
        return false;
    if (left.field != right.field && less_text(left.field, right.field))
        return true;
    if (left.field != right.field && less_text(right.field, left.field))
        return false;
    return false;
}

/// Finds `id` in `ids`.
///
/// @param ids the input list
/// @param id the id to find
/// @param[out] slot the index, when it was found
/// @return true when `id` is in the list
bool find_id(std::span<const std::string> ids, std::string_view id, uint16_t& slot) {
    for (std::size_t index = 0; index < ids.size(); ++index) {
        if (ids[index] == id) {
            slot = static_cast<uint16_t>(index);
            return true;
        }
    }
    return false;
}

/// Appends `id` when the list holds it.
///
/// @param ids the input list
/// @param id the id to append
/// @param[in,out] slots the indices already gathered
void append_id(
    std::span<const std::string> ids, std::string_view id, std::vector<uint16_t>& slots
) {
    uint16_t slot = 0;
    if (!id.empty() && find_id(ids, id, slot))
        slots.push_back(slot);
}

/// Appends each id the list holds, in the order given.
///
/// @param ids the input list
/// @param names the ids on the note
/// @param[in,out] slots the indices already gathered
void append_ids(
    std::span<const std::string> ids,
    const std::vector<std::string>& names,
    std::vector<uint16_t>& slots
) {
    for (const std::string& name : names)
        append_id(ids, name, slots);
}

/// Joins the conflict's mutator ids with ", ".
///
/// @param conflict the conflict
/// @param ids the input list
/// @return the joined ids, or "a mutator" when none resolve
std::string joined_names(const Conflict& conflict, std::span<const std::string> ids) {
    std::string line;
    for (const uint16_t slot : conflict.mutators) {
        if (static_cast<std::size_t>(slot) >= ids.size())
            continue;
        if (!line.empty())
            line += ", ";
        line += ids[slot];
    }
    if (line.empty())
        return "a mutator";
    return line;
}

/// Returns the id at `slot`, or an empty string when the slot is out of range.
///
/// @param ids the input list
/// @param slot the index
/// @return the id
std::string_view id_at(std::span<const std::string> ids, uint16_t slot) {
    if (static_cast<std::size_t>(slot) >= ids.size())
        return {};
    return ids[slot];
}

/// Returns the last mutator the list still holds, or "a mutator".
///
/// The winner of a stacking note is stored last.
///
/// @param conflict the conflict
/// @param ids the input list
/// @return the winner's id
std::string winner_name(const Conflict& conflict, std::span<const std::string> ids) {
    for (std::size_t index = conflict.mutators.size(); index > 0; --index) {
        const std::string_view name = id_at(ids, conflict.mutators[index - 1]);
        if (!name.empty())
            return std::string{name};
    }
    return "a mutator";
}

/// Joins every mutator but the last.
///
/// @param conflict the conflict
/// @param ids the input list
/// @return the earlier mutators, joined with ", "
std::string earlier_names(const Conflict& conflict, std::span<const std::string> ids) {
    std::string line;
    if (conflict.mutators.size() < 2)
        return line;
    for (std::size_t index = 0; index + 1 < conflict.mutators.size(); ++index) {
        const std::string_view name = id_at(ids, conflict.mutators[index]);
        if (name.empty())
            continue;
        if (!line.empty())
            line += ", ";
        line.append(name.data(), name.size());
    }
    return line;
}

/// Appends " on <path>" and, when set, " <field>".
///
/// @param[in,out] line the line
/// @param conflict the conflict
void append_place(std::string& line, const Conflict& conflict) {
    if (!conflict.path.empty()) {
        line += " on ";
        line += conflict.path;
    }
    if (!conflict.field.empty()) {
        line += " ";
        line += conflict.field;
    }
}

} // namespace

void Journal::order() {
    std::stable_sort(entries.begin(), entries.end(), entry_before);
}

std::vector<JournalEntry> Journal::entries_of(uint16_t mutator) const {
    std::vector<JournalEntry> found;
    for (const JournalEntry& entry : entries) {
        for (const uint16_t slot : entry.mutators) {
            if (slot == mutator) {
                found.push_back(entry);
                break;
            }
        }
    }
    std::stable_sort(found.begin(), found.end(), entry_before);
    return found;
}

Conflict conflict_from_file(const FileNote& note, std::span<const std::string> ids) {
    Conflict conflict;
    conflict.path = note.path;
    conflict.detail = note.path;
    if (note.kind == FileNoteKind::add_replaced) {
        conflict.severity = Severity::note;
        conflict.kind = ConflictKind::same_path_added;
        append_id(ids, note.other, conflict.mutators);
        append_id(ids, note.mutator, conflict.mutators);
        return conflict;
    }
    if (note.kind == FileNoteKind::add_masked) {
        conflict.severity = Severity::warning;
        conflict.kind = ConflictKind::added_path_hidden;
        append_id(ids, note.other, conflict.mutators);
        append_id(ids, note.mutator, conflict.mutators);
        return conflict;
    }
    conflict.severity = Severity::note;
    conflict.kind = ConflictKind::add_folder_missing;
    append_id(ids, note.mutator, conflict.mutators);
    return conflict;
}

Conflict conflict_from_rule(const rules::RuleNote& note, std::span<const std::string> ids) {
    Conflict conflict;
    conflict.path = note.path;
    conflict.record = note.record;
    conflict.field = fold_copy(note.key);
    conflict.detail = note.message;
    append_ids(ids, note.mutators, conflict.mutators);
    switch (note.kind) {
    case rules::NoteKind::matched_nothing:
        conflict.severity = Severity::note;
        conflict.kind = ConflictKind::matched_nothing;
        break;
    case rules::NoteKind::value_replaced:
        conflict.severity = Severity::note;
        conflict.kind = ConflictKind::value_replaced;
        break;
    case rules::NoteKind::field_locked:
        conflict.severity = Severity::note;
        conflict.kind = ConflictKind::field_locked;
        break;
    case rules::NoteKind::factor_on_absent_field:
    case rules::NoteKind::not_a_number:
        conflict.severity = Severity::note;
        conflict.kind = ConflictKind::text_change_dropped;
        break;
    case rules::NoteKind::not_finite:
    case rules::NoteKind::check_refused:
        conflict.severity = Severity::note;
        conflict.kind = ConflictKind::write_refused;
        break;
    case rules::NoteKind::check_warned:
        conflict.severity = Severity::warning;
        conflict.kind = ConflictKind::value_past_limit;
        break;
    }
    return conflict;
}

Conflict conflict_from_hook(const HookNote& note, std::span<const std::string> ids) {
    Conflict conflict;
    conflict.path = note.path;
    conflict.record = note.record;
    conflict.field = fold_copy(note.key);
    conflict.detail = note.message;
    append_ids(ids, note.mutators, conflict.mutators);
    switch (note.kind) {
    case HookNoteKind::check_warned:
        conflict.severity = Severity::warning;
        conflict.kind = ConflictKind::value_past_limit;
        break;
    case HookNoteKind::hook_overwrote:
        conflict.severity = Severity::note;
        conflict.kind = ConflictKind::same_field_hooked;
        break;
    case HookNoteKind::download_first_entry_changed:
    case HookNoteKind::not_a_number:
    case HookNoteKind::menu_note:
        conflict.severity = Severity::note;
        conflict.kind = ConflictKind::text_change_dropped;
        break;
    case HookNoteKind::id_kept:
        conflict.severity = Severity::note;
        conflict.kind = ConflictKind::write_refused;
        break;
    }
    return conflict;
}

std::string describe(const Conflict& conflict, std::span<const std::string> ids) {
    const std::string names = joined_names(conflict, ids);
    if (conflict.kind == ConflictKind::same_field_hooked ||
        conflict.kind == ConflictKind::same_path_added) {
        const std::string earlier = earlier_names(conflict, ids);
        std::string line = winner_name(conflict, ids);
        line += " wins ";
        line += conflict.path;
        if (!conflict.field.empty()) {
            line += " ";
            line += conflict.field;
        }
        if (!earlier.empty()) {
            line += " over ";
            line += earlier;
        }
        return line;
    }
    if (conflict.kind == ConflictKind::added_path_hidden) {
        std::string line = winner_name(conflict, ids);
        line += " hides ";
        line += conflict.path;
        const std::string earlier = earlier_names(conflict, ids);
        if (!earlier.empty()) {
            line += " added by ";
            line += earlier;
        }
        return line;
    }
    if (conflict.kind == ConflictKind::add_folder_missing) {
        std::string line = names;
        line += " adds a folder that is not in the package";
        if (!conflict.path.empty()) {
            line += ": ";
            line += conflict.path;
        }
        return line;
    }
    if (conflict.kind == ConflictKind::value_replaced) {
        const std::string earlier = earlier_names(conflict, ids);
        std::string line = winner_name(conflict, ids);
        line += " replaced ";
        const bool record_note = conflict.field.empty() && !conflict.record.empty();
        if (record_note && earlier.empty() && !conflict.detail.empty()) {
            // A record no mutator brought: `detail` says where it came from.
            line += conflict.record;
            line += " from ";
            line += conflict.detail;
            append_place(line, conflict);
            return line;
        }
        line += earlier.empty() ? std::string{"an earlier mutator"} : earlier;
        if (record_note) {
            line += "'s ";
            line += conflict.record;
        } else {
            line += "'s value";
        }
        append_place(line, conflict);
        return line;
    }
    if (conflict.kind == ConflictKind::record_shared) {
        std::string line = winner_name(conflict, ids);
        line += " shares ";
        line += conflict.record;
        const std::string earlier = earlier_names(conflict, ids);
        if (!earlier.empty()) {
            line += " with ";
            line += earlier;
        } else if (!conflict.path.empty()) {
            line += " on ";
            line += conflict.path;
        }
        return line;
    }
    if (conflict.kind == ConflictKind::record_renamed) {
        std::string line = winner_name(conflict, ids);
        line += " renamed ";
        line += conflict.record;
        line += " to ";
        line += conflict.detail;
        return line;
    }
    if (conflict.kind == ConflictKind::field_locked) {
        const std::string locker = earlier_names(conflict, ids);
        std::string line = locker.empty() ? std::string("an earlier mutator") : locker;
        line += " locked";
        if (!conflict.path.empty()) {
            line += " ";
            line += conflict.path;
        }
        if (!conflict.field.empty()) {
            line += " ";
            line += conflict.field;
        }
        line += ", so ";
        line += winner_name(conflict, ids);
        line += " left it alone";
        return line;
    }
    if (conflict.kind == ConflictKind::text_change_dropped) {
        // A menu note keeps its own sentence. The other notes of this kind
        // share one line, and a factor on an absent field is told apart by
        // the words in the rules note.
        if (conflict.detail.find("moved from page") != std::string::npos ||
            conflict.detail.find("has no picture") != std::string::npos ||
            conflict.detail.find("does not see") != std::string::npos ||
            conflict.detail.find("placeholder now holds") != std::string::npos)
            return conflict.detail;
        std::string line = names;
        if (conflict.detail.find("a factor") != std::string::npos)
            line += " found no value to scale";
        else
            line += " dropped a text change";
        append_place(line, conflict);
        return line;
    }
    if (conflict.kind == ConflictKind::matched_nothing) {
        std::string line = names;
        line += " matched nothing";
        return line;
    }
    if (conflict.kind == ConflictKind::hack_note)
        return conflict.detail;
    if (conflict.kind == ConflictKind::integrity_warning)
        return conflict.detail;
    if (conflict.kind == ConflictKind::value_held) {
        constexpr std::string_view k_held_value_note = "{unit}: {field} {value} held at {max}";
        std::string sentence{oa::data::languages::interface_text(k_held_value_note)};
        const std::string::size_type split = conflict.detail.find('\n');
        const std::string value =
            split == std::string::npos ? conflict.detail : conflict.detail.substr(0, split);
        const std::string held_at =
            split == std::string::npos ? std::string{} : conflict.detail.substr(split + 1);
        const auto replace = [&](std::string_view token, std::string_view text) {
            const std::string::size_type at = sentence.find(token);
            if (at != std::string::npos)
                sentence.replace(at, token.size(), text);
        };
        replace("{unit}", conflict.record);
        replace("{field}", conflict.field);
        replace("{value}", value);
        replace("{max}", held_at);
        return sentence;
    }
    if (conflict.kind == ConflictKind::value_past_limit) {
        std::string line = names;
        line += " kept";
        if (!conflict.path.empty()) {
            line += " ";
            line += conflict.path;
        }
        if (!conflict.field.empty()) {
            line += " ";
            line += conflict.field;
        }
        line += " past its limit";
        return line;
    }
    std::string line = names;
    line += " refused a write";
    append_place(line, conflict);
    if (!conflict.detail.empty()) {
        line += ": ";
        line += conflict.detail;
    }
    return line;
}

} // namespace oa::data::mutators
