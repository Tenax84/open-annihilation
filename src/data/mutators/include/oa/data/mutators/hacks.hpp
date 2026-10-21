// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A mutator's hacks: the match parameters the registry opens to mutators,
// read from a plan, merged across the list, and noted where they overlap
// Developer Mode or each other.
#pragma once

#include "oa/data/mod_profile/overrides.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

struct lua_State;

namespace oa::data::mutators {

/// One parameter a mutator's plan sets for one game.
struct MutatorHack {
    /// The mutator's id.
    std::string mutator;
    /// ENTRY.PARAM, such as console.game-speed-range.max.
    std::string path;
    /// The value in the registry's normal form, such as 15.
    std::string value;
};

/// A sentence for the picker's conflict report.
struct HackNote {
    /// The registry entry, such as console.game-speed-range.
    std::string entry;
    /// The sentence.
    std::string text;
    /// The earlier mutator, when two mutators set one path. Empty otherwise.
    std::string earlier;
    /// The mutator whose value plays.
    std::string mutator;
};

/// The hacks that play, and a note for each path two mutators both set.
struct HackMerge {
    /// One record per path, sorted by path. The later mutator's value wins.
    std::vector<MutatorHack> hacks;
    /// One note per overwritten path, sorted by entry.
    std::vector<HackNote> notes;
};

/// Reads a plan's hacks table into records, in sorted path order.
///
/// A table of parameters gives one record each. A scalar gives one when the
/// entry has a shorthand parameter, or, with no shorthand, exactly one
/// parameter the registry marks adjustable per match. true, false or any
/// other scalar for an entry without that is refused. A hack the registry
/// does not mark mutator: true is refused before its parameters are read.
/// On failure `out` is cleared.
///
/// @param state the sandbox state
/// @param index the table's stack index
/// @param mutator the mutator's id, recorded on each hack
/// @param declares_layer whether the manifest lists the hacks layer
/// @param[out] out the records; cleared when the table is refused
/// @return the refusal, starting with "hacks: ", or empty when it was read
[[nodiscard]] std::string read_hacks(
    lua_State* state,
    int index,
    std::string_view mutator,
    bool declares_layer,
    std::vector<MutatorHack>& out
);

/// Merges hack records in list order. The later value wins for a path.
///
/// Each overwritten path is one note, even when the two values are equal:
/// "steady-pace and fast-pace both set console.game-speed-range.min;
/// fast-pace's value plays".
///
/// @param hacks the records, in list order
/// @return the records that play, and the notes
[[nodiscard]] HackMerge merge_hacks(std::span<const MutatorHack> hacks);

/// Notes where a mutator's hack overlaps Developer Mode or a mod that leaves
/// it off.
///
/// One note per entry. An override that turns the entry off is "Developer
/// Mode turns <id> off; <mutator> turns it on for this game". An override
/// that leaves it on is "Developer Mode also sets <id>". An entry the profile
/// leaves off, with no override, is "the mod leaves <id> off; <mutator> turns
/// it on for this game". The mutator named is the later one in list order.
///
/// @param hacks the records that play, in list order
/// @param overrides Developer Mode's overrides; empty when it is off
/// @param profile_on the entries the profile turns on, before overrides
/// @return the notes, in the order the entries first appear
[[nodiscard]] std::vector<HackNote> developer_conflicts(
    std::span<const MutatorHack> hacks,
    std::span<const oa::data::mod_profile::HackOverride> overrides,
    std::span<const std::string> profile_on
);

} // namespace oa::data::mutators
