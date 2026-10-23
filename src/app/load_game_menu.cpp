// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Open Annihilation's Load Game screen on the OA layer (load_game_menu.hpp).

#include "load_game_menu.hpp"
#include "menu_paint.hpp"

#include "oa/ui/kit/canvas.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/text.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
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
constexpr double design_least_columns = 1300.0;
constexpr double content_columns = 1240.0;    ///< the panels' width together
constexpr double title_top = 18.0;            ///< the title's top
constexpr double title_columns = 420.0;       ///< the title's width
constexpr double heading_gap_rows = 4.0;      ///< between the title and the heading
constexpr double heading_rows = 40.0;         ///< the heading's height
constexpr double heading_columns = 820.0;     ///< the heading's width, its rules' reach
constexpr double heading_gap_columns = 18.0;  ///< between the heading and its rules
constexpr double panels_gap_rows = 12.0;      ///< between the heading and the panels
constexpr double panels_rows = 690.0;         ///< the panels' height
constexpr double details_columns = 470.0;     ///< the DETAILS panel's width
constexpr double panel_gap_columns = 14.0;    ///< between the two panels
constexpr double band_rows = 46.0;            ///< a panel's label band
constexpr double rim_columns = 11.0;          ///< a panel's steel rim's width
constexpr double label_left_columns = 10.0;   ///< from a panel's rim to its label plate
constexpr double label_top_rows = 5.0;        ///< from a panel's top to its label plate
constexpr double label_rows = 34.0;           ///< a label plate's height
constexpr double inset_columns = 18.0;        ///< inside a panel, round its content
constexpr double pad_columns = 12.0;          ///< inside a row, round its words
constexpr double gap_rows = 10.0;             ///< between two parts of a panel
constexpr double glow_columns = 6.0;          ///< how far a glow reaches
constexpr double save_rows = 50.0;            ///< a save's row's height
constexpr double save_gap_rows = 4.0;         ///< between two saves' rows
constexpr double status_rows = 40.0;          ///< the line under the saves
constexpr double scroll_columns = 12.0;       ///< the saves' scroll bar's width
constexpr double scroll_gap_columns = 8.0;    ///< between the rows and the scroll bar
constexpr double picture_rows = 330.0;        ///< the radar's screen's height at most
constexpr double bezel_columns = 10.0;        ///< the radar's screen's steel bezel
constexpr double buttons_gap_rows = 18.0;     ///< between the panels and the buttons
constexpr double button_rows = 58.0;          ///< the buttons' height
constexpr double button_columns = 300.0;      ///< the buttons' width
constexpr double back_arrow_columns = 28.0;   ///< from Previous Menu's left to its arrow
constexpr double plate_top = 1004.0;          ///< the version plate's top
constexpr double plate_rows = 36.0;           ///< the version plate's height
constexpr double plate_margin_columns = 30.0; ///< the plate's room either side of its text
/// The points a line of text needs over its font's size.
constexpr int32_t text_room = 8;
/// The extra points after each character of a label band.
constexpr int32_t band_tracking = 2;
/// How many rows the wheel scrolls the saves a notch.
constexpr double wheel_rows_per_notch = 1.0;
/// How many lines the chosen save's name takes in DETAILS at most.
constexpr std::size_t most_title_lines = 2;

constexpr kit::Colour chosen_steel_colour{98, 72, 34};   ///< the chosen row, amber steel
constexpr kit::Colour chosen_colour{232, 164, 52};       ///< amber: the chosen
constexpr kit::Colour chosen_text_colour{255, 206, 120}; ///< the chosen's words
constexpr kit::Colour well_colour{12, 15, 18};           ///< a well's ground
constexpr kit::Colour row_colour{20, 25, 29};            ///< a row's ground
constexpr kit::Colour row_edge_colour{58, 64, 66};       ///< a row's edge
constexpr kit::Colour rule_line_colour{40, 46, 50};      ///< between two facts
constexpr kit::Colour note_colour{150, 152, 144};        ///< faded words
constexpr kit::Colour screen_colour{12, 15, 18};         ///< the radar's screen, dark gunmetal
/// How much amber lies over a row under the pointer, in 256ths.
constexpr uint32_t row_light_opacity = 26;

