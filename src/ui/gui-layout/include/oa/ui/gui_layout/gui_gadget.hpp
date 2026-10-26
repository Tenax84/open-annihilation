// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// Runtime gadget records of the GUI engine. Each record is a 0x15B-byte
// scalar image laid out by the `field` offsets below, so type-specific fields
// overlap. A reference cannot live in a 32-bit record slot on a 64-bit host,
// so GadgetRefs holds the references and the record keeps only their slots.

#include "oa/ui/gui_layout.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace oa::ui::gui_input {
struct GadgetPanel;
}

namespace oa::ui::gui_layout {

inline constexpr std::size_t kGadgetRecordBytes = 0x15B;
// The panel block holds 200 records including the root.
inline constexpr std::size_t kGadgetCapacity = limit::gadgets;
inline constexpr int32_t kNoGadget = -1;

namespace field {
// Common fields.
inline constexpr std::size_t type = 0x00;
inline constexpr std::size_t group = 0x01; // `assoc`
inline constexpr std::size_t name = 0x02;
inline constexpr std::size_t name_bytes = 0x10;
inline constexpr std::size_t x = 0x13;
inline constexpr std::size_t y = 0x15;
inline constexpr std::size_t width = 0x17;
inline constexpr std::size_t height = 0x19;
inline constexpr std::size_t attributes = 0x1B;
inline constexpr std::size_t attributes_high = 0x1C; // second byte of attributes
inline constexpr std::size_t color_foreground = 0x1F;
inline constexpr std::size_t color_background = 0x23;
inline constexpr std::size_t texture = 0x27;
inline constexpr std::size_t font = 0x28;
inline constexpr std::size_t active = 0x29;
inline constexpr std::size_t common_attributes = 0x2A;
inline constexpr std::size_t gadget_gaf = 0x2B;    // slot of GadgetRefs::gadget_gaf
inline constexpr std::size_t button_sprite = 0x2F; // buttons: slot of GadgetRefs::sprite
inline constexpr std::size_t help = 0x33;
inline constexpr std::size_t help_bytes = 0x81;
inline constexpr std::size_t gaf_file = 0xB4;
inline constexpr std::size_t text = 0xB6;
inline constexpr std::size_t text_bytes = 0x80;

// Root panel record.
inline constexpr std::size_t record_count = 0xB6;
inline constexpr std::size_t panel_surface = 0xBC; // slot of GadgetRefs::surface
inline constexpr std::size_t panel_gaf = 0xC0;     // slot of GadgetRefs::panel_gaf
inline constexpr std::size_t panel_skin = 0xC4;    // also type 11; slot of GadgetRefs::skin
inline constexpr std::size_t version_major = 0xC9;
inline constexpr std::size_t version_minor = 0xCA;
inline constexpr std::size_t version_revision = 0xCB;
inline constexpr std::size_t enter_default = 0xCC;
inline constexpr std::size_t escape_default = 0xDC;
inline constexpr std::size_t default_focus = 0xEC;
inline constexpr std::size_t panel_name = 0xFC;

// Type 1 buttons.
inline constexpr std::size_t button_stages = 0x136;
inline constexpr std::size_t button_stage = 0x137;
inline constexpr std::size_t button_status = 0x138;
inline constexpr std::size_t button_quick_key = 0x13A;
inline constexpr std::size_t button_frame_base = 0x13B;
inline constexpr std::size_t button_flags = 0x13C; // bit 0: grayed out

// Type 2 list boxes.
inline constexpr std::size_t list_repeat_tick = 0xB6;
inline constexpr std::size_t list_selected = 0xBA;
inline constexpr std::size_t list_top = 0xBC;
inline constexpr std::size_t list_last_top = 0xBE;
inline constexpr std::size_t list_count = 0xC0;
inline constexpr std::size_t list_lines = 0xC2;      // slot of GadgetRefs::lines
inline constexpr std::size_t list_images = 0xC6;     // slot of GadgetRefs::images
inline constexpr std::size_t list_skin = 0xCA;       // slot of GadgetRefs::list_skin
inline constexpr std::size_t list_changed = 0xCE;    // slot of GadgetRefs::list_changed
inline constexpr std::size_t list_item_flags = 0xD6; // slot of GadgetRefs::item_flags
inline constexpr std::size_t list_item_height = 0xDA;

// Type 3 text boxes.
inline constexpr std::size_t text_max_chars = 0x138;
inline constexpr std::size_t text_skin = 0x13A; // slot of GadgetRefs::text_skin

// Type 4 scroll bars.
inline constexpr std::size_t scroll_range = 0x136;
inline constexpr std::size_t scroll_thickness = 0x13C;
// Slot of GadgetRefs::scroll_value, in the same word as scroll_thickness.
inline constexpr std::size_t scroll_value = 0x13C;
inline constexpr std::size_t scroll_knob = 0x140;
inline constexpr std::size_t scroll_knob_size = 0x142;
inline constexpr size_t scroll_callback = 0x144; // slot of GadgetRefs::scroll_changed
inline constexpr std::size_t scroll_callback_argument = 0x14A;
inline constexpr std::size_t scroll_ticks = 0x14E; // slot of GadgetRefs::scroll_ticks
inline constexpr std::size_t scroll_tick_frame = 0x152;
inline constexpr std::size_t scroll_locked = 0x157;

// Type 5 labels.
inline constexpr std::size_t label_text_bytes = 0x7F;
inline constexpr std::size_t label_link = 0x136;
inline constexpr std::size_t label_link_bytes = 0x10;
inline constexpr std::size_t label_quick_key = 0x147;
inline constexpr std::size_t label_flags = 0x148; // bit 0: shade after drawing

// Type 6 hot surfaces.
inline constexpr std::size_t hot_callback = 0xB6; // slot of GadgetRefs::hot_callback
inline constexpr std::size_t hot_sequence = 0xBE; // slot of GadgetRefs::hot_sequence
inline constexpr std::size_t hot_image = 0xC2;    // slot of GadgetRefs::hot_image
inline constexpr std::size_t hot_flags = 0xC8;    // bit 0: `hotornot`

// Type 7 font resources, type 8 file resources and type 10.
inline constexpr std::size_t filename = 0xB6;
inline constexpr std::size_t filename_bytes = 0x20;
inline constexpr std::size_t nuttin = 0xB6;
inline constexpr std::size_t resource_font = 0xD6; // type 7: slot of GadgetRefs::font
inline constexpr std::size_t resource_file = 0xD6; // type 8: slot of GadgetRefs::file

// Type 6 hot surfaces drawn as images.
inline constexpr size_t hot_frame = 0xC6;

// Type 12 image records and type 13 progress records.
inline constexpr size_t image = 0xB8;            // slot of GadgetRefs::image
inline constexpr size_t image_flags = 0xBC;      // bit 0: shade after drawing
inline constexpr size_t progress_scale = 0xB6;   // ? divisor of the drawn bar
inline constexpr std::size_t flash_level = 0x1F; // type 1 and 12 countdown
inline constexpr std::size_t progress_value = 0xBA;
inline constexpr std::size_t progress_limit = 0xBE;
inline constexpr std::size_t progress_interval = 0xC2;
inline constexpr std::size_t progress_deadline = 0xC6;
// Float step, truncated toward zero and added to the value each interval.
inline constexpr std::size_t progress_step = 0xCA;
inline constexpr std::size_t progress_running = 0xCE;
inline constexpr std::size_t progress_show_value = 0xD2;
} // namespace field

// Type byte values handled by the gadget engine.
namespace gadget_type {
inline constexpr uint8_t panel = 0;
inline constexpr uint8_t button = 1;
inline constexpr uint8_t list_box = 2;
inline constexpr uint8_t text_box = 3;
inline constexpr uint8_t scroll_bar = 4;
inline constexpr uint8_t label = 5;
inline constexpr uint8_t hot_surface = 6;
inline constexpr uint8_t font_resource = 7;
inline constexpr uint8_t file_resource = 8;
inline constexpr uint8_t null_resource = 10;
inline constexpr uint8_t skin = 11; // drawn as its skin sequence (GadgetRefs::skin)
inline constexpr uint8_t image = 12;
inline constexpr uint8_t progress = 13;
} // namespace gadget_type

// Bits of the 32-bit `attribs` word as the engine tests them. Meanings are the
// roles the engine gives them; the authored names are unknown.
namespace attribute {
inline constexpr uint32_t horizontal = 0x1;            // scroll bars; left-aligned text
inline constexpr uint32_t centered = 0x2;              // text alignment; list alnum filter
inline constexpr uint32_t right_aligned = 0x4;         // text alignment; value caption
inline constexpr uint32_t checkbox = 0x8;              // button flips status on release
inline constexpr uint32_t text_list = 0x10;            // list backed by text lines; button holds
inline constexpr uint32_t image_list = 0x20;           // list backed by an image pointer array
inline constexpr uint32_t bottom_caption = 0x20;       // button caption centred along the bottom
inline constexpr uint32_t toggle = 0x40;               // button toggles; list click stays clean
inline constexpr uint32_t image_records = 0x80;        // list backed by 0x18-byte image records
inline constexpr uint32_t cycle_frames = 0x100;        // button cycles frames; list non-selectable
inline constexpr uint32_t skip_headers = 0x200;        // list skips "&G" header lines
inline constexpr uint32_t no_focus = 0x400;            // excluded from keyboard focus
inline constexpr uint32_t item_flags = 0x800;          // list has per-item flag bytes
inline constexpr uint32_t scroll_step_back = 0x1000;   // button steps linked knob -1
inline constexpr uint32_t scroll_step_forward = 0x800; // button steps linked knob +1
inline constexpr uint32_t scroll_step_mask = 0x1800;
inline constexpr uint32_t repeat = 0x2000; // button auto-repeats while held
inline constexpr uint32_t alternate_font = 0x8000;
inline constexpr uint32_t no_quick_key = 0x10000;
} // namespace attribute

using GadgetCallback = void (*)(ui::gui_input::GadgetPanel& panel, int32_t argument);

// Reference-valued record fields. Each member but lines_size has a 32-bit
// slot in the record: the `field` constant described as the member's slot
// gives its offset.
struct GadgetRefs {
    void* surface = nullptr;                 // root record: the panel face
    const void* panel_gaf = nullptr;         // root record: the panel's own GAF file
    const void* skin = nullptr;              // root and type-11 records: skin sequence
    const void* gadget_gaf = nullptr;        // GAF file of a record with its gaf_file bit set
    const void* list_skin = nullptr;         // lists: LISTBOX sequence
    const void* text_skin = nullptr;         // text boxes: TEXTINPUT sequence
    const void* hot_sequence = nullptr;      // type-6 records: image sequence
    const void* hot_image = nullptr;         // type-6 records: single image frame
    const void* file = nullptr;              // type-8 records
    const void* sprite = nullptr;            // buttons: GAF sequence
    const char* lines = nullptr;             // lists: NUL/LF separated rows
    std::size_t lines_size = 0;              // bound for `lines` (host-side)
    const void* images = nullptr;            // lists: image table
    const uint8_t* item_flags = nullptr;     // lists
    const void* font = nullptr;              // type-7 records
    GadgetCallback list_changed = nullptr;   // lists (argument: record index)
    const float* scroll_value = nullptr;     // scroll bars: unread float in scroll_thickness's slot
    GadgetCallback scroll_changed = nullptr; // scroll bars (argument: scroll_callback_argument)
    const void* scroll_ticks = nullptr;      // scroll bars: GAF sequence for scroll art
    GadgetCallback hot_callback = nullptr;   // type-6 records (argument: index)
    const void* image = nullptr;             // type-12 records
};

struct GadgetRecord {
    std::array<uint8_t, kGadgetRecordBytes> bytes{};
    GadgetRefs refs;
};

// Index 0 is the root panel record; its record_count field holds the last used index.
struct GadgetTable {
    std::array<GadgetRecord, kGadgetCapacity> records{};
};

/// Reads an unsigned byte of the record.
[[nodiscard]] inline uint8_t record_u8(const GadgetRecord& record, std::size_t offset) {
    return record.bytes[offset];
}

/// Reads a signed byte of the record.
[[nodiscard]] inline int8_t record_i8(const GadgetRecord& record, std::size_t offset) {
    return static_cast<int8_t>(record.bytes[offset]);
}

/// Reads a little-endian unsigned 16-bit field of the record.
[[nodiscard]] inline uint16_t record_u16(const GadgetRecord& record, std::size_t offset) {
    return static_cast<uint16_t>(record.bytes[offset] | (record.bytes[offset + 1] << 8));
}

/// Reads a little-endian signed 16-bit field of the record.
[[nodiscard]] inline int16_t record_i16(const GadgetRecord& record, std::size_t offset) {
    return static_cast<int16_t>(record_u16(record, offset));
}

/// Reads a little-endian unsigned 32-bit field of the record.
[[nodiscard]] inline uint32_t record_u32(const GadgetRecord& record, std::size_t offset) {
    return static_cast<uint32_t>(record.bytes[offset]) |
           (static_cast<uint32_t>(record.bytes[offset + 1]) << 8) |
           (static_cast<uint32_t>(record.bytes[offset + 2]) << 16) |
           (static_cast<uint32_t>(record.bytes[offset + 3]) << 24);
}

/// Reads a little-endian signed 32-bit field of the record.
[[nodiscard]] inline int32_t record_i32(const GadgetRecord& record, std::size_t offset) {
    return static_cast<int32_t>(record_u32(record, offset));
}

/// Writes an unsigned byte of the record.
inline void set_record_u8(GadgetRecord& record, std::size_t offset, uint8_t value) {
    record.bytes[offset] = value;
}

/// Writes a little-endian unsigned 16-bit field of the record.
inline void set_record_u16(GadgetRecord& record, std::size_t offset, uint16_t value) {
    record.bytes[offset] = static_cast<uint8_t>(value);
    record.bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
}

/// Writes a little-endian signed 16-bit field of the record.
inline void set_record_i16(GadgetRecord& record, std::size_t offset, int16_t value) {
    set_record_u16(record, offset, static_cast<uint16_t>(value));
}

/// Writes a little-endian unsigned 32-bit field of the record.
inline void set_record_u32(GadgetRecord& record, std::size_t offset, uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index)
        record.bytes[offset + index] = static_cast<uint8_t>(value >> (8 * index));
}

