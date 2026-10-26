// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Applies one mutator's rule records to a document set as a step.
// Needs read the records as the rules before them left those records.
// Each change is written before the next change is read. A lock sets the
// value and marks the field so a later mutator's rules leave it alone.
#pragma once

#include "oa/data/defs/layout.hpp"
#include "oa/data/mutation_rules/documents.hpp"
#include "oa/formats/soft_mutator.hpp"

#include <compare>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::data::mutation_rules {

/// One rule file a mutator contributes, in plan order.
struct RuleSource {
    std::string name;                          ///< source name, such as `rules/legacy.sm`
    oa::formats::soft_mutator::RuleFile rules; ///< blocks in file order
};

/// Every rule source of one mutator.
struct MutatorRules {
    std::string mutator;             ///< mutator name, in the player's list
    std::vector<RuleSource> sources; ///< sources in plan order
};

/// Which kind of record a target's files hold.
enum class TargetKind : uint8_t {
    units,    ///< the layout's units folder and its unit extension
    weapons,  ///< the layout's weapons folder
    features, ///< a folder under `features`
    sidedata, ///< `gamedata/sidedata.tdf`
    other,    ///< any other folder
};

/// One field a check or a write names.
struct FieldRef {
    TargetKind kind{};       ///< record kind of the file
    std::string_view path;   ///< document path
    std::string_view record; ///< unit name, or the section name
    std::string_view block;  ///< empty for a key of the record; otherwise the inner block's name
    std::string_view key;    ///< field name
};

/// What a field check decided.
enum class CheckOutcome : uint8_t {
    ok,     ///< the write may proceed
    warn,   ///< the write proceeds, and the caller is told
    refuse, ///< the write is left undone
};

/// The outcome of one field check, with the sentence a player can read.
struct CheckResult {
    CheckOutcome outcome{}; ///< ok, warn or refuse
    std::string reason;     ///< empty when the outcome is ok
};

/// A check of one new value. A null text means the field is removed.
///
/// The pipeline passes the typed field checks through this. This module does
/// not know them.
using FieldCheck =
    std::function<CheckResult(const FieldRef& field, const std::optional<std::string>& text)>;

/// One field the step actually changed.
struct FieldWrite {
    std::vector<std::string> mutators; ///< the mutator that wrote the field
    std::string path;                  ///< document path
    std::string record;                ///< unit name, or the section name
    std::string block;                 ///< empty for a key of the record; otherwise the inner block
    std::string key;                   ///< field name
    std::optional<std::string> old_text; ///< previous text; nullopt when the field was absent
    std::optional<std::string> new_text; ///< new text; nullopt when the field was removed
};

/// Why a rule did not do what it said, or did it with a warning.
enum class NoteKind : uint8_t {
    matched_nothing,        ///< a block selected no record
    value_replaced,         ///< a later mutator replaced a value an earlier mutator wrote
    field_locked,           ///< a later mutator left a locked field alone
    factor_on_absent_field, ///< a factor did nothing because the field is absent and uncreated
    not_a_number,           ///< a factor's field text is not a number
    not_finite,             ///< the value is not finite, so it was not written
    check_warned,           ///< a check warned and the write was kept
    check_refused,          ///< a check, or the document editor, refused the write
};

/// One note from a step, for the change journal and the dry run.
struct RuleNote {
    NoteKind kind{};                   ///< which note
    std::vector<std::string> mutators; ///< mutators involved, in list order
    std::string source;  ///< rule source name; empty when the note has no single source
    uint32_t line{};     ///< line in that source, from 1; 0 when there is none
    std::string path;    ///< document path; empty for a block that matched nothing
    std::string record;  ///< record name; empty when the note is not about one record
    std::string key;     ///< field name; empty when the note is not about one field
    std::string message; ///< what happened, in plain words
};

/// One field a step has locked or written, as the step state names it.
///
/// The four names are folded: ASCII capitals become lower case, and a path
/// also treats '\\' as '/'. Two ids name one field when those four names
/// match.
struct FieldId {
    std::string path;   ///< document path, folded
    std::string record; ///< record name, folded
    std::string block;  ///< inner block name, folded; empty for a key of the record
    std::string key;    ///< field name, folded

    /// Returns whether the four folded names match.
    ///
    /// @param other the id to compare
    /// @return true when both ids name one field
    [[nodiscard]] bool operator==(const FieldId& other) const = default;

