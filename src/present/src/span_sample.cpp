// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/present/span_sample.hpp"

#include "oa/present/display.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace oa::present {

namespace {

constexpr unsigned fixed_shift = 16;
constexpr uint32_t fraction_mask = 0xFFFF;

// Source position walker: a whole-texel step per sample plus two 32-bit
// fraction accumulators whose carries move one texel along each axis.
struct LineStepper {
    int32_t whole = 0;
    uint32_t x_fraction = 0;
    uint32_t y_fraction = 0;
    int32_t x_carry = 0; // +-1 or 0
    int32_t y_carry = 0; // +-pitch or 0
};

struct Quotient {
    uint32_t whole = 0;    // integer texels per sample
    uint32_t fraction = 0; // 0.32 fraction per sample
};

// (span << 16) / count with a 32-bit divide.
Quotient wide_quotient(uint32_t span, uint32_t count) noexcept {
    const uint32_t q = (span << fixed_shift) / count;
    return {q >> fixed_shift, q << fixed_shift};
}

// The same with a 16-bit divide of the low halves, used when the sample
// count exceeds the span. A count of 0x10000 or more would overflow the
// 16-bit quotient, so the wide form is used there.
Quotient narrow_quotient(uint32_t span, uint32_t count) noexcept {
    const uint32_t divisor = count & fraction_mask;
    if (divisor != 0) {
        const uint32_t q = ((span & fraction_mask) << fixed_shift) / divisor;
        if (q <= fraction_mask)
            return {0, q << fixed_shift};
    }
    return {0, wide_quotient(span, count).fraction};
}

int32_t wrap_add(int32_t a, int32_t b) noexcept {
    return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
}

int32_t wrap_mul(int32_t a, int32_t b) noexcept {
    return static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b));
}

void run_stepper(
    uint8_t* out, const uint8_t* origin, int32_t position, const LineStepper& s, uint32_t count
) noexcept {
    uint32_t x_accumulator = 0;
    uint32_t y_accumulator = 0;
    while (true) {
        *out++ = origin[static_cast<std::ptrdiff_t>(position)];
        if (--count == 0)
            return;
        position = wrap_add(position, s.whole);
        const uint32_t y_before = y_accumulator;
        y_accumulator += s.y_fraction;
        if (y_accumulator < y_before)
            position = wrap_add(position, s.y_carry);
        const uint32_t x_before = x_accumulator;
        x_accumulator += s.x_fraction;
        if (x_accumulator < x_before)
            position = wrap_add(position, s.x_carry);
    }
}

// Error-term walk used only when x grows, y shrinks, the y span is the
// longer and the count exceeds both spans: each axis steps when its error,
// starting at `count` and reduced by the span per sample, borrows.
void run_error_walk(
    uint8_t* out,
    const uint8_t* origin,
    int32_t position,
    int32_t pitch,
    uint32_t x_span,
    uint32_t y_span,
    uint32_t count
) noexcept {
    const uint32_t reload = count;
    uint32_t x_error = count;
    uint32_t y_error = count;
    while (true) {
        *out++ = origin[static_cast<std::ptrdiff_t>(position)];
        if (--count == 0)
            return;
        if (y_error < y_span) {
            position = wrap_add(position, -pitch);
            y_error = y_error - y_span + reload;
        } else {
            y_error -= y_span;
        }
        if (x_error < x_span) {
            position = wrap_add(position, 1);
            x_error = x_error - x_span + reload;
        } else {
            x_error -= x_span;
        }
    }
}

template <unsigned RowShift>
void sample_texture_row(
    uint8_t* out,
    const uint8_t* texture,
    uint32_t count,
    uint32_t u,
    uint32_t v,
    uint32_t du,
    uint32_t dv
) noexcept {
    // Row index bits of v land at bit 16 + (16 - RowShift) of the offset.
    constexpr uint32_t row_mask = ~0u << (fixed_shift + 16 - RowShift);
    for (; count != 0; --count) {
        const uint32_t offset = (((v << (16 - RowShift)) & row_mask) + u) >> fixed_shift;
        v += dv;
        u += du;
        *out++ = texture[offset];
    }
}

} // namespace

