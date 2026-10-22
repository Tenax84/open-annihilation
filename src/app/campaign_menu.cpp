// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Open Annihilation's New Campaign screen on the OA layer (campaign_menu.hpp).

#include "campaign_menu.hpp"
#include "menu_paint.hpp"

#include "oa/data/languages/interface_text.hpp"
#include "oa/ui/kit/canvas.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/text.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
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

// The screen's measures, in rows of a window 1080 rows high, as the column
// menus' are.

/// The narrowest width, in the same rows, the measures keep their size in;
/// a narrower window shrinks them to fit its width.
constexpr double design_least_columns = 1200.0;
constexpr double content_columns = 1120.0;     ///< the panels' width together
constexpr double title_top = 22.0;             ///< the title's top
constexpr double title_columns = 520.0;        ///< the title's width
constexpr double heading_gap_rows = 6.0;       ///< between the title and the heading
constexpr double heading_rows = 44.0;          ///< the heading's height
constexpr double heading_columns = 900.0;      ///< the heading's width, its rules' reach
constexpr double heading_gap_columns = 18.0;   ///< between the heading and its rules
constexpr double strip_gap_rows = 12.0;        ///< between the heading and the side strip
constexpr double strip_rows = 282.0;           ///< the side strip's height
constexpr double band_rows = 46.0;             ///< a panel's label band
constexpr double rim_columns = 11.0;           ///< a panel's steel rim's width
constexpr double label_left_columns = 10.0;    ///< from a panel's rim to its label plate
constexpr double label_top_rows = 5.0;         ///< from a panel's top to its label plate
constexpr double label_rows = 34.0;            ///< a label plate's height
constexpr double bezel_columns = 10.0;         ///< a tile's screen's steel bezel
constexpr double glow_columns = 6.0;           ///< how far a glow reaches
constexpr double tile_columns = 214.0;         ///< a side's tile's width
constexpr double tile_gap_columns = 22.0;      ///< between two tiles
constexpr double screen_rows = 160.0;          ///< a tile's screen's height
constexpr double name_gap_rows = 6.0;          ///< between a tile's screen and its name plate
constexpr double name_rows = 38.0;             ///< a tile's name plate's height
constexpr double arrow_columns = 46.0;         ///< a strip arrow's width
constexpr double arrow_rows = 100.0;           ///< a strip arrow's height
constexpr double arrow_inset_columns = 14.0;   ///< from the strip's edge to its arrow
constexpr double arrow_gap_columns = 12.0;     ///< between an arrow and the tiles
constexpr double grid_columns = 14.0;          ///< the pitch of a screen's grid
constexpr double emblem_margin_columns = 8.0;  ///< round a side's emblem in its screen
constexpr double lower_gap_rows = 20.0;        ///< between the strip and the panels under it
constexpr double lower_rows = 284.0;           ///< the campaign list's and panel's height
constexpr double list_columns = 664.0;         ///< the campaign list's width
constexpr double panel_gap_columns = 24.0;     ///< between the list and the panel
constexpr double inset_columns = 22.0;         ///< inside a panel, round its content
constexpr double row_rows = 40.0;              ///< a campaign row's height, at least
constexpr double scroll_columns = 18.0;        ///< the list's scroll bar's width
constexpr double level_rows = 46.0;            ///< a difficulty button's height
constexpr double level_gap_columns = 12.0;     ///< between two difficulty buttons
constexpr double buttons_gap_rows = 30.0;      ///< between the panels and the buttons
constexpr double button_columns = 380.0;       ///< Previous Menu's and Start's width
constexpr double button_rows = 64.0;           ///< their height
constexpr double button_gap_columns = 40.0;    ///< between them
constexpr double back_arrow_columns = 30.0;    ///< from Previous Menu's left to its arrow
constexpr double plate_top = 968.0;            ///< the version plate's top
constexpr double plate_rows = 40.0;            ///< the version plate's height
constexpr double plate_margin_columns = 30.0;  ///< the plate's room either side of its text
/// The points a row's text needs over the regular font's size.
constexpr int32_t row_text_room = 10;
/// The points between the panel's lines of text, over the small font's size.
constexpr int32_t line_leading = 5;
/// The most lines the panel gives the first mission's name.
constexpr std::size_t most_mission_lines = 3;
/// The extra points after each character of a label band.
constexpr int32_t band_tracking = 2;
/// How many rows Page Up and Page Down move.
constexpr std::size_t page_rows = 5;

constexpr kit::Colour screen_colour{12, 15, 18};       ///< a tile's screen, dark gunmetal
constexpr kit::Colour grid_colour{44, 52, 60};         ///< its grid
constexpr kit::Colour emblem_colour{184, 200, 214};    ///< a side's emblem, steel lit cold
constexpr kit::Colour emblem_chosen_colour{246, 184, 80}; ///< the chosen side's emblem, amber
constexpr kit::Colour list_colour{12, 15, 18};         ///< the campaign list's ground
constexpr kit::Colour row_rule_colour{32, 38, 44};     ///< between its rows
constexpr kit::Colour row_chosen_colour{96, 66, 24};   ///< the chosen row
constexpr kit::Colour row_chosen_edge{240, 176, 70};   ///< its edge
constexpr kit::Colour row_glow_colour{232, 164, 52};   ///< the glow round it
constexpr kit::Colour row_hover_colour{28, 34, 40};    ///< the row under the pointer
constexpr kit::Colour row_text_colour{196, 204, 210};  ///< a row's text
constexpr kit::Colour row_chosen_text{255, 242, 214};  ///< the chosen row's text
constexpr kit::Colour button_steel_colour{84, 90, 92}; ///< a small steel button's face
constexpr kit::Colour chosen_colour{232, 164, 52};     ///< round the chosen side and difficulty
constexpr kit::Colour chosen_steel_colour{124, 88, 34}; ///< the chosen side's plate and difficulty
constexpr kit::Colour hover_edge_colour{176, 184, 184}; ///< round what the pointer is over
constexpr kit::Colour fade_colour{6, 8, 10};           ///< over a side without a campaign
constexpr kit::Colour track_colour{16, 20, 24};        ///< the scroll bar's track
constexpr kit::Colour thumb_colour{84, 90, 92};        ///< its thumb
/// The opacity of the shade over a side without a campaign, in 256ths.
constexpr uint32_t fade_opacity = 150;
/// The opacity of the light across the middle of the chosen row, in 256ths.
constexpr uint32_t row_glow_opacity = 60;
/// The brightest channel at and under which an emblem's pixel stays the
/// screen's ground, so that the glow round a logo leaves no square.
constexpr uint32_t emblem_floor = 24;
/// The brightest channel at and over which an emblem's pixel takes its ink
/// whole, as a logo's lines do.
constexpr uint32_t emblem_peak = 110;
/// The brightest channel over which an emblem's pixel is part of the logo
/// itself, its lines and the grid within them, rather than the glow round
/// it; the emblem is cut to these pixels.
constexpr uint32_t emblem_edge = 64;
/// The opacity of the grid over a side's emblem, in 256ths.
constexpr uint32_t grid_opacity = 70;

