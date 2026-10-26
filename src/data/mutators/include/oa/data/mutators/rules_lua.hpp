// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Rule blocks written in Lua, and rule files named from Lua.
//
// A plan's rules list is a sequence of blocks and rule-file names. This
// reads that sequence into the records the mutation pass applies. The needs
// are the same forms the collections use. The changes are the forms a rule
// file writes. A block and the rule-file spelling of that block are the
// same records.
#pragma once

#include "oa/data/mutation_rules/apply.hpp"
#include "oa/formats/soft_mutator.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct lua_State;

namespace oa::data::mutators {

/// The most sources one plan may hold.
///
/// Each run of consecutive Lua blocks is one source, and each `sm` path is
/// one source. A plan with more sources than this is refused.
inline constexpr uint32_t max_plan_sources = 256;

/// Publishes the rule forms in a state.
///
/// The globals are `where`, `all`, `named`, `on`, `set`, `scale`, `lock`,
/// `remove`, `append`, `sm` and `empty`. `range`, `is`, `has`, `absent`,
/// `not_`, `glob` and `pattern` are published too, by `install_selection_forms`.
/// `glob` and `pattern` match maps only: a units rule that names one is
/// refused. `where`, `all`,
/// `named` and `on` build an unfinished block. `:change { }` finishes it,
/// and a second `:change` is an error. A block has one selector: a later
/// `where`, `all` or `named` is an error that names the line. `on` may come
/// before or after the selector and only replaces the folder and extension.
/// `sm` holds a path; it does not read. `empty` assigns empty text, the
/// record a rule file writes with `field=;`.
///
/// `where` and `:change` check every entry of their table when they are
/// called, and an entry they refuse raises the error at that call. Each
/// keeps its own copy of the table's entries, so changing or reusing the
/// table afterwards changes no block already built.
///
/// The state is one the script host opened. The forms' metatables cannot
/// be read or replaced. A script can hold a block after it has been
/// collected: the block keeps its fields as Lua values and has no
/// finalizer. The forms keep no working state between calls, so states on
/// different threads share nothing.
///
/// @param state state to publish into
void install_rule_forms(lua_State* state);

/// Reads one file from the mutator's package.
///
/// `path` uses `/` separators. The function returns the bytes, or no value
/// when the package has no such file. `error` receives a reason when it is
/// not null; it may be left empty. The function does not call into `state`.
/// It may throw. `read_plan_rules` catches that and reports `cannot read`,
/// and itself does not throw.
using PackageReader =
    std::function<std::optional<std::vector<uint8_t>>(std::string_view path, std::string* error)>;

/// The rule sources read from one plan, with any findings from rule files.
struct PlanRules {
    /// Sources in list order. Consecutive Lua blocks share one source named
    /// by the entry. Each rule file is its own source, named by its path.
    std::vector<oa::data::mutation_rules::RuleSource> sources;
    /// Findings from rule files, warnings and errors, in the order found.
    std::vector<oa::formats::soft_mutator::Diagnostic> diagnostics;
    /// The source name of each finding, in the same order as `diagnostics`.
    std::vector<std::string> diagnostic_sources;
    /// Why the plan was refused. Empty when the plan was read.
    std::string error;

    /// Reports whether the plan was read.
    ///
    /// A warning does not refuse the plan. An error does.
    ///
    /// @return true when `error` is empty
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

/// Reads a plan's rules list into rule sources.
///
/// The list is the sequence at `rules_index`, from 1 until the first hole.
/// A Lua block joins the current source, which is named `entry_name`. A
/// `sm` value ends that source and adds one source of its own. Anything
/// else refuses the plan. A block `:change` was not called on refuses the
/// plan, and the message names that block's place in the list, its call
/// and its line, as does any other refusal of one block. The records are
/// copied out: the result does not use the state after return.
///
/// A rule-file path is a relative path inside the package. `\` is read as
/// `/`. A path that is absolute, has a drive, or has a `..` segment is
/// refused, and the reader is not called. The reader is the only way the
/// bytes are obtained. Each distinct path is read and parsed once. A file
/// that cannot be read, or that the rule parser reports an error in,
/// refuses the plan. The message names the path, and for a parse error the
/// line as well. An exception from the reader is reported as `cannot read`
/// for that path. Warnings are kept on the result, once per path.
///
/// Field names in one block are stored in the order of their names compared
/// after ASCII lower-casing, with the raw bytes breaking a tie, so the
/// records do not depend on the order the keys were inserted. A block's
/// line is the line the host reports for the `where`, `all` or `named` call
/// that chose its selector, or for `on` when `on` is what starts the block.
/// The blocks of every source together stay within the rule file's block
/// limit, and the sources stay within `max_plan_sources`. The statement and
/// text limits apply to Lua blocks as they apply to a rule file. Passing a
/// limit refuses the plan.
///
/// @param state state that holds the list; must be the state the forms were built in
/// @param rules_index stack index of the rules list
/// @param read reads one package file; not called for a path outside the package
/// @param entry_name name given to each source built from Lua blocks
/// @return the sources, the findings and the error
[[nodiscard]] PlanRules read_plan_rules(
    lua_State* state, int rules_index, const PackageReader& read, std::string_view entry_name
);

/// Reports whether the value is a finished `where():change()` block.
///
/// @param state state that holds the value
/// @param index stack index
/// @return true when the block chose `where` and `:change` has been called
[[nodiscard]] bool finished_where_rule(lua_State* state, int index);

/// Pushes the needs table a rule block keeps, or nil.
///
/// @param state state that holds the block
/// @param rule stack index of the block
void push_rule_needs(lua_State* state, int rule);

/// Pushes the changes table a rule block keeps, or nil.
///
/// @param state state that holds the block
/// @param rule stack index of the block
void push_rule_changes(lua_State* state, int rule);

/// Reads a `set` or `scale` value built by the rule forms.
///
/// @param state state that holds the value
/// @param index stack index
/// @param[out] kind the change kind; may be null
/// @param[out] value the number or factor; may be null
/// @param[out] low the lower clamp; may be null
/// @param[out] high the upper clamp; may be null
/// @return false when the value is not a change
[[nodiscard]] bool rule_change_at(
    lua_State* state,
    int index,
    oa::formats::soft_mutator::ChangeKind* kind,
    double* value,
    double* low,
    double* high
);

} // namespace oa::data::mutators
