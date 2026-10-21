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

/// Publishes the rule forms in a state.
///
/// The globals are `where`, `all`, `named`, `on`, `set`, `scale`, `lock`,
/// `remove`, `append` and `sm`. `range`, `is`, `has`, `absent` and `not_`
/// are published too, by `install_selection_forms`. `where`, `all`, `named`
/// and `on` build an unfinished block. `:change { }` finishes it, and a
/// second `:change` is an error. `sm` holds a path; it does not read.
///
/// The state is one the script host opened. The forms' metatables cannot
/// be read or replaced.
///
/// @param state state to publish into
void install_rule_forms(lua_State* state);

/// Reads one file from the mutator's package.
///
/// `path` uses `/` separators. The function returns the bytes, or no value
/// when the package has no such file. `error` receives a reason when it is
/// not null; it may be left empty. The function does not call into `state`.
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
/// else, and a block `:change` was not called on, refuses the plan. The
/// records are copied out: the result does not use the state after return.
///
/// A rule-file path is a relative path inside the package. `\` is read as
/// `/`. A path that is absolute, has a drive, or has a `..` segment is
/// refused, and the reader is not called. The reader is the only way the
/// bytes are obtained. A file that cannot be read, or that the rule parser
/// reports an error in, refuses the plan. The message names the path, and
/// for a parse error the line as well. Warnings are kept on the result.
///
/// Field names in one block are stored in the order of their names compared
/// after ASCII lower-casing, with the raw bytes breaking a tie, so the
/// records do not depend on the order the keys were inserted. A block's
/// line is the line the host reports for the `where`, `all`, `named` or
/// `on` call that started it. The limits on blocks, statements and text
/// length apply to Lua blocks as they apply to a rule file.
///
/// @param state state that holds the list; must be the state the forms were built in
/// @param rules_index stack index of the rules list
/// @param read reads one package file; not called for a path outside the package
/// @param entry_name name given to each source built from Lua blocks
/// @return the sources, the findings and the error
[[nodiscard]] PlanRules read_plan_rules(
    lua_State* state, int rules_index, const PackageReader& read, std::string_view entry_name
);

} // namespace oa::data::mutators
