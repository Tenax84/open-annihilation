// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/ui/gui_layout.hpp"

#include "oa/base/text.hpp"
#include "oa/data/languages/translation.hpp"
#include "oa/ui/gui_layout/gui_gadget.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace oa::ui::gui_layout {
namespace {

struct Section {
    std::string name;
    std::unordered_map<std::string, std::string> fields;
    std::vector<Section> children;
};

[[nodiscard]] std::string lower_ascii(std::string_view input) {
    std::string output(input);
    for (auto& character : output) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return output;
}

[[nodiscard]] std::string_view trim(std::string_view input) noexcept {
    while (!input.empty() &&
           (input.front() == ' ' || input.front() == '\t' || input.front() == '\r')) {
        input.remove_prefix(1);
    }
    while (!input.empty() &&
           (input.back() == ' ' || input.back() == '\t' || input.back() == '\r')) {
        input.remove_suffix(1);
    }
    return input;
}

class TextParser {
  public:

    explicit TextParser(std::string_view input) : input_(input) {}

    [[nodiscard]] bool parse_document(std::vector<Section>& sections, Error& error) {
        skip_space_and_comments();
        while (position_ < input_.size()) {
            if (sections.size() >= limit::gadgets) {
                error = make_error(
                    ErrorCode::gadget_limit, "GUI contains more than 200 gadget records"
                );
                return false;
            }
            Section section;
            if (!parse_section(section, 1, error))
                return false;
            sections.push_back(std::move(section));
            skip_space_and_comments();
        }
        return true;
    }

  private:

    std::string_view input_;
    std::size_t position_ = 0;
    std::size_t section_count_ = 0;

    [[nodiscard]] Error make_error(ErrorCode code, std::string message) const {
        return Error{code, position_, std::move(message)};
    }

    void skip_line_comment() noexcept {
        position_ += 2;
        while (position_ < input_.size() && input_[position_] != '\n')
            ++position_;
    }

    void skip_block_comment() noexcept {
        position_ += 2;
        while (position_ + 1 < input_.size() &&
               !(input_[position_] == '*' && input_[position_ + 1] == '/')) {
            ++position_;
        }
        if (position_ + 1 < input_.size())
            position_ += 2;
    }

    void skip_space_and_comments() noexcept {
        for (;;) {
            while (position_ < input_.size() &&
                   std::isspace(static_cast<unsigned char>(input_[position_])) != 0) {
                ++position_;
            }
            if (position_ + 1 < input_.size() && input_[position_] == '/' &&
                input_[position_ + 1] == '/') {
                skip_line_comment();
            } else if (
                position_ + 1 < input_.size() && input_[position_] == '/' &&
                input_[position_ + 1] == '*'
            ) {
                skip_block_comment();
            } else {
                return;
            }
        }
    }

    void skip_horizontal_space() noexcept {
        while (
            position_ < input_.size() &&
            (input_[position_] == ' ' || input_[position_] == '\t' || input_[position_] == '\r')) {
            ++position_;
        }
    }

