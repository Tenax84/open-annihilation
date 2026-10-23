// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Open Annihilation's Multiplayer screen on the OA layer (multiplayer_menu.hpp).

#include "multiplayer_menu.hpp"
#include "menu_paint.hpp"

#include "oa/ui/kit/canvas.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/text.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
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
constexpr double design_least_columns = 1480.0;
constexpr double content_columns = 1420.0;    ///< the panels' width together
constexpr double title_top = 18.0;            ///< the title's top
constexpr double title_columns = 420.0;       ///< the title's width
constexpr double heading_gap_rows = 4.0;      ///< between the title and the heading
constexpr double heading_rows = 40.0;         ///< the heading's height
constexpr double heading_columns = 820.0;     ///< the heading's width, its rules' reach
constexpr double heading_gap_columns = 18.0;  ///< between the heading and its rules
constexpr double panels_gap_rows = 12.0;      ///< between the heading and the panels
constexpr double panels_rows = 690.0;         ///< the panels' height
constexpr double player_columns = 300.0;      ///< the PLAYER panel's width
constexpr double details_columns = 350.0;     ///< the DETAILS panel's width
constexpr double panel_gap_columns = 14.0;    ///< between two panels
constexpr double band_rows = 46.0;            ///< a panel's label band
constexpr double rim_columns = 11.0;          ///< a panel's steel rim's width
constexpr double label_left_columns = 10.0;   ///< from a panel's rim to its label plate
constexpr double label_top_rows = 5.0;        ///< from a panel's top to its label plate
constexpr double label_rows = 34.0;           ///< a label plate's height
constexpr double inset_columns = 18.0;        ///< inside a panel, round its content
constexpr double pad_columns = 12.0;          ///< inside a row or a field, round its parts
constexpr double gap_rows = 10.0;             ///< between two parts of a panel
constexpr double glow_columns = 6.0;          ///< how far a glow reaches
constexpr double plate_rows_design = 64.0;    ///< a connection's plate's height
constexpr double plate_gap_rows = 8.0;        ///< between two connections' plates
constexpr double icon_columns = 36.0;         ///< a connection's picture's side
constexpr double big_icon_columns = 120.0;    ///< the chosen connection's picture in DETAILS
constexpr double field_rows = 44.0;           ///< a text field's height
constexpr double field_label_rows = 30.0;     ///< the words over a field
constexpr double address_label_columns = 140.0; ///< the address field's words, left of it
constexpr double header_rows = 34.0;          ///< the games' column captions
constexpr double game_rows = 50.0;            ///< a game's row's height
constexpr double game_gap_rows = 4.0;         ///< between two games' rows
constexpr double status_rows = 40.0;          ///< the line under the games
constexpr double scroll_columns = 12.0;       ///< the games' scroll bar's width
constexpr double scroll_gap_columns = 8.0;    ///< between the rows and the scroll bar
constexpr double picture_rows = 250.0;        ///< the map's screen's height
constexpr double bezel_columns = 10.0;        ///< the map's screen's steel bezel
constexpr double buttons_gap_rows = 18.0;     ///< between the panels and the buttons
constexpr double button_rows = 58.0;          ///< the buttons' height
constexpr double wide_button_columns = 300.0; ///< OPTIONS' and PREVIOUS MENU's width
constexpr double game_button_columns = 260.0; ///< NEW GAME's and UPDATE's width
constexpr double small_button_columns = 220.0; ///< SELECT's, WATCH's and JOIN's width
constexpr double footer_gap_columns = 16.0;   ///< between two buttons of a side
constexpr double back_arrow_columns = 28.0;   ///< from Previous Menu's left to its arrow
constexpr double plate_top = 1004.0;          ///< the version plate's top
constexpr double plate_rows = 36.0;           ///< the version plate's height
constexpr double plate_margin_columns = 30.0; ///< the plate's room either side of its text
/// The points a line of text needs over its font's size.
constexpr int32_t text_room = 8;
/// The extra points after each character of a label band.
constexpr int32_t band_tracking = 2;
/// The extra points after each character of a column caption.
constexpr int32_t caption_tracking = 1;
/// How many lines of a connection's words DETAILS shows at most.
constexpr std::size_t most_about_lines = 10;
/// How many points the wheel scrolls the games a notch: one row.
constexpr double wheel_rows_per_notch = 1.0;

/// The games' columns' shares of a row: the name, the map, the players,
/// the ping and the status.
constexpr std::array<double, 5> game_column_shares{28, 25, 13, 12, 22};
/// The games' column captions, in English.
constexpr std::array<std::string_view, 5> game_column_captions{
    "Game", "Map", "Players", "Ping", "Status"
};

constexpr kit::Colour entry_steel_colour{64, 70, 72};    ///< a connection's plate
constexpr kit::Colour chosen_steel_colour{98, 72, 34};   ///< the chosen plate, amber steel
constexpr kit::Colour chosen_colour{232, 164, 52};       ///< amber: the chosen and the focused
constexpr kit::Colour chosen_text_colour{255, 206, 120}; ///< the chosen's words
constexpr kit::Colour strip_steel_colour{56, 62, 64};    ///< the column captions' strip
constexpr kit::Colour hover_edge_colour{176, 184, 184};  ///< round what the pointer is over
constexpr kit::Colour well_colour{12, 15, 18};           ///< a field's and a well's ground
constexpr kit::Colour row_colour{20, 25, 29};            ///< a row's ground
constexpr kit::Colour row_edge_colour{58, 64, 66};       ///< a row's edge
constexpr kit::Colour rule_line_colour{40, 46, 50};      ///< between two facts
constexpr kit::Colour icon_colour{214, 218, 206};        ///< a connection's picture
constexpr kit::Colour note_colour{150, 152, 144};        ///< faded words
constexpr kit::Colour screen_colour{12, 15, 18};         ///< the map's screen, dark gunmetal
/// How much amber lies over a row under the pointer, in 256ths.
constexpr uint32_t row_light_opacity = 26;

/// What a press can reach.
enum class Part : uint8_t {
    none,       ///< nothing
    connection, ///< a connection's plate, by its place
    address,    ///< the address field
    game,       ///< a game's row, by its place
    nickname,   ///< the name field
    password,   ///< the password field
    options,    ///< OPTIONS
    select,     ///< SELECT
    back,       ///< PREVIOUS MENU
    new_game,   ///< NEW GAME
    update,     ///< UPDATE
    watch,      ///< WATCH
    join,       ///< JOIN
};

/// A part, and which of its kind.
struct Hit {
    Part part{Part::none}; ///< what it is
    std::size_t index{};   ///< which connection or game

    friend bool operator==(const Hit&, const Hit&) = default;
};

/// Where the screen's fixed parts lie, in its points.
struct MultiplayerLayout {
    kit::Rect background{};   ///< the picture, cut by the screen's edges
    kit::Rect title{};        ///< the title
    kit::Rect heading{};      ///< the heading
    kit::Rect connections{};  ///< the CONNECTION panel, the connection step's
    kit::Rect player{};       ///< the PLAYER panel, the game step's
    kit::Rect games{};        ///< the GAMES panel
    kit::Rect details{};      ///< the DETAILS panel, either step's
    kit::Rect options{};      ///< OPTIONS
    kit::Rect select{};       ///< SELECT
    kit::Rect back_connection{}; ///< PREVIOUS MENU of the connection step
    kit::Rect new_game{};     ///< NEW GAME
    kit::Rect update{};       ///< UPDATE
    kit::Rect watch{};        ///< WATCH
    kit::Rect join{};         ///< JOIN
    kit::Rect back_games{};   ///< PREVIOUS MENU of the game step
    int32_t band{};           ///< a panel's label band
    int32_t inset{};          ///< inside a panel, round its content
    int32_t rim{};            ///< a panel's steel rim's width
    int32_t label_left{};     ///< from a panel's rim to its label plate
    int32_t label_top{};      ///< from a panel's top to its label plate
    int32_t label_height{};   ///< a label plate's height
    int32_t pad{};            ///< inside a row or a field, round its parts
    int32_t gap{};            ///< between two parts of a panel
    int32_t glow{};           ///< how far a glow reaches
    int32_t plate{};          ///< a connection's plate's height
    int32_t plate_gap{};      ///< between two connections' plates
    int32_t icon{};           ///< a connection's picture's side
    int32_t big_icon{};       ///< the chosen connection's picture in DETAILS
    int32_t field{};          ///< a text field's height
    int32_t field_label{};    ///< the words over a field
    int32_t address_label{};  ///< the address field's words, left of it
    int32_t header{};         ///< the games' column captions
    int32_t row{};            ///< a game's row's height
    int32_t row_gap{};        ///< between two games' rows
    int32_t status{};         ///< the line under the games
    int32_t scroll{};         ///< the games' scroll bar's width
    int32_t scroll_gap{};     ///< between the rows and the scroll bar
    int32_t picture{};        ///< the map's screen's height
    int32_t bezel{};          ///< the map's screen's bezel
    int32_t back_arrow{};     ///< from Previous Menu's left to its arrow
    int32_t heading_gap{};    ///< between the heading and its rules
    int32_t plate_top{};      ///< the version plate's top
    int32_t plate_height{};   ///< the version plate's height
    int32_t plate_margin{};   ///< its room either side of the text
};

