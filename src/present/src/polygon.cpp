// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/present/polygon.hpp"

#include "oa/present/display.hpp"
#include "oa/present/raster.hpp"
#include "oa/present/surface.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace oa::present {

namespace {

// Signed 1.13 sine samples, 512 per turn, shared with the movement code.
constexpr int16_t sine_samples[512] = {
#include "oa/base/game_math/trig_table.inc"
};
constexpr uint32_t sine_index_bias = 0x20;
constexpr unsigned sine_index_shift = 7;
constexpr uint32_t sine_index_mask = 511;
constexpr uint16_t quarter_turn = 0x4000;
constexpr int64_t sine_rounding = 0x1000;
constexpr unsigned sine_fraction_bits = 13;

// Initial extremes of the vertex bounding pass.
constexpr int32_t bound_low = 999999;
constexpr int32_t bound_high = -999999;

constexpr unsigned fixed_shift = 16;
// Edge x starts at x + 0xFFFF/0x10000, so the integer part rounds up.
constexpr int32_t edge_round_up = 0xFFFF;
constexpr int32_t shade_row_bytes = 256;

// magnitude * sin(angle): the table sample times the magnitude, plus 0x1000,
// shifted right by 13 bits; the low 32 bits are kept.
int32_t scaled_sine(uint16_t angle, int32_t magnitude) noexcept {
    const int32_t sample =
        sine_samples[((angle + sine_index_bias) >> sine_index_shift) & sine_index_mask];
    const int64_t scaled = static_cast<int64_t>(sample) * magnitude + sine_rounding;
    return static_cast<int32_t>(static_cast<uint32_t>(scaled >> sine_fraction_bits));
}

// Cosine form.
int32_t scaled_cosine(uint16_t angle, int32_t magnitude) noexcept {
    return scaled_sine(static_cast<uint16_t>(angle + quarter_turn), magnitude);
}

int32_t wrap_add(int32_t a, int32_t b) noexcept {
    return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
}

int32_t wrap_sub(int32_t a, int32_t b) noexcept {
    return static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b));
}

int32_t wrap_mul(int32_t a, int32_t b) noexcept {
    return static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b));
}

int32_t to_fixed(int32_t value) noexcept {
    return static_cast<int32_t>(static_cast<uint32_t>(value) << fixed_shift);
}

// Signed division truncated toward zero; callers guarantee a positive divisor.
int32_t divide(int32_t a, int32_t b) noexcept {
    if (a == std::numeric_limits<int32_t>::min() && b == -1)
        return a;
    return a / b;
}

uint8_t depth_byte(int32_t fixed) noexcept {
    return static_cast<uint8_t>(fixed >> fixed_shift);
}

// Vertex attributes interpolated alongside x: depth, then shade.
int32_t vertex_attributes(const PolygonVertex&, int32_t*) noexcept {
    return 0;
}

int32_t vertex_attributes(const DepthVertex& v, int32_t* out) noexcept {
    out[0] = v.depth;
    return 1;
}

int32_t vertex_attributes(const ShadedVertex& v, int32_t* out) noexcept {
    out[0] = v.depth;
    out[1] = v.shade;
    return 2;
}

// Per-thread span table. Rows no edge reaches (non-convex input) read as
// empty. A polygon writes and reads only the rows from its top to its
// bottom, so only those are cleared, or all of them where that span is not
// a count of rows the table holds.
PolygonSpanRow* cleared_span_table(int32_t used) noexcept {
    static thread_local PolygonSpanRow rows[polygon_span_rows];
    const int32_t cleared = used >= 0 && used < polygon_span_rows ? used : polygon_span_rows;
    for (int32_t row = 0; row < cleared; ++row)
        rows[row] = PolygonSpanRow{};
    return rows;
}

struct Bounds {
    int32_t min_x = bound_low;
    int32_t max_x = bound_high;
    int32_t min_y = bound_low;
    int32_t max_y = bound_high;
    int32_t top = 0;    // first vertex with the smallest y
    int32_t bottom = 0; // first vertex with the largest y
};

template <typename Vertex>
Bounds polygon_bounds(const Vertex* vertices, int32_t count) noexcept {
    Bounds b;
    for (int32_t i = 0; i < count; ++i) {
        const Vertex& v = vertices[i];
        if (v.y < b.min_y) {
            b.min_y = v.y;
            b.top = i;
        }
        if (v.y > b.max_y) {
            b.max_y = v.y;
            b.bottom = i;
        }
        if (v.x > b.max_x)
            b.max_x = v.x;
        if (v.x < b.min_x)
            b.min_x = v.x;
    }
    return b;
}