/// What a press can reach.
enum class Part : uint8_t {
    none,       ///< nothing
    side,       ///< a side's tile, by its place
    earlier,    ///< the strip's left arrow
    later,      ///< the strip's right arrow
    campaign,   ///< a row of the list, by its row
    scroll_up,  ///< the scroll bar's track over its thumb
    scroll_down, ///< the scroll bar's track under its thumb
    row_up,     ///< the scroll bar's up arrow
    row_down,   ///< the scroll bar's down arrow
    difficulty, ///< a difficulty button, 0 to 2
    back,       ///< Previous Menu
    start,      ///< Start
};

/// A part, and which of its kind.
struct Hit {
    Part part{Part::none}; ///< what it is
    std::size_t index{};   ///< which side, row or difficulty

    friend bool operator==(const Hit&, const Hit&) = default;
};

/// Where the screen's parts lie, in its points.
struct CampaignLayout {
    kit::Rect background{};               ///< the picture, cut by the screen's edges
    kit::Rect title{};                    ///< the title
    kit::Rect heading{};                  ///< the heading
    kit::Rect strip{};                    ///< the side strip
    kit::Rect strip_band{};               ///< its label band
    kit::Rect earlier{};                  ///< its left arrow
    kit::Rect later{};                    ///< its right arrow
    kit::Rect tiles{};                    ///< the room for the tiles between the arrows
    int32_t tile_width{};                 ///< a tile's width
    int32_t tile_gap{};                   ///< between two tiles
    int32_t screen_height{};              ///< a tile's screen's height
    int32_t name_gap{};                   ///< between a tile's screen and its name plate
    int32_t name_height{};                ///< a tile's name plate's height
    int32_t grid{};                       ///< the pitch of a screen's grid
    int32_t emblem_margin{};              ///< round a side's emblem
    kit::Rect list{};                     ///< the campaign list's panel
    kit::Rect list_band{};                ///< its label band
    kit::Rect rows{};                     ///< where its rows lie
    int32_t row_height{};                 ///< a row's height
    int32_t scroll_width{};               ///< the scroll bar's width
    kit::Rect about{};                    ///< the panel telling about the campaign
    std::array<kit::Rect, 3> levels{};    ///< the difficulty buttons
    kit::Rect back{};                     ///< Previous Menu
    kit::Rect start{};                    ///< Start
    int32_t inset{};                      ///< inside a panel, round its content
    int32_t rim{};                        ///< a panel's steel rim's width
    int32_t label_left{};                 ///< from a panel's rim to its label plate
    int32_t label_top{};                  ///< from a panel's top to its label plate
    int32_t label_height{};               ///< a label plate's height
    int32_t bezel{};                      ///< a tile's screen's steel bezel
    int32_t glow{};                       ///< how far a glow reaches
    int32_t back_arrow{};                 ///< from Previous Menu's left to its arrow
    int32_t heading_gap{};                ///< between the heading and its rules
    int32_t plate_top{};                  ///< the version plate's top
    int32_t plate_height{};               ///< the version plate's height
    int32_t plate_margin{};               ///< its room either side of the text
};

/// Lays the screen out on a screen of a size.
///
/// @param width the screen's width, in points
/// @param height the screen's height, in points
/// @param title_size the title picture's width and height, in pixels
/// @return where its parts lie
CampaignLayout lay_out_campaign_menu(int32_t width, int32_t height, kit::Point title_size) {
    CampaignLayout placed;
    const double columns = std::max(width, 1);
    const double rows = std::max(height, 1);
    // A point is this many rows of the 1080-row window; a window narrower
    // than design_least_columns of them takes a smaller one, centred down.
    const double unit = std::min(rows / design_rows, columns / design_least_columns);
    const double top = (rows - design_rows * unit) / 2.0;
    const double middle = columns / 2.0;
    const auto across = [&](double design) { return nearest(design * unit); };
    const auto down = [&](double design) { return nearest(top + design * unit); };

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

    const int32_t left = nearest(middle - content_columns * unit / 2.0);
    const int32_t content = across(content_columns);
    const double strip_top = heading_top + heading_rows + strip_gap_rows;
    placed.strip = {left, down(strip_top), content, across(strip_rows)};
    placed.strip_band = {left, placed.strip.y, content, across(band_rows)};
    const int32_t tiles_top = placed.strip_band.y + placed.strip_band.height;
    const int32_t tiles_height = placed.strip.y + placed.strip.height - tiles_top;
    const int32_t arrow_width = across(arrow_columns);
    const int32_t arrow_height = across(arrow_rows);
    const int32_t arrow_top = tiles_top + (tiles_height - arrow_height) / 2;
    placed.earlier = {left + across(arrow_inset_columns), arrow_top, arrow_width, arrow_height};
    placed.later = {
        left + content - across(arrow_inset_columns) - arrow_width,
        arrow_top,
        arrow_width,
        arrow_height
    };
    const int32_t tiles_left = placed.earlier.x + arrow_width + across(arrow_gap_columns);
    placed.tiles = {
        tiles_left, tiles_top, placed.later.x - across(arrow_gap_columns) - tiles_left, tiles_height
    };
    placed.tile_width = std::max(across(tile_columns), 1);
    placed.tile_gap = across(tile_gap_columns);
    placed.screen_height = across(screen_rows);
    placed.name_gap = across(name_gap_rows);
    placed.name_height = across(name_rows);
    placed.grid = std::max(across(grid_columns), 4);
    placed.emblem_margin = across(emblem_margin_columns);

    const double lower_top = strip_top + strip_rows + lower_gap_rows;
    placed.list = {left, down(lower_top), across(list_columns), across(lower_rows)};
    placed.list_band = {left, placed.list.y, placed.list.width, across(band_rows)};
    placed.inset = across(inset_columns);
    placed.rows = {
        left + placed.inset,
        placed.list_band.y + placed.list_band.height,
        placed.list.width - 2 * placed.inset,
        placed.list.y + placed.list.height - placed.inset - placed.list_band.y -
            placed.list_band.height
    };
    placed.row_height = std::max(
        across(row_rows), kit::smooth_text_points(kit::FontRole::regular) + row_text_room
    );
    placed.scroll_width = std::max(across(scroll_columns), 6);
    const int32_t about_left = left + placed.list.width + across(panel_gap_columns);
    placed.about = {about_left, placed.list.y, left + content - about_left, placed.list.height};
    const int32_t level_height = across(level_rows);
    const int32_t level_gap = across(level_gap_columns);
    const int32_t level_width = (placed.about.width - 2 * placed.inset - 2 * level_gap) / 3;
    const int32_t level_top = placed.about.y + placed.about.height - placed.inset - level_height;
    for (std::size_t level = 0; level < placed.levels.size(); ++level)
        placed.levels[level] = {
            placed.about.x + placed.inset + static_cast<int32_t>(level) * (level_width + level_gap),
            level_top,
            level_width,
            level_height
        };

    const double buttons_top = lower_top + lower_rows + buttons_gap_rows;
    const int32_t button_width = across(button_columns);
    const int32_t button_height = across(button_rows);
    const int32_t button_gap = across(button_gap_columns);
    const int32_t buttons_left = nearest(middle) - button_width - button_gap / 2;
    placed.back = {buttons_left, down(buttons_top), button_width, button_height};
    placed.start = {
        buttons_left + button_width + button_gap, placed.back.y, button_width, button_height
    };
    placed.rim = across(rim_columns);
    placed.label_left = across(label_left_columns);
    placed.label_top = across(label_top_rows);
    placed.label_height = std::max(across(label_rows), 1);
    placed.bezel = std::max(across(bezel_columns), 3);
    placed.glow = std::max(across(glow_columns), 2);
    placed.back_arrow = across(back_arrow_columns);
    placed.heading_gap = across(heading_gap_columns);
    placed.plate_top = down(plate_top);
    placed.plate_height = std::max(across(plate_rows), 1);
    placed.plate_margin = across(plate_margin_columns);
    return placed;
}