    /// Orders by path, then record, block and key, byte by byte.
    ///
    /// @param other the id to compare
    /// @return the order of the two ids
    [[nodiscard]] auto operator<=>(const FieldId& other) const = default;
};

/// Lock marks and last writers, kept from one step to the next.
///
/// The caller starts from an empty state and passes the same value into each
/// step of a run. A step looks fields up in both maps and adds to them. It
/// does not clear a mark an earlier step made.
///
/// A lock mark is an entry of `locks`. The mutator named there set the field
/// with a lock. A later step does not write that field, and notes the refusal,
/// naming the mutator that locked it and the mutator that tried to change it.
struct ApplyState {
    /// The mutator that locked each field. A lock lasts for the rest of the run.
    std::map<FieldId, std::string> locks;
    /// The mutator that last wrote each field.
    std::map<FieldId, std::string> writers;
};

/// Options for one step.
struct ApplyOptions {
    const oa::data::defs::DataLayout*
        layout{};     ///< layout to resolve folders; null uses the active one
    FieldCheck check; ///< called for each new text or removal; empty skips checks
};

/// The writes and notes of one step.
struct ApplyResult {
    /// Fields that changed. Sorted by path, record, block and key, compared
    /// after ASCII case folding. Writes of one field stay in the order they
    /// were applied, whichever spelling of the key each one used.
    std::vector<FieldWrite> writes;
    /// Notes, sorted by path, record, key, kind, line, source and message.
    std::vector<RuleNote> notes;
};

/// Lists each distinct target the mutators name, in the order first seen.
///
/// @param mutators mutators in list order
/// @return one entry per distinct folder and suffix
[[nodiscard]] std::vector<oa::formats::soft_mutator::Target>
targets_of(std::span<const MutatorRules> mutators);

/// Reports whether one need holds for a record as it stands.
///
/// A field the record leaves out is read as the value the loader uses for
/// that key: a unit field, a weapon field, or nothing. A movement-class file
/// is recognised by the overload that takes the document path. A presence
/// test reads whether the key is written, not the default. A negated need
/// holds exactly when the positive one does not.
///
/// @param record the record's block, or null when every key is left out
/// @param kind which schema a missing field uses
/// @param need the need
/// @return true when the need holds
[[nodiscard]] bool need_met(
    const oa::formats::tdf::Block* record,
    TargetKind kind,
    const oa::formats::soft_mutator::Need& need
);

/// Reports whether one need holds, using the movement-class defaults when
/// `path` is the layout's `gamedata/moveinfo.tdf`.
///
/// @param record the record's block, or null when every key is left out
/// @param kind which schema a missing field uses; units and weapons win over the path
/// @param path document path, as the store lists it
/// @param layout layout whose gamedata folder names the movement-class file
/// @param need the need
/// @return true when the need holds
[[nodiscard]] bool need_met(
    const oa::formats::tdf::Block* record,
    TargetKind kind,
    std::string_view path,
    const oa::data::defs::DataLayout& layout,
    const oa::formats::soft_mutator::Need& need
);

/// Applies one mutator's rules to the documents as they stand.
///
/// The step reads `documents` and `state`, writes the documents its changes
/// reach, and adds lock marks and writer marks to `state`. Sources run in
/// plan order. Each source's blocks and changes run in file order. A block's
/// needs read the records as the rules before that block left them, and each
/// change is written before the next change is read.
///
/// A factor multiplies the current number. An assignment replaces it. A clamp
/// bounds only that scale. A text change or a removal acts on the current
/// text. A lock sets the value and adds a lock mark: a later mutator's change
/// to that field is not written, and the step notes it, naming both mutators.
/// A later assignment, text change or removal that replaces a value an earlier
/// mutator wrote is noted, naming both mutators.
///
/// A number is written only through the TDF number spelling, and only when it
/// is finite and different from the field's current number. A result that is
/// not finite is not written, and a clamp is not allowed to pull that result
/// back into range.
///
/// @param rules one mutator's sources
/// @param[in,out] documents documents to edit
/// @param[in,out] state lock marks and last writers, kept across steps
/// @param options layout and field check
/// @return the writes that changed a field, and the notes
[[nodiscard]] ApplyResult apply_step(
    const MutatorRules& rules,
    DocumentSet& documents,
    ApplyState& state,
    const ApplyOptions& options
);

} // namespace oa::data::mutation_rules