    [[nodiscard]] bool parse_section(Section& section, std::size_t depth, Error& error) {
        if (depth > limit::nesting_depth) {
            error = make_error(ErrorCode::nesting_limit, "GUI section nesting exceeds limit");
            return false;
        }
        if (section_count_ >= limit::sections) {
            error = make_error(ErrorCode::section_limit, "GUI contains too many total sections");
            return false;
        }
        ++section_count_;
        if (position_ >= input_.size() || input_[position_] != '[') {
            error = make_error(ErrorCode::malformed_syntax, "expected '[' before GUI section");
            return false;
        }
        const auto name_begin = ++position_;
        while (position_ < input_.size() && input_[position_] != ']')
            ++position_;
        if (position_ == input_.size()) {
            error = make_error(ErrorCode::malformed_syntax, "unterminated GUI section name");
            return false;
        }
        const auto section_name = trim(input_.substr(name_begin, position_ - name_begin));
        if (section_name.size() > limit::section_name_bytes) {
            error = make_error(ErrorCode::field_limit, "GUI section name exceeds limit");
            return false;
        }
        section.name = std::string(section_name);
        ++position_;
        skip_space_and_comments();
        if (position_ >= input_.size() || input_[position_] != '{') {
            error = make_error(ErrorCode::malformed_syntax, "expected '{' after GUI section name");
            return false;
        }
        ++position_;

        for (;;) {
            skip_space_and_comments();
            if (position_ >= input_.size()) {
                // Shipped SCORE.GUI ends immediately after its last assignment
                // without the final top-level `}`. 3.1c still loads that last
                // gadget.
                if (depth == 1)
                    return true;
                error = make_error(ErrorCode::malformed_syntax, "unterminated GUI section body");
                return false;
            }
            if (input_[position_] == '}') {
                ++position_;
                return true;
            }
            if (input_[position_] == '[') {
                Section child;
                if (!parse_section(child, depth + 1, error))
                    return false;
                section.children.push_back(std::move(child));
                continue;
            }
            if (section.fields.size() >= limit::fields_per_section) {
                error = make_error(ErrorCode::field_limit, "GUI section contains too many fields");
                return false;
            }

            const auto key_begin = position_;
            while (position_ < input_.size() && input_[position_] != '=' &&
                   input_[position_] != '\n' && input_[position_] != '{' &&
                   input_[position_] != '}') {
                ++position_;
            }
            if (position_ == input_.size() || input_[position_] != '=') {
                error =
                    make_error(ErrorCode::malformed_syntax, "expected '=' after GUI field name");
                return false;
            }
            const auto key = lower_ascii(trim(input_.substr(key_begin, position_ - key_begin)));
            if (key.empty()) {
                error = make_error(ErrorCode::malformed_syntax, "empty GUI field name");
                return false;
            }
            if (key.size() > limit::field_name_bytes) {
                error = make_error(ErrorCode::field_limit, "GUI field name exceeds limit");
                return false;
            }
            ++position_;
            skip_horizontal_space();
            const auto value_begin = position_;
            while (position_ < input_.size() && input_[position_] != ';' &&
                   input_[position_] != '\n' && input_[position_] != '}') {
                if (position_ + 1 < input_.size() && input_[position_] == '/' &&
                    (input_[position_ + 1] == '/' || input_[position_ + 1] == '*')) {
                    break;
                }
                ++position_;
            }
            const auto source_value = trim(input_.substr(value_begin, position_ - value_begin));
            if (source_value.size() > limit::source_value_bytes) {
                error = make_error(ErrorCode::field_limit, "GUI source field exceeds limit");
                return false;
            }
            const auto value = std::string(source_value);
            section.fields.insert_or_assign(key, value);
            if (position_ < input_.size() && input_[position_] == ';')
                ++position_;
            if (position_ + 1 < input_.size() && input_[position_] == '/' &&
                input_[position_ + 1] == '/') {
                skip_line_comment();
            } else if (
                position_ + 1 < input_.size() && input_[position_] == '/' &&
                input_[position_ + 1] == '*'
            ) {
                skip_block_comment();
            }
            // A closing brace that terminated an empty/unterminated-semicolon
            // value belongs to the surrounding section and is consumed above.
        }
    }
};

[[nodiscard]] const Section* child_named(const Section& section, std::string_view name) {
    const auto wanted = lower_ascii(name);
    for (const auto& child : section.children) {
        if (lower_ascii(child.name) == wanted)
            return &child;
    }
    return nullptr;
}

[[nodiscard]] std::string field(const Section& section, std::string_view key) {
    const auto found = section.fields.find(lower_ascii(key));
    return found == section.fields.end() ? std::string{} : found->second;
}

[[nodiscard]] bool
parse_integer(const Section& section, std::string_view key, int32_t& value, Error& error) {
    const auto found = section.fields.find(lower_ascii(key));
    if (found == section.fields.end() || found->second.empty()) {
        value = 0;
        return true;
    }
    const auto text = trim(found->second);
    int64_t wide = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), wide, 10);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        wide < std::numeric_limits<int32_t>::min() || wide > std::numeric_limits<int32_t>::max()) {
        error = {
            ErrorCode::invalid_integer,
            0,
            "GUI field '" + std::string(key) + "' is not a signed 32-bit integer"
        };
        return false;
    }
    value = static_cast<int32_t>(wide);
    return true;
}