/// The New Campaign screen as a screen of the OA layer.
///
/// Its points are the layer's: one point is the view's scale in window
/// pixels, from the room's top left corner, and the screen covers the room.
class CampaignMenuScreen final : public LayerScreen {
  public:

    /// Makes the screen.
    ///
    /// @param shows tells whether the screen shows
    /// @param facts reads what NEWGAME.GUI and the setup show now
    /// @param actions what its presses do
    CampaignMenuScreen(
        std::function<bool()> shows,
        std::function<CampaignMenuFacts()> facts,
        CampaignMenuActions actions
    )
        : shows_(std::move(shows)), facts_(std::move(facts)), actions_(std::move(actions)) {
        facts_now_ = facts_();
    }

    /// Returns the screen's name.
    ///
    /// @return new_campaign_menu_screen_name
    [[nodiscard]] std::string_view name() const override { return new_campaign_menu_screen_name; }

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

    /// Draws the background, the title, the heading, the side strip, the
    /// campaign list, the panel about the campaign, the buttons and the
    /// version plate.
    ///
    /// @param canvas where it draws, at the layer's scale
    void draw(const kit::Canvas& canvas) const override {
        const MenuArt& art = menu_art();
        draw_backdrop(canvas, size_, layout_.background);
        fitted_.draw(canvas, layout_.title, art.title);
        draw_ruled_heading(canvas, layout_.heading, worded("NEW CAMPAIGN"), layout_.heading_gap);
        draw_strip(canvas);
        draw_list(canvas);
        draw_about(canvas);
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
        layout_ = lay_out_campaign_menu(
            size_.x,
            size_.y,
            {static_cast<int32_t>(art.title.width), static_cast<int32_t>(art.title.height)}
        );
        laid_out_ = true;
        fitted_.clear();
        emblem_fitted_.clear();
        keep_side_in_view();
        keep_row_in_view();
        ++revision_;
    }

