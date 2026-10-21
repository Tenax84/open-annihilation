// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// What the mutator picker's screen tests share: the made-up Ridge set
// (Fast Builders, Scout Post, Gatehouse, Gatehouse Keep, Ridge Walls and
// Grounded), the synthetic font those layouts are measured in, and the
// checks a layout's parts are held to.
#pragma once

#include "oa/formats/fnt.hpp"
#include "oa/ui/frontend_renderer.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/controls.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/looks.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/library/library.hpp"
#include "oa/ui/mutator_picker/model.hpp"
#include "oa/ui/mutator_picker/screen.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace picker_fixture {

namespace kit = oa::ui::kit;
namespace picker = oa::ui::mutator_picker;
namespace lib = oa::ui::library;

/// The synthetic font of the kit's tests, for both roles. A glyph for byte
/// b is 2 + b % 4 columns by 8 rows.
///
/// @return the fonts
inline kit::Fonts block_fonts() {
    oa::ui::frontend_renderer::TextFont font;
    font.font.nominal_height = 8;
    constexpr uint8_t ink_index = 1;
    for (int byte = 0; byte < 256; ++byte) {
        const auto width = static_cast<uint16_t>(2 + byte % 4);
        constexpr uint16_t height = 8;
        const auto count = static_cast<std::size_t>(width) * height;
        std::vector<uint8_t> pixels(count, ink_index);
        std::vector<uint8_t> coverage(count, 1);
        font.font.glyphs[static_cast<std::size_t>(byte)] =
            oa::formats::fnt::Glyph{width, height, 0, 0, std::move(pixels), std::move(coverage)};
    }
    font.ink[ink_index] = static_cast<uint16_t>(oa::ui::frontend_renderer::blend_opaque);
    return {font, font, {}, {}};
}

/// The fonts every layout here is measured in.
///
/// @return the fonts
inline const kit::Fonts& fonts() {
    static const kit::Fonts loaded = block_fonts();
    return loaded;
}

/// Returns the part of an item that is drawn: its rectangle, within its clip
/// when it has one, and a focus ring's outline round its control.
///
/// @param item the item
/// @return the rectangle
inline kit::Rect drawn_area(const kit::Item& item) {
    kit::Rect area = item.rect;
    if (item.role == kit::Role::focus_ring) {
        const auto* look = std::get_if<kit::FocusRingLook>(&item.look);
        if (look != nullptr && look->around)
            area = kit::grown(area, kit::compact_metrics.focus_inset);
    }
    if (item.clip.width > 0 && item.clip.height > 0)
        area = kit::intersect(area, item.clip);
    return area;
}

/// Returns the part of a control a press reaches.
///
/// @param control the control
/// @return its rectangle, within its clip when it has one
inline kit::Rect press_area(const kit::Control& control) {
    if (control.clip.width > 0 && control.clip.height > 0)
        return kit::intersect(control.rect, control.clip);
    return control.rect;
}

/// Tells whether a rectangle holds any point.
///
/// @param rect the rectangle
/// @return true when it is wide and high
inline bool has_room(const kit::Rect& rect) {
    return rect.width > 0 && rect.height > 0;
}

/// Tells whether two rectangles are the same place.
///
/// @param left one rectangle
/// @param right the other
/// @return true when each edge matches
inline bool same_rect(const kit::Rect& left, const kit::Rect& right) {
    return left.x == right.x && left.y == right.y && left.width == right.width &&
           left.height == right.height;
}

/// Lists the parts of a layout that lie outside its window.
///
/// @param list the display list
/// @param window the window's size
/// @return a line for each part outside; none when all lie inside
inline std::vector<std::string> outside_window(const kit::DisplayList& list, kit::Point window) {
    const kit::Rect whole{0, 0, window.x, window.y};
    std::vector<std::string> problems;
    for (const kit::Item& item : list.items) {
        const kit::Rect area = drawn_area(item);
        if (has_room(area) && !kit::wholly_in(area, whole))
            problems.push_back(
                "item " + std::to_string(static_cast<int>(item.role)) + " \"" + item.text + "\""
            );
    }
    for (const kit::Control& control : list.controls) {
        const kit::Rect area = press_area(control);
        if (has_room(area) && !kit::wholly_in(area, whole))
            problems.push_back("control " + control.name);
    }
    return problems;
}

