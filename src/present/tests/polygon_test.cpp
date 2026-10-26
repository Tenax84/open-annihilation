// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/present/display.hpp"
#include "oa/present/polygon.hpp"
#include "oa/present/surface.hpp"

#include <algorithm>
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
using oa::Surface;

constexpr uint8_t ink = 0x5A;

int count_value(const SurfaceBuffer& b, uint8_t value) {
    int n = 0;
    for (auto p : b.pixels)
        n += p == value ? 1 : 0;
    return n;
}

uint8_t at(const SurfaceBuffer& b, int x, int y) {
    return b.pixels[static_cast<std::size_t>(y * b.surface.pitch + x)];
}

struct SpriteBuffer2 {
    Sprite sprite{};
    std::vector<uint8_t> pixels;
    std::vector<uint8_t> depth;
};

SpriteBuffer2 make_sprite(int w, int h, bool with_depth, uint8_t depth_fill) {
    SpriteBuffer2 s;
    s.pixels.assign(static_cast<std::size_t>(w * h), 0);
    s.sprite.width = static_cast<uint16_t>(w);
    s.sprite.height = static_cast<uint16_t>(h);
    s.sprite.data = s.pixels.data();
    if (with_depth) {
        s.depth.assign(s.pixels.size(), depth_fill);
        s.sprite.aux = s.depth.data();
    }
    return s;
}

void test_fill_square() {
    auto b = create_surface(16, 12);
    const PolygonVertex square[] = {{2, 1}, {8, 1}, {8, 6}, {2, 6}};
    CHECK(fill_polygon(&b.surface, square, 4, ink) == 1);
    // Rows 1..5 and columns 2..7: bottom row and right column stay empty.
    CHECK(count_value(b, ink) == 6 * 5);
    CHECK(at(b, 2, 1) == ink);
    CHECK(at(b, 7, 5) == ink);
    CHECK(at(b, 8, 3) == 0);
    CHECK(at(b, 4, 6) == 0);
}

void test_fill_orientation_and_clip() {
    auto b = create_surface(16, 12);
    // The opposite winding puts the edges on the wrong sides: nothing fills.
    const PolygonVertex reversed[] = {{2, 1}, {2, 6}, {8, 6}, {8, 1}};
    CHECK(fill_polygon(&b.surface, reversed, 4, ink) == 1);
    CHECK(count_value(b, ink) == 0);

    set_surface_clip(b.surface, oa::Rect32{4, 3, 10, 8});
    const PolygonVertex big[] = {{-5, -5}, {30, -5}, {30, 30}, {-5, 30}};
    CHECK(fill_polygon(&b.surface, big, 4, ink) == 1);
    CHECK(count_value(b, ink) == 6 * 5);
    CHECK(at(b, 4, 3) == ink);
    CHECK(at(b, 10, 5) == 0);

    const PolygonVertex away[] = {{20, 20}, {25, 20}, {25, 25}};
    CHECK(fill_polygon(&b.surface, away, 3, ink) == 0);
}

void test_fill_locked_display() {
    auto b = create_surface(8, 8);
    CHECK(fill_polygon(nullptr, nullptr, 0, ink) == 0); // no display bound
    DisplayContext display{};
    display.back_buffer = b.surface;
    bind_display(&display);
    const PolygonVertex tri[] = {{0, 0}, {6, 0}, {0, 6}};
    CHECK(fill_polygon(nullptr, tri, 3, ink) == 1);
    CHECK(at(b, 0, 0) == ink);
    CHECK(at(b, 5, 0) == ink);
    CHECK(at(b, 0, 5) == ink);
    CHECK(at(b, 6, 6) == 0);
    bind_display(nullptr);
}

