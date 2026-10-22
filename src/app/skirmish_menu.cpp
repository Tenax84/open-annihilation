// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Open Annihilation's Skirmish screen on the OA layer (skirmish_menu.hpp).

#include "skirmish_menu.hpp"
#include "menu_paint.hpp"

#include "oa/ui/kit/canvas.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/text.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {

namespace {

namespace kit = oa::ui::kit;
using namespace menu_paint;

// The screen's measures, in rows of a window 1080 rows high, as the other
// menus' are.

/// The narrowest width, in the same rows, the measures keep their size in;
/// a narrower window shrinks them to fit its width.
constexpr double design_least_columns = 1340.0;
constexpr double content_columns = 1280.0;    ///< the panels' width together
constexpr double title_top = 18.0;            ///< the title's top
constexpr double title_columns = 420.0;       ///< the title's width
constexpr double heading_gap_rows = 4.0;      ///< between the title and the heading
constexpr double heading_rows = 40.0;         ///< the heading's height
constexpr double heading_columns = 820.0;     ///< the heading's width, its rules' reach
constexpr double heading_gap_columns = 18.0;  ///< between the heading and its rules
constexpr double panels_gap_rows = 12.0;      ///< between the heading and the panels
constexpr double panels_rows = 690.0;         ///< the PLAYERS panel's height
constexpr double players_columns = 846.0;     ///< the PLAYERS panel's width
constexpr double panel_gap_columns = 18.0;    ///< between the PLAYERS panel and those right of it
constexpr double map_rows = 404.0;            ///< the MAP panel's height
constexpr double panel_gap_rows = 12.0;       ///< between the MAP and GAME OPTIONS panels
constexpr double band_rows = 46.0;            ///< a panel's label band
constexpr double rim_columns = 11.0;          ///< a panel's steel rim's width
constexpr double label_left_columns = 10.0;   ///< from a panel's rim to its label plate
constexpr double label_top_rows = 5.0;        ///< from a panel's top to its label plate
constexpr double label_rows = 34.0;           ///< a label plate's height
constexpr double inset_columns = 20.0;        ///< inside a panel, round its content
constexpr double header_rows = 30.0;          ///< the column captions' height
constexpr double header_gap_rows = 6.0;       ///< between the captions and the rows
constexpr double help_rows = 40.0;            ///< the hint bar's height
constexpr double help_gap_rows = 10.0;        ///< between the rows and the hint bar
constexpr double cell_gap_columns = 4.0;      ///< between two cells of a row
constexpr double cell_gap_rows = 6.0;         ///< between two rows
constexpr double cell_pad_columns = 5.0;      ///< inside a cell, round its picture
constexpr double glow_columns = 6.0;          ///< how far a glow reaches
constexpr double bezel_columns = 10.0;        ///< the map's screen's steel bezel
constexpr double picture_rows = 222.0;        ///< the map's screen's height
constexpr double picture_gap_rows = 8.0;      ///< between the map's screen and its name
constexpr double map_name_rows = 28.0;        ///< the map's name's height
constexpr double map_facts_rows = 22.0;       ///< the line about the map's height
constexpr double map_button_gap_rows = 10.0;  ///< between that line and SELECT MAP
constexpr double map_button_rows = 46.0;      ///< SELECT MAP's height
constexpr double start_mark_columns = 11.0;   ///< a start position's mark's radius
constexpr double option_label_share = 0.36;   ///< the share of a rule's row its label takes
constexpr double option_gap_columns = 6.0;    ///< between a rule's arrows and its value
constexpr double option_button_rows = 38.0;   ///< a rule's arrows' side, and its row's height
constexpr double buttons_gap_rows = 18.0;     ///< between the panels and the buttons
constexpr double button_columns = 340.0;      ///< Previous Menu's and Start's width
constexpr double button_rows = 58.0;          ///< their height
constexpr double button_gap_columns = 36.0;   ///< between them
constexpr double back_arrow_columns = 28.0;   ///< from Previous Menu's left to its arrow
constexpr double plate_top = 1004.0;          ///< the version plate's top
constexpr double plate_rows = 36.0;           ///< the version plate's height
constexpr double plate_margin_columns = 30.0; ///< the plate's room either side of its text
/// How many rows the PLAYERS panel has room for, the most SKIRMISH.GUI has.
constexpr std::size_t row_room = 10;
/// The points a line of text needs over its font's size.
constexpr int32_t text_room = 8;
/// The extra points after each character of a label band.
constexpr int32_t band_tracking = 2;
/// The extra points after each character of a column caption.
constexpr int32_t caption_tracking = 1;

/// A row's fields, left to right after its number.
enum class Field : uint8_t { player, side, colour, team, metal, energy };
/// How many fields a row has.
constexpr std::size_t field_count = 6;
/// The SKIRMISH.GUI buttons of a slot's fields, before the slot's digit.
constexpr std::array<std::string_view, field_count> field_records{
    "Player", "Side", "Color", "Allies", "Metal", "Energy"
};
/// The fields' control names, for automation.
constexpr std::array<std::string_view, field_count> field_names{
    "player", "side", "color", "team", "metal", "energy"
};
/// The fields' column captions, in English.
constexpr std::array<std::string_view, field_count> field_captions{
    "PLAYER", "SIDE", "COLOR", "TEAM", "METAL", "ENERGY"
};
/// The rules' control names, for automation.
constexpr std::array<std::string_view, skirmish_menu_options> option_names{
    "commander", "location", "mapping", "line-of-sight", "difficulty"
};
/// The columns' widths, in rows of the 1080-row window: the slot's number,
/// then the fields. The PLAYERS panel's rows are cut to these shares.
constexpr std::array<double, field_count + 1> column_widths{42, 188, 160, 78, 78, 118, 118};

constexpr kit::Colour screen_colour{12, 15, 18};         ///< the map's screen, dark gunmetal
constexpr kit::Colour emblem_colour{184, 200, 214};      ///< a side's emblem, steel lit cold
constexpr kit::Colour empty_cell_colour{12, 15, 18};     ///< an open slot's empty field
constexpr kit::Colour cell_steel_colour{78, 84, 86};     ///< a field's face
constexpr kit::Colour number_steel_colour{52, 58, 60};   ///< a slot's number's face
constexpr kit::Colour open_steel_colour{50, 54, 56};     ///< an open slot's Player field
constexpr kit::Colour button_steel_colour{84, 90, 92};   ///< a small steel button's face
constexpr kit::Colour chosen_colour{232, 164, 52};       ///< round the local player's row
constexpr kit::Colour mark_colour{124, 88, 34};          ///< a start position's mark
constexpr kit::Colour hover_edge_colour{176, 184, 184};  ///< round what the pointer is over
constexpr kit::Colour help_colour{12, 15, 18};           ///< the hint bar's ground
/// The brightest channel at and under which an emblem's pixel stays the
/// field's ground.
constexpr uint32_t emblem_floor = 24;
/// The brightest channel at and over which an emblem's pixel takes its ink
/// whole.
constexpr uint32_t emblem_peak = 110;
/// The brightest channel over which an emblem's pixel is part of the logo
/// itself; the emblem is cut to these pixels.
constexpr uint32_t emblem_edge = 64;

/// What a press can reach.
enum class Part : uint8_t {
    none,         ///< nothing
    field,        ///< a slot's field: slot * field_count + field
    option_back,  ///< a rule's left arrow, by the rule
    option_on,    ///< a rule's right arrow, by the rule
    select_map,   ///< SELECT MAP
    back,         ///< Previous Menu
    start,        ///< Start
};

/// A part, and which of its kind.
struct Hit {
    Part part{Part::none}; ///< what it is
    std::size_t index{};   ///< which field or rule