/// Writes a little-endian signed 32-bit field of the record.
inline void set_record_i32(GadgetRecord& record, std::size_t offset, int32_t value) {
    set_record_u32(record, offset, static_cast<uint32_t>(value));
}

/// Returns the record bytes at `offset` as characters.
[[nodiscard]] inline char* record_chars(GadgetRecord& record, std::size_t offset) {
    return reinterpret_cast<char*>(record.bytes.data() + offset);
}

/// Returns the record bytes at `offset` as read-only characters.
[[nodiscard]] inline const char* record_chars(const GadgetRecord& record, std::size_t offset) {
    return reinterpret_cast<const char*>(record.bytes.data() + offset);
}

/// Reads a NUL-terminated string stored in the record.
///
/// @param record Record to read.
/// @param offset Byte offset of the string.
/// @return The string, bounded by the end of the record; empty past the end.
[[nodiscard]] std::string_view record_string(const GadgetRecord& record, std::size_t offset);

/// Stores a string in the record with strncpy semantics.
///
/// Copies at most `capacity` bytes and zero-fills the rest of that span; no
/// terminator is written when the value fills it.
///
/// @param[in,out] record Record to write.
/// @param offset Byte offset of the field; ignored past the record end.
/// @param value String to store.
/// @param capacity Field width in bytes, bounded by the record end.
void set_record_string(
    GadgetRecord& record, std::size_t offset, std::string_view value, std::size_t capacity
);

