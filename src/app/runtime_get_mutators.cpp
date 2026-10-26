// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// GET MUTATORS over a guest's battle room, on the OA layer: one screen,
// get-mutators, that is not modal, has no backdrop and is not opaque. It
// shows while the guest is in the battle room and the host's set is not
// empty. VIEW… opens the picker read-only on that set. GET MUTATORS queues
// the releases the host named, through the fetch the room already owns, and
// a bar takes its place while one downloads. Nothing readies the player.

#include "mod_install_state.hpp"
#include "oa_layer.hpp"

#include "oa/app/runtime.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/netgame/presence.hpp"
#include "oa/ui/frontend_multiplayer/get_map.hpp"
#include "oa/ui/frontend_multiplayer/mutator_room.hpp"
#include "oa/ui/frontend_multiplayer/screens.hpp"
#include "oa/ui/frontend_state/oa_button_place.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/components_more.hpp"
#include "oa/ui/kit/controls.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/kit/theme.hpp"
#include "oa/ui/mutator_picker/model.hpp"
#include "oa/ui/pad_controls.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {

namespace {

namespace picker = oa::ui::mutator_picker;
namespace place = oa::ui::frontend_state;
namespace mp = oa::ui::frontend_multiplayer;
namespace pad = oa::ui::pad_controls;
namespace kit = oa::ui::kit;

constexpr std::string_view screen_name = "get-mutators";
constexpr std::string_view view_name = "mutators-view";
constexpr std::string_view fetch_name = "get-mutators";
constexpr std::string_view progress_name = "get-mutators-progress";
constexpr std::string_view text_name = "get-mutators-text";
constexpr std::string_view ready_name = "get-mutators-ready";
constexpr std::string_view word_view = "VIEW…";
constexpr std::string_view word_fetch = "GET MUTATORS";
constexpr std::string_view word_prefix = "Mutators:";
constexpr kit::ControlId view_control = 1;
constexpr kit::ControlId fetch_control = 2;
constexpr kit::ControlId text_control = 3;
constexpr kit::ControlId progress_control = 4;
constexpr kit::ControlId ready_control = 5;
constexpr int32_t summary_line_width = 200;
constexpr int32_t least_summary_width = 80;
constexpr int32_t anchor_gap = 4;
constexpr int32_t column_slack = 10;
constexpr int32_t bar_whole = 1000;
constexpr int32_t bar_per_percent = 10;

/// Which button of the row holds the keyboard focus.
enum class Held : uint8_t { none, view, fetch };

/// Where an arrow looks for the next record.
enum class Aim : uint8_t { up, down, left, right };

/// One focusable record of the battle room.
struct FocusNote {
    int32_t index{-1};       ///< the control
    place::PlaceRect at{};   ///< its rectangle, in points
    int32_t frame_x{};       ///< its left edge, in frame pixels
    int32_t frame_y{};       ///< its top edge, in frame pixels
    int32_t focus_place{-1}; ///< its place in the battle room's order
};

/// One mutator of the summary, and whether this machine lacks it.
struct Listed {
    std::string name{}; ///< the name, or the id
    bool lacking{};     ///< this machine does not have the host's release
};

/// Where the row, its line and the ready line lie.
struct Geometry {
    bool ok{};                                  ///< the guest's block shows
    kit::Rect bounds{};                         ///< the screen's place, in points
    kit::Rect view{};                           ///< VIEW…
    kit::Rect fetch{};                          ///< GET MUTATORS, or the bar in its place
    bool fetch_button{};                        ///< GET MUTATORS is a button
    bool bar{};                                 ///< the bar shows in GET MUTATORS' place
    int32_t bar_done{};                         ///< how much of the bar is filled, in thousandths
    std::string view_caption{};                 ///< VIEW… in the language shown
    std::string fetch_caption{};                ///< GET MUTATORS in the language shown
    std::vector<kit::Rect> lines{};             ///< the one line, when it was kept
    bool summary{};                             ///< the line is the set, not a state line
    std::string line{};                         ///< the line, whole and uncut
    kit::Colour line_colour{kit::colour::text}; ///< a state line's colour
    std::vector<Listed> listed{};               ///< the set, in the host's order
    std::vector<FocusNote> focusable{};         ///< the records Tab can stop on
    place::PlaceRect view_points{};             ///< VIEW…, for the order
    place::PlaceRect fetch_points{};            ///< GET MUTATORS, for the order
    int32_t anchor_index{-1};                   ///< Select Map's record
    int32_t before{-1};                         ///< the record just before the row
    int32_t after{-1};                          ///< the record just after the row
    int32_t first{-1};                          ///< the first record in the order
    int32_t last{-1};                           ///< the last record in the order
    int32_t below{-1};                          ///< the next record down Select Map's column
    std::optional<kit::Rect> ready{};           ///< the line in READY's place
    std::string ready_text{};                   ///< that line's words
};

/// Returns one of the block's words in the language shown.
///
/// @param english the words, in English
/// @return their translation, or the English
std::string translated(std::string_view english) {
    return std::string(oa::data::languages::interface_text(english));
}

place::PlaceRect as_place(const kit::Rect& rect) noexcept {
    return {rect.x, rect.y, rect.width, rect.height};
}

kit::Rect as_kit(const place::PlaceRect& rect) noexcept {
    return {rect.x, rect.y, rect.width, rect.height};
}

/// Grows a rectangle by the focus ring's margin.
///
/// @param rect the rectangle
/// @param margin the margin, in points
/// @return the grown rectangle
kit::Rect outset(const kit::Rect& rect, int32_t margin) noexcept {
    return {rect.x - margin, rect.y - margin, rect.width + margin * 2, rect.height + margin * 2};
}

/// Returns the union of two rectangles.
///
/// @param into the rectangle so far; empty when its width is not positive
/// @param extra the rectangle to include
/// @return the union
kit::Rect unite(const kit::Rect& into, const kit::Rect& extra) noexcept {
    if (into.width <= 0 || into.height <= 0)
        return extra;
    const int32_t span_left = std::min(into.x, extra.x);
    const int32_t top = std::min(into.y, extra.y);
    const int32_t right = std::max(into.x + into.width, extra.x + extra.width);
    const int32_t bottom = std::max(into.y + into.height, extra.y + extra.height);
    return {span_left, top, right - span_left, bottom - top};
}

kit::Rect local_of(const kit::Rect& rect, const kit::Rect& origin) noexcept {
    return {rect.x - origin.x, rect.y - origin.y, rect.width, rect.height};
}

int64_t order_key(const place::PlaceRect& rect) noexcept {
    return static_cast<int64_t>(rect.y) * 5000 + rect.x;
}

/// The record just before the row, the first of a shared key.
///
/// @param records the focusable records
/// @param button the left button
/// @return its index, or -1 when none comes before
int32_t record_before(const std::vector<FocusNote>& records, const place::PlaceRect& button) {
    const int64_t key = order_key(button);
    int64_t best = 0;
    int32_t found = -1;
    for (const FocusNote& record : records) {
        const int64_t other = order_key(record.at);
        if (other <= key && (found < 0 || other > best)) {
            best = other;
            found = record.index;
        }
    }
    return found;
}

/// The next record down Select Map's column that sits wholly under the row.
///
/// @param records the focusable records
/// @param anchor_x Select Map's left edge, in frame pixels
/// @param button the row
/// @return its index, or -1 when the column has none under the row
int32_t column_below(
    const std::vector<FocusNote>& records, int32_t anchor_x, const place::PlaceRect& button
) {
    int32_t best_y = 0;
    int32_t found = -1;
    for (const FocusNote& record : records) {
        const int32_t delta = record.frame_x - anchor_x;
        if (delta <= -column_slack || delta >= column_slack)
            continue;
        if (record.at.y < button.y + button.height)
            continue;
        if (found < 0 || record.frame_y < best_y) {
            best_y = record.frame_y;
            found = record.index;
        }
    }
    return found;
}

/// The nearest focusable record in a direction, by the rectangles' centres.
///
/// @param records the focusable records
/// @param button the button the arrow leaves
/// @param aim the direction
/// @return its index, or -1 when none lies that way
int32_t
nearest_record(const std::vector<FocusNote>& records, const place::PlaceRect& button, Aim aim) {
    const int32_t origin_x = button.x + button.width / 2;
    const int32_t origin_y = button.y + button.height / 2;
    int64_t best = 0;
    int32_t found = -1;
    for (const FocusNote& record : records) {
        const int32_t centre_x = record.at.x + record.at.width / 2;
        const int32_t centre_y = record.at.y + record.at.height / 2;
        const int32_t dx = centre_x - origin_x;
        const int32_t dy = centre_y - origin_y;
        int32_t primary = 0;
        int32_t across = 0;
        switch (aim) {
        case Aim::up:
            primary = -dy;
            across = dx;
            break;
        case Aim::down:
            primary = dy;
            across = dx;
            break;
        case Aim::left:
            primary = -dx;
            across = dy;
            break;
        case Aim::right:
            primary = dx;
            across = dy;
            break;
        }
        const int32_t span = primary < 0 ? -primary : primary;
        const int32_t side = across < 0 ? -across : across;
        if (primary <= 0 || span < side)
            continue;
        const int64_t distance = static_cast<int64_t>(dx) * dx + static_cast<int64_t>(dy) * dy;
        if (found < 0 || distance < best) {
            best = distance;
            found = record.index;
        }
    }
    return found;
}

/// Tells whether READY stays off because this guest has not built the host's list.
///
/// The same rule as the battle room's guest_awaits_mutators. The host is
/// never waiting on itself. No mutator field on the host means the room has
/// no mutators. The guest waits when its own record has no field, names
/// another list, or is not built.
///
/// @param lobby the battle room
/// @return true when READY stays off
bool guest_awaits_mutators(mp::Lobby& lobby) noexcept {
    if (mp::lobby_hosts(lobby) || lobby.game == nullptr)
        return false;
    const uint8_t host_slot = mp::lobby_host_slot(lobby);
    if (host_slot == mp::kNoSlot)
        return false;
    const auto host_record = mp::lobby_presence_record(lobby, static_cast<int32_t>(host_slot));
    const auto host_state =
        oa::netgame::presence_mutators_state(host_record.data(), host_record.size());
    if (!host_state)
        return false;
    const auto own_record = mp::lobby_presence_record(lobby, lobby.game->local_player_index);
    const auto own_state =
        oa::netgame::presence_mutators_state(own_record.data(), own_record.size());
    if (!own_state || own_state->digest != host_state->digest)
        return true;
    return !own_state->built;
}

void add_text(
    kit::DisplayList& list,
    const kit::Rect& box,
    std::string text,
    kit::Colour ink,
    kit::Align align
) {
    kit::Item item;
    item.role = kit::Role::text;
    item.rect = box;
    item.text = std::move(text);
    item.font = kit::FontRole::small;
    item.align = align;
    item.colour = ink;
    list.items.push_back(std::move(item));
}

/// Draws the summary from the right, dropping whole names and ending with an ellipsis.
///
/// @param list the display list
/// @param box the line, in the screen's points
/// @param fonts the fonts
/// @param prefix "Mutators:" in the language shown
/// @param listed the set, in the host's order
/// @param full the whole line, uncut, stored on the area
void add_summary(
    kit::DisplayList& list,
    const kit::Rect& box,
    const kit::Fonts& fonts,
    const std::string& prefix,
    const std::vector<Listed>& listed,
    const std::string& full
) {
    const auto measure = [&fonts](std::string_view text) {
        return kit::text_width(fonts, kit::FontRole::small, text);
    };
    const std::string gap = " ";
    const std::string separator = " · ";
    const std::string dots(kit::ellipsis);
    const auto width_of = [&](std::size_t count, bool with_dots) {
        int32_t width = measure(prefix);
        if (count == 0 && !with_dots)
            return width;
        width += measure(gap);
        for (std::size_t at = 0; at < count; ++at) {
            if (at > 0)
                width += measure(separator);
            width += measure(listed[at].name);
        }
        if (with_dots) {
            if (count > 0)
                width += measure(separator);
            width += measure(dots);
        }
        return width;
    };
    std::size_t count = listed.size();
    bool with_dots = false;
    while (count > 0 && width_of(count, false) > box.width) {
        --count;
        with_dots = true;
    }
    while (with_dots && count > 0 && width_of(count, true) > box.width)
        --count;

    struct Run {
        std::string text;
        kit::Colour colour;
    };

    std::vector<Run> runs;
    if (with_dots && width_of(count, true) > box.width) {
        if (measure(dots) <= box.width)
            runs.push_back({dots, kit::colour::text});
    } else {
        runs.push_back({prefix, kit::colour::text});
        if (count > 0 || with_dots)
            runs.push_back({gap, kit::colour::text});
        for (std::size_t at = 0; at < count; ++at) {
            if (at > 0)
                runs.push_back({separator, kit::colour::text});
            runs.push_back(
                {listed[at].name, listed[at].lacking ? kit::colour::lock : kit::colour::text}
            );
        }
        if (with_dots) {
            if (count > 0)
                runs.push_back({separator, kit::colour::text});
            runs.push_back({dots, kit::colour::text});
        }
    }
    int32_t width = 0;
    for (const Run& run : runs)
        width += measure(run.text);
    int32_t cursor = box.x + box.width - width;
    for (const Run& run : runs) {
        const int32_t run_width = measure(run.text);
        add_text(
            list, {cursor, box.y, run_width, box.height}, run.text, run.colour, kit::Align::left
        );
        cursor += run_width;
    }
    kit::Control control;
    control.id = text_control;
    control.rect = box;
    control.enabled = true;
    control.name = std::string(text_name);
    control.kind = kit::ControlKind::area;
    control.focusable = false;
    control.text = full;
    list.controls.push_back(std::move(control));
}

/// The name the summary and the ready line show for one fetched mutator.
///
/// @param item the fetch row
/// @return its offer's name, or its id
std::string item_name(const Runtime::HostMutatorFetch& item) {
    return item.offer.name.empty() ? item.mutator.id : item.offer.name;
}

/// The registry the chat and the downloads-off line name.
///
/// @param item the fetch row
/// @return the registry's name, or its id
std::string item_registry(const Runtime::HostMutatorFetch& item) {
    return item.offer.registry_name.empty() ? item.offer.registry_id : item.offer.registry_name;
}

} // namespace