// Scan-converts the edge from `from` down to `to` into successive rows of
// one side, starting at row `cursor`. Rows above `top` are skipped by
// pre-stepping; the edge stops before row min(to.y, bottom).
template <typename Vertex>
void scan_edge(
    PolygonSpanRow* rows,
    int32_t& cursor,
    bool right,
    const Vertex& from,
    const Vertex& to,
    int32_t top,
    int32_t bottom
) noexcept {
    if (to.y <= top || from.y >= to.y)
        return;
    const int32_t dy = wrap_sub(to.y, from.y);
    const int32_t x_step = divide(to_fixed(wrap_sub(to.x, from.x)), dy);
    int32_t x = wrap_add(to_fixed(from.x), edge_round_up);
    int32_t a0[2]{};
    int32_t a1[2]{};
    const int32_t attributes = vertex_attributes(from, a0);
    vertex_attributes(to, a1);
    int32_t value[2]{};
    int32_t step[2]{};
    for (int32_t i = 0; i < attributes; ++i) {
        value[i] = to_fixed(a0[i]);
        step[i] = divide(wrap_sub(to_fixed(a1[i]), to_fixed(a0[i])), dy);
    }
    int32_t y = from.y;
    if (y < top) {
        const int32_t skipped = wrap_sub(top, y);
        x = wrap_add(x, wrap_mul(skipped, x_step));
        for (int32_t i = 0; i < attributes; ++i)
            value[i] = wrap_add(value[i], wrap_mul(skipped, step[i]));
        y = top;
    }
    const int32_t end = to.y > bottom ? bottom : to.y;
    for (; y < end && cursor < polygon_span_rows; ++y, ++cursor) {
        PolygonSpanRow& row = rows[cursor];
        (right ? row.right : row.left) = x >> fixed_shift;
        if (attributes > 0)
            (right ? row.right_depth : row.left_depth) = value[0];
        if (attributes > 1)
            (right ? row.right_shade : row.left_shade) = value[1];
        x = wrap_add(x, x_step);
        for (int32_t i = 0; i < attributes; ++i)
            value[i] = wrap_add(value[i], step[i]);
    }
}

// Fills the left column walking backwards from the top vertex and the right
// column walking forwards, each until the bottom vertex. Both columns start
// at row zero; convex input keeps them aligned with the first scanline.
template <typename Vertex>
void scan_polygon(
    PolygonSpanRow* rows,
    const Vertex* vertices,
    int32_t count,
    const Bounds& b,
    int32_t top,
    int32_t bottom
) noexcept {
    int32_t cursor = 0;
    int32_t i = b.top;
    do {
        const int32_t next = i - 1 < 0 ? count - 1 : i - 1;
        scan_edge(rows, cursor, false, vertices[i], vertices[next], top, bottom);
        i = next;
    } while (i != b.bottom);
    cursor = 0;
    i = b.top;
    do {
        const int32_t next = i + 1 >= count ? 0 : i + 1;
        scan_edge(rows, cursor, true, vertices[i], vertices[next], top, bottom);
        i = next;
    } while (i != b.bottom);
}

// Rows handed to a span writer: [first, last) clipped to the table.
template <typename Span>
void emit_rows(
    PolygonSpanRow* rows, int32_t first, int32_t last, Sprite& target, uint8_t color, Span span
) noexcept {
    for (int32_t y = first, r = 0; y < last && r < polygon_span_rows; ++y, ++r) {
        PolygonSpanRow& row = rows[r];
        if (wrap_sub(row.right, row.left) > 0)
            span(y, row, target, color);
    }
}

std::ptrdiff_t sprite_offset(const Sprite& sprite, int32_t x, int32_t y) noexcept {
    return static_cast<std::ptrdiff_t>(
        wrap_add(wrap_mul(static_cast<int32_t>(sprite.width), y), x)
    );
}

// Vertical extent check shared by the sprite-clipped fills; narrows
// [min_y, max_y] to [0, height - 1]. False when nothing can be drawn.
bool clip_to_sprite(const Sprite& target, Bounds& b, int32_t& last_row) noexcept {
    if (b.min_x > static_cast<int32_t>(target.width) - 1 || b.max_y < 0)
        return false;
    last_row = static_cast<int32_t>(target.height) - 1;
    if (b.min_y > last_row)
        return false;
    if (b.min_y < 0)
        b.min_y = 0;
    if (b.max_y > last_row)
        b.max_y = last_row;
    return b.max_y != b.min_y;
}

// Plots one end of a span row, depth tested when the sprite has a depth
// plane; an end outside the sprite is left out.
void plot_span_end(Sprite& target, int32_t x, int32_t y, int32_t depth, uint8_t color) noexcept {
    if (x < 0 || x >= static_cast<int32_t>(target.width) || y < 0 ||
        y >= static_cast<int32_t>(target.height))
        return;
    const std::ptrdiff_t offset = sprite_offset(target, x, y);
    uint8_t* pixel = static_cast<uint8_t*>(target.data) + offset;
    if (target.aux == nullptr) {
        *pixel = color;
        return;
    }
    uint8_t* stored = static_cast<uint8_t*>(target.aux) + offset;
    const uint8_t value = depth_byte(depth);
    if (*stored <= value) {
        *pixel = color;
        *stored = value;
    }
}

} // namespace

