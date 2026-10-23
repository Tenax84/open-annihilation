// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// What Open Annihilation's menus on the OA layer draw alike: the column
// menus (column_menu.hpp), the New Campaign screen (campaign_menu.hpp),
// the Skirmish screen (skirmish_menu.hpp), Settings, the Library and the
// Multiplayer screen (multiplayer_menu.hpp). They share the main menu's art
// (branding/main-menu), its colours, the backdrop put together from the
// battlefield picture, the outlined captions, the heading between metal
// rules, the steel: bolted frames round dark panels, bevelled plates and
// bolts, lines and rings, and the version plate.
#pragma once

#include "oa/app/window_icon.hpp"
#include "oa/formats/gaf.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/ui/kit/canvas.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/text.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app::menu_paint {

/// The window height the menus' measures are given for.
inline constexpr double design_rows = 1080.0;
/// The heading's extra points after each character.
inline constexpr int32_t heading_tracking = 3;
/// The font a secondary button's caption is drawn in: a steel button's, on
/// every menu. A primary button, drawn with the menus' faces, has its
/// caption in the regular font.
inline constexpr oa::ui::kit::FontRole secondary_caption_font = oa::ui::kit::FontRole::small;

inline constexpr oa::ui::kit::Colour caption_colour{236, 230, 198};       ///< a caption, cream
inline constexpr oa::ui::kit::Colour caption_lit_colour{255, 250, 222};   ///< a highlighted caption
inline constexpr oa::ui::kit::Colour caption_faded_colour{150, 152, 144}; ///< a disabled caption
inline constexpr oa::ui::kit::Colour outline_colour{18, 20, 16};          ///< round every caption
inline constexpr oa::ui::kit::Colour key_colour{70, 190, 70};             ///< the quick key's underline
inline constexpr oa::ui::kit::Colour rule_colour{168, 172, 160};          ///< the heading's rules, metal
inline constexpr oa::ui::kit::Colour plate_colour{14, 20, 26};            ///< the version plate and the panels
inline constexpr oa::ui::kit::Colour plate_edge_colour{92, 104, 112};     ///< their edge
/// The panels' edge inside their own.
inline constexpr oa::ui::kit::Colour panel_inner_edge_colour{40, 46, 50};
inline constexpr oa::ui::kit::Colour steel_colour{64, 70, 72};         ///< a steel plate's face
inline constexpr oa::ui::kit::Colour steel_light_colour{164, 170, 166}; ///< its lit edge
inline constexpr oa::ui::kit::Colour steel_dark_colour{18, 20, 22};     ///< its shaded edge
inline constexpr oa::ui::kit::Colour bolt_colour{126, 132, 126};        ///< a bolt's head
inline constexpr oa::ui::kit::Colour bolt_light_colour{214, 218, 206};  ///< the light on it
/// The lit lower edge of what is sunk into a panel.
inline constexpr oa::ui::kit::Colour sunk_light_colour{70, 78, 76};
/// The ground inside a steel frame.
inline constexpr oa::ui::kit::Colour panel_ground_colour{9, 13, 17};
/// The ground's opacity inside a steel frame, in 256ths.
inline constexpr uint32_t panel_ground_opacity = 228;
/// The version label and the panels' text.
inline constexpr oa::ui::kit::Colour plate_text_colour{196, 202, 204};
/// The version plate's and the panels' opacity, in 256ths.
inline constexpr uint32_t plate_opacity = 210;

/// A button's look, the face it is drawn with.
enum class Face : uint8_t { normal, hover, pressed, disabled };

/// The menus' art, decoded.
struct MenuArt {
    WindowIcon background{};           ///< behind everything
    WindowIcon title{};                ///< OPEN ANNIHILATION
    std::array<WindowIcon, 4> faces{}; ///< a button's face in each Face
};

/// Returns the menus' art, decoded the first time it is asked for and kept
/// for the rest of the run.
///
/// @return the art; a picture that cannot be decoded is empty and draws nothing
[[nodiscard]] const MenuArt& menu_art();