/// GET MUTATORS as a screen of the OA layer.
///
/// Its points are the layer's. It shows VIEW… and, while a release can be
/// fetched or the last try failed, GET MUTATORS, with one line under them.
/// While a download runs, the bar stands where GET MUTATORS stood.
struct Runtime::GetMutatorsScreen final : LayerScreen {
    /// Makes the screen.
    ///
    /// @param runtime the runtime, whose fetch and picker it uses
    explicit GetMutatorsScreen(Runtime& runtime) : runtime_(runtime) {}

    /// Returns "get-mutators".
    ///
    /// @return the name
    [[nodiscard]] std::string_view name() const override { return screen_name; }

    /// Returns the row, its line and the ready line, in the window.
    ///
    /// @param placed_on what the screen is placed on
    /// @return its place; empty while it does not show
    [[nodiscard]] LayerPlacement placement(const LayerView& placed_on) const override {
        const Geometry laid = geometry(placed_on);
        if (!laid.ok)
            return {};
        const int32_t percent = std::max(placed_on.scale_percent, int32_t{100});
        const kit::Rect shown_at = kit::to_canvas(laid.bounds, percent);
        return {
            {shown_at.x, shown_at.y, shown_at.width, shown_at.height},
            laid.bounds.width,
            laid.bounds.height,
            percent
        };
    }

