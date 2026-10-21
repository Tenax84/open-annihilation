// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The change journal and the conflict report of one pipeline run.
// Entries name mutators by their place in the list the player gave.
// Nothing here is kept after the call that built it.
#pragma once

#include "oa/data/mutators/collections.hpp"
#include "oa/data/mutators/files_layer.hpp"
#include "oa/data/mutation_rules/apply.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace oa::data::mutators {

/// Where in the pipeline a result or a journal entry belongs.
enum class Phase : uint8_t {
    loadout,   ///< the list, before any mutator is opened
    manifest,  ///< the manifest's works-with and engine requirement
    plan,      ///< the plan returned by mutate
    files,     ///< the files layer
    rules,     ///< the one rules pass
    hooks,     ///< the units hook, then the weapons hook
    integrity, ///< reserved for the check that follows the last hook
};

/// Which collection a change belongs to.
enum class Collection : uint8_t {
    files,    ///< a path added to or hidden in the files layer
    units,    ///< a unit record
    weapons,  ///< a weapon record
    features, ///< a feature record
    menus,    ///< a build menu or a download entry
    sides,    ///< side data
    tables,   ///< another game-data table
    other,    ///< a record that is none of those
};

/// What a journal entry did to a field, a record or a file.
enum class Change : uint8_t {
    set,           ///< a field was written
    removed_field, ///< a field was removed
    added,         ///< a file was added
    removed,       ///< a record was removed
    hidden,        ///< a path was hidden
};

/// One change the pipeline kept.
///
/// `mutators` are indices into the list passed to the pipeline. A rule write
/// names every mutator that contributed, in list order. `field` is lower
/// case and empty for a record or a file. `old_text` and `new_text` are
/// absent when the field was absent or removed. `record` is a unit name, a
/// weapon or feature section, or a path. `block` is empty for a key of the
/// record itself.
struct JournalEntry {
    /// Mutators that made the change, as indices into the input list.
    std::vector<std::uint16_t> mutators;
    /// The phase that made the change.
    Phase phase{};
    /// The collection the change belongs to.
    Collection collection{};
    /// What the change did.
    Change change{};
    /// Unit name, weapon or feature section, or path.
    std::string record;
    /// Block inside the record. Empty for the record's own keys.
    std::string block;
    /// Field name, lower case. Empty for a record or a file.
    std::string field;
    /// Previous text. Absent when the field was absent.
    std::optional<std::string> old_text;
    /// New text. Absent when the field was removed.
    std::optional<std::string> new_text;
    /// Document path, '/' separated.
    std::string path;
};

/// The changes one pipeline run kept, in journal order after `order`.
struct Journal {
    /// The changes. `order` sorts them; `entries_of` does not require that.
    std::vector<JournalEntry> entries;

    /// Sorts `entries` into journal order.
    ///
    /// Phase first, then the first mutator's index, then path, record, block
    /// and field compared byte by byte after ASCII case folding, with the
    /// raw bytes breaking a tie.
    void order();

    /// Returns the entries that name `mutator`, in journal order.
    ///
    /// @param mutator an index into the input list
    /// @return those entries
    [[nodiscard]] std::vector<JournalEntry> entries_of(std::uint16_t mutator) const;
};

/// How loudly a conflict is reported.
enum class Severity : uint8_t {
    note,    ///< recorded, and the change stands
    warning, ///< the change stands, and the player should see it
};

/// Which stacking situation a conflict is.
enum class ConflictKind : uint8_t {
    same_path_added,     ///< two mutators added one path; the later one won
    added_path_hidden,   ///< a mask hid a file an earlier mutator added
    add_folder_missing,  ///< an add folder is not in the package
    same_field_hooked,   ///< two hooks wrote one field; the later one won
    clamp_widened,       ///< a combined value lies outside one mutator's clamp
    text_change_dropped, ///< a text change was dropped
    matched_nothing,     ///< a rule selected no record
    value_past_limit,    ///< a value was kept past its comfortable limit
    write_refused,       ///< a write was refused
};

/// One stacking situation, for the log and the dry run.
///
/// `mutators` are indices into the input list. For a situation with a
/// winner, the winner is last. `field` is lower case.
struct Conflict {
    /// Note or warning.
    Severity severity{};
    /// Which situation.
    ConflictKind kind{};
    /// Mutators involved, in list order. The winner is last when there is one.
    std::vector<std::uint16_t> mutators;
    /// Record name. Empty when the conflict is not about one record.
    std::string record;
    /// Field name, lower case. Empty when the conflict is not about one field.
    std::string field;
    /// Document path, or the add folder when that folder is missing.
    std::string path;
    /// The note's own sentence, when it had one.
    std::string detail;
};

/// The conflicts of one pipeline run, plus documents the parse left out.
///
/// `unread` is the warning for a document that did not parse. Those paths
/// are not `matched_nothing`: a dry run reads that kind for rules that
/// selected nothing.
struct ConflictReport {
    /// Conflicts, in the order they were found.
    std::vector<Conflict> conflicts;
    /// Paths left out because they did not parse, one warning each.
    std::vector<std::string> unread;
};

/// Turns one files-layer note into a conflict.
///
/// `ids` is the input list, in order. An id the list does not hold is left
/// out. The winner or the masking mutator is stored last.
///
/// @param note the files-layer note
/// @param ids mutator ids, in list order
/// @return the conflict
[[nodiscard]] Conflict conflict_from_file(const FileNote& note, std::span<const std::string> ids);

/// Turns one rules note into a conflict.
///
/// Text cancellations, a factor on an absent field and a value that is not
/// a number become `text_change_dropped`. A value that is not finite and a
/// refused check become `write_refused`. `ids` is the input list, in order.
///
/// @param note the rules note
/// @param ids mutator ids, in list order
/// @return the conflict
[[nodiscard]] Conflict conflict_from_rule(
    const oa::data::mutation_rules::RuleNote& note, std::span<const std::string> ids
);

/// Turns one hook note into a conflict.
///
/// An overwrite names both mutators and stores the winner last. A download
/// file whose first entry changed is a `text_change_dropped` note: the
/// journal has no separate kind for it. `ids` is the input list, in order.
///
/// @param note the hook note
/// @param ids mutator ids, in list order
/// @return the conflict
[[nodiscard]] Conflict conflict_from_hook(const HookNote& note, std::span<const std::string> ids);

/// Writes one conflict as a single English line.
///
/// An index `ids` does not hold is left out of the line. A hook that won a
/// field reads as "ridge-tougher wins units/RGCOM.fbi maxdamage over
/// ridge-tough".
///
/// @param conflict the conflict
/// @param ids mutator ids, in the same order as the conflict's indices
/// @return the line, with no line break
[[nodiscard]] std::string describe(const Conflict& conflict, std::span<const std::string> ids);

} // namespace oa::data::mutators
