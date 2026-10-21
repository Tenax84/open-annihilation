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
/// The first record of an id is the one the closure reads.
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
        for (const library::Prerequisite& prerequisite : mutator.needs)
            need.needs.push_back(prerequisite.id);
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

/// Returns an On mutator `mutator` cannot run beside, the highest one.
///
/// @param picker the picker
/// @param mutator the mutator
/// @return that mutator, or null when none of the On list conflicts
inline const Mutator* on_conflict(const Picker& picker, const Mutator& mutator) {
    for (const Chosen& chosen : picker.on) {
        if (chosen.id == mutator.id)
            continue;
        const Mutator* other = find_mutator(picker, chosen.id);
        if (other != nullptr && conflicts(mutator, *other))
            return other;
    }
    return nullptr;
}

/// Joins names the way a sentence lists them.
///
/// @param hooks how texts are looked up
/// @param names the names, in order
/// @return the list; empty when `names` is empty
inline std::string
join_names(const library::TextHooks& hooks, const std::vector<std::string>& names) {
    if (names.empty())
        return {};
    if (names.size() == 1)
        return names.front();
    std::string joined = names.front();
    for (std::size_t index = 1; index + 1 < names.size(); ++index) {
        joined = library::filled(
            hooks, library::words::names_comma, {{"left", joined}, {"right", names[index]}}
        );
    }
    return library::filled(
        hooks, library::words::names_and, {{"left", joined}, {"right", names.back()}}
    );
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

/// Tells whether `id` is one of `ids`.
///
/// @param ids the ids
/// @param id the id
/// @return true when the list holds it
inline bool listed(const std::vector<std::string>& ids, std::string_view id) {
    for (const std::string& item : ids)
        if (item == id)
            return true;
    return false;
}

} // namespace detail
} // namespace oa::ui::mutator_picker