void test_range_rings() {
    auto b = create_surface(64, 64);
    draw_range_ring(&b.surface, 32, 32, 20, ink);
    CHECK(at(b, 52, 32) == ink);
    CHECK(at(b, 32, 12) == ink);
    CHECK(at(b, 12, 32) == ink);
    CHECK(at(b, 32, 32) == 0);
    const int full = count_value(b, ink);
    CHECK(full > 100);

    auto even = create_surface(64, 64);
    auto odd = create_surface(64, 64);
    draw_dashed_range_ring(&even.surface, 32, 32, 20, ink, 16, 0);
    draw_dashed_range_ring(&odd.surface, 32, 32, 20, ink, 16, 1);
    const int a = count_value(even, ink);
    const int c = count_value(odd, ink);
    CHECK(a > 0 && c > 0 && a < full && c < full);
    // An odd phase draws the first chord, which leaves (52, 32) towards +y.
    CHECK(at(odd, 52, 33) == ink || at(odd, 51, 33) == ink);
    CHECK(at(even, 52, 33) == 0 && at(even, 51, 33) == 0);
    auto none = create_surface(8, 8);
    draw_dashed_range_ring(&none.surface, 4, 4, 3, ink, 0, 1);
    CHECK(count_value(none, ink) == 0);
}

void test_depth_polygon() {
    const DepthVertex tri[] = {{2, 1, 0}, {12, 1, 0}, {2, 9, 0}};
    auto hidden = make_sprite(16, 12, true, 255);
    CHECK(fill_depth_polygon(hidden.sprite, tri, 3, ink) == 1);
    for (auto p : hidden.pixels)
        CHECK(p == 0);

    const DepthVertex ramp[] = {{2, 1, 10}, {12, 1, 10}, {2, 9, 10}};
    auto open = make_sprite(16, 12, true, 0);
    CHECK(fill_depth_polygon(open.sprite, ramp, 3, ink) == 1);
    CHECK(open.pixels[1 * 16 + 2] == ink);
    CHECK(open.depth[1 * 16 + 2] == 10);
    CHECK(open.pixels[0] == 0);

    auto plain = make_sprite(16, 12, false, 0);
    CHECK(fill_depth_polygon(plain.sprite, tri, 3, ink) == 1);
    CHECK(plain.pixels[2 * 16 + 3] == ink);
    const DepthVertex above[] = {{0, -9, 0}, {5, -9, 0}, {0, -2, 0}};
    CHECK(fill_depth_polygon(plain.sprite, above, 3, ink) == 0);
}

void test_outline_polygon() {
    const DepthVertex square[] = {{2, 2, 0}, {10, 2, 0}, {10, 8, 0}, {2, 8, 0}};
    auto s = make_sprite(16, 12, false, 0);
    CHECK(outline_depth_polygon(s.sprite, square, 4, ink) == 1);
    CHECK(s.pixels[3 * 16 + 2] == ink);
    CHECK(s.pixels[3 * 16 + 10] == ink);
    CHECK(s.pixels[3 * 16 + 6] == 0);
    const DepthVertex flat[] = {{1, 3, 0}, {9, 3, 0}};
    CHECK(outline_depth_polygon(s.sprite, flat, 2, ink) == 0);
    CHECK(outline_depth_polygon(s.sprite, flat, 0, ink) == 0);
}

// A sprite whose planes lie in buffers with room past their ends, as a
// composite cut to its limits lies in a larger buffer: the room must keep
// what it held.
struct RoomySprite {
    Sprite sprite{};
    std::vector<uint8_t> pixels;
    std::vector<uint8_t> depth;
    std::size_t size{};
};

constexpr uint8_t room_fill = 0xEE;

RoomySprite roomy_sprite(int w, int h, std::size_t room, uint8_t depth_fill) {
    RoomySprite s;
    s.size = static_cast<std::size_t>(w * h);
    s.pixels.assign(s.size + room, room_fill);
    std::fill_n(s.pixels.begin(), s.size, uint8_t{0});
    s.depth.assign(s.size + room, 0);
    std::fill_n(s.depth.begin(), s.size, depth_fill);
    s.sprite.width = static_cast<uint16_t>(w);
    s.sprite.height = static_cast<uint16_t>(h);
    s.sprite.data = s.pixels.data();
    s.sprite.aux = s.depth.data();
    return s;
}

bool room_kept(const RoomySprite& s) {
    for (std::size_t i = s.size; i < s.pixels.size(); ++i)
        if (s.pixels[i] != room_fill || s.depth[i] != 0)
            return false;
    return true;
}

