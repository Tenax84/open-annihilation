// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The closure, the dependants and the order check over a graph of mutator
// ids, and the download plan and question built from that closure. Nothing
// here reads a file, a clock or the network.
#include "oa/ui/library/prerequisites.hpp"

#include "oa/data/mod_profile/registry.hpp"
#include "oa/formats/oamod/package_keys.hpp"
#include "oa/formats/oamut/manifest.hpp"
#include "oa/ui/frontend_renderer/artless.hpp"
#include "oa/ui/kit/tag_icon.hpp"
#include "oa/ui/kit/theme.hpp"
#include "oa/ui/library/screen.hpp"
#include "oa/ui/library/text.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
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

namespace {

namespace hacks = data::mod_profile::registry;

/// Joins phrases with a comma between all but the last pair.
///
/// @param text how texts are looked up
/// @param parts the phrases
/// @param last the pattern for the last pair
/// @return the joined phrases; empty when there are none
std::string
join_parts(const TextHooks& text, const std::vector<std::string>& parts, std::string_view last) {
    if (parts.empty())
        return {};
    if (parts.size() == 1)
        return parts.front();
    std::string joined = parts.front();
    for (std::size_t index = 1; index + 1 < parts.size(); ++index)
        joined = filled(text, words::names_comma, {{"left", joined}, {"right", parts[index]}});
    return filled(text, last, {{"left", joined}, {"right", parts.back()}});
}

/// Joins the words of every range that names one.
///
/// @param text how texts are looked up
/// @param ranges the ranges as written
/// @return the words, such as "4.0 or later"
std::string ranges_words(const TextHooks& text, const std::vector<std::string>& ranges) {
    std::string joined;
    for (const std::string& range : ranges) {
        const std::string words = range_words(range, text);
        if (words.empty())
            continue;
        if (!joined.empty()) {
            joined += shown(text, words::list_separator);
            joined.push_back(' ');
        }
        joined += words;
    }
    return joined;
}

/// Finds a registry by its id.
///
/// @param library the Library
/// @param id the registry's id
/// @return the registry, or null
const Registry* find_registry(const Library& library, std::string_view id) {
    for (const Registry& registry : library.inputs.registries)
        if (registry.id == id)
            return &registry;
    return nullptr;
}

/// The name a registry shows.
///
/// @param library the Library
/// @param id the registry's id
/// @return its name, or the id when the inputs do not name it
std::string registry_name_of(const Library& library, std::string_view id) {
    const Registry* registry = find_registry(library, id);
    if (registry != nullptr && !registry->name.empty())
        return registry->name;
    return std::string(id);
}

/// The first mutator entry of a key, in the Library's order.
///
/// @param library the Library
/// @param id the package key
/// @return the entry, or null
const Entry* first_entry(const Library& library, std::string_view id) {
    for (const Entry& entry : library.entries)
        if (entry.id.kind == Kind::mutator && entry.id.key == id)
            return &entry;
    return nullptr;
}

/// The facts the closure reads for an id: the entry being got for its own
/// key, otherwise the first entry of that key.
///
/// @param library the Library
/// @param entry the entry being got
/// @param id the key
/// @return the facts, or null
const MutatorFacts* facts_for(const Library& library, const Entry& entry, std::string_view id) {
    if (id == entry.id.key)
        return mutator_facts(entry);
    const Entry* found = first_entry(library, id);
    return found != nullptr ? mutator_facts(*found) : nullptr;
}

/// Tells whether an installed copy of `id` meets every range.
///
/// An empty range is any version. A range that does not parse is not met.
/// No installed copy is not met.
///
/// @param library the Library
/// @param id the package key
/// @param ranges the ranges as written
/// @return true when some installed copy meets all of them
bool installed_in_ranges(
    const Library& library, std::string_view id, const std::vector<std::string>& ranges
) {
    for (const Installed& item : library.inputs.installed) {
        if (item.kind != Kind::mutator || item.key != id)
            continue;
        bool meets = true;
        for (const std::string& range_text : ranges) {
            formats::oamut::Condition condition;
            condition.id = std::string(id);
            condition.range_text = range_text;
            if (!range_text.empty()) {
                const std::optional<formats::oamut::VersionRange> range =
                    formats::oamut::parse_version_range(range_text);
                if (!range) {
                    meets = false;
                    break;
                }
                condition.range = *range;
            }
            if (!formats::oamut::condition_met(condition, id, item.version)) {
                meets = false;
                break;
            }
        }
        if (meets)
            return true;
    }
    return false;
}

/// Tells whether a listing's version meets every range.
///
/// @param listing the listing
/// @param ranges the ranges as written
/// @return true when it does; an empty range list is any version
bool listing_meets(const Listing& listing, const std::vector<std::string>& ranges) {
    for (const std::string& range_text : ranges) {
        if (range_text.empty())
            continue;
        const std::optional<formats::oamut::VersionRange> range =
            formats::oamut::parse_version_range(range_text);
        if (!range)
            return false;
        const std::optional<formats::oamut::Version> version =
            formats::oamut::parse_version(listing.version);
        if (!version || !formats::oamut::version_in_range(*range, *version))
            return false;
    }
    return true;
}

/// Tells whether `candidate` is a newer offer than `chosen`.
///
/// A higher release wins, then a higher version, then a lower registry id.
///
/// @param candidate the listing being considered
/// @param chosen the listing kept so far
/// @return true when `candidate` should replace `chosen`
bool newer_listing(const Listing& candidate, const Listing& chosen) {
    if (candidate.release != chosen.release)
        return candidate.release > chosen.release;
    const std::optional<formats::oamut::Version> candidate_version =
        formats::oamut::parse_version(candidate.version);
    const std::optional<formats::oamut::Version> chosen_version =
        formats::oamut::parse_version(chosen.version);
    if (candidate_version && chosen_version && *candidate_version != *chosen_version)
        return *candidate_version > *chosen_version;
    if (candidate_version && !chosen_version)
        return true;
    if (!candidate_version && chosen_version)
        return false;
    return candidate.registry < chosen.registry;
}

/// The first tag id a listing or an installed copy names.
///
/// @param listing the listing; may be null
/// @param installed the installed copy; may be null
/// @return the tag, or nothing
std::optional<std::string> first_tag(const Listing* listing, const Installed* installed) {
    if (listing != nullptr && listing->mutator && !listing->mutator->tags.empty())
        return listing->mutator->tags.front();
    if (installed != nullptr && installed->mutator && !installed->mutator->tags.empty())
        return installed->mutator->tags.front();
    return std::nullopt;
}

/// The installed mutator of a key, the first one.
///
/// @param library the Library
/// @param id the key
/// @return the copy, or null
const Installed* installed_of(const Library& library, std::string_view id) {
    for (const Installed& item : library.inputs.installed)
        if (item.kind == Kind::mutator && item.key == id)
            return &item;
    return nullptr;
}

/// A name a player reads for a prerequisite.
///
/// @param known the name a fact recorded; empty when none did
/// @param listing the chosen listing; may be null
/// @param installed an installed copy; may be null
/// @param id the key
/// @return the name
std::string display_name(
    std::string_view known, const Listing* listing, const Installed* installed, std::string_view id
) {
    if (listing != nullptr && !listing->name.empty())
        return listing->name;
    if (!known.empty())
        return std::string(known);
    if (installed != nullptr && !installed->name.empty())
        return installed->name;
    return std::string(id);
}

/// Names missing hacks: up to named_missing_hacks of them, then how many more.
///
/// @param missing the hack ids
/// @param text how texts are looked up
/// @return the list
std::string hacks_text(const std::vector<std::string_view>& missing, const TextHooks& text) {
    std::string named;
    const std::size_t shown_count = std::min(missing.size(), named_missing_hacks);
    for (std::size_t index = 0; index < shown_count; ++index) {
        if (index != 0) {
            named += shown(text, words::list_separator);
            named.push_back(' ');
        }
        named += missing[index];
    }
    if (missing.size() <= named_missing_hacks)
        return named;
    return filled(
        text,
        words::hacks_and_more,
        {{"hacks", named}, {"count", std::to_string(missing.size() - named_missing_hacks)}}
    );
}

/// Why a listing cannot be got, for the reasons GET is off.
///
/// A mutator that does not work with the mod being played is not one of them.
///
/// @param library the Library
/// @param listing the listing that would be queued
/// @return the reason; empty when it can
std::string step_blocked(const Library& library, const Listing& listing) {
    const TextHooks& text = library.text;
    if (library.inputs.in_match)
        return std::string(shown(text, words::blocked_in_match));
    const Registry* registry = find_registry(library, listing.registry);
    const std::string registry_name =
        registry != nullptr && !registry->name.empty() ? registry->name : listing.registry;
    if (registry != nullptr && !registry->downloads_on) {
        if (!registry->downloads_off_reason.empty())
            return registry->downloads_off_reason;
        return filled(text, words::blocked_downloads_off, {{"registry", registry_name}});
    }
    if (const std::optional<formats::oamod::EngineRange> range =
            formats::oamod::parse_engine_range(listing.requires_engine)) {
        if (!formats::oamod::engine_range_met(*range, library.inputs.engine))
            return filled(text, words::blocked_engine, {{"range", engine_text(*range, text)}});
    }
    if (!listing.base.empty() && listing.base != supported_base)
        return filled(text, words::blocked_base, {{"base", listing.base}});
    std::vector<std::string_view> missing;
    for (const std::string& hack : listing.hack_ids) {
        const hacks::Entry* known = hacks::find_entry(hack);
        if (known == nullptr || !known->implemented)
            missing.push_back(hack);
    }
    if (!missing.empty())
        return filled(text, words::blocked_hacks, {{"hacks", hacks_text(missing, text)}});
    return {};
}

/// One step for a chosen listing.
///
/// @param library the Library
/// @param listing the listing
/// @param known a name a fact recorded
/// @return the step
GetStep step_from_listing(const Library& library, const Listing& listing, std::string_view known) {
    GetStep step;
    step.id = EntryId{listing.registry, Kind::mutator, listing.key};
    step.name = display_name(known, &listing, installed_of(library, listing.key), listing.key);
    step.version = listing.version;
    step.registry_name = registry_name_of(library, listing.registry);
    if (listing.badge)
        step.badge = &*listing.badge;
    step.tag = first_tag(&listing, installed_of(library, listing.key));
    return step;
}

/// The entry itself, as the last step.
///
/// @param library the Library
/// @param entry the entry
/// @return the step
GetStep step_of_entry(const Library& library, const Entry& entry) {
    GetStep step;
    step.id = entry.id;
    step.name = std::string(entry_name(entry));
    if (entry.listing != nullptr)
        step.version = entry.listing->version;
    else if (entry.installed != nullptr)
        step.version = entry.installed->version;
    step.registry_name = !entry.registry_name.empty()
                             ? entry.registry_name
                             : registry_name_of(library, entry.id.registry);
    if (entry.listing != nullptr && entry.listing->badge)
        step.badge = &*entry.listing->badge;
    else if (entry.badge != nullptr)
        step.badge = entry.badge;
    else if (entry.installed != nullptr && entry.installed->badge)
        step.badge = &*entry.installed->badge;
    if (const std::vector<std::string>* tags = mutator_tag_list(entry)) {
        if (!tags->empty())
            step.tag = tags->front();
    }
    return step;
}

/// Tells whether `id` can reach itself through the needs the closure read.
///
/// The walk is bounded by the graph, so a cycle ends.
///
/// @param graph the graph, first record of an id wins
/// @param id the id
/// @return true when a cycle holds it
bool on_cycle(std::span<const Need> graph, std::string_view id) {
    const std::vector<std::string>* start = nullptr;
    for (const Need& need : graph) {
        if (need.id == id) {
            start = &need.needs;
            break;
        }
    }
    if (start == nullptr)
        return false;
    std::vector<std::string> waiting = *start;
    std::set<std::string, std::less<>> seen;
    const std::size_t limit = (graph.size() + 1) * (graph.size() + 1);
    std::size_t steps = 0;
    while (!waiting.empty() && steps < limit) {
        const std::string current = waiting.back();
        waiting.pop_back();
        ++steps;
        if (current == id)
            return true;
        if (!seen.insert(current).second)
            continue;
        for (const Need& need : graph) {
            if (need.id != current)
                continue;
            for (const std::string& deeper : need.needs)
                waiting.push_back(deeper);
            break;
        }
    }
    return false;
}

/// The ids of the loop, in the closure's order. Every id the closure flagged
/// is named; none is dropped.
///
/// @param graph the graph
/// @param order the closure's order
/// @return the ids
std::vector<std::string>
cycle_ids(std::span<const Need> graph, const std::vector<std::string>& order) {
    std::vector<std::string> loop;
    for (const std::string& id : order)
        if (on_cycle(graph, id))
            loop.push_back(id);
    if (loop.empty())
        return order;
    return loop;
}

/// A question item's badge: the picture, else the tag's icon, else the letters.
///
/// @param step the package
/// @return the badge
oa::ui::kit::BadgeLook badge_of(const GetStep& step) {
    oa::ui::kit::BadgeLook look;
    look.face = oa::ui::kit::colour::hint;
    if (step.badge != nullptr) {
        oa::ui::frontend_renderer::RgbaPicture picture;
        picture.width = step.badge->width;
        picture.height = step.badge->height;
        picture.pixels = std::span<const uint8_t>(step.badge->rgba);
        if (oa::ui::frontend_renderer::picture_drawable(picture))
            look.picture = picture;
    }
    if (oa::ui::frontend_renderer::picture_drawable(look.picture))
        return look;
    if (step.tag) {
        if (const std::optional<oa::ui::kit::TagIcon> icon = oa::ui::kit::tag_icon_of(*step.tag)) {
            look.tag = icon;
            return look;
        }
    }
    look.letters = badge_letters(step.name);
    return look;
}

/// A prerequisite as the question names it: its name, then its range when it has one.
///
/// @param text how texts are looked up
/// @param name the name
/// @param ranges the ranges as written
/// @return the phrase
std::string name_and_range(
    const TextHooks& text, std::string_view name, const std::vector<std::string>& ranges
) {
    const std::string words = ranges_words(text, ranges);
    if (words.empty())
        return std::string(name);
    return std::string(name) + " " + words;
}

} // namespace