[[nodiscard]] constexpr int8_t low_i8(int32_t value) noexcept {
    return std::bit_cast<int8_t>(static_cast<uint8_t>(static_cast<uint32_t>(value) & 0xffU));
}

[[nodiscard]] constexpr int16_t low_i16(int32_t value) noexcept {
    return std::bit_cast<int16_t>(static_cast<uint16_t>(static_cast<uint32_t>(value) & 0xffffU));
}

[[nodiscard]] std::string bounded_string(std::string value, std::size_t length) {
    if (value.size() > length)
        value.resize(length);
    return value;
}

[[nodiscard]] bool translate(
    const std::string& source,
    const TranslationLookup& lookup,
    std::string& destination,
    Error& error
) {
    destination = source;
    if (lookup) {
        if (auto replacement = lookup(source))
            destination = std::move(*replacement);
    }
    if (destination.size() > limit::text_bytes) {
        error = {
            ErrorCode::translated_text_limit,
            0,
            "translated GUI text exceeds the bounded 127-byte record field"
        };
        return false;
    }
    return true;
}

[[nodiscard]] bool common_fields(
    const Section& section,
    CommonFields& output,
    const TranslationLookup& translation_lookup,
    Error& error
) {
    const auto* common = child_named(section, "common");
    if (common == nullptr) {
        error = {ErrorCode::malformed_syntax, 0, "GUI gadget lacks a COMMON subsection"};
        return false;
    }
    int32_t value = 0;
    if (!parse_integer(*common, "id", value, error))
        return false;
    output.type = static_cast<GadgetType>(static_cast<uint8_t>(value));
    if (!parse_integer(*common, "assoc", value, error))
        return false;
    output.association = static_cast<uint8_t>(value);
    output.name = bounded_string(field(*common, "name"), limit::name_bytes);
    if (!parse_integer(*common, "xpos", value, error))
        return false;
    output.x = low_i16(value);
    if (!parse_integer(*common, "ypos", value, error))
        return false;
    output.y = low_i16(value);
    if (!parse_integer(*common, "width", value, error))
        return false;
    output.width = low_i16(value);
    if (!parse_integer(*common, "height", value, error))
        return false;
    output.height = low_i16(value);
    if (!parse_integer(*common, "attribs", output.attributes, error))
        return false;
    if (!parse_integer(*common, "colorf", value, error))
        return false;
    output.foreground_color = static_cast<uint16_t>(value);
    if (!parse_integer(*common, "colorb", value, error))
        return false;
    output.background_color = static_cast<uint16_t>(value);
    if (!parse_integer(*common, "texturenumber", value, error))
        return false;
    output.texture_number = low_i8(value);
    if (!parse_integer(*common, "fontnumber", value, error))
        return false;
    output.font_number = low_i8(value);
    if (!parse_integer(*common, "active", value, error))
        return false;
    output.active = low_i8(value);
    if (!parse_integer(*common, "commonattribs", value, error))
        return false;
    output.common_attributes = low_i8(value);
    if (!parse_integer(*common, "gaffile", value, error))
        return false;
    output.gaf_file = (static_cast<uint32_t>(value) & 1U) != 0;
    output.source_help = bounded_string(field(*common, "help"), limit::help_bytes);
    // The game clears the destination before translating it, so the
    // translation still performs a lookup for the empty string.
    if (!translate({}, translation_lookup, output.runtime_help, error))
        return false;
    return true;
}