/// Stores a string in the record with strcpy semantics bounded by the record end.
///
/// The value and one NUL are written and later bytes are left untouched.
///
/// @param[in,out] record Record to write.
/// @param offset Byte offset of the field; ignored past the record end.
/// @param value String to store.
void copy_record_cstring(GadgetRecord& record, std::size_t offset, std::string_view value);

/// Returns the record's type byte.
[[nodiscard]] inline uint8_t gadget_type_of(const GadgetRecord& record) {
    return record.bytes[field::type];
}

/// Returns the record's 32-bit attribute word.
[[nodiscard]] inline uint32_t gadget_attributes(const GadgetRecord& record) {
    return record_u32(record, field::attributes);
}

/// Returns the last used index stored in the root record, -1 for an empty table.
[[nodiscard]] inline int16_t gadget_last_index(std::span<const GadgetRecord> table) {
    return table.empty() ? int16_t{-1} : record_i16(table[0], field::record_count);
}

// Inclusive rectangle of four ints.
struct GadgetRect {
    int32_t left = 0;
    int32_t top = 0;
    int32_t right = -1;
    int32_t bottom = -1;
    bool operator==(const GadgetRect&) const = default;
};

/// Reports whether a point lies inside an inclusive rectangle.
[[nodiscard]] constexpr bool rect_contains(const GadgetRect& rect, int32_t x, int32_t y) {
    return rect.left <= x && x <= rect.right && rect.top <= y && y <= rect.bottom;
}

