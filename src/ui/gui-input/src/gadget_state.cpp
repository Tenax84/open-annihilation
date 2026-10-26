// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "gadget_internal.hpp"
#include "oa/base/game_math.hpp"
#include "oa/base/text/line_break.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace oa::ui::gui_input {
using namespace detail;
using ui::gui_layout::find_gadget;
using ui::gui_layout::kNoGadget;

namespace {

/// Stores a directory, cut to 254 characters, with a backslash after it and the rest zeroed.
void set_path(std::array<char, kPanelPathBytes>& target, std::string_view path) {
    target.fill('\0');
    const auto length = std::min(path.size(), kPanelPathBytes - 2);
    std::copy_n(path.begin(), length, target.begin());
    target[length] = '\\';
}

// strnicmp(a, b, count) == 0 for ASCII names.
[[nodiscard]] bool equal_nocase(std::string_view a, std::string_view b, std::size_t count) {
    for (std::size_t index = 0; index < count; ++index) {
        const auto ca = index < a.size() ? static_cast<unsigned char>(a[index]) : 0;
        const auto cb = index < b.size() ? static_cast<unsigned char>(b[index]) : 0;
        if (lower(ca) != lower(cb))
            return false;
        if (ca == 0)
            return true;
    }
    return true;
}

void set_bit0_u16(GadgetRecord& record, std::size_t offset, uint32_t value) {
    const auto old = ui::gui_layout::record_u16(record, offset);
    ui::gui_layout::set_record_u16(
        record, offset, static_cast<uint16_t>((old & ~1U) | (value & 1U))
    );
}

} // namespace

void init_gadget_panel(GadgetPanel& panel) {
    panel.owner.reset();
    panel.gui_path[0] = '\0';
    panel.gaf_path[0] = '\0';
    panel.font_path[0] = '\0';
    panel.quick_keys_enabled = 1;
    const auto tick = current_tick(panel);
    panel.hovered = kNoGadget;
    panel.last_tick = tick;
    panel.tick_delta = 0;
    panel.word_after_tick_delta = 1;
    panel.list_skin = nullptr;
    panel.backdrop = nullptr;
    panel.byte_after_backdrop = 0;
    panel.dragging = 0;
    panel.gaf_fonts = {};
    panel.keyboard_enabled = 1;
    panel.active_gaf_font = nullptr;
}

void enable_keyboard(GadgetPanel& panel) {
    panel.keyboard_enabled = 1;
}

void disable_keyboard(GadgetPanel& panel) {
    panel.keyboard_enabled = 0;
}

void mark_dirty(GadgetPanel& panel) {
    panel.dirty = 1;
}

void request_owner_redraw(GadgetPanel& panel) {
    owner_redraw(panel);
}

void set_keys_from_queue(GadgetPanel& panel, int32_t value) {
    if (panel.owner)
        panel.owner->keys_from_queue = value;
}

void set_quick_keys_enabled(GadgetPanel& panel, int32_t value) {
    panel.quick_keys_enabled = value;
}

void select_gaf_font(GadgetPanel& panel, int32_t slot) {
    panel.active_gaf_font = panel.gaf_fonts[static_cast<std::size_t>(slot)];
}

void place_root(
    int16_t& x,
    int16_t& y,
    int32_t width,
    int32_t height,
    uint32_t flags,
    int32_t screen_width,
    int32_t screen_height,
    int32_t hud_strip
) noexcept {
    const bool first = (flags & panel_flag::first_draw) != 0;
    if ((flags & panel_flag::centre) != 0 && first) {
        x = root_centred;
        y = root_centred;
    }
    if ((flags & panel_flag::beside_hud) != 0 && first) {
        x = root_beside_hud;
        y = root_centred;
    }
    if (x == root_centred) {
        x = static_cast<int16_t>((screen_width - width) / 2);
        y = static_cast<int16_t>((screen_height - height) / 2);
    }
    if (x == root_beside_hud) {
        x = static_cast<int16_t>((screen_width - hud_strip - width) / 2 + hud_strip);
        y = static_cast<int16_t>((screen_height - height) / 2);
    }
    if (screen_width < width + x)
        x = static_cast<int16_t>((screen_width - width) / 2);
    if (screen_height < y + height)
        y = static_cast<int16_t>((screen_height - height) / 2);
}

int32_t set_backdrop(GadgetPanel& panel, const void* backdrop) {
    if (panel.owner)
        panel.owner->backdrop = backdrop;
    return 1;
}

