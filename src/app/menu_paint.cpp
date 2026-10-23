// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// What Open Annihilation's menus on the OA layer draw alike (menu_paint.hpp).

#include "menu_paint.hpp"

#include "oa/app/main_menu_art.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/ui/frontend_renderer/artless.hpp"
#include "oa/ui/paint/painter.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iterator>
#include <span>
#include <string>

namespace oa::app::menu_paint {

namespace {

namespace kit = oa::ui::kit;
namespace artless = oa::ui::frontend_renderer;

/// How far the background's left edge moves left of the window's when the
/// window is narrower than the picture, as a share of what is cut: the
/// commander on its left stays in view.
constexpr double background_cut_left_share = 0.1;
/// How many bands the shade over the top and the bottom of the picture is
/// drawn in.
constexpr int32_t shade_bands = 24;
/// How far down the shade over the top reaches, as a share of the height.
constexpr double top_shade_share = 0.24;
/// How far up the shade over the bottom reaches, as a share of the height.
constexpr double bottom_shade_share = 0.22;
/// The top shade's opacity at the top edge, in 256ths.
constexpr uint32_t top_shade_opacity = 200;
/// The bottom shade's opacity at the bottom edge, in 256ths.
constexpr uint32_t bottom_shade_opacity = 150;
/// The most filtered pictures a screen keeps: the title, and the four faces
/// at each of the buttons' sizes, which rounding may make differ by a point.
constexpr std::size_t most_fitted_art = 16;

/// How many bands a steel plate's light and shade are drawn in.
constexpr int32_t steel_bands = 8;
/// How far down a steel plate the light reaches, as a share of its height.
constexpr double steel_sheen_reach = 0.45;
/// How far up a steel plate the shade reaches, as a share of its height.
constexpr double steel_shade_reach = 0.45;
/// The light's opacity at a steel plate's top, in 256ths.
constexpr double steel_sheen_opacity = 44.0;
/// The shade's opacity at a steel plate's bottom, in 256ths.
constexpr double steel_shade_opacity = 80.0;
/// The light's opacity over a frame's top bar, in 256ths.
constexpr uint32_t rim_sheen_opacity = 34;
/// The shade's opacity over a frame's bottom bar, in 256ths.
constexpr uint32_t rim_shade_opacity = 70;
/// A glow's opacity next to what it lies round, in 256ths.
constexpr uint32_t glow_opacity = 150;
/// How many lines a frame's vent has.
constexpr int32_t vent_lines = 3;
/// The points from one line of a vent to the next.
constexpr int32_t vent_pitch = 2;

constexpr kit::Colour ground_colour{8, 10, 12};               ///< under the picture
constexpr kit::Colour steel_sheen_colour{236, 240, 236};      ///< the light on steel
constexpr kit::Colour rim_groove_light_colour{104, 110, 108}; ///< a rim's groove's lit side
constexpr kit::Colour label_plate_colour{56, 61, 64};         ///< a label plate's face
constexpr kit::Colour version_plate_colour{30, 34, 38};       ///< the version plate's face
constexpr kit::Colour shade_colour{3, 7, 10};   ///< the shade's

/// Decodes one picture of the art, saying so when it cannot be read.
///
/// @param png the embedded file
/// @param what what it is, for the message
/// @return the picture; empty when it cannot be decoded
WindowIcon decoded_art(std::span<const uint8_t> png, std::string_view what) {
    WindowIcon picture;
    std::string error;
    if (!decode_rgba_icon(png, picture, error)) {
        std::cerr << "open-annihilation: the main menu's " << what << " is missing: " << error
                  << '\n';
        picture = {};
    }
    return picture;
}

/// Returns a decoded picture as the canvas draws it.
///
/// @param icon the picture
/// @return the picture's view of its pixels
artless::RgbaPicture picture_of(const WindowIcon& icon) noexcept {
    return {icon.width, icon.height, icon.pixels};
}

/// The backdrop the menus last put together (draw_backdrop), kept for every
/// menu: the main menu, the Single Player menu and the New Campaign screen
/// lie on the same one, and going from one to another does not put it
/// together again.
struct KeptBackdrop {
    kit::Point covered{};          ///< the pixels it covers, across and down
    kit::Point size{};             ///< the screen's size it was made for, in points
    kit::Rect background{};        ///< where the picture lay, in points
    std::vector<uint8_t> pixels{}; ///< four bytes a pixel, every one opaque
};

/// Returns the kept backdrop. Only the thread that draws the interface uses it.
///
/// @return the backdrop; empty until a menu first draws
KeptBackdrop& kept_backdrop() {
    static KeptBackdrop kept;
    return kept;
}

/// Puts the backdrop together at the pixels the screen covers
/// (draw_backdrop): the ground, the picture over it, and the shade's bands,
/// each band's opacity falling off as the square of its distance from the
/// edge, from the edge's opacity to none.
///
/// @param canvas where it is drawn
/// @param covered the pixels the screen covers, across and down
/// @param size the screen's size, in points
/// @param background where the picture lies, in points
/// @param picture the background's picture
/// @return four bytes a pixel, every one opaque
std::vector<uint8_t> backdrop_pixels(
    const kit::Canvas& canvas,
    kit::Point covered,
    kit::Point size,
    const kit::Rect& background,
    const WindowIcon& picture
) {
    const auto columns = static_cast<std::size_t>(covered.x);
    const auto rows = static_cast<std::size_t>(covered.y);
    std::vector<uint8_t> pixels(columns * rows * 4U);
    for (std::size_t at = 0; at < columns * rows; ++at) {
        pixels[at * 4U] = ground_colour.r;
        pixels[at * 4U + 1U] = ground_colour.g;
        pixels[at * 4U + 2U] = ground_colour.b;
        pixels[at * 4U + 3U] = 255;
    }
    // A point is this many of the screen's pixels across and down.
    const double across = static_cast<double>(covered.x) / size.x;
    const double down = static_cast<double>(covered.y) / size.y;
    const kit::Point fitted = kit::covered_pixels(canvas, background);
    if (fitted.x > 0 && fitted.y > 0 && picture.width > 0 && picture.height > 0) {
        const std::vector<uint8_t> scaled = oa::ui::paint::resized_rgba(
            picture.pixels,
            static_cast<int>(picture.width),
            static_cast<int>(picture.height),
            fitted.x,
            fitted.y
        );
        const auto left = static_cast<int64_t>(std::lround(background.x * across));
        const auto top = static_cast<int64_t>(std::lround(background.y * down));
        const int64_t first = std::max<int64_t>(left, 0);
        const int64_t end = std::min<int64_t>(left + fitted.x, covered.x);
        if (scaled.size() == static_cast<std::size_t>(fitted.x) * fitted.y * 4U && end > first)
            for (int64_t row = std::max<int64_t>(top, 0);
                 row < std::min<int64_t>(top + fitted.y, covered.y);
                 ++row) {
                const uint8_t* from =
                    scaled.data() +
                    static_cast<std::size_t>((row - top) * fitted.x + (first - left)) * 4U;
                uint8_t* to =
                    pixels.data() + static_cast<std::size_t>(row * covered.x + first) * 4U;
                for (int64_t column = first; column < end; ++column, from += 4, to += 4) {
                    const uint32_t alpha = from[3];
                    for (std::size_t channel = 0; channel < 3U; ++channel)
                        to[channel] = static_cast<uint8_t>(
                            (from[channel] * alpha + to[channel] * (255U - alpha) + 127U) / 255U
                        );
                }
            }
    }
    const auto band = [&](double from, double to, uint32_t opacity) {
        if (opacity == 0)
            return;
        const auto first = static_cast<int64_t>(std::lround(nearest(from) * down));
        const auto end = static_cast<int64_t>(std::lround(nearest(to) * down));
        for (int64_t row = std::max<int64_t>(first, 0); row < std::min<int64_t>(end, covered.y);
             ++row) {
            uint8_t* shaded = pixels.data() + static_cast<std::size_t>(row) * columns * 4U;
            for (std::size_t column = 0; column < columns; ++column, shaded += 4) {
                shaded[0] = static_cast<uint8_t>(
                    (shade_colour.r * opacity + shaded[0] * (256U - opacity)) >> 8U
                );
                shaded[1] = static_cast<uint8_t>(
                    (shade_colour.g * opacity + shaded[1] * (256U - opacity)) >> 8U
                );
                shaded[2] = static_cast<uint8_t>(
                    (shade_colour.b * opacity + shaded[2] * (256U - opacity)) >> 8U
                );
            }
        }
    };
    const auto screen_rows = static_cast<double>(size.y);
    const double top_rows = screen_rows * top_shade_share;
    const double bottom_rows = screen_rows * bottom_shade_share;
    for (int32_t index = 0; index < shade_bands; ++index) {
        const double near = 1.0 - (static_cast<double>(index) + 0.5) / shade_bands;
        const double share = near * near;
        band(
            top_rows * index / shade_bands,
            top_rows * (index + 1) / shade_bands,
            static_cast<uint32_t>(std::lround(top_shade_opacity * share))
        );
        band(
            screen_rows - bottom_rows * (index + 1) / shade_bands,
            screen_rows - bottom_rows * index / shade_bands,
            static_cast<uint32_t>(std::lround(bottom_shade_opacity * share))
        );
    }
    return pixels;
}

} // namespace

const MenuArt& menu_art() {
    static const MenuArt art = [] {
        MenuArt decoded;
        decoded.background = decoded_art(main_menu_background_png(), "background");
        decoded.title = decoded_art(main_menu_title_png(), "title");
        decoded.faces[static_cast<std::size_t>(Face::normal)] =
            decoded_art(main_menu_button_normal_png(), "button");
        decoded.faces[static_cast<std::size_t>(Face::hover)] =
            decoded_art(main_menu_button_hover_png(), "highlighted button");
        decoded.faces[static_cast<std::size_t>(Face::pressed)] =
            decoded_art(main_menu_button_pressed_png(), "pressed button");
        decoded.faces[static_cast<std::size_t>(Face::disabled)] =
            decoded_art(main_menu_button_disabled_png(), "disabled button");
        return decoded;
    }();
    return art;
}

int32_t nearest(double value) noexcept {
    return static_cast<int32_t>(std::lround(value));
}

kit::Rect background_place(int32_t width, int32_t height) noexcept {
    const double columns = std::max(width, 1);
    const double rows = std::max(height, 1);
    const double cover = std::max(
        columns / static_cast<double>(main_menu_background_width),
        rows / static_cast<double>(main_menu_background_height)
    );
    const double picture_width = static_cast<double>(main_menu_background_width) * cover;
    const double picture_height = static_cast<double>(main_menu_background_height) * cover;
    return {
        nearest(-(picture_width - columns) * background_cut_left_share),
        nearest(-(picture_height - rows) / 2.0),
        nearest(picture_width),
        nearest(picture_height)
    };
}

void FittedArt::draw(const kit::Canvas& canvas, const kit::Rect& rect, const WindowIcon& picture)
    const {
    const kit::Point covered = kit::covered_pixels(canvas, rect);
    if (covered.x <= 0 || covered.y <= 0 || picture.width == 0 || picture.height == 0)
        return;
    auto kept = std::find_if(fitted_.begin(), fitted_.end(), [&](const Fitted& fitted) {
        return fitted.picture == &picture && fitted.columns == covered.x &&
               fitted.rows == covered.y;
    });
    if (kept == fitted_.end()) {
        if (fitted_.size() >= most_fitted_art)
            fitted_.clear();
        Fitted fitted;
        fitted.picture = &picture;
        fitted.columns = covered.x;
        fitted.rows = covered.y;
        fitted.pixels = oa::ui::paint::resized_rgba(
            picture.pixels,
            static_cast<int>(picture.width),
            static_cast<int>(picture.height),
            covered.x,
            covered.y
        );
        fitted_.push_back(std::move(fitted));
        kept = std::prev(fitted_.end());
    }
    if (kept->pixels.empty()) {
        kit::draw_picture(canvas, rect, picture_of(picture));
        return;
    }
    kit::draw_picture(
        canvas,
        rect,
        {static_cast<uint32_t>(kept->columns), static_cast<uint32_t>(kept->rows), kept->pixels}
    );
}

void draw_backdrop(const kit::Canvas& canvas, kit::Point size, const kit::Rect& background) {
    const kit::Rect whole{0, 0, size.x, size.y};
    const kit::Point covered = kit::covered_pixels(canvas, whole);
    if (covered.x <= 0 || covered.y <= 0 || size.x <= 0 || size.y <= 0)
        return;
    KeptBackdrop& kept = kept_backdrop();
    if (kept.covered.x != covered.x || kept.covered.y != covered.y || kept.size.x != size.x ||
        kept.size.y != size.y || kept.background.x != background.x ||
        kept.background.y != background.y || kept.background.width != background.width ||
        kept.background.height != background.height || kept.pixels.empty()) {
        kept.pixels = backdrop_pixels(canvas, covered, size, background, menu_art().background);
        kept.covered = covered;
        kept.size = size;
        kept.background = background;
    }
    kit::draw_picture(
        canvas,
        whole,
        {static_cast<uint32_t>(covered.x), static_cast<uint32_t>(covered.y), kept.pixels}
    );
}

void outlined_text(
    const kit::Canvas& canvas,
    kit::FontRole role,
    std::string_view text,
    const kit::Rect& box,
    kit::Align align,
    kit::Colour ink,
    int32_t tracking
) {
    constexpr std::array<kit::Point, 8> around{
        {{-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {1, 0}, {-1, 1}, {0, 1}, {1, 1}}
    };
    for (const kit::Point step : around)
        kit::box_text(
            canvas,
            role,
            text,
            {box.x + step.x, box.y + step.y, box.width, box.height},
            align,
            outline_colour,
            tracking
        );
    kit::box_text(canvas, role, text, box, align, ink, tracking);
}

std::optional<std::size_t> quick_key_at(std::string_view caption, char key) {
    if (key == 0)
        return std::nullopt;
    const auto lower = [](char letter) {
        return letter >= 'A' && letter <= 'Z' ? static_cast<char>(letter - 'A' + 'a') : letter;
    };
    for (std::size_t at = 0; at < caption.size(); ++at)
        if (lower(caption[at]) == lower(key))
            return at;
    return std::nullopt;
}

void underline_quick_key(
    const kit::Canvas& canvas, std::string_view text, const kit::Rect& box, std::size_t at
) {
    if (at >= text.size())
        return;
    const int32_t width = kit::measure_text(canvas, kit::FontRole::regular, text);
    const int32_t before = kit::measure_text(canvas, kit::FontRole::regular, text.substr(0, at));
    const int32_t letter = kit::measure_text(canvas, kit::FontRole::regular, text.substr(at, 1));
    const int32_t left = box.x + (box.width - width) / 2 + before;
    // Under the capitals, which box_text centres on the box's height.
    const int32_t row =
        box.y + box.height / 2 + kit::smooth_text_points(kit::FontRole::regular) / 2;
    kit::fill_rect(canvas, {left - 1, row, letter + 2, 3}, outline_colour);
    kit::fill_rect(canvas, {left, row + 1, letter, 1}, key_colour);
}

void draw_arrow(const kit::Canvas& canvas, kit::Point tip, int32_t reach, bool left, kit::Colour ink) {
    const auto triangle = [&](int32_t shift, kit::Colour colour) {
        for (int32_t step = -reach; step <= reach; ++step) {
            const int32_t away = step < 0 ? -step : step;
            const int32_t length = reach - away + 1;
            const int32_t from = left ? tip.x + away : tip.x - away - length + 1;
            kit::fill_rect(canvas, {from + shift, tip.y + shift + step, length, 1}, colour);
        }
    };
    triangle(1, outline_colour);
    triangle(0, ink);
}

void draw_bolt(const kit::Canvas& canvas, kit::Point centre, int32_t radius) {
    if (radius < 1)
        return;
    kit::fill_round_rect(
        canvas,
        {centre.x - radius - 1, centre.y - radius - 1, 2 * radius + 2, 2 * radius + 2},
        radius + 1,
        outline_colour
    );
    kit::fill_round_rect(
        canvas, {centre.x - radius, centre.y - radius, 2 * radius, 2 * radius}, radius, bolt_colour
    );
    const int32_t light = std::max(radius / 2, 1);
    const int32_t offset = radius / 3;
    kit::fill_round_rect(
        canvas,
        {centre.x - offset - light, centre.y - offset - light, light * 2 - 1, light * 2 - 1},
        light,
        bolt_light_colour
    );
}

void draw_steel(const kit::Canvas& canvas, const kit::Rect& rect, kit::Colour face) {
    if (rect.width <= 0 || rect.height <= 0)
        return;
    kit::fill_rect(canvas, rect, face);
    if (rect.width <= 4 || rect.height <= 4)
        return;
    for (int32_t band = 0; band < steel_bands; ++band) {
        const int32_t first = rect.y + rect.height * band / steel_bands;
        const int32_t end = rect.y + rect.height * (band + 1) / steel_bands;
        const kit::Rect stripe{rect.x, first, rect.width, end - first};
        const double along = (static_cast<double>(band) + 0.5) / steel_bands;
        if (along < steel_sheen_reach)
            kit::blend_rect(
                canvas,
                stripe,
                steel_sheen_colour,
                static_cast<uint32_t>(std::lround(
                    steel_sheen_opacity * (steel_sheen_reach - along) / steel_sheen_reach
                ))
            );
        else if (along > 1.0 - steel_shade_reach)
            kit::blend_rect(
                canvas,
                stripe,
                outline_colour,
                static_cast<uint32_t>(std::lround(
                    steel_shade_opacity * (along - 1.0 + steel_shade_reach) / steel_shade_reach
                ))
            );
    }
    kit::outline_rect(canvas, rect, outline_colour);
    kit::bevel_rect(
        canvas, {rect.x + 1, rect.y + 1, rect.width - 2, rect.height - 2}, steel_light_colour,
        steel_dark_colour
    );
}

void draw_glow(const kit::Canvas& canvas, const kit::Rect& rect, kit::Colour ink, int32_t reach) {
    for (int32_t step = 1; step <= reach; ++step) {
        const auto opacity =
            static_cast<uint32_t>(glow_opacity * static_cast<uint32_t>(reach - step + 1) /
                                  static_cast<uint32_t>(reach + 1));
        const kit::Rect ring{
            rect.x - step, rect.y - step, rect.width + 2 * step, rect.height + 2 * step
        };
        kit::blend_rect(canvas, {ring.x, ring.y, ring.width, 1}, ink, opacity);
        kit::blend_rect(canvas, {ring.x, ring.y + ring.height - 1, ring.width, 1}, ink, opacity);
        kit::blend_rect(canvas, {ring.x, ring.y + 1, 1, ring.height - 2}, ink, opacity);
        kit::blend_rect(
            canvas, {ring.x + ring.width - 1, ring.y + 1, 1, ring.height - 2}, ink, opacity
        );
    }
}

void draw_panel_frame(const kit::Canvas& canvas, const kit::Rect& panel, int32_t rim) {
    if (panel.width <= 0 || panel.height <= 0)
        return;
    if (rim < 3 || panel.width <= 4 * rim || panel.height <= 4 * rim) {
        kit::blend_rect(canvas, panel, plate_colour, plate_opacity);
        kit::outline_rect(canvas, panel, plate_edge_colour);
        return;
    }
    const kit::Rect inside{
        panel.x + rim, panel.y + rim, panel.width - 2 * rim, panel.height - 2 * rim
    };
    kit::blend_rect(canvas, inside, panel_ground_colour, panel_ground_opacity);

    // The rim's four bars, the top one lit and the bottom and right ones
    // shaded.
    const kit::Rect top{panel.x, panel.y, panel.width, rim};
    const kit::Rect bottom{panel.x, panel.y + panel.height - rim, panel.width, rim};
    const kit::Rect left{panel.x, inside.y, rim, inside.height};
    const kit::Rect right{panel.x + panel.width - rim, inside.y, rim, inside.height};
    for (const kit::Rect& bar : {top, bottom, left, right})
        kit::fill_rect(canvas, bar, steel_colour);
    kit::blend_rect(canvas, top, steel_sheen_colour, rim_sheen_opacity);
    kit::blend_rect(canvas, bottom, outline_colour, rim_shade_opacity);
    kit::blend_rect(canvas, right, outline_colour, rim_shade_opacity / 2);

    kit::outline_rect(canvas, panel, outline_colour);
    kit::bevel_rect(
        canvas, {panel.x + 1, panel.y + 1, panel.width - 2, panel.height - 2}, steel_light_colour,
        steel_dark_colour
    );
    // The groove along the middle of the rim, sunk: shaded over, lit under.
    const int32_t groove = rim / 2;
    kit::bevel_rect(
        canvas,
        {panel.x + groove, panel.y + groove, panel.width - 2 * groove, panel.height - 2 * groove},
        steel_dark_colour,
        rim_groove_light_colour
    );
    // The inside, sunk into the rim.
    kit::bevel_rect(
        canvas, {inside.x - 1, inside.y - 1, inside.width + 2, inside.height + 2},
        steel_dark_colour, steel_light_colour
    );
    kit::outline_rect(canvas, inside, outline_colour);

    // A vent on each side a little in from each corner.
    const int32_t vent_height = (vent_lines - 1) * vent_pitch + 1;
    if (inside.height > 4 * rim + 2 * vent_height)
        for (const int32_t x : {left.x + 2, right.x + 2})
            for (const int32_t from :
                 {inside.y + 2 * rim, inside.y + inside.height - 2 * rim - vent_height})
                for (int32_t line = 0; line < vent_lines; ++line)
                    kit::fill_rect(
                        canvas, {x, from + line * vent_pitch, rim - 4, 1}, steel_dark_colour
                    );

    const int32_t radius = std::max(rim * 2 / 5, 1);
    const int32_t centre = rim / 2;
    for (const kit::Point corner :
         {kit::Point{panel.x + centre, panel.y + centre},
          kit::Point{panel.x + panel.width - centre - 1, panel.y + centre},
          kit::Point{panel.x + centre, panel.y + panel.height - centre - 1},
          kit::Point{panel.x + panel.width - centre - 1, panel.y + panel.height - centre - 1}})
        draw_bolt(canvas, corner, radius);
}

int32_t label_plate_width(
    const kit::Canvas& canvas, std::string_view text, int32_t height, int32_t tracking
) {
    const auto characters = static_cast<int32_t>(text.size());
    return kit::measure_text(canvas, kit::FontRole::regular, text) +
           tracking * std::max(characters - 1, 0) + 2 * height;
}

void draw_label_plate(
    const kit::Canvas& canvas, const kit::Rect& plate, std::string_view text, int32_t tracking
) {
    if (plate.width <= 0 || plate.height <= 0)
        return;
    draw_steel(canvas, plate, label_plate_colour);
    const int32_t radius = std::max(plate.height / 7, 1);
    const int32_t middle = plate.y + plate.height / 2;
    const int32_t end = plate.height / 3;
    draw_bolt(canvas, {plate.x + end, middle}, radius);
    draw_bolt(canvas, {plate.x + plate.width - end - 1, middle}, radius);
    outlined_text(
        canvas,
        kit::FontRole::regular,
        text,
        {plate.x + plate.height, plate.y, plate.width - 2 * plate.height, plate.height},
        kit::Align::left,
        caption_colour,
        tracking
    );
}

void draw_ruled_heading(
    const kit::Canvas& canvas, const kit::Rect& box, std::string_view text, int32_t gap
) {
    if (text.empty() || box.width <= 0 || box.height <= 0)
        return;
    outlined_text(
        canvas, kit::FontRole::regular, text, box, kit::Align::centre, caption_colour,
        heading_tracking
    );
    const auto characters = static_cast<int32_t>(text.size());
    const int32_t width = kit::measure_text(canvas, kit::FontRole::regular, text) +
                          heading_tracking * std::max(characters - 1, 0);
    const int32_t middle = box.x + box.width / 2;
    const int32_t row = box.y + box.height / 2 - 1;
    const int32_t reach = (box.width - width) / 2 - gap;
    if (reach <= 0)
        return;
    for (const int32_t left : {box.x, middle + width / 2 + gap}) {
        kit::fill_rect(canvas, {left, row, reach, 2}, rule_colour);
        kit::fill_rect(canvas, {left, row + 2, reach, 1}, outline_colour);
    }
}

void draw_version_plate(
    const kit::Canvas& canvas,
    int32_t screen_width,
    int32_t top,
    int32_t height,
    int32_t margin,
    std::string_view version
) {
    if (version.empty())
        return;
    const int32_t text_width = kit::measure_text(canvas, kit::FontRole::small, version);
    const int32_t width = std::min(text_width + 2 * margin, screen_width);
    const kit::Rect plate{(screen_width - width) / 2, top, width, height};
    draw_steel(canvas, plate, version_plate_colour);
    const int32_t radius = std::max(height / 10, 1);
    const int32_t inset = std::max(height / 5, radius + 2);
    for (const kit::Point corner :
         {kit::Point{plate.x + inset, plate.y + inset},
          kit::Point{plate.x + plate.width - inset - 1, plate.y + inset},
          kit::Point{plate.x + inset, plate.y + plate.height - inset - 1},
          kit::Point{plate.x + plate.width - inset - 1, plate.y + plate.height - inset - 1}})
        draw_bolt(canvas, corner, radius);
    kit::box_text(
        canvas, kit::FontRole::small, version, plate, kit::Align::centre, plate_text_colour
    );
}

std::string worded(std::string_view english) {
    return std::string(oa::data::languages::interface_text(english));
}

std::string filled(std::string pattern, std::string_view token, std::string_view value) {
    if (const auto at = pattern.find(token); at != std::string::npos)
        pattern.replace(at, token.size(), value);
    return pattern;
}

WindowIcon picture_of_frame(const oa::formats::gaf::Frame& frame, const oa::PaletteBytes& palette) {
    WindowIcon picture;
    auto rendered = oa::formats::gaf::render_normal(frame);
    if (!rendered.ok())
        return picture;
    const auto& drawn = *rendered.frame;
    const std::size_t pixels = static_cast<std::size_t>(drawn.width) * drawn.height;
    if (drawn.pixels.size() < pixels || drawn.coverage.size() < pixels)
        return picture;
    picture.width = drawn.width;
    picture.height = drawn.height;
    picture.pixels.assign(pixels * 4U, 0);
    for (std::size_t at = 0; at < pixels; ++at) {
        const std::size_t colour = static_cast<std::size_t>(drawn.pixels[at]) * 4U;
        if (drawn.coverage[at] == 0 || colour + 2U >= palette.size())
            continue;
        std::memcpy(picture.pixels.data() + at * 4U, palette.data() + colour, 3);
        picture.pixels[at * 4U + 3U] = 255;
    }
    return picture;
}

namespace {

/// Returns a pixel's brightness: its brightest channel, as opaque as the
/// pixel is.
///
/// @param pixel the pixel's four bytes
/// @return the brightness, 0 to 255
uint32_t brightness(const uint8_t* pixel) noexcept {
    return std::max({pixel[0], pixel[1], pixel[2]}) * uint32_t{pixel[3]} / 255U;
}

} // namespace

WindowIcon trimmed_picture(const WindowIcon& picture, uint32_t edge) {
    const std::size_t pixels = static_cast<std::size_t>(picture.width) * picture.height;
    if (picture.pixels.size() < pixels * 4U)
        return picture;
    uint32_t left = picture.width;
    uint32_t top = picture.height;
    uint32_t right = 0;
    uint32_t bottom = 0;
    for (uint32_t y = 0; y < picture.height; ++y)
        for (uint32_t x = 0; x < picture.width; ++x) {
            const std::size_t at = static_cast<std::size_t>(y) * picture.width + x;
            if (brightness(picture.pixels.data() + at * 4U) <= edge)
                continue;
            left = std::min(left, x);
            top = std::min(top, y);
            right = std::max(right, x + 1);
            bottom = std::max(bottom, y + 1);
        }
    if (right <= left || bottom <= top)
        return picture;
    WindowIcon cut;
    cut.width = right - left;
    cut.height = bottom - top;
    cut.pixels.resize(static_cast<std::size_t>(cut.width) * cut.height * 4U);
    for (uint32_t y = 0; y < cut.height; ++y)
        std::copy_n(
            picture.pixels.data() +
                ((static_cast<std::size_t>(top + y) * picture.width) + left) * 4U,
            static_cast<std::size_t>(cut.width) * 4U,
            cut.pixels.data() + static_cast<std::size_t>(y) * cut.width * 4U
        );
    return cut;
}

WindowIcon tinted_picture(
    const WindowIcon& picture, kit::Colour ground, kit::Colour ink, uint32_t floor, uint32_t peak
) {
    WindowIcon lit = picture;
    const std::size_t pixels = static_cast<std::size_t>(picture.width) * picture.height;
    if (lit.pixels.size() < pixels * 4U || peak <= floor)
        return {};
    const auto toward = [](uint8_t from, uint8_t to, uint32_t share) {
        return static_cast<uint8_t>((from * (255U - share) + to * share) / 255U);
    };
    for (std::size_t at = 0; at < pixels; ++at) {
        uint8_t* pixel = lit.pixels.data() + at * 4U;
        const uint32_t bright = std::clamp(brightness(pixel), floor, peak);
        const uint32_t share = (bright - floor) * 255U / (peak - floor);
        pixel[0] = toward(ground.r, ink.r, share);
        pixel[1] = toward(ground.g, ink.g, share);
        pixel[2] = toward(ground.b, ink.b, share);
        pixel[3] = 255;
    }
    return lit;
}

void draw_upright_arrow(
    const kit::Canvas& canvas, kit::Point tip, int32_t reach, bool down, kit::Colour ink
) {
    const auto triangle = [&](int32_t shift, kit::Colour colour) {
        for (int32_t step = 0; step <= reach; ++step) {
            const int32_t row = down ? tip.y - step : tip.y + step;
            kit::fill_rect(canvas, {tip.x - step + shift, row + shift, 2 * step + 1, 1}, colour);
        }
    };
    triangle(1, outline_colour);
    triangle(0, ink);
}

void draw_steel_button(const kit::Canvas& canvas, const kit::Rect& rect, kit::Colour face) {
    draw_steel(canvas, rect, face);
    // The bolts follow the shorter side, so that a tall narrow plate, such
    // as a scroll bar's thumb, keeps them small and inside it.
    const int32_t side = std::min(rect.width, rect.height);
    const int32_t radius = side / 14;
    if (radius < 1)
        return;
    const int32_t inset = std::max(side / 6, radius + 2);
    for (const kit::Point corner :
         {kit::Point{rect.x + inset, rect.y + inset},
          kit::Point{rect.x + rect.width - inset - 1, rect.y + inset},
          kit::Point{rect.x + inset, rect.y + rect.height - inset - 1},
          kit::Point{rect.x + rect.width - inset - 1, rect.y + rect.height - inset - 1}})
        draw_bolt(canvas, corner, radius);
}

void draw_sunk_edge(const kit::Canvas& canvas, const kit::Rect& rect) {
    kit::bevel_rect(canvas, rect, steel_dark_colour, sunk_light_colour);
    kit::outline_rect(
        canvas, {rect.x + 1, rect.y + 1, rect.width - 2, rect.height - 2}, outline_colour
    );
}

void draw_ring(const kit::Canvas& canvas, const kit::Rect& rect, int32_t width, kit::Colour ink) {
    for (int32_t step = 0; step < width; ++step)
        kit::outline_rect(
            canvas,
            {rect.x + step, rect.y + step, rect.width - 2 * step, rect.height - 2 * step},
            ink
        );
}

void draw_line(
    const kit::Canvas& canvas, kit::Point from, kit::Point to, int32_t width, kit::Colour ink
) {
    const int32_t steps = std::max(std::abs(to.x - from.x), std::abs(to.y - from.y));
    const int32_t half = width / 2;
    for (int32_t step = 0; step <= steps; ++step) {
        const double share = steps > 0 ? static_cast<double>(step) / steps : 0.0;
        const int32_t x = from.x + nearest((to.x - from.x) * share);
        const int32_t y = from.y + nearest((to.y - from.y) * share);
        kit::fill_rect(canvas, {x - half, y - half, std::max(width, 1), std::max(width, 1)}, ink);
    }
}

void draw_circle(const kit::Canvas& canvas, const kit::Rect& box, int32_t width, kit::Colour ink) {
    const int32_t side = std::min(box.width, box.height);
    const kit::Rect square{box.x + (box.width - side) / 2, box.y + (box.height - side) / 2, side, side};
    for (int32_t step = 0; step < width; ++step)
        kit::outline_round_rect(
            canvas,
            {square.x + step, square.y + step, square.width - 2 * step, square.height - 2 * step},
            std::max(side / 2 - step, 0),
            ink
        );
}

} // namespace oa::app::menu_paint
