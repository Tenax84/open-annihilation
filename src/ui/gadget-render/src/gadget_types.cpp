// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Per-type record draws of the gadget engine, into the root record's face.
#include "render_internal.hpp"

#include "oa/ui/frontend_renderer/gadget_draw.hpp"
#include "oa/present/game_text.hpp"
#include "oa/base/game_math.hpp"
#include "oa/present/model/mesh_raster.hpp"
#include "oa/ui/gui_layout.hpp"
#include "oa/base/text.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace oa::ui::gadget_render {

namespace detail {

// An image list holds either a table of frames or a table of pointers to
// records carrying a frame.
const Sprite* list_image(const GadgetRenderer& renderer, const GadgetRecord& list, int32_t item) {
    if (list.refs.images == nullptr)
        return nullptr;
    if ((attributes(list) & attribute::image_records) != 0)
        return static_cast<const Sprite*>(list.refs.images) + item;
    if (renderer.art.list_image == nullptr)
        return nullptr;
    return renderer.art.list_image(renderer.art.context, list.refs.images, item);
}

} // namespace detail

using namespace detail;
namespace bevel = oa::ui::frontend_renderer;

namespace {

// A skin drawn over a record without its own grows this much past each edge.
constexpr int32_t kFallbackSkinGrowth = 3;
constexpr const char* kFallbackSkin = "Listbox";

// Attribute bits as these draws read them.
constexpr uint32_t kCheckboxArt = 0x80;    // buttons: CHECKBOX art; a grayed frame is not shaded
constexpr uint32_t kLabelShadow = 0x8;     // FNT labels: a dark copy one pixel right, three down
constexpr uint32_t kValueFromOne = 0x8;    // scroll bars: the value caption counts from one
constexpr uint32_t kScrollGrayed = 0x10;   // scroll bars: grayed like a locked bar
constexpr int32_t kPressedFrameOffset = 2; // grayed frames sit two past the pressed state
// The pixels a fitted caption leaves free of its gadget's width, as
// ui::gui_input::fit_text_to_width leaves them.
constexpr int32_t kFittedCaptionMargin = 6;

// List rows: image rows keep one texel of their frame's border unsampled;
// a text row shows its item flag 1 as a header.
constexpr int32_t kRowInset = 2;
constexpr int32_t kTexelInset = 1;
constexpr uint8_t kItemShaded = 1;
constexpr uint8_t kItemCrossed = 2;
constexpr int32_t kWrapMargin = 6;

// Scroll bar SLIDERS frames relative to the bar's base frame.
constexpr int32_t kSliderStart = 0;
constexpr int32_t kSliderTrack = 1;
constexpr int32_t kSliderEnd = 2;
constexpr int32_t kSliderKnobStart = 3;
constexpr int32_t kSliderKnobMiddle = 4;
constexpr int32_t kSliderKnobEnd = 5;
constexpr int32_t kKnobInset = 3;
constexpr int32_t kKnobTravelMargin = 6;
constexpr int32_t kKnobEndMargin = 4;
constexpr int32_t kValueGap = 2;
constexpr int32_t kValueDrop = 4;
constexpr size_t kNumberBytes = 20;

constexpr int32_t kProgressBorder = 2;
constexpr int32_t kProgressTrack = 4; // bar width lost to both borders

constexpr int32_t kTextBoxDrop = 3;
constexpr int32_t kShadowOffset = 1;
constexpr int32_t kShadowDrop = 3;

// Caption start of a multi-stage button: stage `stage` of the NUL-separated
// captions, bounded by the record.
char* stage_caption(GadgetRecord& record, int32_t stage) {
    char* text = layout::record_chars(record, field::text);
    char* end = layout::record_chars(record, 0) + layout::kGadgetRecordBytes - 1;
    for (; stage > 0 && text < end; --stage) {
        while (text < end && *text != '\0')
            ++text;
        if (text < end)
            ++text;
    }
    return text;
}

std::string bounded_text(const GadgetRecord& record, size_t offset) {
    return std::string(layout::record_string(record, offset));
}

// The caption width the caption fitting leaves behind: the
// last measure, taken with font slot 1 for labels and alternate-font buttons.
// A caption wider than its gadget less 6 pixels first draws its modern text
// smaller, down to present::lowest_text_size, until it fits: the renderer's
// most_text_size, which the caller puts back. Only a caption still too
// wide, or one the game font draws, loses characters from its end.
int32_t fit_caption(GadgetRenderer& renderer, GadgetPanel& panel, int32_t index) {
    const void* active = panel.active_gaf_font;
    renderer.most_text_size = present::game_font_text_size;
    {
        const auto& record = record_at(panel, index);
        const char* caption =
            ui::gui_input::gadget_text_by_name(panel, bounded_text(record, field::name), nullptr);
        const auto type = layout::gadget_type_of(record);
        if (type == gadget_type::label ||
            (type == gadget_type::button && (attributes(record) & attribute::alternate_font) != 0))
            panel.active_gaf_font = panel.gaf_fonts[1];
        if (caption != nullptr)
            renderer.most_text_size = present::fitted_text_size(
                [&](int32_t most_size) {
                    renderer.most_text_size = most_size;
                    return text_width(renderer, panel, caption);
                },
                i16(record, field::width) - kFittedCaptionMargin
            );
        panel.active_gaf_font = active;
    }
    ui::gui_input::fit_text_to_width(panel, index);
    auto& record = record_at(panel, index);
    const std::string name = bounded_text(record, field::name);
    const char* text = ui::gui_input::gadget_text_by_name(panel, name, nullptr);
    if (text == nullptr) {
        panel.active_gaf_font = active;
        return 0;
    }
    const auto type = layout::gadget_type_of(record);
    if (type == gadget_type::label ||
        (type == gadget_type::button && (attributes(record) & attribute::alternate_font) != 0))
        panel.active_gaf_font = panel.gaf_fonts[1];
    else
        panel.active_gaf_font = active;
    const int32_t width = text_width(renderer, panel, text);
    panel.active_gaf_font = panel.gaf_fonts[0];
    return width;
}

const Surface* backdrop_of(const GadgetPanel& panel) {
    const void* backdrop =
        panel.owner->backdrop != nullptr ? panel.owner->backdrop : panel.backdrop;
    return static_cast<const Surface*>(backdrop);
}

// Quad covering `rect` sampled with a one-texel inset, as the list and hot
// surface images are stretched.
void stretch_image(Surface* target, const Sprite* sprite, const Rect32& rect) {
    const present::PolygonVertex quad[4] = {
        {rect.x1, rect.y1}, {rect.x2, rect.y1}, {rect.x2, rect.y2}, {rect.x1, rect.y2}
    };
    const int32_t right = sprite->width - kTexelInset;
    const int32_t bottom = sprite->height - kTexelInset;
    const present::model::TexturePoint uv[4] = {
        {kTexelInset, kTexelInset}, {right, kTexelInset}, {right, bottom}, {kTexelInset, bottom}
    };
    present::model::texture_quad(target, sprite, quad, uv);
}

void format_number(char* out, int32_t value) {
    std::snprintf(out, kNumberBytes, "%d", value);
}

void draw_image_rows(
    GadgetRenderer& renderer,
    GadgetPanel& panel,
    GadgetRecord& list,
    const Rect32& rect,
    int32_t item_height
) {
    Surface* surface = face_of(panel);
    if (surface == nullptr)
        return;
    const Rect32 saved = present::surface_clip(*surface);
    present::set_surface_clip(*surface, rect);
    const uint32_t attrs = attributes(list);
    int32_t item = i16(list, field::list_top);
    const int32_t x = rect.x1 + kRowInset;
    int32_t y = rect.y1 + kRowInset;
    const uint8_t cross = mapped_color(panel, color_slot::crossed_row);
    do {
        const Sprite* sprite = list_image(renderer, list, item);
        if (sprite != nullptr && sprite->data != nullptr) {
            Rect32 row{x, y, rect.x2, y + item_height - 1};
            stretch_image(surface, sprite, row);
            // Without a flag table no flag applies.
            const uint8_t flags = list.refs.item_flags != nullptr ? list.refs.item_flags[item] : 0;
            if ((flags & kItemShaded) != 0) {
                present::shade_rect_level(surface, &row, shade_level::grayed);
            } else if ((flags & kItemCrossed) != 0) {
                present::draw_clipped_line(
                    surface, row.x1 + 1, row.y2 - 1, row.x2 - 2, row.y1 + 1, cross
                );
                present::draw_clipped_line(
                    surface, row.x1 + 2, row.y2 - 1, row.x2 - 1, row.y1 + 1, cross
                );
                present::draw_clipped_line(
                    surface, row.x1 + 1, row.y1 + 2, row.x2 - 1, row.y2 - 2, cross
                );
                present::draw_clipped_line(
                    surface, row.x1 + 2, row.y1 + 2, row.x2 - 2, row.y2 - 2, cross
                );
            }
        }
        // A missing frame leaves the selected row unshaded.
        if ((attrs & attribute::cycle_frames) == 0 && i16(list, field::list_selected) == item &&
            sprite != nullptr) {
            Rect32 selected{x, y, x + sprite->width - 1, y + sprite->height - 1};
            present::shade_rect_level(surface, &selected, shade_level::selected_image);
        }
        ++item;
        y += item_height;
    } while (y < rect.y2 && item < i16(list, field::list_count));
    present::set_surface_clip(*surface, saved);
}

// Offset of the row after the one starting at `offset` in a line table.
size_t next_row(const GadgetRecord& list, size_t offset) {
    const char* lines = list.refs.lines;
    while (offset < list.refs.lines_size) {
        const char c = lines[offset++];
        if (c == '\0' || c == kLineFeed)
            break;
    }
    return offset;
}

void draw_text_rows(
    GadgetRenderer& renderer,
    GadgetPanel& panel,
    int32_t index,
    const Rect32& rect,
    int32_t item_height,
    int32_t line_height
) {
    GadgetRecord& list = record_at(panel, index);
    ui::gui_input::apply_gadget_font(panel, index);
    Surface* surface = face_of(panel);
    const uint32_t attrs = attributes(list);
    const int32_t top = i16(list, field::list_top);
    const int32_t count = i16(list, field::list_count);
    const char* lines = list.refs.lines;
    const size_t lines_size = list.refs.lines_size;
    size_t row_offset = 0;
    for (int32_t skipped = 0; skipped < top && row_offset < lines_size; ++skipped)
        row_offset = next_row(list, row_offset);
    int32_t remaining = i16(list, field::height);
    int32_t row = 0;
    int32_t offset = 0;
    bool header = false;
    do {
        Rect32 bounds{rect.x1 + kRowInset, offset + kRowInset + rect.y1, 0, 0};
        bounds.x2 = i16(list, field::width) - kRowInset + bounds.x1;
        bounds.y2 = bounds.y1 + item_height;
        // Rows are drawn up to the next NUL, as the game reads them.
        size_t end = row_offset;
        while (end < lines_size && lines[end] != '\0')
            ++end;
        std::string text(lines + std::min(row_offset, lines_size), lines + end);
        size_t skip = 0;
        const int32_t width = text_width(renderer, panel, text.c_str());
        const uint8_t color =
            mapped_color(panel, static_cast<uint32_t>(i32(list, field::color_foreground)));
        const int32_t item = top + row;
        if (list.refs.item_flags == nullptr || list.refs.item_flags[item] != kItemShaded) {
            if (!text.empty() && text[0] == kRowCode) {
                if (text.size() > 1 && text[1] == kHeaderCode)
                    header = true;
                skip = kRowCodeBytes;
            }
        } else {
            header = true;
        }
        row_offset += skip;
        char* shown = text.data() + std::min(skip, text.size());
        int32_t x = 0;
        int32_t span = 0;
        bool aligned = true;
        if ((attrs & attribute::horizontal) != 0) {
            x = bounds.x1;
            span = bounds.x2 - bounds.x1 + 1;
        } else if ((attrs & attribute::right_aligned) != 0) {
            x = bounds.x2 - width;
            span = width;
        } else if ((attrs & attribute::centered) != 0) {
            const int32_t left = bounds.x1;
            x = std::max((bounds.x2 - width + left) / 2, left);
            span = bounds.x2 - x + 1;
        } else {
            // Without an alignment bit nothing lands on the face.
            aligned = false;
        }
        if (aligned) {
            if (text_height(renderer, panel) + kWrapMargin < i16(list, field::list_item_height))
                draw_wrapped_text(
                    renderer, panel, surface, shown, x, bounds.y1, span, rect.y2 - rect.y1, 0
                );
            else
                draw_text(renderer, panel, surface, shown, x, bounds.y1, span, 0);
        }
        row_offset = next_row(list, row_offset);
        if (!header) {
            if ((attrs & attribute::cycle_frames) == 0 && i16(list, field::list_selected) == item &&
                count != 0)
                present::shade_rect_level(surface, &bounds, shade_level::selected_row);
            else
                renderer.text_color = color;
        } else {
            header = false;
            for (int32_t step = 0; step < shade_level::header_rows; ++step)
                present::shade_rect_level(surface, &bounds, shade_level::header_first - step);
        }
        ++row;
        remaining -= item_height;
        offset += item_height;
    } while (line_height <= remaining && top + row < count);
}

} // namespace