// ---- Record loaders (text GUI already parsed by `parse`) ----

/// Fills the shared COMMON fields of one gadget record.
///
/// @param[in,out] record Record to fill; bytes not named by COMMON are kept.
/// @param common Parsed COMMON section.
/// @quirk The help text is read and then cleared before translation, so the
///        stored help is the translation of the empty string.
void load_gadget_common(GadgetRecord& record, const CommonFields& common);

/// Fills the root panel record's count, default-control names and version.
///
/// @param[in,out] record Root record to fill.
/// @param panel Parsed panel fields; the declared total gadget count is stored.
void load_panel_header(GadgetRecord& record, const PanelFields& panel);

/// Fills a type-1 button's status, caption, quick key, grayed bit and stages.
///
/// @param[in,out] record Button record to fill.
/// @param button Parsed button fields.
void load_button(GadgetRecord& record, const ButtonFields& button);

/// Clears a list box's change callback and item-flag pointer and sets its row height.
///
/// @param[in,out] record List box record to fill.
/// @param list Parsed list box fields.
void load_list_box(GadgetRecord& record, const ListBoxFields& list);

/// Fills a text box's maximum length and text.
///
/// @param[in,out] record Text box record to fill.
/// @param text_box Parsed text box fields; the maximum is already clamped to 128 by the parser.
void load_text_box(GadgetRecord& record, const TextBoxFields& text_box);

