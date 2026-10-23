// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Open Annihilation's mission briefing screen on the OA layer (briefing_menu.hpp).

#include "briefing_menu.hpp"
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
constexpr double panels_rows = 690.0;         ///< the panels' height together
constexpr double panorama_rows = 236.0;       ///< the panorama's panel's height
constexpr double planet_columns = 470.0;      ///< the PLANET panel's width
constexpr double panel_gap = 14.0;            ///< between two panels
constexpr double band_rows = 46.0;            ///< a panel's label band
constexpr double rim_columns = 11.0;          ///< a panel's steel rim's width
constexpr double label_left_columns = 10.0;   ///< from a panel's rim to its label plate
constexpr double label_top_rows = 5.0;        ///< from a panel's top to its label plate
constexpr double label_rows = 34.0;           ///< a label plate's height
constexpr double inset_columns = 18.0;        ///< inside a panel, round its content
constexpr double pad_columns = 14.0;          ///< inside a well, round its words
constexpr double gap_rows = 10.0;             ///< between two parts of a panel
constexpr double scroll_columns = 12.0;       ///< the briefing's scroll bar's width
constexpr double more_rows = 40.0;            ///< MORE…'s height
constexpr double more_columns = 240.0;        ///< MORE…'s width
constexpr double narration_rows = 46.0;       ///< the NARRATION switch's row
constexpr double switch_columns = 92.0;       ///< the switch's width
constexpr double ring_columns = 7.0;          ///< the planet's ring's width
constexpr double buttons_gap_rows = 18.0;     ///< between the panels and the buttons
constexpr double button_rows = 58.0;          ///< the buttons' height
constexpr double button_columns = 300.0;      ///< the buttons' width
constexpr double back_arrow_columns = 28.0;   ///< from Previous Menu's left to its arrow
constexpr double plate_top = 1004.0;          ///< the version plate's top
constexpr double plate_rows = 36.0;           ///< the version plate's height
constexpr double plate_margin_columns = 30.0; ///< the plate's room either side of its text
/// The points a line of text needs over its font's size.
constexpr int32_t text_room = 8;
/// The points between two lines of the briefing over a line's.
constexpr int32_t leading = 4;
/// The extra points after each character of a label band.
constexpr int32_t band_tracking = 2;
/// How many lines the wheel scrolls the briefing a notch.
constexpr double wheel_lines_per_notch = 2.0;
/// How many stars the planet's sky has.
constexpr std::size_t star_count = 70;

constexpr kit::Colour title_colour{232, 164, 52};      ///< the briefing's first line, amber
constexpr kit::Colour green_colour{132, 206, 96};      ///< words marked G
constexpr kit::Colour yellow_colour{255, 206, 120};    ///< words marked Y
constexpr kit::Colour red_colour{255, 128, 36};        ///< words marked R, and any other
constexpr kit::Colour chosen_colour{232, 164, 52};     ///< amber: a switch that is on
constexpr kit::Colour well_colour{12, 15, 18};         ///< a well's ground
constexpr kit::Colour sky_colour{5, 7, 11};            ///< the planet's sky
constexpr kit::Colour star_colour{150, 156, 168};      ///< a star
constexpr kit::Colour row_colour{20, 25, 29};          ///< a row's ground
constexpr kit::Colour row_edge_colour{58, 64, 66};     ///< a row's edge
constexpr kit::Colour rule_line_colour{40, 46, 50};    ///< between two facts
constexpr kit::Colour note_colour{150, 152, 144};      ///< faded words
constexpr kit::Colour lamp_off_colour{52, 30, 20};     ///< the switch's lamp, off
constexpr kit::Colour button_steel_colour{84, 90, 92}; ///< a small steel button
constexpr kit::Colour hover_edge_colour{176, 184, 184}; ///< round what the pointer is over
/// How much amber lies over a row under the pointer, in 256ths.
constexpr uint32_t row_light_opacity = 26;

/// What a press can reach.
enum class Part : uint8_t {
    none,      ///< nothing
    more,      ///< MORE… or BACK TO START
    narration, ///< the NARRATION switch
    scroll,    ///< the briefing's scroll bar
    back,      ///< PREVIOUS MENU
    start,     ///< START
};

/// A run of the briefing's words in one colour, placed on its line.
struct Piece {
    std::string text{}; ///< the words
    kit::Colour ink{};  ///< their colour
    int32_t x{};        ///< from the line's left, in points
};

/// A line of the briefing as it is drawn.
struct Line {
    std::vector<Piece> pieces{}; ///< its runs, left to right
    int32_t top{};               ///< from the briefing's top, in points
};

/// Returns the colour a highlight's code marks its words in.
///
/// @param code the character after the opening `&`
/// @return the colour
kit::Colour highlight_colour(char code) noexcept {
    switch (code) {
    case 'G':
    case 'g':
        return green_colour;
    case 'Y':
    case 'y':
        return yellow_colour;
    default:
        return red_colour;
    }
}

