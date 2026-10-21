// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The closure, the dependants and the order check over a graph of mutator
// ids. Nothing here reads a file, a clock or the network, and nothing here
// holds a word a player sees.
#include "oa/ui/library/prerequisites.hpp"

#include <algorithm>
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

/// One step of an iterative walk over need edges.
struct Walk {
    std::size_t item = 0;
    std::size_t next = 0;
};

/// The strongly connected components, each id in the order it was met.
///
/// The edges point from an id to the ids it needs. The walk is a stack, so
/// a chain as long as the closure does not recurse.
///
/// @param items the closure, in the order met
/// @return the components; a component's indices rise in the order met
std::vector<std::vector<std::size_t>> components_of(const std::vector<Item>& items) {
    const std::size_t count = items.size();
    std::vector<char> seen(count, 0);
    std::vector<std::size_t> finish;
    finish.reserve(count);
    std::vector<Walk> stack;
    for (std::size_t start = 0; start < count; ++start) {
        if (seen[start] != 0)
            continue;
        seen[start] = 1;
        stack.push_back(Walk{start, 0});
        while (!stack.empty()) {
            const std::size_t item = stack.back().item;
            const std::vector<std::size_t>& edges = items[item].need_at;
            if (stack.back().next < edges.size()) {
                const std::size_t needed = edges[stack.back().next];
                ++stack.back().next;
                if (seen[needed] == 0) {
                    seen[needed] = 1;
                    stack.push_back(Walk{needed, 0});
                }
            } else {
                finish.push_back(item);
                stack.pop_back();
            }
        }
    }

    std::vector<std::vector<std::size_t>> incoming(count);
    for (std::size_t item = 0; item < count; ++item) {
        for (const std::size_t needed : items[item].need_at)
            incoming[needed].push_back(item);
    }

    std::vector<std::vector<std::size_t>> components;
    std::fill(seen.begin(), seen.end(), 0);
    for (std::size_t place = finish.size(); place > 0; --place) {
        const std::size_t start = finish[place - 1];
        if (seen[start] != 0)
            continue;
        std::vector<std::size_t> members;
        seen[start] = 1;
        stack.push_back(Walk{start, 0});
        while (!stack.empty()) {
            const std::size_t item = stack.back().item;
            const std::vector<std::size_t>& edges = incoming[item];
            if (stack.back().next < edges.size()) {
                const std::size_t dependant = edges[stack.back().next];
                ++stack.back().next;
                if (seen[dependant] == 0) {
                    seen[dependant] = 1;
                    stack.push_back(Walk{dependant, 0});
                }
            } else {
                members.push_back(item);
                stack.pop_back();
            }
        }
        std::sort(members.begin(), members.end());
        components.push_back(std::move(members));
    }
    return components;
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
    const std::vector<std::vector<std::size_t>> components = components_of(items);
    const std::size_t count = items.size();
    const std::size_t component_count = components.size();
    std::vector<std::size_t> component_of(count, 0);
    std::vector<std::size_t> indegree(component_count, 0);
    std::vector<std::vector<std::size_t>> waiting_on(component_count);
    Closure result;
    result.order.reserve(count);
    for (std::size_t component = 0; component < component_count; ++component) {
        for (const std::size_t member : components[component])
            component_of[member] = component;
        if (components[component].size() > 1)
            result.cyclic = true;
    }
    for (std::size_t component = 0; component < component_count; ++component) {
        std::vector<char> needed_component(component_count, 0);
        for (const std::size_t member : components[component]) {
            for (const std::size_t needed : items[member].need_at) {
                if (needed == member)
                    result.cyclic = true;
                const std::size_t other = component_of[needed];
                if (other == component || needed_component[other] != 0)
                    continue;
                needed_component[other] = 1;
                ++indegree[component];
                waiting_on[other].push_back(component);
            }
        }
    }

    std::vector<char> placed(component_count, 0);
    std::size_t remaining = component_count;
    while (remaining > 0) {
        std::size_t next = component_count;
        std::size_t earliest = count;
        for (std::size_t component = 0; component < component_count; ++component) {
            if (placed[component] != 0 || indegree[component] != 0)
                continue;
            const std::size_t met = components[component].front();
            if (met < earliest) {
                earliest = met;
                next = component;
            }
        }
        if (next == component_count)
            break;
        placed[next] = 1;
        --remaining;
        for (const std::size_t member : components[next])
            result.order.push_back(items[member].id);
        for (const std::size_t dependant : waiting_on[next])
            --indegree[dependant];
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