void set_gui_path(GadgetPanel& panel, std::string_view path) {
    set_path(panel.gui_path, path);
}

void set_gaf_path(GadgetPanel& panel, std::string_view path) {
    set_path(panel.gaf_path, path);
}

void set_font_path(GadgetPanel& panel, std::string_view path) {
    set_path(panel.font_path, path);
}

void set_default_font(GadgetPanel& panel, const void* font) {
    panel.default_font = font;
}

bool top_panel_named(const GadgetPanel& panel, std::string_view name) {
    if (!panel.owner)
        return false;
    return equal_nocase(
        ui::gui_layout::record_string(panel.owner->table.records[0], field::name),
        name,
        field::name_bytes
    );
}

bool is_button_message(const GadgetPanel& panel, uint8_t buttons) {
    const auto message = panel.pointer.message;
    if ((buttons & kLeftButton) != 0)
        return message == pointer_message::left_down || message == pointer_message::left_double;
    if ((buttons & kRightButton) != 0)
        return message == pointer_message::right_down || message == pointer_message::right_double;
    return false;
}

bool is_double_click(const GadgetPanel& panel, uint8_t buttons) {
    if ((buttons & kLeftButton) != 0)
        return panel.pointer.message == pointer_message::left_double;
    if ((buttons & kRightButton) != 0)
        return panel.pointer.message == pointer_message::right_double;
    return false;
}

bool buttons_held(const GadgetPanel& panel, int32_t mask) {
    return (static_cast<uint32_t>(panel.buttons) & static_cast<uint32_t>(mask)) != 0;
}

void set_last_message(GadgetPanel& panel, int32_t message) {
    panel.last_message = message;
    panel.owner->last_message = message;
}

int32_t last_message(const GadgetPanel& panel) {
    return panel.last_message;
}

bool capture_gadget(GadgetPanel& panel, int32_t index) {
    if (panel.captured != kNoGadget &&
        ui::gui_layout::gadget_type_of(record_at(panel, panel.captured)) == gadget_type::text_box) {
        panel.captured = kNoGadget;
    }
    if (panel.captured != kNoGadget && panel.captured != index)
        return false;
    panel.captured = index;
    if (index != kNoGadget &&
        ui::gui_layout::gadget_type_of(record_at(panel, index)) == gadget_type::text_box) {
        panel.caret = static_cast<int32_t>(text_length(record_at(panel, index), field::text));
    }
    return true;
}

bool activated_gadget_is(const GadgetPanel& panel, std::string_view name) {
    if (!panel.owner || panel.activated == kNoGadget)
        return false;
    const auto& record = panel.owner->table.records[static_cast<std::size_t>(panel.activated)];
    return ui::gui_layout::record_string(record, field::name) == name;
}

void clear_activation(GadgetPanel& panel) {
    panel.activated = kNoGadget;
}

int32_t apply_gadget_font(GadgetPanel& panel, int32_t index) {
    const auto wanted = signed_byte(record_at(panel, index).bytes[field::font]);
    const auto end = scan_end(panel);
    int32_t ordinal = 0;
    int32_t candidate = 1;
    for (; candidate < end; ++candidate) {
        const auto& record = record_at(panel, candidate);
        if (ui::gui_layout::gadget_type_of(record) != gadget_type::font_resource)
            continue;
        if (ordinal == wanted) {
            select_font(panel, record.refs.font);
            break;
        }
        ++ordinal;
    }
    if (candidate == end) {
        select_font(panel, panel.default_font);
        return kNoGadget;
    }
    return candidate;
}

void update_help_text(GadgetPanel& panel) {
    std::string help;
    if (panel.hovered != kNoGadget) {
        panel.help_source = panel.hovered;
        help = std::string(
            ui::gui_layout::record_string(record_at(panel, panel.hovered), field::help)
        );
    }
    const auto target = find_gadget(panel.owner->records(), "HELPTEXT");
    if (target != kNoGadget) {
        ui::gui_layout::copy_record_cstring(
            record_at(panel, target), field::text, translate(panel, help.c_str())
        );
        panel.dirty = 1;
    }
}

void rename_gadget(GadgetPanel& panel, std::string_view name, std::string_view new_name) {
    if (!panel.owner)
        return;
    const auto index = find_gadget(panel.owner->records(), name);
    if (index == kNoGadget)
        return;
    auto& record = record_at(panel, index);
    const auto length = std::min<std::size_t>(new_name.size(), field::name_bytes);
    std::copy_n(new_name.begin(), length, text_of(record, field::name));
    record.bytes[field::name + length] = 0;
}

