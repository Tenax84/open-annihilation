// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The mutator picker's screen over a made-up set, laid out with a synthetic
// font at Compact, Regular and Large, and at the windows of 640x480 (1x),
// 1920x1080 (2x) and 2560x1440 (2x). At each frame: every part lies inside
// the window; no two controls meet, apart from the list and its rows and a
// row and its grip and switch; every row, Off rows included, has a kit grip
// and a switch; On rows are numbered 1 to n; the headings are present; every
// option is a control of its type; ▲ and ▼ are disabled at a band's ends;
// Compact has no details pane, and DETAILS opens the page, which has BACK.
// Tab follows the declared order, and no grip or label is in it. The names
// are sound and unique. Every text fits, and no button is cut, with a French
// lookup and with long German. A drag draws its row, its well and its line,
// and a refused drop draws no line. Read-only locks the switches, disables
// the grips and shows CLOSE alone. Both empty lists are drawn. waiting
// disables USE THESE and is the summary.
//
// The synthetic font's glyph for byte b is 2 + b % 4 columns by 8 rows.

#include "oa/formats/fnt.hpp"
#include "oa/test/check.hpp"
#include "oa/ui/frontend_renderer.hpp"
#include "oa/ui/kit/chrome.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/controls.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/looks.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/library/library.hpp"
#include "oa/ui/mutator_picker/model.hpp"
#include "oa/ui/mutator_picker/screen.hpp"
#include "oa/ui/mutator_picker/text.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace kit = oa::ui::kit;
namespace picker = oa::ui::mutator_picker;
namespace lib = oa::ui::library;
namespace renderer = oa::ui::frontend_renderer;

/// The synthetic font of the kit's tests, for both roles.
///
/// @return the fonts
kit::Fonts block_fonts() {
    renderer::TextFont font;
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
    font.ink[ink_index] = static_cast<uint16_t>(renderer::blend_opaque);
    return {font, font, {}, {}};
}

/// The fonts every layout here is measured in.
///
/// @return the fonts
const kit::Fonts& fonts() {
    static const kit::Fonts loaded = block_fonts();
    return loaded;
}

/// Prints each problem a check found, and fails the check when there are any.
///
/// @param problems the problems
/// @param where the frame or case they belong to
/// @param file the check's file
/// @param line the check's line
void expect_none(
    const std::vector<std::string>& problems, const std::string& where, const char* file, int line
) {
    for (const std::string& problem : problems)
        std::fprintf(stderr, "%s:%d: %s: %s\n", file, line, where.c_str(), problem.c_str());
    if (!problems.empty())
        ++oa::test::failed_checks();
}

#define OA_EXPECT_NONE(problems, where) expect_none((problems), (where), __FILE__, __LINE__)

/// Tells whether a name starts with a prefix.
///
/// @param name the name
/// @param prefix the prefix
/// @return true when it does
bool starts(std::string_view name, std::string_view prefix) {
    return name.size() >= prefix.size() && name.substr(0, prefix.size()) == prefix;
}

/// Returns the part of an item that is drawn: its rectangle, within its clip
/// when it has one, and a focus ring's outline round its control.
///
/// @param item the item
/// @return the rectangle
kit::Rect drawn_area(const kit::Item& item) {
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
kit::Rect press_area(const kit::Control& control) {
    if (control.clip.width > 0 && control.clip.height > 0)
        return kit::intersect(control.rect, control.clip);
    return control.rect;
}

/// Tells whether a rectangle holds any point.
///
/// @param rect the rectangle
/// @return true when it is wide and high
bool has_room(const kit::Rect& rect) {
    return rect.width > 0 && rect.height > 0;
}

/// Lists the parts of a layout that lie outside its window.
///
/// @param list the display list
/// @param window the window's size
/// @return a line for each part outside; none when all lie inside
std::vector<std::string> outside_window(const kit::DisplayList& list, kit::Point window) {
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
std::vector<std::string>
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
        default:
            break;
        }
    }
    return problems;
}

/// Returns a list's control of a name, or null.
///
/// @param list the display list
/// @param name the name
/// @return the control
const kit::Control* find_named(const kit::DisplayList& list, std::string_view name) {
    return kit::control_of(list, kit::control_named(list, name));
}

/// Tells whether a control is an open menu's item. Those may lie over the
/// controls under the menu. A row is not one of them.
///
/// @param control the control
/// @return true for a menu item
bool menu_item(const kit::Control& control) {
    if (control.kind != kit::ControlKind::list_item)
        return false;
    if (starts(control.name, picker::name_row_prefix))
        return false;
    return starts(control.name, std::string(picker::name_filter) + ".") ||
           starts(control.name, std::string(picker::name_tags) + ".") ||
           starts(control.name, picker::name_option_prefix);
}

/// Tells whether two controls may occupy the same points: the list and its
/// hint or its rows, or a row and its own grip and switch.
///
/// @param outer one control
/// @param inner the other
/// @return true when the pair is one of those
bool held_inside(const kit::Control& outer, const kit::Control& inner) {
    if (outer.name == picker::name_list &&
        (inner.name == picker::name_hint || starts(inner.name, picker::name_row_prefix)))
        return true;
    if (!starts(outer.name, picker::name_row_prefix))
        return false;
    if (outer.name.size() >= picker::name_grip_suffix.size() &&
        outer.name.ends_with(picker::name_grip_suffix))
        return false;
    if (outer.name.size() >= picker::name_switch_suffix.size() &&
        outer.name.ends_with(picker::name_switch_suffix))
        return false;
    return inner.name == outer.name + std::string(picker::name_grip_suffix) ||
           inner.name == outer.name + std::string(picker::name_switch_suffix);
}