/// What a press can reach.
enum class Part : uint8_t {
    none, ///< nothing
    save, ///< a save's row, by its place
    back, ///< PREVIOUS MENU
    load, ///< LOAD
};

/// A part, and which save.
struct Hit {
    Part part{Part::none}; ///< what it is
    std::size_t index{};   ///< which save

    friend bool operator==(const Hit&, const Hit&) = default;
};

/// Where the screen's fixed parts lie, in its points.
struct LoadGameLayout {
    kit::Rect background{};   ///< the picture, cut by the screen's edges
    kit::Rect title{};        ///< the title
    kit::Rect heading{};      ///< the heading
    kit::Rect saves{};        ///< the SAVED GAMES panel
    kit::Rect details{};      ///< the DETAILS panel
    kit::Rect back{};         ///< PREVIOUS MENU
    kit::Rect load{};         ///< LOAD
    int32_t band{};           ///< a panel's label band
    int32_t inset{};          ///< inside a panel, round its content
    int32_t rim{};            ///< a panel's steel rim's width
    int32_t label_left{};     ///< from a panel's rim to its label plate
    int32_t label_top{};      ///< from a panel's top to its label plate
    int32_t label_height{};   ///< a label plate's height
    int32_t pad{};            ///< inside a row, round its words
    int32_t gap{};            ///< between two parts of a panel
    int32_t glow{};           ///< how far a glow reaches
    int32_t row{};            ///< a save's row's height
    int32_t row_gap{};        ///< between two saves' rows
    int32_t status{};         ///< the line under the saves
    int32_t scroll{};         ///< the saves' scroll bar's width
    int32_t scroll_gap{};     ///< between the rows and the scroll bar
    int32_t picture{};        ///< the radar's screen's height at most
    int32_t bezel{};          ///< the radar's screen's bezel
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
LoadGameLayout lay_out_load_game_menu(int32_t width, int32_t height, kit::Point title_size) {
    LoadGameLayout placed;
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
    placed.row = std::max(across(save_rows), regular_line + 2);
    placed.row_gap = std::max(across(save_gap_rows), 1);
    placed.status = std::max(across(status_rows), small_line + 4);
    placed.scroll = std::max(across(scroll_columns), 6);
    placed.scroll_gap = std::max(across(scroll_gap_columns), 2);
    placed.picture = across(picture_rows);
    placed.bezel = std::max(across(bezel_columns), 3);
    placed.back_arrow = across(back_arrow_columns);
    placed.heading_gap = across(heading_gap_columns);

    const int32_t left = nearest(middle - content_columns * unit / 2.0);
    const int32_t right = left + across(content_columns);
    const double panels_top = heading_top + heading_rows + panels_gap_rows;
    const int32_t panels_y = down(panels_top);
    const int32_t panels_height = across(panels_rows);
    const int32_t details_width = across(details_columns);
    placed.details = {right - details_width, panels_y, details_width, panels_height};
    const int32_t saves_right = placed.details.x - across(panel_gap_columns);
    placed.saves = {left, panels_y, saves_right - left, panels_height};

    const int32_t buttons_y = down(panels_top + panels_rows + buttons_gap_rows);
    const int32_t button_height = across(button_rows);
    const int32_t button_width = across(button_columns);
    placed.back = {left, buttons_y, button_width, button_height};
    placed.load = {right - button_width, buttons_y, button_width, button_height};
    placed.plate_top = down(plate_top);
    placed.plate_height = std::max(across(plate_rows), 1);
    placed.plate_margin = across(plate_margin_columns);
    return placed;
}

/// The Load Game screen as a screen of the OA layer.
///
/// Its points are the layer's: one point is the view's scale in window
/// pixels, from the room's top left corner, and the screen covers the room.
class LoadGameMenuScreen final : public LayerScreen {
  public:

    /// Makes the screen.
    ///
    /// @param shows tells whether the screen shows
    /// @param facts reads what the load dialog shows now
    /// @param actions what its presses do
    LoadGameMenuScreen(
        std::function<bool()> shows,
        std::function<LoadGameMenuFacts()> facts,
        LoadGameMenuActions actions
    )
        : shows_(std::move(shows)), facts_(std::move(facts)), actions_(std::move(actions)) {
        facts_now_ = facts_();
    }

