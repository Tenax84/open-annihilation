// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Open Annihilation's Skirmish screen on the OA layer
// (runtime_skirmish_menu.cpp keeps it over SKIRMISH.GUI). It covers the
// whole window with the main menu's art (menu_paint.hpp): under a smaller
// OPEN ANNIHILATION title and a SKIRMISH heading, a PLAYERS panel with a row
// for every player slot SKIRMISH.GUI has, a MAP panel with the map's picture,
// its start positions, its name and SELECT MAP, a GAME OPTIONS panel with the
// five rules, PREVIOUS MENU and START, and the version plate.
//
// A slot's row shows who plays it, the side with its emblem, the colour, the
// team, the metal and the energy; the local player's row is ringed in amber.
// An open slot shows only who plays it. A press on a field presses the
// SKIRMISH.GUI button of that field and slot, with the mouse button it was
// made with, so that the right button steps the colour back and lowers the
// metal and energy, as in the game. An option's right arrow presses its
// button once; its left arrow steps it back by pressing it round to the
// stage before. The bar under the rows says what the field under the pointer
// does.
//
// The screen is not modal: the screens over it take what they show over it,
// and nothing under it sees the pointer. Return and Space start; every other
// key goes on to SKIRMISH.GUI, whose quick keys, Escape and typed codes stay
// the game's.
#pragma once

#include "oa_layer.hpp"

#include "oa/app/window_icon.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app {

/// The Skirmish screen's name on the OA layer.
inline constexpr std::string_view skirmish_menu_screen_name = "skirmish-menu";

/// How many rules the GAME OPTIONS panel shows: Commander, Location,
/// Mapping, Line of Sight and Difficulty.
inline constexpr std::size_t skirmish_menu_options = 5;

/// One player slot, as SKIRMISH.GUI shows it.
struct SkirmishMenuSlot {
    std::string player{};    ///< who plays it, as its Player button reads: Player, Computer or Open
    bool open{};             ///< nobody plays it: its other fields show nothing
    bool local{};            ///< the player at this machine plays it
    std::size_t side{};      ///< its side, by its place in the side data
    std::string side_name{}; ///< the side's name, such as ARM
    int32_t colour{};        ///< its colour, a frame of the colour logos
    int32_t team{};          ///< its alliance group, 1 to 5; 0 for none
    std::string metal{};     ///< its starting metal, as its Metal button reads
    std::string energy{};    ///< its starting energy, as its Energy button reads

    friend bool operator==(const SkirmishMenuSlot&, const SkirmishMenuSlot&) = default;
};

/// One rule of the GAME OPTIONS panel.
struct SkirmishMenuOption {
    std::string record{}; ///< its button's name in SKIRMISH.GUI, such as CommanderDeath
    std::string label{};  ///< what it sets, in the language shown, such as Commander
    std::string value{};  ///< its button's caption now, such as Game ends
    uint32_t stages{};    ///< how many values its button steps through; 0 takes no press
    std::string help{};   ///< what the value now does, as the game's help says

    friend bool operator==(const SkirmishMenuOption&, const SkirmishMenuOption&) = default;
};

/// A start position on the map's picture.
struct SkirmishMenuStart {
    int32_t number{}; ///< its number, from 1
    double across{};  ///< how far right on the picture, 0 to 1
    double down{};    ///< how far down on the picture, 0 to 1

    friend bool operator==(const SkirmishMenuStart&, const SkirmishMenuStart&) = default;
};

/// What the Skirmish screen shows of SKIRMISH.GUI and the setup now.
struct SkirmishMenuFacts {
    std::vector<SkirmishMenuSlot> slots{};                         ///< the slots, in order
    std::array<SkirmishMenuOption, skirmish_menu_options> options{}; ///< the rules
    std::string map{};               ///< the map's name, as Current Map shows it
    std::string map_record{};        ///< the map's name in the game data; changes with its picture
    std::string players{};           ///< how many players the map is for, as it says
    std::string size{};              ///< the map's size, as it says, such as 8 x 8
    std::vector<SkirmishMenuStart> starts{}; ///< the start positions for the players now
    std::array<std::string, 6> help{}; ///< the hints of Player, Side, Color, Allies, Metal and Energy
    char start_key{};                ///< Start's quick key; 0 for none
    char back_key{};                 ///< Previous Menu's quick key; 0 for none
    char map_key{};                  ///< Select Map's quick key; 0 for none
    bool can_start{};                ///< Start takes a press
    bool can_select_map{};           ///< Select Map takes a press
    std::string version{};           ///< the version label; empty for no plate

    friend bool operator==(const SkirmishMenuFacts&, const SkirmishMenuFacts&) = default;
};

/// What the Skirmish screen's presses do.
struct SkirmishMenuActions {
    /// Presses a SKIRMISH.GUI button, by its name, with a mouse button, 1
    /// left or 2 right.
    std::function<void(std::string_view, int32_t)> press{};
    /// Returns a side's emblem, by its place in the side data: its logo, else
    /// its commander's picture, or an empty picture for none. Asked once for
    /// each side.
    std::function<WindowIcon(std::size_t)> emblem{};
    /// Returns a colour's swatch, by its frame of the colour logos; an empty
    /// picture for none. Asked once for each colour.
    std::function<WindowIcon(int32_t)> swatch{};
    /// Returns the map's picture, the part of its minimap the map covers;
    /// an empty picture for none. Asked again when the map changes.
    std::function<WindowIcon()> map_picture{};
};

/// Makes the Skirmish screen.
///
/// @param shows tells whether the screen shows; it closes when it does not
/// @param facts reads what SKIRMISH.GUI and the setup show now
/// @param actions what its presses do
/// @return the screen
[[nodiscard]] std::unique_ptr<LayerScreen> make_skirmish_menu(
    std::function<bool()> shows,
    std::function<SkirmishMenuFacts()> facts,
    SkirmishMenuActions actions
);

} // namespace oa::app