const Listing* choose_listing(
    const Library& library,
    std::string_view preferred,
    std::string_view id,
    const std::vector<std::string>& ranges
) {
    std::vector<const Listing*> meeting;
    for (const Listing& listing : library.inputs.listings) {
        if (listing.kind == Kind::mutator && listing.key == id && listing_meets(listing, ranges))
            meeting.push_back(&listing);
    }
    if (meeting.empty())
        return nullptr;
    std::vector<const Listing*> own;
    std::vector<const Listing*> built_in;
    for (const Listing* listing : meeting) {
        if (!preferred.empty() && listing->registry == preferred)
            own.push_back(listing);
        const Registry* registry = find_registry(library, listing->registry);
        if (registry != nullptr && registry->built_in)
            built_in.push_back(listing);
    }
    std::vector<const Listing*> pool;
    if (!own.empty())
        pool = std::move(own);
    else if (!built_in.empty())
        pool = std::move(built_in);
    else {
        std::string lowest = meeting.front()->registry;
        for (const Listing* listing : meeting)
            if (listing->registry < lowest)
                lowest = listing->registry;
        for (const Listing* listing : meeting)
            if (listing->registry == lowest)
                pool.push_back(listing);
    }
    const Listing* chosen = pool.front();
    for (const Listing* listing : pool)
        if (listing != chosen && newer_listing(*listing, *chosen))
            chosen = listing;
    return chosen;
}