void draw_skin(GadgetRenderer& renderer, GadgetPanel& panel, int32_t index, const void* skin) {
    GadgetRecord& record = record_at(panel, index);
    Rect32 rect = record_rect(record);
    if (skin == nullptr) {
        if (panel.list_skin == nullptr)
            return;
        skin = art_find(renderer, panel.list_skin, kFallbackSkin);
        if (skin == nullptr)
            return;
        rect.x1 -= kFallbackSkinGrowth;
        rect.y1 -= kFallbackSkinGrowth;
        rect.x2 += kFallbackSkinGrowth;
        rect.y2 += kFallbackSkinGrowth;
    }
    Surface* face = face_of(panel);
    // A type-8 record whose texture number matches is never tiled here:
    // find_list_skin_record always returns 0.
    if (art_frame_count(renderer, skin) < 2) {
        present::draw_sprite(face, art_frame(renderer, skin, 0), 0, 0);
        return;
    }
    const Sprite* first = art_frame(renderer, skin, 0);
    if (first == nullptr)
        return;
    const int32_t tile_width = first->width;
    const int32_t tile_height = first->height;
    const int32_t origin_x = index == 0 ? 0 : rect.x1;
    const int32_t origin_y = index == 0 ? 0 : rect.y1;
    const int32_t height = rect.y2 - rect.y1 + 1;
    const int32_t width = rect.x2 - rect.x1 + 1;
    for (const auto& tile : gui_layout::skin_tiles(width, height, tile_width, tile_height))
        present::draw_sprite(
            face,
            art_frame(renderer, skin, static_cast<int32_t>(tile.frame)),
            origin_x + tile.x,
            origin_y + tile.y
        );
}