/// Returns a text in the language shown, from the interface catalogue.
///
/// @param english the text in English
/// @return the text
[[nodiscard]] std::string worded(std::string_view english);

/// Returns a text with a word in braces replaced.
///
/// @param pattern the text, holding the word
/// @param token the word, braces included
/// @param value what replaces it
/// @return the text
[[nodiscard]] std::string filled(std::string pattern, std::string_view token, std::string_view value);

/// Returns a GAF frame as a picture, through a palette; a pixel the frame
/// leaves uncovered is clear.
///
/// @param frame the frame
/// @param palette 4 bytes per colour
/// @return the picture; empty when the frame does not render
[[nodiscard]] WindowIcon picture_of_frame(
    const oa::formats::gaf::Frame& frame, const oa::PaletteBytes& palette
);

/// Returns a picture cut to its pixels brighter than a brightness, so that
/// a logo drawn small in the middle of a dark frame fills what it is drawn
/// in. A pixel's brightness is its brightest channel, as opaque as it is.
///
/// @param picture the picture
/// @param edge the brightness, 0 to 255, at and under which a pixel is cut
/// @return the cut picture; the picture whole when no pixel is brighter
[[nodiscard]] WindowIcon trimmed_picture(const WindowIcon& picture, uint32_t edge);

/// Returns a picture in one colour on a ground: each pixel as bright as its
/// brightest channel, as opaque as it is, was between a floor and a peak,
/// opaque.
///
/// @param picture the picture
/// @param ground the colour of a pixel at or under the floor, and of a clear one
/// @param ink the colour of a pixel at or over the peak
/// @param floor the brightness, 0 to 254, at and under which a pixel is the ground
/// @param peak the brightness, over floor, at and over which a pixel is the ink
/// @return the tinted picture; empty for an empty one
[[nodiscard]] WindowIcon tinted_picture(
    const WindowIcon& picture,
    oa::ui::kit::Colour ground,
    oa::ui::kit::Colour ink,
    uint32_t floor,
    uint32_t peak
);

/// Rounds a measure to the nearest point.
///
/// @param value the measure, in points
/// @return the point
[[nodiscard]] int32_t nearest(double value) noexcept;

/// Returns where the background picture lies on a screen of a size: it
/// covers the screen, cut at the edges it overhangs, mostly on the right,
/// so that the commander on its left stays in view.
///
/// @param width the screen's width, in points
/// @param height the screen's height, in points
/// @return the picture's place, in points
[[nodiscard]] oa::ui::kit::Rect background_place(int32_t width, int32_t height) noexcept;

/// Pictures of the art filtered to the pixels they are drawn on, each made
/// the first time it is drawn there and kept until clear: a redraw copies
/// the pixels and does not filter the pictures again.
class FittedArt {
  public:

    /// Draws a picture scaled to fill a rectangle.
    ///
    /// @param canvas where it draws
    /// @param rect the rectangle, in points
    /// @param picture the picture; it stays as long as the copies made of it
    void draw(
        const oa::ui::kit::Canvas& canvas, const oa::ui::kit::Rect& rect, const WindowIcon& picture
    ) const;

    /// Forgets every copy, as when the screen's size changes.
    void clear() noexcept { fitted_.clear(); }

  private:

    /// One picture filtered to a size.
    struct Fitted {
        const WindowIcon* picture{};   ///< the picture it is made from
        int32_t columns{};             ///< its width, in canvas pixels
        int32_t rows{};                ///< its height, in canvas pixels
        std::vector<uint8_t> pixels{}; ///< four bytes a pixel; empty when it cannot be made
    };

    mutable std::vector<Fitted> fitted_{}; ///< the copies made
};

