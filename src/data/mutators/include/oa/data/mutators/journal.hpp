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
    loadout,   ///< the list: refused whole, or a mutator left out for what it requires
    manifest,  ///< the manifest's works-with and engine requirement
    plan,      ///< the plan returned by mutate
    files,     ///< the files layer
    rules,     ///< one mutator's rules
    hooks,     ///< that mutator's units, weapons, features, sides, tables, then menus hooks
    integrity, ///< the check that follows the last hook
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
    added,         ///< a file was added, or a record file added or replaced a record
    removed,       ///< a record was removed
    hidden,        ///< a path was hidden
};

/// One change the pipeline kept.
///
/// `mutators` are indices into the list passed to the pipeline. A rule write
/// names the mutator whose step wrote the field, and a hook write names the
/// mutator whose hook wrote it. `field` is lower
/// case and empty for a record or a file. `old_text` and `new_text` are
/// absent when the field was absent or removed. `record` is a unit name, a
/// weapon or feature section, or a path. `block` is empty for a key of the
/// record itself.
///
/// A file entry is `added` for each path a mutator's files add, and `hidden`
/// for each file of the mod a mask hides (phase `files`) and for each file a
/// hook's removal empties (phase `hooks`). A mask that hides another
/// mutator's file names both, the one whose file it hid first.
///
/// A record a mutator's file adds is `added`, with the record name and no
/// `old_text`. A record a file replaces is `added` as well, and `old_text`
/// names the record that was already there. When that record is one an
/// earlier mutator brought, the report keeps one `value_replaced` note for
/// the collection and that pair of mutators. A shared record is `added`
/// with `old_text` naming the record and `new_text` `shared`, and it does
/// not take the record from the mutator that brought it. A renamed record
/// is `added` under its private name, with `old_text` the name the mutator
/// wrote and `new_text` the private name.
struct JournalEntry {
    /// Mutators that made the change, as indices into the input list.
    std::vector<uint16_t> mutators;
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
    /// The first mutator's index first, then the phase, so one mutator's
    /// rules come before its hooks. An entry that names no mutator sorts
    /// last. Path, record, block and field are compared byte by byte after
    /// ASCII case folding, with the raw bytes breaking a tie.
    void order();

    /// Returns the entries that name `mutator`, in journal order.
    ///
    /// @param mutator an index into the input list
    /// @return those entries
    [[nodiscard]] std::vector<JournalEntry> entries_of(uint16_t mutator) const;
};

/// How loudly a conflict is reported.
enum class Severity : uint8_t {
    note,    ///< recorded, and the change stands
    warning, ///< the change stands, and the player should see it
    /// The mutator is left out. Only an integrity issue carries it, never a conflict in a report.
    failure,
};

/// Which stacking situation a conflict is.
enum class ConflictKind : uint8_t {
    same_path_added,    ///< two mutators added one path; the later one won
    added_path_hidden,  ///< a mask hid a file an earlier mutator added
    add_folder_missing, ///< an add folder is not in the package
    same_field_hooked,  ///< a later hook wrote a field the last writer, or its locker, had set
    value_replaced,     ///< a later mutator replaced a value, or a record, an earlier mutator wrote
    field_locked,       ///< a later mutator's rule left a field an earlier mutator locked alone
    text_change_dropped, ///< a text change was dropped
    matched_nothing,     ///< a rule selected no record
    value_past_limit,    ///< a value was kept past its comfortable limit
    value_held,          ///< a value was held at the end of the range its field stores
    write_refused,       ///< a write was refused
    hack_note,           ///< a mutator hack overlaps Developer Mode or another mutator
    record_shared,       ///< an added record matched one already there, so it was not written
    record_renamed,      ///< an added record took a private name and the existing one stayed
    integrity_warning,   ///< a problem found after the last hook that does not stop the match
};

/// One stacking situation, for the log and the dry run.
///
/// `mutators` are indices into the input list. For a situation with a
/// winner, the winner is last. A locked field is the exception: the mutator
/// that locked it comes first and keeps its value, and the mutator whose
/// change was left undone comes last. `field` is lower case.
struct Conflict {
    /// Note or warning.
    Severity severity{};
    /// Which situation.
    ConflictKind kind{};
    /// Mutators involved, in list order. The winner is last when there is one.
    std::vector<uint16_t> mutators;
    /// Record name. Empty when the conflict is not about one record.
    std::string record;
    /// Field name, lower case. Empty when the conflict is not about one field.
    std::string field;
    /// Document path, or the add folder when that folder is missing.
    std::string path;
    /// The note's own sentence, when it had one. For a replaced record
    /// that no mutator brought, where it came from: "the game" or "the mod".
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
/// A value a later mutator replaced becomes `value_replaced`, and a locked
/// field a later mutator left alone becomes `field_locked`. Both are notes
/// and keep the note's order: the earlier mutator, then the later one. A
/// factor on an absent field and a value that is not a number become
/// `text_change_dropped`. The factor's sentence says it found no value to
/// scale. A value that is not finite and a refused check become
/// `write_refused`. `ids` is the input list, in order.
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
/// journal has no separate kind for it. A weapon id the game already
/// assigned, and that a hook tried to write, is `write_refused`. `ids` is
/// the input list, in order.
///
/// @param note the hook note
/// @param ids mutator ids, in list order
/// @return the conflict
[[nodiscard]] Conflict conflict_from_hook(const HookNote& note, std::span<const std::string> ids);

/// Writes one conflict as a single English line.
///
/// An index `ids` does not hold is left out of the line. A hook that won a
/// field reads as "ridge-tougher wins units/RGCOM.fbi maxdamage over
/// ridge-tough". A replaced value reads as "ridge-tougher replaced
/// ridge-tough's value on units/rgcom.fbi maxdamage". A replaced record,
/// which has no field, names the record: "ridge-bot-b replaced
/// ridge-bot-a's RGBOT on units/rgbot.fbi". A record no mutator brought
/// names where it came from, which `detail` holds: "ridge-bot-a replaced
/// RGBOT from the game on units/rgbot.fbi", or "from the mod" when the
/// mod in play served it. A locked field reads as
/// "ridge-tough locked units/rgcom.fbi maxdamage, so ridge-tougher left it
/// alone". A shared record reads as "ridge-kit shares SPARK with ridge-pace",
/// or "on <path>" when no earlier mutator brought it. A rename reads as
/// "ridge-kit renamed RGBOT to RGBOT_2". A factor on an absent field reads
/// as "ridge-swift found no value to scale on units/rglab.fbi maxdamage".
/// A refused write names the place, then the reason when `detail` holds
/// one: "ridge-tough refused a write on units/rgcom.fbi maxdamage: kept".
/// An integrity warning is the check's own sentence, which `detail` holds.
///
/// @param conflict the conflict
/// @param ids mutator ids, in the same order as the conflict's indices
/// @return the line, with no line break
[[nodiscard]] std::string describe(const Conflict& conflict, std::span<const std::string> ids);

} // namespace oa::data::mutators