std::vector<Need> needs_of(const Library& library) {
    std::vector<Need> graph;
    for (const Entry& entry : library.entries) {
        if (entry.id.kind != Kind::mutator)
            continue;
        Need record;
        record.id = entry.id.key;
        if (const MutatorFacts* facts = mutator_facts(entry)) {
            for (const Prerequisite& item : facts->needs) {
                if (!item.id.empty())
                    record.needs.push_back(item.id);
            }
        }
        graph.push_back(std::move(record));
    }
    return graph;
}

GetPlan get_plan(const Library& library, const Entry& entry) {
    GetPlan plan;
    if (entry.id.kind != Kind::mutator) {
        plan.steps.push_back(step_of_entry(library, entry));
        if (!entry.blocked.empty())
            plan.blocked = entry.blocked;
        return plan;
    }

    // This entry's facts are the first record of its key, so they win over
    // another registry's copy of the same package.
    std::vector<Need> graph;
    Need self;
    self.id = entry.id.key;
    if (const MutatorFacts* facts = mutator_facts(entry)) {
        for (const Prerequisite& item : facts->needs) {
            if (!item.id.empty())
                self.needs.push_back(item.id);
        }
    }
    graph.push_back(std::move(self));
    const std::vector<Need> rest = needs_of(library);
    graph.insert(graph.end(), rest.begin(), rest.end());

    const std::string root = entry.id.key;
    const std::string roots[]{root};
    const Closure closure = prerequisite_closure(graph, roots);
    if (closure.cyclic) {
        plan.blocked = filled(
            library.text,
            words::blocked_cycle,
            {{"ids", join_parts(library.text, cycle_ids(graph, closure.order), words::names_and)}}
        );
        return plan;
    }

    std::map<std::string, std::vector<std::string>, std::less<>> ranges;
    std::map<std::string, std::string, std::less<>> names;
    for (const std::string& id : closure.order) {
        const MutatorFacts* facts = facts_for(library, entry, id);
        if (facts == nullptr)
            continue;
        for (const Prerequisite& item : facts->needs) {
            if (item.id.empty())
                continue;
            ranges[item.id].push_back(item.range);
            if (!item.name.empty() && names[item.id].empty())
                names[item.id] = item.name;
        }
    }

    bool stopped = false;
    for (const std::string& id : closure.order) {
        if (id == root)
            continue;
        const std::vector<std::string>& asked = ranges[id];
        if (installed_in_ranges(library, id, asked))
            continue;
        const Listing* listing = choose_listing(library, entry.id.registry, id, asked);
        if (listing == nullptr) {
            const std::string name =
                display_name(names[id], nullptr, installed_of(library, id), id);
            const std::string asked_words = ranges_words(library.text, asked);
            // A range-free prerequisite would leave a blank in
            // "Needs {name} {range}, which no registry offers".
            if (asked_words.empty()) {
                plan.blocked =
                    filled(library.text, words::blocked_prerequisite_any, {{"name", name}});
            } else {
                plan.blocked = filled(
                    library.text,
                    words::blocked_prerequisite,
                    {{"name", name}, {"range", asked_words}}
                );
            }
            stopped = true;
            break;
        }
        plan.steps.push_back(step_from_listing(library, *listing, names[id]));
        if (plan.blocked.empty()) {
            const std::string reason = step_blocked(library, *listing);
            if (!reason.empty())
                plan.blocked = reason;
        }
    }
    if (!stopped) {
        plan.steps.push_back(step_of_entry(library, entry));
        if (plan.blocked.empty() && entry.listing != nullptr) {
            const std::string reason = step_blocked(library, *entry.listing);
            if (!reason.empty())
                plan.blocked = reason;
        } else if (plan.blocked.empty() && !entry.blocked.empty()) {
            plan.blocked = entry.blocked;
        }
    }
    return plan;
}