/// Draws what lies under a menu: the ground, the background picture over it
/// cut by the screen's edges, and the shade over its top and bottom, so
/// that the title and the version plate stand out of it. They are put
/// together once, at the pixels the screen covers, and kept for every menu
/// until the size changes: a redraw, or another menu at the same size,
/// copies them.
///
/// @param canvas where it draws
/// @param size the screen's size, in points
/// @param background where the picture lies (background_place)
void draw_backdrop(
    const oa::ui::kit::Canvas& canvas, oa::ui::kit::Point size, const oa::ui::kit::Rect& background
);

/// Draws a text outlined a point all round, as the captions are.
///
/// @param canvas where it draws
/// @param role which font
/// @param text the text
/// @param box the box it is centred in
/// @param align where the text sits along the box's width
/// @param ink the text's colour
/// @param tracking extra points after each character but the last
void outlined_text(
    const oa::ui::kit::Canvas& canvas,
    oa::ui::kit::FontRole role,
    std::string_view text,
    const oa::ui::kit::Rect& box,
    oa::ui::kit::Align align,
    oa::ui::kit::Colour ink,
    int32_t tracking = 0
);

/// Returns where a UTF-8 caption holds its quick key: the first letter that
/// is the key, in either case.
///
/// @param caption the caption
/// @param key the key; 0 for none
/// @return the byte it starts at; none when the caption does not hold it
[[nodiscard]] std::optional<std::size_t> quick_key_at(std::string_view caption, char key);

/// Underlines the quick key of a caption centred in a box in the regular
/// font.
///
/// @param canvas where it draws
/// @param text the caption
/// @param box the box it is centred in
/// @param at the byte its quick key starts at (quick_key_at)
void underline_quick_key(
    const oa::ui::kit::Canvas& canvas,
    std::string_view text,
    const oa::ui::kit::Rect& box,
    std::size_t at
);

/// Draws an arrow pointing left or right, its tip at a point, with a shadow
/// a point right and down.
///
/// @param canvas where it draws
/// @param tip the arrow's tip
/// @param reach how far it reaches up and down from the tip, and back from it
/// @param left it points left; otherwise right
/// @param ink its colour
void draw_arrow(
    const oa::ui::kit::Canvas& canvas,
    oa::ui::kit::Point tip,
    int32_t reach,
    bool left,
    oa::ui::kit::Colour ink
);

/// Draws an arrow pointing up or down, its tip at a point, with a shadow a
/// point right and down.
///
/// @param canvas where it draws
/// @param tip the arrow's tip
/// @param reach how far it reaches left and right of the tip, and back from it
/// @param down it points down; otherwise up
/// @param ink its colour
void draw_upright_arrow(
    const oa::ui::kit::Canvas& canvas,
    oa::ui::kit::Point tip,
    int32_t reach,
    bool down,
    oa::ui::kit::Colour ink
);

/// Draws a small steel button: a bevelled steel plate with a bolt in each
/// corner when its shorter side is long enough for them.
///
/// @param canvas where it draws
/// @param rect the button
/// @param face its face's colour
void draw_steel_button(
    const oa::ui::kit::Canvas& canvas, const oa::ui::kit::Rect& rect, oa::ui::kit::Colour face
);

/// Draws what is sunk into a panel: a shaded edge over and left of it, a lit
/// one under and right, and a dark outline inside them.
///
/// @param canvas where it draws
/// @param rect what is sunk, its edges included
void draw_sunk_edge(const oa::ui::kit::Canvas& canvas, const oa::ui::kit::Rect& rect);

/// Draws a ring of a width round a rectangle, inside it.
///
/// @param canvas where it draws
/// @param rect the rectangle
/// @param width how many points wide
/// @param ink its colour
void draw_ring(
    const oa::ui::kit::Canvas& canvas,
    const oa::ui::kit::Rect& rect,
    int32_t width,
    oa::ui::kit::Colour ink
);

/// Draws a line of a width between two points, as squares along it.
///
/// @param canvas where it draws
/// @param from where it starts
/// @param to where it ends
/// @param width how thick it is, in points
/// @param ink its colour
void draw_line(
    const oa::ui::kit::Canvas& canvas,
    oa::ui::kit::Point from,
    oa::ui::kit::Point to,
    int32_t width,
    oa::ui::kit::Colour ink
);