void clear_group_status(GadgetPanel& panel, int32_t index) {
    const auto group = record_at(panel, index).bytes[field::group];
    if (group == 0)
        return;
    const auto end = scan_end(panel);
    for (int32_t other = 1; other < end; ++other) {
        auto& record = record_at(panel, other);
        if (ui::gui_layout::gadget_type_of(record) == gadget_type::button && other != index &&
            record.bytes[field::group] == group && i16(record, field::button_status) != 0) {
            set_i16(record, field::button_status, 0);
            draw(panel, GadgetDraw::button, other);
            owner_redraw(panel);
        }
    }
}

void clear_group_status_marking(GadgetPanel& panel, int32_t index) {
    const auto group = record_at(panel, index).bytes[field::group];
    const auto end = scan_end(panel);
    for (int32_t other = 1; other < end; ++other) {
        auto& record = record_at(panel, other);
        if (ui::gui_layout::gadget_type_of(record) == gadget_type::button &&
            record.bytes[field::group] == group && i16(record, field::button_status) != 0) {
            set_i16(record, field::button_status, 0);
            draw(panel, GadgetDraw::button, other);
            panel.dirty = 1;
        }
    }
}

void set_gadget_active(GadgetPanel& panel, int32_t index, int32_t active) {
    auto records = panel.owner->records();
    auto& record = record_at(panel, index);
    record.bytes[field::active] = static_cast<uint8_t>(active);
    const auto type = ui::gui_layout::gadget_type_of(record);
    if (type == gadget_type::scroll_bar) {
        const auto last = static_cast<int32_t>(ui::gui_layout::gadget_last_index(records));
        for (int32_t other = 0; other <= last && other < static_cast<int32_t>(records.size());
             ++other) {
            auto& member = record_at(panel, other);
            if (ui::gui_layout::gadget_type_of(member) == gadget_type::button &&
                member.bytes[field::group] == record.bytes[field::group]) {
                member.bytes[field::active] = static_cast<uint8_t>(active);
            }
        }
    } else if (type == gadget_type::list_box && active == 0) {
        const auto bar = ui::gui_layout::find_group_member(records, index, gadget_type::scroll_bar);
        set_gadget_active(panel, bar, 0);
    }
    if (active == 0 && index == panel.owner->focus)
        focus_nearest(panel, FocusDirection::next);
    panel.dirty = 1;
}

int32_t gadget_active_by_name(GadgetPanel& panel, std::string_view name) {
    const auto index = find_gadget(panel.owner->records(), name);
    if (index == kNoGadget)
        return -1;
    return record_at(panel, index).bytes[field::active];
}

void set_gadget_active_by_name(GadgetPanel& panel, std::string_view name, int32_t active) {
    if (!panel.owner)
        return;
    const auto index = find_gadget(panel.owner->records(), name);
    if (index != kNoGadget)
        set_gadget_active(panel, index, active);
}

void assign_quick_key(GadgetPanel& panel, int32_t index) {
    if (index == kNoGadget)
        return;
    auto& record = record_at(panel, index);
    const auto type = ui::gui_layout::gadget_type_of(record);
    if (type == gadget_type::label && text_length(record, field::label_link) == 0)
        return;
    if (type != gadget_type::label && type != gadget_type::button)
        return;
    if (type == gadget_type::button) {
        const auto rule = caption_quick_key(
            attributes(record),
            record.bytes[field::button_stages],
            ui::gui_layout::record_string(record, field::text)
        );
        if (rule == CaptionQuickKey::keep)
            return;
        record.bytes[field::button_quick_key] = 0;
        if (rule == CaptionQuickKey::none)
            return;
    } else {
        record.bytes[field::label_quick_key] = 0;
    }
    const auto caption = std::string(ui::gui_layout::record_string(record, field::text));
    const auto last =
        static_cast<int32_t>(ui::gui_layout::gadget_last_index(panel.owner->records()));
    const auto limit = std::min(last, static_cast<int32_t>(panel.owner->table.records.size()) - 1);
    // A table shorter than its record count assigns nothing.
    if (limit < last)
        return;
    std::vector<int8_t> taken;
    for (int32_t other = 0; other <= limit; ++other) {
        const auto& candidate = record_at(panel, other);
        const auto candidate_type = ui::gui_layout::gadget_type_of(candidate);
        if (candidate_type == gadget_type::button)
            taken.push_back(
                static_cast<int8_t>(signed_byte(candidate.bytes[field::button_quick_key]))
            );
        else if (candidate_type == gadget_type::label)
            taken.push_back(
                static_cast<int8_t>(signed_byte(candidate.bytes[field::label_quick_key]))
            );
    }
    const char key = free_quick_key(caption, taken);
    if (key == '\0')
        return;
    if (type == gadget_type::button)
        record.bytes[field::button_quick_key] = static_cast<uint8_t>(key);
    else
        record.bytes[field::label_quick_key] = static_cast<uint8_t>(key);
}