    /// Tells that what the block does not take goes on.
    ///
    /// @return false
    [[nodiscard]] bool modal() const override { return false; }

    /// Tells that the block darkens nothing.
    ///
    /// @return false
    [[nodiscard]] bool backdrop() const override { return false; }

    /// Tells that the line lies over the picture.
    ///
    /// @return false
    [[nodiscard]] bool opaque() const override { return false; }

    /// Follows the view the block is placed on.
    ///
    /// @param placed_on what the screen is placed on
    void lay_out(const LayerView& placed_on) override { view_ = placed_on; }

    /// Draws the row, the line and, while it holds the focus, the ring.
    ///
    /// @param canvas where it draws
    void draw(const kit::Canvas& canvas) const override { kit::paint(canvas, display_list()); }

    /// Takes a press on VIEW… or GET MUTATORS. A press anywhere else goes on.
    ///
    /// @param kind pointer_move, pointer_down or pointer_up
    /// @param button the pointer's button
    /// @param at the pointer, in the screen's points
    /// @return redraw for the block's own press; pass otherwise
    LayerAnswer pointer(
        ScreenInputKind kind, uint8_t button, kit::Point at, int32_t /*finger_reach*/
    ) override {
        const Geometry laid = geometry(view_);
        const kit::ControlId over = hit(laid, at);
        if (over != hovered_) {
            hovered_ = over;
            ++revision_;
        }
        if (kind == ScreenInputKind::pointer_down && button == SDL_BUTTON_LEFT) {
            if (over == view_control || over == fetch_control) {
                pressed_ = over;
                ++revision_;
                return LayerAnswer::redraw;
            }
            pressed_ = kit::no_control;
            if (focused_ != Held::none) {
                focused_ = Held::none;
                ++revision_;
            }
            return LayerAnswer::pass;
        }
        if (kind == ScreenInputKind::pointer_up && button == SDL_BUTTON_LEFT &&
            pressed_ != kit::no_control) {
            const kit::ControlId held = pressed_;
            pressed_ = kit::no_control;
            ++revision_;
            if (over == held) {
                if (held == view_control)
                    open_view();
                else
                    runtime_.fetch_host_mutators();
            }
            return LayerAnswer::redraw;
        }
        return LayerAnswer::pass;
    }