/// Draws a ring: a circle's edge.
///
/// @param canvas where it draws
/// @param box the square it fills
/// @param width how thick it is, in points
/// @param ink its colour
void draw_circle(
    const oa::ui::kit::Canvas& canvas,
    const oa::ui::kit::Rect& box,
    int32_t width,
    oa::ui::kit::Colour ink
);

/// Draws a round bolt head: a dark ring, the head and the light on its
/// upper left.
///
/// @param canvas where it draws
/// @param centre its centre
/// @param radius its head's radius, in points; under 1 draws nothing
void draw_bolt(const oa::ui::kit::Canvas& canvas, oa::ui::kit::Point centre, int32_t radius);

/// Draws a bevelled steel plate: its face lit towards the top and shaded
/// towards the bottom, a dark outline, a lit edge over and left of it and a
/// shaded one under and right.
///
/// @param canvas where it draws
/// @param rect the plate, in points
/// @param face its face's colour
void draw_steel(
    const oa::ui::kit::Canvas& canvas, const oa::ui::kit::Rect& rect, oa::ui::kit::Colour face
);

/// Draws a glow round the outside of a rectangle, fading out from its edge.
///
/// @param canvas where it draws
/// @param rect the rectangle it lies round, in points
/// @param ink the glow's colour
/// @param reach how many points it reaches out
void draw_glow(
    const oa::ui::kit::Canvas& canvas,
    const oa::ui::kit::Rect& rect,
    oa::ui::kit::Colour ink,
    int32_t reach
);

/// Draws a panel in a steel frame: the dark ground inside, a rim of steel
/// with a groove along it, lit along its top and shaded along its bottom,
/// the inside sunk into it, a bolt in each corner and vents on its sides.
///
/// @param canvas where it draws
/// @param panel the panel, its rim included, in points
/// @param rim the rim's width, in points; under 3, or too wide for the
///     panel, draws a dark panel with a plain edge
void draw_panel_frame(
    const oa::ui::kit::Canvas& canvas, const oa::ui::kit::Rect& panel, int32_t rim
);

/// Returns how wide a label plate is for a text.
///
/// @param canvas where it is drawn
/// @param text the label
/// @param height the plate's height
/// @param tracking extra points after each character but the last
/// @return the width, in points
[[nodiscard]] int32_t label_plate_width(
    const oa::ui::kit::Canvas& canvas, std::string_view text, int32_t height, int32_t tracking
);

/// Draws a steel label plate, as wide as label_plate_width, with a bolt at
/// each end and its label spaced out between them.
///
/// @param canvas where it draws
/// @param plate the plate, in points
/// @param text the label, in the language shown
/// @param tracking extra points after each character but the last
void draw_label_plate(
    const oa::ui::kit::Canvas& canvas,
    const oa::ui::kit::Rect& plate,
    std::string_view text,
    int32_t tracking
);

/// Draws a heading spaced out, centred in a box, with a metal rule either
/// side of it reaching to the box's edges.
///
/// @param canvas where it draws
/// @param box the box
/// @param text the heading, in the language shown
/// @param gap the room between the heading and its rules
void draw_ruled_heading(
    const oa::ui::kit::Canvas& canvas,
    const oa::ui::kit::Rect& box,
    std::string_view text,
    int32_t gap
);

/// Draws the version plate centred across the screen, as wide as its label:
/// dark steel with a bolt in each corner.
///
/// @param canvas where it draws
/// @param screen_width the screen's width, in points
/// @param top the plate's top
/// @param height the plate's height
/// @param margin its room either side of the label
/// @param version the label; empty draws nothing
void draw_version_plate(
    const oa::ui::kit::Canvas& canvas,
    int32_t screen_width,
    int32_t top,
    int32_t height,
    int32_t margin,
    std::string_view version
);

} // namespace oa::app::menu_paint