CaptionQuickKey
caption_quick_key(uint32_t attributes, int32_t stages, std::string_view caption) noexcept {
    if ((attributes & attribute::no_quick_key) != 0)
        return CaptionQuickKey::keep;
    if (stages != 0)
        return CaptionQuickKey::none;
    return caption.empty() ? CaptionQuickKey::keep : CaptionQuickKey::assign;
}

char free_quick_key(std::string_view caption, std::span<const int8_t> taken) noexcept {
    for (std::size_t at = 0; at < caption.size();) {
        // A character of two to four bytes is passed over whole: no one key
        // types it, and its first byte alone is no character.
        const auto read = oa::base::text::break_character(caption.substr(at));
        const char character = caption[at];
        at += read.bytes;
        if (read.bytes != 1 || character == ' ')
            continue;
        const auto wanted = lower(static_cast<int8_t>(character));
        if (std::none_of(taken.begin(), taken.end(), [wanted](int8_t key) {
                return lower(key) == wanted;
            }))
            return character;
    }
    return '\0';
}

void set_text_assign_quick_key(GadgetPanel& panel, std::string_view name, std::string_view text) {
    if (!panel.owner)
        return;
    const auto index = find_gadget(panel.owner->records(), name);
    if (index == kNoGadget)
        return;
    ui::gui_layout::set_record_string(
        record_at(panel, index), field::text, text, field::text_bytes
    );
    panel.dirty = 1;
    assign_quick_key(panel, index);
}

void set_text_box_text(GadgetPanel& panel, int32_t index, std::string_view text) {
    ui::gui_layout::copy_record_cstring(record_at(panel, index), field::text, text);
    if (panel.captured == index)
        panel.caret = static_cast<int32_t>(text.size());
}

void split_stage_captions(GadgetPanel& panel, int32_t index) {
    auto& record = record_at(panel, index);
    std::array<char, field::text_bytes> packed{};
    std::size_t out = 0;
    std::size_t in = field::text;
    const auto stages = record.bytes[field::button_stages];
    for (uint32_t stage = 0; stage < stages; ++stage) {
        const auto source = std::string(ui::gui_layout::record_string(record, in));
        const std::string_view translated = translate(panel, source.c_str());
        const auto length =
            std::min(translated.size(), packed.size() - std::min(out, packed.size()));
        std::copy_n(translated.begin(), length, packed.begin() + static_cast<std::ptrdiff_t>(out));
        out += translated.size() + 1;
        in += source.size() + 1;
        if (out >= packed.size() || in >= ui::gui_layout::kGadgetRecordBytes)
            break;
    }
    std::copy(packed.begin(), packed.end(), record.bytes.begin() + field::text);
}

void set_gadget_text(GadgetPanel& panel, int32_t index, std::string_view text, int32_t max_chars) {
    if (index == kNoGadget || !panel.owner)
        return;
    const std::string source(text);
    const std::string translated = translate(panel, source.c_str());
    auto& record = record_at(panel, index);
    const auto type = ui::gui_layout::gadget_type_of(record);
    if (type == gadget_type::button) {
        ui::gui_layout::set_record_string(record, field::text, translated, field::text_bytes);
        assign_quick_key(panel, index);
        if (record.bytes[field::button_stages] != 0) {
            for (auto offset = field::text;
                 offset < field::text + field::text_bytes && record.bytes[offset] != 0;
                 ++offset) {
                if (record.bytes[offset] == '|')
                    record.bytes[offset] = 0;
            }
            split_stage_captions(panel, index);
        }
    } else if (type == gadget_type::text_box) {
        set_text_box_text(panel, index, translated);
        if (max_chars != 0)
            set_i16(record, field::text_max_chars, max_chars);
    } else if (type == gadget_type::label) {
        ui::gui_layout::set_record_string(record, field::text, translated, field::text_bytes);
        if (record.bytes[field::label_link] != 0)
            assign_quick_key(panel, index);
    }
    panel.dirty = 1;
}