/// Fills a scroll bar's range, thickness, knob position and size, and caption, and clears its callback.
///
/// @param[in,out] record Scroll bar record to fill.
/// @param scroll Parsed scroll bar fields.
void load_scroll_bar(GadgetRecord& record, const ScrollBarFields& scroll);

/// Fills a type-5 label's caption and link name, clearing its quick key.
///
/// @param[in,out] record Label record to fill.
/// @param label Parsed label fields; the caption is cut to 127 bytes.
void load_label(GadgetRecord& record, const LabelFields& label);

/// Stores the `hotornot` flag of a type-6 hot surface.
///
/// @param[in,out] record Hot surface record to fill.
/// @param hot Parsed hot surface fields.
void load_hot_surface(GadgetRecord& record, const HotSurfaceFields& hot);

/// Loads every parsed gadget section into consecutive records by type and stores the last index at the root.
///
/// @param layout Parsed layout.
/// @param[in,out] records Record storage; index 0 receives the root panel.
/// @return Number of records written, bounded by the span size.
std::size_t load_gadget_records(const Layout& layout, std::span<GadgetRecord> records);

// ---- Lookup and geometry ----

/// Finds the first record (from 1) whose 16-byte name matches.
///
/// @param table Gadget records; index 0 is the root and is not searched.
/// @param name Name compared with strncmp over 16 bytes.
/// @return The record index, or -1 (kNoGadget) when none matches.
[[nodiscard]] int32_t find_gadget(std::span<const GadgetRecord> table, std::string_view name);

/// Finds the first record (from 1) whose name contains a fragment.
///
/// @param table Gadget records; index 0 is not searched.
/// @param fragment Substring to look for.
/// @return The record index, or -1 (kNoGadget) when none matches.
[[nodiscard]] int32_t
find_gadget_containing(std::span<const GadgetRecord> table, std::string_view fragment);

/// Copies a record's 16-byte name into a 17-byte buffer and terminates it.
///
/// @param table Gadget records.
/// @param index Record whose name is copied.
/// @param[out] output Buffer of at least 17 bytes; zero-filled after the name.
void copy_gadget_name(std::span<const GadgetRecord> table, int32_t index, char* output);

/// Returns the record with a given name.
///
/// @param table Gadget records.
/// @param name Name compared over 16 bytes.
/// @return The record, or nullptr when none matches.
[[nodiscard]] GadgetRecord*
find_gadget_record(std::span<GadgetRecord> table, std::string_view name);

// Receives the message of a fatal GUI layout error.
using FatalReporter = void (*)(const char* message);

/// Returns the record with a given name, reporting a GUI layout error when absent.
///
/// The reporter decides whether a miss ends the game; in 3.1c it always does.
///
/// @param table Gadget records.
/// @param name Name compared over 16 bytes.
/// @param report_fatal Receives "Error in GUI layout" on a miss and decides
///        whether it is fatal; may be null.
/// @return The record, or nullptr when none matches.
[[nodiscard]] GadgetRecord*
require_gadget(std::span<GadgetRecord> table, std::string_view name, FatalReporter report_fatal);