/// Lists the texts of a layout that do not fit their rectangles by the kit's
/// measure. Buttons, chips, pills, drop-downs, menu items, the header, a
/// heading and an empty field's placeholder are measured. A link, a tab
/// strip and a nav list are not on this screen.
///
/// @param list the display list
/// @param measured the fonts the layout measured in
/// @return a line for each text that does not fit; none when all fit
inline std::vector<std::string>
texts_not_fitting(const kit::DisplayList& list, const kit::Fonts& measured) {
    const kit::Metrics& metrics = kit::compact_metrics;
    std::vector<std::string> problems;
    const auto check = [&](bool fits, const std::string& what) {
        if (!fits)
            problems.push_back(what);
    };
    const auto width = [&measured](kit::FontRole role, std::string_view text) {
        return kit::text_width(measured, role, text);
    };
    for (const kit::Item& item : list.items) {
        switch (item.role) {
        case kit::Role::text:
            check(width(item.font, item.text) <= item.rect.width, "text \"" + item.text + "\"");
            break;
        case kit::Role::button:
            if (const auto* look = std::get_if<kit::ButtonLook>(&item.look))
                check(
                    width(kit::FontRole::small, look->caption) <= item.rect.width,
                    "button \"" + look->caption + "\""
                );
            break;
        case kit::Role::chip:
            if (const auto* look = std::get_if<kit::ChipLook>(&item.look))
                check(
                    kit::chip_width(measured, *look) <= item.rect.width,
                    "chip \"" + look->text + "\""
                );
            break;
        case kit::Role::pill:
            if (const auto* look = std::get_if<kit::PillLook>(&item.look))
                check(
                    kit::pill_width(measured, *look) <= item.rect.width,
                    "pill \"" + look->text + "\""
                );
            break;
        case kit::Role::choice:
            if (const auto* look = std::get_if<kit::ChoiceLook>(&item.look))
                check(
                    width(kit::FontRole::regular, look->text) <=
                        item.rect.width - metrics.choice_text_inset - metrics.choice_arrow_room,
                    "drop-down \"" + look->text + "\""
                );
            break;
        case kit::Role::choice_menu:
            if (const auto* look = std::get_if<kit::ChoiceMenuLook>(&item.look))
                for (std::size_t place = 0; place < look->shown.size(); ++place) {
                    const kit::Rect box = kit::choice_item(item.rect, static_cast<int32_t>(place));
                    check(
                        width(kit::FontRole::regular, look->shown[place]) <=
                            box.width - metrics.choice_item_text_inset,
                        "menu item \"" + look->shown[place] + "\""
                    );
                }
            break;
        case kit::Role::header:
            if (const auto* look = std::get_if<kit::HeaderLook>(&item.look)) {
                check(
                    kit::tracked_width(
                        measured, kit::FontRole::regular, look->title, look->tracking
                    ) <= look->title_width,
                    "header title \"" + look->title + "\""
                );
                check(
                    kit::tracked_width(
                        measured, kit::FontRole::regular, look->second, look->tracking
                    ) <= look->second_width,
                    "header word \"" + look->second + "\""
                );
                check(
                    width(kit::FontRole::small, look->version) <= look->version_width,
                    "header version \"" + look->version + "\""
                );
                const int32_t title_left = metrics.padding + metrics.mark_side + metrics.header_gap;
                const int32_t words_right =
                    title_left + look->title_width +
                    (look->second.empty() ? 0 : metrics.header_gap + look->second_width);
                const int32_t version_left =
                    look->width - metrics.edge - metrics.padding - look->version_width;
                check(words_right <= version_left, "header words reach the version");
            }
            break;
        case kit::Role::heading:
            if (const auto* look = std::get_if<kit::HeadingLook>(&item.look))
                check(
                    kit::tracked_width(
                        measured, kit::FontRole::small, look->text, metrics.heading_tracking
                    ) <= item.rect.width,
                    "heading \"" + look->text + "\""
                );
            break;
        case kit::Role::field:
            if (const auto* look = std::get_if<kit::SearchLook>(&item.look);
                look != nullptr && look->field.text.empty())
                check(
                    width(kit::FontRole::regular, look->placeholder) <=
                        kit::field_text_rect(item.rect, *look).width,
                    "placeholder \"" + look->placeholder + "\""
                );
            break;
        case kit::Role::toggle:
            if (const auto* look = std::get_if<kit::SwitchLook>(&item.look)) {
                const int32_t half = std::max((item.rect.width - 2) / 2, int32_t{0});
                check(
                    width(kit::FontRole::small, look->off_caption) <= half,
                    "switch off \"" + look->off_caption + "\""
                );
                check(
                    width(kit::FontRole::small, look->on_caption) <= half,
                    "switch on \"" + look->on_caption + "\""
                );
            }
            break;
        case kit::Role::levels:
            if (const auto* look = std::get_if<kit::LevelsLook>(&item.look)) {
                for (const std::string& caption : look->captions) {
                    check(
                        width(kit::FontRole::small, caption) <= look->level_width,
                        "level \"" + caption + "\""
                    );
                }
            }
            break;
        default:
            break;
        }
    }
    return problems;
}

/// A made-up mutator that can be switched on.
///
/// @param id the id
/// @param name the name a player sees
/// @param installed true when it is installed
/// @return the mutator
inline picker::Mutator made(std::string id, std::string name, bool installed) {
    picker::Mutator mutator;
    mutator.id = std::move(id);
    mutator.name = std::move(name);
    mutator.installed = installed;
    mutator.version = "2.0";
    mutator.author = "Ridge";
    return mutator;
}