void set_gadget_text_by_name(
    GadgetPanel& panel, std::string_view name, std::string_view text, int32_t max_chars
) {
    if (!panel.owner)
        return;
    const auto index = find_gadget(panel.owner->records(), name);
    if (index != kNoGadget)
        set_gadget_text(panel, index, text, max_chars);
}

void set_gadget_color_by_name(GadgetPanel& panel, std::string_view name, uint32_t color) {
    if (!panel.owner)
        return;
    const auto index = find_gadget(panel.owner->records(), name);
    if (index == kNoGadget)
        return;
    ui::gui_layout::set_record_u32(record_at(panel, index), field::color_foreground, color);
    panel.dirty = 1;
}

char* gadget_text_by_name(GadgetPanel& panel, std::string_view name, std::string* output) {
    if (!panel.owner)
        return nullptr;
    const auto index = find_gadget(panel.owner->records(), name);
    if (index == kNoGadget)
        return nullptr;
    auto& record = record_at(panel, index);
    const auto type = ui::gui_layout::gadget_type_of(record);
    if (type != gadget_type::button && type != gadget_type::text_box && type != gadget_type::label)
        return nullptr;
    if (output != nullptr)
        *output = std::string(ui::gui_layout::record_string(record, field::text));
    return text_of(record);
}

void set_parent_gadget_text(GadgetPanel& panel, std::string_view name, std::string_view text) {
    if (!panel.owner || !panel.owner->next)
        return;
    auto parent = panel.owner->next->records();
    const auto index = find_gadget(parent, name);
    if (index == kNoGadget)
        return;
    auto& record = parent[static_cast<std::size_t>(index)];
    const auto type = ui::gui_layout::gadget_type_of(record);
    if (type == gadget_type::text_box)
        set_text_box_text(panel, index, text);
    else if (type == gadget_type::button || type == gadget_type::label)
        ui::gui_layout::set_record_string(record, field::text, text, field::text_bytes);
    panel.dirty = 1;
}

int32_t gadget_status(GadgetPanel& panel, int32_t index) {
    return i16(record_at(panel, index), field::button_status);
}

int32_t button_stage_by_name(GadgetPanel& panel, std::string_view name) {
    const auto index = find_gadget(panel.owner->records(), name);
    if (index == kNoGadget)
        return -1;
    const auto& record = record_at(panel, index);
    if (ui::gui_layout::gadget_type_of(record) != gadget_type::button)
        return -1;
    return record.bytes[field::button_stage];
}

int32_t button_stage(GadgetPanel& panel, int32_t index) {
    const auto& record = record_at(panel, index);
    if (ui::gui_layout::gadget_type_of(record) != gadget_type::button)
        return -1;
    return record.bytes[field::button_stage];
}

bool set_button_stage(GadgetPanel& panel, int32_t index, uint8_t stage) {
    auto& record = record_at(panel, index);
    if (ui::gui_layout::gadget_type_of(record) != gadget_type::button)
        return false;
    record.bytes[field::button_stage] = stage;
    return true;
}

bool set_button_stage_by_name(GadgetPanel& panel, std::string_view name, uint8_t stage) {
    const auto index = find_gadget(panel.owner->records(), name);
    if (index != kNoGadget)
        record_at(panel, index).bytes[field::button_stage] = stage;
    return index != kNoGadget;
}

bool set_status_by_name(GadgetPanel& panel, std::string_view name, int32_t status) {
    const auto index = find_gadget(panel.owner->records(), name);
    if (index == kNoGadget)
        return false;
    set_i16(record_at(panel, index), field::button_status, status);
    panel.dirty = 1;
    if (status != 0)
        clear_group_status(panel, index);
    return true;
}

void set_status(GadgetPanel& panel, int32_t index, int16_t status) {
    set_i16(record_at(panel, index), field::button_status, status);
    clear_group_status(panel, index);
}

void set_grayed(GadgetPanel& panel, int32_t index, uint8_t grayed) {
    set_bit0_u16(record_at(panel, index), field::button_flags, grayed);
    panel.dirty = 1;
}

void set_grayed_by_name(GadgetPanel& panel, std::string_view name, uint8_t grayed) {
    const auto index = find_gadget(panel.owner->records(), name);
    if (index != kNoGadget)
        set_bit0_u16(record_at(panel, index), field::button_flags, grayed);
}

