// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The four questions a mutator problem asks. Each one is pure: it reads no
// file, preference, clock or network, and every word comes from text.hpp
// through the Library's TextHooks. The app raises them. The kit draws the
// text as given; the amber line is the question's failure.
#pragma once

#include "oa/ui/kit/components_more.hpp"
#include "oa/ui/library/library.hpp"

#include <cstdint>
#include <span>
#include <string>

namespace oa::ui::mutator_picker {

/// BACK on "Start without X?". It leaves the set as it is. The cancel button.
inline constexpr std::string_view start_back_id = "back";
/// START WITHOUT IT, or START WITHOUT THEM. The primary button, and the one marked.
inline constexpr std::string_view start_without_id = "start-without";
/// KEEP IT on "Forget the last set?". It leaves the remembered set. The cancel button.
inline constexpr std::string_view keep_it_id = "keep-it";
/// FORGET IT. It clears that setup's remembered set. The primary button, and the one marked.
inline constexpr std::string_view forget_it_id = "forget-it";
/// CANCEL on "Get mutators". It closes the question and leaves the downloads queued.
inline constexpr std::string_view get_cancel_id = "cancel";
/// RETRY, shown once a download has stopped. The primary button, and the one marked.
inline constexpr std::string_view get_retry_id = "retry";
/// OK on "The game did not start". It is the cancel button and the primary one.
inline constexpr std::string_view did_not_start_id = "ok";

/// One mutator that stopped a game from starting, as the player reads it.
struct Failed {
    std::string name{};   ///< the manifest's name, or the id when it has none
    std::string reason{}; ///< why it stopped, in the player's words
};

/// Makes "Start without X?" for the mutators that stopped a start.
///
/// One mutator asks "Start without {name}?" with START WITHOUT IT. Several,
/// and a list, ask "Start without them?" with START WITHOUT THEM. The
/// paragraph says nothing they changed is kept. Each failure is "{name}:
/// {reason}" on the amber line, or the reason alone when the name is empty,
/// joined with spaces. BACK is the cancel button. START WITHOUT IT, or
/// START WITHOUT THEM, is the primary button and the one marked.
///
/// @param failures the mutators that stopped the start, in list order
/// @param hooks how texts are looked up
/// @return the question
[[nodiscard]] kit::Question
start_without_question(std::span<const Failed> failures, const library::TextHooks& hooks = {});

/// Makes "Forget the last set?" for the mutators a crash left on.
///
/// The body names them. The hint says the game's log names them. KEEP IT is
/// the cancel button. FORGET IT is the primary button and the one marked.
///
/// @param names the mutators' names, in the set's order
/// @param hooks how texts are looked up
/// @return the question
[[nodiscard]] kit::Question
forget_set_question(std::span<const std::string> names, const library::TextHooks& hooks = {});

/// How far "Get mutators" has got with the On mutators that are not installed.
struct GetProgress {
    std::string name{};    ///< the mutator the line names
    std::size_t index{};   ///< its place among those still to install, from 1
    std::size_t total{};   ///< how many of the set are still to install
    int32_t progress{};    ///< the bar, in thousandths of the whole
    std::string problem{}; ///< why the download stopped; empty while it runs
};

/// Makes "Get mutators" for one download of the set.
///
/// The paragraph is "Downloading {name} · {index} of {total}" and the bar is
/// always shown. With a problem, the amber line is "{problem} RETRY tries
/// again." and the buttons are CANCEL and RETRY, RETRY primary and marked.
/// With none, CANCEL is the only button, and it is the cancel button, the
/// primary one and the one marked.
///
/// @param progress the download the question shows
/// @param hooks how texts are looked up
/// @return the question
[[nodiscard]] kit::Question
get_mutators_question(const GetProgress& progress, const library::TextHooks& hooks = {});

/// One machine whose mutators would play differently.
struct Differs {
    std::string player{};  ///< that player's name
    std::string name{};    ///< the mutator's name; empty skips its failure line
    std::string here{};    ///< the release on this machine
    std::string there{};   ///< the release on that machine; empty when it has none
    std::string fetches{}; ///< the release GET MUTATORS can fetch; empty when it cannot
};

/// Makes "The game did not start" for machines whose results differ.
///
/// The paragraph names the player, or the players joined when several
/// differ. Each mutator is "{name}: release {here} here, release {there} on
/// {player}'s machine." or "{name}: {player} does not have it." when `there`
/// is empty, on the amber line, joined with spaces. A failure whose name is
/// empty is left out. Each `fetches` is its own paragraph. OK is the only
/// button, and it is the cancel button and the primary one. Nothing is marked
/// ahead of it: the marked button is OK.
///
/// @param differs the differences, in the order they were found
/// @param hooks how texts are looked up
/// @return the question
[[nodiscard]] kit::Question
did_not_start_question(std::span<const Differs> differs, const library::TextHooks& hooks = {});

} // namespace oa::ui::mutator_picker