    friend bool operator==(const Hit&, const Hit&) = default;
};

/// Where the screen's parts lie, in its points.
struct SkirmishLayout {
    kit::Rect background{};                          ///< the picture, cut by the screen's edges
    kit::Rect title{};                               ///< the title
    kit::Rect heading{};                             ///< the heading
    kit::Rect players{};                             ///< the PLAYERS panel
    kit::Rect header{};                              ///< the column captions
    std::array<int32_t, field_count + 2> columns{};  ///< the columns' left edges, and the right end
    int32_t rows_top{};                              ///< the first row's top
    int32_t row_pitch{};                             ///< from one row's top to the next's
    int32_t row_height{};                            ///< a row's height
    kit::Rect help{};                                ///< the hint bar
    kit::Rect map{};                                 ///< the MAP panel
    kit::Rect monitor{};                             ///< the map's screen, its bezel included
    kit::Rect map_name{};                            ///< the map's name
    kit::Rect map_facts{};                           ///< the line about the map
    kit::Rect select_map{};                          ///< SELECT MAP
    kit::Rect options{};                             ///< the GAME OPTIONS panel
    std::array<kit::Rect, skirmish_menu_options> option_rows{}; ///< each rule's row
    int32_t option_label{};                          ///< a rule's label's width
    kit::Rect back{};                                ///< Previous Menu
    kit::Rect start{};                               ///< Start
    int32_t inset{};                                 ///< inside a panel, round its content
    int32_t rim{};                                   ///< a panel's steel rim's width
    int32_t label_left{};                            ///< from a panel's rim to its label plate
    int32_t label_top{};                             ///< from a panel's top to its label plate
    int32_t label_height{};                          ///< a label plate's height
    int32_t cell_gap{};                              ///< between two cells of a row
    int32_t cell_pad{};                              ///< inside a cell, round its picture
    int32_t option_gap{};                            ///< between a rule's arrows and its value
    int32_t glow{};                                  ///< how far a glow reaches
    int32_t bezel{};                                 ///< the map's screen's bezel
    int32_t mark{};                                  ///< a start position's mark's radius
    int32_t back_arrow{};                            ///< from Previous Menu's left to its arrow
    int32_t heading_gap{};                           ///< between the heading and its rules
    int32_t plate_top{};                             ///< the version plate's top
    int32_t plate_height{};                          ///< the version plate's height
    int32_t plate_margin{};                          ///< its room either side of the text
};

/// Lays the screen out on a screen of a size.
///
/// @param width the screen's width, in points
/// @param height the screen's height, in points
/// @param title_size the title picture's width and height, in pixels
/// @return where its parts lie
SkirmishLayout lay_out_skirmish_menu(int32_t width, int32_t height, kit::Point title_size) {
    SkirmishLayout placed;
    const double columns = std::max(width, 1);
    const double rows = std::max(height, 1);
    // A point is this many rows of the 1080-row window; a window narrower
    // than design_least_columns of them takes a smaller one, centred down.
    const double unit = std::min(rows / design_rows, columns / design_least_columns);
    const double top = (rows - design_rows * unit) / 2.0;
    const double middle = columns / 2.0;
    const auto across = [&](double design) { return nearest(design * unit); };
    const auto down = [&](double design) { return nearest(top + design * unit); };
    const int32_t regular_line = kit::smooth_text_points(kit::FontRole::regular) + text_room;
    const int32_t small_line = kit::smooth_text_points(kit::FontRole::small) + text_room;

    placed.background = background_place(width, height);
    const double title_height = title_size.x > 0 ? title_columns * static_cast<double>(title_size.y) /
                                                       static_cast<double>(title_size.x)
                                                 : 0.0;
    placed.title = {
        nearest(middle - title_columns * unit / 2.0),
        down(title_top),
        across(title_columns),
        across(title_height)
    };
    const double heading_top = title_top + title_height + heading_gap_rows;
    placed.heading = {
        nearest(middle - heading_columns * unit / 2.0),
        down(heading_top),
        across(heading_columns),
        across(heading_rows)
    };
    placed.inset = across(inset_columns);
    placed.rim = across(rim_columns);
    placed.label_left = across(label_left_columns);
    placed.label_top = across(label_top_rows);
    placed.label_height = std::max(across(label_rows), small_line);
    placed.cell_gap = std::max(across(cell_gap_columns), 1);
    placed.cell_pad = std::max(across(cell_pad_columns), 2);
    placed.option_gap = std::max(across(option_gap_columns), 2);
    placed.glow = std::max(across(glow_columns), 2);
    placed.bezel = std::max(across(bezel_columns), 3);
    placed.mark = std::max(across(start_mark_columns), 6);
    placed.back_arrow = across(back_arrow_columns);
    placed.heading_gap = across(heading_gap_columns);
    const int32_t band = std::max(across(band_rows), placed.label_top + placed.label_height + 2);

    const int32_t left = nearest(middle - content_columns * unit / 2.0);
    const int32_t content = across(content_columns);
    const double panels_top = heading_top + heading_rows + panels_gap_rows;
    placed.players = {left, down(panels_top), across(players_columns), across(panels_rows)};
    const kit::Rect& players = placed.players;
    const int32_t inner_left = players.x + placed.inset;
    const int32_t inner_width = players.width - 2 * placed.inset;
    placed.header = {
        inner_left, players.y + band, inner_width, std::max(across(header_rows), small_line)
    };
    double total = 0.0;
    for (const double share : column_widths)
        total += share;
    double reached = 0.0;
    for (std::size_t column = 0; column < column_widths.size(); ++column) {
        placed.columns[column] = inner_left + nearest(inner_width * reached / total);
        reached += column_widths[column];
    }
    placed.columns.back() = inner_left + inner_width;
    placed.help = {
        inner_left,
        players.y + players.height - placed.inset - std::max(across(help_rows), small_line + 4),
        inner_width,
        std::max(across(help_rows), small_line + 4)
    };
    placed.rows_top = placed.header.y + placed.header.height + across(header_gap_rows);
    const int32_t rows_room = placed.help.y - across(help_gap_rows) - placed.rows_top;
    placed.row_pitch = std::max(rows_room / static_cast<int32_t>(row_room), 1);
    placed.row_height = std::max(placed.row_pitch - across(cell_gap_rows), 1);

    const int32_t right_left = players.x + players.width + across(panel_gap_columns);
    const int32_t right_width = left + content - right_left;
    placed.map = {right_left, players.y, right_width, across(map_rows)};
    const int32_t map_inner = right_width - 2 * placed.inset;
    int32_t row = placed.map.y + band;
    placed.monitor = {right_left + placed.inset, row, map_inner, across(picture_rows)};
    row += placed.monitor.height + across(picture_gap_rows);
    placed.map_name = {
        right_left + placed.inset, row, map_inner, std::max(across(map_name_rows), regular_line)
    };
    row += placed.map_name.height;
    placed.map_facts = {
        right_left + placed.inset, row, map_inner, std::max(across(map_facts_rows), small_line)
    };
    row += placed.map_facts.height + across(map_button_gap_rows);
    placed.select_map = {
        right_left + placed.inset, row, map_inner, std::max(across(map_button_rows), regular_line)
    };

    const int32_t options_top = placed.map.y + placed.map.height + across(panel_gap_rows);
    placed.options = {
        right_left, options_top, right_width, players.y + players.height - options_top
    };
    const int32_t options_rows_top = placed.options.y + band;
    const int32_t options_room =
        placed.options.y + placed.options.height - placed.inset / 2 - options_rows_top;
    const int32_t option_height = std::max(across(option_button_rows), regular_line);
    const int32_t option_pitch = std::max(
        options_room / static_cast<int32_t>(skirmish_menu_options), option_height
    );
    // The rows spread over the panel, each centred in its share of it.
    const int32_t option_offset = (option_pitch - option_height) / 2;
    for (std::size_t option = 0; option < skirmish_menu_options; ++option)
        placed.option_rows[option] = {
            right_left + placed.inset,
            options_rows_top + static_cast<int32_t>(option) * option_pitch + option_offset,
            map_inner,
            option_height
        };
    placed.option_label = nearest(map_inner * option_label_share);

    const double buttons_top = panels_top + panels_rows + buttons_gap_rows;
    const int32_t button_width = across(button_columns);
    const int32_t button_height = across(button_rows);
    const int32_t button_gap = across(button_gap_columns);
    const int32_t buttons_left = nearest(middle) - button_width - button_gap / 2;
    placed.back = {buttons_left, down(buttons_top), button_width, button_height};
    placed.start = {
        buttons_left + button_width + button_gap, placed.back.y, button_width, button_height
    };
    placed.plate_top = down(plate_top);
    placed.plate_height = std::max(across(plate_rows), 1);
    placed.plate_margin = across(plate_margin_columns);
    return placed;
}

/// The Skirmish screen as a screen of the OA layer.
///
/// Its points are the layer's: one point is the view's scale in window
/// pixels, from the room's top left corner, and the screen covers the room.
class SkirmishMenuScreen final : public LayerScreen {
  public:

