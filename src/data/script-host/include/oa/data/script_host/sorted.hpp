// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Names and table keys in an order that does not depend on where a value sits
// in memory. `pairs` uses the same key order.
#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

struct lua_State;

namespace oa::data::script_host {

/// Compares two names without regard to ASCII letter case.
///
/// Letters A-Z and a-z fold together. Any other byte compares as itself.
/// When the folded names are equal, the raw bytes break the tie, and a name
/// that is a prefix of the other comes first. Does not raise.
///
/// @param left first name
/// @param right second name
/// @return true when `left` comes before `right`
[[nodiscard]] bool name_less(std::string_view left, std::string_view right) noexcept;

/// Sorts names into `name_less` order.
///
/// Does not call Lua and does not raise.
///
/// @param names names to reorder
void sort_names(std::vector<std::string>& names);

/// Pushes a sequence of names in the order given.
///
/// The caller sorts first, outside any protected region. May raise if the
/// state cannot hold the sequence.
///
/// @param state state to push into
/// @param names names, already in the order to publish
void push_names(lua_State* state, std::span<const std::string> names);

/// Pushes a sequence of a table's keys in `pairs` order.
///
/// Number keys come first, ascending as Lua compares numbers, then string
/// keys in byte order, then false and true. A key whose type is anything
/// else raises `pairs: this table has keys of type <type>, which have no
/// fixed order; keep such values in a sequence and walk it with ipairs`.
/// May raise, including while it sorts.
///
/// @param state state to push into
/// @param index stack index of the table
/// @return how many keys the sequence holds
int push_sorted_keys(lua_State* state, int index);

} // namespace oa::data::script_host
