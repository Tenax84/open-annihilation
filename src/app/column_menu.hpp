// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Open Annihilation's column menus on the OA layer: the main menu
// (runtime_main_menu.cpp) and the Single Player menu
// (runtime_single_player_menu.cpp). Each covers the whole window with the
// main menu's art (branding/main-menu): the battlefield behind, the OPEN
// ANNIHILATION title, a heading under it when the menu has one, one column
// of buttons, a panel beside the column telling what the button under the
// pointer or the focus does when the menu has one and the window is wide
// enough, and under them a plate with the version label.
//
// A menu is not modal: the screens over it take what they show over it, and
// nothing under it sees the pointer. The arrows, Tab, Return and Space move
// between and press its buttons; every other key goes on to the game's
// screen under it, whose quick keys and Escape stay the game's.
#pragma once

#include "oa_layer.hpp"

#include "oa/ui/gui_layout.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app {

/// The main menu's screen's name on the OA layer.
inline constexpr std::string_view main_menu_screen_name = "main-menu";

/// The Single Player menu's screen's name on the OA layer.
inline constexpr std::string_view single_player_menu_screen_name = "single-player-menu";

/// One button of a column menu.
struct ColumnMenuEntry {
    std::string_view name{};    ///< its automation name, after the screen's
    std::string_view english{}; ///< its caption, looked up in the interface catalogue
    /// What it does, looked up in the interface catalogue, for the panel
    /// beside the column; empty for none.
    std::string_view about{};
    /// It leads back: it stands apart at the column's foot, an arrow before
    /// its caption.
    bool back{};
};

/// What a column menu holds and where its parts stand, in rows of a window
/// 1080 rows high.
struct ColumnMenuLook {
    std::string_view screen_name{};            ///< the screen's name on the OA layer
    std::span<const ColumnMenuEntry> entries{}; ///< the buttons, top to bottom
    /// The heading under the title, looked up in the interface catalogue;
    /// empty for none.
    std::string_view heading{};
    double title_top{};        ///< the title's top
    double title_columns{};    ///< the title's width
    double heading_top{};      ///< the heading's top
    double first_button_top{}; ///< the first button's top
    bool about_panel{};        ///< a panel beside the column tells what a button does
};

/// What a column menu shows of the game's screen under it now.
struct ColumnMenuFacts {
    /// Which buttons show, in the look's order. One that does not leaves
    /// its place to the next.
    std::vector<bool> shown{};
    /// Which buttons take a press, in the look's order; one that shows and
    /// takes none is faded.
    std::vector<bool> enabled{};
    /// Each button's quick key, as the game's screen gives it; 0 for none.
    std::vector<char> quick_keys{};
    /// The quick keys of the game's screen's buttons the menu leaves out,
    /// in lower case: their presses go no further, so those buttons stay
    /// out of reach.
    std::string withheld_keys{};
    /// The version label; empty for no plate.
    std::string version{};

    friend bool operator==(const ColumnMenuFacts&, const ColumnMenuFacts&) = default;
};

/// Returns the facts of a menu of a number of buttons: none shown, none
/// enabled, no quick keys, none withheld and no version.
///
/// @param count how many buttons the menu has
/// @return the facts
[[nodiscard]] ColumnMenuFacts column_menu_facts(std::size_t count);

/// Makes a column menu's screen.
///
/// @param look what it holds; the entries and the words stay as long as the screen
/// @param shows tells whether the menu shows; the screen closes when it does not
/// @param facts reads what the game's screen under it shows now
/// @param press presses a button, by its place in the look's entries
/// @return the screen
[[nodiscard]] std::unique_ptr<LayerScreen> make_column_menu(
    const ColumnMenuLook& look,
    std::function<bool()> shows,
    std::function<ColumnMenuFacts()> facts,
    std::function<void(std::size_t)> press
);

/// Returns a record of a GUI layout by its name, compared in either case as
/// the game's own lookup compares them.
///
/// @param gadgets the layout's records
/// @param name the name
/// @return its place among them; none without such a record
[[nodiscard]] std::optional<std::size_t>
gui_record_named(std::span<const oa::ui::gui_layout::Gadget> gadgets, std::string_view name);

} // namespace oa::app