    /// Returns the screen's name.
    ///
    /// @return load_game_menu_screen_name
    [[nodiscard]] std::string_view name() const override { return load_game_menu_screen_name; }

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
        draw_ruled_heading(canvas, layout_.heading, worded("LOAD GAME"), layout_.heading_gap);
        draw_saves(canvas);
        draw_details(canvas);
        draw_footer(canvas, Part::back, art);
        draw_footer(canvas, Part::load, art);
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
        layout_ = lay_out_load_game_menu(
            size_.x,
            size_.y,
            {static_cast<int32_t>(art.title.width), static_cast<int32_t>(art.title.height)}
        );
        laid_out_ = true;
        fitted_.clear();
        picture_fitted_.clear();
        clamp_scroll();
        ++revision_;
    }

    /// Follows the pointer over the parts and presses the part a press is
    /// released on; a second press on a save's row loads it. Takes every
    /// pointer, so that the dialog under it sees none.
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
            if (over.part == Part::save && clicks >= 2 && facts_now_.chosen &&
                *facts_now_.chosen == over.index && facts_now_.can_load) {
                set_pressed({});
                press({Part::load, 0});
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

    /// Chooses a save with the arrows, Page Up, Page Down, Home and End, and
    /// loads it with Return; lets every other key go on.
    ///
    /// @param pressed the key
    /// @return redraw when it acted; pass for a key it lets go on
    LayerAnswer key(kit::Key pressed, uint32_t /*sdl_key*/) override {
        switch (pressed) {
        case kit::Key::up:
        case kit::Key::down:
            move_choice(pressed == kit::Key::down ? 1 : -1);
            return LayerAnswer::redraw;
        case kit::Key::page_up:
        case kit::Key::page_down:
            move_choice(pressed == kit::Key::page_down ? rows_shown() : -rows_shown());
            return LayerAnswer::redraw;
        case kit::Key::home:
        case kit::Key::end:
            move_choice(pressed == kit::Key::end ? 1000000 : -1000000);
            return LayerAnswer::redraw;
        case kit::Key::enter:
            press({Part::load, 0});
            return LayerAnswer::redraw;
        default:
            return LayerAnswer::pass;
        }
    }

    /// Scrolls the saves with the wheel over them.
    ///
    /// @param at the pointer, in the screen's points
    /// @param notches how far the wheel turned, positive away from the player
    /// @return redraw when they scrolled; none otherwise
    LayerAnswer wheel(kit::Point at, float notches) override {
        if (!kit::contains(rows_view(), at))
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

    /// Returns the parts a press can reach, one named control each, for
    /// automation: the saves the view shows, PREVIOUS MENU and LOAD.
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
            control.name = std::string(load_game_menu_screen_name) + "." + std::move(name);
            control.kind = kind;
            control.text = std::move(text);
            control.checked = checked;
            list.controls.push_back(std::move(control));
            list.tab_order.push_back(control_id(hit));
        };
        const kit::Rect view = rows_view();
        for (std::size_t at = 0; at < facts_now_.saves.size(); ++at) {
            const kit::Rect row = save_rect(at);
            if (row.y + row.height <= view.y || row.y >= view.y + view.height)
                continue;
            add({Part::save, at}, row, "save-" + std::to_string(at + 1), facts_now_.saves[at],
                facts_now_.chosen && *facts_now_.chosen == at, kit::ControlKind::list_item);
        }
        add({Part::back, 0}, layout_.back, "previous-menu", worded("PREVIOUS MENU"), false,
            kit::ControlKind::button);
        add({Part::load, 0}, layout_.load, "load", worded("LOAD"), false, kit::ControlKind::button);
        return list;
    }

    /// Returns the hovered and pressed parts, and the chosen save.
    ///
    /// @return the interaction
    [[nodiscard]] kit::Interaction interaction() const override {
        kit::Interaction shown;
        shown.hovered = hovered_.part != Part::none ? control_id(hovered_) : kit::no_control;
        shown.pressed = pressed_.part != Part::none ? control_id(pressed_) : kit::no_control;
        if (facts_now_.chosen)
            shown.focused = control_id({Part::save, *facts_now_.chosen});
        return shown;
    }