    /// Takes the keys the row holds the focus for, and Y, which opens VIEW….
    ///
    /// @param pressed the key
    /// @return none or redraw when the row takes it; pass otherwise
    LayerAnswer key(kit::Key pressed, uint32_t /*sdl_key*/) override { return answer_key(pressed); }

    /// Lets every shortcut go on.
    ///
    /// @return pass
    LayerAnswer shortcut(uint32_t /*sdl_key*/, uint16_t /*modifiers*/) override {
        return LayerAnswer::pass;
    }

    /// Takes Y, and the focus keys, on the same terms as the keyboard.
    ///
    /// @param action the gamepad action
    /// @return none or redraw when the row takes it; pass otherwise
    LayerAnswer pad_action(pad::Action action) override {
        if (action == pad::Action::library_search)
            return answer_key(kit::Key::yes);
        if (action == pad::Action::press)
            return answer_key(kit::Key::enter);
        if (action == pad::Action::focus_next)
            return answer_key(kit::Key::tab);
        if (action == pad::Action::focus_previous)
            return answer_key(kit::Key::back_tab);
        if (action == pad::Action::focus_up)
            return answer_key(kit::Key::up);
        if (action == pad::Action::focus_down)
            return answer_key(kit::Key::down);
        if (action == pad::Action::focus_left)
            return answer_key(kit::Key::left);
        if (action == pad::Action::focus_right)
            return answer_key(kit::Key::right);
        return LayerAnswer::pass;
    }

    /// Lets the wheel go on.
    ///
    /// @return pass
    LayerAnswer wheel(kit::Point /*at*/, float /*notches*/) override { return LayerAnswer::pass; }

