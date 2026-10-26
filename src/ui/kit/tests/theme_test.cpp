// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Every colour token equals the value it was copied from, or the value the
// design gives a new one, written here as hex, and Compact metrics equal the
// settings dialog's, its notices' and its prompts' sizes and the sizes the
// design gives the new controls. Regular and Large metrics hold the design's
// table of sizes, differ from Compact's only there, and each class's dialog
// and notices fit the least window of their class.

#include "oa/test/check.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/theme.hpp"

#include <array>
#include <stdint.h>

namespace {

namespace kit = oa::ui::kit;

/// Checks one token against a colour written out in the test.
///
/// @param token the token
/// @param red red, 0 to 255
/// @param green green, 0 to 255
/// @param blue blue, 0 to 255
/// @param opacity opacity, 0 to 255
void equals(kit::Colour token, uint8_t red, uint8_t green, uint8_t blue, uint8_t opacity = 255) {
    OA_CHECK(token == (kit::Colour{red, green, blue, opacity}));
}

/// Checks the settings dialog's colours, the Game files colours and the touch
/// controls' colours, each against its own literal.
void every_token_keeps_its_value() {
    equals(kit::colour::panel, 0x1b, 0x1e, 0x19);
    equals(kit::colour::band, 0x14, 0x16, 0x12);
    equals(kit::colour::list, 0x17, 0x1a, 0x15);
    equals(kit::colour::list_selected, 0x26, 0x2b, 0x21);
    equals(kit::colour::hover, 0x20, 0x24, 0x1c);
    equals(kit::colour::rule, 0x2b, 0x30, 0x27);
    equals(kit::colour::edge_light, 0x4a, 0x51, 0x43);
    equals(kit::colour::edge_dark, 0x08, 0x09, 0x07);
    equals(kit::colour::text, 0xe7, 0xe8, 0xdf);
    equals(kit::colour::hint, 0x9a, 0xa1, 0x90);
    equals(kit::colour::quiet, 0x8a, 0x91, 0x80);
    equals(kit::colour::list_text, 0xa3, 0xaa, 0x98);
    equals(kit::colour::accent, 0x9c, 0xcc, 0x3c);
    equals(kit::colour::accent_light, 0xb6, 0xe0, 0x5a);
    equals(kit::colour::accent_held, 0x8a, 0xb8, 0x30);
    equals(kit::colour::on_accent, 0x10, 0x12, 0x0d);
    equals(kit::colour::filled, 0x3e, 0x52, 0x18);
    equals(kit::colour::filled_light, 0x49, 0x60, 0x1c);
    equals(kit::colour::filled_held, 0x35, 0x45, 0x14);
    equals(kit::colour::well, 0x12, 0x14, 0x10);
    equals(kit::colour::control_border, 0x3a, 0x40, 0x34);
    equals(kit::colour::control_hover, 0x5b, 0x63, 0x52);
    equals(kit::colour::off_selected, 0x2c, 0x32, 0x26);
    equals(kit::colour::switch_idle, 0x7d, 0x84, 0x74);
    equals(kit::colour::button_text, 0xc9, 0xcd, 0xbf);
    equals(kit::colour::lock, 0xe0, 0xb0, 0x4f);
    equals(kit::colour::backdrop, 5, 6, 4);
    equals(kit::colour::danger, 0xe0, 0x6c, 0x5c);
    equals(kit::colour::online, 0x6e, 0xa8, 0xd6);

    equals(kit::screen_colour::background, 0x1b, 0x1e, 0x19);
    equals(kit::screen_colour::panel, 0x23, 0x27, 0x21);
    equals(kit::screen_colour::line, 0x3a, 0x40, 0x34);
    equals(kit::screen_colour::text, 0xe7, 0xe8, 0xdf);
    equals(kit::screen_colour::dim, 0x9a, 0xa0, 0x94);
    equals(kit::screen_colour::green, 0x9c, 0xcc, 0x3c);
    equals(kit::screen_colour::amber, 0xe0, 0xb0, 0x4f);
    equals(kit::screen_colour::red, 0xe0, 0x6c, 0x5c);
    equals(kit::screen_colour::ink, 0x10, 0x12, 0x0d);
    equals(kit::screen_colour::button_text, 0xc9, 0xcd, 0xbf);
    // The screen's text, outlines and warnings are the dialog's text, control border and
    // lock, the near-duplicates (e8eae4, 3a4037 and e8b44c) they were before.
    OA_CHECK(kit::screen_colour::text == kit::colour::text);
    OA_CHECK(kit::screen_colour::line == kit::colour::control_border);
    OA_CHECK(kit::screen_colour::amber == kit::colour::lock);

    equals(kit::hud_colour::panel, 0x1b, 0x1e, 0x19, 217);
    equals(kit::hud_colour::sheet, 0x14, 0x16, 0x12, 248);
    equals(kit::hud_colour::cell, 0x24, 0x28, 0x20, 230);
    equals(kit::hud_colour::pressed, 0x3b, 0x42, 0x35, 240);
    equals(kit::hud_colour::edge, 0xff, 0xff, 0xff, 34);
    equals(kit::hud_colour::lit, 0x9c, 0xcc, 0x3c);
    equals(kit::hud_colour::lit_pressed, 0xb8, 0xde, 0x66);
    equals(kit::hud_colour::ink, 0x1b, 0x1e, 0x19);
    equals(kit::hud_colour::label, 0xf2, 0xf4, 0xee);
    equals(kit::hud_colour::quiet, 0xb4, 0xb9, 0xae);
    equals(kit::hud_colour::danger, 0xc0, 0x39, 0x2b);
    equals(kit::hud_colour::danger_panel, 0x2a, 0x15, 0x12, 235);
    equals(kit::hud_colour::danger_label, 0xf0, 0x8c, 0x80);
    equals(kit::hud_colour::wedge_gap, 0x08, 0x09, 0x07);
    OA_CHECK(kit::hud_colour::greyed_opacity == 0.4F);

    OA_CHECK(kit::menu_backdrop_opacity == 159);
    OA_CHECK(kit::spotlight_opacity == 189);
    OA_CHECK(kit::ingame_backdrop_opacity == 128);
    OA_CHECK(kit::locked_fade == 115);
    OA_CHECK(kit::selected_tint == 15);

    constexpr kit::Colour with_opacity{1, 2, 3, 4};
    OA_CHECK(kit::rgb(with_opacity) == (std::array<uint8_t, 3>{1, 2, 3}));
}

/// Checks Compact metrics against the settings dialog's, its notices' and its
/// prompts' sizes.
void compact_metrics_match_the_dialog() {
    OA_CHECK(kit::compact_metrics.edge == 1);
    OA_CHECK(kit::compact_metrics.header_height == 26);
    OA_CHECK(kit::compact_metrics.footer_height == 32);
    OA_CHECK(kit::compact_metrics.padding == 12);
    OA_CHECK(kit::compact_metrics.mark_top == 4);
    OA_CHECK(kit::compact_metrics.mark_side == 20);
    OA_CHECK(kit::compact_metrics.mark_square == 13);
    OA_CHECK(kit::compact_metrics.header_gap == 6);
    OA_CHECK(kit::compact_metrics.heading_tracking == 1);
    OA_CHECK(kit::compact_metrics.button_height == 17);
    OA_CHECK(kit::compact_metrics.button_width == 52);
    OA_CHECK(kit::compact_metrics.button_gap == 5);
    OA_CHECK(kit::compact_metrics.focus_inset == 2);
    OA_CHECK(kit::compact_metrics.small_line == 12);
    OA_CHECK(kit::compact_metrics.regular_line == 16);
    OA_CHECK(kit::compact_metrics.switch_width == 52);
    OA_CHECK(kit::compact_metrics.knob_width == 7);
    OA_CHECK(kit::compact_metrics.knob_height == 12);
    OA_CHECK(kit::compact_metrics.track_height == 4);
    OA_CHECK(kit::compact_metrics.track_offset == 4);
    OA_CHECK(kit::compact_metrics.stop_height == 2);
    OA_CHECK(kit::compact_metrics.stop_offset == 12);
    OA_CHECK(kit::compact_metrics.least_stop_spacing == 4);
    OA_CHECK(kit::compact_metrics.choice_text_inset == 6);
    OA_CHECK(kit::compact_metrics.choice_arrow_room == 16);
    OA_CHECK(kit::compact_metrics.choice_arrow_width == 7);
    OA_CHECK(kit::compact_metrics.choice_arrow_height == 4);
    OA_CHECK(kit::compact_metrics.choice_item_height == 16);
    OA_CHECK(kit::compact_metrics.most_shown_choices == 8);
    OA_CHECK(kit::compact_metrics.choice_item_text_inset == 12);
    OA_CHECK(kit::compact_metrics.padlock_width == 5);
    OA_CHECK(kit::compact_metrics.padlock_height == 7);
    OA_CHECK(kit::compact_metrics.padlock_gap == 3);
    OA_CHECK(kit::compact_metrics.scroll_thumb_width == 5);
    OA_CHECK(kit::compact_metrics.least_thumb_height == 16);
    OA_CHECK(kit::compact_metrics.button_icon_inset == 3);
    OA_CHECK(kit::compact_metrics.button_icon_press == 1);
    OA_CHECK(kit::compact_metrics.button_square_numerator == 20);
    OA_CHECK(kit::compact_metrics.button_square_denominator == 32);
    OA_CHECK(kit::compact_metrics.large_mark_margin == 2);
    // The notice's and the prompt's own sizes.
    OA_CHECK(kit::compact_metrics.text_top == 38);
    OA_CHECK(kit::compact_metrics.paragraph_gap == 6);
    OA_CHECK(kit::compact_metrics.text_bottom_gap == 10);
    OA_CHECK(kit::compact_metrics.open_width == 96);
    OA_CHECK(kit::compact_metrics.prompt_button_padding == 16);
    OA_CHECK(kit::compact_metrics.progress_bar_height == 8);
}

/// Checks the Compact sizes of the list rows, chips, tabs, text fields,
/// cards and hover cards, which the settings dialog does not have.
void compact_metrics_of_the_new_controls() {
    OA_CHECK(kit::compact_metrics.list_row_height == 36);
    OA_CHECK(kit::compact_metrics.list_row_badge == 28);
    OA_CHECK(kit::compact_metrics.short_list_row_height == 28);
    OA_CHECK(kit::compact_metrics.list_row_inset == 4);
    OA_CHECK(kit::compact_metrics.list_row_title_top == 1);
    OA_CHECK(kit::compact_metrics.list_row_line_top == 15);
    OA_CHECK(kit::compact_metrics.list_row_gap == 6);
    OA_CHECK(kit::compact_metrics.hairline == 1);
    OA_CHECK(kit::compact_metrics.badge_dash == 2);
    OA_CHECK(kit::compact_metrics.badge_dash_gap == 1);
    OA_CHECK(kit::compact_metrics.chip_height == 12);
    OA_CHECK(kit::compact_metrics.chip_padding == 4);
    OA_CHECK(kit::compact_metrics.chip_gap == 4);
    OA_CHECK(kit::compact_metrics.state_icon_side == 14);
    OA_CHECK(kit::compact_metrics.tab_height == 18);
    OA_CHECK(kit::compact_metrics.tab_padding == 10);
    OA_CHECK(kit::compact_metrics.tab_tracking == 1);
    OA_CHECK(kit::compact_metrics.tab_rule == 2);
    OA_CHECK(kit::compact_metrics.tab_count_height == 12);
    OA_CHECK(kit::compact_metrics.tab_count_padding == 3);
    OA_CHECK(kit::compact_metrics.tab_count_gap == 4);
    OA_CHECK(kit::compact_metrics.field_height == 16);
    OA_CHECK(kit::compact_metrics.field_inset == 4);
    OA_CHECK(kit::compact_metrics.magnifier_side == 7);
    OA_CHECK(kit::compact_metrics.caret_width == 1);
    OA_CHECK(kit::compact_metrics.card_least_width == 120);
    OA_CHECK(kit::compact_metrics.card_gap == 6);
    OA_CHECK(kit::compact_metrics.card_inset == 4);
    OA_CHECK(kit::compact_metrics.hover_card_padding == 6);
    OA_CHECK(kit::compact_metrics.hover_card_arrow == 5);
    OA_CHECK(kit::compact_metrics.language_card_width == 139);
    OA_CHECK(kit::compact_metrics.language_card_height == 182);
    OA_CHECK(kit::compact_metrics.carousel_step == 132);
    OA_CHECK(kit::regular_metrics.language_card_width == 186);
    OA_CHECK(kit::regular_metrics.language_card_height == 244);
    OA_CHECK(kit::regular_metrics.carousel_step == 177);
    OA_CHECK(kit::large_metrics.language_card_width == 232);
    OA_CHECK(kit::large_metrics.language_card_height == 304);
    OA_CHECK(kit::large_metrics.carousel_step == 236);
}

/// Checks the Compact sizes of a settings section's rows against the
/// settings dialog's.
void compact_metrics_of_the_rows() {
    OA_CHECK(kit::compact_metrics.row_padding == 8);
    OA_CHECK(kit::compact_metrics.label_gap == 8);
    OA_CHECK(kit::compact_metrics.hint_gap == 2);
    OA_CHECK(kit::compact_metrics.tall_hint_line_gap == 3);
    OA_CHECK(kit::compact_metrics.most_hint_lines == 2);
    OA_CHECK(kit::compact_metrics.most_notice_lines == 4);
    OA_CHECK(kit::compact_metrics.slider_gap == 4);
    OA_CHECK(kit::compact_metrics.slider_line_height == 14);
    OA_CHECK(kit::compact_metrics.slider_value_width == 110);
    OA_CHECK(kit::compact_metrics.slider_value_gap == 10);
    OA_CHECK(kit::compact_metrics.lock_width == 148);
    OA_CHECK(kit::compact_metrics.choice_width == 200);
    OA_CHECK(kit::compact_metrics.wide_choice_width == 248);
    OA_CHECK(kit::compact_metrics.button_row_width == 76);
    OA_CHECK(kit::compact_metrics.folder_button_widths == (std::array<int32_t, 3>{46, 82, 44}));
    OA_CHECK(kit::compact_metrics.folder_button_gap == 4);
}

/// One size class's row of the design's table of sizes, in points.
struct ClassSizes {
    const kit::Metrics* metrics{};    ///< the class's metrics
    int32_t dialog_width{};           ///< the settings dialog's width
    int32_t dialog_height{};          ///< the settings dialog's height
    int32_t padding{};                ///< the padding
    int32_t nav_width{};              ///< the nav list's width
    int32_t hint_line_characters{};   ///< a host's line's characters under a row
    int32_t notice_width{};           ///< a notice's and a prompt's width
    int32_t greatest_notice_height{}; ///< their greatest height
    int32_t least_notice_height{};    ///< their least height
    kit::Point least_window{};        ///< the least window of the class, in points
};

/// The design's table of sizes: Compact, Regular and Large. Compact's least
/// window is the 640 by 480 picture.
const ClassSizes kClassSizes[] = {
    {&kit::compact_metrics, 480, 324, 12, 144, 50, 400, 440, 150, {640, 480}},
    {&kit::regular_metrics, 720, 486, 16, 176, 82, 520, 500, 150, {960, 540}},
    {&kit::large_metrics, 960, 600, 20, 208, 114, 600, 640, 150, {1280, 720}},
};

/// Checks each class's metrics against the design's table of sizes, and
/// that each class's dialog and notices fit its least window.
void each_class_has_the_tables_sizes() {
    for (const ClassSizes& sizes : kClassSizes) {
        const kit::Metrics& metrics = *sizes.metrics;
        OA_CHECK(metrics.dialog_width == sizes.dialog_width);
        OA_CHECK(metrics.dialog_height == sizes.dialog_height);
        OA_CHECK(metrics.padding == sizes.padding);
        OA_CHECK(metrics.nav_width == sizes.nav_width);
        OA_CHECK(metrics.hint_line_characters == sizes.hint_line_characters);
        OA_CHECK(metrics.notice_width == sizes.notice_width);
        OA_CHECK(metrics.greatest_notice_height == sizes.greatest_notice_height);
        OA_CHECK(metrics.least_notice_height == sizes.least_notice_height);
        OA_CHECK(metrics.dialog_width <= sizes.least_window.x);
        OA_CHECK(metrics.dialog_height <= sizes.least_window.y);
        OA_CHECK(metrics.notice_width <= sizes.least_window.x);
        OA_CHECK(metrics.greatest_notice_height <= sizes.least_window.y);
    }
    // The least windows are the classes' own.
    OA_CHECK(kClassSizes[1].least_window.x == kit::regular_from.x);
    OA_CHECK(kClassSizes[1].least_window.y == kit::regular_from.y);
    OA_CHECK(kClassSizes[2].least_window.x == kit::large_from.x);
    OA_CHECK(kClassSizes[2].least_window.y == kit::large_from.y);
    OA_CHECK(kit::regular_from.x == 960 && kit::regular_from.y == 540);
    OA_CHECK(kit::large_from.x == 1280 && kit::large_from.y == 720);
}

/// Checks that Regular and Large keep every other size Compact's: rows,
/// text and controls do not grow.
void the_classes_differ_only_in_the_tables_sizes() {
    for (const kit::Metrics* metrics : {&kit::regular_metrics, &kit::large_metrics}) {
        kit::Metrics same = *metrics;
        same.dialog_width = kit::compact_metrics.dialog_width;
        same.dialog_height = kit::compact_metrics.dialog_height;
        same.padding = kit::compact_metrics.padding;
        same.nav_width = kit::compact_metrics.nav_width;
        same.hint_line_characters = kit::compact_metrics.hint_line_characters;
        same.notice_width = kit::compact_metrics.notice_width;
        same.greatest_notice_height = kit::compact_metrics.greatest_notice_height;
        same.least_notice_height = kit::compact_metrics.least_notice_height;
        same.language_card_width = kit::compact_metrics.language_card_width;
        same.language_card_height = kit::compact_metrics.language_card_height;
        same.carousel_step = kit::compact_metrics.carousel_step;
        OA_CHECK(same == kit::compact_metrics);
        // Among them the sizes the design names as the same in every class.
        OA_CHECK(metrics->row_padding == 8);
        OA_CHECK(metrics->regular_line == 16);
        OA_CHECK(metrics->small_line == 12);
        OA_CHECK(metrics->slider_line_height == 14);
        OA_CHECK(metrics->switch_width == 52);
        OA_CHECK(metrics->button_height == 17);
    }
}

} // namespace

int main() {
    every_token_keeps_its_value();
    compact_metrics_match_the_dialog();
    compact_metrics_of_the_new_controls();
    compact_metrics_of_the_rows();
    each_class_has_the_tables_sizes();
    the_classes_differ_only_in_the_tables_sizes();
    return oa::test::check_exit_status();
}
