// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/present/display.hpp"
#include "oa/present/span_sample.hpp"
#include "oa/present/surface.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

int failures = 0;

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x);             \
            ++failures;                                                                            \
        }                                                                                          \
    } while (false)

using namespace oa::present;
using oa::Sprite;

// 16x16 surface whose texel at (x, y) is y * 16 + x.
SurfaceBuffer ramp_surface() {
    auto b = create_surface(16, 16);
    for (int i = 0; i < 256; ++i)
        b.pixels[static_cast<std::size_t>(i)] = static_cast<uint8_t>(i);
    return b;
}

uint8_t texel(int x, int y) {
    return static_cast<uint8_t>(y * 16 + x);
}

void test_line_straight() {
    const auto b = ramp_surface();
    std::array<uint8_t, 16> out{};
    sample_surface_line(out.data(), b.surface, 2, 3, 9, 3, 8);
    for (int i = 0; i < 8; ++i)
        CHECK(out[static_cast<std::size_t>(i)] == texel(2 + i, 3));
    sample_surface_line(out.data(), b.surface, 9, 3, 2, 3, 8);
    for (int i = 0; i < 8; ++i)
        CHECK(out[static_cast<std::size_t>(i)] == texel(9 - i, 3));
    sample_surface_line(out.data(), b.surface, 4, 1, 4, 6, 6);
    for (int i = 0; i < 6; ++i)
        CHECK(out[static_cast<std::size_t>(i)] == texel(4, 1 + i));
    sample_surface_line(out.data(), b.surface, 4, 6, 4, 1, 6);
    for (int i = 0; i < 6; ++i)
        CHECK(out[static_cast<std::size_t>(i)] == texel(4, 6 - i));
    sample_surface_line(out.data(), b.surface, 1, 1, 5, 5, 5);
    for (int i = 0; i < 5; ++i)
        CHECK(out[static_cast<std::size_t>(i)] == texel(1 + i, 1 + i));
}

void test_line_stretch_and_shrink() {
    const auto b = ramp_surface();
    std::array<uint8_t, 16> out{};
    // Twice as many samples as texels: each texel repeats.
    sample_surface_line(out.data(), b.surface, 0, 2, 3, 2, 8);
    const uint8_t doubled[] = {
        texel(0, 2),
        texel(0, 2),
        texel(1, 2),
        texel(1, 2),
        texel(2, 2),
        texel(2, 2),
        texel(3, 2),
        texel(3, 2)
    };
    for (int i = 0; i < 8; ++i)
        CHECK(out[static_cast<std::size_t>(i)] == doubled[i]);
    // Half as many samples: every other texel.
    sample_surface_line(out.data(), b.surface, 0, 5, 0, 12, 4);
    for (int i = 0; i < 4; ++i)
        CHECK(out[static_cast<std::size_t>(i)] == texel(0, 5 + 2 * i));
    // A single point stretched stays on that texel.
    sample_surface_line(out.data(), b.surface, 7, 7, 7, 7, 5);
    for (int i = 0; i < 5; ++i)
        CHECK(out[static_cast<std::size_t>(i)] == texel(7, 7));
    // Diagonal stretch walks both axes and ends on the far endpoint's row.
    sample_surface_line(out.data(), b.surface, 2, 9, 4, 5, 10);
    CHECK(out[0] == texel(2, 9));
    CHECK(out[9] == texel(4, 5));
    out.fill(0xEE);
    sample_surface_line(out.data(), b.surface, 0, 0, 3, 0, 0);
    CHECK(out[0] == 0xEE);
}

void test_texture_rows() {
    std::vector<uint8_t> texture(0x10000);
    for (std::size_t i = 0; i < texture.size(); ++i)
        texture[i] = static_cast<uint8_t>(i * 7 + (i >> 8));
    std::array<uint8_t, 4> out{};
    const uint32_t u = 5u << 16;
    const uint32_t v = 3u << 16;
    sample_texture_row_w128(out.data(), texture.data(), 3, u, v, 1u << 16, 1u << 16);
    CHECK(out[0] == texture[3 * 128 + 5]);
    CHECK(out[1] == texture[4 * 128 + 6]);
    CHECK(out[2] == texture[5 * 128 + 7]);
    sample_texture_row_w64(out.data(), texture.data(), 1, u, v, 0, 0);
    CHECK(out[0] == texture[3 * 64 + 5]);
    sample_texture_row_w32(out.data(), texture.data(), 1, u, v, 0, 0);
    CHECK(out[0] == texture[3 * 32 + 5]);
    sample_texture_row_w16(out.data(), texture.data(), 1, u, v, 0, 0);
    CHECK(out[0] == texture[3 * 16 + 5]);
    // Row bits above the texture height wrap.
    sample_texture_row_w128(out.data(), texture.data(), 1, 0, (512u + 2u) << 16, 0, 0);
    CHECK(out[0] == texture[2 * 128]);
    out.fill(0xEE);
    sample_texture_row_w16(out.data(), texture.data(), 0, u, v, 0, 0);
    CHECK(out[0] == 0xEE);
}

