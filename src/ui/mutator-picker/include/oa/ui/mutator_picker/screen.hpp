// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The mutator picker's screen, built only from the UI kit's components: a
// modal window at the size class's dialog size, the filters and the search,
// the On rows numbered above the Off rows, the selected mutator's details,
// and the footer. Nothing here takes a pointer, a key or a wheel, and
// nothing hosts the screen. A host lays it out for its frame and draws it.
// Every player word is looked up in the interface catalogue. Which rows
// show, their order, their lines and what is locked come from the model.
#pragma once

#include "oa/ui/kit/canvas.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/mutator_picker/model.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace oa::ui::mutator_picker {

/// The picker's name on the OA layer. Its controls' dialog is oa. and it.
inline constexpr std::string_view screen_name = "mutator-picker";

// The controls' automation names, which journeys press as oa. and the name.

/// Compact's filter drop-down. Its items are all, installed, downloads and
/// tag-<tag> for each tag the filter keeps.
inline constexpr std::string_view name_filter = "mutators.filter";
/// The All pill.
inline constexpr std::string_view name_filter_all = "mutators.filter.all";
/// The Installed pill.
inline constexpr std::string_view name_filter_installed = "mutators.filter.installed";
/// The Downloads pill.
inline constexpr std::string_view name_filter_downloads = "mutators.filter.downloads";
/// TAGS. Its items are mutators.tags.<tag> for the tags that do not fit as pills.
inline constexpr std::string_view name_tags = "mutators.tags";
/// The search field.
inline constexpr std::string_view name_search = "mutators.search";
/// The list. Its rows are mutators.row.<id>.
inline constexpr std::string_view name_list = "mutators.list";
/// What a row's name starts with. The mutator's id, as a name word, follows.
inline constexpr std::string_view name_row_prefix = "mutators.row.";
/// What follows a row's name for its drag handle. A button, out of Tab's order.
inline constexpr std::string_view name_grip_suffix = ".grip";
/// What follows a row's name for its switch. A toggle, parts off and on.
inline constexpr std::string_view name_switch_suffix = ".switch";
/// What an option control's name starts with. The option's id, as a name word, follows.
inline constexpr std::string_view name_option_prefix = "mutators.option.";
/// Add or Replace, on a mutator that brings units or weapons. Parts `add` and `replace`.
inline constexpr std::string_view name_clash = "mutators.clash";
/// ▲ UP.
inline constexpr std::string_view name_up = "mutators.up";
/// ▼ DOWN.
inline constexpr std::string_view name_down = "mutators.down";
/// Compact's DETAILS.
inline constexpr std::string_view name_details = "mutators.details";
/// The details page's BACK.
inline constexpr std::string_view name_back = "mutators.back";
/// CANCEL.
inline constexpr std::string_view name_cancel = "mutators.cancel";
/// USE THESE.
inline constexpr std::string_view name_use = "mutators.use";
/// Read-only's CLOSE.
inline constexpr std::string_view name_done = "mutators.done";
/// The header's close mark.
inline constexpr std::string_view name_close = "mutators.close";
/// The hint. Kind area: it takes no press, and its text is the words shown.
inline constexpr std::string_view name_hint = "mutators.hint";
/// The footer's summary. Kind area, as the hint is.
inline constexpr std::string_view name_summary = "mutators.summary";
/// The verdict chip's words. Kind area, as the hint is.
inline constexpr std::string_view name_verdict = "mutators.verdict";
/// The list's scroll bar. It takes no focus.
inline constexpr std::string_view name_list_bar = "mutators.list.bar";
/// The details' scroll bar. It takes no focus.
inline constexpr std::string_view name_details_bar = "mutators.details.bar";

/// The list pane's width at Regular, in points.
inline constexpr int32_t regular_list_width = 400;
/// The list pane's width at Large, in points.
inline constexpr int32_t large_list_width = 440;
/// The search field's width at Regular, in points.
inline constexpr int32_t regular_search_width = 220;
/// The search field's width at Large, in points.
inline constexpr int32_t large_search_width = 280;
/// Compact's filter drop-down's width, in points.
inline constexpr int32_t compact_filter_width = 128;
/// A row's height at Compact, in points.
inline constexpr int32_t compact_row_height = 32;
/// A row's height at Regular, in points.
inline constexpr int32_t regular_row_height = 34;
/// A row's height at Large, in points.
inline constexpr int32_t large_row_height = 36;
/// The least width of the number column at Regular and Large, in points.
/// A layout widens it to the widest On number it shows.
inline constexpr int32_t number_column = 12;
/// The least width of the number column at Compact, in points.
inline constexpr int32_t compact_number_column = 9;
/// A row's badge, in points. A square.
inline constexpr int32_t row_badge_side = 20;
/// The details' badge, in points. A square.
inline constexpr int32_t details_badge_side = 40;