void set_gadget_disabled(GadgetPanel& panel, int32_t index, uint32_t disabled) {
    if (index == kNoGadget)
        return;
    auto& record = record_at(panel, index);
    switch (ui::gui_layout::gadget_type_of(record)) {
    case gadget_type::button:
        set_bit0_u16(record, field::button_flags, disabled);
        break;
    case gadget_type::list_box:
        if (disabled != 0)
            record.bytes[field::attributes_high] |= 1;
        else
            record.bytes[field::attributes_high] &= 0xFE;
        break;
    case gadget_type::scroll_bar: {
        ui::gui_layout::set_record_u32(record, field::scroll_locked, disabled);
        const auto group = record.bytes[field::group];
        const auto end = scan_end(panel);
        for (int32_t other = 1; other < end; ++other) {
            auto& member = record_at(panel, other);
            if (ui::gui_layout::gadget_type_of(member) == gadget_type::button &&
                member.bytes[field::group] == group &&
                (attributes(member) & attribute::scroll_step_mask) != 0) {
                set_bit0_u16(member, field::button_flags, disabled);
            }
        }
        break;
    }
    case gadget_type::label: {
        const auto flags = ui::gui_layout::record_u32(record, field::label_flags);
        ui::gui_layout::set_record_u32(record, field::label_flags, (flags & ~1U) | (disabled & 1U));
        break;
    }
    case gadget_type::image:
        set_bit0_u16(record, field::image_flags, disabled);
        break;
    default:
        break;
    }
}

void set_gadget_disabled_by_name(GadgetPanel& panel, std::string_view name, uint32_t disabled) {
    const auto index = find_gadget(panel.owner->records(), name);
    if (index != kNoGadget)
        set_gadget_disabled(panel, index, disabled);
}

void set_quick_key_by_name(GadgetPanel& panel, std::string_view name, uint8_t key) {
    const auto index = find_gadget(panel.owner->records(), name);
    if (index != kNoGadget)
        record_at(panel, index).bytes[field::button_quick_key] = key;
    panel.dirty = 1;
}

void fit_text_to_width(GadgetPanel& panel, int32_t index) {
    auto& record = record_at(panel, index);
    const auto name = std::string(ui::gui_layout::record_string(record, field::name));
    char* text = gadget_text_by_name(panel, name, nullptr);
    if (text == nullptr)
        return;
    const auto type = ui::gui_layout::gadget_type_of(record);
    if (type == gadget_type::label ||
        (type == gadget_type::button && (attributes(record) & attribute::alternate_font) != 0)) {
        panel.active_gaf_font = panel.gaf_fonts[1];
    }
    for (;;) {
        if (text_width(panel, text) <= i16(record, field::width) - 6)
            break;
        const auto length = std::strlen(text);
        if (length == 0)
            break;
        text[length - 1] = '\0';
    }
    panel.active_gaf_font = panel.gaf_fonts[0];
}

void mark_text_box_focus(GadgetPanel& panel, int32_t index) {
    panel.dirty = 1;
    const auto last = std::min<int32_t>(
        ui::gui_layout::gadget_last_index(panel.owner->records()),
        static_cast<int32_t>(panel.owner->table.records.size()) - 1
    );
    for (int32_t other = 1; other <= last; ++other) {
        auto& record = record_at(panel, other);
        if (ui::gui_layout::gadget_type_of(record) == gadget_type::text_box)
            ui::gui_layout::set_record_u32(record, field::color_foreground, 0);
    }
    auto& record = record_at(panel, index);
    const auto type = ui::gui_layout::gadget_type_of(record);
    if (type == gadget_type::text_box) {
        ui::gui_layout::set_record_u32(record, field::color_foreground, 0x1E);
    } else if (type != gadget_type::label && type != gadget_type::list_box) {
        draw(panel, GadgetDraw::focus_outline, index);
    }
}

void set_caret_and_draw(
    GadgetPanel& panel, int32_t index, char* text, int32_t max_chars, int32_t clear
) {
    const auto length = static_cast<int32_t>(std::strlen(text));
    if (clear == 0 && length <= max_chars) {
        panel.caret = length;
    } else {
        text[0] = '\0';
        panel.caret = 0;
    }
    draw(panel, GadgetDraw::text_box, index);
}

void focus_text_box(GadgetPanel& panel, int32_t index) {
    apply_gadget_font(panel, index);
    capture_gadget(panel, index);
    panel.owner->focus = index;
    auto& record = record_at(panel, index);
    set_caret_and_draw(panel, index, text_of(record), i16(record, field::text_max_chars), 0);
    clear_keys(panel);
}

void focus_gadget_by_name(GadgetPanel& panel, std::string_view name) {
    const auto index = find_gadget(panel.owner->records(), name);
    if (index == kNoGadget)
        return;
    panel.captured = kNoGadget;
    panel.owner->focus = index;
    if (ui::gui_layout::gadget_type_of(record_at(panel, index)) == gadget_type::text_box)
        focus_text_box(panel, index);
}