void draw_range_ring(
    Surface* target, int32_t cx, int32_t cy, int32_t radius, uint8_t color
) noexcept {
    int32_t px = wrap_add(cx, radius);
    int32_t py = cy;
    for (int32_t angle = range_ring_step; angle <= full_turn; angle += range_ring_step) {
        const int32_t x = wrap_add(scaled_cosine(static_cast<uint16_t>(angle), radius), cx);
        const int32_t y = wrap_add(scaled_sine(static_cast<uint16_t>(angle), radius), cy);
        draw_clipped_line(target, px, py, x, y, color);
        px = x;
        py = y;
    }
}

void draw_dashed_range_ring(
    Surface* target,
    int32_t cx,
    int32_t cy,
    int32_t radius,
    uint8_t color,
    int32_t segments,
    int32_t phase
) noexcept {
    if (segments <= 0)
        return;
    const int32_t step = full_turn / segments;
    int32_t px = wrap_add(cx, radius);
    int32_t py = cy;
    for (int32_t angle = step; angle <= full_turn; angle += step) {
        const int32_t x = wrap_add(scaled_cosine(static_cast<uint16_t>(angle), radius), cx);
        const int32_t y = wrap_add(scaled_sine(static_cast<uint16_t>(angle), radius), cy);
        if ((phase & 1) != 0)
            draw_clipped_line(target, px, py, x, y, color);
        phase = wrap_add(phase, 1);
        px = x;
        py = y;
    }
}

int32_t fill_polygon(
    Surface* target, const PolygonVertex* vertices, int32_t count, uint8_t color
) noexcept {
    Surface locked{};
    const bool uses_display = target == nullptr;
    if (uses_display) {
        if (lock_display_surface(locked) == 0)
            return 0;
        target = &locked;
    }
    Bounds b = polygon_bounds(vertices, count);
    const Rect32 clip = surface_clip(*target);
    // The rows the clip gives are worked out whole; a band (surface_band)
    // only leaves the rows outside it unwritten.
    const SurfaceRows band = surface_band(*target);
    int32_t drawn = 0;
    if (b.max_x >= clip.x1 && b.min_x <= clip.x2 && b.max_y >= clip.y1 && b.min_y <= clip.y2) {
        if (b.min_y < clip.y1)
            b.min_y = clip.y1;
        if (b.max_y > clip.y2)
            b.max_y = clip.y2;
        if (b.max_y != b.min_y) {
            const int32_t first_row = b.min_y < band.first ? band.first : b.min_y;
            const int32_t end_row = b.max_y > band.end ? band.end : b.max_y;
            if (first_row >= end_row) {
                if (uses_display)
                    unlock_display_surface();
                return 1;
            }
            PolygonSpanRow* rows = cleared_span_table(wrap_sub(b.max_y, b.min_y));
            scan_polygon(rows, vertices, count, b, clip.y1, clip.y2);
            for (int32_t y = first_row, r = first_row - b.min_y;
                 y < end_row && r < polygon_span_rows;
                 ++y, ++r) {
                PolygonSpanRow& row = rows[r];
                if (row.right > clip.x2)
                    row.right = clip.x2;
                if (row.left < clip.x1)
                    row.left = clip.x1;
                const int32_t width = wrap_sub(row.right, row.left);
                if (width <= 0)
                    continue;
                uint8_t* first = target->pixels + wrap_add(wrap_mul(target->pitch, y), row.left);
                for (int32_t i = 0; i < width; ++i)
                    first[i] = color;
            }
            drawn = 1;
        }
    }
    if (uses_display)
        unlock_display_surface();
    return drawn;
}

void plot_span_ends(int32_t y, const PolygonSpanRow& row, Sprite& target, uint8_t color) noexcept {
    if (wrap_sub(row.right, row.left) <= 0)
        return;
    plot_span_end(target, row.left, y, row.left_depth, color);
    plot_span_end(target, row.right, y, row.right_depth, color);
}

int32_t outline_depth_polygon(
    Sprite& target, const DepthVertex* vertices, int32_t count, uint8_t color
) noexcept {
    if (count <= 0)
        return 0;
    const Bounds b = polygon_bounds(vertices, count);
    if (b.max_y == b.min_y)
        return 0;
    PolygonSpanRow* rows = cleared_span_table(wrap_sub(b.max_y, b.min_y));
    scan_polygon(
        rows,
        vertices,
        count,
        b,
        std::numeric_limits<int32_t>::min(),
        std::numeric_limits<int32_t>::max()
    );
    emit_rows(rows, b.min_y, b.max_y, target, color, plot_span_ends);
    return 1;
}