    /// Returns the row, the line, the bar and the ready line.
    ///
    /// VIEW… is mutators-view. GET MUTATORS is get-mutators. The bar is
    /// get-mutators-progress and its text is the percent. The line is
    /// get-mutators-text and stores the whole line, uncut. The ready line is
    /// get-mutators-ready. The endpoint adds oa. to each name.
    ///
    /// @return the display list
    [[nodiscard]] kit::DisplayList display_list() const override {
        kit::DisplayList list;
        const Geometry laid = geometry(view_);
        const kit::Fonts* fonts = runtime_.oa_layer().screen_fonts();
        if (!laid.ok || fonts == nullptr)
            return list;
        const kit::Rect view_rect = local_of(laid.view, laid.bounds);
        kit::ButtonLook view_look;
        view_look.caption = laid.view_caption;
        view_look.style = kit::ButtonStyle::inset;
        view_look.hovered = hovered_ == view_control;
        view_look.held = pressed_ == view_control && view_look.hovered;
        kit::add_button(list, view_rect, view_look, view_control, std::string(view_name));
        list.tab_order.push_back(view_control);
        if (laid.fetch_button) {
            const kit::Rect fetch = local_of(laid.fetch, laid.bounds);
            kit::ButtonLook fetch_look;
            fetch_look.caption = laid.fetch_caption;
            fetch_look.style = kit::ButtonStyle::accent;
            fetch_look.hovered = hovered_ == fetch_control;
            fetch_look.held = pressed_ == fetch_control && fetch_look.hovered;
            kit::add_button(list, fetch, fetch_look, fetch_control, std::string(fetch_name));
            list.tab_order.push_back(fetch_control);
        }
        if (laid.bar) {
            const kit::Rect bar = local_of(laid.fetch, laid.bounds);
            kit::add_progress(list, bar, laid.bar_done, bar_whole);
            kit::Control progress;
            progress.id = progress_control;
            progress.rect = bar;
            progress.enabled = false;
            progress.focusable = false;
            progress.name = std::string(progress_name);
            progress.kind = kit::ControlKind::area;
            progress.text = std::to_string(laid.bar_done / bar_per_percent) + "%";
            list.controls.push_back(std::move(progress));
        }
        if (!laid.lines.empty()) {
            const kit::Rect box = local_of(laid.lines.front(), laid.bounds);
            if (laid.summary) {
                add_summary(list, box, *fonts, translated(word_prefix), laid.listed, laid.line);
            } else {
                const auto measure = [&fonts](std::string_view text) {
                    return kit::text_width(*fonts, kit::FontRole::small, text);
                };
                add_text(
                    list,
                    box,
                    kit::fit(laid.line, box.width, measure),
                    laid.line_colour,
                    kit::Align::right
                );
                kit::Control said;
                said.id = text_control;
                said.rect = box;
                said.enabled = false;
                said.focusable = false;
                said.name = std::string(text_name);
                said.kind = kit::ControlKind::area;
                said.text = laid.line;
                list.controls.push_back(std::move(said));
            }
        }
        if (laid.ready) {
            const kit::Rect box = local_of(*laid.ready, laid.bounds);
            const auto measure = [&fonts](std::string_view text) {
                return kit::text_width(*fonts, kit::FontRole::small, text);
            };
            add_text(
                list,
                box,
                kit::fit(laid.ready_text, box.width, measure),
                kit::colour::lock,
                kit::Align::right
            );
            kit::Control waiting;
            waiting.id = ready_control;
            waiting.rect = box;
            waiting.enabled = false;
            waiting.focusable = false;
            waiting.name = std::string(ready_name);
            waiting.kind = kit::ControlKind::area;
            waiting.text = laid.ready_text;
            list.controls.push_back(std::move(waiting));
        }
        if (focused_ == Held::view)
            kit::add_focus_ring(list, view_rect, true);
        else if (focused_ == Held::fetch && laid.fetch_button)
            kit::add_focus_ring(list, local_of(laid.fetch, laid.bounds), true);
        return list;
    }

    /// Returns the row's hover, press and focus.
    ///
    /// @return the interaction
    [[nodiscard]] kit::Interaction interaction() const override {
        kit::Interaction shown_state;
        shown_state.hovered = hovered_;
        shown_state.pressed = pressed_;
        if (focused_ == Held::view)
            shown_state.focused = view_control;
        else if (focused_ == Held::fetch)
            shown_state.focused = fetch_control;
        shown_state.focus_shown = focused_ != Held::none;
        return shown_state;
    }

    /// Closes once the multiplayer screens go. Gives up the focus once the
    /// battle room's own focus is on a record again, and once GET MUTATORS
    /// is no longer a button.
    ///
    /// @return close when it should leave; redraw when what it shows changed; otherwise none
    LayerAnswer tick() override {
        if (!mp::multiplayer_showing())
            return LayerAnswer::close;
        const uint64_t before = revision_;
        const Geometry laid = geometry(view_);
        const Held was = focused_;
        if (focused_ != Held::none && mp::multiplayer_battle_room_focus() > 0)
            focused_ = Held::none;
        if (focused_ == Held::fetch && !laid.fetch_button)
            focused_ = Held::none;
        const std::string stamp = stamp_of(laid);
        if (focused_ != was || stamp != stamp_) {
            stamp_ = std::move(stamp);
            ++revision_;
        }
        return revision_ != before ? LayerAnswer::redraw : LayerAnswer::none;
    }

