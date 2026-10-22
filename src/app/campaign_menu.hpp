// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Open Annihilation's New Campaign screen on the OA layer
// (runtime_new_campaign_menu.cpp keeps it over NEWGAME.GUI). It covers the
// whole window with the main menu's art (menu_paint.hpp): under a smaller
// OPEN ANNIHILATION title and a NEW CAMPAIGN heading, a SELECT SIDE strip
// with a tile for every side the game data defines, a SELECT CAMPAIGN list
// of the chosen side's campaigns, a panel telling about the chosen campaign
// with the difficulty under it, PREVIOUS MENU and START, and the version
// plate.
//
// The strip holds as many sides as the side data names, the arrows at its
// ends paging through those it has no room for; a side past 3.1c's two
// carries a MOD tag. A side without a campaign shows faded and takes no
// press. The list scrolls when it holds more campaigns than it shows.
//
// The screen is not modal: the screens over it take what they show over it,
// and nothing under it sees the pointer. Left and Right choose the side, Up,
// Down, Page Up, Page Down, Home and End the campaign, Return and Space
// start; every other key goes on to NEWGAME.GUI, whose quick keys and Escape
// stay the game's.
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

/// The New Campaign screen's name on the OA layer.
inline constexpr std::string_view new_campaign_menu_screen_name = "new-campaign-menu";

/// One side the strip shows.
struct CampaignMenuSide {
    std::string name{};    ///< its name, as the side data gives it, such as ARM
    bool has_campaign{};   ///< a campaign names it: it takes a press
    bool added{};          ///< it is past 3.1c's two sides: it carries a MOD tag

    friend bool operator==(const CampaignMenuSide&, const CampaignMenuSide&) = default;
};

/// What the New Campaign screen shows of NEWGAME.GUI and the setup now.
struct CampaignMenuFacts {
    std::vector<CampaignMenuSide> sides{}; ///< the sides, in the side data's order
    std::size_t side{};                    ///< the chosen side
    std::vector<std::string> campaigns{};  ///< the chosen side's campaigns, as the list names them
    std::size_t campaign{};                ///< the chosen campaign's row
    std::size_t missions{};                ///< how many missions the chosen campaign has
    std::string first_mission{};           ///< the chosen campaign's first mission, as listed
    /// The difficulties' captions, easy to hard, in the language shown.
    std::array<std::string, 3> difficulties{};
    uint32_t difficulty{};  ///< the chosen difficulty, 0 easy to 2 hard
    char start_key{};       ///< Start's quick key; 0 for none
    char back_key{};        ///< Previous Menu's quick key; 0 for none
    bool can_start{};       ///< Start takes a press
    std::string version{};  ///< the version label; empty for no plate

    friend bool operator==(const CampaignMenuFacts&, const CampaignMenuFacts&) = default;
};

/// What the New Campaign screen's presses do.
struct CampaignMenuActions {
    std::function<void(std::size_t)> choose_side{};     ///< chooses a side, by its place
    std::function<void(std::size_t)> choose_campaign{}; ///< chooses a campaign, by its row
    std::function<void(uint32_t)> choose_difficulty{};  ///< chooses a difficulty, 0 to 2
    std::function<void()> start{};                      ///< presses Start
    std::function<void()> back{};                       ///< presses Previous Menu
    /// Returns a side's emblem, by its place: its logo, else its commander's
    /// picture, or an empty picture for none. Asked once for each side.
    std::function<WindowIcon(std::size_t)> emblem{};
};

/// Makes the New Campaign screen.
///
/// @param shows tells whether the screen shows; it closes when it does not
/// @param facts reads what NEWGAME.GUI and the setup show now
/// @param actions what its presses do
/// @return the screen
[[nodiscard]] std::unique_ptr<LayerScreen> make_campaign_menu(
    std::function<bool()> shows,
    std::function<CampaignMenuFacts()> facts,
    CampaignMenuActions actions
);

} // namespace oa::app