    /// Makes the screen.
    ///
    /// @param shows tells whether the screen shows
    /// @param facts reads what SKIRMISH.GUI and the setup show now
    /// @param actions what its presses do
    SkirmishMenuScreen(
        std::function<bool()> shows,
        std::function<SkirmishMenuFacts()> facts,
        SkirmishMenuActions actions
    )
        : shows_(std::move(shows)), facts_(std::move(facts)), actions_(std::move(actions)) {
        facts_now_ = facts_();
    }

    /// Returns the screen's name.
    ///
    /// @return skirmish_menu_screen_name
    [[nodiscard]] std::string_view name() const override { return skirmish_menu_screen_name; }

    /// Returns where the screen shows: the whole room, at the view's scale;
    /// nowhere in a match.
    ///
    /// @param placed_on what the screen is placed on
    /// @return its place
    [[nodiscard]] LayerPlacement placement(const LayerView& placed_on) const override {
        if (placed_on.match || placed_on.room.width <= 0 || placed_on.room.height <= 0)
            return {};
        const int32_t percent = std::max(placed_on.scale_percent, int32_t{100});
        const kit::Point size = points_of(placed_on);
        const kit::Rect at = kit::to_canvas({0, 0, size.x, size.y}, percent);
        return {{placed_on.room.x, placed_on.room.y, at.width, at.height}, size.x, size.y, percent};
    }

    /// Tells that the screen lets the screens over it take what they show.
    ///
    /// @return false
    [[nodiscard]] bool modal() const override { return false; }

    /// Tells that the screen darkens nothing.
    ///
    /// @return false
    [[nodiscard]] bool backdrop() const override { return false; }

    /// Draws the background, the title, the heading, the panels, the
    /// buttons and the version plate.
    ///
    /// @param canvas where it draws, at the layer's scale
    void draw(const kit::Canvas& canvas) const override {
        const MenuArt& art = menu_art();
        draw_backdrop(canvas, size_, layout_.background);
        fitted_.draw(canvas, layout_.title, art.title);
        draw_ruled_heading(canvas, layout_.heading, worded("SKIRMISH"), layout_.heading_gap);
        draw_players(canvas);
        draw_map(canvas);
        draw_options(canvas);
        draw_button(canvas, Part::back, art);
        draw_button(canvas, Part::start, art);
        draw_version_plate(
            canvas,
            size_.x,
            layout_.plate_top,
            layout_.plate_height,
            layout_.plate_margin,
            facts_now_.version
        );
    }

    /// Follows the view: the room's size in the layer's points.
    ///
    /// @param placed_on what the screen is placed on
    void lay_out(const LayerView& placed_on) override {
        if (placed_on.match)
            return;
        const kit::Point size = points_of(placed_on);
        if (size.x == size_.x && size.y == size_.y && laid_out_)
            return;
        size_ = size;
        const MenuArt& art = menu_art();
        layout_ = lay_out_skirmish_menu(
            size_.x,
            size_.y,
            {static_cast<int32_t>(art.title.width), static_cast<int32_t>(art.title.height)}
        );
        laid_out_ = true;
        fitted_.clear();
        emblem_fitted_.clear();
        swatch_fitted_.clear();
        map_fitted_.clear();
        ++revision_;
    }

    /// Follows the pointer over the parts and presses the one a press is
    /// released on, with the button it was made with. Takes every pointer,
    /// so that SKIRMISH.GUI under it sees none.
    ///
    /// @param kind pointer_move, pointer_down or pointer_up
    /// @param button the pointer's button, 1 for the left
    /// @param at the pointer, in the screen's points
    /// @return redraw when the look changed; none otherwise
    LayerAnswer pointer(
        ScreenInputKind kind, uint8_t button, kit::Point at, int32_t /*finger_reach*/
    ) override {
        const uint64_t before = revision_;
        const Hit over = hit_at(at);
        set_hovered(over);
        const bool usable_button = button == SDL_BUTTON_LEFT || button == SDL_BUTTON_RIGHT;
        if (kind == ScreenInputKind::pointer_down && usable_button) {
            set_pressed(over);
            pressed_button_ = button;
        } else if (kind == ScreenInputKind::pointer_up && usable_button &&
                   button == pressed_button_) {
            const Hit held = pressed_;
            set_pressed({});
            if (held.part != Part::none && held == over) {
                press(held, button == SDL_BUTTON_RIGHT ? 2 : 1);
                return LayerAnswer::redraw;
            }
        }
        return revision_ != before ? LayerAnswer::redraw : LayerAnswer::none;
    }