void fill_depth_span(int32_t y, PolygonSpanRow& row, Sprite& target, uint8_t color) noexcept {
    const int32_t step =
        divide(wrap_sub(row.right_depth, row.left_depth), wrap_sub(row.right, row.left));
    if (row.left < 0) {
        row.left_depth = wrap_sub(row.left_depth, wrap_mul(row.left, step));
        row.left = 0;
    }
    const int32_t last = static_cast<int32_t>(target.width) - 1;
    if (row.right > last)
        row.right = last;
    const int32_t span = wrap_sub(row.right, row.left);
    if (span <= 0)
        return;
    const std::ptrdiff_t offset = sprite_offset(target, row.left, y);
    uint8_t* pixel = static_cast<uint8_t*>(target.data) + offset;
    if (target.aux == nullptr) {
        for (int32_t i = 0; i < span; ++i)
            pixel[i] = color;
        return;
    }
    uint8_t* depth = static_cast<uint8_t*>(target.aux) + offset;
    int32_t z = row.left_depth;
    for (int32_t i = 0; i < span; ++i) {
        const uint8_t d = depth_byte(z);
        if (depth[i] <= d) {
            pixel[i] = color;
            depth[i] = d;
        }
        z = wrap_add(z, step);
    }
}

int32_t fill_depth_polygon(
    Sprite& target, const DepthVertex* vertices, int32_t count, uint8_t color
) noexcept {
    Bounds b = polygon_bounds(vertices, count);
    int32_t last_row = 0;
    if (!clip_to_sprite(target, b, last_row))
        return 0;
    PolygonSpanRow* rows = cleared_span_table(wrap_sub(b.max_y, b.min_y));
    scan_polygon(rows, vertices, count, b, 0, last_row);
    emit_rows(rows, b.min_y, b.max_y, target, color, fill_depth_span);
    return 1;
}

void fill_shaded_span(int32_t y, PolygonSpanRow& row, Sprite& target, uint8_t color) noexcept {
    const DisplayContext* display = display_context();
    const int32_t width = wrap_sub(row.right, row.left);
    const int32_t depth_step = divide(wrap_sub(row.right_depth, row.left_depth), width);
    const int32_t shade_step = divide(wrap_sub(row.right_shade, row.left_shade), width);
    if (row.left < 0) {
        row.left_depth = wrap_sub(row.left_depth, wrap_mul(row.left, depth_step));
        row.left_shade = wrap_sub(row.left_shade, wrap_mul(row.left, shade_step));
        row.left = 0;
    }
    const int32_t last = static_cast<int32_t>(target.width) - 1;
    if (row.right > last)
        row.right = last;
    const int32_t span = wrap_sub(row.right, row.left);
    if (span <= 0 || display == nullptr || display->shade_table == nullptr)
        return;
    const uint8_t* table = display->shade_table;
    auto shaded = [&](int32_t shade) {
        const int32_t index = wrap_add(wrap_mul(shade >> fixed_shift, shade_row_bytes), color);
        return table[static_cast<std::ptrdiff_t>(index)];
    };
    const std::ptrdiff_t offset = sprite_offset(target, row.left, y);
    uint8_t* pixel = static_cast<uint8_t*>(target.data) + offset;
    int32_t shade = row.left_shade;
    if (target.aux == nullptr) {
        for (int32_t i = 0; i < span; ++i) {
            pixel[i] = shaded(shade);
            shade = wrap_add(shade, shade_step);
        }
        return;
    }
    uint8_t* depth = static_cast<uint8_t*>(target.aux) + offset;
    int32_t z = row.left_depth;
    for (int32_t i = 0; i < span; ++i) {
        const uint8_t d = depth_byte(z);
        if (depth[i] <= d) {
            pixel[i] = shaded(shade);
            depth[i] = d;
        }
        shade = wrap_add(shade, shade_step);
        z = wrap_add(z, depth_step);
    }
}

int32_t fill_shaded_polygon(
    Sprite& target, const ShadedVertex* vertices, int32_t count, uint8_t color
) noexcept {
    Bounds b = polygon_bounds(vertices, count);
    int32_t last_row = 0;
    if (!clip_to_sprite(target, b, last_row))
        return 0;
    PolygonSpanRow* rows = cleared_span_table(wrap_sub(b.max_y, b.min_y));
    scan_polygon(rows, vertices, count, b, 0, last_row);
    emit_rows(rows, b.min_y, b.max_y, target, color, fill_shaded_span);
    return 1;
}

} // namespace oa::present
