// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The selection bar's layout and drawing (selection_bar.hpp).

#include "selection_bar.hpp"

#include "menu_paint.hpp"

#include "oa/ui/kit/text.hpp"

#include <algorithm>
#include <string>
#include <string_view>

namespace oa::app::selection_bar {

namespace kit = oa::ui::kit;
using namespace menu_paint;

namespace {

/// The panels' rim, in points.
constexpr int32_t rim = 4;
/// The room between the two panels.
constexpr int32_t panel_gap = 3;
/// The room between a panel's rim and its buttons, either side.
constexpr int32_t panel_padding = 4;
/// The header plates' height; each sits on its panel's top rim.
constexpr int32_t plate_height = 14;
/// The header plates' left edge, in from their panel's.
constexpr int32_t plate_inset = 10;
/// The header plates' extra points after each character.
constexpr int32_t plate_tracking = 2;
/// The buttons' top.
constexpr int32_t buttons_top = plate_height + 1;
/// A group button's size, and the room between two buttons.
constexpr int32_t group_width = 22;
constexpr int32_t group_height = 28;
constexpr int32_t button_gap = 2;
/// The line of the groups' keys under their buttons.
constexpr int32_t hint_height = 12;
/// A selection button's height: down to the end of the hint's line.
constexpr int32_t command_height = group_height + hint_height;
/// A selection button's width at the least and at the most, and the room
/// either side of its caption.
constexpr int32_t command_least_width = 36;
constexpr int32_t command_most_width = 84;
constexpr int32_t command_padding = 3;
/// A selection button's pictogram: its box's height, from the button's top.
constexpr int32_t pictogram_top = 2;
constexpr int32_t pictogram_height = 13;
/// A selection button's caption and key lines, small_line high.
constexpr int32_t caption_top = pictogram_top + pictogram_height;
constexpr int32_t text_line = 12;
/// A group button's number line, and its count line under it.
constexpr int32_t number_height = 15;
constexpr int32_t count_height = 11;
/// How far the lit group's glow reaches.
constexpr int32_t glow_reach = 2;

constexpr kit::Colour seam_colour{18, 20, 22};          ///< between the panels
constexpr kit::Colour button_colour{78, 84, 86};        ///< a button's steel
constexpr kit::Colour hovered_colour{96, 102, 104};     ///< under the pointer
constexpr kit::Colour held_colour{56, 60, 62};          ///< pressed
constexpr kit::Colour lit_colour{116, 84, 36};          ///< the lit group's steel, amber
constexpr kit::Colour amber_colour{232, 164, 52};       ///< its glow and its number
constexpr kit::Colour hover_edge_colour{176, 184, 184}; ///< round what the pointer is over
constexpr kit::Colour hint_colour{168, 172, 160};       ///< the line of the groups' keys

/// The selection buttons' captions, by Command.
constexpr std::array<std::string_view, command_count> command_captions{
    "ALL", "COMMANDER", "AIR", "COMBAT", "BUILDERS", "FACTORIES", "ON SCREEN", "SAME TYPE",
};

/// Returns a width a text takes in the small font.
///
/// @param canvas what measures it
/// @param text the text
/// @return the width, in points
int32_t small_width(const kit::Canvas& canvas, std::string_view text) {
    return kit::measure_text(canvas, kit::FontRole::small, text);
}

/// Returns the line of the groups' keys, in the language shown.
///
/// @param facts what the bar shows
/// @return the line
std::string hint_line(const Facts& facts) {
    return worded("Ctrl+# store") + "  " +
           worded(facts.digit_selects ? "# select" : "Alt+# select");
}

/// Returns the GROUPS panel's width.
///
/// @return the width, in points
constexpr int32_t groups_width() {
    return 2 * (rim + panel_padding) + static_cast<int32_t>(group_count) * group_width +
           static_cast<int32_t>(group_count - 1) * button_gap;
}

/// Returns how narrow a selection button may be: as wide as its caption
/// and its key, and command_least_width at the least.
///
/// @param canvas what measures the text
/// @param facts what the bar shows
/// @return the width, in points
int32_t command_width(const kit::Canvas& canvas, const Facts& facts) {
    int32_t widest = command_least_width;
    for (std::size_t index = 0; index < command_count; ++index) {
        widest = std::max(
            widest, small_width(canvas, worded(command_captions[index])) + 2 * command_padding
        );
        widest = std::max(widest, small_width(canvas, facts.keys[index]) + 2 * command_padding);
    }
    return widest;
}

/// Returns a SELECT panel's width for its buttons' width.
///
/// @param button the buttons' width
/// @return the width, in points
constexpr int32_t commands_width(int32_t button) {
    return 2 * (rim + panel_padding) + static_cast<int32_t>(command_count) * button +
           static_cast<int32_t>(command_count - 1) * button_gap;
}

/// Draws a tank seen from the side, its tracks under it and its gun
/// pointing right, in a box 15 by 10 points from a corner.
///
/// @param canvas where it draws
/// @param at the box's top left corner
/// @param ink its colour
void draw_tank(const kit::Canvas& canvas, kit::Point at, kit::Colour ink) {
    kit::fill_rect(canvas, {at.x + 1, at.y + 7, 13, 3}, ink);
    kit::fill_rect(canvas, {at.x + 2, at.y + 4, 11, 3}, ink);
    kit::fill_rect(canvas, {at.x + 5, at.y + 2, 5, 2}, ink);
    kit::fill_rect(canvas, {at.x + 10, at.y + 2, 5, 1}, ink);
    kit::fill_rect(canvas, {at.x + 2, at.y + 8, 11, 1}, seam_colour);
}

/// Draws a small tank, 7 by 5 points from a corner.
///
/// @param canvas where it draws
/// @param at the box's top left corner
/// @param ink its colour
void draw_small_tank(const kit::Canvas& canvas, kit::Point at, kit::Colour ink) {
    kit::fill_rect(canvas, {at.x, at.y + 3, 7, 2}, ink);
    kit::fill_rect(canvas, {at.x + 1, at.y + 2, 5, 1}, ink);
    kit::fill_rect(canvas, {at.x + 2, at.y + 1, 3, 1}, ink);
    kit::fill_rect(canvas, {at.x + 4, at.y, 3, 1}, ink);
}

/// Draws an aircraft seen from above, its nose up, in a box 15 by 12
/// points from a corner.
///
/// @param canvas where it draws
/// @param at the box's top left corner
/// @param ink its colour
void draw_aircraft(const kit::Canvas& canvas, kit::Point at, kit::Colour ink) {
    kit::fill_rect(canvas, {at.x + 7, at.y, 1, 12}, ink);
    kit::fill_rect(canvas, {at.x + 6, at.y + 2, 3, 8}, ink);
    draw_line(canvas, {at.x + 6, at.y + 4}, {at.x, at.y + 7}, 1, ink);
    draw_line(canvas, {at.x + 8, at.y + 4}, {at.x + 14, at.y + 7}, 1, ink);
    kit::fill_rect(canvas, {at.x + 1, at.y + 6, 13, 1}, ink);
    kit::fill_rect(canvas, {at.x + 4, at.y + 11, 7, 1}, ink);
}

/// Draws a small aircraft, 7 by 6 points from a corner.
///
/// @param canvas where it draws
/// @param at the box's top left corner
/// @param ink its colour
void draw_small_aircraft(const kit::Canvas& canvas, kit::Point at, kit::Colour ink) {
    kit::fill_rect(canvas, {at.x + 3, at.y, 1, 6}, ink);
    kit::fill_rect(canvas, {at.x, at.y + 2, 7, 1}, ink);
    kit::fill_rect(canvas, {at.x + 1, at.y + 3, 5, 1}, ink);
    kit::fill_rect(canvas, {at.x + 2, at.y + 5, 3, 1}, ink);
}

/// Draws a command's pictogram centred in a box.
///
/// @param canvas where it draws
/// @param command which pictogram
/// @param box the box, in points
/// @param ink its colour
void draw_pictogram(const kit::Canvas& canvas, Command command, const kit::Rect& box, kit::Colour ink) {
    // Each pictogram is drawn in a 15 by 12 box, centred.
    const kit::Point at{box.x + (box.width - 15) / 2, box.y + (box.height - 12) / 2};
    switch (command) {
    case Command::all:
        for (const kit::Point corner :
             {kit::Point{2, 1}, kit::Point{9, 1}, kit::Point{2, 7}, kit::Point{9, 7}})
            kit::fill_rect(canvas, {at.x + corner.x, at.y + corner.y, 4, 4}, ink);
        return;
    case Command::commander: {
        // A five-pointed star, its points joined every second one.
        const std::array<kit::Point, 5> points{
            kit::Point{at.x + 7, at.y},
            kit::Point{at.x + 13, at.y + 4},
            kit::Point{at.x + 11, at.y + 11},
            kit::Point{at.x + 3, at.y + 11},
            kit::Point{at.x + 1, at.y + 4},
        };
        for (std::size_t point = 0; point < points.size(); ++point)
            draw_line(canvas, points[point], points[(point + 2) % points.size()], 1, ink);
        kit::fill_rect(canvas, {at.x + 5, at.y + 5, 5, 3}, ink);
        return;
    }
    case Command::air:
        draw_aircraft(canvas, at, ink);
        return;
    case Command::combat:
        draw_tank(canvas, {at.x, at.y + 1}, ink);
        return;
    case Command::builders:
        // A truck with a crane, its hook hanging from the jib's end.
        kit::fill_rect(canvas, {at.x + 1, at.y + 8, 10, 3}, ink);
        kit::fill_rect(canvas, {at.x + 1, at.y + 5, 4, 3}, ink);
        draw_line(canvas, {at.x + 6, at.y + 7}, {at.x + 13, at.y + 1}, 1, ink);
        kit::fill_rect(canvas, {at.x + 13, at.y + 1, 1, 5}, ink);
        kit::fill_rect(canvas, {at.x + 2, at.y + 11, 2, 1}, ink);
        kit::fill_rect(canvas, {at.x + 8, at.y + 11, 2, 1}, ink);
        return;
    case Command::factories:
        // A hall with a saw-tooth roof and a chimney.
        kit::fill_rect(canvas, {at.x + 1, at.y + 7, 13, 5}, ink);
        for (const int32_t tooth : {1, 5, 9}) {
            draw_line(canvas, {at.x + tooth, at.y + 7}, {at.x + tooth + 3, at.y + 4}, 1, ink);
            kit::fill_rect(canvas, {at.x + tooth + 3, at.y + 4, 1, 3}, ink);
        }
        kit::fill_rect(canvas, {at.x + 12, at.y + 1, 2, 6}, ink);
        kit::fill_rect(canvas, {at.x + 3, at.y + 9, 2, 3}, seam_colour);
        return;
    case Command::on_screen:
        // A dashed frame.
        for (int32_t step = 0; step < 15; step += 3) {
            kit::fill_rect(canvas, {at.x + step, at.y + 1, 2, 1}, ink);
            kit::fill_rect(canvas, {at.x + step, at.y + 11, 2, 1}, ink);
        }
        for (int32_t step = 1; step < 12; step += 3) {
            kit::fill_rect(canvas, {at.x, at.y + step, 1, 2}, ink);
            kit::fill_rect(canvas, {at.x + 14, at.y + step, 1, 2}, ink);
        }
        return;
    case Command::same_type:
        draw_small_tank(canvas, {at.x - 2, at.y + 5}, ink);
        kit::fill_rect(canvas, {at.x + 6, at.y + 6, 3, 1}, ink);
        kit::fill_rect(canvas, {at.x + 6, at.y + 8, 3, 1}, ink);
        draw_small_tank(canvas, {at.x + 10, at.y + 5}, ink);
        return;
    }
}

/// Returns a button's steel by what the pointer does with it.
///
/// @param index the button
/// @param pointer the pointer on the bar
/// @return its face's colour
kit::Colour face_of(int32_t index, const Pointer& pointer) {
    if (pointer.held == index && pointer.hovered == index)
        return held_colour;
    return pointer.hovered == index ? hovered_colour : button_colour;
}

/// Draws a group button.
///
/// @param canvas where it draws
/// @param rect the button
/// @param group which group, 0 to 8
/// @param facts what the bar shows
/// @param pointer the pointer on the bar
void draw_group(
    const kit::Canvas& canvas, const kit::Rect& rect, int32_t group, const Facts& facts,
    const Pointer& pointer
) {
    const bool lit = facts.lit_group == group;
    const bool hovered = pointer.hovered == group;
    const bool held = hovered && pointer.held == group;
    if (lit)
        draw_glow(canvas, rect, amber_colour, glow_reach);
    draw_steel(canvas, rect, lit && !held ? lit_colour : face_of(group, pointer));
    if (lit)
        kit::outline_rect(canvas, rect, amber_colour);
    else if (hovered)
        kit::outline_rect(canvas, rect, hover_edge_colour);
    const int32_t shift = held ? 1 : 0;
    const auto& shown = facts.groups[static_cast<std::size_t>(group)];
    const kit::Colour ink = lit       ? amber_colour
                            : hovered ? caption_lit_colour
                            : shown.units == 0 ? caption_faded_colour
                                               : caption_colour;
    outlined_text(
        canvas, kit::FontRole::regular, std::to_string(group + 1),
        {rect.x + shift, rect.y + 1 + shift, rect.width, number_height}, kit::Align::centre, ink
    );
    const kit::Rect line{rect.x + shift, rect.y + 1 + number_height + shift, rect.width, count_height};
    if (shown.units == 0) {
        kit::fill_rect(
            canvas, {line.x + line.width / 2 - 2, line.y + line.height / 2, 4, 1},
            caption_faded_colour
        );
        return;
    }
    const std::string count = std::to_string(shown.units);
    const int32_t count_width = small_width(canvas, count);
    // A small unit, then the count, centred together.
    constexpr int32_t unit_width = 7;
    const int32_t left = line.x + (line.width - unit_width - 1 - count_width) / 2;
    if (shown.aircraft)
        draw_small_aircraft(canvas, {left, line.y + 2}, ink);
    else
        draw_small_tank(canvas, {left, line.y + 3}, ink);
    outlined_text(
        canvas, kit::FontRole::small, count,
        {left + unit_width + 1, line.y, count_width + 2, line.height}, kit::Align::left, ink
    );
}

/// Draws a selection button.
///
/// @param canvas where it draws
/// @param rect the button
/// @param command which selection
/// @param facts what the bar shows
/// @param pointer the pointer on the bar
void draw_command(
    const kit::Canvas& canvas, const kit::Rect& rect, std::size_t command, const Facts& facts,
    const Pointer& pointer
) {
    const auto index = static_cast<int32_t>(group_count + command);
    const bool hovered = pointer.hovered == index;
    const bool held = hovered && pointer.held == index;
    draw_steel_button(canvas, rect, face_of(index, pointer));
    if (hovered)
        kit::outline_rect(canvas, rect, hover_edge_colour);
    const int32_t shift = held ? 1 : 0;
    const kit::Rect box{rect.x + shift, rect.y + shift, rect.width, rect.height};
    const kit::Colour ink = hovered ? caption_lit_colour : caption_colour;
    draw_pictogram(
        canvas, static_cast<Command>(command),
        {box.x, box.y + pictogram_top, box.width, pictogram_height}, ink
    );
    const kit::Measure small = [&canvas](std::string_view line) { return small_width(canvas, line); };
    const int32_t room = box.width - 2 * command_padding;
    outlined_text(
        canvas, kit::FontRole::small, kit::fit(worded(command_captions[command]), room, small),
        {box.x, box.y + caption_top, box.width, text_line}, kit::Align::centre, ink
    );
    if (!facts.keys[command].empty())
        outlined_text(
            canvas, kit::FontRole::small, kit::fit(facts.keys[command], room, small),
            {box.x, box.y + caption_top + text_line, box.width, text_line}, kit::Align::centre,
            hint_colour
        );
}

} // namespace

int32_t least_width(const kit::Canvas& canvas, const Facts& facts) {
    const int32_t groups = std::max(
        groups_width(), small_width(canvas, hint_line(facts)) + 2 * (rim + panel_padding)
    );
    return groups + panel_gap + commands_width(command_width(canvas, facts));
}

Layout lay_out(const kit::Canvas& canvas, const Facts& facts, int32_t width) {
    Layout laid{};
    const int32_t groups = std::max(
        groups_width(), small_width(canvas, hint_line(facts)) + 2 * (rim + panel_padding)
    );
    // The selection buttons share what the groups leave, each between its
    // least and its most.
    const int32_t room = width - groups - panel_gap - commands_width(0);
    const int32_t button = std::clamp(
        room / static_cast<int32_t>(command_count), command_width(canvas, facts),
        std::max(command_most_width, command_width(canvas, facts))
    );
    const int32_t commands = commands_width(button);
    laid.width = groups + panel_gap + commands;
    laid.groups = {0, plate_height / 2, groups, bar_height - plate_height / 2};
    laid.commands = {groups + panel_gap, plate_height / 2, commands, bar_height - plate_height / 2};
    const auto plate = [&canvas](const kit::Rect& panel, std::string_view text) {
        return kit::Rect{
            panel.x + plate_inset, 0,
            label_plate_width(canvas, worded(text), plate_height, plate_tracking), plate_height
        };
    };
    laid.groups_plate = plate(laid.groups, "GROUPS");
    laid.commands_plate = plate(laid.commands, "SELECT");
    // The group buttons centred across their panel, the hint's line under them.
    const int32_t group_row = static_cast<int32_t>(group_count) * group_width +
                              static_cast<int32_t>(group_count - 1) * button_gap;
    const int32_t group_left = laid.groups.x + (laid.groups.width - group_row) / 2;
    for (std::size_t group = 0; group < group_count; ++group)
        laid.buttons[group] = {
            group_left + static_cast<int32_t>(group) * (group_width + button_gap), buttons_top,
            group_width, group_height
        };
    laid.hint = {
        laid.groups.x + rim, buttons_top + group_height, laid.groups.width - 2 * rim, hint_height
    };
    const int32_t command_left = laid.commands.x + rim + panel_padding;
    for (std::size_t command = 0; command < command_count; ++command)
        laid.buttons[group_count + command] = {
            command_left + static_cast<int32_t>(command) * (button + button_gap), buttons_top,
            button, command_height
        };
    return laid;
}

int32_t button_at(const Layout& layout, kit::Point at) noexcept {
    for (std::size_t index = 0; index < button_count; ++index) {
        const kit::Rect& rect = layout.buttons[index];
        if (at.x >= rect.x && at.y >= rect.y && at.x < rect.x + rect.width &&
            at.y < rect.y + rect.height)
            return static_cast<int32_t>(index);
    }
    return no_button;
}

void draw(const kit::Canvas& canvas, const Layout& layout, const Facts& facts, const Pointer& pointer) {
    kit::fill_rect(
        canvas, {layout.groups.x + layout.groups.width, layout.groups.y, panel_gap, layout.groups.height},
        seam_colour
    );
    draw_panel_frame(canvas, layout.groups, rim);
    draw_panel_frame(canvas, layout.commands, rim);
    draw_label_plate(canvas, layout.groups_plate, worded("GROUPS"), plate_tracking);
    draw_label_plate(canvas, layout.commands_plate, worded("SELECT"), plate_tracking);
    for (std::size_t group = 0; group < group_count; ++group)
        draw_group(canvas, layout.buttons[group], static_cast<int32_t>(group), facts, pointer);
    outlined_text(
        canvas, kit::FontRole::small, hint_line(facts), layout.hint, kit::Align::centre, hint_colour
    );
    for (std::size_t command = 0; command < command_count; ++command)
        draw_command(canvas, layout.buttons[group_count + command], command, facts, pointer);
}

} // namespace oa::app::selection_bar