    /// Starts with Return and Space; lets every other key go on.
    ///
    /// @param pressed the key
    /// @return redraw when it acted; pass otherwise
    LayerAnswer key(kit::Key pressed, uint32_t /*sdl_key*/) override {
        if (pressed != kit::Key::enter && pressed != kit::Key::space)
            return LayerAnswer::pass;
        press({Part::start, 0}, 1);
        return LayerAnswer::redraw;
    }

    /// Takes no wheel.
    ///
    /// @return none
    LayerAnswer wheel(kit::Point /*at*/, float /*notches*/) override { return LayerAnswer::none; }

    /// Returns the parts a press can reach, one named control each, for
    /// automation: each slot's fields as player-1, side-1 and on, each rule's
    /// arrows, Select Map, Previous Menu and Start.
    ///
    /// @return the list
    [[nodiscard]] kit::DisplayList display_list() const override {
        kit::DisplayList list;
        const auto add = [&](Hit hit, const kit::Rect& rect, std::string name, std::string text,
                             bool checked) {
            kit::Control control;
            control.id = control_id(hit);
            control.rect = rect;
            control.enabled = enabled(hit);
            control.name = std::string(skirmish_menu_screen_name) + "." + std::move(name);
            control.kind = kit::ControlKind::button;
            control.text = std::move(text);
            control.checked = checked;
            list.controls.push_back(std::move(control));
            list.tab_order.push_back(control_id(hit));
        };
        for (std::size_t slot = 0; slot < shown_slots(); ++slot) {
            const SkirmishMenuSlot& shown = facts_now_.slots[slot];
            for (std::size_t field = 0; field < field_count; ++field) {
                if (shown.open && field != 0)
                    break;
                add({Part::field, slot * field_count + field},
                    cell_rect(slot, field + 1),
                    std::string(field_names[field]) + "-" + std::to_string(slot + 1),
                    field_text(shown, static_cast<Field>(field)),
                    shown.local);
            }
        }
        for (std::size_t option = 0; option < skirmish_menu_options; ++option) {
            const auto& shown = facts_now_.options[option];
            add({Part::option_back, option}, arrow_rect(option, true),
                std::string(option_names[option]) + "-previous", shown.value, false);
            add({Part::option_on, option}, arrow_rect(option, false),
                std::string(option_names[option]) + "-next", shown.value, false);
        }
        add({Part::select_map, 0}, layout_.select_map, "select-map", worded("SELECT MAP"), false);
        add({Part::back, 0}, layout_.back, "previous-menu", worded("PREVIOUS MENU"), false);
        add({Part::start, 0}, layout_.start, "start", worded("START"), false);
        return list;
    }

    /// Returns the hovered and pressed parts; Start holds the focus, as
    /// Return presses it.
    ///
    /// @return the interaction
    [[nodiscard]] kit::Interaction interaction() const override {
        kit::Interaction shown;
        shown.hovered = hovered_.part != Part::none ? control_id(hovered_) : kit::no_control;
        shown.pressed = pressed_.part != Part::none ? control_id(pressed_) : kit::no_control;
        shown.focused = control_id({Part::start, 0});
        return shown;
    }

    /// Closes the screen when it no longer shows, and follows what
    /// SKIRMISH.GUI and the setup show.
    ///
    /// @return close when the screen no longer shows; redraw when what it
    ///     shows changed; otherwise none
    LayerAnswer tick() override {
        if (!shows_())
            return LayerAnswer::close;
        SkirmishMenuFacts now = facts_();
        if (now == facts_now_)
            return LayerAnswer::none;
        if (now.map_record != facts_now_.map_record) {
            map_picture_.reset();
            map_fitted_.clear();
        }
        facts_now_ = std::move(now);
        if (!enabled(hovered_))
            hovered_ = {};
        ++revision_;
        return LayerAnswer::redraw;
    }

    /// Returns a number that changes with what the screen draws.
    ///
    /// @return the revision
    [[nodiscard]] uint64_t revision() const override { return revision_; }

    /// Tells that the screen draws nothing that moves on its own.
    ///
    /// @return true
    [[nodiscard]] bool revision_covers_drawing() const override { return true; }

    /// Has nothing of its own to close.
    void close(bool /*by_key*/) override {}

  private:

    /// Returns the room's size in the view's points, rounded up, so that the
    /// screen covers every pixel of the room.
    ///
    /// @param placed_on what the screen is placed on
    /// @return the width and height, in points
    [[nodiscard]] static kit::Point points_of(const LayerView& placed_on) noexcept {
        const int64_t percent = std::max(placed_on.scale_percent, int32_t{100});
        const auto up = [percent](int32_t pixels) {
            return static_cast<int32_t>(
                (static_cast<int64_t>(pixels) * 100 + percent - 1) / percent
            );
        };
        return {up(placed_on.room.width), up(placed_on.room.height)};
    }

    /// Returns a part's control number.
    ///
    /// @param hit the part
    /// @return its number
    [[nodiscard]] static kit::ControlId control_id(Hit hit) noexcept {
        // Each kind has a thousand numbers, more than any it has.
        return static_cast<kit::ControlId>(static_cast<int32_t>(hit.part) * 1000) +
               static_cast<kit::ControlId>(std::min<std::size_t>(hit.index, 999));
    }

    /// Returns the text a field shows.
    ///
    /// @param slot the slot
    /// @param field the field
    /// @return the text
    [[nodiscard]] static std::string field_text(const SkirmishMenuSlot& slot, Field field) {
        switch (field) {
        case Field::player:
            return slot.player;
        case Field::side:
            return slot.side_name;
        case Field::colour:
            return std::to_string(slot.colour + 1);
        case Field::team:
            return slot.team > 0 ? std::to_string(slot.team) : std::string("-");
        case Field::metal:
            return slot.metal;
        case Field::energy:
            return slot.energy;
        }
        return {};
    }

    /// Returns how many slots the panel shows.
    ///
    /// @return the count, at most row_room
    [[nodiscard]] std::size_t shown_slots() const noexcept {
        return std::min(facts_now_.slots.size(), row_room);
    }

    /// Returns a cell of a slot's row.
    ///
    /// @param slot the slot
    /// @param column 0 for its number, then its fields from 1
    /// @return the cell
    [[nodiscard]] kit::Rect cell_rect(std::size_t slot, std::size_t column) const noexcept {
        const int32_t left = layout_.columns[column];
        const int32_t right = layout_.columns[column + 1] - layout_.cell_gap;
        return {
            left,
            layout_.rows_top + static_cast<int32_t>(slot) * layout_.row_pitch,
            std::max(right - left, 1),
            layout_.row_height
        };
    }

