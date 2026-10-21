// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The order every mutator screen uses. A prerequisite is switched on with
// the mutator that needs it and placed above it; the list applies from the
// top down. The graph is ids only: a caller turns its records into needs.
// Version ranges are the caller's.
#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::ui::library {

/// One mutator id and the ids it needs, compared exactly as the catalogue writes them.
struct Need {
    std::string id;                 ///< the mutator's id
    std::vector<std::string> needs; ///< the ids it needs, each before it
};

/// The ids a set reaches, in the order they apply, and whether a cycle remains.
struct Closure {
    /// Each id after everything it needs. The ids of one cycle stay together,
    /// in the order first met.
    std::vector<std::string> order;
    bool cyclic{}; ///< a cycle kept each of its ids once
};

/// Returns every id `ids` reach, each placed after everything it needs.
///
/// An id is met when `ids` names it, or when a need list names it. The ids
/// already met are read in the order they were met, each need list from
/// left to right, and one id's needs are finished before the next id in
/// `ids`. The ids of one cycle stay together in that order. Every other
/// id goes after everything it needs. A component goes after the components
/// it needs, and a tie keeps the component whose id was met first. An id
/// the graph does not hold is a leaf. A cycle sets `cyclic`. A repeated id
/// in `ids` is kept once. The walk is bounded by the graph.
///
/// @param graph each id and the ids it needs; the first record of an id is the one read
/// @param ids the ids being switched on or downloaded
/// @return those ids and everything they need, top to bottom
[[nodiscard]] Closure
prerequisite_closure(std::span<const Need> graph, std::span<const std::string> ids);

/// Returns the ids in `among` that need `id`, directly or through others.
///
/// The result keeps `among`'s order, including a repeated id. `id` itself
/// is included only when a cycle leads back to it.
///
/// @param graph each id and the ids it needs; the first record of an id is the one read
/// @param among the ids to choose from, in their order
/// @param id the id whose dependants are wanted
/// @return the dependants, in `among`'s order
[[nodiscard]] std::vector<std::string>
dependants_of(std::span<const Need> graph, std::span<const std::string> among, std::string_view id);

/// Tells whether every prerequisite in `order` stands above what needs it.
///
/// True when no id comes before an id of `order` that it needs, directly
/// or through others. An id the graph does not hold needs nothing.
///
/// @param graph each id and the ids it needs; the first record of an id is the one read
/// @param order the list, top to bottom
/// @return true when the order keeps every prerequisite above what needs it
[[nodiscard]] bool
prerequisites_above(std::span<const Need> graph, std::span<const std::string> order);

} // namespace oa::ui::library