    /// Returns the block's revision.
    ///
    /// @return the revision
    [[nodiscard]] uint64_t revision() const override { return revision_; }

    /// Has nothing of its own to close. A download goes on.
    void close(bool /*by_key*/) override {}

  private:

    /// Places the guest's block above Select Map, missing the room and GET MAP.
    ///
    /// @param placed_on what the screen is placed on
    /// @return the place; not ok when the guest has nothing to show
    [[nodiscard]] Geometry geometry(const LayerView& placed_on) const {
        Geometry laid;
        const kit::Fonts* fonts = runtime_.oa_layer().screen_fonts();
        if (fonts == nullptr || placed_on.picture_size.x <= 0 || placed_on.match)
            return laid;
        mp::BattleRoomMap map;
        if (!mp::multiplayer_battle_room_map(&map) || !map.joiner)
            return laid;
        const std::vector<Runtime::HostMutatorFetch> fetch = runtime_.host_mutators_fetch();
        if (fetch.empty())
            return laid;

        std::vector<mp::MutatorFetch> states;
        states.reserve(fetch.size());
        const Runtime::HostMutatorFetch* deciding = nullptr;
        laid.listed.reserve(fetch.size());
        for (const Runtime::HostMutatorFetch& item : fetch) {
            states.push_back(item.state);
            Listed one;
            one.name = item_name(item);
            one.lacking = item.state != mp::MutatorFetch::have;
            if (deciding == nullptr && one.lacking)
                deciding = &item;
            laid.listed.push_back(std::move(one));
        }
        const mp::GetMutators stage = mp::get_mutators_state(states);
        laid.fetch_button = mp::get_mutators_button_shown(stage);
        laid.bar = stage == mp::GetMutators::downloading;
        std::string deciding_name;
        std::string deciding_registry;
        std::string problem;
        int32_t percent = 0;
        if (deciding != nullptr) {
            deciding_name = item_name(*deciding);
            deciding_registry = item_registry(*deciding);
            problem = deciding->failure;
            if (deciding->offer.bytes > 0) {
                const uint64_t bytes = deciding->offer.bytes;
                const uint64_t done = std::min(deciding->done, bytes);
                percent = static_cast<int32_t>(done * 100 / bytes);
                laid.bar_done = static_cast<int32_t>(done * bar_whole / bytes);
            }
        }
        laid.summary = stage == mp::GetMutators::none || stage == mp::GetMutators::get;
        if (laid.summary) {
            const std::string prefix = translated(word_prefix);
            laid.line = prefix;
            for (std::size_t at = 0; at < laid.listed.size(); ++at) {
                laid.line += at == 0 ? " " : " · ";
                laid.line += laid.listed[at].name;
            }
        } else {
            laid.line =
                mp::get_mutators_text(stage, deciding_name, percent, problem, deciding_registry);
            laid.line_colour =
                stage == mp::GetMutators::downloading ? kit::colour::hint : kit::colour::lock;
        }

        const std::vector<mp::BattleRoomControl> controls = mp::multiplayer_battle_room_controls();
        std::vector<place::PlaceRect> others;
        std::vector<place::PlaceRect> reason_others;
        place::PlaceRect anchor{};
        place::PlaceRect start_rect{};
        bool found = false;
        bool have_start = false;
        int32_t anchor_frame_x = 0;
        others.reserve(controls.size() + 1);
        for (const mp::BattleRoomControl& control : controls) {
            const kit::Rect frame{control.x, control.y, control.width, control.height};
            const kit::Rect points = frame_to_points(placed_on, frame);
            if (points.width <= 0 || points.height <= 0)
                continue;
            const bool map_control = Runtime::tdf_names_equal(control.name, "MAP");
            const bool start = Runtime::tdf_names_equal(control.name, "START");
            if (map_control && !found) {
                anchor = as_place(points);
                anchor_frame_x = control.x;
                laid.anchor_index = control.index;
                found = true;
            } else {
                others.push_back(as_place(points));
            }
            if (start && !have_start) {
                start_rect = as_place(points);
                have_start = true;
            } else {
                reason_others.push_back(as_place(points));
            }
            if (control.focus_place >= 0) {
                FocusNote record;
                record.index = control.index;
                record.at = as_place(points);
                record.frame_x = control.x;
                record.frame_y = control.y;
                record.focus_place = control.focus_place;
                laid.focusable.push_back(record);
            }
        }
        if (!found)
            return laid;
        const Runtime::ShownBox map_box = runtime_.get_map_box();
        if (map_box.width > 0 && map_box.height > 0) {
            const place::PlaceRect box{map_box.x, map_box.y, map_box.width, map_box.height};
            others.push_back(box);
            reason_others.push_back(box);
        }

        const kit::Metrics& metrics = kit::compact_metrics;
        laid.view_caption = translated(word_view);
        laid.fetch_caption = translated(word_fetch);
        const int32_t view_width = std::max(
            metrics.button_width,
            kit::text_width(*fonts, kit::FontRole::small, laid.view_caption) +
                metrics.prompt_button_padding
        );
        const int32_t fetch_width = std::max(
            metrics.button_width,
            kit::text_width(*fonts, kit::FontRole::small, laid.fetch_caption) +
                metrics.prompt_button_padding
        );
        const bool two = laid.fetch_button || laid.bar;
        place::BlockSizes sizes{
            view_width,
            metrics.button_height,
            metrics.small_line,
            summary_line_width,
            least_summary_width,
            anchor_gap,
            1,
            0
        };
        sizes.button_gap = metrics.button_gap;
        if (two)
            sizes.button_widths = {view_width, fetch_width};
        else
            sizes.button_widths = {view_width};
        const kit::Rect room_frame{0, 0, placed_on.picture_size.x, placed_on.picture_size.y};
        const kit::Rect room = frame_to_points(placed_on, room_frame);
        const place::BlockPlace block =
            mp::place_over_anchor(anchor, others, as_place(room), sizes);
        if (block.buttons.empty())
            return laid;
        laid.view = as_kit(block.buttons.front());
        laid.view_points = block.buttons.front();
        if (two && block.buttons.size() >= 2) {
            laid.fetch = as_kit(block.buttons[1]);
            laid.fetch_points = block.buttons[1];
        }
        laid.lines.reserve(block.lines.size());
        for (const place::PlaceRect& line : block.lines)
            laid.lines.push_back(as_kit(line));
        kit::Rect bounds = outset(laid.view, metrics.focus_inset);
        if (two)
            bounds = unite(bounds, outset(laid.fetch, metrics.focus_inset));
        for (const kit::Rect& line : laid.lines)
            bounds = unite(bounds, line);
        laid.before = record_before(laid.focusable, laid.view_points);
        int32_t before_place = -1;
        for (const FocusNote& record : laid.focusable) {
            if (record.index == laid.before)
                before_place = record.focus_place;
        }
        if (before_place >= 0) {
            for (const FocusNote& record : laid.focusable) {
                if (record.focus_place == before_place + 1)
                    laid.after = record.index;
            }
        }
        int32_t earliest = 0;
        int32_t latest = -1;
        for (const FocusNote& record : laid.focusable) {
            if (laid.first < 0 || record.focus_place < earliest) {
                earliest = record.focus_place;
                laid.first = record.index;
            }
            if (record.focus_place > latest) {
                latest = record.focus_place;
                laid.last = record.index;
            }
        }
        laid.below = column_below(laid.focusable, anchor_frame_x, laid.view_points);
        if (have_start && deciding != nullptr && guest_awaits_mutators(mp::multiplayer_lobby())) {
            const place::PlaceRect box = mp::reason_box(
                start_rect, reason_others, metrics.small_line, summary_line_width * 2
            );
            if (box.width > 0 && box.height > 0) {
                laid.ready = as_kit(box);
                laid.ready_text = mp::guest_ready_line(deciding_name);
                bounds = unite(bounds, *laid.ready);
            }
        }
        laid.bounds = bounds;
        laid.ok = bounds.width > 0 && bounds.height > 0;
        return laid;
    }