    /// Returns a slot's whole row.
    ///
    /// @param slot the slot
    /// @return the row
    [[nodiscard]] kit::Rect row_rect(std::size_t slot) const noexcept {
        const kit::Rect first = cell_rect(slot, 0);
        return {
            first.x,
            first.y,
            layout_.columns.back() - layout_.cell_gap - first.x,
            first.height
        };
    }

    /// Returns one of a rule's arrows.
    ///
    /// @param option the rule
    /// @param back the left arrow; otherwise the right
    /// @return the arrow
    [[nodiscard]] kit::Rect arrow_rect(std::size_t option, bool back) const noexcept {
        const kit::Rect& row = layout_.option_rows[option];
        const int32_t side = row.height;
        return {
            back ? row.x + layout_.option_label : row.x + row.width - side, row.y, side, side
        };
    }

    /// Returns where a rule's value shows, between its arrows.
    ///
    /// @param option the rule
    /// @return the value's plate
    [[nodiscard]] kit::Rect value_rect(std::size_t option) const noexcept {
        const kit::Rect back = arrow_rect(option, true);
        const kit::Rect on = arrow_rect(option, false);
        const int32_t left = back.x + back.width + layout_.option_gap;
        return {left, back.y, std::max(on.x - layout_.option_gap - left, 1), back.height};
    }

    /// Returns the part under a point.
    ///
    /// @param at the point, in the screen's points
    /// @return the part; none when nothing a press reaches is there
    [[nodiscard]] Hit hit_at(kit::Point at) const noexcept {
        for (std::size_t slot = 0; slot < shown_slots(); ++slot) {
            if (!kit::contains(row_rect(slot), at))
                continue;
            for (std::size_t field = 0; field < field_count; ++field)
                if (kit::contains(cell_rect(slot, field + 1), at)) {
                    const Hit hit{Part::field, slot * field_count + field};
                    return enabled(hit) ? hit : Hit{};
                }
            return {};
        }
        for (std::size_t option = 0; option < skirmish_menu_options; ++option) {
            for (const bool back : {true, false})
                if (kit::contains(arrow_rect(option, back), at)) {
                    const Hit hit{back ? Part::option_back : Part::option_on, option};
                    return enabled(hit) ? hit : Hit{};
                }
        }
        if (kit::contains(layout_.select_map, at))
            return enabled({Part::select_map, 0}) ? Hit{Part::select_map, 0} : Hit{};
        if (kit::contains(layout_.back, at))
            return {Part::back, 0};
        if (kit::contains(layout_.start, at))
            return enabled({Part::start, 0}) ? Hit{Part::start, 0} : Hit{};
        return {};
    }

    /// Tells whether a part takes a press.
    ///
    /// @param hit the part
    /// @return true when it does
    [[nodiscard]] bool enabled(Hit hit) const noexcept {
        switch (hit.part) {
        case Part::none:
            return false;
        case Part::field: {
            const std::size_t slot = hit.index / field_count;
            if (slot >= shown_slots())
                return false;
            return hit.index % field_count == 0 || !facts_now_.slots[slot].open;
        }
        case Part::option_back:
        case Part::option_on:
            return hit.index < skirmish_menu_options && facts_now_.options[hit.index].stages > 0;
        case Part::select_map:
            return facts_now_.can_select_map;
        case Part::start:
            return facts_now_.can_start;
        case Part::back:
            return true;
        }
        return false;
    }

    /// Presses a part that takes a press.
    ///
    /// @param hit the part
    /// @param mouse_button 1 left or 2 right
    void press(Hit hit, int32_t mouse_button) {
        if (!enabled(hit) || !actions_.press)
            return;
        switch (hit.part) {
        case Part::field: {
            const std::size_t field = hit.index % field_count;
            const auto kind = static_cast<Field>(field);
            // Only the colour, the metal and the energy tell the buttons
            // apart; a right press on another field presses nothing.
            if (mouse_button == 2 && kind != Field::colour && kind != Field::metal &&
                kind != Field::energy)
                return;
            actions_.press(
                std::string(field_records[field]) + std::to_string(hit.index / field_count),
                mouse_button
            );
            break;
        }
        case Part::option_back:
        case Part::option_on: {
            if (mouse_button != 1)
                return;
            const SkirmishMenuOption& shown = facts_now_.options[hit.index];
            // A rule's button only steps on; round to the stage before is
            // one press fewer than its stages.
            const uint32_t presses = hit.part == Part::option_on ? 1U : shown.stages - 1U;
            for (uint32_t press = 0; press < presses; ++press)
                actions_.press(shown.record, 1);
            break;
        }
        case Part::select_map:
            if (mouse_button == 1)
                actions_.press("SelectMap", 1);
            break;
        case Part::back:
            if (mouse_button == 1)
                actions_.press("PrevMenu", 1);
            break;
        case Part::start:
            if (mouse_button == 1)
                actions_.press("Start", 1);
            break;
        case Part::none:
            break;
        }
        ++revision_;
    }

    /// Sets the part under the pointer, counting a change.
    ///
    /// @param hit the part
    void set_hovered(Hit hit) {
        if (hit == hovered_)
            return;
        hovered_ = hit;
        ++revision_;
    }

    /// Sets the part a press holds, counting a change.
    ///
    /// @param hit the part
    void set_pressed(Hit hit) {
        if (hit == pressed_)
            return;
        pressed_ = hit;
        ++revision_;
    }

    /// Returns a side's emblem lit in steel, made the first time it is drawn.
    ///
    /// @param side the side
    /// @return the emblem; empty for none
    [[nodiscard]] const WindowIcon& emblem(std::size_t side) const {
        auto& kept = emblems_[side];
        if (!kept) {
            const WindowIcon picture =
                trimmed_picture(actions_.emblem ? actions_.emblem(side) : WindowIcon{}, emblem_edge);
            kept = std::make_unique<WindowIcon>(
                tinted_picture(picture, screen_colour, emblem_colour, emblem_floor, emblem_peak)
            );
        }
        return *kept;
    }

    /// Returns a colour's swatch, made the first time it is drawn.
    ///
    /// @param colour the colour
    /// @return the swatch; empty for none
    [[nodiscard]] const WindowIcon& swatch(int32_t colour) const {
        auto& kept = swatches_[colour];
        if (!kept)
            kept = std::make_unique<WindowIcon>(
                actions_.swatch ? actions_.swatch(colour) : WindowIcon{}
            );
        return *kept;
    }

    /// Returns the map's picture, made the first time it is drawn for the
    /// map.
    ///
    /// @return the picture; empty for none
    [[nodiscard]] const WindowIcon& map_picture() const {
        if (!map_picture_)
            map_picture_ = std::make_unique<WindowIcon>(
                actions_.map_picture ? actions_.map_picture() : WindowIcon{}
            );
        return *map_picture_;
    }

