// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A route over one movement map. The search runs on the calling thread, on a
// private grid, and does not change the map or any unit.
#pragma once

#include "oa/sim/ground_orders/movement_map.hpp"

#include <cstdint>

namespace oa::sim::brains {

/// Instructions added to every search, on top of the cells it expands.
inline constexpr uint64_t route_call_charge = 16;

/// The most cells one search expands. That is the cells of a 1024 by 1024
/// map, so one search stays inside a tick's allowance.
inline constexpr uint32_t route_cell_cap = 1u << 20;

/// Returns the charge for a search that expanded `cells`.
///
/// One instruction per expanded cell, plus `route_call_charge`. Zero cells
/// still cost the per-call charge.
///
/// @param cells cells the search expanded
/// @return the instruction charge
[[nodiscard]] inline uint64_t route_charge(uint32_t cells) noexcept {
    return static_cast<uint64_t>(cells) + route_call_charge;
}

/// One cell of a route, in movement-map coordinates.
struct RouteCell {
    /// Column.
    int32_t column{};
    /// Row.
    int32_t row{};
};

/// The route `find_route` found, or a miss. Plain values only: the path, when
/// one is wanted, is written into the caller's buffer.
struct RouteResult {
    /// True when `a` and `b` meet and the search stayed inside its cap.
    bool found{};
    /// True when the search stopped because it reached `cell_cap`.
    bool capped{};
    /// Travel along the route. A clear side step is 16, a clear diagonal
    /// step is 22, and a slow cell adds 30. Zero when `found` is false.
    int32_t length{};
    /// Cells the route crosses, including both ends. Zero when `found` is false.
    uint32_t cells{};
    /// Cells popped from the open set, including a start that is itself the goal.
    uint32_t expanded{};
};

/// Searches `grid` from one anchor cell to another.
///
/// The open cells are the ones `grid` already holds: a cell of 1 or 3 is
/// open, and 0 is not. Eight neighbours are tried, in the planner's order,
/// with no corner check. A slow cell costs more than a clear one. Costs and
/// the tie-break are integers, so every machine agrees. An equal score keeps
/// the earlier step. The anchor is the start when it is open; otherwise the
/// first open cell of its 3 by 3, scanned by row then column. The goal
/// anchor is the only goal when it is open; otherwise every open cell of its
/// 3 by 3 is a goal.
///
/// The search does not prepare `grid`, does not read what a seat has seen,
/// and does not use the match's path-search worker. The private grid is freed
/// before this returns. The path is written into `path` only when `path` is
/// not null and `path_room` is at least the number of cells.
///
/// @param grid the class's movement map
/// @param from_column anchor column of the first position
/// @param from_row anchor row of the first position
/// @param to_column anchor column of the second position
/// @param to_row anchor row of the second position
/// @param cell_cap expanded cells after which the search stops
/// @param path receives the route, start cell first, or null to count only
/// @param path_room capacity of `path`
/// @return the route, or a miss (`found` false) when the anchors cannot
///     meet or the cap is reached
[[nodiscard]] RouteResult find_route(
    const sim::ground_orders::MovementMap& grid,
    int32_t from_column,
    int32_t from_row,
    int32_t to_column,
    int32_t to_row,
    uint32_t cell_cap,
    RouteCell* path,
    uint32_t path_room
);

/// Writes the cells where the route changes direction.
///
/// The first and last cells are not included. `room` is how many entries
/// `turns` can hold. The return can be larger than `room`; only the first
/// `room` turns are written. A null `path` or a null `turns` writes nothing.
///
/// @param path cells of a route, start cell first
/// @param cells how many cells `path` holds
/// @param turns receives the turn cells, or null to count only
/// @param room capacity of `turns`
/// @return how many turns the route has
[[nodiscard]] uint32_t
route_turns(const RouteCell* path, uint32_t cells, RouteCell* turns, uint32_t room) noexcept;

} // namespace oa::sim::brains
