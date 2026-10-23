// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Open Annihilation's selection bar in a match (runtime_selection_bar.cpp
// draws it and takes its presses): a slim strip of bolted steel along the
// bottom of the battlefield, in the main menu's steel (menu_paint.hpp). On
// its left, under a GROUPS plate, the nine groups' buttons, each with its
// number, how many units it holds and a tank or an aircraft for what most
// of them are, the group that is the whole selection lit in amber, and a
// line naming the keys that store and select a group. On its right, under
// a SELECT plate, a button for each of the selections the Ctrl+letter keys
// make, and one for every ground combat unit, each with a pictogram, its
// caption and its key. Everything is laid out in points from the bar's top
// left corner; the bar is drawn at a whole scale.
#pragma once

#include "oa/ui/kit/canvas.hpp"
#include "oa/ui/kit/layout.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace oa::app::selection_bar {

/// How many group buttons the bar has: groups 1 to 9.
inline constexpr std::size_t group_count = 9;
/// How many selection buttons the bar has.
inline constexpr std::size_t command_count = 8;
/// How many buttons the bar has: the groups' first, then the selections'.
inline constexpr std::size_t button_count = group_count + command_count;
/// The button index for none.
inline constexpr int32_t no_button = -1;
/// The bar's height, in points.
inline constexpr int32_t bar_height = 60;

/// The selection buttons, left to right; a button's index is group_count
/// more than its command's.
enum class Command : uint8_t {
    all,       ///< every unit of the player's (Ctrl+A)
    commander, ///< the commander, the view following it (Ctrl+C)
    air,       ///< every aircraft (Ctrl+V)
    combat,    ///< every ground combat unit: mobile, armed, no builder and no aircraft
    builders,  ///< every mobile builder (Ctrl+B)
    factories, ///< every factory (Ctrl+F)
    on_screen, ///< every unit on the screen (Ctrl+S)
    same_type, ///< every unit of a selected unit's type (Ctrl+Z)
};

/// What a group button shows of its group.
struct Group {
    uint16_t units{}; ///< how many of the player's units it holds
    bool aircraft{};  ///< most of them fly
};

/// What the bar shows now.
struct Facts {
    std::array<Group, group_count> groups{}; ///< groups 1 to 9
    /// The group, 0 to 8, that is the whole selection and holds nothing
    /// else; no_button for none.
    int32_t lit_group{no_button};
    /// A digit alone selects its group (SwitchAlt); otherwise Alt and the
    /// digit do.
    bool digit_selects{};
    /// Each selection button's key, as the player presses it; empty for a
    /// button whose selection no key makes now.
    std::array<std::string, command_count> keys{};
};

/// The pointer on the bar.
struct Pointer {
    int32_t hovered{no_button}; ///< the button under the pointer
    int32_t held{no_button};    ///< the button pressed and not yet released
};

/// Where the bar's parts lie, in its points.
struct Layout {
    int32_t width{};                                  ///< the bar's width
    oa::ui::kit::Rect groups{};                       ///< the GROUPS panel
    oa::ui::kit::Rect commands{};                     ///< the SELECT panel
    oa::ui::kit::Rect groups_plate{};                 ///< GROUPS
    oa::ui::kit::Rect commands_plate{};               ///< SELECT
    oa::ui::kit::Rect hint{};                         ///< the line of the groups' keys
    std::array<oa::ui::kit::Rect, button_count> buttons{}; ///< by button index
};

/// Returns how wide the bar is at the least: its buttons as narrow as
/// their captions and keys let them be.
///
/// @param canvas what measures the text
/// @param facts what the bar shows
/// @return the width, in points
[[nodiscard]] int32_t least_width(const oa::ui::kit::Canvas& canvas, const Facts& facts);

/// Lays the bar out for a width: the GROUPS panel as wide as its buttons,
/// the SELECT panel the rest, its buttons sharing it, up to a width each,
/// and the bar no wider than its panels then are.
///
/// @param canvas what measures the text
/// @param facts what the bar shows
/// @param width the room, in points; at least least_width
/// @return the layout
[[nodiscard]] Layout lay_out(const oa::ui::kit::Canvas& canvas, const Facts& facts, int32_t width);

/// Returns the button at a point.
///
/// @param layout the bar's layout
/// @param at the point, in the bar's points
/// @return the button's index; no_button when none is there
[[nodiscard]] int32_t button_at(const Layout& layout, oa::ui::kit::Point at) noexcept;

/// Draws the bar from its top left corner.
///
/// @param canvas where it draws, at the bar's scale
/// @param layout the bar's layout
/// @param facts what it shows
/// @param pointer the pointer on it
void draw(
    const oa::ui::kit::Canvas& canvas,
    const Layout& layout,
    const Facts& facts,
    const Pointer& pointer
);

} // namespace oa::app::selection_bar