    /// Draws a picture fitted inside a box, its aspect kept, centred.
    ///
    /// @param canvas where it draws
    /// @param box the box
    /// @param picture the picture
    /// @param fitted the copies it is drawn from
    /// @return where it was drawn; empty when it was not
    static kit::Rect draw_fitted(
        const kit::Canvas& canvas, const kit::Rect& box, const WindowIcon& picture,
        const FittedArt& fitted
    ) {
        if (picture.width == 0 || picture.height == 0 || box.width <= 0 || box.height <= 0)
            return {};
        const double scale = std::min(
            static_cast<double>(box.width) / picture.width,
            static_cast<double>(box.height) / picture.height
        );
        const int32_t width = std::max(nearest(picture.width * scale), 1);
        const int32_t height = std::max(nearest(picture.height * scale), 1);
        const kit::Rect at{
            box.x + (box.width - width) / 2, box.y + (box.height - height) / 2, width, height
        };
        fitted.draw(canvas, at, picture);
        return at;
    }

    /// Draws a panel's label on a steel plate set into its frame at its top
    /// left.
    ///
    /// @param canvas where it draws
    /// @param panel the panel
    /// @param text the label
    void draw_label(const kit::Canvas& canvas, const kit::Rect& panel, std::string_view text) const {
        const int32_t left = panel.x + layout_.rim + layout_.label_left;
        const int32_t reach = panel.x + panel.width - layout_.rim - layout_.inset;
        const int32_t width = std::min(
            label_plate_width(canvas, text, layout_.label_height, band_tracking), reach - left
        );
        draw_label_plate(
            canvas,
            {left, panel.y + layout_.label_top, width, layout_.label_height},
            text,
            band_tracking
        );
    }

    /// Draws the PLAYERS panel: its label, the column captions, a row for
    /// every slot and the hint bar.
    ///
    /// @param canvas where it draws
    void draw_players(const kit::Canvas& canvas) const {
        draw_panel_frame(canvas, layout_.players, layout_.rim);
        draw_label(canvas, layout_.players, worded("PLAYERS"));
        const kit::Measure small = [&canvas](std::string_view line) {
            return kit::measure_text(canvas, kit::FontRole::small, line);
        };
        for (std::size_t column = 0; column <= field_count; ++column) {
            const kit::Rect cell{
                layout_.columns[column],
                layout_.header.y,
                layout_.columns[column + 1] - layout_.cell_gap - layout_.columns[column],
                layout_.header.height
            };
            draw_steel(canvas, cell, number_steel_colour);
            if (column == 0)
                continue;
            kit::box_text(
                canvas,
                kit::FontRole::small,
                kit::fit(worded(field_captions[column - 1]), cell.width - 4, small),
                cell,
                kit::Align::centre,
                caption_colour,
                caption_tracking
            );
        }
        for (std::size_t slot = 0; slot < shown_slots(); ++slot)
            draw_row(canvas, slot);
        for (std::size_t slot = shown_slots(); slot < row_room; ++slot)
            draw_sunk_cells(canvas, slot, 0);
        const kit::Rect& bar = layout_.help;
        kit::fill_rect(canvas, bar, help_colour);
        draw_sunk_edge(canvas, bar);
        kit::box_text(
            canvas,
            kit::FontRole::small,
            kit::fit(hint(), bar.width - 2 * layout_.inset, small),
            bar,
            kit::Align::centre,
            plate_text_colour
        );
    }

    /// Draws a row's cells from one on as empty: dark, sunk into the panel.
    ///
    /// @param canvas where it draws
    /// @param slot the slot
    /// @param from the first column drawn so
    void draw_sunk_cells(const kit::Canvas& canvas, std::size_t slot, std::size_t from) const {
        for (std::size_t column = from; column <= field_count; ++column) {
            const kit::Rect cell = cell_rect(slot, column);
            kit::fill_rect(canvas, cell, empty_cell_colour);
            draw_sunk_edge(canvas, cell);
        }
    }

    /// Draws a slot's row: its number and its fields, or for an open slot
    /// its Player field alone, and an amber ring round the local player's.
    ///
    /// @param canvas where it draws
    /// @param slot the slot
    void draw_row(const kit::Canvas& canvas, std::size_t slot) const {
        const SkirmishMenuSlot& shown = facts_now_.slots[slot];
        if (shown.local)
            draw_glow(canvas, row_rect(slot), chosen_colour, layout_.glow);
        const kit::Rect number = cell_rect(slot, 0);
        draw_steel(canvas, number, number_steel_colour);
        outlined_text(
            canvas,
            kit::FontRole::regular,
            std::to_string(slot + 1),
            number,
            kit::Align::centre,
            shown.open ? caption_faded_colour : caption_colour
        );
        const kit::Measure regular = [&canvas](std::string_view line) {
            return kit::measure_text(canvas, kit::FontRole::regular, line);
        };
        for (std::size_t field = 0; field < field_count; ++field) {
            if (shown.open && field > 0) {
                draw_sunk_cells(canvas, slot, field + 1);
                break;
            }
            const Hit hit{Part::field, slot * field_count + field};
            const kit::Rect cell = cell_rect(slot, field + 1);
            const bool lit = hovered_ == hit;
            const bool held = lit && pressed_ == hit;
            const auto kind = static_cast<Field>(field);
            draw_steel_button(
                canvas,
                cell,
                kind == Field::player && shown.open ? open_steel_colour : cell_steel_colour
            );
            kit::Rect box = cell;
            if (held) {
                box.x += 1;
                box.y += 1;
            }
            const kit::Colour ink = shown.open ? caption_faded_colour
                                    : lit      ? caption_lit_colour
                                               : caption_colour;
            const int32_t pad = layout_.cell_pad;
            if (kind == Field::colour) {
                const kit::Rect inner{
                    box.x + pad, box.y + pad, box.width - 2 * pad, box.height - 2 * pad
                };
                const WindowIcon& picture = swatch(shown.colour);
                if (picture.width > 0 && inner.width > 4 && inner.height > 4) {
                    const int32_t side = std::min(inner.width, inner.height);
                    const kit::Rect square{
                        inner.x + (inner.width - side) / 2, inner.y, side, side
                    };
                    swatch_fitted_.draw(canvas, square, picture);
                    kit::bevel_rect(canvas, square, steel_light_colour, steel_dark_colour);
                } else {
                    outlined_text(
                        canvas, kit::FontRole::regular, field_text(shown, kind), box,
                        kit::Align::centre, ink
                    );
                }
            } else if (kind == Field::side) {
                const int32_t side = box.height - 2 * pad;
                const WindowIcon& picture = emblem(shown.side);
                kit::Rect text_box = box;
                if (picture.width > 0 && side > 4) {
                    // The emblem lies on a small dark screen sunk into the
                    // field, as the side tiles of New Campaign show theirs.
                    const kit::Rect screen{box.x + pad * 2, box.y + pad, side, side};
                    kit::fill_rect(canvas, screen, screen_colour);
                    draw_fitted(
                        canvas,
                        {screen.x + 2, screen.y + 2, screen.width - 4, screen.height - 4},
                        picture,
                        emblem_fitted_
                    );
                    draw_sunk_edge(canvas, screen);
                    const int32_t after = screen.x + screen.width;
                    text_box = {after, box.y, box.x + box.width - after, box.height};
                }
                outlined_text(
                    canvas,
                    kit::FontRole::regular,
                    kit::fit(shown.side_name, text_box.width - 2 * pad, regular),
                    text_box,
                    kit::Align::centre,
                    ink
                );
            } else if (kind == Field::player) {
                outlined_text(
                    canvas,
                    kit::FontRole::regular,
                    kit::fit(shown.player, box.width - 4 * pad, regular),
                    {box.x + 3 * pad, box.y, box.width - 4 * pad, box.height},
                    kit::Align::left,
                    ink
                );
            } else {
                outlined_text(
                    canvas,
                    kit::FontRole::regular,
                    kit::fit(field_text(shown, kind), box.width - 2 * pad, regular),
                    box,
                    kit::Align::centre,
                    ink
                );
            }
            if (lit)
                kit::outline_rect(canvas, cell, hover_edge_colour);
        }
        if (shown.local)
            draw_ring(canvas, row_rect(slot), 2, chosen_colour);
    }