/// A choice option.
///
/// @param id the option id
/// @param label the label
/// @param values the values, which are also the captions
/// @param fallback the default
/// @return the option
inline picker::OptionSpec choice_option(
    std::string id, std::string label, std::vector<std::string> values, std::string fallback
) {
    picker::OptionSpec spec;
    spec.id = std::move(id);
    spec.type = picker::OptionSpec::Type::choice;
    spec.label = std::move(label);
    spec.values = std::move(values);
    spec.captions = spec.values;
    spec.fallback = std::move(fallback);
    return spec;
}

/// A toggle. The off value is first.
///
/// @param id the option id
/// @param label the label
/// @return the option
inline picker::OptionSpec toggle_option(std::string id, std::string label) {
    picker::OptionSpec spec;
    spec.id = std::move(id);
    spec.type = picker::OptionSpec::Type::toggle;
    spec.label = std::move(label);
    spec.values = {"No", "Yes"};
    spec.fallback = "No";
    return spec;
}

/// A number option.
///
/// @param id the option id
/// @param label the label
/// @return the option
inline picker::OptionSpec number_option(std::string id, std::string label) {
    picker::OptionSpec spec;
    spec.id = std::move(id);
    spec.type = picker::OptionSpec::Type::number;
    spec.label = std::move(label);
    spec.least = "0";
    spec.most = "10";
    spec.step = "2";
    spec.fallback = "4";
    return spec;
}

/// Marks a mutator checked and compatible, so the footer can show its chip.
///
/// @param[in,out] mutator the mutator
inline void compatible(picker::Mutator& mutator) {
    mutator.verdict.state = lib::Verdict::State::compatible;
    mutator.verdict.summary = "Changes the ridge.";
}

/// The fixture: Fast Builders, Scout Post, Gatehouse and Gatehouse Keep on,
/// in that order, and Ridge Walls and Grounded off. Scout Post has one
/// option of every type. Ridge Walls is locked. Gatehouse Keep needs
/// Gatehouse and is not installed.
///
/// @param text how texts are looked up; empty shows the English
/// @param read_only true when a guest is looking
/// @return the opened picker, with Scout Post selected
inline picker::Picker opened(lib::TextHooks text, bool read_only) {
    picker::Inputs inputs;
    inputs.setup = picker::Setup::skirmish;
    inputs.mod_name = "Ridge";
    inputs.remembered = true;
    inputs.read_only = read_only;
    inputs.text = text;

    picker::Mutator builders = made("fast-builders", "Fast Builders", true);
    for (int time = 0; time < 40; ++time)
        builders.summary += "ridge ";
    builders.tags = {"economy", "ridge-watch-outpost"};
    compatible(builders);

    picker::Mutator scout = made("scout-post", "Scout Post", true);
    scout.summary = "A post on the ridge.";
    for (int time = 0; time < 20; ++time)
        scout.description += "The scout watches the ridge. ";
    scout.tags = {"gameplay", "statistics", "commanders", "signal-post-marker"};
    scout.layers = {"units", "weapons"};
    scout.brings = true;
    scout.options.push_back(choice_option("speed", "Speed", {"1x", "2x", "4x"}, "2x"));
    scout.options.push_back(choice_option(
        "signal", "Signal", {"North", "South", "East", "West", "Near", "Far"}, "North"
    ));
    scout.options.push_back(toggle_option("alarm", "Alarm"));
    scout.options.push_back(number_option("range", "Range"));
    compatible(scout);

    picker::Mutator gatehouse = made("gatehouse", "Gatehouse", true);
    gatehouse.summary = "The gate on the ridge.";
    gatehouse.tags = {"ai", "new-units"};
    compatible(gatehouse);

    picker::Mutator keep = made("gatehouse-keep", "Gatehouse Keep", false);
    keep.summary = "The keep above the gate.";
    keep.tags = {"disable", "keep-stone-marker"};
    lib::Prerequisite need;
    need.id = "gatehouse";
    keep.needs.push_back(std::move(need));
    compatible(keep);

    picker::Mutator walls = made("ridge-walls", "Ridge Walls", true);
    walls.summary = "Walls along the ridge.";
    walls.tags = {"maps", "wall-line-marker"};
    walls.unavailable = "The ridge has no gate";

    picker::Mutator grounded = made("grounded", "Grounded", false);
    grounded.summary = "Nothing leaves the ground.";
    grounded.tags = {"ground-cover-marker"};

    const auto choose = [](std::string id) {
        picker::Chosen chosen;
        chosen.id = std::move(id);
        return chosen;
    };
    inputs.last_set.push_back(choose("fast-builders"));
    inputs.last_set.push_back(choose("scout-post"));
    inputs.last_set.push_back(choose("gatehouse"));
    inputs.last_set.push_back(choose("gatehouse-keep"));
    inputs.mutators.push_back(std::move(builders));
    inputs.mutators.push_back(std::move(scout));
    inputs.mutators.push_back(std::move(gatehouse));
    inputs.mutators.push_back(std::move(keep));
    inputs.mutators.push_back(std::move(walls));
    inputs.mutators.push_back(std::move(grounded));

    picker::Picker model;
    picker::open(model, std::move(inputs));
    model.selected = "scout-post";
    return model;
}

} // namespace picker_fixture