    /// Follows the pointer over the parts and presses the one a press is
    /// released on. Takes every pointer, so that NEWGAME.GUI under it sees
    /// none.
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
        if (kind == ScreenInputKind::pointer_down && button == SDL_BUTTON_LEFT) {
            set_pressed(over);
        } else if (kind == ScreenInputKind::pointer_up && button == SDL_BUTTON_LEFT) {
            const Hit held = pressed_;
            set_pressed({});
            if (held.part != Part::none && held == over) {
                press(held);
                return LayerAnswer::redraw;
            }
        }
        return revision_ != before ? LayerAnswer::redraw : LayerAnswer::none;
    }

    /// Chooses the side with Left and Right, the campaign with Up, Down,
    /// Page Up, Page Down, Home and End, and starts with Return and Space.
    /// Lets every other key go on.
    ///
    /// @param pressed the key
    /// @return redraw when it acted; pass otherwise
    LayerAnswer key(kit::Key pressed, uint32_t /*sdl_key*/) override {
        const std::size_t rows = facts_now_.campaigns.size();
        const std::size_t row = facts_now_.campaign;
        switch (pressed) {
        case kit::Key::left:
            step_side(-1);
            return LayerAnswer::redraw;
        case kit::Key::right:
            step_side(1);
            return LayerAnswer::redraw;
        case kit::Key::up:
            if (row > 0)
                choose_row(row - 1);
            return LayerAnswer::redraw;
        case kit::Key::down:
            if (row + 1 < rows)
                choose_row(row + 1);
            return LayerAnswer::redraw;
        case kit::Key::page_up:
            if (rows > 0)
                choose_row(row > page_rows ? row - page_rows : 0);
            return LayerAnswer::redraw;
        case kit::Key::page_down:
            if (rows > 0)
                choose_row(std::min(row + page_rows, rows - 1));
            return LayerAnswer::redraw;
        case kit::Key::home:
            if (rows > 0)
                choose_row(0);
            return LayerAnswer::redraw;
        case kit::Key::end:
            if (rows > 0)
                choose_row(rows - 1);
            return LayerAnswer::redraw;
        case kit::Key::enter:
        case kit::Key::space:
            press({Part::start, 0});
            return LayerAnswer::redraw;
        default:
            return LayerAnswer::pass;
        }
    }

    /// Scrolls the campaign list under the pointer, or pages the side strip
    /// under it.
    ///
    /// @param at the pointer, in the screen's points
    /// @param notches how far the wheel turned, away from the player positive
    /// @return redraw when something moved; none otherwise
    LayerAnswer wheel(kit::Point at, float notches) override {
        const int32_t steps = static_cast<int32_t>(std::lround(notches));
        if (steps == 0)
            return LayerAnswer::none;
        const uint64_t before = revision_;
        if (kit::contains(layout_.list, at)) {
            scroll_rows(-steps);
        } else if (kit::contains(layout_.strip, at)) {
            scroll_sides(-steps);
        }
        if (revision_ != before)
            set_hovered(hit_at(at));
        return revision_ != before ? LayerAnswer::redraw : LayerAnswer::none;
    }

    /// Returns the parts a press can reach, one named control each, for
    /// automation: the sides as side-1 and on, the strip's arrows, the
    /// campaign rows shown as campaign-1 and on, the difficulties, Previous
    /// Menu and Start.
    ///
    /// @return the list
    [[nodiscard]] kit::DisplayList display_list() const override {
        kit::DisplayList list;
        const auto add = [&](Hit hit, const kit::Rect& rect, std::string name, kit::ControlKind kind,
                             std::string text, bool checked) {
            kit::Control control;
            control.id = control_id(hit);
            control.rect = rect;
            control.enabled = enabled(hit);
            control.name = std::string(new_campaign_menu_screen_name) + "." + std::move(name);
            control.kind = kind;
            control.text = std::move(text);
            control.checked = checked;
            list.controls.push_back(std::move(control));
            list.tab_order.push_back(control_id(hit));
        };
        for (std::size_t side = first_side_; side < facts_now_.sides.size(); ++side) {
            const auto tile = tile_rect(side);
            if (!tile)
                break;
            add({Part::side, side},
                *tile,
                "side-" + std::to_string(side + 1),
                kit::ControlKind::button,
                facts_now_.sides[side].name,
                side == facts_now_.side);
        }
        if (strip_overflows()) {
            add({Part::earlier, 0}, layout_.earlier, "side-earlier", kit::ControlKind::button, "<",
                false);
            add({Part::later, 0}, layout_.later, "side-later", kit::ControlKind::button, ">", false);
        }
        for (std::size_t row = first_row_; row < facts_now_.campaigns.size(); ++row) {
            const auto rect = row_rect(row);
            if (!rect)
                break;
            add({Part::campaign, row},
                *rect,
                "campaign-" + std::to_string(row + 1),
                kit::ControlKind::list_item,
                facts_now_.campaigns[row],
                row == facts_now_.campaign);
        }
        constexpr std::array<std::string_view, 3> level_names{
            "difficulty-easy", "difficulty-medium", "difficulty-hard"
        };
        for (std::size_t level = 0; level < layout_.levels.size(); ++level)
            add({Part::difficulty, level},
                layout_.levels[level],
                std::string(level_names[level]),
                kit::ControlKind::button,
                facts_now_.difficulties[level],
                level == facts_now_.difficulty);
        add({Part::back, 0}, layout_.back, "previous-menu", kit::ControlKind::button,
            worded("PREVIOUS MENU"), false);
        add({Part::start, 0}, layout_.start, "start", kit::ControlKind::button, worded("START"),
            false);
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
    /// NEWGAME.GUI and the setup show.
    ///
    /// @return close when the screen no longer shows; redraw when what it
    ///     shows changed; otherwise none
    LayerAnswer tick() override {
        if (!shows_())
            return LayerAnswer::close;
        CampaignMenuFacts now = facts_();
        if (now == facts_now_)
            return LayerAnswer::none;
        const bool side_moved = now.side != facts_now_.side || now.sides != facts_now_.sides;
        const bool list_changed = now.campaigns != facts_now_.campaigns;
        const bool row_moved = now.campaign != facts_now_.campaign;
        if (now.sides.size() != facts_now_.sides.size()) {
            emblems_.clear();
            emblem_fitted_.clear();
        }
        facts_now_ = std::move(now);
        if (list_changed)
            first_row_ = 0;
        if (side_moved)
            keep_side_in_view();
        if (list_changed || row_moved)
            keep_row_in_view();
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
        // Each kind has a thousand numbers, more than any list it has.
        return static_cast<kit::ControlId>(static_cast<int32_t>(hit.part) * 1000) +
               static_cast<kit::ControlId>(std::min<std::size_t>(hit.index, 999));
    }

    /// Returns how many tiles the strip has room for.
    ///
    /// @return the count, at least 1
    [[nodiscard]] std::size_t tile_room() const noexcept {
        const int32_t pitch = layout_.tile_width + layout_.tile_gap;
        return static_cast<std::size_t>(
            std::max((layout_.tiles.width + layout_.tile_gap) / std::max(pitch, 1), 1)
        );
    }

    /// Tells whether the strip has more sides than room for them.
    ///
    /// @return true when it does
    [[nodiscard]] bool strip_overflows() const noexcept {
        return facts_now_.sides.size() > tile_room();
    }

    /// Returns where a side's tile lies: the tiles in view are centred
    /// between the arrows.
    ///
    /// @param side the side
    /// @return the tile; none when it is out of view
    [[nodiscard]] std::optional<kit::Rect> tile_rect(std::size_t side) const noexcept {
        const std::size_t room = tile_room();
        if (side < first_side_ || side >= first_side_ + room || side >= facts_now_.sides.size())
            return std::nullopt;
        const auto shown = static_cast<int32_t>(
            std::min(room, facts_now_.sides.size() - first_side_)
        );
        const int32_t width = shown * layout_.tile_width + (shown - 1) * layout_.tile_gap;
        const int32_t left = layout_.tiles.x + (layout_.tiles.width - width) / 2;
        const int32_t height = layout_.screen_height + layout_.name_gap + layout_.name_height;
        const auto place = static_cast<int32_t>(side - first_side_);
        return kit::Rect{
            left + place * (layout_.tile_width + layout_.tile_gap),
            layout_.tiles.y + (layout_.tiles.height - height) / 2,
            layout_.tile_width,
            height
        };
    }

    /// Returns how many rows the list shows at once.
    ///
    /// @return the count, at least 1
    [[nodiscard]] std::size_t row_room() const noexcept {
        return static_cast<std::size_t>(
            std::max(layout_.rows.height / std::max(layout_.row_height, 1), 1)
        );
    }

    /// Tells whether the list has more campaigns than rows to show them.
    ///
    /// @return true when it does
    [[nodiscard]] bool list_overflows() const noexcept {
        return facts_now_.campaigns.size() > row_room();
    }

    /// Returns where the list's rows lie, the scroll bar's room left out
    /// when it shows.
    ///
    /// @return the room
    [[nodiscard]] kit::Rect row_area() const noexcept {
        kit::Rect area = layout_.rows;
        if (list_overflows())
            area.width -= layout_.scroll_width + 4;
        return area;
    }

    /// Returns where a campaign's row lies.
    ///
    /// @param row the row
    /// @return the row; none when it is out of view
    [[nodiscard]] std::optional<kit::Rect> row_rect(std::size_t row) const noexcept {
        if (row < first_row_ || row >= first_row_ + row_room() ||
            row >= facts_now_.campaigns.size())
            return std::nullopt;
        const kit::Rect area = row_area();
        return kit::Rect{
            area.x,
            area.y + static_cast<int32_t>(row - first_row_) * layout_.row_height,
            area.width,
            layout_.row_height
        };
    }

    /// Returns the scroll bar's track.
    ///
    /// @return the track; empty when the list does not overflow
    [[nodiscard]] kit::Rect track_rect() const noexcept {
        if (!list_overflows())
            return {};
        return {
            layout_.rows.x + layout_.rows.width - layout_.scroll_width,
            layout_.rows.y,
            layout_.scroll_width,
            static_cast<int32_t>(row_room()) * layout_.row_height
        };
    }

    /// Returns one of the scroll bar's arrows, at the ends of its track.
    ///
    /// @param down the lower one; otherwise the upper one
    /// @return the arrow; empty when the list does not overflow
    [[nodiscard]] kit::Rect step_rect(bool down) const noexcept {
        const kit::Rect bar = track_rect();
        if (bar.height < 3 * bar.width)
            return {};
        return {
            bar.x, down ? bar.y + bar.height - bar.width : bar.y, bar.width, bar.width
        };
    }

    /// Returns where the scroll bar's thumb moves: its track between its
    /// arrows.
    ///
    /// @return the room; empty when the list does not overflow
    [[nodiscard]] kit::Rect travel_rect() const noexcept {
        kit::Rect travel = track_rect();
        if (step_rect(false).height > 0) {
            travel.y += travel.width;
            travel.height -= 2 * travel.width;
        }
        return travel;
    }

    /// Returns the scroll bar's thumb, as long against the room it moves in
    /// as the rows shown are against the list.
    ///
    /// @return the thumb; empty when the list does not overflow
    [[nodiscard]] kit::Rect thumb_rect() const noexcept {
        const kit::Rect track = travel_rect();
        if (track.height <= 0)
            return {};
        const double count = static_cast<double>(facts_now_.campaigns.size());
        const int32_t length = std::max(
            nearest(track.height * static_cast<double>(row_room()) / count), layout_.scroll_width
        );
        const int32_t top = track.y + nearest((track.height - length) *
                                              static_cast<double>(first_row_) /
                                              std::max(count - static_cast<double>(row_room()), 1.0));
        return {track.x, top, track.width, length};
    }

    /// Returns the part under a point.
    ///
    /// @param at the point, in the screen's points
    /// @return the part; none when nothing a press reaches is there
    [[nodiscard]] Hit hit_at(kit::Point at) const noexcept {
        for (std::size_t side = first_side_; side < facts_now_.sides.size(); ++side) {
            const auto tile = tile_rect(side);
            if (!tile)
                break;
            if (kit::contains(*tile, at))
                return enabled({Part::side, side}) ? Hit{Part::side, side} : Hit{};
        }
        if (strip_overflows()) {
            if (kit::contains(layout_.earlier, at))
                return enabled({Part::earlier, 0}) ? Hit{Part::earlier, 0} : Hit{};
            if (kit::contains(layout_.later, at))
                return enabled({Part::later, 0}) ? Hit{Part::later, 0} : Hit{};
        }
        for (std::size_t row = first_row_; row < facts_now_.campaigns.size(); ++row) {
            const auto rect = row_rect(row);
            if (!rect)
                break;
            if (kit::contains(*rect, at))
                return {Part::campaign, row};
        }
        if (const kit::Rect track = track_rect(); kit::contains(track, at)) {
            if (kit::contains(step_rect(false), at))
                return {Part::row_up, 0};
            if (kit::contains(step_rect(true), at))
                return {Part::row_down, 0};
            const kit::Rect thumb = thumb_rect();
            if (at.y < thumb.y)
                return {Part::scroll_up, 0};
            if (at.y >= thumb.y + thumb.height)
                return {Part::scroll_down, 0};
            return {};
        }
        for (std::size_t level = 0; level < layout_.levels.size(); ++level)
            if (kit::contains(layout_.levels[level], at))
                return {Part::difficulty, level};
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
        case Part::side:
            return hit.index < facts_now_.sides.size() && facts_now_.sides[hit.index].has_campaign;
        case Part::earlier:
            return first_side_ > 0;
        case Part::later:
            return first_side_ + tile_room() < facts_now_.sides.size();
        case Part::campaign:
            return hit.index < facts_now_.campaigns.size();
        case Part::start:
            return facts_now_.can_start;
        case Part::scroll_up:
        case Part::scroll_down:
        case Part::row_up:
        case Part::row_down:
        case Part::difficulty:
        case Part::back:
            return true;
        }
        return false;
    }

    /// Presses a part that takes a press.
    ///
    /// @param hit the part
    void press(Hit hit) {
        if (!enabled(hit))
            return;
        switch (hit.part) {
        case Part::side:
            if (hit.index != facts_now_.side && actions_.choose_side)
                actions_.choose_side(hit.index);
            break;
        case Part::earlier:
            scroll_sides(-static_cast<int32_t>(tile_room()));
            break;
        case Part::later:
            scroll_sides(static_cast<int32_t>(tile_room()));
            break;
        case Part::campaign:
            choose_row(hit.index);
            break;
        case Part::scroll_up:
            scroll_rows(-static_cast<int32_t>(row_room()));
            break;
        case Part::scroll_down:
            scroll_rows(static_cast<int32_t>(row_room()));
            break;
        case Part::row_up:
            scroll_rows(-1);
            break;
        case Part::row_down:
            scroll_rows(1);
            break;
        case Part::difficulty:
            if (actions_.choose_difficulty)
                actions_.choose_difficulty(static_cast<uint32_t>(hit.index));
            break;
        case Part::back:
            if (actions_.back)
                actions_.back();
            break;
        case Part::start:
            if (actions_.start)
                actions_.start();
            break;
        case Part::none:
            break;
        }
        ++revision_;
    }

    /// Chooses a campaign's row.
    ///
    /// @param row the row
    void choose_row(std::size_t row) {
        if (row >= facts_now_.campaigns.size())
            return;
        if (row != facts_now_.campaign && actions_.choose_campaign)
            actions_.choose_campaign(row);
        ++revision_;
    }

    /// Chooses the next side from the chosen one that has a campaign.
    ///
    /// @param step 1 right, -1 left
    void step_side(int32_t step) {
        const auto count = static_cast<int64_t>(facts_now_.sides.size());
        for (int64_t side = static_cast<int64_t>(facts_now_.side) + step; side >= 0 && side < count;
             side += step)
            if (facts_now_.sides[static_cast<std::size_t>(side)].has_campaign) {
                press({Part::side, static_cast<std::size_t>(side)});
                return;
            }
    }

    /// Moves the strip's sides in view.
    ///
    /// @param step how many sides, right positive
    void scroll_sides(int32_t step) {
        const std::size_t room = tile_room();
        const std::size_t count = facts_now_.sides.size();
        const std::size_t last = count > room ? count - room : 0;
        const auto moved = static_cast<int64_t>(first_side_) + step;
        const std::size_t first =
            static_cast<std::size_t>(std::clamp<int64_t>(moved, 0, static_cast<int64_t>(last)));
        if (first == first_side_)
            return;
        first_side_ = first;
        ++revision_;
    }

    /// Moves the list's rows in view.
    ///
    /// @param step how many rows, down positive
    void scroll_rows(int32_t step) {
        const std::size_t room = row_room();
        const std::size_t count = facts_now_.campaigns.size();
        const std::size_t last = count > room ? count - room : 0;
        const auto moved = static_cast<int64_t>(first_row_) + step;
        const std::size_t first =
            static_cast<std::size_t>(std::clamp<int64_t>(moved, 0, static_cast<int64_t>(last)));
        if (first == first_row_)
            return;
        first_row_ = first;
        ++revision_;
    }

    /// Brings the chosen side into the strip's view.
    void keep_side_in_view() {
        const std::size_t room = tile_room();
        const std::size_t count = facts_now_.sides.size();
        if (facts_now_.side < first_side_)
            first_side_ = facts_now_.side;
        else if (facts_now_.side >= first_side_ + room)
            first_side_ = facts_now_.side + 1 - room;
        if (first_side_ + room > count)
            first_side_ = count > room ? count - room : 0;
    }

    /// Brings the chosen campaign into the list's view.
    void keep_row_in_view() {
        const std::size_t room = row_room();
        const std::size_t count = facts_now_.campaigns.size();
        if (facts_now_.campaign < first_row_)
            first_row_ = facts_now_.campaign;
        else if (facts_now_.campaign >= first_row_ + room)
            first_row_ = facts_now_.campaign + 1 - room;
        if (first_row_ + room > count)
            first_row_ = count > room ? count - room : 0;
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

    /// Returns a side's emblem lit in steel, or in amber for the chosen
    /// side, made the first time it is drawn so.
    ///
    /// @param side the side
    /// @param chosen it is the chosen side
    /// @return the emblem; empty for none
    [[nodiscard]] const WindowIcon& emblem(std::size_t side, bool chosen) const {
        if (emblems_.size() != facts_now_.sides.size()) {
            emblems_.clear();
            emblems_.resize(facts_now_.sides.size());
            emblem_fitted_.clear();
        }
        auto& kept = emblems_[side];
        if (!kept) {
            const WindowIcon picture = trimmed_picture(
                actions_.emblem ? actions_.emblem(side) : WindowIcon{}, emblem_edge
            );
            kept = std::make_unique<LitEmblem>(LitEmblem{
                tinted_picture(picture, screen_colour, emblem_colour, emblem_floor, emblem_peak),
                tinted_picture(
                    picture, screen_colour, emblem_chosen_colour, emblem_floor, emblem_peak
                )
            });
        }
        return chosen ? kept->chosen : kept->plain;
    }

    /// Draws a panel's label on a steel plate set into its frame at its top
    /// left, and when asked a rule from the plate to the panel's right.
    ///
    /// @param canvas where it draws
    /// @param band the panel's label band
    /// @param text the label
    /// @param ruled a rule runs on from the plate
    void draw_label(
        const kit::Canvas& canvas, const kit::Rect& band, std::string_view text, bool ruled
    ) const {
        const int32_t left = band.x + layout_.rim + layout_.label_left;
        const int32_t reach = band.x + band.width - layout_.rim - layout_.inset;
        const int32_t width = std::min(
            label_plate_width(canvas, text, layout_.label_height, band_tracking), reach - left
        );
        const kit::Rect plate{left, band.y + layout_.label_top, width, layout_.label_height};
        draw_label_plate(canvas, plate, text, band_tracking);
        const int32_t from = plate.x + plate.width + layout_.inset / 2;
        if (!ruled || reach <= from)
            return;
        const int32_t row = plate.y + plate.height - plate.height / 4;
        kit::fill_rect(canvas, {from, row, reach - from, 1}, plate_edge_colour);
        kit::fill_rect(canvas, {from, row + 1, reach - from, 1}, outline_colour);
    }

    /// Draws the side strip: its panel, its label, the tiles in view and,
    /// when it has more sides than room, the arrows at its ends.
    ///
    /// @param canvas where it draws
    void draw_strip(const kit::Canvas& canvas) const {
        draw_panel_frame(canvas, layout_.strip, layout_.rim);
        draw_label(canvas, layout_.strip_band, worded("SELECT SIDE"), true);
        for (std::size_t side = first_side_; side < facts_now_.sides.size(); ++side) {
            const auto tile = tile_rect(side);
            if (!tile)
                break;
            draw_tile(canvas, side, *tile);
        }
        if (!strip_overflows())
            return;
        for (const Part part : {Part::earlier, Part::later}) {
            const kit::Rect& rect = part == Part::earlier ? layout_.earlier : layout_.later;
            const bool usable = enabled({part, 0});
            const bool lit = usable && hovered_ == Hit{part, 0};
            draw_steel_button(canvas, rect, button_steel_colour);
            if (lit)
                kit::outline_rect(canvas, rect, hover_edge_colour);
            const int32_t reach = std::max(rect.width / 4, 3);
            const int32_t middle = rect.y + rect.height / 2;
            const kit::Point tip = part == Part::earlier
                                       ? kit::Point{rect.x + rect.width / 2 - reach / 2, middle}
                                       : kit::Point{rect.x + rect.width / 2 + reach / 2, middle};
            draw_arrow(
                canvas,
                tip,
                reach,
                part == Part::earlier,
                !usable ? caption_faded_colour : lit ? caption_lit_colour : caption_colour
            );
        }
    }

    /// Draws a side's tile: its screen with the side's emblem on a dark
    /// grid, its name plate under it, a MOD tag on a side past 3.1c's two,
    /// a ring round the chosen side, and a shade over a side without a
    /// campaign.
    ///
    /// @param canvas where it draws
    /// @param side the side
    /// @param tile where its tile lies
    void draw_tile(const kit::Canvas& canvas, std::size_t side, const kit::Rect& tile) const {
        const CampaignMenuSide& shown = facts_now_.sides[side];
        const bool chosen = side == facts_now_.side;
        const bool usable = enabled({Part::side, side});
        const bool lit = usable && hovered_ == Hit{Part::side, side};
        const kit::Rect screen{tile.x, tile.y, tile.width, layout_.screen_height};
        const kit::Rect plate{
            tile.x, screen.y + screen.height + layout_.name_gap, tile.width, layout_.name_height
        };

        const int32_t bezel = std::min(layout_.bezel, std::min(screen.width, screen.height) / 4);
        if (chosen)
            draw_glow(
                canvas,
                {screen.x - 2, screen.y - 2, screen.width + 4, plate.y + plate.height - screen.y + 4},
                chosen_colour,
                layout_.glow
            );
        draw_steel(canvas, screen, steel_colour);
        const int32_t bolt = std::max(bezel * 2 / 5, 1);
        const int32_t centre = bezel / 2;
        for (const kit::Point corner :
             {kit::Point{screen.x + centre, screen.y + centre},
              kit::Point{screen.x + screen.width - centre - 1, screen.y + centre},
              kit::Point{screen.x + centre, screen.y + screen.height - centre - 1},
              kit::Point{screen.x + screen.width - centre - 1, screen.y + screen.height - centre - 1}})
            draw_bolt(canvas, corner, bolt);
        const kit::Rect glass{
            screen.x + bezel, screen.y + bezel, screen.width - 2 * bezel, screen.height - 2 * bezel
        };
        kit::fill_rect(canvas, glass, screen_colour);
        const WindowIcon& picture = emblem(side, chosen);
        const int32_t room = std::min(glass.width, glass.height) - 2 * layout_.emblem_margin;
        if (picture.width > 0 && picture.height > 0 && room > 0) {
            const double scale = static_cast<double>(room) /
                                 static_cast<double>(std::max(picture.width, picture.height));
            const int32_t width = nearest(picture.width * scale);
            const int32_t height = nearest(picture.height * scale);
            emblem_fitted_.draw(
                canvas,
                {glass.x + (glass.width - width) / 2, glass.y + (glass.height - height) / 2, width,
                 height},
                picture
            );
        } else {
            // Without a picture, the side's name stands in its screen.
            outlined_text(
                canvas, kit::FontRole::regular, shown.name, glass, kit::Align::centre,
                row_text_colour, heading_tracking
            );
        }
        for (int32_t x = glass.x + layout_.grid; x < glass.x + glass.width; x += layout_.grid)
            kit::blend_rect(canvas, {x, glass.y, 1, glass.height}, grid_colour, grid_opacity);
        for (int32_t y = glass.y + layout_.grid; y < glass.y + glass.height; y += layout_.grid)
            kit::blend_rect(canvas, {glass.x, y, glass.width, 1}, grid_colour, grid_opacity);
        if (shown.added) {
            const std::string tag = worded("MOD");
            const int32_t tag_width = kit::measure_text(canvas, kit::FontRole::small, tag) + 12;
            const int32_t tag_height = kit::smooth_text_points(kit::FontRole::small) + 8;
            const kit::Rect box{
                glass.x + glass.width - tag_width - 4, glass.y + 4, tag_width, tag_height
            };
            draw_steel(canvas, box, steel_colour);
            outlined_text(canvas, kit::FontRole::small, tag, box, kit::Align::centre, caption_colour);
        }
        draw_sunk_edge(canvas, {glass.x - 2, glass.y - 2, glass.width + 4, glass.height + 4});

        draw_steel_button(canvas, plate, chosen ? chosen_steel_colour : steel_colour);
        const kit::Colour ink = !usable ? caption_faded_colour
                                : chosen || lit ? caption_lit_colour
                                                : caption_colour;
        const kit::Measure measure = [&canvas](std::string_view line) {
            return kit::measure_text(canvas, kit::FontRole::regular, line);
        };
        outlined_text(
            canvas,
            kit::FontRole::regular,
            kit::fit(shown.name, plate.width - 8, measure),
            plate,
            kit::Align::centre,
            ink
        );

        if (!usable && !chosen) {
            kit::blend_rect(canvas, screen, fade_colour, fade_opacity);
            kit::blend_rect(canvas, plate, fade_colour, fade_opacity);
        }
        if (chosen) {
            draw_ring(canvas, {screen.x - 2, screen.y - 2, screen.width + 4, screen.height + 4}, 2,
                 chosen_colour);
            draw_ring(canvas, {plate.x - 2, plate.y - 2, plate.width + 4, plate.height + 4}, 2,
                 chosen_colour);
        } else if (lit) {
            kit::outline_rect(
                canvas, {screen.x - 2, screen.y - 2, screen.width + 4, screen.height + 4},
                hover_edge_colour
            );
        }
    }

    /// Draws the campaign list: its panel, its label, the rows in view,
    /// the chosen one lit, and the scroll bar when it has more campaigns than
    /// rows.
    ///
    /// @param canvas where it draws
    void draw_list(const kit::Canvas& canvas) const {
        draw_panel_frame(canvas, layout_.list, layout_.rim);
        draw_label(canvas, layout_.list_band, worded("SELECT CAMPAIGN"), false);
        const kit::Rect box{
            layout_.rows.x - 2,
            layout_.rows.y - 2,
            layout_.rows.width + 4,
            static_cast<int32_t>(row_room()) * layout_.row_height + 4
        };
        kit::fill_rect(canvas, box, list_colour);
        draw_sunk_edge(canvas, {box.x - 2, box.y - 2, box.width + 4, box.height + 4});
        const kit::Measure measure = [&canvas](std::string_view line) {
            return kit::measure_text(canvas, kit::FontRole::regular, line);
        };
        for (std::size_t row = first_row_; row < facts_now_.campaigns.size(); ++row) {
            const auto rect = row_rect(row);
            if (!rect)
                break;
            const bool chosen = row == facts_now_.campaign;
            if (chosen) {
                // Lit from within: brighter over its middle, its edge glowing.
                kit::fill_rect(canvas, *rect, row_chosen_colour);
                kit::blend_rect(
                    canvas, {rect->x, rect->y + rect->height / 4, rect->width, rect->height / 2},
                    row_glow_colour, row_glow_opacity
                );
                kit::outline_rect(canvas, *rect, row_chosen_edge);
                kit::outline_rect(
                    canvas, {rect->x + 1, rect->y + 1, rect->width - 2, rect->height - 2},
                    row_glow_colour
                );
            } else {
                if (hovered_ == Hit{Part::campaign, row})
                    kit::fill_rect(canvas, *rect, row_hover_colour);
                kit::fill_rect(
                    canvas, {rect->x, rect->y + rect->height - 1, rect->width, 1}, row_rule_colour
                );
            }
            const kit::Rect text_box{
                rect->x + layout_.inset / 2 + 4, rect->y, rect->width - layout_.inset - 8,
                rect->height
            };
            kit::box_text(
                canvas,
                kit::FontRole::regular,
                kit::fit(facts_now_.campaigns[row], text_box.width, measure),
                text_box,
                kit::Align::left,
                chosen ? row_chosen_text : row_text_colour
            );
        }
        if (facts_now_.campaigns.empty())
            kit::box_text(
                canvas,
                kit::FontRole::small,
                worded("No campaign is listed for this side."),
                {layout_.rows.x, layout_.rows.y, layout_.rows.width, layout_.row_height},
                kit::Align::centre,
                plate_text_colour
            );
        if (const kit::Rect track = track_rect(); track.height > 0) {
            kit::fill_rect(canvas, track, track_colour);
            kit::outline_rect(canvas, track, outline_colour);
            const kit::Rect thumb = thumb_rect();
            draw_steel(
                canvas, {thumb.x + 2, thumb.y + 1, thumb.width - 4, thumb.height - 2}, thumb_colour
            );
            for (const Part part : {Part::row_up, Part::row_down}) {
                const kit::Rect cap = step_rect(part == Part::row_down);
                if (cap.height <= 0)
                    continue;
                const bool lit = hovered_ == Hit{part, 0};
                draw_steel(canvas, cap, button_steel_colour);
                const int32_t reach = std::max(cap.width / 4, 2);
                const int32_t middle = cap.y + cap.height / 2;
                draw_upright_arrow(
                    canvas,
                    {cap.x + cap.width / 2,
                     part == Part::row_down ? middle + reach / 2 : middle - reach / 2},
                    reach,
                    part == Part::row_down,
                    lit ? caption_lit_colour : caption_colour
                );
            }
        }
    }

    /// Draws the panel about the chosen campaign: its name, how many
    /// missions it has and its first mission, and under them the difficulty
    /// and its three buttons.
    ///
    /// @param canvas where it draws
    void draw_about(const kit::Canvas& canvas) const {
        const kit::Rect& panel = layout_.about;
        draw_panel_frame(canvas, panel, layout_.rim);
        const int32_t inset = std::max(layout_.inset, 4);
        const int32_t room = panel.width - 2 * inset;
        if (room <= 0)
            return;
        const kit::Measure small = [&canvas](std::string_view line) {
            return kit::measure_text(canvas, kit::FontRole::small, line);
        };
        const kit::Measure regular = [&canvas](std::string_view line) {
            return kit::measure_text(canvas, kit::FontRole::regular, line);
        };
        const int32_t pitch = kit::smooth_text_points(kit::FontRole::small) + line_leading;
        const int32_t level_label_top =
            layout_.levels[0].y - pitch - std::max(inset / 2, line_leading);
        int32_t row = panel.y + inset;
        const auto line = [&](std::string_view text) {
            if (row + pitch > level_label_top - line_leading)
                return;
            kit::box_text(
                canvas, kit::FontRole::small, text, {panel.x + inset, row, room, pitch},
                kit::Align::left, plate_text_colour
            );
            row += pitch;
        };
        if (facts_now_.campaign < facts_now_.campaigns.size()) {
            const int32_t title_points = kit::smooth_text_points(kit::FontRole::regular) * 2;
            outlined_text(
                canvas,
                kit::FontRole::regular,
                kit::fit(facts_now_.campaigns[facts_now_.campaign], room, regular),
                {panel.x + inset, row, room, title_points},
                kit::Align::left,
                caption_colour
            );
            row += title_points + line_leading;
            if (facts_now_.missions > 0)
                line(facts_now_.missions == 1
                         ? worded("1 mission")
                         : filled(worded("{count} missions"), "{count}",
                                  std::to_string(facts_now_.missions)));
            if (!facts_now_.first_mission.empty()) {
                row += line_leading;
                const auto wrapped = kit::wrap(
                    filled(worded("First mission: {mission}"), "{mission}",
                           facts_now_.first_mission),
                    room,
                    small
                );
                for (std::size_t at = 0; at < wrapped.size() && at < most_mission_lines; ++at)
                    line(wrapped[at]);
            }
        }
        kit::fill_rect(
            canvas, {panel.x + inset, level_label_top - line_leading, room, 1}, plate_edge_colour
        );
        kit::box_text(
            canvas, kit::FontRole::small, worded("DIFFICULTY"),
            {panel.x + inset, level_label_top, room, pitch}, kit::Align::left, caption_colour,
            band_tracking
        );
        for (std::size_t level = 0; level < layout_.levels.size(); ++level) {
            const kit::Rect& rect = layout_.levels[level];
            const bool chosen = level == facts_now_.difficulty;
            const bool lit = hovered_ == Hit{Part::difficulty, level};
            const bool held = lit && pressed_ == hovered_;
            if (chosen)
                draw_glow(canvas, rect, chosen_colour, layout_.glow);
            draw_steel_button(canvas, rect, chosen ? chosen_steel_colour : button_steel_colour);
            if (chosen)
                draw_ring(canvas, rect, 2, chosen_colour);
            else if (lit)
                kit::outline_rect(canvas, rect, hover_edge_colour);
            kit::Rect box = rect;
            if (held) {
                box.x += 1;
                box.y += 1;
            }
            outlined_text(
                canvas,
                kit::FontRole::regular,
                kit::fit(facts_now_.difficulties[level], rect.width - 12, regular),
                box,
                kit::Align::centre,
                chosen || lit ? caption_lit_colour : caption_colour
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
        const kit::Colour ink = !usable                 ? caption_faded_colour
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

    std::function<bool()> shows_{};                 ///< whether the screen shows
    std::function<CampaignMenuFacts()> facts_{};    ///< reads NEWGAME.GUI and the setup
    CampaignMenuActions actions_{};                 ///< what its presses do
    CampaignMenuFacts facts_now_{};                 ///< what they show, as last read
    kit::Point size_{};                             ///< the screen's size, in points
    CampaignLayout layout_{};                       ///< where its parts lie
    bool laid_out_{};                               ///< layout_ is for size_
    Hit hovered_{};                                 ///< the part under the pointer
    Hit pressed_{};                                 ///< the part a press holds
    std::size_t first_side_{};                      ///< the first side in the strip's view
    std::size_t first_row_{};                       ///< the first campaign in the list's view
    FittedArt fitted_{};                            ///< the title and the faces, filtered
    mutable FittedArt emblem_fitted_{};             ///< the emblems, filtered
    /// A side's emblem lit both ways a tile shows it.
    struct LitEmblem {
        WindowIcon plain{};  ///< in steel
        WindowIcon chosen{}; ///< in amber, for the chosen side
    };

    /// The sides' emblems, each made when first drawn; each stays where it
    /// is while copies of it are kept.
    mutable std::vector<std::unique_ptr<LitEmblem>> emblems_{};
    uint64_t revision_{1};                          ///< counts the changes to what it draws
};

} // namespace

std::unique_ptr<LayerScreen> make_campaign_menu(
    std::function<bool()> shows,
    std::function<CampaignMenuFacts()> facts,
    CampaignMenuActions actions
) {
    return std::make_unique<CampaignMenuScreen>(
        std::move(shows), std::move(facts), std::move(actions)
    );
}

} // namespace oa::app