    /// Returns what the hint bar says: what the part under the pointer
    /// does, or how a computer player is added.
    ///
    /// @return the hint, in the language shown
    [[nodiscard]] std::string hint() const {
        const std::string fallback = worded("Click a slot to add a computer player.");
        switch (hovered_.part) {
        case Part::field: {
            const std::size_t field = hovered_.index % field_count;
            switch (static_cast<Field>(field)) {
            case Field::player:
                return facts_now_.slots[hovered_.index / field_count].open
                           ? fallback
                           : worded("Click to change who plays this slot.");
            case Field::side:
                return worded("Click to change the side.");
            case Field::colour:
                return worded("Left click for the next colour, right click for the one before.");
            case Field::team:
                return worded("Click to change the team.");
            case Field::metal:
            case Field::energy:
                if (!facts_now_.help[field].empty())
                    return facts_now_.help[field];
                return fallback;
            }
            return fallback;
        }
        case Part::option_back:
        case Part::option_on:
            if (!facts_now_.options[hovered_.index].help.empty())
                return facts_now_.options[hovered_.index].help;
            return fallback;
        case Part::select_map:
            return worded("Choose the map to play on.");
        default:
            return fallback;
        }
    }

    /// Draws the MAP panel: its label, the map's picture with its start
    /// positions on a screen in a steel bezel, its name, how many players
    /// it is for and its size, and SELECT MAP.
    ///
    /// @param canvas where it draws
    void draw_map(const kit::Canvas& canvas) const {
        draw_panel_frame(canvas, layout_.map, layout_.rim);
        draw_label(canvas, layout_.map, worded("MAP"));
        const kit::Rect& monitor = layout_.monitor;
        draw_steel(canvas, monitor, steel_colour);
        const int32_t bezel = std::min(layout_.bezel, std::min(monitor.width, monitor.height) / 4);
        const int32_t bolt = std::max(bezel * 2 / 5, 1);
        const int32_t centre = bezel / 2;
        for (const kit::Point corner :
             {kit::Point{monitor.x + centre, monitor.y + centre},
              kit::Point{monitor.x + monitor.width - centre - 1, monitor.y + centre},
              kit::Point{monitor.x + centre, monitor.y + monitor.height - centre - 1},
              kit::Point{monitor.x + monitor.width - centre - 1,
                         monitor.y + monitor.height - centre - 1}})
            draw_bolt(canvas, corner, bolt);
        const kit::Rect glass{
            monitor.x + bezel, monitor.y + bezel, monitor.width - 2 * bezel,
            monitor.height - 2 * bezel
        };
        kit::fill_rect(canvas, glass, screen_colour);
        const kit::Rect drawn = draw_fitted(
            canvas, {glass.x + 2, glass.y + 2, glass.width - 4, glass.height - 4}, map_picture(),
            map_fitted_
        );
        if (drawn.width > 0) {
            kit::outline_rect(canvas, drawn, outline_colour);
            for (const SkirmishMenuStart& start : facts_now_.starts)
                draw_start(canvas, drawn, start);
        } else {
            kit::box_text(
                canvas, kit::FontRole::small, worded("No picture"), glass, kit::Align::centre,
                caption_faded_colour
            );
        }
        draw_sunk_edge(canvas, {glass.x - 2, glass.y - 2, glass.width + 4, glass.height + 4});

        const kit::Measure regular = [&canvas](std::string_view line) {
            return kit::measure_text(canvas, kit::FontRole::regular, line);
        };
        const kit::Measure small = [&canvas](std::string_view line) {
            return kit::measure_text(canvas, kit::FontRole::small, line);
        };
        outlined_text(
            canvas,
            kit::FontRole::regular,
            kit::fit(facts_now_.map, layout_.map_name.width, regular),
            layout_.map_name,
            kit::Align::centre,
            caption_colour
        );
        std::string about;
        if (!facts_now_.players.empty())
            about = filled(worded("{count} players"), "{count}", facts_now_.players);
        if (!facts_now_.size.empty()) {
            if (!about.empty())
                about += "  -  ";
            about += filled(worded("Size {size}"), "{size}", facts_now_.size);
        }
        kit::box_text(
            canvas,
            kit::FontRole::small,
            kit::fit(about, layout_.map_facts.width, small),
            layout_.map_facts,
            kit::Align::centre,
            plate_text_colour
        );

        const Hit hit{Part::select_map, 0};
        const bool usable = enabled(hit);
        const bool lit = usable && hovered_ == hit;
        const bool held = lit && pressed_ == hit;
        const kit::Rect& rect = layout_.select_map;
        draw_steel_button(canvas, rect, button_steel_colour);
        if (lit)
            kit::outline_rect(canvas, rect, hover_edge_colour);
        kit::Rect box = rect;
        if (held) {
            box.x += 1;
            box.y += 1;
        }
        const std::string text = worded("SELECT MAP");
        outlined_text(
            canvas,
            kit::FontRole::regular,
            text,
            box,
            kit::Align::centre,
            !usable ? caption_faded_colour : lit ? caption_lit_colour : caption_colour,
            heading_tracking - 1
        );
    }