    /// The control under a point of the screen, or none.
    ///
    /// @param laid the place
    /// @param at the point, in the screen's points
    /// @return the control
    [[nodiscard]] kit::ControlId hit(const Geometry& laid, kit::Point at) const noexcept {
        if (!laid.ok)
            return kit::no_control;
        if (kit::contains(local_of(laid.view, laid.bounds), at))
            return view_control;
        if (laid.fetch_button && kit::contains(local_of(laid.fetch, laid.bounds), at))
            return fetch_control;
        return kit::no_control;
    }

    /// Takes the focus onto a button and clears the battle room's ring.
    ///
    /// @param which the button
    /// @return redraw
    LayerAnswer take_focus(Held which) {
        focused_ = which;
        pressed_ = kit::no_control;
        mp::multiplayer_set_battle_room_focus(mp::kNoControl);
        ++revision_;
        return LayerAnswer::redraw;
    }

    /// Hands the focus to a record and clears the row's ring.
    ///
    /// @param index the record
    /// @return redraw
    LayerAnswer hand_focus(int32_t index) {
        focused_ = Held::none;
        mp::multiplayer_set_battle_room_focus(index);
        ++revision_;
        return LayerAnswer::redraw;
    }

    /// Answers one key, for the keyboard and for the pad actions that become keys.
    ///
    /// @param pressed the key
    /// @return none or redraw when the row takes it; pass otherwise
    LayerAnswer answer_key(kit::Key pressed) {
        const Geometry laid = geometry(view_);
        if (!laid.ok)
            return LayerAnswer::pass;
        if (pressed == kit::Key::yes) {
            open_view();
            return LayerAnswer::none;
        }
        const int32_t current = mp::multiplayer_battle_room_focus();
        if (focused_ == Held::none) {
            if (pressed == kit::Key::tab && current == laid.before && laid.before > 0)
                return take_focus(Held::view);
            if (pressed == kit::Key::back_tab && current == laid.after && laid.after > 0)
                return take_focus(laid.fetch_button ? Held::fetch : Held::view);
            if (pressed == kit::Key::down && current == laid.anchor_index && laid.anchor_index > 0)
                return take_focus(Held::view);
            if (pressed == kit::Key::up && current == laid.below && laid.below > 0)
                return take_focus(Held::view);
            return LayerAnswer::pass;
        }
        if (pressed == kit::Key::enter || pressed == kit::Key::space) {
            if (focused_ == Held::fetch && laid.fetch_button)
                runtime_.fetch_host_mutators();
            else
                open_view();
            return LayerAnswer::none;
        }
        if (pressed == kit::Key::tab) {
            if (focused_ == Held::view && laid.fetch_button)
                return take_focus(Held::fetch);
            const int32_t next = laid.after > 0 ? laid.after : laid.first;
            return next > 0 ? hand_focus(next) : LayerAnswer::none;
        }
        if (pressed == kit::Key::back_tab) {
            if (focused_ == Held::fetch)
                return take_focus(Held::view);
            const int32_t next = laid.before > 0 ? laid.before : laid.last;
            return next > 0 ? hand_focus(next) : LayerAnswer::none;
        }
        const place::PlaceRect from =
            focused_ == Held::fetch && laid.fetch_button ? laid.fetch_points : laid.view_points;
        if (pressed == kit::Key::left) {
            if (focused_ == Held::fetch)
                return take_focus(Held::view);
            const int32_t next = nearest_record(laid.focusable, from, Aim::left);
            return next > 0 ? hand_focus(next) : LayerAnswer::none;
        }
        if (pressed == kit::Key::right) {
            if (focused_ == Held::view && laid.fetch_button)
                return take_focus(Held::fetch);
            const int32_t next = nearest_record(laid.focusable, from, Aim::right);
            return next > 0 ? hand_focus(next) : LayerAnswer::none;
        }
        if (pressed == kit::Key::up || pressed == kit::Key::down) {
            const int32_t next =
                nearest_record(laid.focusable, from, pressed == kit::Key::up ? Aim::up : Aim::down);
            return next > 0 ? hand_focus(next) : LayerAnswer::none;
        }
        return LayerAnswer::pass;
    }

