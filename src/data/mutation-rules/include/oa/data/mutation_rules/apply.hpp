// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Applies every mutator's rule records to a document set in one pass.
// Needs read the records as the files layer left them. Each mutator's changes
// to one field become one contribution, and the contributions combine into
// one write. The result does not depend on mutator order, except text appends,
// which follow the list.
#pragma once

#include "oa/data/defs/layout.hpp"
#include "oa/data/mutation_rules/documents.hpp"
#include "oa/formats/soft_mutator.hpp"

#include <cstdint>
#include <functional>
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

/// One field the pass actually changed.
struct FieldWrite {
    std::vector<std::string> mutators; ///< mutators that contributed, in list order
    std::string path;                  ///< document path
    std::string record;                ///< unit name, or the section name
    std::string block;                 ///< empty for a key of the record; otherwise the inner block
    std::string key;                   ///< field name
    std::optional<std::string> old_text; ///< previous text; nullopt when the field was absent
    std::optional<std::string> new_text; ///< new text; nullopt when the field was removed
};

/// Why a rule did not do what it said, or did it with a warning.
enum class NoteKind : uint8_t {
    matched_nothing,                ///< a block selected no record
    clamp_widened,                  ///< the combined value lies outside one mutator's own clamp
    text_assignments_cancel,        ///< more than one assignment or removal, so the field stays
    text_cancelled_by_number,       ///< a number change dropped the text changes
    text_assignment_lost_to_append, ///< an assignment or removal lost because another mutator appends
    factor_on_absent_field, ///< a factor did nothing because the field is absent and uncreated
    not_a_number,           ///< a factor's field text is not a number
    not_finite,             ///< the combined value is not finite, so it was not written
    check_warned,           ///< a check warned and the write was kept
    check_refused,          ///< a check, or the document editor, refused the write
};

/// One note from a pass, for the change journal and the dry run.
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

/// Options for one pass.
struct ApplyOptions {
    const oa::data::defs::DataLayout*
        layout{};     ///< layout to resolve folders; null uses the active one
    FieldCheck check; ///< called for each new text or removal; empty skips checks
};

/// The writes and notes of one pass.
struct ApplyResult {
    std::vector<FieldWrite> writes; ///< fields that changed, sorted by path, record, block and key
    std::vector<RuleNote> notes;    ///< notes, sorted by path, record, key and kind
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

/// Applies every mutator's rules to the documents.
///
/// Needs are decided on the records as found, before any write. Within one
/// mutator the last change of a field decides its kind, and changes of that
/// kind compose. Across mutators, meta values average and win, assignments
/// average, factors multiply, clamps widen, and text follows the text rules.
/// A number is written only through the TDF number spelling, and only when it
/// is finite and different from the field's current number.
///
/// @param mutators mutators in list order
/// @param[in,out] documents documents to edit
/// @param options layout and field check
/// @return the writes that changed a field, and the notes
[[nodiscard]] ApplyResult apply_rules(
    std::span<const MutatorRules> mutators, DocumentSet& documents, const ApplyOptions& options
);

} // namespace oa::data::mutation_rules