/// Lays the screen out on a screen of a size.
///
/// @param width the screen's width, in points
/// @param height the screen's height, in points
/// @param title_size the title picture's width and height, in pixels
/// @return where its parts lie
MultiplayerLayout lay_out_multiplayer_menu(int32_t width, int32_t height, kit::Point title_size) {
    MultiplayerLayout placed;
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
    placed.band = std::max(across(band_rows), placed.label_top + placed.label_height + 2);
    placed.pad = std::max(across(pad_columns), 3);
    placed.gap = std::max(across(gap_rows), 3);
    placed.glow = std::max(across(glow_columns), 2);
    placed.plate = std::max(across(plate_rows_design), regular_line + 4);
    placed.plate_gap = std::max(across(plate_gap_rows), 2);
    placed.icon = std::max(across(icon_columns), 12);
    placed.big_icon = std::max(across(big_icon_columns), 32);
    placed.field = std::max(across(field_rows), regular_line + 4);
    placed.field_label = std::max(across(field_label_rows), small_line);
    placed.address_label = across(address_label_columns);
    placed.header = std::max(across(header_rows), small_line);
    placed.row = std::max(across(game_rows), regular_line + 2);
    placed.row_gap = std::max(across(game_gap_rows), 1);
    placed.status = std::max(across(status_rows), small_line + 4);
    placed.scroll = std::max(across(scroll_columns), 6);
    placed.scroll_gap = std::max(across(scroll_gap_columns), 2);
    placed.picture = across(picture_rows);
    placed.bezel = std::max(across(bezel_columns), 3);
    placed.back_arrow = across(back_arrow_columns);
    placed.heading_gap = across(heading_gap_columns);

    const int32_t left = nearest(middle - content_columns * unit / 2.0);
    const int32_t content = across(content_columns);
    const int32_t right = left + content;
    const double panels_top = heading_top + heading_rows + panels_gap_rows;
    const int32_t panels_y = down(panels_top);
    const int32_t panels_height = across(panels_rows);
    const int32_t gap = across(panel_gap_columns);
    const int32_t details_width = across(details_columns);
    placed.details = {right - details_width, panels_y, details_width, panels_height};
    placed.connections = {left, panels_y, placed.details.x - gap - left, panels_height};
    placed.player = {left, panels_y, across(player_columns), panels_height};
    const int32_t games_left = placed.player.x + placed.player.width + gap;
    placed.games = {games_left, panels_y, placed.details.x - gap - games_left, panels_height};

    const int32_t buttons_y = down(panels_top + panels_rows + buttons_gap_rows);
    const int32_t button_height = across(button_rows);
    const int32_t footer_gap = across(footer_gap_columns);
    const int32_t wide = across(wide_button_columns);
    const int32_t game_wide = across(game_button_columns);
    const int32_t small = across(small_button_columns);
    placed.options = {left, buttons_y, wide, button_height};
    placed.back_connection = {right - wide, buttons_y, wide, button_height};
    placed.select = {placed.back_connection.x - footer_gap - small, buttons_y, small, button_height};
    placed.new_game = {left, buttons_y, game_wide, button_height};
    placed.update = {left + game_wide + footer_gap, buttons_y, game_wide, button_height};
    placed.back_games = {right - wide, buttons_y, wide, button_height};
    placed.join = {placed.back_games.x - footer_gap - small, buttons_y, small, button_height};
    placed.watch = {placed.join.x - footer_gap - small, buttons_y, small, button_height};
    placed.plate_top = down(plate_top);
    placed.plate_height = std::max(across(plate_rows), 1);
    placed.plate_margin = across(plate_margin_columns);
    return placed;
}

/// Draws a globe: a ring, its equator and two parallels, and a meridian.
///
/// @param canvas where it draws
/// @param box the square it fills
/// @param ink its colour
void draw_globe(const kit::Canvas& canvas, const kit::Rect& box, kit::Colour ink) {
    const int32_t side = std::min(box.width, box.height);
    const kit::Rect square{box.x + (box.width - side) / 2, box.y + (box.height - side) / 2, side, side};
    const int32_t width = std::max(side / 18, 1);
    draw_circle(canvas, square, width, ink);
    const int32_t middle_x = square.x + side / 2;
    const int32_t middle_y = square.y + side / 2;
    draw_line(canvas, {square.x + width, middle_y}, {square.x + side - width - 1, middle_y}, width, ink);
    const int32_t parallel = side / 4;
    const int32_t parallel_reach = side * 43 / 100;
    for (const int32_t y : {middle_y - parallel, middle_y + parallel})
        draw_line(canvas, {middle_x - parallel_reach, y}, {middle_x + parallel_reach, y}, width, ink);
    // The meridian: an ellipse as tall as the globe, a fifth of it wide
    // each side, drawn point by point round it.
    const double across = side * 0.2;
    const double down = side / 2.0 - width / 2.0;
    const int32_t steps = std::max(side * 3, 48);
    for (int32_t step = 0; step < steps; ++step) {
        const double turn = 6.283185307179586 * step / steps;
        const int32_t x = middle_x + nearest(std::cos(turn) * across);
        const int32_t y = middle_y + nearest(std::sin(turn) * down);
        kit::fill_rect(canvas, {x - width / 2, y - width / 2, width, width}, ink);
    }
}

/// Draws two screens joined by a line, for a connection over a local network.
///
/// @param canvas where it draws
/// @param box the square it fills
/// @param ink its colour
void draw_network(const kit::Canvas& canvas, const kit::Rect& box, kit::Colour ink) {
    const int32_t side = std::min(box.width, box.height);
    const kit::Rect square{box.x + (box.width - side) / 2, box.y + (box.height - side) / 2, side, side};
    const int32_t width = std::max(side / 18, 1);
    const int32_t screen = side * 2 / 5;
    const kit::Rect first{square.x, square.y + side / 8, screen, screen * 3 / 4};
    const kit::Rect second{square.x + side - screen, square.y + side / 2, screen, screen * 3 / 4};
    for (const kit::Rect& rect : {first, second})
        for (int32_t step = 0; step < width; ++step)
            kit::outline_rect(
                canvas,
                {rect.x + step, rect.y + step, rect.width - 2 * step, rect.height - 2 * step},
                ink
            );
    draw_line(
        canvas,
        {first.x + first.width / 2, first.y + first.height},
        {first.x + first.width / 2, second.y + second.height / 2},
        width,
        ink
    );
    draw_line(
        canvas,
        {first.x + first.width / 2, second.y + second.height / 2},
        {second.x, second.y + second.height / 2},
        width,
        ink
    );
}

/// The Multiplayer screen as a screen of the OA layer.
///
/// Its points are the layer's: one point is the view's scale in window
/// pixels, from the room's top left corner, and the screen covers the room.
class MultiplayerMenuScreen final : public LayerScreen {
  public:

    /// Makes the screen.
    ///
    /// @param shows tells whether the screen shows
    /// @param facts reads what the game's screen under it shows now
    /// @param actions what its presses do
    MultiplayerMenuScreen(
        std::function<bool()> shows,
        std::function<MultiplayerMenuFacts()> facts,
        MultiplayerMenuActions actions
    )
        : shows_(std::move(shows)), facts_(std::move(facts)), actions_(std::move(actions)) {
        facts_now_ = facts_();
        focus_ = first_focus();
    }

    /// Returns the screen's name.
    ///
    /// @return multiplayer_menu_screen_name
    [[nodiscard]] std::string_view name() const override { return multiplayer_menu_screen_name; }

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

    /// Draws the background, the title, the heading, the step's panels and
    /// buttons, and the version plate.
    ///
    /// @param canvas where it draws, at the layer's scale
    void draw(const kit::Canvas& canvas) const override {
        const MenuArt& art = menu_art();
        draw_backdrop(canvas, size_, layout_.background);
        fitted_.draw(canvas, layout_.title, art.title);
        draw_ruled_heading(canvas, layout_.heading, worded("MULTIPLAYER"), layout_.heading_gap);
        if (facts_now_.step == MultiplayerMenuStep::connection) {
            draw_connections(canvas);
            draw_connection_details(canvas);
            draw_footer(canvas, {Part::options, 0}, art);
            draw_footer(canvas, {Part::select, 0}, art);
            draw_footer(canvas, {Part::back, 0}, art);
        } else {
            draw_player(canvas);
            draw_games(canvas);
            draw_game_details(canvas);
            for (const Part part : {Part::new_game, Part::update, Part::watch, Part::join, Part::back})
                draw_footer(canvas, {part, 0}, art);
        }
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
        layout_ = lay_out_multiplayer_menu(
            size_.x,
            size_.y,
            {static_cast<int32_t>(art.title.width), static_cast<int32_t>(art.title.height)}
        );
        laid_out_ = true;
        fitted_.clear();
        map_fitted_.clear();
        clamp_scroll();
        ++revision_;
    }