/// Lays the briefing out in lines no wider than a width: its first line
/// that has words is its title, in amber; a line that has none leaves half
/// a line's room; a highlight's words are in its colour.
///
/// @param text the briefing, as BriefingMenuFacts::text holds it
/// @param width the lines' width, in points
/// @param measure measures a run of words in the regular font
/// @param pitch from one line's top to the next's
/// @param[out] height how tall the lines are together
/// @return the lines
std::vector<Line> lay_out_text(
    std::string_view text, int32_t width, const kit::Measure& measure, int32_t pitch, int32_t& height
) {
    std::vector<Line> lines;
    int32_t top = 0;
    bool titled = false;
    const int32_t space = std::max(measure(" "), 1);
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = std::min(text.find('\n', start), text.size());
        const std::string_view source = text.substr(start, end - start);
        start = end + 1;
        if (source.find_first_not_of(" \t&") == std::string_view::npos) {
            if (!lines.empty())
                top += pitch / 2;
            if (end >= text.size())
                break;
            continue;
        }
        // The words, each a list of runs, a run's colour changing where a
        // highlight starts or ends inside it.
        std::vector<std::vector<Piece>> words;
        std::optional<kit::Colour> marked;
        const kit::Colour plain = titled ? caption_colour : title_colour;
        titled = true;
        bool new_word = true;
        for (std::size_t at = 0; at < source.size(); ++at) {
            const char c = source[at];
            if (c == '&') {
                if (marked) {
                    marked.reset();
                } else {
                    marked = at + 1 < source.size() ? highlight_colour(source[at + 1]) : red_colour;
                    ++at;
                }
                continue;
            }
            if (c == ' ' || c == '\t') {
                new_word = true;
                continue;
            }
            const kit::Colour ink = marked ? *marked : plain;
            if (new_word) {
                words.emplace_back();
                new_word = false;
            }
            auto& word = words.back();
            if (word.empty() || word.back().ink != ink)
                word.push_back({{}, ink, 0});
            word.back().text += c;
        }
        Line line;
        line.top = top;
        int32_t x = 0;
        const auto next_line = [&] {
            lines.push_back(std::move(line));
            top += pitch;
            line = {};
            line.top = top;
            x = 0;
        };
        for (auto& word : words) {
            int32_t word_width = 0;
            for (const Piece& piece : word)
                word_width += measure(piece.text);
            if (x > 0 && x + space + word_width > width)
                next_line();
            if (x > 0)
                x += space;
            if (word_width > width && x == 0) {
                // A word wider than a line, as a line of Chinese or Japanese
                // is, breaks where the wrap lets it, in its first colour.
                std::string joined;
                for (const Piece& piece : word)
                    joined += piece.text;
                const std::vector<std::string> parts = kit::wrap(joined, width, measure);
                for (std::size_t part = 0; part < parts.size(); ++part) {
                    line.pieces.push_back({parts[part], word.front().ink, 0});
                    if (part + 1 < parts.size())
                        next_line();
                    else
                        x = measure(parts[part]);
                }
                continue;
            }
            for (Piece& piece : word) {
                piece.x = x;
                x += measure(piece.text);
                line.pieces.push_back(std::move(piece));
            }
        }
        next_line();
        if (end >= text.size())
            break;
    }
    height = top;
    return lines;
}

/// Where the screen's fixed parts lie, in its points.
struct BriefingLayout {
    kit::Rect background{};  ///< the picture, cut by the screen's edges
    kit::Rect title{};       ///< the title
    kit::Rect heading{};     ///< the heading
    kit::Rect panorama{};    ///< the panorama's panel
    kit::Rect orders{};      ///< the ORDERS panel
    kit::Rect planet{};      ///< the PLANET panel
    kit::Rect back{};        ///< PREVIOUS MENU
    kit::Rect start{};       ///< START
    int32_t band{};          ///< a panel's label band
    int32_t inset{};         ///< inside a panel, round its content
    int32_t rim{};           ///< a panel's steel rim's width
    int32_t label_left{};    ///< from a panel's rim to its label plate
    int32_t label_top{};     ///< from a panel's top to its label plate
    int32_t label_height{};  ///< a label plate's height
    int32_t pad{};           ///< inside a well, round its words
    int32_t gap{};           ///< between two parts of a panel
    int32_t scroll{};        ///< the briefing's scroll bar's width
    int32_t more_height{};   ///< MORE…'s height
    int32_t more_width{};    ///< MORE…'s width
    int32_t narration{};     ///< the NARRATION switch's row
    int32_t switch_width{};  ///< the switch's width
    int32_t ring{};          ///< the planet's ring's width
    int32_t back_arrow{};    ///< from Previous Menu's left to its arrow
    int32_t heading_gap{};   ///< between the heading and its rules
    int32_t plate_top{};     ///< the version plate's top
    int32_t plate_height{};  ///< the version plate's height
    int32_t plate_margin{};  ///< its room either side of the text
};

