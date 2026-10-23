// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Open Annihilation's Load Game screen on the OA layer
// (runtime_load_game_menu.cpp keeps it over LOADGAME.GUI in its load role).
// It covers the whole window with the main menu's art (menu_paint.hpp):
// under a smaller OPEN ANNIHILATION title and a LOAD GAME heading, a SAVED
// GAMES panel with a row for every save, newest first, the chosen one in
// amber, and a line telling how many there are; a DETAILS panel with the
// chosen save's radar picture, its name and what the dialog tells of it;
// PREVIOUS MENU and LOAD, and the version plate. Without saves the list says
// so itself and LOAD takes no press.
//
// The screen is not modal: the screens over it take what they show over it,
// and nothing under it sees the pointer. The arrows, Page Up, Page Down,
// Home and End choose a save and Return loads it; every other key goes on to
// LOADGAME.GUI, whose quick keys and Escape stay the game's.
#pragma once

#include "oa_layer.hpp"

#include "oa/app/window_icon.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app {

/// The Load Game screen's name on the OA layer.
inline constexpr std::string_view load_game_menu_screen_name = "load-game-menu";

/// One fact about a save: what it is and its value.
struct LoadGameMenuFact {
    std::string label{}; ///< what it is, in the language shown
    std::string value{}; ///< its value

    friend bool operator==(const LoadGameMenuFact&, const LoadGameMenuFact&) = default;
};

/// What the Load Game screen shows of the load dialog now.
struct LoadGameMenuFacts {
    std::vector<std::string> saves{};      ///< the saves' names, newest first
    std::optional<std::size_t> chosen{};   ///< the chosen save; none for none
    std::vector<LoadGameMenuFact> facts{}; ///< the chosen save's details, for DETAILS
    bool can_load{};                       ///< LOAD takes a press
    char load_key{};                       ///< LOAD's quick key; 0 for none
    char back_key{};                       ///< PREVIOUS MENU's quick key; 0 for none
    std::string version{};                 ///< the version label; empty for no plate

    friend bool operator==(const LoadGameMenuFacts&, const LoadGameMenuFacts&) = default;
};

/// What the Load Game screen's presses do.
struct LoadGameMenuActions {
    /// Chooses a save, by its place.
    std::function<void(std::size_t)> choose{};
    /// Loads the chosen save.
    std::function<void()> load{};
    /// Leaves for the screen the dialog was opened over.
    std::function<void()> back{};
    /// Returns the radar picture saved with the chosen save; an empty
    /// picture for none. Asked again when the choice changes.
    std::function<WindowIcon()> picture{};
};

/// Makes the Load Game screen.
///
/// @param shows tells whether the screen shows; it closes when it does not
/// @param facts reads what the load dialog shows now
/// @param actions what its presses do
/// @return the screen
[[nodiscard]] std::unique_ptr<LayerScreen> make_load_game_menu(
    std::function<bool()> shows,
    std::function<LoadGameMenuFacts()> facts,
    LoadGameMenuActions actions
);

} // namespace oa::app
