// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// Convex polygon scan conversion and range rings. The polygon fills build
// a per-scanline table of left/right edge crossings (plus interpolated depth
// and shade in 16.16) by walking the vertex loop both ways from the topmost
// vertex, then hand each row to a span writer. The depth-tested variants
// draw into a Sprite whose `aux` plane is an 8-bit depth buffer: a pixel is
// written when the stored depth is <= the interpolated depth's integer byte.

#include "oa/present/surface.h"

#include <cstdint>

namespace oa::present {

// Rows available in a span table (0x14000 bytes of 0x28-byte rows).
inline constexpr int32_t polygon_span_rows = 2048;

// Angular step between range-ring vertices (1/32 turn in 16-bit angle units).
inline constexpr int32_t range_ring_step = 0x800;
inline constexpr int32_t full_turn = 0x10000;

struct PolygonVertex {
    int32_t x{};
    int32_t y{};
};

struct DepthVertex {
    int32_t x{};
    int32_t y{};
    int32_t depth{}; // integer depth; interpolated as 16.16
};

struct ShadedVertex {
    int32_t x{};
    int32_t y{};
    int32_t depth{};
    int32_t shade{}; // row of the display shade table
};

// One scanline of a span table. Edge x values are integers; depth and shade
// are 16.16. The untextured fills here never touch the four texture words;
// they take the names model::MeshSpanRow, which has the same layout, gives
// its texture coordinates.
struct PolygonSpanRow {
    int32_t left{};
    int32_t right{};   // exclusive
    int32_t left_u{};  // ? 16.16 texture u at the left edge
    int32_t left_v{};  // ? 16.16 texture v at the left edge
    int32_t right_u{}; // ? 16.16 texture u at the right edge
    int32_t right_v{}; // ? 16.16 texture v at the right edge
    int32_t left_depth{};
    int32_t right_depth{};
    int32_t left_shade{};
    int32_t right_shade{};
};

static_assert(sizeof(PolygonSpanRow) == 0x28);

/// Draws a range ring: a 32-segment circle of clipped lines.
///
/// The first chord starts at (cx + radius, cy); vertices come from the
/// 512-sample sine table in 1/32-turn steps.
///
/// @param[in,out] target target surface; null for the locked display surface
/// @param cx centre column
/// @param cy centre row
/// @param radius radius in pixels
/// @param color line colour
void draw_range_ring(
    Surface* target, int32_t cx, int32_t cy, int32_t radius, uint8_t color
) noexcept;

/// Draws a dashed range ring: a circle of chords, only every other chord drawn.
///
/// A chord is drawn when its running index, starting at `phase`, is odd.
///
/// @param[in,out] target target surface; null for the locked display surface
/// @param cx centre column
/// @param cy centre row
/// @param radius radius in pixels
/// @param color line colour
/// @param segments number of chords per turn; non-positive draws nothing
/// @param phase index of the first chord
void draw_dashed_range_ring(
    Surface* target,
    int32_t cx,
    int32_t cy,
    int32_t radius,
    uint8_t color,
    int32_t segments,
    int32_t phase
) noexcept;

/// Fills a convex polygon clipped to the surface clip rectangle.
///
/// The right edge and the clip's last row and column stay unfilled.
///
/// @param[in,out] target target surface; null for the locked display surface
/// @param vertices polygon vertices in order around the outline
/// @param count number of vertices
/// @param color fill colour
/// @return 1 when anything was scanned; 0 when the polygon misses the clip
///     rectangle, is flat or the display cannot be locked
int32_t
fill_polygon(Surface* target, const PolygonVertex* vertices, int32_t count, uint8_t color) noexcept;

/// Plots both end pixels of one span row, depth tested when the sprite has a depth plane.
///
/// An end outside the sprite is left out.
///
/// @param y sprite row
/// @param row span row: integer left and exclusive right edges, 16.16 depths
/// @param[in,out] target sprite whose pixels (and depth plane) receive the ends
/// @param color pixel colour
void plot_span_ends(int32_t y, const PolygonSpanRow& row, Sprite& target, uint8_t color) noexcept;

/// Plots the left and right edge pixels of every scanline of a convex polygon.
///
/// An outline without horizontal runs; depth tested when the sprite has a
/// depth plane. The polygon is scanned unclipped, as 3.1c scans it, and
/// only the ends inside the sprite are plotted: a model cut to a composite
/// smaller than itself (limits.model-composite clamp-oversize) is outlined
/// where the cut leaves it.
///
/// @param[in,out] target sprite to draw into
/// @param vertices polygon vertices in order around the outline
/// @param count number of vertices; non-positive draws nothing
/// @param color pixel colour
/// @return 1 when the polygon was scanned; 0 for no vertices or a flat polygon
int32_t outline_depth_polygon(
    Sprite& target, const DepthVertex* vertices, int32_t count, uint8_t color
) noexcept;

/// Fills one span row with a colour, depth tested when the sprite has a depth plane.
///
/// @param y sprite row
/// @param[in,out] row span row; clipped in place to x >= 0 and x < width - 1,
///     the left depth moved along with the left edge
/// @param[in,out] target sprite whose pixels (and depth plane) are written
/// @param color fill colour
void fill_depth_span(int32_t y, PolygonSpanRow& row, Sprite& target, uint8_t color) noexcept;

/// Fills a convex depth polygon clipped to the sprite, its last row and column excluded.
///
/// @param[in,out] target sprite to draw into; depth tested when it has a depth plane
/// @param vertices polygon vertices in order around the outline
/// @param count number of vertices
/// @param color fill colour
/// @return 1 when the polygon was scanned; 0 when it misses the sprite or is flat
int32_t fill_depth_polygon(
    Sprite& target, const DepthVertex* vertices, int32_t count, uint8_t color
) noexcept;

/// Fills one span row with a colour remapped through the interpolated row of the display shade table.
///
/// The shade table has 256 entries per row; nothing is drawn without one.
/// Depth tested when the sprite has a depth plane.
///
/// @param y sprite row
/// @param[in,out] row span row; clipped in place as fill_depth_span does, the
///     left depth and shade moved along with the left edge
/// @param[in,out] target sprite whose pixels (and depth plane) are written
/// @param color colour remapped by the shade rows
void fill_shaded_span(int32_t y, PolygonSpanRow& row, Sprite& target, uint8_t color) noexcept;

/// Fills a convex Gouraud-shaded depth polygon clipped to the sprite.
///
/// @param[in,out] target sprite to draw into; depth tested when it has a depth plane
/// @param vertices polygon vertices with depth and shade row, in order around the outline
/// @param count number of vertices
/// @param color colour remapped by the shade rows
/// @return 1 when the polygon was scanned; 0 when it misses the sprite or is flat
int32_t fill_shaded_polygon(
    Sprite& target, const ShadedVertex* vertices, int32_t count, uint8_t color
) noexcept;

} // namespace oa::present
