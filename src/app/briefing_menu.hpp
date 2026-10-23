// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Open Annihilation's mission briefing screen on the OA layer
// (runtime_briefing_menu.cpp keeps it over MSNBRIEF.GUI). It covers the
// whole window with the main menu's art (menu_paint.hpp): under a smaller
// OPEN ANNIHILATION title and a MISSION BRIEFING heading, the mission's
// panorama scrolling in a bolted frame with the planet's name on a plate
// in its corner; an ORDERS panel with the briefing, its first line as the
// title in amber and its highlighted words in their colours, a scroll bar
// and MORE…; a PLANET panel with the planet turning in a round frame, a
// table of the planet, the side, the wind, the gravity and the difficulty,
// and the NARRATION switch; PREVIOUS MENU and START, and the version plate.
//
// The screen is not modal: the screens over it take what they show over it,
// and nothing under it sees the pointer. The arrows, Page Up, Page Down,
// Home and End scroll the briefing; every other key goes on to
// MSNBRIEF.GUI, whose Return and Escape stay the game's.
#pragma once

#include "oa_layer.hpp"

#include "oa/app/window_icon.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app {

/// The briefing screen's name on the OA layer.
inline constexpr std::string_view briefing_menu_screen_name = "briefing-menu";

/// What the briefing screen shows of the mission briefing now.
struct BriefingMenuFacts {
    /// The briefing in UTF-8, its lines ended by line feeds, its highlighted
    /// words marked as the briefing files mark them: `&X` before them, X
    /// naming the colour, and `&` after.
    std::string text{};
    std::string planet{};          ///< the planet's name
    std::string side{};            ///< the side's name, in the language shown
    std::string wind{};            ///< the wind speed shown
    std::string gravity{};         ///< the gravity shown
    std::string difficulty{};      ///< the difficulty, in the language shown
    bool narration{};              ///< the narration plays: SHUTUP is on
    int32_t panorama_scroll{};     ///< the panorama's first column on show
    std::size_t planet_frame{};    ///< the planet's turn, by its frame
    bool can_start{};              ///< START takes a press
    char start_key{};              ///< START's quick key; 0 for none
    char back_key{};               ///< PREVIOUS MENU's quick key; 0 for none
    std::string version{};         ///< the version label; empty for no plate

    friend bool operator==(const BriefingMenuFacts&, const BriefingMenuFacts&) = default;
};

/// What the briefing screen's presses do and the pictures it shows.
struct BriefingMenuActions {
    /// Starts the mission.
    std::function<void()> start{};
    /// Leaves for the screen the briefing was opened over.
    std::function<void()> back{};
    /// Switches the narration on or off.
    std::function<void()> narration{};
    /// Returns the panorama's strip of frames side by side, left to right;
    /// an empty picture for none. Asked once.
    std::function<WindowIcon()> panorama{};
    /// Returns the planet's frames, one a step of its turn; none for no
    /// planet. Asked once.
    std::function<std::vector<WindowIcon>()> planet{};
};

/// Makes the briefing screen.
///
/// @param shows tells whether the screen shows; it closes when it does not
/// @param facts reads what the briefing shows now
/// @param actions what its presses do
/// @return the screen
[[nodiscard]] std::unique_ptr<LayerScreen> make_briefing_menu(
    std::function<bool()> shows,
    std::function<BriefingMenuFacts()> facts,
    BriefingMenuActions actions
);

} // namespace oa::app
