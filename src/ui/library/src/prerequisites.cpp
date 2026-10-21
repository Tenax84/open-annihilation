// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The closure, the dependants and the order check over a graph of mutator
// ids. Nothing here reads a file, a clock or the network, and nothing here
// holds a word a player sees.
#include "oa/ui/library/prerequisites.hpp"

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::ui::library {

namespace {

/// One id in a closure, and the closure indices of the ids it needs.
struct Item {
    std::string id;
    std::vector<std::size_t> need_at;
};

/// One id whose need list is still being read.
struct Frame {
    std::size_t item = 0;
    std::size_t next = 0;
};

/// The first record of each id. A later record of the same id is not read.
///
/// @param graph the graph
/// @return the first index of each id
std::map<std::string, std::size_t, std::less<>> first_of(std::span<const Need> graph) {
    std::map<std::string, std::size_t, std::less<>> index;
    for (std::size_t slot = 0; slot < graph.size(); ++slot)
        index.emplace(graph[slot].id, slot);
    return index;
}

/// Adds an id to the closure the first time it is met.
///
/// @param index the closure index of each id met so far
/// @param items the closure, in the order met
/// @param id the id
/// @return its index in the closure
std::size_t meet(
    std::map<std::string, std::size_t, std::less<>>& index,
    std::vector<Item>& items,
    const std::string& id
) {
    const auto found = index.find(id);
    if (found != index.end())
        return found->second;
    const std::size_t at = items.size();
    items.push_back(Item{id, {}});
    index.emplace(id, at);
    return at;
}

/// Reads one id's needs before the ids named after it.
///
/// @param graph the graph
/// @param graph_at the first record of each id
/// @param items the closure, in the order met
/// @param met the closure index of each id met so far
/// @param root the id whose needs are read
void read_needs(
    std::span<const Need> graph,
    const std::map<std::string, std::size_t, std::less<>>& graph_at,
    std::vector<Item>& items,
    std::map<std::string, std::size_t, std::less<>>& met,
    std::size_t root
) {
    std::vector<Frame> frames;
    frames.push_back(Frame{root, 0});
    while (!frames.empty()) {
        const std::size_t item = frames.back().item;
        const auto recorded = graph_at.find(items[item].id);
        const std::vector<std::string>* needs =
            recorded == graph_at.end() ? nullptr : &graph[recorded->second].needs;
        if (needs == nullptr || frames.back().next >= needs->size()) {
            frames.pop_back();
            continue;
        }
        const std::string need_id = (*needs)[frames.back().next];
        ++frames.back().next;
        const std::size_t size_before = items.size();
        const std::size_t at = meet(met, items, need_id);
        std::vector<std::size_t>& slots = items[item].need_at;
        bool known = false;
        for (const std::size_t have : slots) {
            if (have == at) {
                known = true;
                break;
            }
        }
        if (!known)
            slots.push_back(at);
        if (items.size() != size_before)
            frames.push_back(Frame{at, 0});
    }
}

/// The closure in the order the ids were met, with one edge each way.
///
/// @param graph the graph
/// @param ids the ids being closed
/// @return the ids, each with the closure indices it needs
std::vector<Item> closure_items(std::span<const Need> graph, std::span<const std::string> ids) {
    const std::map<std::string, std::size_t, std::less<>> graph_at = first_of(graph);
    std::vector<Item> items;
    std::map<std::string, std::size_t, std::less<>> met;
    for (const std::string& id : ids) {
        const std::size_t size_before = items.size();
        const std::size_t root = meet(met, items, id);
        if (items.size() == size_before)
            continue;
        read_needs(graph, graph_at, items, met, root);
    }
    return items;
}

/// The ids that list each id, directly, from the first record of each id.
///
/// @param graph the graph
/// @return each needed id and the ids that list it
std::map<std::string, std::vector<std::string>, std::less<>>
dependants_by_need(std::span<const Need> graph) {
    const std::map<std::string, std::size_t, std::less<>> graph_at = first_of(graph);
    std::map<std::string, std::vector<std::string>, std::less<>> reverse;
    for (const auto& [id, slot] : graph_at) {
        std::vector<std::string> seen;
        for (const std::string& need : graph[slot].needs) {
            bool known = false;
            for (const std::string& earlier : seen) {
                if (earlier == need) {
                    known = true;
                    break;
                }
            }
            if (known)
                continue;
            seen.push_back(need);
            reverse[need].push_back(id);
        }
    }
    return reverse;
}

/// Tells whether `needed` is named in the list after `from`.
///
/// @param positions each id's places in the list
/// @param from the place that must stay above the need
/// @param needed the id that is needed
/// @return true when a later place names it
bool named_after(
    const std::map<std::string, std::vector<std::size_t>, std::less<>>& positions,
    std::size_t from,
    const std::string& needed
) {
    const auto found = positions.find(needed);
    if (found == positions.end())
        return false;
    for (const std::size_t place : found->second) {
        if (place > from)
            return true;
    }
    return false;
}

} // namespace

Closure prerequisite_closure(std::span<const Need> graph, std::span<const std::string> ids) {
    const std::vector<Item> items = closure_items(graph, ids);
    const std::size_t count = items.size();
    std::vector<std::size_t> indegree(count, 0);
    std::vector<std::vector<std::size_t>> dependants(count);
    for (std::size_t item = 0; item < count; ++item) {
        for (const std::size_t need : items[item].need_at) {
            ++indegree[item];
            dependants[need].push_back(item);
        }
    }

    std::vector<char> placed(count, 0);
    Closure result;
    result.order.reserve(count);
    std::size_t remaining = count;
    while (remaining > 0) {
        std::size_t next = count;
        for (std::size_t item = 0; item < count; ++item) {
            if (placed[item] == 0 && indegree[item] == 0) {
                next = item;
                break;
            }
        }
        if (next == count)
            break;
        placed[next] = 1;
        result.order.push_back(items[next].id);
        --remaining;
        for (const std::size_t dependant : dependants[next])
            --indegree[dependant];
    }
    if (remaining > 0) {
        result.cyclic = true;
        for (std::size_t item = 0; item < count; ++item) {
            if (placed[item] == 0)
                result.order.push_back(items[item].id);
        }
    }
    return result;
}

std::vector<std::string> dependants_of(
    std::span<const Need> graph, std::span<const std::string> among, std::string_view id
) {
    const std::map<std::string, std::vector<std::string>, std::less<>> reverse =
        dependants_by_need(graph);
    const auto direct = reverse.find(std::string(id));
    std::vector<std::string> waiting;
    if (direct != reverse.end())
        waiting = direct->second;
    std::map<std::string, bool, std::less<>> reached;
    std::size_t head = 0;
    while (head < waiting.size()) {
        const std::string current = waiting[head];
        ++head;
        if (!reached.emplace(current, true).second)
            continue;
        const auto further = reverse.find(current);
        if (further == reverse.end())
            continue;
        for (const std::string& dependant : further->second)
            waiting.push_back(dependant);
    }

    std::vector<std::string> chosen;
    for (const std::string& candidate : among) {
        if (reached.find(candidate) != reached.end())
            chosen.push_back(candidate);
    }
    return chosen;
}

bool prerequisites_above(std::span<const Need> graph, std::span<const std::string> order) {
    const std::map<std::string, std::size_t, std::less<>> graph_at = first_of(graph);
    std::map<std::string, std::vector<std::size_t>, std::less<>> positions;
    for (std::size_t place = 0; place < order.size(); ++place)
        positions[order[place]].push_back(place);

    for (std::size_t place = 0; place < order.size(); ++place) {
        const auto recorded = graph_at.find(order[place]);
        if (recorded == graph_at.end())
            continue;
        std::vector<std::string> waiting = graph[recorded->second].needs;
        std::map<std::string, bool, std::less<>> reached;
        std::size_t head = 0;
        while (head < waiting.size()) {
            const std::string needed = waiting[head];
            ++head;
            if (!reached.emplace(needed, true).second)
                continue;
            if (named_after(positions, place, needed))
                return false;
            const auto further = graph_at.find(needed);
            if (further == graph_at.end())
                continue;
            for (const std::string& deeper : graph[further->second].needs)
                waiting.push_back(deeper);
        }
    }
    return true;
}

} // namespace oa::ui::library