oa::ui::kit::Question get_with_question(const Library& library, const Entry& entry) {
    const TextHooks& text = library.text;
    const GetPlan plan = get_plan(library, entry);
    const std::string name(entry_name(entry));
    oa::ui::kit::Question question;
    question.title = filled(text, words::get_with_title, {{"name", name}});

    std::vector<Need> graph;
    Need self;
    self.id = entry.id.key;
    if (const MutatorFacts* facts = mutator_facts(entry)) {
        for (const Prerequisite& item : facts->needs)
            if (!item.id.empty())
                self.needs.push_back(item.id);
    }
    graph.push_back(std::move(self));
    const std::vector<Need> rest = needs_of(library);
    graph.insert(graph.end(), rest.begin(), rest.end());
    const std::string root = entry.id.key;
    const std::string roots[]{root};
    const Closure closure = prerequisite_closure(graph, roots);
    std::map<std::string, std::vector<std::string>, std::less<>> ranges;
    std::map<std::string, std::string, std::less<>> names;
    if (!closure.cyclic) {
        for (const std::string& id : closure.order) {
            const MutatorFacts* facts = facts_for(library, entry, id);
            if (facts == nullptr)
                continue;
            for (const Prerequisite& item : facts->needs) {
                if (item.id.empty())
                    continue;
                ranges[item.id].push_back(item.range);
                if (!item.name.empty() && names[item.id].empty())
                    names[item.id] = item.name;
            }
        }
    }

    std::vector<std::string> phrases;
    std::string short_name;
    for (const GetStep& step : plan.steps) {
        if (step.id.key == root && step.id.registry == entry.id.registry)
            continue;
        const std::string shown_name = names[step.id.key].empty() ? step.name : names[step.id.key];
        if (short_name.empty())
            short_name = shown_name;
        phrases.push_back(name_and_range(text, shown_name, ranges[step.id.key]));
    }
    if (phrases.size() <= 1) {
        const std::string prerequisite = phrases.empty() ? short_name : phrases.front();
        question.paragraphs.push_back(
            {filled(
                 text,
                 words::get_with_body_one,
                 {{"name", name}, {"prerequisite", prerequisite}, {"short", short_name}}
             ),
             false}
        );
    } else {
        question.paragraphs.push_back(
            {filled(
                 text,
                 words::get_with_body_many,
                 {{"name", name}, {"prerequisites", join_parts(text, phrases, words::names_and)}}
             ),
             false}
        );
    }

    for (const GetStep& step : plan.steps) {
        oa::ui::kit::QuestionItem item;
        item.badge = badge_of(step);
        item.text =
            filled(text, words::queue_package, {{"name", step.name}, {"version", step.version}});
        item.aside = " " + filled(text, words::get_with_from, {{"registry", step.registry_name}});
        question.items.push_back(std::move(item));
    }

    question.buttons.push_back(
        {std::string(shown(text, words::action_cancel)), false, std::string(get_with_cancel_id)}
    );
    const bool both = plan.steps.size() <= 2;
    question.buttons.push_back(
        {std::string(shown(text, both ? words::action_get_both : words::action_get_all)),
         true,
         std::string(both ? get_with_both_id : get_with_all_id)}
    );
    question.cancel_button = 0;
    question.primary_button = 1;
    question.marked = 0;
    return question;
}

} // namespace oa::ui::library
