// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The four mutator questions. Pure: the words are looked up, the places
// filled, and the kit question returned. Nothing here is shown.
#include "oa/ui/mutator_picker/prompts.hpp"

#include "oa/ui/library/text.hpp"
#include "oa/ui/mutator_picker/text.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace oa::ui::mutator_picker {
namespace {

namespace kit = oa::ui::kit;
namespace library = oa::ui::library;

/// Joins failure segments with spaces, dropping empty ones.
///
/// @param parts the segments, in order
/// @return the amber line
std::string join_parts(const std::vector<std::string>& parts) {
    std::string joined;
    for (const std::string& part : parts) {
        if (part.empty())
            continue;
        if (!joined.empty())
            joined += ' ';
        joined += part;
    }
    return joined;
}

/// The names of the players, each once, in the order they were met.
///
/// @param differs the differences
/// @return the names
std::vector<std::string> players_of(std::span<const Differs> differs) {
    std::vector<std::string> players;
    for (const Differs& one : differs) {
        if (one.player.empty())
            continue;
        bool seen = false;
        for (const std::string& player : players) {
            if (player == one.player)
                seen = true;
        }
        if (!seen)
            players.push_back(one.player);
    }
    return players;
}

/// One button.
///
/// @param caption the words
/// @param accent true for the primary look
/// @param id the name to automation
/// @return the button
kit::QuestionButton button_of(std::string caption, bool accent, std::string_view id) {
    kit::QuestionButton made;
    made.caption = std::move(caption);
    made.accent = accent;
    made.id = std::string(id);
    return made;
}

} // namespace

kit::Question
start_without_question(std::span<const Failed> failures, const library::TextHooks& hooks) {
    const bool one = failures.size() == 1;
    std::vector<std::string> names;
    std::vector<std::string> parts;
    names.reserve(failures.size());
    parts.reserve(failures.size());
    for (const Failed& failed : failures) {
        if (!failed.name.empty())
            names.push_back(failed.name);
        if (failed.name.empty())
            parts.push_back(failed.reason);
        else
            parts.push_back(
                library::filled(
                    hooks, words::failure_line, {{"name", failed.name}, {"reason", failed.reason}}
                )
            );
    }
    const std::string listed = library::join_names(hooks, names);
    kit::Question question;
    if (one) {
        const std::string name = failures.front().name;
        question.title = library::filled(hooks, words::start_without_one, {{"name", name}});
        question.paragraphs.push_back(
            {library::filled(hooks, words::stopped_one, {{"name", name}}), false}
        );
    } else {
        question.title = std::string(library::shown(hooks, words::start_without_many));
        question.paragraphs.push_back(
            {library::filled(hooks, words::stopped_many, {{"names", listed}}), false}
        );
    }
    question.failure = join_parts(parts);
    question.buttons.push_back(
        button_of(std::string(library::shown(hooks, words::back_button)), false, start_back_id)
    );
    question.buttons.push_back(button_of(
        std::string(
            library::shown(hooks, one ? words::start_without_it : words::start_without_them)
        ),
        true,
        start_without_id
    ));
    question.cancel_button = 0;
    question.primary_button = 1;
    question.marked = 1;
    return question;
}

kit::Question
forget_set_question(std::span<const std::string> names, const library::TextHooks& hooks) {
    std::vector<std::string> listed(names.begin(), names.end());
    kit::Question question;
    question.title = std::string(library::shown(hooks, words::forget_title));
    question.paragraphs.push_back(
        {library::filled(
             hooks, words::forget_body, {{"names", library::join_names(hooks, listed)}}
         ),
         false}
    );
    question.paragraphs.push_back({std::string(library::shown(hooks, words::forget_hint)), false});
    question.buttons.push_back(
        button_of(std::string(library::shown(hooks, words::keep_it)), false, keep_it_id)
    );
    question.buttons.push_back(
        button_of(std::string(library::shown(hooks, words::forget_it)), true, forget_it_id)
    );
    question.cancel_button = 0;
    question.primary_button = 1;
    question.marked = 1;
    return question;
}

kit::Question get_mutators_question(const GetProgress& progress, const library::TextHooks& hooks) {
    int32_t filled_amount = progress.progress;
    if (filled_amount < 0)
        filled_amount = 0;
    if (filled_amount > kit::question_progress_whole)
        filled_amount = kit::question_progress_whole;
    kit::Question question;
    question.title = std::string(library::shown(hooks, words::get_mutators_title));
    question.paragraphs.push_back(
        {library::filled(
             hooks,
             words::downloading_of,
             {{"name", progress.name},
              {"index", std::to_string(progress.index)},
              {"total", std::to_string(progress.total)}}
         ),
         false}
    );
    question.progress = filled_amount;
    question.buttons.push_back(
        button_of(std::string(library::shown(hooks, words::cancel)), false, get_cancel_id)
    );
    if (!progress.problem.empty()) {
        question.failure =
            library::filled(hooks, words::problem_retry, {{"problem", progress.problem}});
        question.buttons.push_back(button_of(
            std::string(library::shown(hooks, library::words::action_retry)), true, get_retry_id
        ));
        question.cancel_button = 0;
        question.primary_button = 1;
        question.marked = 1;
    } else {
        question.cancel_button = 0;
        question.primary_button = 0;
        question.marked = 0;
    }
    return question;
}

kit::Question
did_not_start_question(std::span<const Differs> differs, const library::TextHooks& hooks) {
    const std::vector<std::string> players = players_of(differs);
    const std::string player = library::join_names(hooks, players);
    std::vector<std::string> parts;
    std::vector<std::string> fetches;
    for (const Differs& one : differs) {
        if (!one.name.empty()) {
            if (one.there.empty())
                parts.push_back(
                    library::filled(
                        hooks, words::missing_there, {{"name", one.name}, {"player", one.player}}
                    )
                );
            else
                parts.push_back(
                    library::filled(
                        hooks,
                        words::release_here_there,
                        {{"name", one.name},
                         {"here", one.here},
                         {"there", one.there},
                         {"player", one.player}}
                    )
                );
        }
        if (!one.fetches.empty())
            fetches.push_back(
                library::filled(
                    hooks, words::get_fetches, {{"player", one.player}, {"release", one.fetches}}
                )
            );
    }
    kit::Question question;
    question.title = std::string(library::shown(hooks, words::did_not_start_title));
    question.paragraphs.push_back(
        {library::filled(hooks, words::different_result, {{"player", player}}), false}
    );
    for (const std::string& line : fetches)
        question.paragraphs.push_back({line, false});
    question.failure = join_parts(parts);
    question.buttons.push_back(button_of(
        std::string(library::shown(hooks, library::words::action_ok)), true, did_not_start_id
    ));
    question.cancel_button = 0;
    question.primary_button = 0;
    question.marked = 0;
    return question;
}

} // namespace oa::ui::mutator_picker
