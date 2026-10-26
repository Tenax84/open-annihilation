// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The words a mutator picker's rows, footer, hint and turn-off question
// show. Every sentence is looked up through the Library's TextHooks.
#include "oa/ui/mutator_picker/model.hpp"

#include "internal.hpp"

#include "oa/ui/kit/tag_icon.hpp"
#include "oa/ui/library/prerequisites.hpp"
#include "oa/ui/library/text.hpp"
#include "oa/ui/mutator_picker/text.hpp"

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::ui::mutator_picker {
namespace {

/// Tells whether a filter keeps a mutator, On or Off.
///
/// @param mutator the mutator
/// @param filter the filter
/// @return true when it counts
bool filter_keeps(const Mutator& mutator, Filter filter) {
    if (filter == Filter::installed)
        return mutator.installed;
    if (filter == Filter::downloads)
        return !mutator.installed;
    return true;
}

/// Tells whether the mutator carries the tag, matched exactly.
///
/// @param mutator the mutator
/// @param tag the tag id; empty matches every mutator
/// @return true when the row keeps it
bool tag_matches(const Mutator& mutator, std::string_view tag) {
    if (tag.empty())
        return true;
    return detail::listed(mutator.tags, tag);
}

/// Tells whether every word of the search occurs in the mutator's texts.
///
/// Words split on ASCII space. An empty search matches everything.
///
/// @param picker the picker
/// @param mutator the mutator
/// @return true when the row keeps it
bool query_matches(const Picker& picker, const Mutator& mutator) {
    const std::string folded_query = detail::folded(picker.query);
    std::vector<std::string> terms;
    std::string term;
    for (const char byte : folded_query) {
        if (byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r') {
            if (!term.empty())
                terms.push_back(std::move(term));
            term.clear();
            continue;
        }
        term.push_back(byte);
    }
    if (!term.empty())
        terms.push_back(std::move(term));
    if (terms.empty())
        return true;

    std::vector<std::string> fields;
    fields.push_back(detail::folded(detail::display_name(mutator)));
    fields.push_back(detail::folded(mutator.id));
    fields.push_back(detail::folded(mutator.summary));
    fields.push_back(detail::folded(mutator.author));
    fields.push_back(detail::folded(mutator.description));
    for (const std::string& tag : mutator.tags) {
        fields.push_back(detail::folded(tag));
        const std::string label(library::tag_label(tag, picker.inputs.text));
        fields.push_back(detail::folded(label));
    }
    for (const std::string& needle : terms) {
        bool found = false;
        for (const std::string& field : fields) {
            if (field.find(needle) != std::string::npos) {
                found = true;
                break;
            }
        }
        if (!found)
            return false;
    }
    return true;
}

/// The option lines of an On mutator, and its clash choice when it brings units.
///
/// @param picker the picker
/// @param mutator the mutator
/// @param chosen the On record
/// @return the line; empty when there is nothing to show
std::string option_lines(const Picker& picker, const Mutator& mutator, const Chosen& chosen) {
    const library::TextHooks& text = picker.inputs.text;
    std::vector<std::string> parts;
    for (const OptionSpec& spec : mutator.options) {
        const std::string value = detail::chosen_value(chosen, spec.id);
        const std::string caption = detail::option_caption(spec, value);
        parts.push_back(
            library::filled(text, words::option_line, {{"label", spec.label}, {"value", caption}})
        );
    }
    if (mutator.brings && chosen.clash) {
        const std::string_view mode =
            *chosen.clash == Clash::add ? words::clash_add : words::clash_replace;
        const std::string label(library::shown(text, words::clash_label));
        const std::string value(library::shown(text, mode));
        parts.push_back(
            library::filled(text, words::option_line, {{"label", label}, {"value", value}})
        );
    }
    return detail::join_dot(text, parts);
}

/// Fills a row's line, tone and marks. The first case that holds wins: a
/// row locked beside the On list comes first.
///
/// @param picker the picker
/// @param graph the prerequisite graph
/// @param on the On ids, top to bottom
/// @param mutator the mutator; null when the inputs no longer hold it
/// @param chosen the On record, or null in the Off list
/// @param[in,out] row the row
void fill_line(
    const Picker& picker,
    const std::vector<library::Need>& graph,
    const std::vector<std::string>& on,
    const Mutator* mutator,
    const Chosen* chosen,
    Row& row
) {
    const library::TextHooks& text = picker.inputs.text;
    if (mutator == nullptr)
        return;
    std::string blocked = detail::blocked_line(picker, graph, on, mutator->id);
    if (!blocked.empty()) {
        row.line = std::move(blocked);
        row.tone = Tone::bad;
        row.locked = true;
        return;
    }
    if (mutator->verdict.state == library::Verdict::State::incompatible) {
        row.line = library::filled(
            text, words::line_not_compatible, {{"reason", mutator->verdict.reason}}
        );
        row.tone = Tone::bad;
        return;
    }
    if (mutator->verdict.state == library::Verdict::State::failed) {
        row.line = library::filled(text, words::line_failed, {{"reason", mutator->verdict.reason}});
        row.tone = Tone::bad;
        return;
    }
    if (row.on) {
        const std::vector<std::string> dependants = library::dependants_of(graph, on, row.id);
        for (const std::string& dependant : dependants) {
            if (dependant == row.id)
                continue;
            row.line = library::filled(
                text, words::line_needed_by, {{"name", detail::display_name_of(picker, dependant)}}
            );
            row.tone = Tone::warn;
            return;
        }
    }
    if (mutator->verdict.state == library::Verdict::State::no_effect) {
        row.line = std::string(library::shown(text, words::line_no_effect));
        row.tone = Tone::warn;
        return;
    }
    if (mutator->queued) {
        row.line = library::queue_status_text(*mutator->queued, text);
        row.tone = Tone::hint;
        return;
    }
    // An On mutator from a last set, or one brought with it, needs a download
    // too. Only an Off row says so in the line; the mark is what USE THESE reads.
    if (!mutator->installed) {
        row.to_download = true;
        if (!row.on) {
            row.line = std::string(library::shown(text, words::line_not_installed));
            row.tone = Tone::hint;
            return;
        }
    }
    if (row.on && chosen != nullptr) {
        const std::string options = option_lines(picker, *mutator, *chosen);
        if (!options.empty()) {
            row.line = options;
            row.tone = Tone::hint;
            return;
        }
    }
    row.line = mutator->summary;
    row.tone = Tone::hint;
}

} // namespace

std::vector<Row> rows(const Picker& picker) {
    const std::vector<library::Need> graph = detail::needs_graph(picker);
    const std::vector<std::string> on = detail::on_ids(picker);
    std::vector<Row> built;
    std::size_t number = 1;
    for (const Chosen& chosen : picker.on) {
        Row row;
        row.id = chosen.id;
        row.on = true;
        row.number = number;
        ++number;
        fill_line(picker, graph, on, detail::find_mutator(picker, chosen.id), &chosen, row);
        built.push_back(std::move(row));
    }

    std::vector<const Mutator*> off;
    for (const Mutator& mutator : picker.inputs.mutators) {
        if (detail::on_index(picker, mutator.id))
            continue;
        if (!filter_keeps(mutator, picker.filter) || !tag_matches(mutator, picker.tag) ||
            !query_matches(picker, mutator))
            continue;
        off.push_back(&mutator);
    }
    std::sort(off.begin(), off.end(), [](const Mutator* left, const Mutator* right) {
        const std::string left_name = detail::folded(detail::display_name(*left));
        const std::string right_name = detail::folded(detail::display_name(*right));
        if (left_name != right_name)
            return left_name < right_name;
        return left->id < right->id;
    });
    for (const Mutator* mutator : off) {
        Row row;
        row.id = mutator->id;
        row.on = false;
        row.number = 0;
        fill_line(picker, graph, on, mutator, nullptr, row);
        built.push_back(std::move(row));
    }
    return built;
}

std::size_t filter_count(const Picker& picker, Filter filter) {
    std::size_t count = 0;
    for (const Mutator& mutator : picker.inputs.mutators) {
        if (filter_keeps(mutator, filter))
            ++count;
    }
    return count;
}

std::size_t tag_count(const Picker& picker, std::string_view tag) {
    std::size_t count = 0;
    for (const Mutator& mutator : picker.inputs.mutators) {
        if (!filter_keeps(mutator, picker.filter))
            continue;
        if (detail::listed(mutator.tags, tag))
            ++count;
    }
    return count;
}

std::string default_options_text(const Picker& picker, std::string_view id) {
    const Mutator* mutator = detail::find_mutator(picker, id);
    if (mutator == nullptr)
        return {};
    return option_lines(picker, *mutator, detail::with_defaults(picker, *mutator));
}

std::vector<std::string> tags_shown(const Picker& picker) {
    std::vector<std::string> have;
    for (const Mutator& mutator : picker.inputs.mutators) {
        if (!filter_keeps(mutator, picker.filter))
            continue;
        for (const std::string& tag : mutator.tags) {
            if (!detail::listed(have, tag))
                have.push_back(tag);
        }
    }
    std::vector<std::string> ordered;
    for (const std::string_view id : kit::tag_ids) {
        if (detail::listed(have, id))
            ordered.emplace_back(id);
    }
    std::vector<std::string> rest;
    for (const std::string& tag : have) {
        bool known = false;
        for (const std::string_view id : kit::tag_ids) {
            if (tag == id) {
                known = true;
                break;
            }
        }
        if (!known)
            rest.push_back(tag);
    }
    std::sort(rest.begin(), rest.end());
    ordered.insert(ordered.end(), rest.begin(), rest.end());
    return ordered;
}

Footer footer(const Picker& picker) {
    Footer foot;
    const library::TextHooks& text = picker.inputs.text;
    if (picker.on.empty()) {
        foot.summary = std::string(library::shown(text, words::summary_none));
        return foot;
    }
    std::string summary =
        library::filled(text, words::summary_on, {{"count", std::to_string(picker.on.size())}});
    for (const CameWith& record : picker.came_with) {
        if (!detail::on_index(picker, record.prerequisite) ||
            !detail::on_index(picker, record.dependant))
            continue;
        const std::string brought = library::filled(
            text,
            words::summary_came_with,
            {{"name", detail::display_name_of(picker, record.prerequisite)},
             {"dependant", detail::display_name_of(picker, record.dependant)}}
        );
        summary = library::filled(
            text, library::words::fact_join, {{"left", summary}, {"right", brought}}
        );
        break;
    }
    foot.summary = std::move(summary);

    bool any_checking = false;
    bool any_unchecked = false;
    const Mutator* bad = nullptr;
    bool bad_failed = false;
    const Mutator* plain = nullptr;
    for (const Chosen& chosen : picker.on) {
        const Mutator* mutator = detail::find_mutator(picker, chosen.id);
        if (mutator == nullptr)
            continue;
        const library::Verdict::State state = mutator->verdict.state;
        if (state == library::Verdict::State::checking)
            any_checking = true;
        else if (state == library::Verdict::State::incompatible && bad == nullptr)
            bad = mutator;
        else if (state == library::Verdict::State::failed && bad == nullptr) {
            bad = mutator;
            bad_failed = true;
        } else if (state == library::Verdict::State::unchecked)
            any_unchecked = true;
        else if (state == library::Verdict::State::no_effect && plain == nullptr)
            plain = mutator;
    }
    const std::string mod = picker.inputs.mod_name;
    // With no mod played, each chip is the base game's own sentence.
    const bool base_game = mod.empty() || mod == library::shown(text, words::no_mod);
    const auto chip =
        [&](std::string_view with_mod, std::string_view with_base, std::string_view name) {
            if (base_game)
                return library::filled(text, with_base, {{"name", name}});
            return library::filled(text, with_mod, {{"mod", mod}, {"name", name}});
        };
    if (any_checking) {
        foot.chip = chip(words::verdict_checking, words::verdict_checking_base, {});
        foot.chip_mark = library::Mark::none;
    } else if (bad != nullptr) {
        const std::string name = detail::display_name(*bad);
        if (bad_failed)
            foot.chip = chip(words::verdict_failed, words::verdict_failed_base, name);
        else
            foot.chip =
                chip(words::verdict_not_compatible, words::verdict_not_compatible_base, name);
        foot.chip_mark = library::Mark::bad;
    } else if (any_unchecked) {
        foot.chip_mark = library::Mark::none;
    } else if (plain != nullptr) {
        foot.chip = chip(
            words::verdict_no_effect, words::verdict_no_effect_base, detail::display_name(*plain)
        );
        foot.chip_mark = library::Mark::warn;
    } else {
        foot.chip = chip(words::verdict_checked, words::verdict_checked_base, {});
        foot.chip_mark = library::Mark::good;
    }
    return foot;
}

std::string hint(const Picker& picker) {
    const library::TextHooks& text = picker.inputs.text;
    if (picker.inputs.read_only)
        return std::string(library::shown(text, words::hint_view));
    std::vector<std::string> parts;
    if (picker.from_last_set) {
        std::string_view pattern = words::hint_last_skirmish;
        if (picker.inputs.setup == Setup::multiplayer)
            pattern = words::hint_last_multiplayer;
        else if (picker.inputs.setup == Setup::campaign)
            pattern = words::hint_last_campaign;
        parts.emplace_back(library::shown(text, pattern));
    }
    if (picker.inputs.setup == Setup::campaign)
        parts.emplace_back(library::shown(text, words::hint_campaign));
    if (!picker.left_out.empty()) {
        parts.push_back(
            library::filled(
                text, words::hint_left_out, {{"names", detail::join_names(text, picker.left_out)}}
            )
        );
    }
    std::string sentence;
    for (std::size_t index = 0; index < parts.size(); ++index) {
        if (index != 0)
            sentence.push_back(' ');
        sentence += parts[index];
    }
    return sentence;
}

std::string item_text(const Picker& picker, const Chosen& chosen) {
    const Mutator* mutator = detail::find_mutator(picker, chosen.id);
    std::string text = mutator != nullptr ? detail::display_name(*mutator) : chosen.id;
    if (mutator == nullptr)
        return text;
    for (const OptionSpec& spec : mutator->options) {
        if (spec.type == OptionSpec::Type::toggle)
            continue;
        const std::string value = detail::chosen_value(chosen, spec.id);
        if (value.empty())
            continue;
        text.push_back(' ');
        text += detail::option_caption(spec, value);
    }
    return text;
}

std::string list_text(const Picker& picker, std::span<const Chosen> chosen) {
    std::vector<std::string> parts;
    parts.reserve(chosen.size());
    for (const Chosen& item : chosen)
        parts.push_back(item_text(picker, item));
    return detail::join_dot(picker.inputs.text, parts);
}

kit::Question turn_off_question(const Picker& picker) {
    kit::Question question;
    if (!picker.asking)
        return question;
    const library::TextHooks& text = picker.inputs.text;
    const std::string name = detail::display_name_of(picker, picker.asking->id);
    question.title = library::filled(text, words::turn_off_title, {{"name", name}});
    // The dependants On now, as confirm_turn_off finds them; those recorded
    // when it asked while the prerequisite is no longer on.
    std::vector<std::string> ids = detail::on_dependants(picker, picker.asking->id);
    if (ids.empty())
        ids = picker.asking->dependants;
    std::vector<std::string> dependants;
    dependants.reserve(ids.size());
    for (const std::string& id : ids)
        dependants.push_back(detail::display_name_of(picker, id));
    const std::string names = detail::join_names(text, dependants);
    const bool one = dependants.size() <= 1;
    question.paragraphs.push_back(
        {library::filled(
             text,
             one ? words::turn_off_one : words::turn_off_many,
             {{"names", names}, {"name", name}}
         ),
         false}
    );
    if (one) {
        const std::string dependant = dependants.empty() ? std::string{} : dependants.front();
        question.paragraphs.push_back(
            {library::filled(
                 text, words::turn_off_kept_one, {{"dependant", dependant}, {"name", name}}
             ),
             false}
        );
    } else {
        question.paragraphs.push_back(
            {library::filled(text, words::turn_off_kept_many, {{"name", name}}), false}
        );
    }
    question.buttons.push_back(
        {std::string(library::shown(text, words::cancel)), false, std::string(cancel_button_id)}
    );
    question.buttons.push_back(
        {std::string(library::shown(text, words::turn_off)), true, std::string(turn_off_button_id)}
    );
    question.marked = 0;
    question.cancel_button = 0;
    question.primary_button = 1;
    return question;
}

} // namespace oa::ui::mutator_picker