/// Lays the screen out on a screen of a size.
///
/// @param width the screen's width, in points
/// @param height the screen's height, in points
/// @param title_size the title picture's width and height, in pixels
/// @return where its parts lie
BriefingLayout lay_out_briefing_menu(int32_t width, int32_t height, kit::Point title_size) {
    BriefingLayout placed;
    const double columns = std::max(width, 1);
    const double rows = std::max(height, 1);
    // A point is this many rows of the 1080-row window; a window narrower
    // than design_least_columns of them takes a smaller one, centred down.
    const double unit = std::min(rows / design_rows, columns / design_least_columns);
    const double top = (rows - design_rows * unit) / 2.0;
    const double middle = columns / 2.0;
    const auto across = [&](double design) { return nearest(design * unit); };
    const auto down = [&](double design) { return nearest(top + design * unit); };
    const int32_t small_line = kit::smooth_text_points(kit::FontRole::small) + text_room;

    placed.background = background_place(width, height);
    const double title_height = title_size.x > 0 ? title_columns * static_cast<double>(title_size.y) /
                                                       static_cast<double>(title_size.x)
                                                 : 0.0;
    placed.title = {
        nearest(middle - title_columns * unit / 2.0), down(title_top), across(title_columns),
        across(title_height)
    };
    const double heading_top = title_top + title_height + heading_gap_rows;
    placed.heading = {
        nearest(middle - heading_columns * unit / 2.0), down(heading_top), across(heading_columns),
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
    placed.scroll = std::max(across(scroll_columns), 6);
    placed.more_height = std::max(across(more_rows), small_line + 4);
    placed.more_width = across(more_columns);
    placed.narration = std::max(across(narration_rows), small_line + 8);
    placed.switch_width = across(switch_columns);
    placed.ring = std::max(across(ring_columns), 2);
    placed.back_arrow = across(back_arrow_columns);
    placed.heading_gap = across(heading_gap_columns);

    const int32_t left = nearest(middle - content_columns * unit / 2.0);
    const int32_t right = left + across(content_columns);
    const double panels_top = heading_top + heading_rows + panels_gap_rows;
    const int32_t panorama_y = down(panels_top);
    placed.panorama = {left, panorama_y, right - left, across(panorama_rows)};
    const int32_t lower_y = down(panels_top + panorama_rows + panel_gap);
    const int32_t lower_bottom = down(panels_top + panels_rows);
    const int32_t planet_width = across(planet_columns);
    placed.planet = {right - planet_width, lower_y, planet_width, lower_bottom - lower_y};
    const int32_t orders_right = placed.planet.x - across(panel_gap);
    placed.orders = {left, lower_y, orders_right - left, lower_bottom - lower_y};

    const int32_t buttons_y = down(panels_top + panels_rows + buttons_gap_rows);
    const int32_t button_height = across(button_rows);
    const int32_t button_width = across(button_columns);
    placed.back = {left, buttons_y, button_width, button_height};
    placed.start = {right - button_width, buttons_y, button_width, button_height};
    placed.plate_top = down(plate_top);
    placed.plate_height = std::max(across(plate_rows), 1);
    placed.plate_margin = across(plate_margin_columns);
    return placed;
}

/// The briefing screen as a screen of the OA layer.
///
/// Its points are the layer's: one point is the view's scale in window
/// pixels, from the room's top left corner, and the screen covers the room.
class BriefingMenuScreen final : public LayerScreen {
  public:

    /// Makes the screen.
    ///
    /// @param shows tells whether the screen shows
    /// @param facts reads what the briefing shows now
    /// @param actions what its presses do
    BriefingMenuScreen(
        std::function<bool()> shows,
        std::function<BriefingMenuFacts()> facts,
        BriefingMenuActions actions
    )
        : shows_(std::move(shows)), facts_(std::move(facts)), actions_(std::move(actions)) {
        facts_now_ = facts_();
        read_pictures();
    }

    /// Returns the screen's name.
    ///
    /// @return briefing_menu_screen_name
    [[nodiscard]] std::string_view name() const override { return briefing_menu_screen_name; }

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
        draw_ruled_heading(canvas, layout_.heading, worded("MISSION BRIEFING"), layout_.heading_gap);
        draw_panorama(canvas);
        draw_orders(canvas);
        draw_planet(canvas);
        draw_footer(canvas, Part::back, art);
        draw_footer(canvas, Part::start, art);
        draw_version_plate(
            canvas, size_.x, layout_.plate_top, layout_.plate_height, layout_.plate_margin,
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
        layout_ = lay_out_briefing_menu(
            size_.x, size_.y,
            {static_cast<int32_t>(art.title.width), static_cast<int32_t>(art.title.height)}
        );
        laid_out_ = true;
        fitted_.clear();
        panorama_fitted_.clear();
        planet_fitted_.clear();
        text_width_ = -1;
        ++revision_;
    }

    /// Follows the pointer over the parts and presses the part a press is
    /// released on; a press on the scroll bar pages the briefing at once.
    /// Takes every pointer, so that the briefing under it sees none.
    ///
    /// @param kind pointer_move, pointer_down or pointer_up
    /// @param button the pointer's button, 1 for the left
    /// @param at the pointer, in the screen's points
    /// @param finger_reach unused
    /// @return redraw when the look changed; none otherwise
    LayerAnswer pointer(
        ScreenInputKind kind, uint8_t button, kit::Point at, int32_t /*finger_reach*/
    ) override {
        const uint64_t before = revision_;
        const Part over = part_at(at);
        set_hovered(over);
        if (button != SDL_BUTTON_LEFT)
            return revision_ != before ? LayerAnswer::redraw : LayerAnswer::none;
        if (kind == ScreenInputKind::pointer_down) {
            if (over == Part::scroll) {
                page_towards(at.y);
                return LayerAnswer::redraw;
            }
            set_pressed(over);
        } else if (kind == ScreenInputKind::pointer_up) {
            const Part held = pressed_;
            set_pressed(Part::none);
            if (held != Part::none && held == over) {
                press(held);
                return LayerAnswer::redraw;
            }
        }
        return revision_ != before ? LayerAnswer::redraw : LayerAnswer::none;
    }

    /// Scrolls the briefing with the arrows, Page Up, Page Down, Home and
    /// End; lets every other key go on.
    ///
    /// @param pressed the key
    /// @return redraw when it scrolled; pass for a key it lets go on
    LayerAnswer key(kit::Key pressed, uint32_t /*sdl_key*/) override {
        switch (pressed) {
        case kit::Key::up:
        case kit::Key::down:
            scroll_by(pressed == kit::Key::down ? pitch() : -pitch());
            return LayerAnswer::redraw;
        case kit::Key::page_up:
        case kit::Key::page_down:
            scroll_by(pressed == kit::Key::page_down ? page() : -page());
            return LayerAnswer::redraw;
        case kit::Key::home:
        case kit::Key::end:
            scroll_by(pressed == kit::Key::end ? text_height_ : -text_height_);
            return LayerAnswer::redraw;
        default:
            return LayerAnswer::pass;
        }
    }

    /// Scrolls the briefing with the wheel over the ORDERS panel.
    ///
    /// @param at the pointer, in the screen's points
    /// @param notches how far the wheel turned, positive away from the player
    /// @return redraw when it scrolled; none otherwise
    LayerAnswer wheel(kit::Point at, float notches) override {
        if (!kit::contains(layout_.orders, at))
            return LayerAnswer::none;
        const int32_t before = scroll_;
        scroll_by(-nearest(notches * wheel_lines_per_notch * pitch()));
        return scroll_ != before ? LayerAnswer::redraw : LayerAnswer::none;
    }

    /// Returns the parts a press can reach, one named control each, for
    /// automation: MORE…, the NARRATION switch, PREVIOUS MENU and START.
    ///
    /// @return the list
    [[nodiscard]] kit::DisplayList display_list() const override {
        kit::DisplayList list;
        const auto add = [&](Part part, const kit::Rect& rect, std::string name, std::string text,
                             bool checked, kit::ControlKind kind) {
            kit::Control control;
            control.id = control_id(part);
            control.rect = rect;
            control.enabled = enabled(part);
            control.name = std::string(briefing_menu_screen_name) + "." + std::move(name);
            control.kind = kind;
            control.text = std::move(text);
            control.checked = checked;
            list.controls.push_back(std::move(control));
            list.tab_order.push_back(control_id(part));
        };
        add(Part::more, more_rect(), "more", worded(more_caption()), false, kit::ControlKind::button);
        add(Part::narration, narration_rect(), "narration", worded("NARRATION"), facts_now_.narration,
            kit::ControlKind::toggle);
        add(Part::back, layout_.back, "previous-menu", worded("PREVIOUS MENU"), false,
            kit::ControlKind::button);
        add(Part::start, layout_.start, "start", worded("START"), false, kit::ControlKind::button);
        return list;
    }

    /// Returns the hovered and pressed parts.
    ///
    /// @return the interaction
    [[nodiscard]] kit::Interaction interaction() const override {
        kit::Interaction shown;
        shown.hovered = hovered_ != Part::none ? control_id(hovered_) : kit::no_control;
        shown.pressed = pressed_ != Part::none ? control_id(pressed_) : kit::no_control;
        return shown;
    }

    /// Closes the screen when it no longer shows, and follows the briefing:
    /// the panorama scrolling, the planet turning and the wind wandering.
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

    /// Tells that what the screen draws changes only with its revision: the
    /// panorama and the planet move as the briefing's ticker moves them.
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
            return static_cast<int32_t>((static_cast<int64_t>(pixels) * 100 + percent - 1) / percent);
        };
        return {up(placed_on.room.width), up(placed_on.room.height)};
    }

    /// Returns a part's control number.
    ///
    /// @param part the part
    /// @return its number
    [[nodiscard]] static kit::ControlId control_id(Part part) noexcept {
        return static_cast<kit::ControlId>(part);
    }

    /// Reads the panorama's strip and the planet's frames.
    void read_pictures() {
        panorama_ = actions_.panorama ? actions_.panorama() : WindowIcon{};
        planet_frames_ = actions_.planet ? actions_.planet() : std::vector<WindowIcon>{};
        panorama_window_ = {};
        panorama_shown_ = {-1, -1, -1};
        panorama_fitted_.clear();
        planet_fitted_.clear();
    }

    /// Reads what the briefing shows, and follows it: another mission's
    /// briefing brings its own pictures and starts at its top.
    ///
    /// @return true when what the screen shows changed
    bool refresh() {
        BriefingMenuFacts now = facts_();
        if (now == facts_now_)
            return false;
        const bool other = now.text != facts_now_.text || now.planet != facts_now_.planet;
        facts_now_ = std::move(now);
        if (other) {
            read_pictures();
            text_width_ = -1;
            scroll_ = 0;
        }
        if (!enabled(hovered_))
            hovered_ = Part::none;
        ++revision_;
        return true;
    }

    /// Returns from one line's top to the next's.
    ///
    /// @return the pitch, in points
    [[nodiscard]] static int32_t pitch() noexcept {
        return kit::smooth_text_points(kit::FontRole::regular) + text_room + leading;
    }

    /// Returns how far MORE… and Page Down scroll: the view's height less a
    /// line, so that the last line seen stays in view.
    ///
    /// @return the distance, in points
    [[nodiscard]] int32_t page() const noexcept {
        return std::max(text_view().height - pitch(), pitch());
    }

    /// Returns how far the briefing scrolls at most.
    ///
    /// @return the distance, in points
    [[nodiscard]] int32_t scroll_reach() const noexcept {
        return std::max(text_height_ - text_view().height, 0);
    }

    /// Scrolls the briefing, inside what it needs.
    ///
    /// @param by how far, down for more than 0
    void scroll_by(int32_t by) {
        const int32_t to = std::clamp(scroll_ + by, 0, scroll_reach());
        if (to == scroll_)
            return;
        scroll_ = to;
        ++revision_;
    }

    /// Pages the briefing towards a point of its scroll bar.
    ///
    /// @param y the point's row
    void page_towards(int32_t y) {
        const kit::Rect thumb = thumb_rect();
        if (y < thumb.y)
            scroll_by(-page());
        else if (y >= thumb.y + thumb.height)
            scroll_by(page());
    }

    /// Tells whether the briefing is more than its view holds.
    ///
    /// @return true when it overflows
    [[nodiscard]] bool overflows() const noexcept { return scroll_reach() > 0; }

    /// Returns MORE…'s caption: BACK TO START at the briefing's end.
    ///
    /// @return the caption, in English
    [[nodiscard]] std::string_view more_caption() const noexcept {
        return overflows() && scroll_ >= scroll_reach() ? "BACK TO START" : "MORE…";
    }

    /// Returns the well the briefing lies in, in the ORDERS panel.
    ///
    /// @return the well
    [[nodiscard]] kit::Rect well_rect() const noexcept {
        const kit::Rect& panel = layout_.orders;
        return {
            panel.x + layout_.inset, panel.y + layout_.band, panel.width - 2 * layout_.inset,
            panel.height - layout_.band - layout_.inset
        };
    }

    /// Returns MORE…, centred at the well's foot.
    ///
    /// @return its box
    [[nodiscard]] kit::Rect more_rect() const noexcept {
        const kit::Rect well = well_rect();
        const int32_t width = std::min(layout_.more_width, well.width - 2 * layout_.pad);
        return {
            well.x + (well.width - layout_.scroll - layout_.pad - width) / 2,
            well.y + well.height - layout_.pad - layout_.more_height, width, layout_.more_height
        };
    }

    /// Returns where the briefing's lines scroll.
    ///
    /// @return the view
    [[nodiscard]] kit::Rect text_view() const noexcept {
        const kit::Rect well = well_rect();
        const int32_t left = well.x + layout_.pad;
        const int32_t top = well.y + layout_.pad;
        const int32_t right = well.x + well.width - 2 * layout_.pad - layout_.scroll;
        const int32_t bottom = more_rect().y - layout_.gap;
        return {left, top, std::max(right - left, 1), std::max(bottom - top, 1)};
    }

    /// Returns the briefing's scroll bar's well.
    ///
    /// @return its box
    [[nodiscard]] kit::Rect scroll_rect() const noexcept {
        const kit::Rect well = well_rect();
        return {
            well.x + well.width - layout_.pad - layout_.scroll, well.y + layout_.pad, layout_.scroll,
            well.height - 2 * layout_.pad
        };
    }

    /// Returns the scroll bar's thumb: the part of the briefing in view.
    ///
    /// @return its box; the whole well when nothing overflows
    [[nodiscard]] kit::Rect thumb_rect() const noexcept {
        const kit::Rect bar = scroll_rect();
        const int32_t view = text_view().height;
        if (text_height_ <= view)
            return bar;
        const int32_t height = std::max(bar.height * view / text_height_, layout_.scroll * 2);
        const int32_t travel = bar.height - height;
        const int32_t reach = scroll_reach();
        return {bar.x, bar.y + (reach > 0 ? travel * scroll_ / reach : 0), bar.width, height};
    }

    /// Returns the PLANET panel's content.
    ///
    /// @return its box
    [[nodiscard]] kit::Rect planet_content() const noexcept {
        const kit::Rect& panel = layout_.planet;
        return {
            panel.x + layout_.inset, panel.y + layout_.band, panel.width - 2 * layout_.inset,
            panel.height - layout_.band - layout_.inset
        };
    }

    /// Returns the NARRATION switch's row, at the PLANET panel's foot.
    ///
    /// @return its box
    [[nodiscard]] kit::Rect narration_rect() const noexcept {
        const kit::Rect about = planet_content();
        return {about.x, about.y + about.height - layout_.narration, about.width, layout_.narration};
    }

    /// Returns the height of a row of the planet's table.
    ///
    /// @return the height, in points
    [[nodiscard]] static int32_t fact_rows() noexcept {
        return kit::smooth_text_points(kit::FontRole::small) + 6 + 2;
    }

    /// Returns the planet's table, over the switch.
    ///
    /// @return its box
    [[nodiscard]] kit::Rect table_rect() const noexcept {
        const kit::Rect about = planet_content();
        const int32_t height = 5 * fact_rows();
        return {about.x, narration_rect().y - layout_.gap - height, about.width, height};
    }

    /// Returns the planet's sky, over the table.
    ///
    /// @return its box
    [[nodiscard]] kit::Rect sky_rect() const noexcept {
        const kit::Rect about = planet_content();
        return {about.x, about.y, about.width, std::max(table_rect().y - layout_.gap - about.y, 1)};
    }

    /// Returns the part under a point.
    ///
    /// @param at the point, in the screen's points
    /// @return the part; none when nothing a press reaches is there
    [[nodiscard]] Part part_at(kit::Point at) const noexcept {
        for (const Part part : {Part::more, Part::narration, Part::scroll, Part::back, Part::start})
            if (kit::contains(part_rect(part), at))
                return enabled(part) ? part : Part::none;
        return Part::none;
    }

    /// Returns where a part lies.
    ///
    /// @param part the part
    /// @return its box; empty for none
    [[nodiscard]] kit::Rect part_rect(Part part) const noexcept {
        switch (part) {
        case Part::more:
            return more_rect();
        case Part::narration:
            return narration_rect();
        case Part::scroll:
            return scroll_rect();
        case Part::back:
            return layout_.back;
        case Part::start:
            return layout_.start;
        case Part::none:
            break;
        }
        return {};
    }

    /// Tells whether a part takes a press.
    ///
    /// @param part the part
    /// @return true when it does
    [[nodiscard]] bool enabled(Part part) const noexcept {
        switch (part) {
        case Part::none:
            return false;
        case Part::more:
        case Part::scroll:
            return overflows();
        case Part::narration:
        case Part::back:
            return true;
        case Part::start:
            return facts_now_.can_start;
        }
        return false;
    }

    /// Presses a part that takes a press, and reads what the briefing shows
    /// after.
    ///
    /// @param part the part
    void press(Part part) {
        if (!enabled(part))
            return;
        switch (part) {
        case Part::more:
            if (scroll_ >= scroll_reach())
                scroll_by(-scroll_);
            else
                scroll_by(page());
            break;
        case Part::narration:
            if (actions_.narration)
                actions_.narration();
            break;
        case Part::back:
            if (actions_.back)
                actions_.back();
            break;
        case Part::start:
            if (actions_.start)
                actions_.start();
            break;
        case Part::scroll:
        case Part::none:
            break;
        }
        if (shows_())
            refresh();
        ++revision_;
    }

    /// Sets the part under the pointer, counting a change.
    ///
    /// @param part the part
    void set_hovered(Part part) {
        if (part == hovered_)
            return;
        hovered_ = part;
        ++revision_;
    }

    /// Sets the part a press holds, counting a change.
    ///
    /// @param part the part
    void set_pressed(Part part) {
        if (part == pressed_)
            return;
        pressed_ = part;
        ++revision_;
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

    /// Draws the panorama's panel: the part of the strip the briefing
    /// scrolls to, filling the frame, and the planet's name on a plate in
    /// its corner.
    ///
    /// @param canvas where it draws
    void draw_panorama(const kit::Canvas& canvas) const {
        const kit::Rect& panel = layout_.panorama;
        draw_panel_frame(canvas, panel, layout_.rim);
        const int32_t edge = layout_.rim + 2;
        const kit::Rect view{
            panel.x + edge, panel.y + edge, panel.width - 2 * edge, panel.height - 2 * edge
        };
        kit::fill_rect(canvas, view, sky_colour);
        if (panorama_.width > 0 && panorama_.height > 0 && view.width > 0 && view.height > 0) {
            // The strip keeps its height; the frame shows as many of its
            // columns as its shape takes, from the scroll on, its start
            // following its end.
            const auto strip = static_cast<int32_t>(panorama_.width);
            const auto rows = static_cast<int32_t>(panorama_.height);
            const int32_t columns = std::clamp(
                nearest(static_cast<double>(rows) * view.width / view.height), 1, strip
            );
            const int32_t first = ((facts_now_.panorama_scroll % strip) + strip) % strip;
            if (panorama_shown_[0] != first || panorama_shown_[1] != columns ||
                panorama_shown_[2] != rows) {
                panorama_window_.width = static_cast<uint32_t>(columns);
                panorama_window_.height = static_cast<uint32_t>(rows);
                panorama_window_.pixels.assign(
                    static_cast<std::size_t>(columns) * rows * window_icon_pixel_bytes, 0
                );
                for (int32_t row = 0; row < rows; ++row)
                    for (int32_t column = 0; column < columns; ++column) {
                        const int32_t from = (first + column) % strip;
                        const auto source = (static_cast<std::size_t>(row) * strip + from) *
                                            window_icon_pixel_bytes;
                        const auto target = (static_cast<std::size_t>(row) * columns + column) *
                                            window_icon_pixel_bytes;
                        std::copy_n(
                            panorama_.pixels.data() + source, window_icon_pixel_bytes,
                            panorama_window_.pixels.data() + target
                        );
                    }
                panorama_shown_ = {first, columns, rows};
                panorama_fitted_.clear();
            }
            panorama_fitted_.draw(canvas, view, panorama_window_);
        }
        draw_sunk_edge(canvas, {view.x - 2, view.y - 2, view.width + 4, view.height + 4});
        if (!facts_now_.planet.empty())
            draw_label(canvas, panel, facts_now_.planet);
    }

    /// Draws the ORDERS panel: the briefing in its well as far as it is
    /// scrolled, its scroll bar and MORE….
    ///
    /// @param canvas where it draws
    void draw_orders(const kit::Canvas& canvas) const {
        const kit::Rect& panel = layout_.orders;
        draw_panel_frame(canvas, panel, layout_.rim);
        draw_label(canvas, panel, worded("ORDERS"));
        const kit::Rect well = well_rect();
        kit::fill_rect(canvas, well, well_colour);
        draw_sunk_edge(canvas, well);
        const kit::Rect view = text_view();
        const kit::Measure regular = [&canvas](std::string_view line) {
            return kit::measure_text(canvas, kit::FontRole::regular, line);
        };
        if (text_width_ != view.width) {
            lines_ = lay_out_text(facts_now_.text, view.width, regular, pitch(), text_height_);
            text_width_ = view.width;
        }
        if (lines_.empty()) {
            kit::box_text(
                canvas, kit::FontRole::regular,
                kit::fit(worded("This mission has no briefing."), view.width, regular), view,
                kit::Align::centre, note_colour
            );
        } else {
            const int32_t scrolled = std::clamp(scroll_, 0, scroll_reach());
            const kit::Canvas inside = kit::clipped(canvas, view);
            const int32_t line_height = kit::smooth_text_points(kit::FontRole::regular) + text_room;
            for (const Line& line : lines_) {
                const int32_t y = view.y + line.top - scrolled;
                if (y + line_height < view.y || y > view.y + view.height)
                    continue;
                for (const Piece& piece : line.pieces)
                    outlined_text(
                        inside, kit::FontRole::regular, piece.text,
                        {view.x + piece.x, y, view.width - piece.x + line_height, line_height},
                        kit::Align::left, piece.ink
                    );
            }
        }
        const kit::Rect bar = scroll_rect();
        kit::fill_rect(canvas, bar, plate_colour);
        draw_sunk_edge(canvas, bar);
        if (overflows())
            draw_steel_button(canvas, thumb_rect(), steel_colour);
        if (overflows())
            draw_small_button(canvas, more_rect(), Part::more, worded(more_caption()));
    }

    /// Draws a small steel button: MORE….
    ///
    /// @param canvas where it draws
    /// @param rect the button
    /// @param part its part
    /// @param caption its words
    void draw_small_button(
        const kit::Canvas& canvas, const kit::Rect& rect, Part part, std::string_view caption
    ) const {
        const bool lit = enabled(part) && hovered_ == part;
        const bool held = lit && pressed_ == part;
        draw_steel_button(canvas, rect, button_steel_colour);
        if (lit)
            kit::outline_rect(canvas, rect, hover_edge_colour);
        kit::Rect box = rect;
        if (held) {
            box.x += 1;
            box.y += 1;
        }
        const kit::Measure secondary = [&canvas](std::string_view line) {
            return kit::measure_text(canvas, secondary_caption_font, line);
        };
        outlined_text(
            canvas, secondary_caption_font, kit::fit(caption, box.width - 8, secondary), box,
            kit::Align::centre, lit ? caption_lit_colour : caption_colour
        );
    }

    /// Draws the PLANET panel: the planet turning in a ring of steel under
    /// a sky of stars, the table of what the briefing tells of it, and the
    /// NARRATION switch.
    ///
    /// @param canvas where it draws
    void draw_planet(const kit::Canvas& canvas) const {
        const kit::Rect& panel = layout_.planet;
        draw_panel_frame(canvas, panel, layout_.rim);
        draw_label(canvas, panel, worded("PLANET"));
        draw_sky(canvas, sky_rect());
        draw_table(canvas, table_rect());
        draw_switch(canvas);
    }

    /// Draws the planet's sky: its stars, and the planet in its ring.
    ///
    /// @param canvas where it draws
    /// @param sky the sky's box
    void draw_sky(const kit::Canvas& canvas, const kit::Rect& sky) const {
        kit::fill_rect(canvas, sky, sky_colour);
        // The stars lie where a fixed sequence puts them, so that they stay
        // put from frame to frame.
        uint32_t seed = 0x2545F491U;
        for (std::size_t star = 0; star < star_count; ++star) {
            seed = seed * 1664525U + 1013904223U;
            const auto x = static_cast<int32_t>((seed >> 8) % static_cast<uint32_t>(std::max(sky.width - 2, 1)));
            seed = seed * 1664525U + 1013904223U;
            const auto y = static_cast<int32_t>((seed >> 8) % static_cast<uint32_t>(std::max(sky.height - 2, 1)));
            const uint32_t light = 60 + (seed >> 4) % 160;
            kit::blend_rect(canvas, {sky.x + 1 + x, sky.y + 1 + y, 1, 1}, star_colour, light);
        }
        const int32_t side = std::max(std::min(sky.width, sky.height) - 2 * layout_.pad, 1);
        const kit::Rect ring{sky.x + (sky.width - side) / 2, sky.y + (sky.height - side) / 2, side, side};
        if (!planet_frames_.empty()) {
            const WindowIcon& frame = planet_frames_[facts_now_.planet_frame % planet_frames_.size()];
            if (frame.width > 0 && frame.height > 0) {
                const int32_t inner = side - 2 * layout_.ring - 2;
                const double scale = std::min(
                    static_cast<double>(inner) / frame.width, static_cast<double>(inner) / frame.height
                );
                const int32_t width = std::max(nearest(frame.width * scale), 1);
                const int32_t height = std::max(nearest(frame.height * scale), 1);
                planet_fitted_.draw(
                    canvas,
                    {ring.x + (side - width) / 2, ring.y + (side - height) / 2, width, height},
                    frame
                );
            }
        }
        draw_circle(canvas, ring, layout_.ring, steel_colour);
        draw_circle(canvas, ring, 1, steel_light_colour);
        draw_circle(
            canvas,
            {ring.x + layout_.ring - 1, ring.y + layout_.ring - 1, side - 2 * layout_.ring + 2,
             side - 2 * layout_.ring + 2},
            1, steel_dark_colour
        );
        // A bolt at each of the ring's diagonals.
        const double reach = side / 2.0 - layout_.ring / 2.0;
        const int32_t bolt = std::max(layout_.ring * 2 / 5, 1);
        for (const double x_sign : {-1.0, 1.0})
            for (const double y_sign : {-1.0, 1.0})
                draw_bolt(
                    canvas,
                    {nearest(ring.x + side / 2.0 + x_sign * reach * 0.7071),
                     nearest(ring.y + side / 2.0 + y_sign * reach * 0.7071)},
                    bolt
                );
        draw_sunk_edge(canvas, sky);
    }

    /// Draws the planet's table: the planet, the side, the wind speed, the
    /// gravity and the difficulty.
    ///
    /// @param canvas where it draws
    /// @param table the table's box
    void draw_table(const kit::Canvas& canvas, const kit::Rect& table) const {
        kit::fill_rect(canvas, table, well_colour);
        draw_sunk_edge(canvas, table);
        const kit::Measure small = [&canvas](std::string_view line) {
            return kit::measure_text(canvas, kit::FontRole::small, line);
        };
        const int32_t row = fact_rows();
        const int32_t label_width = table.width * 42 / 100;
        const int32_t label_left = table.x + layout_.pad;
        const int32_t value_left = table.x + label_width + layout_.pad;
        const int32_t label_room = std::max(label_width - layout_.pad - 4, 1);
        const int32_t value_room = std::max(table.x + table.width - layout_.pad - value_left, 1);
        const std::pair<std::string_view, const std::string*> facts[] = {
            {"Planet", &facts_now_.planet},       {"Side", &facts_now_.side},
            {"Wind speed", &facts_now_.wind},     {"Gravity", &facts_now_.gravity},
            {"Difficulty", &facts_now_.difficulty},
        };
        int32_t y = table.y;
        for (std::size_t at = 0; at < std::size(facts); ++at) {
            const auto& [label, value] = facts[at];
            kit::box_text(
                canvas, kit::FontRole::small, kit::fit(worded(label), label_room, small),
                {label_left, y, label_room, row}, kit::Align::left, note_colour
            );
            kit::fill_rect(canvas, {table.x + label_width, y + 4, 1, row - 8}, row_edge_colour);
            kit::box_text(
                canvas, kit::FontRole::small, kit::fit(*value, value_room, small),
                {value_left, y, value_room, row}, kit::Align::left, caption_colour
            );
            y += row;
            if (at + 1 < std::size(facts))
                kit::fill_rect(canvas, {table.x + 4, y - 1, table.width - 8, 1}, rule_line_colour);
        }
    }

    /// Draws the NARRATION switch: its name, and a lit ON with an amber
    /// lamp while the narration plays, a faded OFF with a dark one while it
    /// does not.
    ///
    /// @param canvas where it draws
    void draw_switch(const kit::Canvas& canvas) const {
        const kit::Rect row = narration_rect();
        const bool lit = hovered_ == Part::narration;
        const bool held = lit && pressed_ == Part::narration;
        draw_steel(canvas, row, steel_colour);
        if (lit)
            kit::blend_rect(canvas, row, chosen_colour, row_light_opacity);
        const kit::Measure small = [&canvas](std::string_view line) {
            return kit::measure_text(canvas, kit::FontRole::small, line);
        };
        const int32_t margin = std::max(row.height / 5, 3);
        const int32_t switch_width = std::min(layout_.switch_width, row.width / 3);
        kit::Rect toggle{
            row.x + row.width - margin - switch_width, row.y + margin, switch_width,
            row.height - 2 * margin
        };
        const int32_t name_left = row.x + layout_.pad;
        outlined_text(
            canvas, kit::FontRole::small,
            kit::fit(worded("NARRATION"), std::max(toggle.x - name_left - layout_.pad, 1), small),
            {name_left, row.y, std::max(toggle.x - name_left, 1), row.height}, kit::Align::left,
            lit ? caption_lit_colour : caption_colour, band_tracking
        );
        kit::fill_rect(canvas, toggle, well_colour);
        draw_sunk_edge(canvas, toggle);
        if (lit)
            kit::outline_rect(canvas, toggle, chosen_colour);
        const bool on = facts_now_.narration;
        const int32_t lamp_width = std::max(toggle.height / 4, 3);
        const kit::Rect lamp{
            toggle.x + toggle.width - margin - lamp_width, toggle.y + margin / 2 + 1, lamp_width,
            std::max(toggle.height - margin - 2, 2)
        };
        kit::fill_rect(canvas, lamp, on ? chosen_colour : lamp_off_colour);
        if (on)
            draw_glow(canvas, lamp, chosen_colour, std::max(lamp_width / 2, 1));
        kit::Rect words{toggle.x, toggle.y, lamp.x - toggle.x, toggle.height};
        if (held) {
            words.x += 1;
            words.y += 1;
        }
        outlined_text(
            canvas, kit::FontRole::small, worded(on ? "ON" : "OFF"), words, kit::Align::centre,
            on ? yellow_colour : caption_faded_colour
        );
    }

    /// Draws a footer button with the main menu's button faces, its caption
    /// outlined, Previous Menu's arrow and the quick key's underline.
    ///
    /// @param canvas where it draws
    /// @param part the button
    /// @param art the menus' art
    void draw_footer(const kit::Canvas& canvas, Part part, const MenuArt& art) const {
        const bool usable = enabled(part);
        const bool lit = usable && hovered_ == part;
        const bool held = lit && pressed_ == part;
        const Face face = !usable ? Face::disabled : held ? Face::pressed : lit ? Face::hover : Face::normal;
        const kit::Rect rect = part_rect(part);
        fitted_.draw(canvas, rect, art.faces[static_cast<std::size_t>(face)]);
        kit::Rect box = rect;
        // A held button's caption sinks a point right and down, as the
        // game's do.
        if (held) {
            box.x += 1;
            box.y += 1;
        }
        const kit::Colour ink =
            !usable ? caption_faded_colour : face == Face::hover ? caption_lit_colour : caption_colour;
        const bool back = part == Part::back;
        const std::string_view english = back ? "PREVIOUS MENU" : "START";
        const char key = back ? facts_now_.back_key : facts_now_.start_key;
        const kit::Measure regular = [&canvas](std::string_view line) {
            return kit::measure_text(canvas, kit::FontRole::regular, line);
        };
        const int32_t arrow_room = back ? 2 * layout_.back_arrow : 0;
        const std::string text = kit::fit(worded(english), box.width - 8 - arrow_room, regular);
        outlined_text(canvas, kit::FontRole::regular, text, box, kit::Align::centre, ink);
        if (back)
            draw_arrow(
                canvas, {box.x + layout_.back_arrow, box.y + box.height / 2},
                std::max(kit::smooth_text_points(kit::FontRole::regular) / 2, 3), true, ink
            );
        const auto at = quick_key_at(text, key);
        if (usable && at)
            underline_quick_key(canvas, text, box, *at);
    }

    std::function<bool()> shows_{};               ///< whether the screen shows
    std::function<BriefingMenuFacts()> facts_{};  ///< reads the briefing
    BriefingMenuActions actions_{};               ///< what its presses do
    BriefingMenuFacts facts_now_{};               ///< what it shows, as last read
    kit::Point size_{};                           ///< the screen's size, in points
    BriefingLayout layout_{};                     ///< where its fixed parts lie
    bool laid_out_{};                             ///< layout_ is for size_
    Part hovered_{Part::none};                    ///< the part under the pointer
    Part pressed_{Part::none};                    ///< the part a press holds
    int32_t scroll_{};                            ///< how far the briefing is scrolled, in points
    WindowIcon panorama_{};                       ///< the panorama's whole strip
    std::vector<WindowIcon> planet_frames_{};     ///< the planet's frames
    mutable WindowIcon panorama_window_{};        ///< the part of the strip on show
    /// The panorama_window_'s first column, width and height; -1 for none.
    mutable std::array<int32_t, 3> panorama_shown_{-1, -1, -1};
    mutable std::vector<Line> lines_{};           ///< the briefing laid out
    mutable int32_t text_width_{-1};              ///< the width lines_ is laid out at; -1 for none
    mutable int32_t text_height_{};               ///< how tall lines_ is
    FittedArt fitted_{};                          ///< the title and the faces, filtered
    mutable FittedArt panorama_fitted_{};         ///< the panorama, filtered
    mutable FittedArt planet_fitted_{};           ///< the planet, filtered
    uint64_t revision_{1};                        ///< counts the changes to what it draws
};

} // namespace

std::unique_ptr<LayerScreen> make_briefing_menu(
    std::function<bool()> shows,
    std::function<BriefingMenuFacts()> facts,
    BriefingMenuActions actions
) {
    return std::make_unique<BriefingMenuScreen>(std::move(shows), std::move(facts), std::move(actions));
}

} // namespace oa::app