void sample_surface_line(
    uint8_t* out,
    const Surface& source,
    int32_t x0,
    int32_t y0,
    int32_t x1,
    int32_t y1,
    uint32_t count
) noexcept {
    if (count == 0)
        return;
    const int32_t pitch = source.pitch;
    const uint8_t* origin = source.pixels;
    const int32_t start = wrap_add(wrap_mul(y0, pitch), x0);
    const int32_t dx = static_cast<int32_t>(static_cast<uint32_t>(x1) - static_cast<uint32_t>(x0));
    const int32_t dy = static_cast<int32_t>(static_cast<uint32_t>(y1) - static_cast<uint32_t>(y0));
    const int32_t x_dir = dx > 0 ? 1 : dx < 0 ? -1 : 0;
    const int32_t y_dir = dy > 0 || dx == 0 ? 1 : dy < 0 ? -1 : 0;
    // Spans count the endpoints inclusively.
    const uint32_t x_span =
        (x_dir < 0 ? 0u - static_cast<uint32_t>(dx) : static_cast<uint32_t>(dx)) + 1;
    const uint32_t y_span =
        (dy < 0 ? 0u - static_cast<uint32_t>(dy) : static_cast<uint32_t>(dy)) + 1;
    const auto signed_count = static_cast<int32_t>(count);
    LineStepper s;
    if (dx == 0 || dy == 0) {
        // Axis-aligned: a single stepper along the moving axis; vertical
        // when dx is zero (including a single point).
        const bool vertical = dx == 0;
        const uint32_t span = vertical ? y_span : x_span;
        const int32_t unit = vertical ? (dy < 0 ? -pitch : pitch) : x_dir;
        Quotient q;
        if (count == span) {
            q = {1, 0};
        } else if (signed_count > static_cast<int32_t>(span)) {
            // Rightward runs divide 32-bit here; every other direction divides 16-bit.
            q = !vertical && x_dir > 0 ? Quotient{0, wide_quotient(span, count).fraction}
                                       : narrow_quotient(span, count);
        } else {
            q = wide_quotient(span, count);
        }
        s.whole = wrap_mul(static_cast<int32_t>(q.whole), unit);
        s.x_fraction = q.fraction;
        s.x_carry = unit;
        run_stepper(out, origin, start, s, count);
        return;
    }
    const int32_t y_unit = y_dir < 0 ? -pitch : pitch;
    const bool stretch =
        signed_count > static_cast<int32_t>(y_span) && signed_count > static_cast<int32_t>(x_span);
    if (stretch) {
        const bool x_major = static_cast<int32_t>(x_span - y_span) >= 0;
        if (x_dir > 0 && y_dir < 0 && !x_major) {
            run_error_walk(out, origin, start, pitch, x_span, y_span, count);
            return;
        }
        s.x_fraction = narrow_quotient(x_span, count).fraction;
        s.y_fraction = narrow_quotient(y_span, count).fraction;
    } else {
        const Quotient qx = wide_quotient(x_span, count);
        const Quotient qy = wide_quotient(y_span, count);
        s.whole = wrap_add(
            wrap_mul(static_cast<int32_t>(qx.whole), x_dir),
            wrap_mul(static_cast<int32_t>(qy.whole), y_unit)
        );
        s.x_fraction = qx.fraction;
        s.y_fraction = qy.fraction;
    }
    s.x_carry = x_dir;
    s.y_carry = y_unit;
    run_stepper(out, origin, start, s, count);
}

void sample_texture_row_w128(
    uint8_t* out,
    const uint8_t* texture,
    uint32_t count,
    uint32_t u,
    uint32_t v,
    uint32_t du,
    uint32_t dv
) noexcept {
    sample_texture_row<9>(out, texture, count, u, v, du, dv);
}