struct DepthSprite {
    Sprite sprite{};
    std::vector<uint8_t> pixels;
    std::vector<uint8_t> depth;
};

DepthSprite depth_sprite(int w, int h, uint8_t pixel, uint8_t depth) {
    DepthSprite s;
    s.pixels.assign(static_cast<std::size_t>(w * h), pixel);
    s.depth.assign(s.pixels.size(), depth);
    s.sprite.width = static_cast<uint16_t>(w);
    s.sprite.height = static_cast<uint16_t>(h);
    s.sprite.data = s.pixels.data();
    s.sprite.aux = s.depth.data();
    return s;
}

void test_composite() {
    auto source = depth_sprite(2, 2, 7, 10);
    source.pixels[1] = 0; // key
    source.sprite.key = 0;
    source.depth[2] = 1;
    auto target = depth_sprite(6, 4, 3, 12);
    composite_depth_sprite(source.sprite, target.sprite, 1, 1, 2);
    // Source depth 10 + 2 reaches the stored 12 except where it is 1 + 2.
    CHECK(target.pixels[1 * 6 + 1] == 7);
    CHECK(target.depth[1 * 6 + 1] == 12);
    CHECK(target.pixels[1 * 6 + 2] == 3); // key
    CHECK(target.pixels[2 * 6 + 1] == 3); // too deep
    CHECK(target.pixels[2 * 6 + 2] == 7);
    auto untouched = depth_sprite(6, 4, 3, 0);
    composite_depth_sprite(source.sprite, untouched.sprite, -1, 0, 0);
    for (auto p : untouched.pixels)
        CHECK(p == 3);
    // Hotspots shift the placement.
    source.sprite.origin_x = 1;
    auto shifted = depth_sprite(6, 4, 3, 0);
    composite_depth_sprite(source.sprite, shifted.sprite, 3, 0, 0);
    CHECK(shifted.pixels[2] == 7);
    CHECK(shifted.pixels[1] == 3);
}

// A source reaching past the target's right and bottom edges draws only
// the part inside it: none past its rows, none wrapped into the next row.
void test_composite_past_the_target() {
    constexpr uint8_t room_fill = 0xEE;
    constexpr std::size_t size = 6 * 4;
    constexpr std::size_t room = 64;
    const auto source = depth_sprite(4, 4, 7, 10);
    auto target = depth_sprite(6, 4, 3, 0);
    target.pixels.resize(size + room, room_fill);
    target.depth.resize(size + room, 0);
    target.sprite.data = target.pixels.data();
    target.sprite.aux = target.depth.data();
    composite_depth_sprite(source.sprite, target.sprite, 4, 2, 0);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 6; ++x) {
            const bool inside = x >= 4 && y >= 2;
            const auto at = static_cast<std::size_t>(y * 6 + x);
            CHECK(target.pixels[at] == (inside ? 7 : 3));
            CHECK(target.depth[at] == (inside ? 10 : 0));
        }
    for (std::size_t i = size; i < size + room; ++i)
        CHECK(target.pixels[i] == room_fill && target.depth[i] == 0);
}

void test_depth_masks() {
    std::array<uint8_t, 256> blue{};
    for (int i = 0; i < 256; ++i)
        blue[static_cast<std::size_t>(i)] = static_cast<uint8_t>(255 - i);
    DisplayContext display{};
    display.blue_table = blue.data();
    bind_display(&display);
    auto s = depth_sprite(4, 1, 10, 0);
    s.sprite.key = 9;
    s.pixels[1] = 9;
    const uint8_t depths[] = {5, 5, 6, 7};
    for (std::size_t i = 0; i < 4; ++i)
        s.depth[i] = depths[i];
    tint_sprite_below_depth(s.sprite, 6);
    CHECK(s.pixels[0] == 245);
    CHECK(s.pixels[1] == 9);
    CHECK(s.pixels[2] == 245);
    CHECK(s.pixels[3] == 10);
    bind_display(nullptr);

    clear_sprite_below_depth(s.sprite, 5);
    CHECK(s.pixels[0] == 9);
    CHECK(s.pixels[2] == 245);
}

std::array<int, 4> visit_order{};
int visits = 0;

int32_t record_visit(void* cursor) {
    visit_order[static_cast<std::size_t>(visits++)] = *static_cast<int*>(cursor);
    return 0;
}

void test_step_all_cursors() {
    int a = 1, b = 2, c = 3;
    void* cursors[] = {&a, &b, &c};
    step_all_cursors(cursors, 3, record_visit);
    CHECK(visits == 3);
    CHECK(visit_order[0] == 3 && visit_order[1] == 2 && visit_order[2] == 1);
    step_all_cursors(cursors, 0, record_visit);
    CHECK(visits == 3);
}

} // namespace

int main() {
    test_line_straight();
    test_line_stretch_and_shrink();
    test_texture_rows();
    test_composite();
    test_composite_past_the_target();
    test_depth_masks();
    test_step_all_cursors();
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    std::puts("span sample tests passed");
    return EXIT_SUCCESS;
}