    /// Opens the picker read-only on the host's set. USE THESE is not offered.
    void open_view() {
        oa::netgame::PresenceLoadoutAssembly assembly;
        if (!mp::multiplayer_room_mutators(&assembly))
            return;
        MutatorPickerRequest request;
        request.setup = picker::Setup::multiplayer;
        request.read_only = true;
        request.set.reserve(assembly.mutators.size());
        for (const oa::netgame::PresenceMutator& item : assembly.mutators) {
            picker::Chosen chosen;
            chosen.id = item.id;
            chosen.options.reserve(item.options.size());
            for (const oa::netgame::PresenceMutatorOption& option : item.options)
                chosen.options.emplace_back(option.key, option.value);
            request.set.push_back(std::move(chosen));
        }
        runtime_.open_mutator_picker(std::move(request));
    }

    /// A text that changes when what the block shows changes.
    ///
    /// @param laid the place
    /// @return the text
    [[nodiscard]] static std::string stamp_of(const Geometry& laid) {
        std::string stamp = laid.ok ? "1" : "0";
        stamp += laid.fetch_button ? "b" : "";
        stamp += laid.bar ? "p" : "";
        stamp += std::to_string(laid.bar_done);
        stamp += laid.line;
        stamp += laid.ready_text;
        stamp += std::to_string(laid.view.x);
        stamp += std::to_string(laid.view.y);
        stamp += std::to_string(laid.fetch.x);
        stamp += std::to_string(laid.fetch.y);
        return stamp;
    }

    Runtime& runtime_;                        ///< the runtime
    LayerView view_{};                        ///< the view it was last laid out for
    kit::ControlId hovered_{kit::no_control}; ///< the button under the pointer
    kit::ControlId pressed_{kit::no_control}; ///< the button a press is held on
    Held focused_{Held::none};                ///< the button the keyboard focus is on
    uint64_t revision_{};                     ///< counts the changes to how it looks
    std::string stamp_{};                     ///< what it last showed
};

bool Runtime::ModInstallState::get_mutators_shows(const Runtime& runtime) {
    // The battle room shows with no dialog over it, and the guest's block has
    // a place there: it shows nowhere while the host's list is empty.
    mp::BattleRoomMap room;
    if (!runtime.oa_layer_ || !mp::multiplayer_battle_room_map(&room) || !room.joiner)
        return false;
    const LayerPlacement placed = runtime.oa_layer_->placement_of(screen_name);
    return placed.shown.width > 0 && placed.shown.height > 0;
}

void Runtime::keep_get_mutators_screen() {
    if (screen_ == Screen::match || !mp::multiplayer_showing())
        return;
    OaLayer& layer = oa_layer();
    if (layer.find(screen_name) != nullptr)
        return;
    layer.push(std::make_unique<GetMutatorsScreen>(*this));
}

} // namespace oa::app