    /// Draws a start position's mark: an amber disc with its number.
    ///
    /// @param canvas where it draws
    /// @param picture where the map's picture lies
    /// @param start the start position
    void draw_start(
        const kit::Canvas& canvas, const kit::Rect& picture, const SkirmishMenuStart& start
    ) const {
        const int32_t radius = std::min(layout_.mark, std::min(picture.width, picture.height) / 6);
        if (radius < 3)
            return;
        const int32_t x = picture.x + nearest(std::clamp(start.across, 0.0, 1.0) * picture.width);
        const int32_t y = picture.y + nearest(std::clamp(start.down, 0.0, 1.0) * picture.height);
        const kit::Rect disc{
            std::clamp(x - radius, picture.x, picture.x + picture.width - 2 * radius),
            std::clamp(y - radius, picture.y, picture.y + picture.height - 2 * radius),
            2 * radius,
            2 * radius
        };
        kit::fill_round_rect(
            canvas, {disc.x - 1, disc.y - 1, disc.width + 2, disc.height + 2}, radius + 1,
            outline_colour
        );
        kit::fill_round_rect(canvas, disc, radius, chosen_colour);
        kit::fill_round_rect(
            canvas, {disc.x + 2, disc.y + 2, disc.width - 4, disc.height - 4}, radius - 2,
            mark_colour
        );
        outlined_text(
            canvas, kit::FontRole::small, std::to_string(start.number), disc, kit::Align::centre,
            caption_lit_colour
        );
    }

    /// Draws the GAME OPTIONS panel: for each rule its label, its arrows and
    /// its value between them.
    ///
    /// @param canvas where it draws
    void draw_options(const kit::Canvas& canvas) const {
        draw_panel_frame(canvas, layout_.options, layout_.rim);
        draw_label(canvas, layout_.options, worded("GAME OPTIONS"));
        const kit::Measure regular = [&canvas](std::string_view line) {
            return kit::measure_text(canvas, kit::FontRole::regular, line);
        };
        for (std::size_t option = 0; option < skirmish_menu_options; ++option) {
            const SkirmishMenuOption& shown = facts_now_.options[option];
            const kit::Rect& row = layout_.option_rows[option];
            outlined_text(
                canvas,
                kit::FontRole::regular,
                kit::fit(shown.label, layout_.option_label - layout_.option_gap, regular),
                {row.x, row.y, layout_.option_label - layout_.option_gap, row.height},
                kit::Align::left,
                caption_colour
            );
            for (const bool back : {true, false}) {
                const Hit hit{back ? Part::option_back : Part::option_on, option};
                const kit::Rect rect = arrow_rect(option, back);
                const bool usable = enabled(hit);
                const bool lit = usable && hovered_ == hit;
                draw_steel_button(canvas, rect, button_steel_colour);
                if (lit)
                    kit::outline_rect(canvas, rect, hover_edge_colour);
                const int32_t reach = std::max(rect.width / 5, 3);
                const int32_t middle = rect.y + rect.height / 2;
                draw_arrow(
                    canvas,
                    back ? kit::Point{rect.x + rect.width / 2 - reach / 2, middle}
                         : kit::Point{rect.x + rect.width / 2 + reach / 2, middle},
                    reach,
                    back,
                    !usable ? caption_faded_colour : lit ? caption_lit_colour : caption_colour
                );
            }
            const kit::Rect value = value_rect(option);
            draw_steel(canvas, value, cell_steel_colour);
            outlined_text(
                canvas,
                kit::FontRole::regular,
                kit::fit(shown.value, value.width - 8, regular),
                value,
                kit::Align::centre,
                caption_colour
            );
        }
    }

    /// Draws Previous Menu or Start with the main menu's button faces, its
    /// caption outlined, Previous Menu's arrow and the quick key's
    /// underline.
    ///
    /// @param canvas where it draws
    /// @param part Part::back or Part::start
    /// @param art the menus' art
    void draw_button(const kit::Canvas& canvas, Part part, const MenuArt& art) const {
        const Hit hit{part, 0};
        const bool usable = enabled(hit);
        const bool lit = usable && hovered_ == hit;
        const bool held = lit && pressed_ == hit;
        Face face = Face::normal;
        if (!usable)
            face = Face::disabled;
        else if (held)
            face = Face::pressed;
        else if (lit)
            face = Face::hover;
        const kit::Rect& rect = part == Part::back ? layout_.back : layout_.start;
        fitted_.draw(canvas, rect, art.faces[static_cast<std::size_t>(face)]);
        const std::string text = worded(part == Part::back ? "PREVIOUS MENU" : "START");
        kit::Rect box = rect;
        // A held button's caption sinks a point right and down, as the
        // game's do.
        if (held) {
            box.x += 1;
            box.y += 1;
        }
        const kit::Colour ink = !usable               ? caption_faded_colour
                                : face == Face::hover ? caption_lit_colour
                                                      : caption_colour;
        outlined_text(canvas, kit::FontRole::regular, text, box, kit::Align::centre, ink);
        if (part == Part::back)
            draw_arrow(
                canvas,
                {box.x + layout_.back_arrow, box.y + box.height / 2},
                std::max(kit::smooth_text_points(kit::FontRole::regular) / 2, 3),
                true,
                ink
            );
        const auto key = quick_key_at(
            text, part == Part::back ? facts_now_.back_key : facts_now_.start_key
        );
        if (usable && key)
            underline_quick_key(canvas, text, box, *key);
    }

    std::function<bool()> shows_{};              ///< whether the screen shows
    std::function<SkirmishMenuFacts()> facts_{}; ///< reads SKIRMISH.GUI and the setup
    SkirmishMenuActions actions_{};              ///< what its presses do
    SkirmishMenuFacts facts_now_{};              ///< what they show, as last read
    kit::Point size_{};                          ///< the screen's size, in points
    SkirmishLayout layout_{};                    ///< where its parts lie
    bool laid_out_{};                            ///< layout_ is for size_
    Hit hovered_{};                              ///< the part under the pointer
    Hit pressed_{};                              ///< the part a press holds
    uint8_t pressed_button_{};                   ///< the mouse button the press is of
    FittedArt fitted_{};                         ///< the title and the faces, filtered
    mutable FittedArt emblem_fitted_{};          ///< the emblems, filtered
    mutable FittedArt swatch_fitted_{};          ///< the swatches, filtered
    mutable FittedArt map_fitted_{};             ///< the map's picture, filtered
    /// The sides' emblems, by side, each made when first drawn; each stays
    /// where it is while copies of it are kept.
    mutable std::map<std::size_t, std::unique_ptr<WindowIcon>> emblems_{};
    /// The colours' swatches, by colour, each made when first drawn.
    mutable std::map<int32_t, std::unique_ptr<WindowIcon>> swatches_{};
    /// The map's picture, made when first drawn for the map.
    mutable std::unique_ptr<WindowIcon> map_picture_{};
    uint64_t revision_{1};                       ///< counts the changes to what it draws
};

} // namespace

std::unique_ptr<LayerScreen> make_skirmish_menu(
    std::function<bool()> shows,
    std::function<SkirmishMenuFacts()> facts,
    SkirmishMenuActions actions
) {
    return std::make_unique<SkirmishMenuScreen>(
        std::move(shows), std::move(facts), std::move(actions)
    );
}

} // namespace oa::app