[[nodiscard]] bool type_fields(
    const Section& section,
    Gadget& gadget,
    const TranslationLookup& translation_lookup,
    Error& error
) {
    int32_t value = 0;
    switch (gadget.common.type) {
    case GadgetType::panel: {
        PanelFields panel;
        if (!parse_integer(section, "totalgadgets", value, error))
            return false;
        panel.declared_total_gadgets = low_i16(value);
        panel.panel = bounded_string(field(section, "panel"), limit::panel_name_bytes);
        panel.carriage_return_default =
            bounded_string(field(section, "crdefault"), limit::panel_name_bytes);
        panel.escape_default =
            bounded_string(field(section, "escdefault"), limit::panel_name_bytes);
        panel.default_focus =
            bounded_string(field(section, "defaultfocus"), limit::panel_name_bytes);
        if (const auto* version = child_named(section, "version")) {
            if (!parse_integer(*version, "major", value, error))
                return false;
            panel.version.major = low_i8(value);
            if (!parse_integer(*version, "minor", value, error))
                return false;
            panel.version.minor = low_i8(value);
            if (!parse_integer(*version, "revision", value, error))
                return false;
            panel.version.revision = low_i8(value);
        }
        gadget.fields = std::move(panel);
        return true;
    }
    case GadgetType::button: {
        ButtonFields button;
        if (!parse_integer(section, "status", value, error))
            return false;
        button.status = low_i16(value);
        button.source_text = bounded_string(field(section, "text"), limit::text_bytes);
        if (!translate(button.source_text, translation_lookup, button.text, error))
            return false;
        const auto quick_key =
            bounded_string(field(section, "quickkey"), limit::quick_key_source_bytes);
        if (!quick_key.empty() && ((quick_key.front() >= 'A' && quick_key.front() <= 'Z') ||
                                   (quick_key.front() >= 'a' && quick_key.front() <= 'z'))) {
            button.quick_key = std::bit_cast<int8_t>(static_cast<uint8_t>(quick_key.front()));
        } else {
            Section quick_key_section;
            quick_key_section.fields.emplace("quickkey", quick_key);
            if (!parse_integer(quick_key_section, "quickkey", value, error))
                return false;
            button.quick_key = low_i8(value);
        }
        if (!parse_integer(section, "grayedout", value, error))
            return false;
        button.grayed_out = (static_cast<uint32_t>(value) & 1U) != 0;
        if (!parse_integer(section, "stages", value, error))
            return false;
        button.stages = low_i8(value);
        gadget.fields = std::move(button);
        return true;
    }
    case GadgetType::list_box: {
        ListBoxFields list_box;
        if (!parse_integer(section, "itemheight", value, error))
            return false;
        list_box.item_height = low_i16(value);
        gadget.fields = list_box;
        return true;
    }
    case GadgetType::text_box: {
        TextBoxFields text_box;
        if (!parse_integer(section, "maxchars", value, error))
            return false;
        text_box.max_characters = low_i16(value);
        if (text_box.max_characters > limit::text_box_max_characters) {
            text_box.max_characters = limit::text_box_max_characters;
        }
        text_box.source_text = bounded_string(field(section, "text"), limit::text_bytes);
        if (!translate(text_box.source_text, translation_lookup, text_box.text, error))
            return false;
        gadget.fields = std::move(text_box);
        return true;
    }
    case GadgetType::scroll_bar: {
        ScrollBarFields scroll_bar;
        if (!parse_integer(section, "range", value, error))
            return false;
        scroll_bar.range = low_i16(value);
        if (!parse_integer(section, "thick", value, error))
            return false;
        scroll_bar.thickness = low_i16(value);
        if (!parse_integer(section, "knobpos", value, error))
            return false;
        scroll_bar.knob_position = low_i16(value);
        if (!parse_integer(section, "knobsize", value, error))
            return false;
        scroll_bar.knob_size = low_i16(value);
        scroll_bar.source_text = bounded_string(field(section, "text"), limit::text_bytes);
        if (!translate(scroll_bar.source_text, translation_lookup, scroll_bar.text, error))
            return false;
        gadget.fields = std::move(scroll_bar);
        return true;
    }
    case GadgetType::label: {
        LabelFields label;
        label.source_text = bounded_string(field(section, "text"), limit::text_bytes);
        if (!translate(label.source_text, translation_lookup, label.text, error))
            return false;
        label.link = bounded_string(field(section, "link"), limit::link_bytes);
        gadget.fields = std::move(label);
        return true;
    }
    case GadgetType::hot_surface: {
        HotSurfaceFields surface;
        if (!parse_integer(section, "hotornot", value, error))
            return false;
        surface.hot = (static_cast<uint32_t>(value) & 1U) != 0;
        gadget.fields = surface;
        return true;
    }
    case GadgetType::font_resource:
    case GadgetType::file_resource:
        gadget.fields =
            FileResourceFields{bounded_string(field(section, "filename"), limit::filename_bytes)};
        return true;
    case GadgetType::null_resource: {
        NullResourceFields resource;
        if (!parse_integer(section, "nuttin", resource.nuttin, error))
            return false;
        gadget.fields = resource;
        return true;
    }
    case GadgetType::unhandled:
    case GadgetType::image:
    default:
        gadget.fields = CommonOnlyFields{};
        return true;
    }
}