    /// Follows the pointer over the parts, focuses a field a press lands on
    /// and presses the part a press is released on; a second press on a
    /// game's row joins it. Takes every pointer, so that the game's screen
    /// under it sees none.
    ///
    /// @param kind pointer_move, pointer_down or pointer_up
    /// @param button the pointer's button, 1 for the left
    /// @param at the pointer, in the screen's points
    /// @param finger_reach unused
    /// @param clicks how many presses landed as one
    /// @return redraw when the look changed; none otherwise
    LayerAnswer pointer_with_clicks(
        ScreenInputKind kind, uint8_t button, kit::Point at, int32_t /*finger_reach*/, uint8_t clicks
    ) override {
        const uint64_t before = revision_;
        const Hit over = hit_at(at);
        set_hovered(over);
        if (button != SDL_BUTTON_LEFT)
            return revision_ != before ? LayerAnswer::redraw : LayerAnswer::none;
        if (kind == ScreenInputKind::pointer_down) {
            set_pressed(over);
            const MultiplayerMenuField field = field_of(over);
            if (field != focus_) {
                focus_ = field;
                ++revision_;
            }
            if (over.part == Part::game && clicks >= 2 && facts_now_.game &&
                *facts_now_.game == over.index && facts_now_.can_join) {
                set_pressed({});
                press_button(MultiplayerMenuButton::join);
                return LayerAnswer::redraw;
            }
        } else if (kind == ScreenInputKind::pointer_up) {
            const Hit held = pressed_;
            set_pressed({});
            if (held.part != Part::none && held == over) {
                press(held);
                return LayerAnswer::redraw;
            }
        }
        return revision_ != before ? LayerAnswer::redraw : LayerAnswer::none;
    }

    /// Takes a pointer as pointer_with_clicks does, as one press.
    LayerAnswer pointer(
        ScreenInputKind kind, uint8_t button, kit::Point at, int32_t finger_reach
    ) override {
        return pointer_with_clicks(kind, button, at, finger_reach, 1);
    }

    /// Takes a key that types while a field holds the focus, and OPTIONS'
    /// quick key, which opens Settings in place of the game's options.
    ///
    /// @param sdl_key the key's SDL keycode
    /// @param modifiers the modifier keys held
    /// @return none for a key taken; pass otherwise
    LayerAnswer shortcut(uint32_t sdl_key, uint16_t modifiers) override {
        const bool commanded = (modifiers & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) != 0;
        if (focus_ != MultiplayerMenuField::none && !commanded && sdl_key >= 0x20 && sdl_key < 0x7f)
            return LayerAnswer::none;
        if (!commanded && facts_now_.step == MultiplayerMenuStep::connection &&
            facts_now_.options_key != 0 && sdl_key < 0x80 &&
            static_cast<char>(sdl_key) ==
                static_cast<char>(std::tolower(static_cast<unsigned char>(facts_now_.options_key)))) {
            press({Part::options, 0});
            return LayerAnswer::redraw;
        }
        return LayerAnswer::pass;
    }

    /// Edits the focused field, moves between the fields with Tab, chooses
    /// with the arrows, and presses SELECT or JOIN with Return; lets every
    /// other key go on.
    ///
    /// @param pressed the key
    /// @return redraw when it acted; pass for a key it lets go on
    LayerAnswer key(kit::Key pressed, uint32_t /*sdl_key*/) override {
        if (focus_ != MultiplayerMenuField::none)
            return field_key(pressed);
        const bool connection = facts_now_.step == MultiplayerMenuStep::connection;
        switch (pressed) {
        case kit::Key::up:
        case kit::Key::down:
            move_choice(pressed == kit::Key::down ? 1 : -1);
            return LayerAnswer::redraw;
        case kit::Key::page_up:
        case kit::Key::page_down:
            if (connection)
                return LayerAnswer::none;
            move_choice(pressed == kit::Key::page_down ? rows_shown() : -rows_shown());
            return LayerAnswer::redraw;
        case kit::Key::home:
        case kit::Key::end:
            if (connection)
                return LayerAnswer::none;
            move_choice(pressed == kit::Key::end ? 1000 : -1000);
            return LayerAnswer::redraw;
        case kit::Key::tab:
        case kit::Key::back_tab: {
            const auto order = field_order();
            if (order.empty())
                return LayerAnswer::none;
            focus_ = pressed == kit::Key::tab ? order.front() : order.back();
            ++revision_;
            return LayerAnswer::redraw;
        }
        case kit::Key::enter:
            if (connection)
                press({Part::select, 0});
            else if (facts_now_.can_join)
                press({Part::join, 0});
            return LayerAnswer::redraw;
        default:
            return LayerAnswer::pass;
        }
    }

    /// Scrolls the games with the wheel over them.
    ///
    /// @param at the pointer, in the screen's points
    /// @param notches how far the wheel turned, positive away from the player
    /// @return redraw when they scrolled; none otherwise
    LayerAnswer wheel(kit::Point at, float notches) override {
        if (facts_now_.step != MultiplayerMenuStep::games || !kit::contains(rows_view(), at))
            return LayerAnswer::none;
        const int32_t before = scroll_;
        scroll_ -= nearest(notches * wheel_rows_per_notch * pitch());
        clamp_scroll();
        if (scroll_ == before)
            return LayerAnswer::none;
        set_hovered(hit_at(at));
        ++revision_;
        return LayerAnswer::redraw;
    }

    /// Takes text typed into the focused field.
    ///
    /// @param utf8 the text
    /// @return redraw when a field took it; pass otherwise
    LayerAnswer text(std::string_view utf8) override {
        if (focus_ == MultiplayerMenuField::none || utf8.empty())
            return LayerAnswer::pass;
        if (actions_.type)
            actions_.type(focus_, utf8);
        refresh();
        return LayerAnswer::redraw;
    }

    /// Returns the focused field.
    ///
    /// @return the field; none while no field holds the focus
    [[nodiscard]] std::optional<kit::Rect> text_field() const override {
        if (focus_ == MultiplayerMenuField::none)
            return std::nullopt;
        return field_rect(focus_);
    }

    /// Returns the parts a press can reach, one named control each, for
    /// automation: the connections and the address on the connection step;
    /// the fields and the games the view shows on the game step; and the
    /// step's buttons.
    ///
    /// @return the list
    [[nodiscard]] kit::DisplayList display_list() const override {
        kit::DisplayList list;
        const auto add = [&](Hit hit, const kit::Rect& rect, std::string name, std::string text,
                             bool checked, kit::ControlKind kind) {
            kit::Control control;
            control.id = control_id(hit);
            control.rect = rect;
            control.enabled = enabled(hit);
            control.name = std::string(multiplayer_menu_screen_name) + "." + std::move(name);
            control.kind = kind;
            control.text = std::move(text);
            control.checked = checked;
            list.controls.push_back(std::move(control));
            list.tab_order.push_back(control_id(hit));
        };
        if (facts_now_.step == MultiplayerMenuStep::connection) {
            for (std::size_t at = 0; at < facts_now_.connections.size(); ++at)
                add({Part::connection, at}, connection_rect(at),
                    "connection-" + std::to_string(at + 1), facts_now_.connections[at].name,
                    facts_now_.connection && *facts_now_.connection == at, kit::ControlKind::list_item);
            if (takes_address())
                add({Part::address, 0}, field_rect(MultiplayerMenuField::address), "address",
                    facts_now_.address, false, kit::ControlKind::text_field);
            add({Part::options, 0}, layout_.options, "options", worded("OPTIONS"), false,
                kit::ControlKind::button);
            add({Part::select, 0}, layout_.select, "select", worded("SELECT"), false,
                kit::ControlKind::button);
            add({Part::back, 0}, layout_.back_connection, "previous-menu", worded("PREVIOUS MENU"),
                false, kit::ControlKind::button);
            return list;
        }
        add({Part::nickname, 0}, field_rect(MultiplayerMenuField::nickname), "nickname",
            facts_now_.nickname, false, kit::ControlKind::text_field);
        add({Part::password, 0}, field_rect(MultiplayerMenuField::password), "password",
            std::string(facts_now_.password.size(), '*'), false, kit::ControlKind::text_field);
        const kit::Rect view = rows_view();
        for (std::size_t at = 0; at < facts_now_.games.size(); ++at) {
            const kit::Rect row = game_rect(at);
            if (row.y + row.height <= view.y || row.y >= view.y + view.height)
                continue;
            add({Part::game, at}, row, "game-" + std::to_string(at + 1), facts_now_.games[at].name,
                facts_now_.game && *facts_now_.game == at, kit::ControlKind::list_item);
        }
        add({Part::new_game, 0}, layout_.new_game, "new-game", worded("NEW GAME"), false,
            kit::ControlKind::button);
        add({Part::update, 0}, layout_.update, "update", worded("UPDATE"), false,
            kit::ControlKind::button);
        add({Part::watch, 0}, layout_.watch, "watch", worded("WATCH"), false,
            kit::ControlKind::button);
        add({Part::join, 0}, layout_.join, "join", worded("JOIN"), false, kit::ControlKind::button);
        add({Part::back, 0}, layout_.back_games, "previous-menu", worded("PREVIOUS MENU"), false,
            kit::ControlKind::button);
        return list;
    }

