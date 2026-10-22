// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Open Annihilation's column menus on the OA layer (column_menu.hpp): the
// screen the main menu and the Single Player menu are made of.

#include "column_menu.hpp"
#include "menu_paint.hpp"

#include "oa/app/window_icon.hpp"
#include "oa/data/languages/interface_text.hpp"
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
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {

namespace {

namespace kit = oa::ui::kit;
using namespace menu_paint;

// The menus' measures, in rows of a window 1080 rows high: the art is drawn
// for that window, and every other window takes them at its own height.

/// The narrowest width, in the same rows, the measures keep their size in;
/// a narrower window shrinks them to fit its width.
constexpr double design_least_columns = 1000.0;
constexpr double button_columns = 422.0;      ///< a button's width
constexpr double button_rows = 64.0;          ///< a button's height
constexpr double button_pitch = 76.0;         ///< from one button's top to the next's
constexpr double back_gap_rows = 24.0;        ///< the room a button leading back stands apart by
constexpr double heading_rows = 44.0;         ///< the heading's height
constexpr double heading_gap_columns = 18.0;  ///< between the heading and its rules
constexpr double panel_gap_columns = 44.0;    ///< between the column and the panel
constexpr double panel_columns = 430.0;       ///< the panel's width
constexpr double panel_rows = 280.0;          ///< the panel's height, at most the column's
constexpr double panel_margin_columns = 24.0; ///< the room the panel keeps from the window's edge
constexpr double panel_inset_columns = 24.0;  ///< the room inside the panel round its text
constexpr double panel_rim_columns = 11.0;    ///< the width of the steel rim round the panel
constexpr double arrow_inset_columns = 30.0;  ///< from a back button's left to its arrow
constexpr double plate_top = 968.0;           ///< the version plate's top
constexpr double plate_rows = 40.0;           ///< the version plate's height
constexpr double plate_margin_columns = 30.0; ///< the plate's room either side of its text
/// The points between the panel's lines of text, over the small font's size.
constexpr int32_t panel_line_leading = 5;

/// Where a menu's parts lie, in the screen's points.
struct MenuLayout {
    kit::Rect background{};             ///< the picture, cut by the screen's edges
    kit::Rect title{};                  ///< the title
    kit::Rect heading{};                ///< the heading; empty for none
    std::vector<kit::Rect> buttons{};   ///< the buttons, in the look's order; empty when hidden
    kit::Rect panel{};                  ///< the panel; empty when there is none or no room for it
    int32_t panel_inset{};              ///< the room inside the panel round its text
    int32_t panel_rim{};                ///< the width of the steel rim round the panel
    int32_t arrow_inset{};              ///< from a back button's left to its arrow
    int32_t heading_gap{};              ///< between the heading and its rules
    int32_t plate_top{};                ///< the version plate's top
    int32_t plate_height{};             ///< the version plate's height
    int32_t plate_margin{};             ///< its room either side of the text
};

/// Lays a menu out on a screen of a size.
///
/// @param look what the menu holds
/// @param shown which buttons show, in the look's order
/// @param width the screen's width, in points
/// @param height the screen's height, in points
/// @param title_size the title picture's width and height, in pixels
/// @return where its parts lie
MenuLayout lay_out_menu(
    const ColumnMenuLook& look,
    const std::vector<bool>& shown,
    int32_t width,
    int32_t height,
    kit::Point title_size
) {
    MenuLayout placed;
    const double columns = std::max(width, 1);
    const double rows = std::max(height, 1);
    // A point is this many rows of the 1080-row window; a window narrower
    // than design_least_columns of them takes a smaller one, centred down.
    const double unit = std::min(rows / design_rows, columns / design_least_columns);
    const double top = (rows - design_rows * unit) / 2.0;
    const double middle = columns / 2.0;

    // The picture covers the screen, cut at the edges it overhangs.
    placed.background = background_place(width, height);

    const double title_width = look.title_columns * unit;
    const double title_height = title_size.x > 0 ? title_width * static_cast<double>(title_size.y) /
                                                       static_cast<double>(title_size.x)
                                                 : 0.0;
    placed.title = {
        nearest(middle - title_width / 2.0),
        nearest(top + look.title_top * unit),
        nearest(title_width),
        nearest(title_height)
    };
    if (!look.heading.empty())
        placed.heading = {
            placed.title.x,
            nearest(top + look.heading_top * unit),
            placed.title.width,
            nearest(heading_rows * unit)
        };

    const int32_t button_width = nearest(button_columns * unit);
    const int32_t button_height = nearest(button_rows * unit);
    placed.buttons.assign(look.entries.size(), kit::Rect{});
    double row = look.first_button_top;
    bool placed_one = false;
    std::optional<kit::Rect> first;
    kit::Rect last{};
    for (std::size_t index = 0; index < look.entries.size(); ++index) {
        if (index >= shown.size() || !shown[index])
            continue;
        if (look.entries[index].back && placed_one)
            row += back_gap_rows;
        placed.buttons[index] = {
            nearest(middle) - button_width / 2, nearest(top + row * unit), button_width, button_height
        };
        if (!first)
            first = placed.buttons[index];
        last = placed.buttons[index];
        placed_one = true;
        row += button_pitch;
    }

    // The panel stands beside the column, from its top, while the window
    // has room for it.
    if (look.about_panel && first) {
        const kit::Rect panel{
            first->x + first->width + nearest(panel_gap_columns * unit),
            first->y,
            nearest(panel_columns * unit),
            std::min(nearest(panel_rows * unit), last.y + last.height - first->y)
        };
        if (panel.x + panel.width + nearest(panel_margin_columns * unit) <= width)
            placed.panel = panel;
    }
    placed.panel_inset = nearest(panel_inset_columns * unit);
    placed.panel_rim = nearest(panel_rim_columns * unit);
    placed.arrow_inset = nearest(arrow_inset_columns * unit);
    placed.heading_gap = nearest(heading_gap_columns * unit);
    placed.plate_top = nearest(top + plate_top * unit);
    placed.plate_height = std::max(nearest(plate_rows * unit), 1);
    placed.plate_margin = nearest(plate_margin_columns * unit);
    return placed;
}

/// A column menu as a screen of the OA layer.
///
/// Its points are the layer's: one point is the view's scale in window
/// pixels, from the room's top left corner, and the screen covers the room.
class MenuScreen final : public LayerScreen {
  public:

    /// Makes the screen.
    ///
    /// @param look what it holds
    /// @param shows tells whether the menu shows
    /// @param facts reads what the game's screen under it shows now
    /// @param press presses a button
    MenuScreen(
        const ColumnMenuLook& look,
        std::function<bool()> shows,
        std::function<ColumnMenuFacts()> facts,
        std::function<void(std::size_t)> press
    )
        : look_(look), shows_(std::move(shows)), facts_(std::move(facts)),
          press_(std::move(press)) {
        facts_now_ = read_facts();
        if (!enabled(focus_))
            focus_ = next_enabled(focus_, 1);
    }

    /// Returns the look's screen name.
    ///
    /// @return the name
    [[nodiscard]] std::string_view name() const override { return look_.screen_name; }

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

    /// Draws the background, the title, the heading, the buttons, the panel
    /// and the version plate.
    ///
    /// @param canvas where it draws, at the layer's scale
    void draw(const kit::Canvas& canvas) const override {
        const MenuArt& art = menu_art();
        draw_backdrop(canvas, size_, layout_.background);
        fitted_.draw(canvas, layout_.title, art.title);
        draw_heading(canvas);
        for (std::size_t index = 0; index < look_.entries.size(); ++index)
            if (shown(index))
                draw_button(canvas, index, art);
        draw_panel(canvas);
        draw_plate(canvas);
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
        relay_out();
        fitted_.clear();
    }

    /// Follows the pointer over the buttons and presses the one a press is
    /// released on. Takes every pointer, so that the game's screen under it
    /// sees none.
    ///
    /// @param kind pointer_move, pointer_down or pointer_up
    /// @param button the pointer's button, 1 for the left
    /// @param at the pointer, in the screen's points
    /// @return redraw when the look changed; none otherwise
    LayerAnswer pointer(
        ScreenInputKind kind, uint8_t button, kit::Point at, int32_t /*finger_reach*/
    ) override {
        const uint64_t before = revision_;
        const int32_t over = button_at(at);
        set_hovered(over);
        if (over >= 0 && over != focus_) {
            focus_ = over;
            ++revision_;
        }
        if (kind == ScreenInputKind::pointer_down && button == SDL_BUTTON_LEFT) {
            set_pressed(over);
        } else if (kind == ScreenInputKind::pointer_up && button == SDL_BUTTON_LEFT) {
            const int32_t held = pressed_;
            set_pressed(-1);
            if (held >= 0 && held == over) {
                press(held);
                return LayerAnswer::redraw;
            }
        }
        return revision_ != before ? LayerAnswer::redraw : LayerAnswer::none;
    }

    /// Takes a press of a quick key the look withholds, so that it goes no
    /// further. Lets every other key go on.
    ///
    /// @param sdl_key the key's SDL keycode
    /// @return none for a withheld key; pass otherwise
    LayerAnswer shortcut(uint32_t sdl_key, uint16_t /*modifiers*/) override {
        if (sdl_key < 0x80 && facts_now_.withheld_keys.find(static_cast<char>(sdl_key)) !=
                                  std::string_view::npos)
            return LayerAnswer::none;
        return LayerAnswer::pass;
    }

    /// Moves the focus with the arrows and Tab, and presses the focused
    /// button with Return and Space. Lets every other key go on.
    ///
    /// @param pressed the key
    /// @return redraw when the focus moved or a button was pressed; pass otherwise
    LayerAnswer key(kit::Key pressed, uint32_t /*sdl_key*/) override {
        switch (pressed) {
        case kit::Key::up:
        case kit::Key::back_tab:
            move_focus(-1);
            return LayerAnswer::redraw;
        case kit::Key::down:
        case kit::Key::tab:
            move_focus(1);
            return LayerAnswer::redraw;
        case kit::Key::enter:
        case kit::Key::space:
            focus_shown_ = true;
            press(focus_);
            ++revision_;
            return LayerAnswer::redraw;
        default:
            return LayerAnswer::pass;
        }
    }

    /// Takes the wheel, which turns nothing here.
    ///
    /// @return none
    LayerAnswer wheel(kit::Point /*at*/, float /*notches*/) override { return LayerAnswer::none; }

    /// Returns the buttons that show, one named control each, for automation.
    ///
    /// @return the list
    [[nodiscard]] kit::DisplayList display_list() const override {
        kit::DisplayList list;
        for (std::size_t index = 0; index < look_.entries.size(); ++index) {
            if (!shown(index))
                continue;
            kit::Control control;
            control.id = static_cast<kit::ControlId>(index);
            control.rect = layout_.buttons[index];
            control.enabled = enabled(static_cast<int32_t>(index));
            control.name =
                std::string(look_.screen_name) + "." + std::string(look_.entries[index].name);
            control.kind = kit::ControlKind::button;
            control.text = caption(index);
            list.controls.push_back(std::move(control));
            list.tab_order.push_back(static_cast<kit::ControlId>(index));
        }
        return list;
    }

    /// Returns the hovered, pressed and focused buttons.
    ///
    /// @return the interaction
    [[nodiscard]] kit::Interaction interaction() const override {
        kit::Interaction shown;
        shown.hovered = hovered_;
        shown.pressed = pressed_;
        shown.focused = focus_;
        shown.focus_shown = focus_shown_;
        return shown;
    }

    /// Closes the screen when the menu no longer shows, and follows what the
    /// game's screen under it shows: which buttons show and take a press,
    /// and the version label.
    ///
    /// @return close when the menu no longer shows; redraw when what it
    ///     shows changed; otherwise none
    LayerAnswer tick() override {
        if (!shows_())
            return LayerAnswer::close;
        const uint64_t before = revision_;
        ColumnMenuFacts now = read_facts();
        if (!(now == facts_now_)) {
            const bool moved = now.shown != facts_now_.shown;
            facts_now_ = std::move(now);
            if (moved && laid_out_)
                relay_out();
            if (!enabled(focus_))
                focus_ = next_enabled(focus_, 1);
            if (hovered_ >= 0 && !enabled(hovered_))
                hovered_ = -1;
            ++revision_;
        }
        return revision_ != before ? LayerAnswer::redraw : LayerAnswer::none;
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

    /// Reads the facts, each list as long as the look's entries.
    ///
    /// @return the facts
    [[nodiscard]] ColumnMenuFacts read_facts() const {
        ColumnMenuFacts now = facts_();
        const std::size_t count = look_.entries.size();
        now.shown.resize(count, false);
        now.enabled.resize(count, false);
        now.quick_keys.resize(count, 0);
        return now;
    }

    /// Lays the menu out again at the screen's size, for the buttons that
    /// show now.
    void relay_out() {
        const MenuArt& art = menu_art();
        layout_ = lay_out_menu(
            look_,
            facts_now_.shown,
            size_.x,
            size_.y,
            {static_cast<int32_t>(art.title.width), static_cast<int32_t>(art.title.height)}
        );
        laid_out_ = true;
        ++revision_;
    }

    /// Tells whether a button shows.
    ///
    /// @param index the button
    /// @return true when it does
    [[nodiscard]] bool shown(std::size_t index) const noexcept {
        return index < facts_now_.shown.size() && facts_now_.shown[index] &&
               index < layout_.buttons.size();
    }

    /// Tells whether a button shows and takes a press.
    ///
    /// @param index the button
    /// @return true when it does
    [[nodiscard]] bool enabled(int32_t index) const noexcept {
        if (index < 0)
            return false;
        const auto at = static_cast<std::size_t>(index);
        return at < facts_now_.enabled.size() && facts_now_.enabled[at] &&
               at < facts_now_.shown.size() && facts_now_.shown[at];
    }

    /// Returns a button's caption, in the language shown.
    ///
    /// @param index the button
    /// @return the caption
    [[nodiscard]] std::string caption(std::size_t index) const {
        return std::string(oa::data::languages::interface_text(look_.entries[index].english));
    }

    /// Returns the enabled button under a point.
    ///
    /// @param at the point, in the screen's points
    /// @return the button; -1 for none
    [[nodiscard]] int32_t button_at(kit::Point at) const noexcept {
        for (std::size_t index = 0; index < layout_.buttons.size(); ++index)
            if (enabled(static_cast<int32_t>(index)) && kit::contains(layout_.buttons[index], at))
                return static_cast<int32_t>(index);
        return -1;
    }

    /// Returns the next enabled button from one, a step at a time, round
    /// the column.
    ///
    /// @param from the button to step from
    /// @param step 1 down, -1 up
    /// @return the button; from itself when no other is enabled
    [[nodiscard]] int32_t next_enabled(int32_t from, int32_t step) const noexcept {
        const auto count = static_cast<int32_t>(look_.entries.size());
        for (int32_t moved = 1; moved <= count; ++moved) {
            const int32_t index = ((from + step * moved) % count + count) % count;
            if (enabled(index))
                return index;
        }
        return from;
    }

    /// Moves the focus a step, and shows it.
    ///
    /// @param step 1 down, -1 up
    void move_focus(int32_t step) {
        // The first key shows the focus where it is; the next ones move it.
        if (focus_shown_)
            focus_ = next_enabled(focus_, step);
        focus_shown_ = true;
        ++revision_;
    }

    /// Sets the hovered button, counting a change.
    ///
    /// @param index the button; -1 for none
    void set_hovered(int32_t index) {
        if (index == hovered_)
            return;
        hovered_ = index;
        ++revision_;
    }

    /// Sets the held button, counting a change.
    ///
    /// @param index the button; -1 for none
    void set_pressed(int32_t index) {
        if (index == pressed_)
            return;
        pressed_ = index;
        ++revision_;
    }

    /// Presses an enabled button.
    ///
    /// @param index the button
    void press(int32_t index) {
        if (!enabled(index))
            return;
        press_(static_cast<std::size_t>(index));
    }

    /// Draws the heading under the title, spaced out, with a metal rule
    /// either side of it reaching to the title's edges.
    ///
    /// @param canvas where it draws
    void draw_heading(const kit::Canvas& canvas) const {
        const kit::Rect& box = layout_.heading;
        if (look_.heading.empty() || box.width <= 0 || box.height <= 0)
            return;
        draw_ruled_heading(
            canvas, box, oa::data::languages::interface_text(look_.heading), layout_.heading_gap
        );
    }

    /// Draws one button: its face in its look, its caption outlined, the
    /// arrow of a button leading back, and the quick key's underline.
    ///
    /// @param canvas where it draws
    /// @param index the button
    /// @param art the menus' art
    void draw_button(const kit::Canvas& canvas, std::size_t index, const MenuArt& art) const {
        const auto at = static_cast<int32_t>(index);
        const bool usable = enabled(at);
        const bool held = pressed_ == at && hovered_ == at;
        const bool lit = hovered_ == at || (focus_ == at && (focus_shown_ || hovered_ < 0));
        Face face = Face::normal;
        if (!usable)
            face = Face::disabled;
        else if (held)
            face = Face::pressed;
        else if (lit)
            face = Face::hover;
        const kit::Rect& rect = layout_.buttons[index];
        fitted_.draw(canvas, rect, art.faces[static_cast<std::size_t>(face)]);

        const std::string text = caption(index);
        kit::Rect box = rect;
        // A held button's caption sinks a point right and down, as the
        // game's do.
        if (held) {
            box.x += 1;
            box.y += 1;
        }
        const kit::Colour ink = !usable ? caption_faded_colour
                                : lit   ? caption_lit_colour
                                        : caption_colour;
        outlined_text(canvas, kit::FontRole::regular, text, box, kit::Align::centre, ink);
        if (look_.entries[index].back)
            draw_back_arrow(canvas, box, ink);

        const auto key = quick_key_at(text, facts_now_.quick_keys[index]);
        if (!usable || !key)
            return;
        underline_quick_key(canvas, text, box, *key);
    }

    /// Draws the arrow pointing left near a back button's left edge, with a
    /// shadow a point right and down.
    ///
    /// @param canvas where it draws
    /// @param box the button's box, moved as its caption is
    /// @param ink the arrow's colour, the caption's
    void draw_back_arrow(const kit::Canvas& canvas, const kit::Rect& box, kit::Colour ink) const {
        const int32_t reach = std::max(kit::smooth_text_points(kit::FontRole::regular) / 2, 3);
        const int32_t tip = box.x + layout_.arrow_inset;
        draw_arrow(canvas, {tip, box.y + box.height / 2}, reach, true, ink);
    }

    /// Draws the panel beside the column: the button under the pointer, or
    /// else the focused one, its caption over a rule and what it does under
    /// it, wrapped to the panel's width.
    ///
    /// @param canvas where it draws
    void draw_panel(const kit::Canvas& canvas) const {
        const kit::Rect& panel = layout_.panel;
        if (panel.width <= 0 || panel.height <= 0)
            return;
        draw_panel_frame(canvas, panel, layout_.panel_rim);

        const int32_t told = hovered_ >= 0 ? hovered_ : focus_;
        if (told < 0 || static_cast<std::size_t>(told) >= look_.entries.size())
            return;
        const ColumnMenuEntry& entry = look_.entries[static_cast<std::size_t>(told)];
        const int32_t inset = std::max(layout_.panel_inset, 4);
        const int32_t room = panel.width - 2 * inset;
        if (room <= 0)
            return;
        const int32_t caption_rows = kit::smooth_text_points(kit::FontRole::regular) * 3;
        const kit::Rect heading{panel.x + inset, panel.y + inset, room, caption_rows};
        outlined_text(
            canvas,
            kit::FontRole::regular,
            caption(static_cast<std::size_t>(told)),
            heading,
            kit::Align::left,
            caption_colour
        );
        const int32_t rule_row = heading.y + heading.height;
        kit::fill_rect(canvas, {panel.x + inset, rule_row, room, 1}, plate_edge_colour);
        if (entry.about.empty())
            return;
        const std::string about(oa::data::languages::interface_text(entry.about));
        const kit::Measure measure = [&canvas](std::string_view line) {
            return kit::measure_text(canvas, kit::FontRole::small, line);
        };
        const int32_t pitch = kit::smooth_text_points(kit::FontRole::small) + panel_line_leading;
        int32_t row = rule_row + inset / 2 + panel_line_leading;
        for (const std::string& line : kit::wrap(about, room, measure)) {
            if (row + pitch > panel.y + panel.height - inset / 2)
                break;
            kit::box_text(
                canvas,
                kit::FontRole::small,
                line,
                {panel.x + inset, row, room, pitch},
                kit::Align::left,
                plate_text_colour
            );
            row += pitch;
        }
    }

    /// Draws the version plate under the buttons, as wide as its label.
    ///
    /// @param canvas where it draws
    void draw_plate(const kit::Canvas& canvas) const {
        draw_version_plate(
            canvas,
            size_.x,
            layout_.plate_top,
            layout_.plate_height,
            layout_.plate_margin,
            facts_now_.version
        );
    }

    ColumnMenuLook look_{};                         ///< what it holds
    std::function<bool()> shows_{};                 ///< whether the menu shows
    std::function<ColumnMenuFacts()> facts_{};      ///< reads the game's screen under it
    std::function<void(std::size_t)> press_{};      ///< presses a button
    ColumnMenuFacts facts_now_{};                   ///< what the game's screen shows, as last read
    kit::Point size_{};                             ///< the screen's size, in points
    MenuLayout layout_{};                           ///< where its parts lie
    bool laid_out_{};                               ///< layout_ is for size_
    int32_t hovered_{-1};                           ///< the button under the pointer; -1 for none
    int32_t pressed_{-1};                           ///< the button a press holds; -1 for none
    int32_t focus_{};                               ///< the button the keys act on
    bool focus_shown_{};                            ///< a key has shown the focus
    /// The pictures of the art filtered to the sizes they are drawn at.
    FittedArt fitted_{};
    uint64_t revision_{1};                          ///< counts the changes to what it draws
};

/// Tells whether two names of a GUI layout's records are the same, as the
/// game's own lookup compares them, in either case.
///
/// @param first a name
/// @param second another
/// @return true when they are
bool same_record_name(std::string_view first, std::string_view second) noexcept {
    if (first.size() != second.size())
        return false;
    const auto lower = [](char letter) {
        return letter >= 'A' && letter <= 'Z' ? static_cast<char>(letter - 'A' + 'a') : letter;
    };
    for (std::size_t index = 0; index < first.size(); ++index)
        if (lower(first[index]) != lower(second[index]))
            return false;
    return true;
}

} // namespace

ColumnMenuFacts column_menu_facts(std::size_t count) {
    ColumnMenuFacts facts;
    facts.shown.assign(count, false);
    facts.enabled.assign(count, false);
    facts.quick_keys.assign(count, 0);
    return facts;
}

std::unique_ptr<LayerScreen> make_column_menu(
    const ColumnMenuLook& look,
    std::function<bool()> shows,
    std::function<ColumnMenuFacts()> facts,
    std::function<void(std::size_t)> press
) {
    return std::make_unique<MenuScreen>(
        look, std::move(shows), std::move(facts), std::move(press)
    );
}

std::optional<std::size_t>
gui_record_named(std::span<const oa::ui::gui_layout::Gadget> gadgets, std::string_view name) {
    if (name.empty())
        return std::nullopt;
    for (std::size_t index = 0; index < gadgets.size(); ++index)
        if (same_record_name(gadgets[index].common.name, name))
            return index;
    return std::nullopt;
}

} // namespace oa::app