[[nodiscard]] ParseResult failure(ErrorCode code, std::size_t offset, std::string message) {
    return {std::nullopt, Error{code, offset, std::move(message)}};
}

} // namespace

ParseResult parse(std::span<const uint8_t> bytes, const TranslationLookup& translation_lookup) {
    if (bytes.size() > limit::input_bytes) {
        return failure(ErrorCode::input_limit, 0, "GUI input exceeds the 4 MiB parser limit");
    }
    if (std::find(bytes.begin(), bytes.end(), uint8_t{0}) != bytes.end()) {
        return failure(
            ErrorCode::binary_format,
            0,
            "compiled/binary GUI data is not handled by the text GUI loader"
        );
    }
    const std::string_view input(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    std::vector<Section> sections;
    Error error;
    TextParser parser(input);
    if (!parser.parse_document(sections, error)) {
        return {std::nullopt, std::move(error)};
    }
    if (sections.empty()) {
        return failure(ErrorCode::invalid_root, 0, "GUI has no gadget records");
    }

    Layout layout;
    layout.gadgets.reserve(sections.size());
    for (const auto& section : sections) {
        Gadget gadget;
        if (!common_fields(section, gadget.common, translation_lookup, error) ||
            !type_fields(section, gadget, translation_lookup, error)) {
            return {std::nullopt, std::move(error)};
        }
        layout.gadgets.push_back(std::move(gadget));
    }
    if (layout.gadgets.front().common.type != GadgetType::panel ||
        !std::holds_alternative<PanelFields>(layout.gadgets.front().fields)) {
        return failure(
            ErrorCode::invalid_root, 0, "first GUI record is not the required panel gadget"
        );
    }
    auto& panel = std::get<PanelFields>(layout.gadgets.front().fields);
    panel.loaded_total_gadgets = static_cast<int16_t>(layout.gadgets.size() - 1);
    return {std::move(layout), std::nullopt};
}

std::optional<Rect> gadget_rectangle(std::span<const Gadget> gadgets, std::size_t index) noexcept {
    if (index >= gadgets.size())
        return std::nullopt;
    const auto& record = gadgets[index].common;
    auto left = static_cast<int32_t>(record.x);
    auto top = static_cast<int32_t>(record.y);
    // The root record's stored origin is added only when this type byte is
    // nonzero. Index 0 with a nonzero type adds that origin twice.
    if (record.type != GadgetType::panel) {
        const auto& root = gadgets.front().common;
        left += static_cast<int32_t>(root.x);
        top += static_cast<int32_t>(root.y);
    }
    return Rect{
        left,
        top,
        left + static_cast<int32_t>(record.width) - 1,
        top + static_cast<int32_t>(record.height) - 1,
    };
}

std::optional<CaptionPen> button_caption_pen(
    const Rect& button, uint32_t attributes, int32_t text_width, int32_t line_height, bool pressed
) noexcept {
    // A caption starts this far in from the edge it is aligned to; a bottom
    // caption's line ends this many rows above the bottom edge.
    constexpr int32_t edge_inset = 3;
    constexpr int32_t bottom_gap = 4;
    const int32_t press = pressed ? 1 : 0;
    const int32_t span_x = button.right - button.left;
    const int32_t centred_x = button.left + (span_x - text_width) / 2 + press + 1;
    const int32_t centred_y = button.top + (button.bottom - button.top - line_height) / 2 + press;
    if ((attributes & attribute::horizontal) != 0)
        return CaptionPen{button.left + press + edge_inset, centred_y};
    if ((attributes & attribute::right_aligned) != 0)
        return CaptionPen{std::max(button.right - text_width - edge_inset, button.left), centred_y};
    if ((attributes & attribute::centered) != 0)
        return CaptionPen{centred_x, centred_y};
    if ((attributes & attribute::bottom_caption) != 0)
        return CaptionPen{centred_x, button.bottom + press - line_height - bottom_gap};
    return std::nullopt;
}

std::vector<SkinTile>
skin_tiles(int32_t width, int32_t height, int32_t tile_width, int32_t tile_height) {
    std::vector<SkinTile> tiles;
    if (height < 1 || tile_width < 1 || tile_height < 1)
        return tiles;
    int32_t y = 0;
    do {
        std::size_t row = skin_frame::top_row;
        if (y != 0)
            row = height - tile_height + 1 <= y ? skin_frame::bottom_row : skin_frame::middle_row;
        if (height < y + tile_height)
            y = height - tile_height;
        int32_t x = 0;
        while (x < width) {
            std::size_t column = skin_frame::left_column;
            if (x + tile_width < width) {
                if (x != 0)
                    column = skin_frame::middle_column;
            } else {
                x = width - tile_width;
                column = skin_frame::right_column;
            }
            tiles.push_back({row + column, x, y});
            x += tile_width;
        }
        y += tile_height;
    } while (y < height);
    return tiles;
}

TranslationLookup game_translation_lookup() {
    return [](std::string_view source) -> std::optional<std::string> {
        auto translated = oa::data::languages::translation_of(source);
        // Cut between whole UTF-8 characters, as a language whose text is
        // UTF-8 needs.
        if (translated && translated->size() > limit::text_bytes)
            translated->resize(oa::base::text::whole_characters(*translated, limit::text_bytes));
        return translated;
    };
}

bool keyboard_focusable(
    int8_t active,
    uint32_t attributes,
    uint8_t type,
    bool button_grayed,
    bool scroll_locked,
    int32_t width,
    int32_t height
) noexcept {
    if (active == 0 || (attributes & attribute::no_focus) != 0)
        return false;
    switch (type) {
    case static_cast<uint8_t>(GadgetType::button):
        return !button_grayed;
    case static_cast<uint8_t>(GadgetType::text_box):
    case static_cast<uint8_t>(GadgetType::hot_surface):
        return true;
    case static_cast<uint8_t>(GadgetType::scroll_bar):
        if (width < height || scroll_locked)
            return false;
        return true;
    case static_cast<uint8_t>(GadgetType::list_box):
        return (attributes & attribute::cycle_frames) == 0;
    default:
        return false;
    }
}

bool keyboard_focusable(const Gadget& gadget) noexcept {
    bool button_grayed = false;
    bool scroll_locked = false;
    if (const auto* button = std::get_if<ButtonFields>(&gadget.fields))
        button_grayed = button->grayed_out;
    if (const auto* bar = std::get_if<ScrollBarFields>(&gadget.fields))
        scroll_locked = bar->locked;
    return keyboard_focusable(
        gadget.common.active,
        static_cast<uint32_t>(gadget.common.attributes),
        static_cast<uint8_t>(gadget.common.type),
        button_grayed,
        scroll_locked,
        gadget.common.width,
        gadget.common.height
    );
}

} // namespace oa::ui::gui_layout