void draw_button(GadgetRenderer& renderer, GadgetPanel& panel, int32_t index) {
    // The size a caption too wide is fitted at holds while it is drawn.
    struct SizeKept {
        GadgetRenderer& renderer;

        ~SizeKept() { renderer.most_text_size = present::game_font_text_size; }
    } size_kept{renderer};

    panel.owner->redraw = 1;
    GadgetRecord& record = record_at(panel, index);
    Rect32 rect = record_rect(record);
    const uint32_t attrs = attributes(record);
    if ((attrs & attribute::alternate_font) != 0)
        panel.active_gaf_font = panel.gaf_fonts[1];
    ui::gui_input::apply_gadget_font(panel, index);
    const int32_t fit_width = fit_caption(renderer, panel, index);
    Surface* face = face_of(panel);
    const bool grayed = (record.bytes[field::button_flags] & 1) != 0;
    const int16_t status = i16(record, field::button_status);
    const uint8_t stages = record.bytes[field::button_stages];
    const uint8_t stage = record.bytes[field::button_stage];
    const uint8_t base = record.bytes[field::button_frame_base];
    const uint8_t dark = mapped_color(panel, color_slot::dark_edge);
    bool shaded = false;
    const void* art = record.refs.sprite;
    if (art == nullptr) {
        if (grayed) {
            const uint8_t gray = mapped_color(panel, color_slot::grayed_face);
            bevel::fill_box_raised(face, rect, dark, gray, gray);
        } else {
            const uint8_t light = mapped_color(panel, color_slot::light_edge);
            const uint8_t fill = mapped_color(panel, color_slot::face);
            if (status != 0)
                bevel::fill_box_raised(face, rect, dark, light, fill);
            else
                bevel::fill_box_sunken(face, rect, dark, light, fill);
        }
    } else {
        const int32_t count = art_frame_count(renderer, art);
        Sprite* frame = nullptr;
        if (grayed) {
            if ((attrs & attribute::cycle_frames) != 0) {
                frame = art_frame(renderer, art, count - 1);
            } else if (stages != 0) {
                frame = art_frame(renderer, art, stage);
                shaded = true;
            } else if ((attrs & attribute::scroll_step_mask) != 0) {
                frame = art_frame(renderer, art, base);
                shaded = true;
            } else {
                const int32_t offset = std::min(status + kPressedFrameOffset, count - 1);
                frame = art_frame(renderer, art, base + offset);
                shaded = (attrs & kCheckboxArt) == 0;
            }
        } else if (status != 0 && stages < count) {
            frame = art_frame(renderer, art, stages != 0 ? count - 2 : base + status);
        } else {
            frame = art_frame(renderer, art, stages != 0 ? stage : base);
        }
        if (frame != nullptr) {
            const int32_t level = i32(record, field::color_foreground);
            const int32_t x = rect.x1 + frame->origin_x;
            const int32_t y = rect.y1 + frame->origin_y;
            if (level != 0)
                present::draw_sprite_lit(face, frame, x, y, level);
            else
                present::draw_sprite(face, frame, x, y);
        }
    }

    const uint8_t caption_color =
        status != 0
            ? dark
            : mapped_color(panel, static_cast<uint32_t>(i32(record, field::color_foreground)));
    renderer.text_color = caption_color;
    char* text = stage_caption(record, stages != 0 ? stage : 0);
    // A caption aligned across is centred down the button by the line height
    // of the panel's first font, which the caption fitting leaves active, even
    // when the alternate font writes it; a bottom caption's line ends by the
    // line height of the font that writes it.
    const int32_t fitted_line_height = text_height(renderer, panel);
    if ((attrs & attribute::alternate_font) != 0)
        panel.active_gaf_font = panel.gaf_fonts[1];
    constexpr uint32_t aligned_across =
        attribute::horizontal | attribute::right_aligned | attribute::centered;
    const auto pen = layout::button_caption_pen(
        {rect.x1, rect.y1, rect.x2, rect.y2},
        attrs,
        fit_width,
        (attrs & aligned_across) != 0 ? fitted_line_height : text_height(renderer, panel),
        status != 0
    );
    const int32_t span = rect.x2 - rect.x1 + 1;
    const char key = static_cast<char>(record.bytes[field::button_quick_key]);
    if (pen && (attrs & (attribute::horizontal | attribute::right_aligned)) != 0) {
        draw_text(renderer, panel, face, text, pen->x, pen->y, span, 0);
    } else if (pen && (attrs & attribute::centered) != 0) {
        int32_t x = pen->x;
        const int32_t y = pen->y;
        char copy[field::text_bytes + 1]{};
        char needle[2] = {key, '\0'};
        char* found = nullptr;
        if (!grayed) {
            oa::base::text::copy_padded(copy, text, field::text_bytes);
            if (const auto offset = ui::frontend_renderer::quick_key_offset(copy, key);
                offset != std::string_view::npos)
                found = copy + offset;
        }
        if (found == nullptr) {
            draw_text(renderer, panel, face, text, x, y, span, 0);
        } else if (!split_for_quick_key(copy)) {
            // A caption past ASCII is drawn whole. Its key is underlined
            // where the font draws the caption glyph by glyph, as measured.
            draw_text(renderer, panel, face, text, x, y, span, 0);
            if (drawn_in_game_font(renderer, panel, copy)) {
                *found = '\0';
                const int32_t key_x = x + text_width(renderer, panel, copy);
                const int32_t key_end = key_x + text_width(renderer, panel, needle);
                const uint8_t underline =
                    status != 0 ? dark : mapped_color(panel, color_slot::underline);
                const int32_t line_y = text_height(renderer, panel) - 1 + y;
                present::draw_clipped_line(face, key_x, line_y, key_end - 1, line_y, underline);
            }
        } else {
            *found = '\0';
            draw_text(renderer, panel, face, copy, x, y, span, 0);
            x += text_width(renderer, panel, copy);
            const int32_t key_x = x;
            renderer.text_color = caption_color;
            draw_text(renderer, panel, face, needle, x, y, span, 0);
            x += text_width(renderer, panel, needle);
            const uint8_t underline =
                status != 0 ? dark : mapped_color(panel, color_slot::underline);
            const int32_t line_y = text_height(renderer, panel) - 1 + y;
            present::draw_clipped_line(face, key_x, line_y, x - 1, line_y, underline);
            renderer.text_color = caption_color;
            draw_text(renderer, panel, face, found + 1, x, y, span, 0);
        }
    } else if (pen) {
        // Bottom caption: the quick key is cut out of the record text itself,
        // so a later draw shows only the part before it. A caption the
        // modern fonts draw part of is drawn whole, its key unmarked.
        int32_t x = pen->x;
        const int32_t bottom_y = pen->y;
        const auto offset = ui::frontend_renderer::quick_key_offset(text, key);
        char* found = offset != std::string_view::npos && drawn_in_game_font(renderer, panel, text)
                          ? text + offset
                          : nullptr;
        if (found == nullptr) {
            draw_text(renderer, panel, face, text, x, bottom_y, span, 0);
        } else {
            char glyph[2] = {key, '\0'};
            *found = '\0';
            draw_text(renderer, panel, face, text, x, bottom_y, span, 0);
            x += text_width(renderer, panel, layout::record_chars(record, field::text));
            renderer.text_color = mapped_color(panel, color_slot::quick_key);
            draw_text(renderer, panel, face, glyph, x, bottom_y, span, 0);
            x += text_width(renderer, panel, glyph);
            renderer.text_color =
                mapped_color(panel, static_cast<uint32_t>(i32(record, field::color_foreground)));
            draw_text(renderer, panel, face, found + 1, x, bottom_y, span, 0);
        }
    }
    panel.active_gaf_font = panel.gaf_fonts[0];
    if (shaded) {
        present::gray_rect(face, rect);
        present::shade_rect_level(face, &rect, shade_level::grayed);
    }
}