void sample_texture_row_w64(
    uint8_t* out,
    const uint8_t* texture,
    uint32_t count,
    uint32_t u,
    uint32_t v,
    uint32_t du,
    uint32_t dv
) noexcept {
    sample_texture_row<10>(out, texture, count, u, v, du, dv);
}

void sample_texture_row_w32(
    uint8_t* out,
    const uint8_t* texture,
    uint32_t count,
    uint32_t u,
    uint32_t v,
    uint32_t du,
    uint32_t dv
) noexcept {
    sample_texture_row<11>(out, texture, count, u, v, du, dv);
}

void sample_texture_row_w16(
    uint8_t* out,
    const uint8_t* texture,
    uint32_t count,
    uint32_t u,
    uint32_t v,
    uint32_t du,
    uint32_t dv
) noexcept {
    sample_texture_row<12>(out, texture, count, u, v, du, dv);
}

void composite_depth_sprite(
    const Sprite& source, Sprite& target, int32_t x, int32_t y, int32_t depth_bias
) noexcept {
    const int32_t left = wrap_add(target.origin_x - source.origin_x, x);
    const int32_t top = wrap_add(target.origin_y - source.origin_y, y);
    if (left < 0 || top < 0)
        return;
    // The columns and rows that land inside the target.
    const int32_t columns = std::min(
        static_cast<int32_t>(source.width), std::max(static_cast<int32_t>(target.width) - left, 0)
    );
    const int32_t rows = std::min(
        static_cast<int32_t>(source.height), std::max(static_cast<int32_t>(target.height) - top, 0)
    );
    for (int32_t r = 0; r < rows; ++r) {
        const std::size_t from = static_cast<std::size_t>(r) * source.width;
        const auto* pixel = static_cast<const uint8_t*>(source.data) + from;
        const auto* depth = static_cast<const uint8_t*>(source.aux) + from;
        const int32_t offset =
            wrap_add(wrap_mul(static_cast<int32_t>(target.width), wrap_add(top, r)), left);
        uint8_t* out = static_cast<uint8_t*>(target.data) + offset;
        uint8_t* out_depth = static_cast<uint8_t*>(target.aux) + offset;
        for (int32_t i = 0; i < columns; ++i) {
            const int32_t biased = wrap_add(depth[i], depth_bias);
            if (pixel[i] != source.key && static_cast<int32_t>(out_depth[i]) <= biased) {
                out[i] = pixel[i];
                out_depth[i] = static_cast<uint8_t>(biased);
            }
        }
    }
}

void tint_sprite_below_depth(Sprite& sprite, uint8_t threshold) noexcept {
    const DisplayContext* display = display_context();
    if (display == nullptr || display->blue_table == nullptr)
        return;
    auto* pixel = static_cast<uint8_t*>(sprite.data);
    const auto* depth = static_cast<const uint8_t*>(sprite.aux);
    const uint32_t total = uint32_t{sprite.height} * sprite.width;
    for (uint32_t i = 0; i < total; ++i)
        if (depth[i] <= threshold && pixel[i] != sprite.key)
            pixel[i] = display->blue_table[pixel[i]];
}

void clear_sprite_below_depth(Sprite& sprite, uint8_t threshold) noexcept {
    auto* pixel = static_cast<uint8_t*>(sprite.data);
    const auto* depth = static_cast<const uint8_t*>(sprite.aux);
    const uint32_t total = uint32_t{sprite.height} * sprite.width;
    for (uint32_t i = 0; i < total; ++i)
        if (depth[i] <= threshold)
            pixel[i] = sprite.key;
}

void step_all_cursors(void* const* cursors, int32_t count, CursorStep step) noexcept {
    for (int32_t i = count - 1; i >= 0; --i)
        step(cursors[i]);
}

} // namespace oa::present