/// Lists the pairs of controls whose press areas meet.
///
/// @param list the display list
/// @return a line for each pair
std::vector<std::string> meeting_controls(const kit::DisplayList& list) {
    std::vector<std::string> problems;
    for (std::size_t first_at = 0; first_at < list.controls.size(); ++first_at) {
        for (std::size_t second_at = first_at + 1; second_at < list.controls.size(); ++second_at) {
            const kit::Control& first = list.controls[first_at];
            const kit::Control& second = list.controls[second_at];
            if (menu_item(first) || menu_item(second))
                continue;
            if (held_inside(first, second) || held_inside(second, first))
                continue;
            if (has_room(kit::intersect(press_area(first), press_area(second))))
                problems.push_back(first.name + " meets " + second.name);
        }
    }
    return problems;
}

/// The French of the mockup, for this test only. OFF is the switch's "Non":
/// the heading and the switch share one English word, so the heading says
/// Non here too.
///
/// @param context unused
/// @param english the English word
/// @return the French, or the English when this test leaves it
std::string_view french_shown(void* /*context*/, std::string_view english) {
    using picker::words::back_button;
    using picker::words::cancel;
    using picker::words::details_button;
    using picker::words::filter_all;
    using picker::words::heading_off;
    using picker::words::heading_on;
    using picker::words::no_mod;
    using picker::words::search_mutators;
    using picker::words::setup_skirmish;
    using picker::words::switch_on;
    using picker::words::title_mutators;
    using picker::words::use_these;
    if (english == title_mutators)
        return "Mutateurs";
    if (english == setup_skirmish)
        return "Escarmouche";
    if (english == no_mod)
        return "Sans mod";
    if (english == filter_all)
        return "Tous";
    if (english == search_mutators)
        return "Rechercher des mutateurs";
    if (english == heading_on)
        return "Actifs · appliqués de haut en bas";
    if (english == heading_off)
        return "Non";
    if (english == switch_on)
        return "Oui";
    if (english == details_button)
        return "Détails";
    if (english == cancel)
        return "Annuler";
    if (english == use_these)
        return "Valider";
    if (english == back_button)
        return "Retour";
    return english;
}

/// Long German button words, long enough to grow a button past the kit's
/// button width, and the long On heading. The switch halves stay short.
///
/// @param context unused
/// @param english the English word
/// @return the German, or the English when this test leaves it
std::string_view german_shown(void* /*context*/, std::string_view english) {
    using picker::words::back_button;
    using picker::words::cancel;
    using picker::words::close_button;
    using picker::words::details_button;
    using picker::words::heading_off;
    using picker::words::heading_on;
    using picker::words::switch_on;
    using picker::words::use_these;
    if (english == use_these)
        return "ÜBERNEHMEN UND LOS";
    if (english == details_button)
        return "EINZELHEITEN ANZEIGEN";
    if (english == cancel)
        return "ABBRECHEN UND ZURÜCK";
    if (english == back_button)
        return "ZURÜCK ZUR LISTE";
    if (english == close_button)
        return "SCHLIESSEN UND GEHEN";
    if (english == heading_on)
        return "Aktiv · von oben nach unten angewandt";
    if (english == heading_off)
        return "AUS";
    if (english == switch_on)
        return "AN";
    return english;
}