// An outline reaching past the sprite's right and bottom edges plots only
// the ends inside it: none past its rows, none wrapped into the next row.
void test_outline_past_the_sprite() {
    auto s = roomy_sprite(8, 6, 256, 0x10);
    const DepthVertex square[] = {{2, 1, 0x20}, {14, 1, 0x20}, {14, 9, 0x20}, {2, 9, 0x20}};
    CHECK(outline_depth_polygon(s.sprite, square, 4, ink) == 1);
    for (int y = 0; y < 6; ++y)
        for (int x = 0; x < 8; ++x) {
            const bool end = x == 2 && y >= 1;
            const auto at = static_cast<std::size_t>(y * 8 + x);
            CHECK(s.pixels[at] == (end ? ink : 0));
            CHECK(s.depth[at] == (end ? 0x20 : 0x10));
        }
    CHECK(room_kept(s));
}

void test_depth_span_clip() {
    auto s = make_sprite(8, 2, true, 0);
    PolygonSpanRow row{};
    row.left = -2;
    row.right = 20;
    row.left_depth = 0;
    row.right_depth = 22 << 16;
    fill_depth_span(1, row, s.sprite, ink);
    CHECK(row.left == 0);
    CHECK(row.right == 7);
    CHECK(row.left_depth == 2 << 16);
    for (int x = 0; x < 7; ++x) {
        CHECK(s.pixels[8 + x] == ink);
        CHECK(s.depth[8 + x] == 2 + x);
    }
    CHECK(s.pixels[8 + 7] == 0);
}

void test_shaded_polygon() {
    std::vector<uint8_t> shade(4 * 256);
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 256; ++c)
            shade[static_cast<std::size_t>(r * 256 + c)] = static_cast<uint8_t>(r * 64 + (c & 63));
    DisplayContext display{};
    display.shade_table = shade.data();
    bind_display(&display);
    const ShadedVertex quad[] = {{0, 0, 0, 0}, {16, 0, 0, 3}, {16, 8, 0, 3}, {0, 8, 0, 0}};
    auto s = make_sprite(20, 10, false, 0);
    CHECK(fill_shaded_polygon(s.sprite, quad, 4, 5) == 1);
    CHECK(s.pixels[0] == 5);
    CHECK(s.pixels[15] == 2 * 64 + 5);
    PolygonSpanRow row{};
    row.left = 0;
    row.right = 4;
    row.left_shade = 1 << 16;
    row.right_shade = 1 << 16;
    auto d = make_sprite(8, 1, true, 9);
    row.left_depth = 8 << 16;
    row.right_depth = 12 << 16;
    fill_shaded_span(0, row, d.sprite, 1);
    CHECK(d.pixels[0] == 0);
    CHECK(d.pixels[1] == 65);
    CHECK(d.depth[1] == 9);
    CHECK(d.depth[3] == 11);
    bind_display(nullptr);
}

// A polygon draws alike before and after a taller one, which leaves its
// span table rows past the shorter one's height as it wrote them: the rows
// a polygon reads are its own, the notched one's though it is not convex.
void test_span_rows_after_a_taller_polygon() {
    const DepthVertex notched[] = {{2, 2, 0}, {12, 2, 0}, {12, 10, 0}, {7, 5, 0}, {2, 10, 0}};
    const DepthVertex tall[] = {{0, 0, 0}, {15, 0, 0}, {15, 40, 0}, {0, 40, 0}};
    for (const auto draw : {&fill_depth_polygon, &outline_depth_polygon}) {
        auto first = make_sprite(16, 12, false, 0);
        CHECK(draw(first.sprite, notched, 5, ink) == 1);
        auto taller = make_sprite(16, 48, false, 0);
        CHECK(draw(taller.sprite, tall, 4, ink) == 1);
        auto again = make_sprite(16, 12, false, 0);
        CHECK(draw(again.sprite, notched, 5, ink) == 1);
        CHECK(again.pixels == first.pixels);
        CHECK(taller.pixels[39 * 16] == ink);
    }
}

} // namespace

int main() {
    test_fill_square();
    test_fill_orientation_and_clip();
    test_fill_locked_display();
    test_range_rings();
    test_depth_polygon();
    test_outline_polygon();
    test_outline_past_the_sprite();
    test_depth_span_clip();
    test_shaded_polygon();
    test_span_rows_after_a_taller_polygon();
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    std::puts("polygon tests passed");
    return EXIT_SUCCESS;
}