    /// Closes the screen when it no longer shows, and follows what the load
    /// dialog shows.
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
        // Each kind has a hundred thousand numbers, more than the saves.
        return static_cast<kit::ControlId>(static_cast<int32_t>(hit.part) * 100000) +
               static_cast<kit::ControlId>(std::min<std::size_t>(hit.index, 99999));
    }

    /// Reads what the load dialog shows, and follows it: a new choice
    /// brings its own picture.
    ///
    /// @return true when what the screen shows changed
    bool refresh() {
        LoadGameMenuFacts now = facts_();
        if (now == facts_now_)
            return false;
        const bool new_choice = now.chosen != facts_now_.chosen || now.saves != facts_now_.saves;
        facts_now_ = std::move(now);
        if (new_choice) {
            picture_.reset();
            picture_fitted_.clear();
        }
        clamp_scroll();
        if (!enabled(hovered_))
            hovered_ = {};
        ++revision_;
        return true;
    }

    /// Moves the choice of a save by some places.
    ///
    /// @param by how many places, down for more than 0
    void move_choice(int32_t by) {
        const std::size_t count = facts_now_.saves.size();
        if (count == 0)
            return;
        const int32_t from =
            facts_now_.chosen ? static_cast<int32_t>(*facts_now_.chosen) : (by > 0 ? -1 : 0);
        const auto to = static_cast<std::size_t>(
            std::clamp(from + by, int32_t{0}, static_cast<int32_t>(count) - 1)
        );
        press({Part::save, to});
        show_save(to);
    }

    /// Scrolls the saves so that one shows whole.
    ///
    /// @param save the save, by its place
    void show_save(std::size_t save) {
        const kit::Rect view = rows_view();
        const int32_t top = static_cast<int32_t>(save) * pitch();
        if (top < scroll_)
            scroll_ = top;
        else if (top + layout_.row > scroll_ + view.height)
            scroll_ = top + layout_.row - view.height;
        clamp_scroll();
        ++revision_;
    }

    /// Returns how many saves' rows the view shows whole.
    ///
    /// @return the count, at least 1
    [[nodiscard]] int32_t rows_shown() const noexcept {
        return std::max(rows_view().height / std::max(pitch(), 1), 1);
    }

    /// Returns from one save's row's top to the next's.
    ///
    /// @return the pitch, in points
    [[nodiscard]] int32_t pitch() const noexcept { return layout_.row + layout_.row_gap; }

    /// Returns how tall the saves' rows are together.
    ///
    /// @return the height, in points
    [[nodiscard]] int32_t rows_needed() const noexcept {
        return static_cast<int32_t>(facts_now_.saves.size()) * pitch() - layout_.row_gap;
    }

    /// Keeps the saves' scroll inside what they need.
    void clamp_scroll() noexcept {
        scroll_ = std::clamp(scroll_, 0, std::max(rows_needed() - rows_view().height, 0));
    }

    /// Returns where the saves' rows scroll in the SAVED GAMES panel.
    ///
    /// @return the view
    [[nodiscard]] kit::Rect rows_view() const noexcept {
        const kit::Rect& panel = layout_.saves;
        const int32_t left = panel.x + layout_.inset;
        const int32_t top = panel.y + layout_.band;
        const int32_t bottom = status_rect().y - layout_.gap;
        const int32_t width = panel.width - 2 * layout_.inset - layout_.scroll - layout_.scroll_gap;
        return {left, top, std::max(width, 1), std::max(bottom - top, 1)};
    }

    /// Returns the line under the saves.
    ///
    /// @return its box
    [[nodiscard]] kit::Rect status_rect() const noexcept {
        const kit::Rect& panel = layout_.saves;
        return {
            panel.x + layout_.inset,
            panel.y + panel.height - layout_.inset - layout_.status,
            panel.width - 2 * layout_.inset,
            layout_.status
        };
    }

    /// Returns where a save's row lies, scrolled.
    ///
    /// @param save the save, by its place
    /// @return the row
    [[nodiscard]] kit::Rect save_rect(std::size_t save) const noexcept {
        const kit::Rect view = rows_view();
        return {
            view.x,
            view.y + static_cast<int32_t>(save) * pitch() - scroll_,
            view.width,
            layout_.row
        };
    }

    /// Returns the rectangle of a footer button.
    ///
    /// @param part the button
    /// @return its rectangle; empty for a part that is neither
    [[nodiscard]] kit::Rect button_rect(Part part) const noexcept {
        switch (part) {
        case Part::back:
            return layout_.back;
        case Part::load:
            return layout_.load;
        default:
            return {};
        }
    }

    /// Returns the part under a point.
    ///
    /// @param at the point, in the screen's points
    /// @return the part; none when nothing a press reaches is there
    [[nodiscard]] Hit hit_at(kit::Point at) const noexcept {
        if (kit::contains(rows_view(), at))
            for (std::size_t save = 0; save < facts_now_.saves.size(); ++save)
                if (kit::contains(save_rect(save), at))
                    return {Part::save, save};
        for (const Part part : {Part::back, Part::load})
            if (kit::contains(button_rect(part), at))
                return enabled({part, 0}) ? Hit{part, 0} : Hit{};
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
        case Part::save:
            return hit.index < facts_now_.saves.size();
        case Part::back:
            return true;
        case Part::load:
            return facts_now_.can_load;
        }
        return false;
    }

    /// Presses a part that takes a press, and reads what the dialog shows
    /// after.
    ///
    /// @param hit the part
    void press(Hit hit) {
        if (!enabled(hit))
            return;
        switch (hit.part) {
        case Part::save:
            if (actions_.choose)
                actions_.choose(hit.index);
            break;
        case Part::back:
            if (actions_.back)
                actions_.back();
            break;
        case Part::load:
            if (actions_.load)
                actions_.load();
            break;
        case Part::none:
            break;
        }
        refresh();
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

    /// Returns the chosen save's radar picture, made the first time it is
    /// drawn for the choice.
    ///
    /// @return the picture; empty for none
    [[nodiscard]] const WindowIcon& picture() const {
        if (!picture_)
            picture_ = std::make_unique<WindowIcon>(
                actions_.picture && facts_now_.chosen ? actions_.picture() : WindowIcon{}
            );
        return *picture_;
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

    /// Draws the SAVED GAMES panel: a row for each save, the chosen one in
    /// amber, or the words saying there is none; the scroll bar while they
    /// overflow, and the line telling how many there are.
    ///
    /// @param canvas where it draws
    void draw_saves(const kit::Canvas& canvas) const {
        const kit::Rect& panel = layout_.saves;
        draw_panel_frame(canvas, panel, layout_.rim);
        draw_label(canvas, panel, worded("SAVED GAMES"));
        const kit::Measure regular = [&canvas](std::string_view line) {
            return kit::measure_text(canvas, kit::FontRole::regular, line);
        };
        const kit::Measure small = [&canvas](std::string_view line) {
            return kit::measure_text(canvas, kit::FontRole::small, line);
        };
        const kit::Rect view = rows_view();
        if (facts_now_.saves.empty()) {
            kit::fill_rect(canvas, view, well_colour);
            kit::box_text(
                canvas, kit::FontRole::regular,
                kit::fit(worded("There are no saved games yet."), view.width, regular), view,
                kit::Align::centre, note_colour
            );
        } else {
            const kit::Canvas inside = kit::clipped(canvas, view);
            for (std::size_t save = 0; save < facts_now_.saves.size(); ++save) {
                const kit::Rect row = save_rect(save);
                if (row.y + row.height < view.y || row.y > view.y + view.height)
                    continue;
                draw_save(inside, save, regular);
            }
        }
        draw_scroll_bar(canvas);
        const kit::Rect status = status_rect();
        kit::fill_rect(canvas, status, well_colour);
        draw_sunk_edge(canvas, status);
        const std::size_t count = facts_now_.saves.size();
        const std::string counted =
            count == 0   ? worded("No saved games")
            : count == 1 ? worded("1 saved game")
                         : filled(worded("{n} saved games"), "{n}", std::to_string(count));
        kit::box_text(
            canvas, kit::FontRole::small, kit::fit(counted, status.width - 2 * layout_.pad, small),
            {status.x + layout_.pad, status.y, status.width - 2 * layout_.pad, status.height},
            kit::Align::left, plate_text_colour
        );
    }

    /// Draws a save's row: its name.
    ///
    /// @param canvas where it draws, clipped to the view
    /// @param save the save, by its place
    /// @param regular measures a line in the regular font
    void draw_save(const kit::Canvas& canvas, std::size_t save, const kit::Measure& regular) const {
        const kit::Rect row = save_rect(save);
        const bool chosen = facts_now_.chosen && *facts_now_.chosen == save;
        const bool lit = hovered_ == Hit{Part::save, save};
        kit::fill_rect(canvas, row, chosen ? chosen_steel_colour : row_colour);
        if (lit && !chosen)
            kit::blend_rect(canvas, row, chosen_colour, row_light_opacity);
        kit::outline_rect(canvas, row, row_edge_colour);
        const int32_t width = std::max(row.width - 2 * layout_.pad, 1);
        outlined_text(
            canvas, kit::FontRole::regular, kit::fit(facts_now_.saves[save], width, regular),
            {row.x + layout_.pad, row.y, width, row.height}, kit::Align::left,
            chosen ? chosen_text_colour : lit ? caption_lit_colour : caption_colour
        );
        if (chosen) {
            draw_glow(canvas, row, chosen_colour, layout_.glow / 2);
            draw_ring(canvas, row, 2, chosen_colour);
        }
    }

    /// Draws the saves' scroll bar: a sunk well and, while the saves
    /// overflow the view, a steel thumb showing the part in view.
    ///
    /// @param canvas where it draws
    void draw_scroll_bar(const kit::Canvas& canvas) const {
        const kit::Rect view = rows_view();
        const kit::Rect well{view.x + view.width + layout_.scroll_gap, view.y, layout_.scroll, view.height};
        kit::fill_rect(canvas, well, well_colour);
        draw_sunk_edge(canvas, well);
        const int32_t needed = rows_needed();
        if (needed <= view.height)
            return;
        const int32_t thumb_height = std::max(well.height * view.height / needed, layout_.scroll * 2);
        const int32_t travel = well.height - thumb_height;
        const int32_t reach = needed - view.height;
        const int32_t thumb_y = well.y + (reach > 0 ? travel * scroll_ / reach : 0);
        draw_steel_button(canvas, {well.x + 1, thumb_y, well.width - 2, thumb_height}, steel_colour);
    }

    /// Draws the DETAILS panel: the chosen save's radar picture on a screen
    /// in a steel bezel, its name and its details.
    ///
    /// @param canvas where it draws
    void draw_details(const kit::Canvas& canvas) const {
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
        if (!facts_now_.chosen || *facts_now_.chosen >= facts_now_.saves.size()) {
            kit::WrapRules rules;
            int32_t y = about.y;
            const std::string_view words = facts_now_.saves.empty()
                                               ? "Save a game during a battle to go on with it here."
                                               : "Choose a saved game to read about it.";
            for (const std::string& line : kit::wrap(worded(words), about.width, small, rules)) {
                kit::box_text(canvas, kit::FontRole::small, line, {about.x, y, about.width, small_line},
                              kit::Align::left, note_colour);
                y += small_line;
            }
            return;
        }
        const kit::Canvas clip = kit::clipped(canvas, about);
        int32_t y = about.y;
        const kit::Rect monitor{about.x, y, about.width, std::min(layout_.picture, about.width)};
        draw_monitor(clip, monitor);
        y += monitor.height + layout_.gap;
        kit::WrapRules title_rules;
        title_rules.most_lines = most_title_lines;
        for (const std::string& line :
             kit::wrap(facts_now_.saves[*facts_now_.chosen], about.width, regular, title_rules)) {
            outlined_text(clip, kit::FontRole::regular, line, {about.x, y, about.width, regular_line},
                          kit::Align::left, caption_lit_colour);
            y += regular_line;
        }
        y += layout_.gap / 2;
        kit::fill_rect(clip, {about.x, y, about.width, 1}, steel_dark_colour);
        kit::fill_rect(clip, {about.x, y + 1, about.width, 1}, sunk_light_colour);
        y += layout_.gap / 2;
        const int32_t label_width = about.width * 40 / 100;
        const int32_t value_left = about.x + label_width + layout_.pad;
        const int32_t value_width = std::max(about.x + about.width - value_left, 1);
        for (const LoadGameMenuFact& fact : facts_now_.facts) {
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

    /// Draws the radar's screen: a steel bezel with a bolt in each corner
    /// round the save's radar picture, or round the words saying there is
    /// none.
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
        const WindowIcon& shown = picture();
        const kit::Rect inner{glass.x + 2, glass.y + 2, glass.width - 4, glass.height - 4};
        if (shown.width > 0 && shown.height > 0 && inner.width > 0 && inner.height > 0) {
            const double scale = std::min(
                static_cast<double>(inner.width) / shown.width,
                static_cast<double>(inner.height) / shown.height
            );
            const int32_t width = std::max(nearest(shown.width * scale), 1);
            const int32_t height = std::max(nearest(shown.height * scale), 1);
            const kit::Rect at{
                inner.x + (inner.width - width) / 2, inner.y + (inner.height - height) / 2, width,
                height
            };
            picture_fitted_.draw(canvas, at, shown);
            kit::outline_rect(canvas, at, outline_colour);
        } else {
            kit::box_text(
                canvas, kit::FontRole::small, worded("No picture"), glass, kit::Align::centre,
                caption_faded_colour
            );
        }
        draw_sunk_edge(canvas, {glass.x - 2, glass.y - 2, glass.width + 4, glass.height + 4});
    }

    /// Draws a footer button with the main menu's button faces, its caption
    /// outlined, Previous Menu's arrow and the quick key's underline.
    ///
    /// @param canvas where it draws
    /// @param part the button
    /// @param art the menus' art
    void draw_footer(const kit::Canvas& canvas, Part part, const MenuArt& art) const {
        const Hit hit{part, 0};
        const bool usable = enabled(hit);
        const bool lit = usable && hovered_ == hit;
        const bool held = lit && pressed_ == hit;
        const Face face = !usable ? Face::disabled : held ? Face::pressed : lit ? Face::hover : Face::normal;
        const kit::Rect rect = button_rect(part);
        fitted_.draw(canvas, rect, art.faces[static_cast<std::size_t>(face)]);
        kit::Rect box = rect;
        // A held button's caption sinks a point right and down, as the
        // game's do.
        if (held) {
            box.x += 1;
            box.y += 1;
        }
        const kit::Colour ink = !usable ? caption_faded_colour : face == Face::hover ? caption_lit_colour : caption_colour;
        const bool back = part == Part::back;
        const std::string_view english = back ? "PREVIOUS MENU" : "LOAD";
        const char key = back ? facts_now_.back_key : facts_now_.load_key;
        const kit::Measure regular = [&canvas](std::string_view line) {
            return kit::measure_text(canvas, kit::FontRole::regular, line);
        };
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

    std::function<bool()> shows_{};              ///< whether the screen shows
    std::function<LoadGameMenuFacts()> facts_{}; ///< reads the load dialog
    LoadGameMenuActions actions_{};              ///< what its presses do
    LoadGameMenuFacts facts_now_{};              ///< what it shows, as last read
    kit::Point size_{};                          ///< the screen's size, in points
    LoadGameLayout layout_{};                    ///< where its fixed parts lie
    bool laid_out_{};                            ///< layout_ is for size_
    Hit hovered_{};                              ///< the part under the pointer
    Hit pressed_{};                              ///< the part a press holds
    int32_t scroll_{};                           ///< how far the saves are scrolled, in points
    FittedArt fitted_{};                         ///< the title and the faces, filtered
    mutable FittedArt picture_fitted_{};         ///< the radar picture, filtered
    /// The chosen save's radar picture, made when first drawn for the choice.
    mutable std::unique_ptr<WindowIcon> picture_{};
    uint64_t revision_{1};                       ///< counts the changes to what it draws
};

} // namespace

std::unique_ptr<LayerScreen> make_load_game_menu(
    std::function<bool()> shows,
    std::function<LoadGameMenuFacts()> facts,
    LoadGameMenuActions actions
) {
    return std::make_unique<LoadGameMenuScreen>(
        std::move(shows), std::move(facts), std::move(actions)
    );
}

} // namespace oa::app