/// A made-up mutator that can be switched on.
///
/// @param id the id
/// @param name the name a player sees
/// @param installed true when it is installed
/// @return the mutator
picker::Mutator made(std::string id, std::string name, bool installed) {
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
picker::OptionSpec choice_option(
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
picker::OptionSpec toggle_option(std::string id, std::string label) {
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
picker::OptionSpec number_option(std::string id, std::string label) {
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
void compatible(picker::Mutator& mutator) {
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
picker::Picker opened(lib::TextHooks text, bool read_only) {
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

/// Lays the picker out in the synthetic font.
///
/// @param model the picker
/// @param state the screen's state
/// @param frame the frame
/// @return the display list
kit::DisplayList
laid_out(const picker::Picker& model, const picker::ScreenState& state, const kit::Frame& frame) {
    return picker::picker_layout(model, state, frame, fonts());
}

/// The frames the screen is checked at, and the window each one gives.
struct FrameCase {
    const char* name;  ///< the case
    kit::Frame frame;  ///< the frame
    kit::Point window; ///< the window's size
};

/// Returns one window frame.
///
/// @param width the canvas's width, in pixels
/// @param height the canvas's height, in pixels
/// @param percent the scale, as a percentage
/// @return the frame
kit::Frame window_frame(int32_t width, int32_t height, int32_t percent) {
    kit::Viewport viewport;
    viewport.width = width;
    viewport.height = height;
    viewport.scale_percent = percent;
    return kit::frame_of(viewport);
}

/// The six frames.
///
/// @return the frames
std::vector<FrameCase> frames() {
    const kit::Frame compact_frame{{0, 0, 480, 324}, kit::SizeClass::compact, 100};
    const kit::Frame regular_frame{{0, 0, 720, 486}, kit::SizeClass::regular, 100};
    const kit::Frame large_frame{{0, 0, 960, 600}, kit::SizeClass::large, 100};
    const kit::Frame narrow = window_frame(640, 480, 100);
    const kit::Frame desktop = window_frame(1920, 1080, 200);
    const kit::Frame wide = window_frame(2560, 1440, 200);
    return {
        {"compact", compact_frame, picker::window_size(compact_frame)},
        {"regular", regular_frame, picker::window_size(regular_frame)},
        {"large", large_frame, picker::window_size(large_frame)},
        {"640x480", narrow, picker::window_size(narrow)},
        {"1920x1080", desktop, picker::window_size(desktop)},
        {"2560x1440", wide, picker::window_size(wide)},
    };
}

/// Tells whether a control can take Tab.
///
/// @param control the control
/// @return true when it is enabled and focusable
bool tabbable(const kit::Control& control) {
    return control.enabled && control.focusable;
}

/// Appends a control's name when it is present and can take Tab.
///
/// @param[in,out] names the names so far
/// @param list the display list
/// @param name the name
void append_tab(
    std::vector<std::string>& names, const kit::DisplayList& list, std::string_view name
) {
    const kit::Control* control = find_named(list, name);
    if (control != nullptr && tabbable(*control))
        names.push_back(control->name);
}

/// Tab's names, in order.
///
/// @param list the display list
/// @return the names
std::vector<std::string> tab_names(const kit::DisplayList& list) {
    std::vector<std::string> names;
    for (const kit::ControlId id : list.tab_order) {
        const kit::Control* control = kit::control_of(list, id);
        names.push_back(control != nullptr ? control->name : std::string("?"));
    }
    return names;
}

/// Checks Tab's order against the controls that can take it.
///
/// @param list the display list
/// @param frame the frame
/// @param page Compact's details page
/// @param where the case
void check_tabs(
    const kit::DisplayList& list, const kit::Frame& frame, bool page, const std::string& where
) {
    const bool compact = frame.size_class == kit::SizeClass::compact;
    std::vector<std::string> expected;
    if (!page && compact) {
        append_tab(expected, list, picker::name_filter);
    } else if (!page) {
        append_tab(expected, list, picker::name_filter_all);
        append_tab(expected, list, picker::name_filter_installed);
        append_tab(expected, list, picker::name_filter_downloads);
        for (const kit::Control& control : list.controls) {
            if (control.kind == kit::ControlKind::button &&
                starts(control.name, std::string(picker::name_tags) + ".") && tabbable(control))
                expected.push_back(control.name);
        }
        append_tab(expected, list, picker::name_tags);
    }
    if (!page) {
        append_tab(expected, list, picker::name_search);
        append_tab(expected, list, picker::name_list);
    }
    for (const kit::Control& control : list.controls) {
        if (starts(control.name, picker::name_option_prefix) && tabbable(control))
            expected.push_back(control.name);
    }
    append_tab(expected, list, picker::name_up);
    append_tab(expected, list, picker::name_down);
    if (page)
        append_tab(expected, list, picker::name_back);
    else if (compact)
        append_tab(expected, list, picker::name_details);
    append_tab(expected, list, picker::name_cancel);
    append_tab(expected, list, picker::name_use);
    append_tab(expected, list, picker::name_done);
    append_tab(expected, list, picker::name_close);

    const std::vector<std::string> got = tab_names(list);
    if (got != expected) {
        std::fprintf(stderr, "%s: tab order\n", where.c_str());
        for (const std::string& name : got)
            std::fprintf(stderr, "  got %s\n", name.c_str());
        for (const std::string& name : expected)
            std::fprintf(stderr, "  want %s\n", name.c_str());
    }
    OA_CHECK(got == expected);
    for (const std::string& name : got) {
        OA_CHECK(!name.ends_with(picker::name_grip_suffix));
        OA_CHECK(
            !(starts(name, picker::name_row_prefix) && name.ends_with(picker::name_switch_suffix))
        );
        OA_CHECK(name != picker::name_hint);
        OA_CHECK(name != picker::name_summary);
        OA_CHECK(name != picker::name_verdict);
        OA_CHECK(name != "mutators.order");
        OA_CHECK(name != "mutators.list.bar");
        OA_CHECK(name != "mutators.details.bar");
    }
}

/// The button captions that contain the ellipsis.
///
/// @param list the display list
/// @return a line for each one
std::vector<std::string> cut_buttons(const kit::DisplayList& list) {
    std::vector<std::string> problems;
    for (const kit::Item& item : list.items) {
        const auto* look = std::get_if<kit::ButtonLook>(&item.look);
        if (item.role == kit::Role::button && look != nullptr &&
            look->caption.find(kit::ellipsis) != std::string::npos)
            problems.push_back(look->caption);
    }
    return problems;
}

/// Tells whether a text item lies inside a row.
///
/// @param item the item
/// @param row the row
/// @return true when its rectangle is inside the row
bool text_in_row(const kit::Item& item, const kit::Rect& row) {
    return item.role == kit::Role::text && kit::wholly_in(item.rect, row);
}

/// Checks one frame of the fixture: the geometry, the rows, the options, the
/// names and the texts.
///
/// @param model the picker
/// @param where the case's name
/// @param frame the frame
/// @param window the window's size
void check_frame(
    const picker::Picker& model,
    const std::string& where,
    const kit::Frame& frame,
    kit::Point window
) {
    picker::ScreenState state;
    const kit::DisplayList list = laid_out(model, state, frame);
    const kit::Metrics& metrics = kit::metrics_of(frame.size_class);
    const bool compact = frame.size_class == kit::SizeClass::compact;
    const int32_t row_height = compact ? picker::compact_row_height
                               : frame.size_class == kit::SizeClass::large
                                   ? picker::large_row_height
                                   : picker::regular_row_height;
    const int32_t number_width = compact ? picker::compact_number_column : picker::number_column;

    OA_CHECK(window.x == picker::window_size(frame).x);
    OA_CHECK(window.y == picker::window_size(frame).y);
    OA_EXPECT_NONE(outside_window(list, window), where);
    OA_EXPECT_NONE(meeting_controls(list), where);
    OA_EXPECT_NONE(texts_not_fitting(list, fonts()), where);
    OA_EXPECT_NONE(cut_buttons(list), where);
    const std::string names = kit::name_problem(list);
    if (!names.empty())
        std::fprintf(stderr, "%s: %s\n", where.c_str(), names.c_str());
    OA_CHECK(names.empty());
    check_tabs(list, frame, false, where);

    const std::vector<picker::Row> shown = picker::rows(model);
    std::vector<std::string> numbers;
    for (const picker::Row& row : shown) {
        const std::string row_name = std::string(picker::name_row_prefix) + row.id;
        const kit::Control* row_control = find_named(list, row_name);
        const std::string grip_name = row_name + std::string(picker::name_grip_suffix);
        const std::string switch_name = row_name + std::string(picker::name_switch_suffix);
        const kit::Control* grip = find_named(list, grip_name);
        const kit::Control* toggle = find_named(list, switch_name);
        OA_CHECK(row_control != nullptr);
        OA_CHECK(grip != nullptr);
        OA_CHECK(toggle != nullptr);
        if (row_control == nullptr || grip == nullptr || toggle == nullptr)
            continue;
        OA_CHECK(row_control->kind == kit::ControlKind::list_item);
        OA_CHECK(!row_control->text.empty());
        if (row.id == "fast-builders")
            OA_CHECK(row_control->text == "Fast Builders");
        if (row.id == "ridge-walls")
            OA_CHECK(row_control->text == "Ridge Walls");
        OA_CHECK(row_control->rect.height == row_height);
        OA_CHECK(!row_control->focusable);
        OA_CHECK(grip->kind == kit::ControlKind::button);
        OA_CHECK(!grip->focusable);
        OA_CHECK(grip->enabled);
        OA_CHECK(grip->rect.width == kit::grip_width);
        OA_CHECK(grip->rect.height == kit::grip_height);
        OA_CHECK(toggle->kind == kit::ControlKind::toggle);
        OA_CHECK(!toggle->focusable);
        bool grip_drawn = false;
        for (const kit::Item& item : list.items) {
            if (item.role == kit::Role::grip && item.control == grip->id) {
                grip_drawn = true;
                OA_CHECK(item.rect.width == kit::grip_width);
                OA_CHECK(item.rect.height == kit::grip_height);
                OA_CHECK(std::get_if<kit::GripLook>(&item.look) != nullptr);
            }
        }
        OA_CHECK(grip_drawn);
        int32_t lines = 0;
        for (const kit::Item& item : list.items) {
            if (!text_in_row(item, row_control->rect))
                continue;
            OA_CHECK(item.text.find('\n') == std::string::npos);
            if (item.font == kit::FontRole::small && item.align == kit::Align::left)
                ++lines;
            if (row.on && item.align == kit::Align::right && item.colour == kit::colour::quiet) {
                numbers.push_back(item.text);
                OA_CHECK(item.rect.width == number_width);
            }
        }
        OA_CHECK(lines == 1);
    }
    OA_CHECK(numbers.size() == 4);
    if (numbers.size() == 4) {
        OA_CHECK(numbers[0] == "1");
        OA_CHECK(numbers[1] == "2");
        OA_CHECK(numbers[2] == "3");
        OA_CHECK(numbers[3] == "4");
    }

    std::vector<std::string> headings;
    for (const kit::Item& item : list.items) {
        if (item.role != kit::Role::heading)
            continue;
        const auto* look = std::get_if<kit::HeadingLook>(&item.look);
        if (look != nullptr)
            headings.push_back(look->text);
    }
    OA_CHECK(headings.size() == 2);
    if (headings.size() == 2) {
        OA_CHECK(headings[0] == picker::words::heading_on);
        OA_CHECK(headings[1] == picker::words::heading_off);
    }

    const int32_t content = window.x - metrics.edge - 2 * metrics.padding;
    const int32_t list_width = compact ? content
                               : frame.size_class == kit::SizeClass::large
                                   ? picker::large_list_width
                                   : picker::regular_list_width;
    bool pane = false;
    for (const kit::Item& item : list.items) {
        if (item.role == kit::Role::fill && item.colour == kit::colour::list &&
            item.rect.x == metrics.padding && item.rect.width == list_width &&
            item.rect.height > row_height)
            pane = true;
    }
    OA_CHECK(pane);
    const kit::Control* search = find_named(list, picker::name_search);
    OA_CHECK(search != nullptr);
    if (search != nullptr) {
        const int32_t search_width =
            compact ? content - picker::compact_filter_width - metrics.button_gap
            : frame.size_class == kit::SizeClass::large ? picker::large_search_width
                                                        : picker::regular_search_width;
        OA_CHECK(search->rect.width == search_width);
    }

    bool details_badge = false;
    bool locked = false;
    bool download = false;
    for (const kit::Item& item : list.items) {
        if (item.role == kit::Role::package_badge && item.rect.width == picker::details_badge_side)
            details_badge = true;
        if (item.role == kit::Role::locked_fade)
            locked = true;
        if (const auto* look = std::get_if<kit::ChipLook>(&item.look);
            item.role == kit::Role::chip && look != nullptr && look->icon &&
            *look->icon == kit::StateIcon::download_arrow)
            download = true;
    }
    OA_CHECK(locked);
    OA_CHECK(download);
    OA_CHECK(details_badge == !compact);

    const kit::Control* speed = find_named(list, "mutators.option.speed");
    const kit::Control* signal = find_named(list, "mutators.option.signal");
    const kit::Control* alarm = find_named(list, "mutators.option.alarm");
    const kit::Control* range = find_named(list, "mutators.option.range");
    if (compact) {
        OA_CHECK(speed == nullptr);
        OA_CHECK(find_named(list, picker::name_details) != nullptr);
        OA_CHECK(find_named(list, picker::name_filter) != nullptr);
        OA_CHECK(find_named(list, picker::name_tags) == nullptr);
        OA_CHECK(find_named(list, picker::name_verdict) == nullptr);
        const kit::Control* filter = find_named(list, picker::name_filter);
        OA_CHECK(filter != nullptr && filter->rect.width == picker::compact_filter_width);
    } else {
        OA_CHECK(speed != nullptr && speed->kind == kit::ControlKind::levels);
        OA_CHECK(signal != nullptr && signal->kind == kit::ControlKind::choice);
        OA_CHECK(alarm != nullptr && alarm->kind == kit::ControlKind::toggle);
        OA_CHECK(range != nullptr && range->kind == kit::ControlKind::slider);
        OA_CHECK(find_named(list, picker::name_up) != nullptr);
        OA_CHECK(find_named(list, picker::name_down) != nullptr);
        OA_CHECK(find_named(list, picker::name_tags) != nullptr);
        OA_CHECK(find_named(list, picker::name_details) == nullptr);
        const kit::Control* verdict = find_named(list, picker::name_verdict);
        OA_CHECK(verdict != nullptr);
        if (verdict != nullptr)
            OA_CHECK(verdict->text.find("Ridge") != std::string::npos);
        const kit::Control* all = find_named(list, picker::name_filter_all);
        const kit::Control* installed = find_named(list, picker::name_filter_installed);
        const kit::Control* downloads = find_named(list, picker::name_filter_downloads);
        OA_CHECK(all != nullptr && all->text == "All (6)");
        OA_CHECK(installed != nullptr && installed->text == "Installed (4)");
        OA_CHECK(downloads != nullptr && downloads->text == "Downloads (2)");
    }
    const kit::Control* hint = find_named(list, picker::name_hint);
    OA_CHECK(hint != nullptr);
    if (hint != nullptr)
        OA_CHECK(hint->text.find("skirmish") != std::string::npos);
    const kit::Control* summary = find_named(list, picker::name_summary);
    OA_CHECK(summary != nullptr);
}

/// Checks ▲ and ▼ at one selection.
///
/// @param model the picker; its selection is replaced
/// @param id the mutator
/// @param up_enabled ▲ can be pressed
/// @param down_enabled ▼ can be pressed
/// @param frame the frame
void check_band(
    picker::Picker model,
    std::string id,
    bool up_enabled,
    bool down_enabled,
    const kit::Frame& frame
) {
    model.selected = std::move(id);
    picker::ScreenState state;
    const kit::DisplayList list = laid_out(model, state, frame);
    const kit::Control* up = find_named(list, picker::name_up);
    const kit::Control* down = find_named(list, picker::name_down);
    OA_CHECK(up != nullptr && down != nullptr);
    if (up == nullptr || down == nullptr)
        return;
    OA_CHECK(up->enabled == up_enabled);
    OA_CHECK(down->enabled == down_enabled);
}

/// The bottom of the lowest row, in window points.
///
/// @param list the display list
/// @return the row's bottom, or 0 when there is none
int32_t lowest_row_bottom(const kit::DisplayList& list) {
    int32_t bottom = 0;
    for (const kit::Control& control : list.controls) {
        if (control.kind != kit::ControlKind::list_item)
            continue;
        if (!starts(control.name, picker::name_row_prefix))
            continue;
        if (control.name.ends_with(picker::name_grip_suffix) ||
            control.name.ends_with(picker::name_switch_suffix))
            continue;
        bottom = std::max(bottom, control.rect.y + control.rect.height);
    }
    return bottom;
}

/// Checks a drag: the row moves, its place is the well, and the line is drawn
/// only when the drop has a target.
///
/// @param model the picker
/// @param frame the frame
/// @param window the window
void check_drag(const picker::Picker& model, const kit::Frame& frame, kit::Point window) {
    picker::ScreenState state;
    const kit::DisplayList resting = laid_out(model, state, frame);
    const int32_t y = lowest_row_bottom(resting);
    picker::DragShown drag;
    drag.id = "scout-post";
    drag.y = y;
    drag.target = picker::Drop{true, 1};
    state.drag = drag;
    const kit::DisplayList moving = laid_out(model, state, frame);
    OA_EXPECT_NONE(outside_window(moving, window), "drag");
    OA_EXPECT_NONE(meeting_controls(moving), "drag");
    const kit::Control* row = find_named(moving, "mutators.row.scout-post");
    OA_CHECK(row != nullptr && row->rect.y == y);
    if (row == nullptr)
        return;
    bool well = false;
    bool line = false;
    for (const kit::Item& item : moving.items) {
        if (item.role == kit::Role::fill && item.colour == kit::colour::well &&
            item.rect.height == row->rect.height)
            well = true;
        if (item.role == kit::Role::fill && item.colour == kit::colour::accent &&
            item.rect.height == 2)
            line = true;
    }
    OA_CHECK(well);
    OA_CHECK(line);

    state.drag->target.reset();
    const kit::DisplayList refused = laid_out(model, state, frame);
    bool refused_line = false;
    for (const kit::Item& item : refused.items) {
        if (item.role == kit::Role::fill && item.colour == kit::colour::accent &&
            item.rect.height == 2)
            refused_line = true;
    }
    OA_CHECK(!refused_line);
    const kit::Control* moved = find_named(refused, "mutators.row.scout-post");
    OA_CHECK(moved != nullptr && moved->rect.y == y);
}

/// Checks read-only viewing: locked switches, disabled grips, CLOSE alone.
///
/// @param frame the frame
/// @param window the window
void check_read_only(const kit::Frame& frame, kit::Point window) {
    const picker::Picker model = opened({}, true);
    picker::ScreenState state;
    const kit::DisplayList list = laid_out(model, state, frame);
    OA_EXPECT_NONE(outside_window(list, window), "read-only");
    OA_EXPECT_NONE(meeting_controls(list), "read-only");
    OA_EXPECT_NONE(texts_not_fitting(list, fonts()), "read-only");
    check_tabs(list, frame, false, "read-only");
    OA_CHECK(find_named(list, picker::name_done) != nullptr);
    OA_CHECK(find_named(list, picker::name_cancel) == nullptr);
    OA_CHECK(find_named(list, picker::name_use) == nullptr);
    OA_CHECK(find_named(list, picker::name_details) == nullptr);
    OA_CHECK(find_named(list, picker::name_back) == nullptr);
    OA_CHECK(find_named(list, picker::name_close) != nullptr);
    OA_CHECK(find_named(list, picker::name_verdict) == nullptr);
    const kit::Control* search = find_named(list, picker::name_search);
    OA_CHECK(search != nullptr && search->focusable && search->enabled);
    for (const kit::Control& control : list.controls) {
        if (control.kind == kit::ControlKind::button &&
            control.name.ends_with(picker::name_grip_suffix))
            OA_CHECK(!control.enabled);
        if (starts(control.name, picker::name_option_prefix))
            OA_CHECK(!control.enabled);
    }
    for (const kit::Item& item : list.items) {
        if (item.role == kit::Role::grip) {
            const auto* look = std::get_if<kit::GripLook>(&item.look);
            OA_CHECK(look != nullptr && look->disabled);
        }
        if (item.role == kit::Role::toggle) {
            const auto* look = std::get_if<kit::SwitchLook>(&item.look);
            OA_CHECK(look != nullptr && look->locked);
        }
    }
    const kit::Control* done = find_named(list, picker::name_done);
    if (done != nullptr) {
        bool accent = false;
        for (const kit::Item& item : list.items) {
            const auto* look = std::get_if<kit::ButtonLook>(&item.look);
            if (item.control == done->id && look != nullptr)
                accent = look->style == kit::ButtonStyle::accent;
        }
        OA_CHECK(accent);
    }
}

/// Checks that waiting disables USE THESE and replaces the summary.
///
/// @param model the picker
/// @param frame the frame
void check_waiting(const picker::Picker& model, const kit::Frame& frame) {
    picker::ScreenState state;
    state.waiting = "Checking with Ridge";
    const kit::DisplayList list = laid_out(model, state, frame);
    const kit::Control* use = find_named(list, picker::name_use);
    const kit::Control* summary = find_named(list, picker::name_summary);
    OA_CHECK(use != nullptr && !use->enabled);
    OA_CHECK(summary != nullptr && summary->text == "Checking with Ridge");
    for (const std::string& name : tab_names(list))
        OA_CHECK(name != picker::name_use);
}

/// Checks both empty lists.
///
/// @param frame the frame
/// @param window the window
void check_empty(const kit::Frame& frame, kit::Point window) {
    picker::Inputs none;
    none.mod_name = "Ridge";
    none.setup = picker::Setup::skirmish;
    picker::Picker empty;
    picker::open(empty, std::move(none));
    picker::ScreenState state;
    const kit::DisplayList bare = laid_out(empty, state, frame);
    OA_EXPECT_NONE(outside_window(bare, window), "empty");
    bool none_drawn = false;
    for (const kit::Item& item : bare.items) {
        if (item.role == kit::Role::text &&
            item.text.find("No mutators here yet") != std::string::npos)
            none_drawn = true;
    }
    OA_CHECK(none_drawn);
    OA_CHECK(find_named(bare, picker::name_list) == nullptr || picker::rows(empty).empty());

    picker::Inputs filtered_inputs;
    filtered_inputs.mod_name = "Ridge";
    filtered_inputs.setup = picker::Setup::skirmish;
    picker::Mutator installed = made("fast-builders", "Fast Builders", true);
    installed.summary = "Builds fast.";
    filtered_inputs.mutators.push_back(std::move(installed));
    picker::Picker filtered;
    picker::open(filtered, std::move(filtered_inputs));
    filtered.filter = picker::Filter::downloads;
    const kit::DisplayList kept = laid_out(filtered, state, frame);
    OA_EXPECT_NONE(outside_window(kept, window), "filtered");
    bool filtered_drawn = false;
    for (const kit::Item& item : kept.items) {
        if (item.role == kit::Role::text &&
            item.text.find("No mutator matches these filters") != std::string::npos)
            filtered_drawn = true;
    }
    OA_CHECK(filtered_drawn);
    OA_CHECK(picker::rows(filtered).empty());
}

/// Checks the French lookup and the long German words.
///
/// @param frame the frame
/// @param window the window
/// @param where the case
void check_language(const kit::Frame& frame, kit::Point window, const std::string& where) {
    lib::TextHooks french;
    french.shown = french_shown;
    const picker::Picker french_model = opened(french, false);
    picker::ScreenState state;
    const kit::DisplayList french_list = laid_out(french_model, state, frame);
    OA_EXPECT_NONE(outside_window(french_list, window), where + " fr");
    OA_EXPECT_NONE(meeting_controls(french_list), where + " fr");
    OA_EXPECT_NONE(texts_not_fitting(french_list, fonts()), where + " fr");
    OA_EXPECT_NONE(cut_buttons(french_list), where + " fr");
    bool title = false;
    bool heading = false;
    bool placeholder = false;
    bool switched = false;
    for (const kit::Item& item : french_list.items) {
        if (const auto* look = std::get_if<kit::HeaderLook>(&item.look))
            title = look->title == "Mutateurs" &&
                    look->version.find("Escarmouche") != std::string::npos;
        if (const auto* look = std::get_if<kit::HeadingLook>(&item.look)) {
            if (look->text == "Actifs · appliqués de haut en bas")
                heading = true;
        }
        if (const auto* look = std::get_if<kit::SearchLook>(&item.look))
            placeholder = look->placeholder == "Rechercher des mutateurs";
        if (const auto* look = std::get_if<kit::SwitchLook>(&item.look))
            switched = switched || (look->off_caption == "Non" && look->on_caption == "Oui");
    }
    OA_CHECK(title);
    OA_CHECK(heading);
    OA_CHECK(placeholder);
    OA_CHECK(switched);
    const kit::Control* use = find_named(french_list, picker::name_use);
    OA_CHECK(use != nullptr && use->text == "Valider");
    if (frame.size_class == kit::SizeClass::compact) {
        const kit::Control* details = find_named(french_list, picker::name_details);
        OA_CHECK(details != nullptr && details->text == "Détails");
    }

    lib::TextHooks german;
    german.shown = german_shown;
    const picker::Picker german_model = opened(german, false);
    const kit::DisplayList german_list = laid_out(german_model, state, frame);
    OA_EXPECT_NONE(outside_window(german_list, window), where + " de");
    OA_EXPECT_NONE(texts_not_fitting(german_list, fonts()), where + " de");
    OA_EXPECT_NONE(cut_buttons(german_list), where + " de");
    const kit::Control* german_use = find_named(german_list, picker::name_use);
    const kit::Control* german_cancel = find_named(german_list, picker::name_cancel);
    OA_CHECK(german_use != nullptr && german_use->text == "ÜBERNEHMEN UND LOS");
    OA_CHECK(german_cancel != nullptr && german_cancel->text == "ABBRECHEN UND ZURÜCK");
    if (frame.size_class == kit::SizeClass::compact) {
        const kit::Control* details = find_named(german_list, picker::name_details);
        OA_CHECK(details != nullptr && details->text == "EINZELHEITEN ANZEIGEN");
        OA_CHECK(details->rect.width > kit::compact_metrics.button_width);
    }
    bool german_heading = false;
    for (const kit::Item& item : german_list.items) {
        const auto* look = std::get_if<kit::HeadingLook>(&item.look);
        if (look != nullptr && look->text == "Aktiv · von oben nach unten angewandt")
            german_heading = true;
    }
    OA_CHECK(german_heading);
}

/// Checks Compact's details page.
///
/// @param model the picker
/// @param frame a Compact frame
/// @param window the window
void check_details_page(const picker::Picker& model, const kit::Frame& frame, kit::Point window) {
    picker::ScreenState state;
    state.details_page = true;
    const kit::DisplayList page = laid_out(model, state, frame);
    const kit::DisplayList direct = picker::details_page_layout(model, state, frame, fonts());
    OA_EXPECT_NONE(outside_window(page, window), "details");
    OA_EXPECT_NONE(meeting_controls(page), "details");
    OA_EXPECT_NONE(texts_not_fitting(page, fonts()), "details");
    OA_EXPECT_NONE(cut_buttons(page), "details");
    check_tabs(page, frame, true, "details");
    OA_CHECK(find_named(page, picker::name_back) != nullptr);
    OA_CHECK(find_named(page, picker::name_list) == nullptr);
    OA_CHECK(find_named(page, picker::name_details) == nullptr);
    OA_CHECK(find_named(page, picker::name_filter) == nullptr);
    OA_CHECK(find_named(direct, picker::name_back) != nullptr);
    OA_CHECK(find_named(direct, picker::name_list) == nullptr);
    const kit::Control* speed = find_named(page, "mutators.option.speed");
    const kit::Control* signal = find_named(page, "mutators.option.signal");
    const kit::Control* alarm = find_named(page, "mutators.option.alarm");
    const kit::Control* range = find_named(page, "mutators.option.range");
    OA_CHECK(speed != nullptr && speed->kind == kit::ControlKind::levels);
    OA_CHECK(signal != nullptr && signal->kind == kit::ControlKind::choice);
    OA_CHECK(alarm != nullptr && alarm->kind == kit::ControlKind::toggle);
    OA_CHECK(range != nullptr && range->kind == kit::ControlKind::slider);
    bool badge = false;
    for (const kit::Item& item : page.items) {
        if (item.role == kit::Role::package_badge && item.rect.width == picker::details_badge_side)
            badge = true;
    }
    OA_CHECK(badge);

    state.menu = picker::Menu::choice;
    state.menu_option = "signal";
    const kit::DisplayList menu = laid_out(model, state, frame);
    OA_EXPECT_NONE(outside_window(menu, window), "choice menu");
    OA_EXPECT_NONE(meeting_controls(menu), "choice menu");
    bool item_named = false;
    for (const kit::Control& control : menu.controls) {
        if (starts(control.name, "mutators.option.signal.") &&
            control.kind == kit::ControlKind::list_item)
            item_named = true;
    }
    OA_CHECK(item_named);
}

/// Checks the keyboard focus ring, and Compact's open filter menu.
///
/// @param model the picker
/// @param compact a Compact frame
/// @param regular a Regular frame
/// @param window the Compact window
void check_focus_and_menu(
    const picker::Picker& model,
    const kit::Frame& compact,
    const kit::Frame& regular,
    kit::Point window
) {
    picker::ScreenState state;
    const kit::DisplayList plain = laid_out(model, state, regular);
    const kit::Control* cancel = find_named(plain, picker::name_cancel);
    OA_CHECK(cancel != nullptr);
    if (cancel != nullptr) {
        state.interaction.focus_shown = true;
        state.interaction.focused = cancel->id;
        const kit::DisplayList focused = laid_out(model, state, regular);
        bool ring = false;
        for (const kit::Item& item : focused.items) {
            if (item.role == kit::Role::focus_ring && item.control == cancel->id)
                ring = true;
        }
        OA_CHECK(ring);
    }
    const kit::Control* search = find_named(plain, picker::name_search);
    OA_CHECK(search != nullptr);
    if (search != nullptr) {
        state.interaction.focus_shown = true;
        state.interaction.focused = search->id;
        const kit::DisplayList field = laid_out(model, state, regular);
        bool ring = false;
        for (const kit::Item& item : field.items) {
            if (item.role == kit::Role::focus_ring)
                ring = true;
        }
        OA_CHECK(!ring);
    }

    picker::ScreenState open_menu;
    open_menu.menu = picker::Menu::filter;
    const kit::DisplayList filtering = laid_out(model, open_menu, compact);
    OA_EXPECT_NONE(outside_window(filtering, window), "filter menu");
    OA_EXPECT_NONE(meeting_controls(filtering), "filter menu");
    OA_EXPECT_NONE(texts_not_fitting(filtering, fonts()), "filter menu");
    OA_CHECK(find_named(filtering, "mutators.filter.all") != nullptr);
    OA_CHECK(find_named(filtering, "mutators.filter.installed") != nullptr);
    OA_CHECK(find_named(filtering, "mutators.filter.downloads") != nullptr);

    picker::ScreenState tags;
    tags.menu = picker::Menu::tags;
    const kit::DisplayList tag_menu = laid_out(model, tags, regular);
    OA_EXPECT_NONE(outside_window(tag_menu, picker::window_size(regular)), "tags menu");
    OA_EXPECT_NONE(meeting_controls(tag_menu), "tags menu");
    const std::vector<std::string> shown = picker::tags_shown(model);
    std::vector<std::string> pills;
    std::vector<std::string> overflow;
    for (const kit::Control& control : tag_menu.controls) {
        if (!starts(control.name, std::string(picker::name_tags) + "."))
            continue;
        if (control.kind == kit::ControlKind::button)
            pills.push_back(control.name.substr(std::string(picker::name_tags).size() + 1));
        if (control.kind == kit::ControlKind::list_item)
            overflow.push_back(control.name.substr(std::string(picker::name_tags).size() + 1));
    }
    std::size_t pill_at = 0;
    std::size_t overflow_at = 0;
    for (const std::string& tag : shown) {
        if (pill_at < pills.size() && pills[pill_at] == tag)
            ++pill_at;
        else if (overflow_at < overflow.size() && overflow[overflow_at] == tag)
            ++overflow_at;
        else {
            std::fprintf(stderr, "tag \"%s\" is neither a pill nor a menu item\n", tag.c_str());
            OA_CHECK(false);
        }
    }
    OA_CHECK(pill_at == pills.size());
    OA_CHECK(overflow_at == overflow.size());
}

} // namespace

int main() {
    const std::vector<FrameCase> cases = frames();
    OA_CHECK(cases.size() == 6);
    OA_CHECK(cases[0].window.x == 480 && cases[0].window.y == 324);
    OA_CHECK(cases[1].window.x == 720 && cases[1].window.y == 486);
    OA_CHECK(cases[2].window.x == 960 && cases[2].window.y == 600);
    OA_CHECK(cases[3].frame.size_class == kit::SizeClass::compact);
    OA_CHECK(cases[3].window.x == 480 && cases[3].window.y == 324);
    OA_CHECK(cases[4].frame.size_class == kit::SizeClass::regular);
    OA_CHECK(cases[4].window.x == 720 && cases[4].window.y == 486);
    OA_CHECK(cases[5].frame.size_class == kit::SizeClass::large);
    OA_CHECK(cases[5].window.x == 960 && cases[5].window.y == 600);

    const picker::Picker model = opened({}, false);
    const std::vector<picker::Row> shown = picker::rows(model);
    OA_CHECK(shown.size() == 6);
    OA_CHECK(model.on.size() == 4);
    if (shown.size() == 6) {
        OA_CHECK(shown[0].id == "fast-builders" && shown[0].on && shown[0].number == 1);
        OA_CHECK(shown[1].id == "scout-post" && shown[1].number == 2);
        OA_CHECK(shown[2].id == "gatehouse" && shown[2].number == 3);
        OA_CHECK(shown[3].id == "gatehouse-keep" && shown[3].on && shown[3].to_download);
        OA_CHECK(shown[4].id == "grounded" && !shown[4].on);
        OA_CHECK(shown[5].id == "ridge-walls" && shown[5].locked);
    }

    for (const FrameCase& one : cases)
        check_frame(model, one.name, one.frame, one.window);

    const kit::Frame regular = cases[1].frame;
    check_band(model, "fast-builders", false, true, regular);
    check_band(model, "scout-post", true, true, regular);
    check_band(model, "gatehouse", true, false, regular);
    check_band(model, "gatehouse-keep", false, false, regular);
    check_band(model, "ridge-walls", false, false, regular);
    check_band(model, "grounded", false, false, regular);

    for (const FrameCase& one : cases)
        check_drag(model, one.frame, one.window);

    check_read_only(regular, cases[1].window);
    check_read_only(cases[0].frame, cases[0].window);
    check_waiting(model, regular);
    check_empty(regular, cases[1].window);
    check_empty(cases[0].frame, cases[0].window);
    check_language(cases[0].frame, cases[0].window, "compact");
    check_language(regular, cases[1].window, "regular");
    check_details_page(model, cases[0].frame, cases[0].window);
    check_focus_and_menu(model, cases[0].frame, regular, cases[0].window);

    picker::ScreenState page_state;
    page_state.details_page = true;
    const picker::Picker guest = opened({}, true);
    const kit::DisplayList guest_page = laid_out(guest, page_state, cases[0].frame);
    OA_CHECK(find_named(guest_page, picker::name_done) != nullptr);
    OA_CHECK(find_named(guest_page, picker::name_back) == nullptr);
    OA_CHECK(find_named(guest_page, picker::name_use) == nullptr);
    check_tabs(guest_page, cases[0].frame, true, "read-only page");

    return oa::test::check_exit_status();
}