void draw_list(GadgetRenderer& renderer, GadgetPanel& panel, int32_t index) {
    panel.owner->redraw = 1;
    GadgetRecord& list = record_at(panel, index);
    const Rect32 rect = record_rect(list);
    if (const Surface* backdrop = backdrop_of(panel); backdrop != nullptr)
        present::blit_rect(face_of(panel), *backdrop, rect, rect);
    else if ((panel.owner->flags & ui::gui_input::panel_flag::modal_backdrop) == 0)
        draw_skin(renderer, panel, index, nullptr);
    const int32_t line_height = text_height(renderer, panel);
    const int16_t authored = i16(list, field::list_item_height);
    const int32_t item_height = authored == 0 ? line_height + 1 : authored;
    const uint32_t attrs = attributes(list);
    if ((attrs & attribute::text_list) != 0 && list.refs.lines != nullptr &&
        i16(list, field::list_count) != 0)
        draw_text_rows(renderer, panel, index, rect, item_height, line_height);
    else if ((attrs & (attribute::image_list | attribute::image_records)) != 0)
        draw_image_rows(renderer, panel, list, rect, item_height);
}

void draw_scroll_bar(GadgetRenderer& renderer, GadgetPanel& panel, int32_t index) {
    GadgetRecord& bar = record_at(panel, index);
    Surface* face = face_of(panel);
    ui::gui_input::apply_gadget_font(panel, index);
    layout::GadgetRect outer{};
    layout::GadgetRect knob{};
    layout::scroll_bar_rects(bar, outer, knob);
    const void* art = bar.refs.scroll_ticks;
    const int32_t x0 = i16(bar, field::x);
    const int32_t y0 = i16(bar, field::y);
    const int32_t width = i16(bar, field::width);
    const int32_t height = i16(bar, field::height);
    const int32_t base = bar.bytes[field::scroll_tick_frame];
    const int32_t knob_position = i16(bar, field::scroll_knob);
    // A missing frame counts as zero wide.
    const auto tile = [&](int32_t frame) { return art_frame(renderer, art, base + frame); };
    const auto frame_width = [](const Sprite* frame) {
        return frame != nullptr ? int32_t{frame->width} : 0;
    };
    const auto frame_height = [](const Sprite* frame) {
        return frame != nullptr ? int32_t{frame->height} : 0;
    };
    if (art == nullptr) {
        const uint8_t dark = mapped_color(panel, color_slot::dark_edge);
        const uint8_t light = mapped_color(panel, color_slot::light_edge);
        const uint8_t fill = mapped_color(panel, color_slot::face);
        bevel::fill_frame_raised(face, to_rect(outer), dark, light, fill);
        bevel::fill_frame_sunken(face, to_rect(knob), dark, light, fill);
    } else if (width < height) {
        int32_t y = y0;
        int32_t x = x0;
        int32_t bottom = height + y0 - 1;
        const Sprite* start = tile(kSliderStart);
        if (start != nullptr)
            present::draw_sprite(face, start, x, y);
        y += frame_height(start);
        const Sprite* track = tile(kSliderTrack);
        while (frame_height(track) + y <= bottom) {
            present::draw_sprite(face, track, x, y);
            y += frame_height(track);
            if (track == nullptr)
                break;
        }
        const Sprite* end = tile(kSliderEnd);
        bottom = bottom - frame_height(end) + 1;
        present::draw_sprite(face, end, x, bottom);
        x += frame_width(end) >> 1;
        const Sprite* knob_start = tile(kSliderKnobStart);
        x -= frame_width(knob_start) >> 1;
        int32_t knob_top = knob_position + y0 + kKnobInset;
        int32_t size =
            std::min(height - kKnobTravelMargin, int32_t{i16(bar, field::scroll_knob_size)});
        const int32_t knob_bottom = std::min(size + knob_top - 1, height + y0 - kKnobEndMargin);
        knob_top = std::min(knob_top, knob_bottom - size + 1);
        present::draw_sprite(face, knob_start, x, knob_top);
        size -= frame_height(knob_start);
        knob_top += frame_height(knob_start);
        const Sprite* knob_middle = tile(kSliderKnobMiddle);
        while (knob_top <= knob_bottom - frame_height(knob_middle)) {
            present::draw_sprite(face, knob_middle, x, knob_top);
            knob_top += frame_height(knob_middle);
            size -= frame_height(knob_middle);
            if (knob_middle == nullptr)
                break;
        }
        present::draw_sprite(face, knob_middle, x, knob_bottom - frame_height(knob_middle));
        const Sprite* knob_end = tile(kSliderKnobEnd);
        present::draw_sprite(face, knob_end, x, knob_bottom - frame_height(knob_end) + 1);
    } else {
        int32_t x = x0;
        int32_t y = y0;
        const int32_t right = width + x0 - 1;
        const Sprite* start = tile(kSliderStart);
        if (start != nullptr)
            present::draw_sprite(face, start, x, y);
        x += frame_width(start);
        const Sprite* track = tile(kSliderTrack);
        while (frame_width(track) + x <= right) {
            present::draw_sprite(face, track, x, y);
            x += frame_width(track);
            if (track == nullptr)
                break;
        }
        const Sprite* end = tile(kSliderEnd);
        present::draw_sprite(face, end, right - frame_width(end) + 1, y);
        y += frame_height(end) >> 1;
        const Sprite* knob_start = tile(kSliderKnobStart);
        y -= frame_height(knob_start) >> 1;
        const int32_t knob_x =
            std::min(knob_position + x0 + kKnobInset, right - frame_width(knob_start) - 2);
        present::draw_sprite(face, knob_start, knob_x, y);
    }
    const uint32_t attrs = attributes(bar);
    if ((attrs & attribute::right_aligned) != 0) {
        renderer.text_color = mapped_color(panel, color_slot::scroll_value);
        char caption[kNumberBytes]{};
        if (bar.bytes[field::text] != 0) {
            oa::base::text::copy_padded(
                caption, layout::record_chars(bar, field::text), sizeof(caption) - 1
            );
        } else if (const int32_t scale = i32(bar, field::scroll_thickness); scale != 0) {
            const double value = static_cast<double>(knob_position) * scale /
                                 static_cast<double>(width - i16(bar, field::scroll_knob_size));
            format_number(caption, static_cast<int32_t>(base::game_math::truncate_to_int64(value)));
        } else {
            format_number(caption, knob_position + ((attrs & kValueFromOne) != 0 ? 1 : 0));
        }
        fnt_text(renderer, face, caption, width + x0 + kValueGap, y0 + kValueDrop, kNoLimit);
    }
    if ((attrs & kScrollGrayed) != 0 || i32(bar, field::scroll_locked) != 0) {
        Rect32 rect = record_rect(bar);
        present::gray_rect(face, rect);
        present::shade_rect_level(face, &rect, shade_level::grayed);
    }
}