void set_focus(GadgetPanel& panel, int32_t index) {
    panel.captured = kNoGadget;
    panel.owner->focus = index;
    if (ui::gui_layout::gadget_type_of(record_at(panel, index)) == gadget_type::text_box)
        focus_text_box(panel, index);
}

void focus_nearest(GadgetPanel& panel, FocusDirection direction) {
    focus_nearest(panel, static_cast<int32_t>(direction));
}

void focus_nearest(GadgetPanel& panel, int32_t direction) {
    constexpr int32_t wrap = 25'000'000;
    constexpr int32_t row_stride = 5000;
    int32_t chosen = panel.owner->focus;
    if (chosen == kNoGadget)
        return;
    std::array<int32_t, ui::gui_layout::kGadgetCapacity> column{};
    const auto end = scan_end(panel);
    for (int32_t index = 1; index < end; ++index) {
        const auto x = static_cast<int32_t>(i16(record_at(panel, index), field::x));
        int32_t snapped = x;
        for (int32_t probe = 1; probe < end && column[static_cast<std::size_t>(probe)] != 0;
             ++probe) {
            const auto delta = column[static_cast<std::size_t>(probe)] - x;
            if (delta < 10 && delta > -10) {
                snapped = column[static_cast<std::size_t>(probe)];
                break;
            }
        }
        column[static_cast<std::size_t>(index)] = snapped;
    }

    const auto& focus = record_at(panel, chosen);
    int32_t origin = direction;
    int32_t best = 0;
    switch (direction) {
    case 0:
    case 1:
        origin = i16(focus, field::y) * row_stride + i16(focus, field::x);
        best = direction == 0 ? origin - wrap : origin + wrap;
        break;
    case 2:
    case 3:
        origin = column[static_cast<std::size_t>(chosen)] * row_stride + i16(focus, field::y);
        best = direction == 2 ? origin - wrap : origin + wrap;
        break;
    default:
        break;
    }

    int32_t score = direction;
    for (int32_t index = 1; index < end; ++index) {
        const auto& record = record_at(panel, index);
        const auto type = ui::gui_layout::gadget_type_of(record);
        if (!ui::gui_layout::keyboard_focusable(
                static_cast<int8_t>(record.bytes[field::active]),
                attributes(record),
                type,
                (record.bytes[field::button_flags] & 1) != 0,
                type == gadget_type::scroll_bar && i32(record, field::scroll_locked) != 0,
                i16(record, field::width),
                i16(record, field::height)
            ))
            continue;
        if (direction == 0 || direction == 1) {
            score = i16(record, field::y) * row_stride + i16(record, field::x);
        } else if (direction == 2 || direction == 3) {
            score = column[static_cast<std::size_t>(index)] * row_stride + i16(record, field::y);
        }
        if (direction == 0 || direction == 2) {
            if (score >= origin)
                score -= wrap;
            if (score > best) {
                chosen = index;
                best = score;
            }
        } else if (direction == 1 || direction == 3) {
            if (score <= origin)
                score += wrap;
            if (score < best) {
                chosen = index;
                best = score;
            }
        }
    }
    set_focus(panel, chosen);
    const auto focused = panel.owner->focus;
    if (ui::gui_layout::gadget_type_of(record_at(panel, focused)) == gadget_type::text_box)
        focus_text_box(panel, focused);
}

void close_top_panel(GadgetPanel& panel) {
    if (!panel.owner)
        return;
    const auto flags = panel.owner->flags;
    panel.captured = kNoGadget;
    panel.activated = kNoGadget;
    panel.hovered = kNoGadget;
    if (panel.owner->on_command != nullptr)
        panel.owner->on_command(panel);
    draw_panel(panel, 2);
    auto closed = std::move(panel.owner);
    panel.owner = std::move(closed->next);
    if (panel.owner)
        panel.owner->redraw = 1;
    closed.reset();
    if ((flags & panel_flag::shade_below) != 0)
        draw_panel(panel, panel_flag::redraw);
}

