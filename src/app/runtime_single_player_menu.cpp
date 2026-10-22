// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Open Annihilation's Single Player menu on the OA layer: one column menu
// (column_menu.hpp), single-player-menu, kept at the bottom of the layer
// while SINGLE.GUI shows (Runtime::keep_single_player_menu_screen, once a
// frame from OaLayer::tick). Under a smaller OPEN ANNIHILATION title and a
// SINGLE PLAYER heading it has one column of five buttons, New Campaign,
// Load Game, Skirmish, Any Mission and, standing apart, Previous Menu;
// beside them a panel telling what the button under the pointer or
// the focus does; and under them the main menu's version plate. Each
// button is pressed as a click on SINGLE.GUI's own presses it
// (Runtime::press_menu_gadget), so its sound, its checks and the screen it
// leads to are the game's. A button SINGLE.GUI grays out, as Load Game is
// without saved games and Skirmish in the demo, shows faded and takes no
// press; one it hides, as Any Mission is until its code is typed, leaves
// its place to the next. SINGLE.GUI's Options has no button here, and its
// quick key goes no further: the main menu's Settings will hold what it
// sets. Every other key but the arrows, Tab, Return and Space goes on to
// SINGLE.GUI, so that its quick keys, Escape and the code stay the game's. While a message box of the game's shows, the menu steps
// aside, and SINGLE.GUI shows under the box.

#include "column_menu.hpp"
#include "oa_layer.hpp"

#include "oa/app/runtime.hpp"
#include "oa/ui/gui_layout.hpp"

#include <array>
#include <cctype>
#include <cstddef>
#include <string_view>
#include <variant>

namespace oa::app {

namespace {

/// How many buttons the menu has.
constexpr std::size_t entry_count = 5;

/// Each button, top to bottom.
constexpr std::array<ColumnMenuEntry, entry_count> entries{{
    {"new-campaign",
     "NEW CAMPAIGN",
     "Begin the Arm or the Core campaign from its first mission.",
     false},
    {"load-game", "LOAD GAME", "Go on with a game you saved.", false},
    {"skirmish",
     "SKIRMISH",
     "Fight computer players on a map you choose, with the sides, the resources and the rules "
     "you set.",
     false},
    {"any-mission", "ANY MISSION", "Play any mission of either campaign.", false},
    {"previous-menu", "PREVIOUS MENU", "Go back to the main menu.", true},
}};

/// The record of SINGLE.GUI each button presses, in the entries' order.
constexpr std::array<std::string_view, entry_count> records{
    {"NewCamp", "LoadGame", "Skirmish", "AnyMsn", "PrevMenu"}
};

/// The record of SINGLE.GUI the menu leaves out, whose quick key it
/// withholds: the main menu's Settings will hold what it sets.
constexpr std::string_view left_out_record = "Options";

/// Returns what the menu holds and where its parts stand, in rows of a
/// window 1080 rows high: a smaller title than the main menu's, the
/// heading under it, and the column beside the panel.
///
/// @return the look
ColumnMenuLook single_player_look() {
    ColumnMenuLook look;
    look.screen_name = single_player_menu_screen_name;
    look.entries = entries;
    look.heading = "SINGLE PLAYER";
    look.title_top = 36.0;
    look.title_columns = 700.0;
    look.heading_top = 228.0;
    look.first_button_top = 300.0;
    look.about_panel = true;
    return look;
}

} // namespace

bool Runtime::oa_single_player_menu_shown() {
    return oa_layer().find(single_player_menu_screen_name) != nullptr;
}

void Runtime::keep_single_player_menu_screen() {
    const auto shows = [this] { return screen_ == Screen::single_player && oa_menu_may_show(); };
    if (!shows())
        return;
    OaLayer& layer = oa_layer();
    if (layer.find(single_player_menu_screen_name) != nullptr)
        return;
    const auto facts = [this] {
        ColumnMenuFacts now = column_menu_facts(entry_count);
        const auto& gadgets = resources_.layout.gadgets;
        for (std::size_t index = 0; index < entry_count; ++index) {
            const auto found = gui_record_named(gadgets, records[index]);
            if (!found || gadgets[*found].common.active == 0)
                continue;
            now.shown[index] = true;
            now.enabled[index] = frontend_gadget_pressable(*found);
            if (const auto* button =
                    std::get_if<oa::ui::gui_layout::ButtonFields>(&gadgets[*found].fields))
                now.quick_keys[index] = static_cast<char>(button->quick_key);
        }
        if (const auto found = gui_record_named(gadgets, left_out_record))
            if (const auto* button =
                    std::get_if<oa::ui::gui_layout::ButtonFields>(&gadgets[*found].fields))
                if (button->quick_key != 0)
                    now.withheld_keys.push_back(static_cast<char>(
                        std::tolower(static_cast<unsigned char>(button->quick_key))
                    ));
        now.version = menu_version_;
        return now;
    };
    const auto press = [this](std::size_t index) {
        if (index < entry_count)
            press_menu_gadget(Screen::single_player, records[index]);
    };
    layer.push_under(make_column_menu(single_player_look(), shows, facts, press));
}

} // namespace oa::app
