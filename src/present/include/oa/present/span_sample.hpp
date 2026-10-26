// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// Texel samplers and depth-plane sprite compositing. The row samplers read a
// 64 KiB texture addressed by 16.16 (u, v) coordinates whose row stride is
// fixed per sampler; the sprite compositors treat Sprite::aux as an 8-bit
// depth plane of the same size as the pixels.

#include "oa/present/surface.h"

#include <cstdint>

namespace oa::present {

/// Resamples texels along an inclusive source segment, stretching or skipping with 16.16 steppers.
///
/// Axis-aligned segments use one stepper; sloped ones step both axes, and a
/// segment whose x grows while y shrinks, with y the longer span and more
/// samples than either span, walks with error terms instead. A count of
/// 0x10000 or more, whose 16-bit quotient would overflow, uses the 32-bit
/// quotient.
///
/// @param[out] out `count` output bytes
/// @param source surface to sample; the segment is not clipped
/// @param x0 start column
/// @param y0 start row
/// @param x1 end column
/// @param y1 end row
/// @param count number of samples; 0 writes nothing
/// @quirk Stretched rightward runs divide in 32 bits and every other direction
///     in 16 bits, as the game does, so their rounding differs.
void sample_surface_line(
    uint8_t* out,
    const Surface& source,
    int32_t x0,
    int32_t y0,
    int32_t x1,
    int32_t y1,
    uint32_t count
) noexcept;

/// Writes texels from a 128x512 texture along a 16.16 (u, v) walk.
///
/// Each texel is read at row v >> 16 (masked to the texture height) and
/// column u >> 16, then (u, v) steps by (du, dv).
///
/// @param[out] out `count` output bytes
/// @param texture 64 KiB texture, 128 bytes per row
/// @param count number of texels
/// @param u start column in 16.16
/// @param v start row in 16.16
/// @param du column step per texel in 16.16
/// @param dv row step per texel in 16.16
void sample_texture_row_w128(
    uint8_t* out,
    const uint8_t* texture,
    uint32_t count,
    uint32_t u,
    uint32_t v,
    uint32_t du,
    uint32_t dv
) noexcept;

/// Writes texels from a 64x1024 texture along a 16.16 (u, v) walk.
///
/// Each texel is read at row v >> 16 (masked to the texture height) and
/// column u >> 16, then (u, v) steps by (du, dv).
///
/// @param[out] out `count` output bytes
/// @param texture 64 KiB texture, 64 bytes per row
/// @param count number of texels
/// @param u start column in 16.16
/// @param v start row in 16.16
/// @param du column step per texel in 16.16
/// @param dv row step per texel in 16.16
void sample_texture_row_w64(
    uint8_t* out,
    const uint8_t* texture,
    uint32_t count,
    uint32_t u,
    uint32_t v,
    uint32_t du,
    uint32_t dv
) noexcept;

/// Writes texels from a 32x2048 texture along a 16.16 (u, v) walk.
///
/// Each texel is read at row v >> 16 (masked to the texture height) and
/// column u >> 16, then (u, v) steps by (du, dv).
///
/// @param[out] out `count` output bytes
/// @param texture 64 KiB texture, 32 bytes per row
/// @param count number of texels
/// @param u start column in 16.16
/// @param v start row in 16.16
/// @param du column step per texel in 16.16
/// @param dv row step per texel in 16.16
void sample_texture_row_w32(
    uint8_t* out,
    const uint8_t* texture,
    uint32_t count,
    uint32_t u,
    uint32_t v,
    uint32_t du,
    uint32_t dv
) noexcept;

/// Writes texels from a 16x4096 texture along a 16.16 (u, v) walk.
///
/// Each texel is read at row v >> 16 (masked to the texture height) and
/// column u >> 16, then (u, v) steps by (du, dv).
///
/// @param[out] out `count` output bytes
/// @param texture 64 KiB texture, 16 bytes per row
/// @param count number of texels
/// @param u start column in 16.16
/// @param v start row in 16.16
/// @param du column step per texel in 16.16
/// @param dv row step per texel in 16.16
void sample_texture_row_w16(
    uint8_t* out,
    const uint8_t* texture,
    uint32_t count,
    uint32_t u,
    uint32_t v,
    uint32_t du,
    uint32_t dv
) noexcept;

/// Depth-composites the non-key pixels of one depth-plane sprite into another.
///
/// The source's top-left lands on target column target.origin_x -
/// source.origin_x + x and row target.origin_y - source.origin_y + y. A
/// pixel is written, with its biased depth, where the target depth is not
/// above the source depth plus `depth_bias`. The source's part past the
/// target's right or bottom edge is left out: a composite cut to a size
/// smaller than the units it carries (limits.model-composite
/// clamp-oversize) keeps them where the cut leaves them.
///
/// @param source sprite with pixels and a depth plane in aux
/// @param[in,out] target sprite with pixels and a depth plane in aux
/// @param x column offset
/// @param y row offset
/// @param depth_bias added to every source depth
/// @quirk Nothing is drawn when the placement starts left of or above the
///     target.
void composite_depth_sprite(
    const Sprite& source, Sprite& target, int32_t x, int32_t y, int32_t depth_bias
) noexcept;

/// Remaps every non-key pixel whose depth is at or below a threshold through the display blue table.
///
/// @param[in,out] sprite sprite with pixels and a depth plane in aux; nothing
///     changes without a display blue table
/// @param threshold highest depth tinted
void tint_sprite_below_depth(Sprite& sprite, uint8_t threshold) noexcept;

/// Sets every pixel whose depth is at or below a threshold to the sprite key.
///
/// @param[in,out] sprite sprite with pixels and a depth plane in aux
/// @param threshold highest depth cleared
void clear_sprite_below_depth(Sprite& sprite, uint8_t threshold) noexcept;

// Advances one animation cursor; the result is ignored by the caller below.
using CursorStep = int32_t (*)(void* cursor);

/// Steps every registered animation cursor, last registered first.
///
/// @param cursors registered cursors
/// @param count number of cursors; non-positive steps none
/// @param step function that advances one cursor; its result is ignored
void step_all_cursors(void* const* cursors, int32_t count, CursorStep step) noexcept;

} // namespace oa::present
