// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Lookups the picker's two sources share. Not a header the host includes.
#pragma once

#include "oa/ui/mutator_picker/model.hpp"
#include "oa/ui/mutator_picker/text.hpp"

#include "oa/ui/library/prerequisites.hpp"
#include "oa/ui/library/text.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::ui::mutator_picker {
namespace detail {

/// Returns the mutator with `id`, or null.
///
/// @param picker the picker
/// @param id the mutator's id
/// @return the mutator, or null when the inputs do not hold it
inline const Mutator* find_mutator(const Picker& picker, std::string_view id) {
    for (const Mutator& mutator : picker.inputs.mutators)
        if (mutator.id == id)
            return &mutator;
    return nullptr;
}

/// Returns the name a player sees. The id when the mutator names none.
///
/// @param mutator the mutator
/// @return the name
inline std::string display_name(const Mutator& mutator) {
    if (!mutator.name.empty())
        return mutator.name;
    return mutator.id;
}

/// Returns the name of `id`, or the id when the inputs do not hold it.
///
/// @param picker the picker
/// @param id the mutator's id
/// @return the name
inline std::string display_name_of(const Picker& picker, std::string_view id) {
    if (const Mutator* mutator = find_mutator(picker, id))
        return display_name(*mutator);
    return std::string(id);
}

/// Returns where `id` sits in the On list.
///
/// @param picker the picker
/// @param id the mutator's id
/// @return the index, or nothing when it is off
inline std::optional<std::size_t> on_index(const Picker& picker, std::string_view id) {
    for (std::size_t index = 0; index < picker.on.size(); ++index)
        if (picker.on[index].id == id)
            return index;
    return std::nullopt;
}

/// A mutator switched on with its default options and clash mode.
///
/// A number's default is the canonical spelling the On list keeps, so a
/// fallback of `4.0` is kept as `4`.
///
/// @param picker the picker
/// @param mutator the mutator
/// @return its On record
[[nodiscard]] Chosen with_defaults(const Picker& picker, const Mutator& mutator);

/// Returns the On ids, top to bottom.
///
/// @param picker the picker
/// @return the ids
inline std::vector<std::string> on_ids(const Picker& picker) {
    std::vector<std::string> ids;
    ids.reserve(picker.on.size());
    for (const Chosen& chosen : picker.on)
        ids.push_back(chosen.id);
    return ids;
}

/// Returns the prerequisite graph of every mutator in the inputs.
///
/// The first record of an id is the one the closure reads. A need with no
/// id is left out.
///
/// @param picker the picker
/// @return the graph
inline std::vector<library::Need> needs_graph(const Picker& picker) {
    std::vector<library::Need> graph;
    graph.reserve(picker.inputs.mutators.size());
    for (const Mutator& mutator : picker.inputs.mutators) {
        library::Need need;
        need.id = mutator.id;
        need.needs.reserve(mutator.needs.size());
        for (const library::Prerequisite& prerequisite : mutator.needs) {
            if (!prerequisite.id.empty())
                need.needs.push_back(prerequisite.id);
        }
        graph.push_back(std::move(need));
    }
    return graph;
}

/// Folds ASCII letters to lower case. Any other byte is kept.
///
/// @param text the text
/// @return the folded text
inline std::string folded(std::string_view text) {
    std::string lower(text);
    for (char& byte : lower) {
        if (byte >= 'A' && byte <= 'Z')
            byte = static_cast<char>(byte - 'A' + 'a');
    }
    return lower;
}

/// Tells whether either mutator names the other in its conflicts.
///
/// @param left one mutator
/// @param right the other
/// @return true when they cannot be on together
inline bool conflicts(const Mutator& left, const Mutator& right) {
    if (left.id == right.id)
        return false;
    for (const std::string& id : left.conflicts)
        if (id == right.id)
            return true;
    for (const std::string& id : right.conflicts)
        if (id == left.id)
            return true;
    return false;
}

/// Joins names the way a sentence lists them.
///
/// @param hooks how texts are looked up
/// @param names the names, in order
/// @return the list; empty when `names` is empty
inline std::string
join_names(const library::TextHooks& hooks, const std::vector<std::string>& names) {
    return library::join_names(hooks, names);
}

/// Joins parts with a middle dot, through the Library's join.
///
/// @param hooks how texts are looked up
/// @param parts the parts, in order
/// @return the line; empty when `parts` is empty
inline std::string
join_dot(const library::TextHooks& hooks, const std::vector<std::string>& parts) {
    if (parts.empty())
        return {};
    std::string joined = parts.front();
    for (std::size_t index = 1; index < parts.size(); ++index) {
        joined = library::filled(
            hooks, library::words::fact_join, {{"left", joined}, {"right", parts[index]}}
        );
    }
    return joined;
}

/// Returns the value `chosen` holds for `option`, or empty when it holds none.
///
/// @param chosen the On mutator
/// @param option the option's id
/// @return the value as written
inline std::string chosen_value(const Chosen& chosen, std::string_view option) {
    for (const auto& [id, value] : chosen.options)
        if (id == option)
            return value;
    return {};
}

/// Returns the caption a row shows for an option's value.
///
/// A number's caption is the value as written. A choice or a toggle uses
/// the caption at the value's index when there is one, and the value otherwise.
///
/// @param spec the option
/// @param value the value as written
/// @return the caption
inline std::string option_caption(const OptionSpec& spec, std::string_view value) {
    if (spec.type == OptionSpec::Type::number)
        return std::string(value);
    for (std::size_t index = 0; index < spec.values.size(); ++index) {
        if (spec.values[index] != value)
            continue;
        if (index < spec.captions.size() && !spec.captions[index].empty())
            return spec.captions[index];
        return std::string(value);
    }
    return std::string(value);
}

/// Drops `came_with` records that name `id`.
///
/// @param[in,out] picker the picker
/// @param id the mutator's id
inline void drop_came_with(Picker& picker, std::string_view id) {
    std::vector<CameWith> kept;
    for (const CameWith& record : picker.came_with)
        if (record.prerequisite != id && record.dependant != id)
            kept.push_back(record);
    picker.came_with = std::move(kept);
}

/// The clash mode a mutator uses when none has been chosen yet.
///
/// Empty when it brings no units or weapons. Otherwise the manifest's mode,
/// or the inputs' default when the manifest names none. The same rule
/// `open` uses once a chosen mode is set aside.
///
/// @param mutator the mutator
/// @param inputs the host's inputs
/// @return the mode, or empty when the mutator brings nothing
[[nodiscard]] inline std::optional<Clash>
default_clash(const Mutator& mutator, const Inputs& inputs) {
    if (!mutator.brings)
        return std::nullopt;
    if (mutator.clashes)
        return mutator.clashes;
    return inputs.clash_default;
}

/// Tells whether `id` is one of `ids`.
///
/// @param ids the ids
/// @param id the id
/// @return true when the list holds it
inline bool listed(std::span<const std::string> ids, std::string_view id) {
    for (const std::string& item : ids)
        if (item == id)
            return true;
    return false;
}

/// Returns why `id` cannot be on beside the mutators `on` names, in the
/// words its row shows and `switch_on` refuses with.
///
/// The checks run over the mutator's prerequisite closure, in this order:
/// the mutator is unavailable; the closure is a loop (the ids in it); a
/// prerequisite is not among the inputs, is unavailable, or has a version
/// outside the range its dependant asks for; the mutator, or a prerequisite
/// it would bring (one `on` does not name), conflicts with a mutator `on`
/// names, or with another of those. A conflict between two mutators `on`
/// already names is not this mutator's. The words are `line_not_compatible`
/// with the reason, or `line_conflicts` naming the other mutator, the On
/// one when there is one.
///
/// @param picker the picker
/// @param graph the prerequisite graph of its inputs
/// @param on the mutators it would be on beside, top to bottom
/// @param id the mutator
/// @return the line; empty when it can be on, or the inputs do not hold it
[[nodiscard]] std::string blocked_line(
    const Picker& picker,
    std::span<const library::Need> graph,
    std::span<const std::string> on,
    std::string_view id
);

/// Returns the On mutators that need `id` now, top to bottom, `id` left out.
///
/// @param picker the picker
/// @param id the prerequisite
/// @return its On dependants
inline std::vector<std::string> on_dependants(const Picker& picker, std::string_view id) {
    const std::vector<library::Need> graph = needs_graph(picker);
    const std::vector<std::string> ids = on_ids(picker);
    std::vector<std::string> dependants;
    for (std::string& item : library::dependants_of(graph, ids, id)) {
        if (item != id && !listed(dependants, item))
            dependants.push_back(std::move(item));
    }
    return dependants;
}

} // namespace detail
} // namespace oa::ui::mutator_picker