    /// Returns the hovered and pressed parts, and the focused field, or else
    /// the chosen connection or game.
    ///
    /// @return the interaction
    [[nodiscard]] kit::Interaction interaction() const override {
        kit::Interaction shown;
        shown.hovered = hovered_.part != Part::none ? control_id(hovered_) : kit::no_control;
        shown.pressed = pressed_.part != Part::none ? control_id(pressed_) : kit::no_control;
        if (focus_ != MultiplayerMenuField::none)
            shown.focused = control_id({part_of(focus_), 0});
        else if (facts_now_.step == MultiplayerMenuStep::connection && facts_now_.connection)
            shown.focused = control_id({Part::connection, *facts_now_.connection});
        else if (facts_now_.step == MultiplayerMenuStep::games && facts_now_.game)
            shown.focused = control_id({Part::game, *facts_now_.game});
        return shown;
    }

    /// Closes the screen when it no longer shows, and follows what the
    /// game's screen under it shows.
    ///
    /// @return close when the screen no longer shows; redraw when what it
    ///     shows changed; otherwise none
    LayerAnswer tick() override {
        if (!shows_())
            return LayerAnswer::close;
        return refresh() ? LayerAnswer::redraw : LayerAnswer::none;
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

    /// Returns the field a part is.
    ///
    /// @param hit the part
    /// @return the field; none for a part that is not one
    [[nodiscard]] static MultiplayerMenuField field_of(Hit hit) noexcept {
        switch (hit.part) {
        case Part::address:
            return MultiplayerMenuField::address;
        case Part::nickname:
            return MultiplayerMenuField::nickname;
        case Part::password:
            return MultiplayerMenuField::password;
        default:
            return MultiplayerMenuField::none;
        }
    }

    /// Returns the part a field is.
    ///
    /// @param field the field
    /// @return the part
    [[nodiscard]] static Part part_of(MultiplayerMenuField field) noexcept {
        switch (field) {
        case MultiplayerMenuField::address:
            return Part::address;
        case MultiplayerMenuField::nickname:
            return Part::nickname;
        case MultiplayerMenuField::password:
            return Part::password;
        case MultiplayerMenuField::none:
            break;
        }
        return Part::none;
    }

    /// Tells whether the chosen connection takes an address.
    ///
    /// @return true when it does
    [[nodiscard]] bool takes_address() const noexcept {
        return facts_now_.step == MultiplayerMenuStep::connection && facts_now_.connection &&
               *facts_now_.connection < facts_now_.connections.size() &&
               facts_now_.connections[*facts_now_.connection].takes_address;
    }

    /// Returns the fields the step's Tab moves through, in order.
    ///
    /// @return the fields
    [[nodiscard]] std::vector<MultiplayerMenuField> field_order() const {
        if (facts_now_.step == MultiplayerMenuStep::connection) {
            if (takes_address())
                return {MultiplayerMenuField::address};
            return {};
        }
        if (facts_now_.password_asked)
            return {MultiplayerMenuField::nickname, MultiplayerMenuField::password};
        return {MultiplayerMenuField::nickname};
    }

    /// Returns the field that holds the focus as a step opens: the address
    /// on the connection step, the name on the game step while it is empty.
    ///
    /// @return the field; none for none
    [[nodiscard]] MultiplayerMenuField first_focus() const noexcept {
        if (facts_now_.step == MultiplayerMenuStep::connection)
            return takes_address() ? MultiplayerMenuField::address : MultiplayerMenuField::none;
        return facts_now_.nickname.empty() ? MultiplayerMenuField::nickname
                                           : MultiplayerMenuField::none;
    }

    /// Reads what the game's screen shows, and follows it: a new step
    /// starts at its top with its first field focused; a field that went
    /// away loses the focus.
    ///
    /// @return true when what the screen shows changed
    bool refresh() {
        MultiplayerMenuFacts now = facts_();
        if (now == facts_now_)
            return false;
        const bool new_step = now.step != facts_now_.step;
        const std::string old_map = chosen_map_file();
        facts_now_ = std::move(now);
        if (new_step) {
            focus_ = first_focus();
            scroll_ = 0;
            hovered_ = {};
            pressed_ = {};
        } else {
            const auto order = field_order();
            if (focus_ != MultiplayerMenuField::none &&
                std::find(order.begin(), order.end(), focus_) == order.end())
                focus_ = MultiplayerMenuField::none;
        }
        if (chosen_map_file() != old_map) {
            map_picture_.reset();
            map_fitted_.clear();
        }
        clamp_scroll();
        if (!enabled(hovered_))
            hovered_ = {};
        ++revision_;
        return true;
    }

    /// Returns the chosen game's map's name in the game data.
    ///
    /// @return the name; empty for no game chosen
    [[nodiscard]] std::string chosen_map_file() const {
        if (facts_now_.step != MultiplayerMenuStep::games || !facts_now_.game ||
            *facts_now_.game >= facts_now_.games.size())
            return {};
        return facts_now_.games[*facts_now_.game].map_file;
    }

    /// Takes a key while a field holds the focus: Backspace erases, Tab
    /// moves on, Return accepts and Escape lets the field go.
    ///
    /// @param pressed the key
    /// @return redraw when it acted; none otherwise
    LayerAnswer field_key(kit::Key pressed) {
        switch (pressed) {
        case kit::Key::backspace:
            if (actions_.erase)
                actions_.erase(focus_);
            refresh();
            return LayerAnswer::redraw;
        case kit::Key::tab:
        case kit::Key::back_tab: {
            const auto order = field_order();
            const auto at = std::find(order.begin(), order.end(), focus_);
            if (at == order.end()) {
                focus_ = MultiplayerMenuField::none;
            } else if (pressed == kit::Key::tab) {
                focus_ = at + 1 == order.end() ? MultiplayerMenuField::none : *(at + 1);
            } else {
                focus_ = at == order.begin() ? MultiplayerMenuField::none : *(at - 1);
            }
            ++revision_;
            return LayerAnswer::redraw;
        }
        case kit::Key::enter:
            if (focus_ == MultiplayerMenuField::address) {
                press({Part::select, 0});
                return LayerAnswer::redraw;
            }
            focus_ = MultiplayerMenuField::none;
            ++revision_;
            return LayerAnswer::redraw;
        case kit::Key::escape:
            focus_ = MultiplayerMenuField::none;
            ++revision_;
            return LayerAnswer::redraw;
        case kit::Key::up:
        case kit::Key::down:
            if (facts_now_.step == MultiplayerMenuStep::connection) {
                move_choice(pressed == kit::Key::down ? 1 : -1);
                return LayerAnswer::redraw;
            }
            return LayerAnswer::none;
        default:
            return LayerAnswer::none;
        }
    }

    /// Moves the choice of a connection or a game by some places.
    ///
    /// @param by how many places, down for more than 0
    void move_choice(int32_t by) {
        const bool connection = facts_now_.step == MultiplayerMenuStep::connection;
        const std::size_t count =
            connection ? facts_now_.connections.size() : facts_now_.games.size();
        if (count == 0)
            return;
        const std::optional<std::size_t> chosen =
            connection ? facts_now_.connection : facts_now_.game;
        const int32_t from = chosen ? static_cast<int32_t>(*chosen) : (by > 0 ? -1 : 0);
        const auto to = static_cast<std::size_t>(
            std::clamp(from + by, int32_t{0}, static_cast<int32_t>(count) - 1)
        );
        press({connection ? Part::connection : Part::game, to});
        if (!connection)
            show_game(to);
    }

    /// Scrolls the games so that one shows whole.
    ///
    /// @param game the game, by its place
    void show_game(std::size_t game) {
        const kit::Rect view = rows_view();
        const int32_t top = static_cast<int32_t>(game) * pitch();
        if (top < scroll_)
            scroll_ = top;
        else if (top + layout_.row > scroll_ + view.height)
            scroll_ = top + layout_.row - view.height;
        clamp_scroll();
        ++revision_;
    }

    /// Returns how many games' rows the view shows whole.
    ///
    /// @return the count, at least 1
    [[nodiscard]] int32_t rows_shown() const noexcept {
        return std::max(rows_view().height / std::max(pitch(), 1), 1);
    }

    /// Returns from one game's row's top to the next's.
    ///
    /// @return the pitch, in points
    [[nodiscard]] int32_t pitch() const noexcept { return layout_.row + layout_.row_gap; }

    /// Keeps the games' scroll inside what they need.
    void clamp_scroll() noexcept {
        const int32_t needed =
            static_cast<int32_t>(facts_now_.games.size()) * pitch() - layout_.row_gap;
        scroll_ = std::clamp(scroll_, 0, std::max(needed - rows_view().height, 0));
    }

    /// Returns where the games' rows scroll in the GAMES panel.
    ///
    /// @return the view
    [[nodiscard]] kit::Rect rows_view() const noexcept {
        const kit::Rect& panel = layout_.games;
        const int32_t left = panel.x + layout_.inset;
        const int32_t top = panel.y + layout_.band + layout_.header + layout_.gap / 2;
        const int32_t bottom = status_rect().y - layout_.gap;
        const int32_t width = panel.width - 2 * layout_.inset - layout_.scroll - layout_.scroll_gap;
        return {left, top, std::max(width, 1), std::max(bottom - top, 1)};
    }

    /// Returns the line under the games.
    ///
    /// @return its box
    [[nodiscard]] kit::Rect status_rect() const noexcept {
        const kit::Rect& panel = layout_.games;
        return {
            panel.x + layout_.inset,
            panel.y + panel.height - layout_.inset - layout_.status,
            panel.width - 2 * layout_.inset,
            layout_.status
        };
    }

    /// Returns the games' column captions' strip.
    ///
    /// @return its box
    [[nodiscard]] kit::Rect header_rect() const noexcept {
        const kit::Rect view = rows_view();
        return {view.x, layout_.games.y + layout_.band, view.width, layout_.header};
    }

    /// Returns where a game's row lies, scrolled.
    ///
    /// @param game the game, by its place
    /// @return the row
    [[nodiscard]] kit::Rect game_rect(std::size_t game) const noexcept {
        const kit::Rect view = rows_view();
        return {
            view.x,
            view.y + static_cast<int32_t>(game) * pitch() - scroll_,
            view.width,
            layout_.row
        };
    }

    /// Returns the left edge of each of the games' columns, and the right end.
    ///
    /// @param row the row or strip the columns cut
    /// @return the edges
    [[nodiscard]] std::array<int32_t, game_column_shares.size() + 1>
    column_edges(const kit::Rect& row) const noexcept {
        std::array<int32_t, game_column_shares.size() + 1> edges{};
        double total = 0.0;
        for (const double share : game_column_shares)
            total += share;
        double reached = 0.0;
        const int32_t inner = row.width - 2 * layout_.pad;
        for (std::size_t column = 0; column < game_column_shares.size(); ++column) {
            edges[column] = row.x + layout_.pad + nearest(inner * reached / total);
            reached += game_column_shares[column];
        }
        edges.back() = row.x + row.width - layout_.pad;
        return edges;
    }

    /// Returns a connection's plate.
    ///
    /// @param connection the connection, by its place
    /// @return the plate
    [[nodiscard]] kit::Rect connection_rect(std::size_t connection) const noexcept {
        const kit::Rect& panel = layout_.connections;
        return {
            panel.x + layout_.inset,
            panel.y + layout_.band + static_cast<int32_t>(connection) * (layout_.plate + layout_.plate_gap),
            panel.width - 2 * layout_.inset,
            layout_.plate
        };
    }

    /// Returns a field's box.
    ///
    /// @param field the field
    /// @return its box; empty for none
    [[nodiscard]] kit::Rect field_rect(MultiplayerMenuField field) const noexcept {
        const int32_t small_line = kit::smooth_text_points(kit::FontRole::small) + 6;
        switch (field) {
        case MultiplayerMenuField::address: {
            const kit::Rect& panel = layout_.connections;
            const int32_t left = panel.x + layout_.inset + layout_.address_label;
            return {
                left,
                panel.y + panel.height - layout_.inset - small_line - layout_.field,
                panel.x + panel.width - layout_.inset - left,
                layout_.field
            };
        }
        case MultiplayerMenuField::nickname:
        case MultiplayerMenuField::password: {
            const kit::Rect& panel = layout_.player;
            // The connection's words and box, a divider, then each field
            // under its words.
            int32_t y = panel.y + layout_.band + layout_.field_label + layout_.field + small_line +
                        2 * layout_.gap + layout_.field_label;
            if (field == MultiplayerMenuField::password)
                y += layout_.field + layout_.gap + layout_.field_label;
            return {panel.x + layout_.inset, y, panel.width - 2 * layout_.inset, layout_.field};
        }
        case MultiplayerMenuField::none:
            break;
        }
        return {};
    }

    /// Returns the rectangle of a footer button.
    ///
    /// @param part the button
    /// @return its rectangle; empty for a part that is none of them
    [[nodiscard]] kit::Rect button_rect(Part part) const noexcept {
        const bool connection = facts_now_.step == MultiplayerMenuStep::connection;
        switch (part) {
        case Part::options:
            return layout_.options;
        case Part::select:
            return layout_.select;
        case Part::back:
            return connection ? layout_.back_connection : layout_.back_games;
        case Part::new_game:
            return layout_.new_game;
        case Part::update:
            return layout_.update;
        case Part::watch:
            return layout_.watch;
        case Part::join:
            return layout_.join;
        default:
            return {};
        }
    }

    /// Returns the part under a point.
    ///
    /// @param at the point, in the screen's points
    /// @return the part; none when nothing a press reaches is there
    [[nodiscard]] Hit hit_at(kit::Point at) const noexcept {
        const auto usable = [this](Hit hit) { return enabled(hit) ? hit : Hit{}; };
        if (facts_now_.step == MultiplayerMenuStep::connection) {
            for (std::size_t connection = 0; connection < facts_now_.connections.size(); ++connection)
                if (kit::contains(connection_rect(connection), at))
                    return {Part::connection, connection};
            if (takes_address() && kit::contains(field_rect(MultiplayerMenuField::address), at))
                return {Part::address, 0};
            for (const Part part : {Part::options, Part::select, Part::back})
                if (kit::contains(button_rect(part), at))
                    return usable({part, 0});
            return {};
        }
        if (kit::contains(field_rect(MultiplayerMenuField::nickname), at))
            return {Part::nickname, 0};
        if (kit::contains(field_rect(MultiplayerMenuField::password), at))
            return usable({Part::password, 0});
        if (kit::contains(rows_view(), at))
            for (std::size_t game = 0; game < facts_now_.games.size(); ++game)
                if (kit::contains(game_rect(game), at))
                    return {Part::game, game};
        for (const Part part : {Part::new_game, Part::update, Part::watch, Part::join, Part::back})
            if (kit::contains(button_rect(part), at))
                return usable({part, 0});
        return {};
    }

    /// Tells whether a part takes a press.
    ///
    /// @param hit the part
    /// @return true when it does
    [[nodiscard]] bool enabled(Hit hit) const noexcept {
        const bool connection = facts_now_.step == MultiplayerMenuStep::connection;
        switch (hit.part) {
        case Part::none:
            return false;
        case Part::connection:
            return connection && hit.index < facts_now_.connections.size();
        case Part::address:
            return takes_address();
        case Part::options:
        case Part::back:
            return true;
        case Part::select:
            return connection && facts_now_.connection.has_value();
        case Part::game:
            return !connection && hit.index < facts_now_.games.size();
        case Part::nickname:
        case Part::new_game:
        case Part::update:
            return !connection;
        case Part::password:
            return !connection && facts_now_.password_asked;
        case Part::watch:
            return !connection && facts_now_.can_watch;
        case Part::join:
            return !connection && facts_now_.can_join;
        }
        return false;
    }

    /// Presses a button of the game's screen, and reads what it shows after.
    ///
    /// @param button the button
    void press_button(MultiplayerMenuButton button) {
        if (actions_.press)
            actions_.press(button);
        refresh();
        ++revision_;
    }

    /// Presses a part that takes a press.
    ///
    /// @param hit the part
    void press(Hit hit) {
        if (!enabled(hit))
            return;
        switch (hit.part) {
        case Part::connection:
            if (actions_.choose_connection)
                actions_.choose_connection(hit.index);
            refresh();
            if (focus_ == MultiplayerMenuField::none && takes_address())
                focus_ = MultiplayerMenuField::address;
            break;
        case Part::game:
            if (actions_.choose_game)
                actions_.choose_game(hit.index);
            refresh();
            break;
        case Part::options:
            focus_ = MultiplayerMenuField::none;
            if (actions_.options)
                actions_.options();
            break;
        case Part::select:
            press_button(MultiplayerMenuButton::select);
            break;
        case Part::back:
            press_button(MultiplayerMenuButton::back);
            break;
        case Part::new_game:
            press_button(MultiplayerMenuButton::new_game);
            break;
        case Part::update:
            press_button(MultiplayerMenuButton::update);
            break;
        case Part::watch:
            press_button(MultiplayerMenuButton::watch);
            break;
        case Part::join:
            press_button(MultiplayerMenuButton::join);
            break;
        case Part::address:
        case Part::nickname:
        case Part::password:
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

    /// Returns the chosen game's map's picture, made the first time it is
    /// drawn for the map.
    ///
    /// @return the picture; empty for none
    [[nodiscard]] const WindowIcon& map_picture() const {
        if (!map_picture_) {
            const std::string map = chosen_map_file();
            map_picture_ = std::make_unique<WindowIcon>(
                actions_.map_picture && !map.empty() ? actions_.map_picture(map) : WindowIcon{}
            );
        }
        return *map_picture_;
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
        const int32_t width =
            std::min(label_plate_width(canvas, text, layout_.label_height, band_tracking), reach - left);
        draw_label_plate(
            canvas, {left, panel.y + layout_.label_top, width, layout_.label_height}, text, band_tracking
        );
    }

    /// Draws a divider across a panel: a dark line over a lit one.
    ///
    /// @param canvas where it draws
    /// @param left its left end
    /// @param y its row
    /// @param width how long it is
    static void draw_divider(const kit::Canvas& canvas, int32_t left, int32_t y, int32_t width) {
        kit::fill_rect(canvas, {left, y, width, 1}, steel_dark_colour);
        kit::fill_rect(canvas, {left, y + 1, width, 1}, steel_light_colour);
    }

    /// Draws a text field: a sunk well with its words, or its hint in faded
    /// words, an amber ring and a caret while it holds the focus, and an
    /// edge while the pointer is over it.
    ///
    /// @param canvas where it draws
    /// @param field the field
    /// @param words what it holds
    /// @param usable it takes the focus
    void draw_field(
        const kit::Canvas& canvas, MultiplayerMenuField field, std::string_view words, bool usable
    ) const {
        const kit::Rect box = field_rect(field);
        kit::fill_rect(canvas, box, well_colour);
        draw_sunk_edge(canvas, box);
        const bool focused = focus_ == field;
        if (focused)
            draw_ring(canvas, box, 2, chosen_colour);
        else if (usable && hovered_ == Hit{part_of(field), 0})
            kit::outline_rect(canvas, box, hover_edge_colour);
        const kit::Measure regular = [&canvas](std::string_view line) {
            return kit::measure_text(canvas, kit::FontRole::regular, line);
        };
        const int32_t text_left = box.x + layout_.pad;
        const int32_t text_width = std::max(box.width - 2 * layout_.pad - 4, 1);
        std::string shown = field == MultiplayerMenuField::password
                                ? std::string(words.size(), '*')
                                : std::string(words);
        if (regular(shown) > text_width) {
            // The end shows, as it is being typed.
            while (!shown.empty() && regular("…" + shown) > text_width)
                shown.erase(0, kit::character_bytes(shown));
            shown = "…" + shown;
        }
        kit::box_text(
            canvas, kit::FontRole::regular, shown, {text_left, box.y, text_width, box.height},
            kit::Align::left, usable ? caption_lit_colour : caption_faded_colour
        );
        if (focused) {
            const int32_t caret_height = kit::smooth_text_points(kit::FontRole::regular) + 4;
            kit::fill_rect(
                canvas,
                {text_left + (shown.empty() ? 0 : regular(shown)) + 1,
                 box.y + (box.height - caret_height) / 2, 2, caret_height},
                chosen_colour
            );
        }
    }

    /// Draws the words over a field, or beside it.
    ///
    /// @param canvas where it draws
    /// @param box where they stand
    /// @param text the words, in the language shown
    /// @param usable the field takes the focus
    static void draw_field_label(
        const kit::Canvas& canvas, const kit::Rect& box, std::string_view text, bool usable
    ) {
        const kit::Measure regular = [&canvas](std::string_view line) {
            return kit::measure_text(canvas, kit::FontRole::regular, line);
        };
        outlined_text(
            canvas, kit::FontRole::regular, kit::fit(text, box.width, regular), box, kit::Align::left,
            usable ? caption_colour : caption_faded_colour
        );
    }

    /// Draws the picture a connection is shown with: a globe for one that
    /// takes an address, two joined screens for another.
    ///
    /// @param canvas where it draws
    /// @param box the square it fills
    /// @param connection the connection
    /// @param ink its colour
    static void draw_connection_icon(
        const kit::Canvas& canvas, const kit::Rect& box, const MultiplayerMenuConnection& connection,
        kit::Colour ink
    ) {
        if (connection.takes_address)
            draw_globe(canvas, box, ink);
        else
            draw_network(canvas, box, ink);
    }

    /// Draws the CONNECTION panel: a plate for each connection, the chosen
    /// one in amber, and the address field under them while the chosen
    /// connection takes one.
    ///
    /// @param canvas where it draws
    void draw_connections(const kit::Canvas& canvas) const {
        const kit::Rect& panel = layout_.connections;
        draw_panel_frame(canvas, panel, layout_.rim);
        draw_label(canvas, panel, worded("CONNECTION"));
        const kit::Measure regular = [&canvas](std::string_view line) {
            return kit::measure_text(canvas, kit::FontRole::regular, line);
        };
        const kit::Measure small = [&canvas](std::string_view line) {
            return kit::measure_text(canvas, kit::FontRole::small, line);
        };
        const int32_t small_line = kit::smooth_text_points(kit::FontRole::small) + 6;
        const kit::Rect address = field_rect(MultiplayerMenuField::address);
        const int32_t list_bottom = takes_address()
                                        ? address.y - 2 * layout_.gap
                                        : panel.y + panel.height - layout_.inset;
        const kit::Canvas inside = kit::clipped(
            canvas, {panel.x, panel.y, panel.width, std::max(list_bottom - panel.y, 1)}
        );
        if (facts_now_.connections.empty()) {
            kit::box_text(
                canvas, kit::FontRole::small, worded("No connections are available."),
                {panel.x + layout_.inset, panel.y + layout_.band, panel.width - 2 * layout_.inset,
                 small_line},
                kit::Align::left, note_colour
            );
        }
        for (std::size_t at = 0; at < facts_now_.connections.size(); ++at) {
            const MultiplayerMenuConnection& shown = facts_now_.connections[at];
            const kit::Rect plate = connection_rect(at);
            const bool chosen = facts_now_.connection && *facts_now_.connection == at;
            const Hit hit{Part::connection, at};
            const bool lit = hovered_ == hit;
            const bool held = lit && pressed_ == hit;
            draw_steel_button(inside, plate, chosen ? chosen_steel_colour : entry_steel_colour);
            kit::Rect box = plate;
            if (held) {
                box.x += 1;
                box.y += 1;
            }
            const int32_t icon = std::min(layout_.icon, box.height - 8);
            const kit::Rect picture{
                box.x + layout_.pad * 2, box.y + (box.height - icon) / 2, icon, icon
            };
            draw_connection_icon(inside, picture, shown, chosen ? chosen_text_colour : icon_colour);
            const int32_t text_left = picture.x + picture.width + layout_.pad * 2;
            const int32_t text_width = std::max(box.x + box.width - layout_.pad - text_left, 1);
            outlined_text(
                inside, kit::FontRole::regular, kit::fit(shown.name, text_width, regular),
                {text_left, box.y, text_width, box.height}, kit::Align::left,
                chosen ? chosen_text_colour : lit ? caption_lit_colour : caption_colour
            );
            if (chosen) {
                draw_glow(inside, plate, chosen_colour, layout_.glow);
                draw_ring(inside, plate, 2, chosen_colour);
            } else if (lit) {
                kit::outline_rect(inside, plate, hover_edge_colour);
            }
        }
        if (!takes_address())
            return;
        draw_divider(
            canvas, panel.x + layout_.inset, address.y - layout_.gap - 1, panel.width - 2 * layout_.inset
        );
        draw_field_label(
            canvas,
            {panel.x + layout_.inset, address.y, layout_.address_label - layout_.pad, address.height},
            worded("Address"), true
        );
        draw_field(canvas, MultiplayerMenuField::address, facts_now_.address, true);
        kit::box_text(
            canvas, kit::FontRole::small,
            kit::fit(worded("Leave blank to search the local network"), address.width, small),
            {address.x, address.y + address.height, address.width, small_line}, kit::Align::left,
            note_colour
        );
    }

    /// Draws the connection step's DETAILS panel: the chosen connection's
    /// picture, its name and what it does.
    ///
    /// @param canvas where it draws
    void draw_connection_details(const kit::Canvas& canvas) const {
        const kit::Rect& panel = layout_.details;
        draw_panel_frame(canvas, panel, layout_.rim);
        draw_label(canvas, panel, worded("DETAILS"));
        const kit::Measure regular = [&canvas](std::string_view line) {
            return kit::measure_text(canvas, kit::FontRole::regular, line);
        };
        const kit::Measure small = [&canvas](std::string_view line) {
            return kit::measure_text(canvas, kit::FontRole::small, line);
        };
        const int32_t regular_line = kit::smooth_text_points(kit::FontRole::regular) + 8;
        const int32_t small_line = kit::smooth_text_points(kit::FontRole::small) + 6;
        const kit::Rect about{
            panel.x + layout_.inset, panel.y + layout_.band, panel.width - 2 * layout_.inset,
            panel.height - layout_.band - layout_.inset
        };
        if (!facts_now_.connection || *facts_now_.connection >= facts_now_.connections.size()) {
            kit::WrapRules rules;
            int32_t y = about.y;
            for (const std::string& line :
                 kit::wrap(worded("Choose how to connect."), about.width, small, rules)) {
                kit::box_text(canvas, kit::FontRole::small, line, {about.x, y, about.width, small_line},
                              kit::Align::left, note_colour);
                y += small_line;
            }
            return;
        }
        const MultiplayerMenuConnection& shown = facts_now_.connections[*facts_now_.connection];
        int32_t y = about.y + layout_.gap * 2;
        const int32_t icon = std::min(layout_.big_icon, about.width / 2);
        draw_connection_icon(canvas, {about.x + (about.width - icon) / 2, y, icon, icon}, shown, icon_colour);
        y += icon + layout_.gap * 2;
        kit::WrapRules title_rules;
        title_rules.most_lines = 3;
        for (const std::string& line : kit::wrap(shown.name, about.width, regular, title_rules)) {
            outlined_text(canvas, kit::FontRole::regular, line, {about.x, y, about.width, regular_line},
                          kit::Align::centre, caption_lit_colour);
            y += regular_line;
        }
        y += layout_.gap;
        kit::WrapRules about_rules;
        about_rules.most_lines = most_about_lines;
        about_rules.newlines = true;
        for (const std::string& line : kit::wrap(shown.about, about.width, small, about_rules)) {
            kit::box_text(canvas, kit::FontRole::small, line, {about.x, y, about.width, small_line},
                          kit::Align::left, plate_text_colour);
            y += small_line;
        }
    }

    /// Draws the PLAYER panel: the connection the games are found over and
    /// where, the name field and the password field.
    ///
    /// @param canvas where it draws
    void draw_player(const kit::Canvas& canvas) const {
        const kit::Rect& panel = layout_.player;
        draw_panel_frame(canvas, panel, layout_.rim);
        draw_label(canvas, panel, worded("PLAYER"));
        const kit::Measure regular = [&canvas](std::string_view line) {
            return kit::measure_text(canvas, kit::FontRole::regular, line);
        };
        const kit::Measure small = [&canvas](std::string_view line) {
            return kit::measure_text(canvas, kit::FontRole::small, line);
        };
        const int32_t small_line = kit::smooth_text_points(kit::FontRole::small) + 6;
        const int32_t left = panel.x + layout_.inset;
        const int32_t width = panel.width - 2 * layout_.inset;
        int32_t y = panel.y + layout_.band;
        draw_field_label(canvas, {left, y, width, layout_.field_label}, worded("Connection"), true);
        y += layout_.field_label;
        const kit::Rect box{left, y, width, layout_.field};
        kit::fill_rect(canvas, box, well_colour);
        draw_sunk_edge(canvas, box);
        kit::box_text(
            canvas, kit::FontRole::regular,
            kit::fit(facts_now_.connection_name, width - 2 * layout_.pad, regular),
            {left + layout_.pad, y, width - 2 * layout_.pad, layout_.field}, kit::Align::left,
            caption_colour
        );
        y += layout_.field;
        kit::box_text(
            canvas, kit::FontRole::small, kit::fit(facts_now_.connection_line, width, small),
            {left, y, width, small_line}, kit::Align::left, note_colour
        );
        y += small_line + layout_.gap;
        draw_divider(canvas, left, y, width);
        const kit::Rect nickname = field_rect(MultiplayerMenuField::nickname);
        draw_field_label(
            canvas, {left, nickname.y - layout_.field_label, width, layout_.field_label},
            worded("Your name"), true
        );
        draw_field(canvas, MultiplayerMenuField::nickname, facts_now_.nickname, true);
        const kit::Rect password = field_rect(MultiplayerMenuField::password);
        draw_field_label(
            canvas, {left, password.y - layout_.field_label, width, layout_.field_label},
            worded("Password"), facts_now_.password_asked
        );
        draw_field(
            canvas, MultiplayerMenuField::password, facts_now_.password, facts_now_.password_asked
        );
        kit::WrapRules rules;
        rules.most_lines = 2;
        int32_t note_y = password.y + password.height + layout_.gap / 2;
        for (const std::string& line :
             kit::wrap(worded("Only for games that ask for one."), width, small, rules)) {
            kit::box_text(canvas, kit::FontRole::small, line, {left, note_y, width, small_line},
                          kit::Align::left, note_colour);
            note_y += small_line;
        }
    }

    /// Draws the GAMES panel: the column captions, a row for each game
    /// found, the chosen one in amber, the scroll bar while they overflow,
    /// and the line telling how many were found.
    ///
    /// @param canvas where it draws
    void draw_games(const kit::Canvas& canvas) const {
        const kit::Rect& panel = layout_.games;
        draw_panel_frame(canvas, panel, layout_.rim);
        draw_label(canvas, panel, worded("GAMES"));
        const kit::Measure regular = [&canvas](std::string_view line) {
            return kit::measure_text(canvas, kit::FontRole::regular, line);
        };
        const kit::Measure small = [&canvas](std::string_view line) {
            return kit::measure_text(canvas, kit::FontRole::small, line);
        };
        const kit::Rect header = header_rect();
        draw_steel_button(canvas, header, strip_steel_colour);
        const auto header_edges = column_edges(header);
        for (std::size_t column = 0; column < game_column_captions.size(); ++column) {
            const int32_t width = header_edges[column + 1] - header_edges[column] - layout_.pad / 2;
            outlined_text(
                canvas, kit::FontRole::small,
                kit::fit(worded(game_column_captions[column]), width, small),
                {header_edges[column], header.y, width, header.height}, kit::Align::left,
                caption_colour, caption_tracking
            );
        }
        const kit::Rect view = rows_view();
        if (facts_now_.games.empty()) {
            kit::fill_rect(canvas, view, well_colour);
            kit::box_text(
                canvas, kit::FontRole::small,
                kit::fit(worded("No games found. Press Update to look again."), view.width, small),
                view, kit::Align::centre, note_colour
            );
        } else {
            const kit::Canvas inside = kit::clipped(canvas, view);
            for (std::size_t game = 0; game < facts_now_.games.size(); ++game) {
                const kit::Rect row = game_rect(game);
                if (row.y + row.height < view.y || row.y > view.y + view.height)
                    continue;
                draw_game(inside, game, regular);
            }
        }
        draw_scroll_bar(canvas);
        const kit::Rect status = status_rect();
        kit::fill_rect(canvas, status, well_colour);
        draw_sunk_edge(canvas, status);
        const std::size_t count = facts_now_.games.size();
        const std::string found =
            count == 0   ? worded("No games found")
            : count == 1 ? worded("1 game found")
                         : filled(worded("{n} games found"), "{n}", std::to_string(count));
        kit::box_text(
            canvas, kit::FontRole::small, kit::fit(found, status.width - 2 * layout_.pad, small),
            {status.x + layout_.pad, status.y, status.width - 2 * layout_.pad, status.height},
            kit::Align::left, plate_text_colour
        );
    }

    /// Draws a game's row: its name, map, players, ping and status.
    ///
    /// @param canvas where it draws, clipped to the view
    /// @param game the game, by its place
    /// @param regular measures a line in the regular font
    void draw_game(const kit::Canvas& canvas, std::size_t game, const kit::Measure& regular) const {
        const MultiplayerMenuGame& shown = facts_now_.games[game];
        const kit::Rect row = game_rect(game);
        const bool chosen = facts_now_.game && *facts_now_.game == game;
        const bool lit = hovered_ == Hit{Part::game, game};
        kit::fill_rect(canvas, row, chosen ? chosen_steel_colour : row_colour);
        if (lit && !chosen)
            kit::blend_rect(canvas, row, chosen_colour, row_light_opacity);
        kit::outline_rect(canvas, row, row_edge_colour);
        const auto edges = column_edges(row);
        const std::array<std::string_view, 5> cells{
            shown.name, shown.map, shown.players, shown.ping, shown.status
        };
        for (std::size_t column = 0; column < cells.size(); ++column) {
            const int32_t width = edges[column + 1] - edges[column] - layout_.pad / 2;
            kit::Colour ink = column == 0 ? caption_lit_colour : caption_colour;
            if (column == 4 && !shown.open)
                ink = note_colour;
            if (chosen)
                ink = chosen_text_colour;
            outlined_text(
                canvas, kit::FontRole::regular, kit::fit(cells[column], width, regular),
                {edges[column], row.y, width, row.height}, kit::Align::left, ink
            );
        }
        if (chosen) {
            draw_glow(canvas, row, chosen_colour, layout_.glow / 2);
            draw_ring(canvas, row, 2, chosen_colour);
        }
    }

    /// Draws the games' scroll bar: a sunk well and, while the games
    /// overflow the view, a steel thumb showing the part in view.
    ///
    /// @param canvas where it draws
    void draw_scroll_bar(const kit::Canvas& canvas) const {
        const kit::Rect view = rows_view();
        const kit::Rect well{view.x + view.width + layout_.scroll_gap, view.y, layout_.scroll, view.height};
        kit::fill_rect(canvas, well, well_colour);
        draw_sunk_edge(canvas, well);
        const int32_t needed =
            static_cast<int32_t>(facts_now_.games.size()) * pitch() - layout_.row_gap;
        if (needed <= view.height)
            return;
        const int32_t thumb_height = std::max(well.height * view.height / needed, layout_.scroll * 2);
        const int32_t travel = well.height - thumb_height;
        const int32_t reach = needed - view.height;
        const int32_t thumb_y = well.y + (reach > 0 ? travel * scroll_ / reach : 0);
        draw_steel_button(canvas, {well.x + 1, thumb_y, well.width - 2, thumb_height}, steel_colour);
    }

    /// Draws the game step's DETAILS panel: the chosen game's map picture
    /// on a screen in a steel bezel, its name, its map and its rules.
    ///
    /// @param canvas where it draws
    void draw_game_details(const kit::Canvas& canvas) const {
        const kit::Rect& panel = layout_.details;
        draw_panel_frame(canvas, panel, layout_.rim);
        draw_label(canvas, panel, worded("DETAILS"));
        const kit::Measure regular = [&canvas](std::string_view line) {
            return kit::measure_text(canvas, kit::FontRole::regular, line);
        };
        const kit::Measure small = [&canvas](std::string_view line) {
            return kit::measure_text(canvas, kit::FontRole::small, line);
        };
        const int32_t regular_line = kit::smooth_text_points(kit::FontRole::regular) + 8;
        const int32_t small_line = kit::smooth_text_points(kit::FontRole::small) + 6;
        const kit::Rect about{
            panel.x + layout_.inset, panel.y + layout_.band, panel.width - 2 * layout_.inset,
            panel.height - layout_.band - layout_.inset
        };
        if (!facts_now_.game || *facts_now_.game >= facts_now_.games.size()) {
            kit::WrapRules rules;
            int32_t y = about.y;
            for (const std::string& line :
                 kit::wrap(worded("Choose a game to read about it."), about.width, small, rules)) {
                kit::box_text(canvas, kit::FontRole::small, line, {about.x, y, about.width, small_line},
                              kit::Align::left, note_colour);
                y += small_line;
            }
            return;
        }
        const MultiplayerMenuGame& shown = facts_now_.games[*facts_now_.game];
        const kit::Canvas clip = kit::clipped(canvas, about);
        int32_t y = about.y;
        const kit::Rect monitor{about.x, y, about.width, std::min(layout_.picture, about.width)};
        draw_monitor(clip, monitor);
        y += monitor.height + layout_.gap;
        kit::WrapRules title_rules;
        title_rules.most_lines = 2;
        for (const std::string& line : kit::wrap(shown.name, about.width, regular, title_rules)) {
            outlined_text(clip, kit::FontRole::regular, line, {about.x, y, about.width, regular_line},
                          kit::Align::left, caption_lit_colour);
            y += regular_line;
        }
        kit::box_text(
            clip, kit::FontRole::small, kit::fit(shown.map, about.width, small),
            {about.x, y, about.width, small_line}, kit::Align::left, chosen_text_colour
        );
        y += small_line + layout_.gap / 2;
        kit::fill_rect(clip, {about.x, y, about.width, 1}, steel_dark_colour);
        kit::fill_rect(clip, {about.x, y + 1, about.width, 1}, sunk_light_colour);
        y += layout_.gap / 2;
        const int32_t label_width = about.width * 45 / 100;
        const int32_t value_left = about.x + label_width + layout_.pad;
        const int32_t value_width = std::max(about.x + about.width - value_left, 1);
        for (const MultiplayerMenuFact& fact : shown.facts) {
            if (y + small_line > about.y + about.height)
                break;
            kit::box_text(clip, kit::FontRole::small, kit::fit(fact.label, label_width, small),
                          {about.x, y, label_width, small_line}, kit::Align::left, note_colour);
            kit::fill_rect(clip, {about.x + label_width, y + 3, 1, small_line - 6}, row_edge_colour);
            kit::box_text(clip, kit::FontRole::small, kit::fit(fact.value, value_width, small),
                          {value_left, y, value_width, small_line}, kit::Align::left, caption_colour);
            y += small_line + 2;
            kit::fill_rect(clip, {about.x, y - 1, about.width, 1}, rule_line_colour);
        }
    }

    /// Draws the map's screen: a steel bezel with a bolt in each corner
    /// round the map's picture, or round the words saying there is none.
    ///
    /// @param canvas where it draws
    /// @param monitor the screen, its bezel included
    void draw_monitor(const kit::Canvas& canvas, const kit::Rect& monitor) const {
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
        const WindowIcon& picture = map_picture();
        const kit::Rect inner{glass.x + 2, glass.y + 2, glass.width - 4, glass.height - 4};
        if (picture.width > 0 && picture.height > 0 && inner.width > 0 && inner.height > 0) {
            const double scale = std::min(
                static_cast<double>(inner.width) / picture.width,
                static_cast<double>(inner.height) / picture.height
            );
            const int32_t width = std::max(nearest(picture.width * scale), 1);
            const int32_t height = std::max(nearest(picture.height * scale), 1);
            const kit::Rect at{
                inner.x + (inner.width - width) / 2, inner.y + (inner.height - height) / 2, width,
                height
            };
            map_fitted_.draw(canvas, at, picture);
            kit::outline_rect(canvas, at, outline_colour);
        } else {
            kit::box_text(
                canvas, kit::FontRole::small, worded("No picture"), glass, kit::Align::centre,
                caption_faded_colour
            );
        }
        draw_sunk_edge(canvas, {glass.x - 2, glass.y - 2, glass.width + 4, glass.height + 4});
    }

    /// Returns a footer button's caption, in English, and its quick key.
    ///
    /// @param part the button
    /// @return the caption and the key; 0 for none
    [[nodiscard]] std::pair<std::string_view, char> footer_caption(Part part) const noexcept {
        switch (part) {
        case Part::options:
            return {"OPTIONS", facts_now_.options_key};
        case Part::select:
            return {"SELECT", facts_now_.select_key};
        case Part::back:
            return {"PREVIOUS MENU", facts_now_.back_key};
        case Part::new_game:
            return {"NEW GAME", facts_now_.new_key};
        case Part::update:
            return {"UPDATE", facts_now_.update_key};
        case Part::watch:
            return {"WATCH", facts_now_.watch_key};
        case Part::join:
            return {"JOIN", facts_now_.join_key};
        default:
            return {{}, 0};
        }
    }

    /// Draws a footer button with the main menu's button faces, its caption
    /// outlined, Previous Menu's arrow and the quick key's underline.
    ///
    /// @param canvas where it draws
    /// @param hit the button
    /// @param art the menus' art
    void draw_footer(const kit::Canvas& canvas, Hit hit, const MenuArt& art) const {
        const bool usable = enabled(hit);
        const bool lit = usable && hovered_ == hit;
        const bool held = lit && pressed_ == hit;
        const Face face = !usable ? Face::disabled : held ? Face::pressed : lit ? Face::hover : Face::normal;
        const kit::Rect rect = button_rect(hit.part);
        fitted_.draw(canvas, rect, art.faces[static_cast<std::size_t>(face)]);
        kit::Rect box = rect;
        // A held button's caption sinks a point right and down, as the
        // game's do.
        if (held) {
            box.x += 1;
            box.y += 1;
        }
        const kit::Colour ink = !usable ? caption_faded_colour : face == Face::hover ? caption_lit_colour : caption_colour;
        const auto [english, key] = footer_caption(hit.part);
        const kit::Measure regular = [&canvas](std::string_view line) {
            return kit::measure_text(canvas, kit::FontRole::regular, line);
        };
        const bool back = hit.part == Part::back;
        const int32_t arrow_room = back ? 2 * layout_.back_arrow : 0;
        const std::string text = kit::fit(worded(english), box.width - 8 - arrow_room, regular);
        outlined_text(canvas, kit::FontRole::regular, text, box, kit::Align::centre, ink);
        if (back)
            draw_arrow(
                canvas,
                {box.x + layout_.back_arrow, box.y + box.height / 2},
                std::max(kit::smooth_text_points(kit::FontRole::regular) / 2, 3),
                true,
                ink
            );
        const auto at = quick_key_at(text, key);
        if (usable && at)
            underline_quick_key(canvas, text, box, *at);
    }

    std::function<bool()> shows_{};                 ///< whether the screen shows
    std::function<MultiplayerMenuFacts()> facts_{}; ///< reads the game's screen
    MultiplayerMenuActions actions_{};              ///< what its presses do
    MultiplayerMenuFacts facts_now_{};              ///< what it shows, as last read
    kit::Point size_{};                             ///< the screen's size, in points
    MultiplayerLayout layout_{};                    ///< where its fixed parts lie
    bool laid_out_{};                               ///< layout_ is for size_
    Hit hovered_{};                                 ///< the part under the pointer
    Hit pressed_{};                                 ///< the part a press holds
    MultiplayerMenuField focus_{};                  ///< the field typed text goes to
    int32_t scroll_{};                              ///< how far the games are scrolled, in points
    FittedArt fitted_{};                            ///< the title and the faces, filtered
    mutable FittedArt map_fitted_{};                ///< the map's picture, filtered
    /// The chosen game's map's picture, made when first drawn for the map.
    mutable std::unique_ptr<WindowIcon> map_picture_{};
    uint64_t revision_{1};                          ///< counts the changes to what it draws
};

} // namespace

std::unique_ptr<LayerScreen> make_multiplayer_menu(
    std::function<bool()> shows,
    std::function<MultiplayerMenuFacts()> facts,
    MultiplayerMenuActions actions
) {
    return std::make_unique<MultiplayerMenuScreen>(
        std::move(shows), std::move(facts), std::move(actions)
    );
}

} // namespace oa::app