/// Which drop-down is open. One menu is open at a time.
enum class Menu : uint8_t {
    none,   ///< no menu
    filter, ///< Compact's filter drop-down
    tags,   ///< TAGS
    choice, ///< one option's drop-down; menu_option names it
};

/// A row being dragged, as the screen draws it. The host, or the input
/// later, fills it. The layout does not move the model.
struct DragShown {
    std::string id{};             ///< the mutator's id
    int32_t y{};                  ///< the row's top, in window points
    std::optional<Drop> target{}; ///< where it would land; none draws no line
};

/// What the picker's screen keeps between layouts, beside the model.
struct ScreenState {
    kit::Interaction interaction{};  ///< the hover, the press and the focus
    int32_t list_scroll{};           ///< how far the list is scrolled, in points
    int32_t details_scroll{};        ///< how far the details are scrolled, in points
    kit::WheelCarry list_wheel{};    ///< the wheel's turn the list has not scrolled yet
    kit::WheelCarry details_wheel{}; ///< the wheel's turn the details have not scrolled yet
    /// The search field's text and caret. The host keeps its text the same
    /// as the model's query.
    kit::TextField search{};
    Menu menu{Menu::none};     ///< which drop-down is open
    std::string menu_option{}; ///< the option, when menu is choice
    int32_t menu_first{};      ///< the first item the open menu shows
    int32_t menu_marked{-1};   ///< the item the pointer or the keys mark; -1 is none
    /// Compact shows the selected mutator's details on their own page.
    bool details_page{false};
    /// Which scroll bar a press holds.
    enum class Bar : uint8_t {
        none,    ///< none
        list,    ///< the list's
        details, ///< the details'
    };
    Bar bar{Bar::none}; ///< the held scroll bar
    int32_t bar_grab{}; ///< where on the thumb the pointer took hold, from its top
    /// Set while a row is being dragged. Empty when none is.
    std::optional<DragShown> drag{};
    /// Why USE THESE waits. Empty when it may act. While it is set the
    /// summary reads this and USE THESE is disabled.
    std::string waiting{};
};

/// Returns the window's size for a frame: the size class's dialog size
/// (kit::metrics_of), no larger than the frame's area.
///
/// @param frame the frame the layer gives
/// @return the width and height, in points
[[nodiscard]] kit::Point window_size(const kit::Frame& frame) noexcept;

/// Lays the picker out for a frame.
///
/// The window, from its top left corner: the header titled "Mutators" with
/// the setup and the mod at its right, and a close mark; the filters and
/// the search; the list, On rows numbered above the Off rows, each with a
/// drag handle, a badge, a name, one line and a switch; the selected
/// mutator's details beside the list at Regular and Large; and the footer.
/// At Compact the details are a page of their own: while details_page is
/// set this returns what details_page_layout returns. Every control is
/// named (the names above). Tab's order is the filters, TAGS, the search,
/// the list, the details' option controls, the clash mode when the mutator
/// brings units or weapons, ▲ UP, ▼ DOWN, the footer's buttons and the
/// close mark. At Compact it is the drop-down, the search, the list,
/// DETAILS, CANCEL, USE THESE and the close mark. The grips and the labels
/// take no focus, and neither does a row's switch. A drag draws the row
/// whole inside the list's view, and leaves its controls on the well.
///
/// @param picker the picker
/// @param state the screen's state
/// @param frame the frame the layer gives
/// @param fonts the fonts the texts are measured in
/// @return the display list, in the window's points
[[nodiscard]] kit::DisplayList picker_layout(
    const Picker& picker, const ScreenState& state, const kit::Frame& frame, const kit::Fonts& fonts
);

/// Lays out Compact's details page: the header, the selected mutator's
/// details, and BACK and USE THESE.
///
/// picker_layout calls this while details_page is set at Compact. Tab's
/// order is the option controls, the clash mode when the mutator brings
/// units or weapons, ▲ UP, ▼ DOWN, BACK, USE THESE and the close mark. A
/// read-only page shows the summary and CLOSE alone.
///
/// @param picker the picker
/// @param state the screen's state
/// @param frame the frame the layer gives
/// @param fonts the fonts the texts are measured in
/// @return the display list, in the window's points
[[nodiscard]] kit::DisplayList details_page_layout(
    const Picker& picker, const ScreenState& state, const kit::Frame& frame, const kit::Fonts& fonts
);

/// Draws the picker: the window's face, the display list, then the window's
/// edge (the kit's chrome), from the canvas's origin.
///
/// @param canvas where it is drawn
/// @param list what picker_layout or details_page_layout gave
/// @param size the window's size, as window_size gave it
void draw_picker(const kit::Canvas& canvas, const kit::DisplayList& list, kit::Point size);

} // namespace oa::ui::mutator_picker
