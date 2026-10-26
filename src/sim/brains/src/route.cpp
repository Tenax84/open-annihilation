// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Route estimate for one movement class. The match's path-search worker
// prepares the movement map and belongs to the match, so this search cannot
// call it. It uses that planner's heap, step costs and neighbour order on
// the calling thread, over a private grid.
#include "route.hpp"

#include "oa/sim/ground_orders/search_heap.hpp"
#include "oa/sim/ground_orders/search_worker.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>

namespace oa::sim::brains {
namespace orders = sim::ground_orders;

namespace {

constexpr uint8_t mark_visit = 3;
constexpr uint8_t mark_open = 1;
constexpr uint8_t mark_closed = 2;
constexpr uint8_t mark_blocked = 3;
constexpr uint8_t mark_goal = 4;
constexpr uint8_t no_arrival = 0xff;
constexpr int32_t cell_limit = 32767;

struct GridCell {
    uint8_t marks{};
    uint8_t arrived{no_arrival};
    uint32_t handle{};
};

struct GoalCell {
    int32_t column{};
    int32_t row{};
};

bool cell_in_map(const orders::MovementMap& grid, int32_t column, int32_t row) noexcept {
    return column >= 0 && row >= 0 && static_cast<uint32_t>(column) < grid.width() &&
           static_cast<uint32_t>(row) < grid.height();
}

bool cell_open(const orders::MovementMap& grid, int32_t column, int32_t row) noexcept {
    return cell_in_map(grid, column, row) &&
           grid.cell(static_cast<uint32_t>(column), static_cast<uint32_t>(row)) >= 1;
}

int32_t
nearest_goal(int32_t column, int32_t row, const GoalCell* goals, uint32_t goal_count) noexcept {
    int32_t best = std::numeric_limits<int32_t>::max();
    const int32_t side = orders::search_cardinal_cost;
    const int32_t diagonal_extra = static_cast<int32_t>(orders::search_diagonal_cost) - side;
    for (uint32_t index = 0; index < goal_count; ++index) {
        const int32_t raw_column = column - goals[index].column;
        const int32_t raw_row = row - goals[index].row;
        const int32_t across_column = raw_column < 0 ? -raw_column : raw_column;
        const int32_t across_row = raw_row < 0 ? -raw_row : raw_row;
        const int32_t across = across_column > across_row ? across_column : across_row;
        const int32_t along = across_column > across_row ? across_row : across_column;
        const int32_t estimate = across * side + along * diagonal_extra;
        if (estimate < best)
            best = estimate;
    }
    return best;
}

orders::SearchHeap::Payload
pack_open(int32_t column, int32_t row, int32_t travelled, int32_t score_value) noexcept {
    orders::SearchHeap::Payload value{};
    value[0] = static_cast<uint32_t>(static_cast<uint16_t>(static_cast<int16_t>(column))) |
               (static_cast<uint32_t>(static_cast<uint16_t>(static_cast<int16_t>(row))) << 16);
    value[1] = std::bit_cast<uint32_t>(travelled);
    value[2] = std::bit_cast<uint32_t>(score_value);
    return value;
}

int32_t payload_column(const orders::SearchHeap::Payload& value) noexcept {
    return std::bit_cast<int16_t>(static_cast<uint16_t>(value[0]));
}

int32_t payload_row(const orders::SearchHeap::Payload& value) noexcept {
    return std::bit_cast<int16_t>(static_cast<uint16_t>(value[0] >> 16));
}

int32_t payload_travel(const orders::SearchHeap::Payload& value) noexcept {
    return std::bit_cast<int32_t>(value[1]);
}

int step_between(const RouteCell& from, const RouteCell& to) noexcept {
    const int32_t delta_column = to.column - from.column;
    const int32_t delta_row = to.row - from.row;
    for (int direction = 0; direction < 8; ++direction) {
        const auto index = static_cast<std::size_t>(direction);
        if (orders::search_direction_x[index] == delta_column &&
            orders::search_direction_z[index] == delta_row)
            return direction;
    }
    return -1;
}

bool pick_start(
    const orders::MovementMap& grid,
    int32_t anchor_column,
    int32_t anchor_row,
    int32_t* column,
    int32_t* row
) noexcept {
    if (cell_open(grid, anchor_column, anchor_row)) {
        *column = anchor_column;
        *row = anchor_row;
        return true;
    }
    for (int32_t delta_row = -1; delta_row <= 1; ++delta_row) {
        for (int32_t delta_column = -1; delta_column <= 1; ++delta_column) {
            if (delta_column == 0 && delta_row == 0)
                continue;
            const int32_t next_column = anchor_column + delta_column;
            const int32_t next_row = anchor_row + delta_row;
            if (!cell_open(grid, next_column, next_row))
                continue;
            *column = next_column;
            *row = next_row;
            return true;
        }
    }
    return false;
}

uint32_t collect_goals(
    const orders::MovementMap& grid, int32_t anchor_column, int32_t anchor_row, GoalCell* goals
) noexcept {
    if (cell_open(grid, anchor_column, anchor_row)) {
        goals[0] = GoalCell{anchor_column, anchor_row};
        return 1;
    }
    uint32_t count = 0;
    for (int32_t delta_row = -1; delta_row <= 1; ++delta_row) {
        for (int32_t delta_column = -1; delta_column <= 1; ++delta_column) {
            const int32_t next_column = anchor_column + delta_column;
            const int32_t next_row = anchor_row + delta_row;
            if (!cell_open(grid, next_column, next_row))
                continue;
            goals[count] = GoalCell{next_column, next_row};
            ++count;
        }
    }
    return count;
}

std::size_t grid_index(uint32_t columns, int32_t column, int32_t row) noexcept {
    return static_cast<std::size_t>(row) * static_cast<std::size_t>(columns) +
           static_cast<std::size_t>(column);
}

bool cell_in_map_size(uint32_t columns, uint32_t rows, int32_t column, int32_t row) noexcept {
    return column >= 0 && row >= 0 && static_cast<uint32_t>(column) < columns &&
           static_cast<uint32_t>(row) < rows;
}

bool reconstruct(
    const GridCell* cells,
    uint32_t columns,
    uint32_t rows,
    int32_t start_column,
    int32_t start_row,
    int32_t goal_column,
    int32_t goal_row,
    uint32_t expanded,
    RouteCell* path,
    uint32_t path_room,
    RouteResult* result
) {
    int32_t column = goal_column;
    int32_t row = goal_row;
    uint32_t count = 0;
    while (true) {
        ++count;
        if (column == start_column && row == start_row)
            break;
        if (count > expanded || !cell_in_map_size(columns, rows, column, row))
            return false;
        const GridCell& current = cells[grid_index(columns, column, row)];
        if (current.arrived > 7)
            return false;
        const auto direction = static_cast<std::size_t>(current.arrived);
        column -= orders::search_direction_x[direction];
        row -= orders::search_direction_z[direction];
    }
    result->found = true;
    result->cells = count;
    if (path == nullptr || path_room < count)
        return true;
    column = goal_column;
    row = goal_row;
    for (uint32_t index = count; index > 0; --index) {
        path[index - 1] = RouteCell{column, row};
        if (column == start_column && row == start_row)
            break;
        const GridCell& current = cells[grid_index(columns, column, row)];
        const auto direction = static_cast<std::size_t>(current.arrived);
        column -= orders::search_direction_x[direction];
        row -= orders::search_direction_z[direction];
    }
    return true;
}

} // namespace

RouteResult find_route(
    const orders::MovementMap& grid,
    int32_t from_column,
    int32_t from_row,
    int32_t to_column,
    int32_t to_row,
    uint32_t cell_cap,
    RouteCell* path,
    uint32_t path_room
) {
    RouteResult result;
    if (cell_cap == 0) {
        result.capped = true;
        return result;
    }
    const uint32_t columns = grid.width();
    const uint32_t rows = grid.height();
    if (columns == 0 || rows == 0 || columns > static_cast<uint32_t>(cell_limit) ||
        rows > static_cast<uint32_t>(cell_limit))
        return result;
    const uint64_t cell_count = static_cast<uint64_t>(columns) * static_cast<uint64_t>(rows);
    if (cell_count == 0 || cell_count > static_cast<uint64_t>(SIZE_MAX))
        return result;

    int32_t start_column = 0;
    int32_t start_row = 0;
    if (!pick_start(grid, from_column, from_row, &start_column, &start_row))
        return result;
    GoalCell goals[9]{};
    const uint32_t goal_count = collect_goals(grid, to_column, to_row, goals);
    if (goal_count == 0)
        return result;

    auto cells = std::unique_ptr<GridCell[]>(new (std::nothrow)
                                                 GridCell[static_cast<std::size_t>(cell_count)]());
    if (cells == nullptr)
        return result;
    for (uint32_t index = 0; index < goal_count; ++index) {
        GridCell& goal = cells[grid_index(columns, goals[index].column, goals[index].row)];
        goal.marks = static_cast<uint8_t>(goal.marks | mark_goal);
    }

    try {
        orders::SearchHeap heap;
        const int32_t start_estimate = nearest_goal(start_column, start_row, goals, goal_count);
        const orders::SearchHeap::Handle start_handle =
            heap.insert(pack_open(start_column, start_row, 0, start_estimate));
        if (start_handle == orders::SearchHeap::no_handle)
            return result;
        GridCell& start = cells[grid_index(columns, start_column, start_row)];
        start.marks = static_cast<uint8_t>((start.marks & mark_goal) | mark_open);
        start.handle = start_handle;
        start.arrived = no_arrival;

        while (!heap.exhausted()) {
            if (result.expanded >= cell_cap) {
                result.capped = true;
                return result;
            }
            const orders::SearchHeap::Payload parent = heap.peek();
            heap.pop();
            ++result.expanded;
            const int32_t column = payload_column(parent);
            const int32_t row = payload_row(parent);
            const int32_t travelled = payload_travel(parent);
            if (!cell_in_map_size(columns, rows, column, row))
                continue;
            GridCell& current = cells[grid_index(columns, column, row)];
            if ((current.marks & mark_goal) != 0) {
                result.length = travelled;
                if (!reconstruct(
                        cells.get(),
                        columns,
                        rows,
                        start_column,
                        start_row,
                        column,
                        row,
                        result.expanded,
                        path,
                        path_room,
                        &result
                    ))
                    result.found = false;
                return result;
            }
            current.marks = mark_closed;
            const int32_t step_limit = std::numeric_limits<int32_t>::max();
            for (uint8_t direction = 0; direction < 8; ++direction) {
                const auto index = static_cast<std::size_t>(direction);
                const int32_t next_column = column + orders::search_direction_x[index];
                const int32_t next_row = row + orders::search_direction_z[index];
                if (!cell_in_map_size(columns, rows, next_column, next_row))
                    continue;
                GridCell& next = cells[grid_index(columns, next_column, next_row)];
                const uint8_t visit = static_cast<uint8_t>(next.marks & mark_visit);
                if (visit == mark_closed || visit == mark_blocked)
                    continue;
                const uint8_t classification =
                    grid.cell(static_cast<uint32_t>(next_column), static_cast<uint32_t>(next_row));
                if (classification < 1) {
                    next.marks = mark_blocked;
                    continue;
                }
                const int32_t base = (direction & 1u) != 0
                                         ? static_cast<int32_t>(orders::search_diagonal_cost)
                                         : static_cast<int32_t>(orders::search_cardinal_cost);
                const int32_t extra =
                    classification > 1 ? 0 : static_cast<int32_t>(orders::search_difficult_extra);
                if (travelled > step_limit - base - extra)
                    continue;
                const int32_t next_travel = travelled + base + extra;
                const int32_t estimate = nearest_goal(next_column, next_row, goals, goal_count);
                if (estimate == step_limit || next_travel > step_limit - estimate)
                    continue;
                const int32_t score_value = next_travel + estimate;
                if (visit == mark_open) {
                    orders::SearchHeap::Payload& node = heap.payload(next.handle);
                    if (next_travel >= payload_travel(node))
                        continue;
                    node[1] = std::bit_cast<uint32_t>(next_travel);
                    node[2] = std::bit_cast<uint32_t>(score_value);
                    next.arrived = direction;
                    heap.decrease_key(next.handle);
                    continue;
                }
                const orders::SearchHeap::Handle handle =
                    heap.insert(pack_open(next_column, next_row, next_travel, score_value));
                if (handle == orders::SearchHeap::no_handle)
                    continue;
                next.marks = static_cast<uint8_t>((next.marks & mark_goal) | mark_open);
                next.handle = handle;
                next.arrived = direction;
            }
        }
    } catch (const std::bad_alloc&) {
        result.found = false;
    }
    return result;
}

uint32_t
route_turns(const RouteCell* path, uint32_t cells, RouteCell* turns, uint32_t room) noexcept {
    if (path == nullptr || cells < 3)
        return 0;
    uint32_t count = 0;
    int previous = step_between(path[0], path[1]);
    for (uint32_t index = 1; index + 1 < cells; ++index) {
        const int next = step_between(path[index], path[index + 1]);
        if (next == previous)
            continue;
        if (turns != nullptr && count < room)
            turns[count] = path[index];
        ++count;
        previous = next;
    }
    return count;
}

} // namespace oa::sim::brains
