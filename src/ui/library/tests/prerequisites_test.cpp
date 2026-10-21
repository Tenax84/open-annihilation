// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The order of made-up mutators: one id, a chain of three, a diamond, two
// roots that share a prerequisite, a cycle of two and of three, an id the
// graph does not hold and a repeated id; dependants direct, through another
// and none, in the list's order; an order that keeps a chain, a diamond and
// a list with an outside id, and one that does not; and a chain of 1,000.
#include "oa/test/check.hpp"
#include "oa/ui/library/prerequisites.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace lib = oa::ui::library;

constexpr std::string_view grounded = "grounded";
constexpr std::string_view gatehouse = "gatehouse";
constexpr std::string_view ridge_walls = "ridge-walls";
constexpr std::string_view gatehouse_keep = "gatehouse-keep";
constexpr std::string_view fast_builders = "fast-builders";
constexpr std::string_view outside = "not-a-mutator";

/// One need, for a short graph.
///
/// @param id the mutator's id
/// @param needs the ids it needs
/// @return the record
lib::Need need(std::string_view id, std::vector<std::string> needs) {
    return lib::Need{std::string(id), std::move(needs)};
}

/// The made-up mutators and what each needs.
///
/// Gatehouse Keep needs Gatehouse and Ridge Walls. Gatehouse and Ridge Walls
/// both need Grounded. Fast Builders needs nothing.
///
/// @return the graph
std::vector<lib::Need> made_up() {
    return {
        need(fast_builders, {}),
        need(grounded, {}),
        need(gatehouse, {std::string(grounded)}),
        need(ridge_walls, {std::string(grounded)}),
        need(gatehouse_keep, {std::string(gatehouse), std::string(ridge_walls)}),
    };
}

/// Checks a closure's ids and whether it met a cycle.
///
/// @param result the closure
/// @param expected the ids, in order
/// @param cyclic whether a cycle remains
void check_closure(
    const lib::Closure& result, const std::vector<std::string>& expected, bool cyclic
) {
    OA_CHECK(result.cyclic == cyclic);
    OA_CHECK(result.order.size() == expected.size());
    if (result.order.size() != expected.size())
        return;
    for (std::size_t index = 0; index < expected.size(); ++index)
        OA_CHECK(result.order[index] == expected[index]);
}

/// Checks a list of ids.
///
/// @param actual the ids written
/// @param expected the ids the case expects
void check_ids(const std::vector<std::string>& actual, const std::vector<std::string>& expected) {
    OA_CHECK(actual.size() == expected.size());
    if (actual.size() != expected.size())
        return;
    for (std::size_t index = 0; index < expected.size(); ++index)
        OA_CHECK(actual[index] == expected[index]);
}

/// One id, a chain of three, a diamond, two roots, and a repeated id.
void closures() {
    const std::vector<lib::Need> graph = made_up();
    const std::string only_grounded[]{std::string(grounded)};
    check_closure(lib::prerequisite_closure(graph, only_grounded), {std::string(grounded)}, false);

    const lib::Need chain[]{
        need(grounded, {}),
        need(gatehouse, {std::string(grounded)}),
        need(gatehouse_keep, {std::string(gatehouse)}),
    };
    const std::string chain_from[]{std::string(gatehouse_keep)};
    check_closure(
        lib::prerequisite_closure(chain, chain_from),
        {std::string(grounded), std::string(gatehouse), std::string(gatehouse_keep)},
        false
    );

    const std::string keep_only[]{std::string(gatehouse_keep)};
    check_closure(
        lib::prerequisite_closure(graph, keep_only),
        {std::string(grounded),
         std::string(gatehouse),
         std::string(ridge_walls),
         std::string(gatehouse_keep)},
        false
    );

    const std::string two_roots[]{std::string(ridge_walls), std::string(gatehouse)};
    check_closure(
        lib::prerequisite_closure(graph, two_roots),
        {std::string(grounded), std::string(ridge_walls), std::string(gatehouse)},
        false
    );

    const std::string repeated[]{std::string(gatehouse_keep), std::string(gatehouse_keep)};
    check_closure(
        lib::prerequisite_closure(graph, repeated),
        {std::string(grounded),
         std::string(gatehouse),
         std::string(ridge_walls),
         std::string(gatehouse_keep)},
        false
    );
}

/// An id the graph does not hold stays, and needs nothing of its own.
void outside_id() {
    const std::vector<lib::Need> graph = made_up();
    const std::string ids[]{std::string(outside), std::string(gatehouse)};
    check_closure(
        lib::prerequisite_closure(graph, ids),
        {std::string(outside), std::string(grounded), std::string(gatehouse)},
        false
    );
}