GadgetOwner* load_panel(
    GadgetPanel& panel, const ui::gui_layout::Layout& layout, std::string_view name, uint32_t flags
) {
    const bool merge = (flags & panel_flag::merge) != 0 && panel.owner;
    if ((flags & panel_flag::shade_below) != 0) {
        draw(panel, GadgetDraw::shade_panel, 0);
        owner_redraw(panel);
    }
    int32_t drawn = 1;
    if (layout.gadgets.empty())
        return nullptr;

    GadgetOwner* owner = nullptr;
    std::size_t base = 0;
    if (!merge) {
        auto created = std::make_unique<GadgetOwner>();
        owner = created.get();
        ui::gui_layout::load_gadget_records(layout, owner->records());
        created->next = std::move(panel.owner);
        panel.owner = std::move(created);
    } else {
        owner = panel.owner.get();
        auto records = owner->records();
        base = static_cast<std::size_t>(ui::gui_layout::gadget_last_index(records) + 1);
        if (base >= records.size())
            return nullptr;
        const auto loaded = ui::gui_layout::load_gadget_records(layout, records.subspan(base));
        auto& loaded_root = records[base];
        const auto loaded_last =
            static_cast<int32_t>(ui::gui_layout::gadget_last_index(records.subspan(base)));
        const auto target = find_gadget(records, "PANEL");
        int32_t dx = i16(loaded_root, field::x);
        int32_t dy = i16(loaded_root, field::y);
        if (target != kNoGadget) {
            flags |= panel_flag::no_save_under;
            auto& frame = records[static_cast<std::size_t>(target)];
            frame.bytes[field::active] = 0;
            dx = (i16(frame, field::width) - i16(loaded_root, field::width)) / 2 +
                 i16(frame, field::x);
            dy = (i16(frame, field::height) - i16(loaded_root, field::height)) / 2 +
                 i16(frame, field::y);
        }
        for (int32_t index = 1;
             index <= loaded_last && base + static_cast<std::size_t>(index) < records.size();
             ++index) {
            auto& record = records[base + static_cast<std::size_t>(index)];
            set_i16(record, field::x, i16(record, field::x) + dx);
            set_i16(record, field::y, i16(record, field::y) + dy);
        }
        set_i16(
            records[0], field::record_count, i16(records[0], field::record_count) + loaded_last
        );
        for (std::size_t index = 0; index + 1 < loaded; ++index)
            records[base + index] = records[base + index + 1];
    }
    owner->on_tick = nullptr;
    owner->backdrop = nullptr;
    owner->on_key = nullptr;
    owner->flags = 0;
    if (!merge && (flags & panel_flag::modal_backdrop) != 0)
        owner->flags = panel_flag::modal_backdrop;
    owner->flags |= flags & panel_flag::shade_below;
    panel.owner->redraw = 1;
    panel.owner->keys_from_queue = 0;
    auto records = owner->records();
    auto& root = records[0];
    ui::gui_layout::set_record_string(root, field::name, name, field::name_bytes);
    panel.captured = kNoGadget;
    if ((flags & panel_flag::no_draw) == 0)
        drawn = draw_panel(panel, flags | panel_flag::first_draw);

    const auto last = std::min<int32_t>(
        ui::gui_layout::gadget_last_index(records), static_cast<int32_t>(records.size()) - 1
    );
    const auto bind_default = [&](std::size_t slot,
                                  std::string_view first,
                                  std::size_t first_count,
                                  std::string_view second,
                                  std::size_t second_count) {
        if (text_length(root, slot) != 0)
            return;
        for (int32_t index = 1; index <= last; ++index) {
            const auto& record = records[static_cast<std::size_t>(index)];
            if (ui::gui_layout::gadget_type_of(record) != gadget_type::button)
                continue;
            const auto record_name = ui::gui_layout::record_string(record, field::name);
            if (equal_nocase(record_name, first, first_count) ||
                equal_nocase(record_name, second, second_count)) {
                ui::gui_layout::copy_record_cstring(root, slot, record_name);
                break;
            }
        }
    };
    bind_default(field::enter_default, "OK", 2, "NEXT", 4);
    bind_default(field::escape_default, "PREV", 4, "Cancel", 6);
    if (text_length(root, field::default_focus) == 0) {
        owner->focus = 0;
        focus_nearest(panel, FocusDirection::next);
    } else {
        owner->focus =
            find_gadget(records, ui::gui_layout::record_string(root, field::default_focus));
    }
    panel.activated = kNoGadget;
    if (drawn == 1) {
        if (i16(root, field::record_count) == 1 &&
            ui::gui_layout::gadget_type_of(records[1]) == gadget_type::text_box) {
            focus_text_box(panel, 1);
        }
        return owner;
    }
    if (!merge) {
        auto failed = std::move(panel.owner);
        panel.owner = std::move(failed->next);
    }
    return nullptr;
}

} // namespace oa::ui::gui_input