void draw_text_box(GadgetRenderer& renderer, GadgetPanel& panel, int32_t index) {
    GadgetRecord& box = record_at(panel, index);
    ui::gui_input::apply_gadget_font(panel, index);
    const Rect32 rect = record_rect(box);
    Surface* face = face_of(panel);
    if ((attributes(box) & attribute::horizontal) == 0) {
        if (const Surface* backdrop = backdrop_of(panel); backdrop != nullptr)
            present::blit_rect(face, *backdrop, rect, rect);
        else
            draw_skin(renderer, panel, index, nullptr);
    } else {
        fill_clipped(face, rect, mapped_color(panel, color_slot::dark_edge));
    }
    const int32_t level = i32(box, field::color_foreground);
    renderer.text_color = mapped_color(panel, static_cast<uint32_t>(level));
    const int32_t y = rect.y1 + kTextBoxDrop;
    char* text = layout::record_chars(box, field::text);
    draw_text(renderer, panel, face, text, rect.x1, y, rect.x2 - rect.x1, level);
    if (index == panel.captured) {
        const auto caret =
            static_cast<size_t>(std::clamp<int32_t>(panel.caret, 0, field::text_bytes - 1));
        const char held = text[caret];
        text[caret] = '\0';
        const int32_t width = text_width(renderer, panel, text);
        text[caret] = held;
        present::draw_clipped_line(
            face,
            width + rect.x1,
            y,
            width + rect.x1,
            text_height(renderer, panel) + y,
            mapped_color(panel, color_slot::caret)
        );
    }
}