/// Compares the 16-byte name of one record with a name.
///
/// @param table Gadget records.
/// @param index Record to compare; -1 never matches.
/// @param name Name compared with strncmp over 16 bytes.
/// @return True when the names match.
[[nodiscard]] bool
gadget_name_matches(std::span<const GadgetRecord> table, int32_t index, std::string_view name);

/// Returns a record's inclusive panel-relative rectangle.
///
/// @param record Gadget record; a type-0 record uses origin (0,0).
/// @return The rectangle.
[[nodiscard]] GadgetRect panel_relative_rect(const GadgetRecord& record);

/// Returns a record's inclusive screen rectangle.
///
/// @param table Gadget records; index 0 supplies the panel origin.
/// @param index Record to measure; nonzero types add the root record's origin.
/// @return The rectangle.
[[nodiscard]] GadgetRect screen_rect(std::span<const GadgetRecord> table, std::size_t index);

/// Computes a scroll bar's track and knob rectangles in panel coordinates.
///
/// @param record Scroll bar record.
/// @param[out] outer Track rectangle with exclusive right and bottom edges.
/// @param[out] knob Knob rectangle along the bar's axis, inset by one pixel
///             across it and offset two pixels along a vertical bar.
void scroll_bar_rects(const GadgetRecord& record, GadgetRect& outer, GadgetRect& knob);

/// Finds the first record of a type sharing another record's group byte.
///
/// @param table Gadget records.
/// @param index Record whose group byte is matched.
/// @param type Type byte wanted (2 or 4).
/// @return The record index, or 0 when none matches.
/// @quirk The miss value is 0, not -1; callers that test for -1 then act on the root.
[[nodiscard]] int32_t
find_group_member(std::span<const GadgetRecord> table, int32_t index, uint8_t type);

/// Finds a list box's skin record.
///
/// @param table Gadget records (unused).
/// @param index List box record (unused).
/// @return Always 0, so the flat skin table is used.
/// @quirk Every path returns 0, although a type-8 record may carry a
///        matching texture number.
[[nodiscard]] int32_t find_list_skin_record(std::span<const GadgetRecord> table, int32_t index);

// ---- Record construction ----

/// Appends a zeroed, active record of a type.
///
/// @param[in,out] table Gadget records; the root's last index grows by one.
/// @param type Type byte of the new record.
/// @return The new index, or -1 when the table is full.
int32_t add_gadget(std::span<GadgetRecord> table, uint8_t type);

/// Appends a type-5 label with a fixed 15-pixel height and colour 15.
///
/// @param[in,out] table Gadget records; the root's last index grows by one.
/// @param name Gadget name, cut to 16 bytes.
/// @param text Caption, cut to 127 bytes.
/// @param x Left edge relative to the panel, in pixels.
/// @param y Top edge relative to the panel, in pixels.
/// @param width Width in pixels; -1 extends to five pixels short of the root's right edge.
/// @param attributes Attribute word of the label.
/// @return The new index, or -1 when the table is full.
/// @quirk Bytes not named here keep whatever the slot held before.
int32_t add_label(
    std::span<GadgetRecord> table,
    std::string_view name,
    std::string_view text,
    int16_t x,
    int16_t y,
    int32_t width,
    uint32_t attributes
);

/// Appends a copy of the first 0x13E bytes of a record as a type-1 button.
///
/// A negative record count, or a table whose storage span is full, refuses
/// the copy too.
///
/// @param[in,out] table Gadget records.
/// @param source Record to copy.
/// @return False when the table is full.
/// @quirk The count check refuses a copy at exactly 200 records.
bool append_button_copy(std::span<GadgetRecord> table, const GadgetRecord& source);

/// Appends a copy of the first 0xCC bytes of a record as a reset type-6 record.
///
/// @param[in,out] table Gadget records.
/// @param source Record to copy.
/// @return False when the table is full.
bool append_hot_surface_copy(std::span<GadgetRecord> table, const GadgetRecord& source);

/// Appends a copy of the first 0xD6 bytes of a record as a type-13 progress record.
///
/// @param[in,out] table Gadget records.
/// @param source Record to copy.
/// @return False when the table is full.
bool append_progress_copy(std::span<GadgetRecord> table, const GadgetRecord& source);

/// Clears the callback, image sequence, image and frame of a type-6 record.
///
/// The list references that share those slots are cleared too.
///
/// @param[in,out] record Hot surface record.
void reset_hot_surface(GadgetRecord& record);

} // namespace oa::ui::gui_layout