/// A cycle of two and of three keeps each id once, in the order first met,
/// and an id that only needs a cycle stays below that cycle.
void cycles() {
    const lib::Need pair[]{
        need(gatehouse, {std::string(ridge_walls)}),
        need(ridge_walls, {std::string(gatehouse)}),
    };
    const std::string pair_from[]{std::string(gatehouse)};
    check_closure(
        lib::prerequisite_closure(pair, pair_from),
        {std::string(gatehouse), std::string(ridge_walls)},
        true
    );

    const lib::Need triple[]{
        need(gatehouse_keep, {std::string(gatehouse)}),
        need(gatehouse, {std::string(ridge_walls)}),
        need(ridge_walls, {std::string(gatehouse_keep)}),
    };
    const std::string triple_from[]{std::string(gatehouse)};
    check_closure(
        lib::prerequisite_closure(triple, triple_from),
        {std::string(gatehouse), std::string(ridge_walls), std::string(gatehouse_keep)},
        true
    );

    const lib::Need above_pair[]{
        need("x", {std::string("a")}),
        need("a", {std::string("b")}),
        need("b", {std::string("a")}),
    };
    const std::string above_pair_from[]{std::string("x")};
    check_closure(
        lib::prerequisite_closure(above_pair, above_pair_from),
        {std::string("a"), std::string("b"), std::string("x")},
        true
    );

    const lib::Need above_deeper[]{
        need("keep", {std::string("gate")}),
        need("gate", {std::string("wall"), std::string("base")}),
        need("wall", {std::string("tower")}),
        need("tower", {std::string("wall")}),
        need("base", {}),
    };
    const std::string above_deeper_from[]{std::string("keep")};
    check_closure(
        lib::prerequisite_closure(above_deeper, above_deeper_from),
        {std::string("wall"),
         std::string("tower"),
         std::string("base"),
         std::string("gate"),
         std::string("keep")},
        true
    );
}

/// Dependants direct, through another id, and none, in the list's order.
void dependants() {
    const std::vector<lib::Need> graph = made_up();
    const std::string direct_among[]{
        std::string(ridge_walls), std::string(fast_builders), std::string(gatehouse)
    };
    check_ids(
        lib::dependants_of(graph, direct_among, grounded),
        {std::string(ridge_walls), std::string(gatehouse)}
    );

    const std::string through_among[]{std::string(fast_builders), std::string(gatehouse_keep)};
    check_ids(lib::dependants_of(graph, through_among, grounded), {std::string(gatehouse_keep)});

    const std::string none_among[]{std::string(fast_builders), std::string(grounded)};
    check_ids(lib::dependants_of(graph, none_among, fast_builders), {});

    const std::string ordered[]{
        std::string(ridge_walls),
        std::string(fast_builders),
        std::string(gatehouse_keep),
        std::string(gatehouse),
    };
    check_ids(
        lib::dependants_of(graph, ordered, grounded),
        {std::string(ridge_walls), std::string(gatehouse_keep), std::string(gatehouse)}
    );
}

/// A chain, a diamond and a list that names an id outside the graph.
void order_holds() {
    const std::vector<lib::Need> graph = made_up();
    const std::string chain_above[]{
        std::string(grounded), std::string(gatehouse), std::string(gatehouse_keep)
    };
    const std::string chain_below[]{
        std::string(gatehouse_keep), std::string(gatehouse), std::string(grounded)
    };
    OA_CHECK(lib::prerequisites_above(graph, chain_above));
    OA_CHECK(!lib::prerequisites_above(graph, chain_below));

    const std::string diamond_above[]{
        std::string(grounded),
        std::string(gatehouse),
        std::string(ridge_walls),
        std::string(gatehouse_keep),
    };
    const std::string diamond_below[]{
        std::string(gatehouse),
        std::string(ridge_walls),
        std::string(grounded),
        std::string(gatehouse_keep),
    };
    OA_CHECK(lib::prerequisites_above(graph, diamond_above));
    OA_CHECK(!lib::prerequisites_above(graph, diamond_below));

    const std::string outside_above[]{
        std::string(outside), std::string(grounded), std::string(gatehouse)
    };
    const std::string outside_below[]{
        std::string(gatehouse), std::string(outside), std::string(grounded)
    };
    OA_CHECK(lib::prerequisites_above(graph, outside_above));
    OA_CHECK(!lib::prerequisites_above(graph, outside_below));
}

/// A chain of 1,000 closes, the first id at the top, without a cycle.
void long_chain() {
    constexpr std::size_t count = 1000;
    std::vector<lib::Need> graph;
    graph.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        std::vector<std::string> needs;
        if (index > 0)
            needs.push_back("m" + std::to_string(index - 1));
        graph.push_back(need("m" + std::to_string(index), std::move(needs)));
    }
    const std::string ids[]{graph.back().id};
    const lib::Closure result = lib::prerequisite_closure(graph, ids);
    OA_CHECK(!result.cyclic);
    OA_CHECK(result.order.size() == count);
    if (result.order.size() == count) {
        OA_CHECK(result.order.front() == "m0");
        OA_CHECK(result.order.back() == "m999");
        OA_CHECK(result.order[500] == "m500");
    }
    const lib::Closure again = lib::prerequisite_closure(graph, ids);
    OA_CHECK(again.order == result.order);
    OA_CHECK(again.cyclic == result.cyclic);
}

} // namespace

int main() {
    closures();
    outside_id();
    cycles();
    dependants();
    order_holds();
    long_chain();
    return oa::test::check_exit_status();
}