void draw_progress(GadgetRenderer& renderer, GadgetPanel& panel, int32_t index) {
    panel.active_gaf_font = panel.gaf_fonts[1];
    GadgetRecord& bar = record_at(panel, index);
    Surface* surface = face_of(panel);
    // The host's backdrop is read-only here, so the lock goes into a copy of
    // its descriptor.
    Surface backdrop{};
    if (surface == nullptr && panel.backdrop != nullptr) {
        backdrop = *static_cast<const Surface*>(panel.backdrop);
        surface = &backdrop;
    }
    if (surface != nullptr)
        (void)present::lock_display_surface(*surface);
    const int32_t x = i16(bar, field::x);
    const int32_t y = i16(bar, field::y);
    const int32_t width = i16(bar, field::width);
    const int32_t height = i16(bar, field::height);
    Rect32 rect{x, y, width + x, height + y};
    bevel::fill_box_raised(
        surface,
        rect,
        mapped_color(panel, color_slot::dark_edge),
        mapped_color(panel, color_slot::light_edge),
        mapped_color(panel, color_slot::face)
    );
    rect.x1 += kProgressBorder;
    rect.y1 += kProgressBorder;
    rect.x2 -= kProgressBorder;
    rect.y2 -= kProgressBorder;
    fill_clipped(
        surface, rect, static_cast<uint8_t>(layout::record_u32(bar, field::color_background))
    );
    const int32_t value = i32(bar, field::progress_value);
    const double filled = static_cast<double>(value) /
                          static_cast<double>(i32(bar, field::progress_scale)) *
                          static_cast<double>(width - kProgressTrack);
    rect.x2 = static_cast<int32_t>(base::game_math::truncate_to_int64(filled)) + rect.x1;
    fill_clipped(
        surface, rect, static_cast<uint8_t>(layout::record_u32(bar, field::color_foreground))
    );
    if (i32(bar, field::progress_show_value) != 0) {
        char digits[kNumberBytes]{};
        format_number(digits, value);
        const int32_t text = text_width(renderer, panel, digits);
        const int32_t line = text_height(renderer, panel);
        draw_text(
            renderer,
            panel,
            surface,
            digits,
            (width / 2 - text / 2) + x,
            (height / 2 - line / 2) + y,
            kNoLimit,
            0
        );
    }
    panel.active_gaf_font = panel.gaf_fonts[0];
    present::unlock_display_surface();
}

