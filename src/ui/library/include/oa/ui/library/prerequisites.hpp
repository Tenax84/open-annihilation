// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The order every mutator screen uses. A prerequisite is switched on with
// the mutator that needs it and placed above it; the list applies from the
// top down. The graph is ids only: a caller turns its records into needs.
// Version ranges are the caller's. The download plan and its question are
// here too, so the picker asks for the same packages the Library does.
#pragma once

#include "oa/ui/kit/components_more.hpp"
#include "oa/ui/library/library.hpp"

#include <optional>
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

/// The ids each mutator entry needs, in the Library's entry order.
///
/// One record per mutator entry. The id is the package key. The needs are
/// the ids of `MutatorFacts::needs` on the installed manifest when the entry
/// is installed, otherwise on the listing. An entry with no facts needs
/// nothing. The closure reads the first record of an id.
///
/// @param library the Library
/// @return the graph `prerequisite_closure` reads
[[nodiscard]] std::vector<Need> needs_of(const Library& library);

/// One package a GET WITH downloads, in the order it is queued.
struct GetStep {
    EntryId id{};                ///< the registry, kind and key that are queued
    std::string name{};          ///< the name a player sees
    std::string version{};       ///< the version the listing offers
    std::string registry_name{}; ///< the registry's name; its id when it has none
    const Badge* badge{};        ///< the listing's badge; null without one; valid until refresh
    std::optional<std::string> tag{}; ///< the first tag id, for the icon when there is no picture
};

/// The packages GET WITH queues, prerequisites first, and why it cannot.
struct GetPlan {
    std::vector<GetStep> steps{}; ///< the downloads, in closure order, then the entry
    /// Why the button is off, in the player's words. Empty when every step can be queued.
    std::string blocked{};
};

/// The packages getting `entry` downloads, prerequisites first.
///
/// The closure is `prerequisite_closure` over `needs_of`. A prerequisite
/// already installed inside every range that asks for it is left out. One
/// installed below a range is an update of the listing that offers a version
/// inside every range: the entry's own registry, else a registry that ships
/// with the game, else the lowest registry id, and within those the newest
/// release. A prerequisite no listing offers inside its range blocks the
/// plan with `blocked_prerequisite` and is not a step. A step a player
/// cannot get for the reasons GET is off (a game being played, downloads
/// off, the engine, the base game, the hacks) blocks the plan with that
/// reason. A step that does not work with the mod being played does not.
/// A cycle blocks the plan, naming every id in the loop, and queues nothing.
///
/// @param library the Library
/// @param entry the mutator the player would get
/// @return the plan
[[nodiscard]] GetPlan get_plan(const Library& library, const Entry& entry);

/// The listing a package is downloaded from.
///
/// `preferred` wins when one of its listings meets the ranges, then a
/// registry that ships with the game, then the lowest registry id. Within
/// that set the newest release wins. An empty `preferred` skips the first
/// pool, which is how a row with no parent registry is chosen. This is the
/// listing rule `get_plan` uses.
///
/// @param library the Library
/// @param preferred the registry to prefer; empty skips that pool
/// @param id the package key
/// @param ranges the ranges the version must meet; empty is any version
/// @return the listing, or null when none meets the ranges
[[nodiscard]] const Listing* choose_listing(
    const Library& library,
    std::string_view preferred,
    std::string_view id,
    const std::vector<std::string>& ranges
);

/// The question's CANCEL button's name to automation. It is marked at first
/// and queues nothing.
inline constexpr std::string_view get_with_cancel_id = "cancel";
/// The question's GET BOTH button, when two packages download. It is primary.
inline constexpr std::string_view get_with_both_id = "get-both";
/// The question's GET ALL button, when three or more packages download. It is primary.
inline constexpr std::string_view get_with_all_id = "get-all";

/// The question GET WITH asks: "Get {name}?", one paragraph naming every
/// missing prerequisite, one item per package that will download (its badge,
/// "{name} {version}" and the registry), then CANCEL and GET BOTH or GET ALL.
///
/// CANCEL is marked at first. GET BOTH, for two packages, or GET ALL, for
/// more, is primary. The word is the kit's default, so automation reads
/// prompt.cancel, prompt.get-both or prompt.get-all, and prompt.body.
///
/// @param library the Library
/// @param entry the mutator
/// @return the question
[[nodiscard]] oa::ui::kit::Question get_with_question(const Library& library, const Entry& entry);

} // namespace oa::ui::library