void draw_hot_image(GadgetRenderer& renderer, GadgetPanel& panel, int32_t index) {
    GadgetRecord& record = record_at(panel, index);
    const Rect32 rect = record_rect(record);
    Surface* face = face_of(panel);
    const Sprite* sprite = nullptr;
    if (record.refs.hot_sequence == nullptr) {
        if (record.refs.hot_image == nullptr) {
            fill_clipped(face, rect, mapped_color(panel, color_slot::empty_image));
            return;
        }
        sprite = static_cast<const Sprite*>(record.refs.hot_image);
    } else {
        sprite = art_frame(renderer, record.refs.hot_sequence, i16(record, field::hot_frame));
        if (sprite == nullptr)
            return;
        if (sprite->encoding != OA_SPRITE_RAW) {
            present::draw_sprite(
                face, sprite, sprite->origin_x + rect.x1, sprite->origin_y + rect.y1
            );
            return;
        }
    }
    stretch_image(face, sprite, rect);
}

void draw_label(GadgetRenderer& renderer, GadgetPanel& panel, int32_t index) {
    panel.active_gaf_font = panel.gaf_fonts[1];
    GadgetRecord& label = record_at(panel, index);
    const int32_t font_record = ui::gui_input::apply_gadget_font(panel, index);
    char* text = layout::record_chars(label, field::text);

    // A one-line label wider than its gadget draws its modern text smaller,
    // down to present::lowest_text_size, until it fits, and is placed at that
    // width; a label that wraps keeps the game font's size.
    struct SizeKept {
        GadgetRenderer& renderer;

        ~SizeKept() { renderer.most_text_size = present::game_font_text_size; }
    } size_kept{renderer};

    if (const Rect32 lines = record_rect(label);
        font_record == layout::kNoGadget && text_height(renderer, panel) * 2 >= lines.y2 - lines.y1)
        renderer.most_text_size = present::fitted_text_size(
            [&](int32_t most_size) {
                renderer.most_text_size = most_size;
                return text_width(renderer, panel, text);
            },
            lines.x2 - lines.x1 + 1
        );
    if (i16(label, field::x) == -1) {
        const int32_t root_width = i16(root_of(panel), field::width);
        layout::set_record_i16(
            label,
            field::x,
            static_cast<int16_t>((root_width - text_width(renderer, panel, text)) / 2)
        );
    }
    const Rect32 rect = record_rect(label);
    Surface* face = face_of(panel);
    if (const uint32_t background = layout::record_u32(label, field::color_background);
        background != 0)
        fill_clipped(face, rect, mapped_color(panel, background));
    const uint32_t attrs = attributes(label);
    const int32_t width = i16(label, field::width);
    int32_t x = rect.x1;
    if ((attrs & attribute::right_aligned) != 0)
        x = width + rect.x1 - text_width(renderer, panel, text);
    else if ((attrs & attribute::centered) != 0)
        x = width / 2 + rect.x1 - text_width(renderer, panel, text) / 2;
    if (font_record != layout::kNoGadget && (attrs & kLabelShadow) != 0) {
        renderer.text_color = mapped_color(panel, color_slot::dark_edge);
        fnt_text(renderer, face, text, x + kShadowOffset, rect.y1 + kShadowDrop, kNoLimit);
    }
    const int32_t level = i32(label, field::color_foreground);
    renderer.text_color = level;
    if (font_record == layout::kNoGadget) {
        const int32_t height = rect.y2 - rect.y1;
        if (text_height(renderer, panel) * 2 < height)
            draw_wrapped_text(
                renderer, panel, face, text, x, rect.y1, rect.x2 - rect.x1 + 1, height + 1, level
            );
        else
            draw_text(renderer, panel, face, text, x, rect.y1, rect.x2 - rect.x1 + 1, level);
    } else {
        fnt_text(renderer, face, text, x, rect.y1, kNoLimit);
    }
    if ((label.bytes[field::label_flags] & 1) == 0) {
        const char key = static_cast<char>(label.bytes[field::label_quick_key]);
        // A label the modern fonts draw part of leaves its key unmarked.
        if (key != '\0' && drawn_in_game_font(renderer, panel, text)) {
            char copy[field::text_bytes + 1]{};
            oa::base::text::copy_padded(copy, text, field::text_bytes);
            char needle[2] = {key, '\0'};
            if (const auto offset = ui::frontend_renderer::quick_key_offset(copy, key);
                offset != std::string_view::npos) {
                char* found = copy + offset;
                *found = '\0';
                const int32_t before = text_width(renderer, panel, copy);
                const int32_t key_width = text_width(renderer, panel, needle);
                const int32_t line_y = text_height(renderer, panel) - 1 + rect.y1;
                present::draw_clipped_line(
                    face,
                    rect.x1 + before,
                    line_y,
                    rect.x1 + before + key_width - 1,
                    line_y,
                    mapped_color(panel, color_slot::underline)
                );
            }
        }
    } else {
        shade_record(renderer, panel, index);
    }
    panel.active_gaf_font = panel.gaf_fonts[0];
}

void draw_image(GadgetRenderer&, GadgetPanel& panel, int32_t index) {
    panel.owner->redraw = 1;
    GadgetRecord& record = record_at(panel, index);
    const auto* sprite = static_cast<const Sprite*>(record.refs.image);
    if (sprite == nullptr)
        return;
    Rect32 rect = record_rect(record);
    Surface* face = face_of(panel);
    const int32_t level = i32(record, field::flash_level);
    if (level < 1)
        present::draw_sprite(face, sprite, sprite->origin_x + rect.x1, sprite->origin_y + rect.y1);
    else
        present::draw_sprite_lit(
            face, sprite, sprite->origin_x + rect.x1, sprite->origin_y + rect.y1, level
        );
    if ((record.bytes[field::image_flags] & 1) != 0)
        present::shade_rect_level(face, &rect, shade_level::image_flagged);
}

void shade_record(GadgetRenderer&, GadgetPanel& panel, int32_t index) {
    Rect32 rect = record_rect(record_at(panel, index));
    Surface* face = face_of(panel);
    present::gray_rect(face, rect);
    present::shade_rect_level(face, &rect, shade_level::grayed);
}

} // namespace oa::ui::gadget_render
