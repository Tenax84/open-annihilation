// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The selection bar in a running match (selection_bar.hpp), shown while
// User Interface's Selection bar is on: drawn on the
// battlefield's layer along the bottom of the overlays' area, after the
// units' bars and before the status strip, which rises over it; the
// pointer on it is no battlefield's (placed_hud_covers, refresh_pointer_area),
// and its buttons give the selections of the game's own keys: the groups'
// Ctrl+digit and Alt+digit, and Ctrl+A, C, V, B, F, S and Z
// (run_selection_command, which the keys run too).

#include "selection_bar.hpp"

#include "engine_settings_state.hpp"
#include "oa/app/runtime.hpp"
#include "oa/sim/selection.hpp"
#include "oa/ui/console/game_fields.hpp"
#include "oa/ui/display_layout.hpp"
#include "touch_state.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace oa::app {

namespace {

namespace kit = oa::ui::kit;
namespace bar = oa::app::selection_bar;

/// The selection behind each of the bar's selection buttons, by its Command.
constexpr std::array<SelectionCommand, bar::command_count> bar_commands{
    SelectionCommand::all,       SelectionCommand::commander,
    SelectionCommand::air,       SelectionCommand::combat,
    SelectionCommand::builders,  SelectionCommand::factories,
    SelectionCommand::on_screen, SelectionCommand::same_type,
};

/// The keys of the selection buttons, by their Command; empty for none.
constexpr std::array<std::string_view, bar::command_count> command_keys{
    "Ctrl+A", "Ctrl+C", "Ctrl+V", "", "Ctrl+B", "Ctrl+F", "Ctrl+S", "Ctrl+Z",
};

/// Returns a canvas that measures text in the game's fonts, drawing nothing.
///
/// @param fonts the fonts
/// @return the canvas
kit::Canvas measuring_canvas(const kit::Fonts* fonts) {
    kit::Canvas canvas;
    canvas.fonts = fonts;
    return canvas;
}

/// Gives each selection button its key. With ui.selection-shortcuts,
/// Ctrl+S, Ctrl+B and Ctrl+F make other selections than the buttons do, so
/// those buttons name no key.
///
/// @param[in,out] facts what the bar shows; its keys are set
/// @param shortcuts_on ui.selection-shortcuts is on
void name_keys(bar::Facts& facts, bool shortcuts_on) {
    for (std::size_t command = 0; command < bar::command_count; ++command) {
        const auto which = bar_commands[command];
        const bool changed = which == SelectionCommand::on_screen ||
                             which == SelectionCommand::builders ||
                             which == SelectionCommand::factories;
        if (!(shortcuts_on && changed))
            facts.keys[command] = std::string(command_keys[command]);
    }
}

} // namespace

void Runtime::run_selection_command(SelectionCommand command, bool shift) {
    if (!match_)
        return;
    switch (command) {
    case SelectionCommand::all:
        select_units_matching([](const oa::sim::unit_spawn::Slot&) { return true; });
        return;
    case SelectionCommand::commander:
        select_and_follow_commander(shift);
        return;
    case SelectionCommand::air:
        select_units_matching([&](const oa::sim::unit_spawn::Slot& slot) {
            const auto* def = definition_for(slot.unit_index);
            return def && def->can_fly;
        });
        return;
    case SelectionCommand::combat:
        select_units_matching([&](const oa::sim::unit_spawn::Slot& slot) {
            const auto* def = definition_for(slot.unit_index);
            return def && !def->can_fly && !def->builder && !def->commander &&
                   def->max_velocity_fixed > 0 && (def->can_attack || !def->weapon1.empty());
        });
        return;
    case SelectionCommand::builders:
        select_units_matching([&](const oa::sim::unit_spawn::Slot& slot) {
            const auto* def = definition_for(slot.unit_index);
            return def && def->builder && def->bm_code != 0;
        });
        return;
    case SelectionCommand::factories:
        select_units_matching([&](const oa::sim::unit_spawn::Slot& slot) {
            const auto* def = definition_for(slot.unit_index);
            return def && def->builder && def->bm_code == 0;
        });
        return;
    case SelectionCommand::on_screen: {
        // The local units of the on-screen list: those whose box overlaps
        // the view.
        auto& world = match_->state();
        world.game.local_player_index = match_local_player_;
        oa::sim::selection::select_visible_units(world, on_screen_lists(), selection_hooks());
        adopt_selected_units();
        return;
    }
    case SelectionCommand::same_type: {
        // Adds to the selection every selectable local unit, anywhere on the
        // map, whose type is the type of any selected unit, as 3.1c does.
        // The type set holds the profile's type ids, or every type id with
        // ui.selection-shortcuts' same-type-bitset-fix.
        auto& world = match_->state();
        world.game.local_player_index = match_local_player_;
        const bool every_type =
            selection_shortcuts_on() && ui_rules().selection_shortcuts.same_type_bitset_fix;
        oa::sim::selection::select_matching_types(
            world,
            selection_hooks(),
            every_type ? oa::data::limits::highest_type_bits : limits_.unit_types.bitset_bits
        );
        adopt_selected_units();
        int count = 0;
        for (const auto& slot : match_->world().slots)
            count += slot.unit != nullptr && slot.owner_index == match_local_player_ &&
                     (slot.unit->flags & OA_UNIT_FLAG_SELECTED) != 0;
        status_ = count == 0 ? "No units" : std::to_string(count) + " selected";
        return;
    }
    }
}

bool Runtime::selection_bar_shows() const {
    if (!engine_settings_ || !engine_settings_->current.selection_bar || !match_ ||
        screen_ != Screen::match || match_finished_ || match_paused_ ||
        director_ != nullptr || local_player_watches() || touch_controls_active() ||
        oa::ui::display_layout::placed_mode(match_layout_))
        return false;
    const auto* state = touch_state_if_made();
    return state == nullptr || !state->hud.pad.hud;
}

bool Runtime::selection_bar_covers(float x, float y) const {
    const auto& rect = selection_bar_rect_;
    return rect.width > 0 && rect.height > 0 && selection_bar_shows() &&
           x >= static_cast<float>(rect.x) && y >= static_cast<float>(rect.y) &&
           x < static_cast<float>(rect.x + rect.width) &&
           y < static_cast<float>(rect.y + rect.height);
}

void Runtime::draw_selection_bar() {
    selection_bar_rect_ = {};
    selection_bar_buttons_.clear();
    const kit::Fonts* fonts = engine_settings_fonts();
    if (!selection_bar_shows() || fonts == nullptr)
        return;
    bar::Facts facts;
    // The groups, and the one that is the whole selection.
    std::array<uint16_t, bar::group_count> aircraft{};
    std::array<uint16_t, bar::group_count> selected_in_group{};
    uint32_t selected = 0;
    for (const auto& slot : match_->world().slots) {
        if (slot.unit_index == 0 || slot.unit == nullptr || slot.owner_index != match_local_player_)
            continue;
        const bool chosen = (slot.unit->flags & OA_UNIT_FLAG_SELECTED) != 0;
        selected += chosen ? 1U : 0U;
        const auto squad = static_cast<std::size_t>(slot.record.squad);
        if (squad < 1 || squad > bar::group_count)
            continue;
        ++facts.groups[squad - 1].units;
        const auto* def = definition_for(slot.unit_index);
        aircraft[squad - 1] += def != nullptr && def->can_fly ? 1U : 0U;
        selected_in_group[squad - 1] += chosen ? 1U : 0U;
    }
    for (std::size_t group = 0; group < bar::group_count; ++group) {
        auto& shown = facts.groups[group];
        shown.aircraft = aircraft[group] * 2U > shown.units;
        if (selected != 0 && shown.units == selected && selected_in_group[group] == selected)
            facts.lit_group = static_cast<int32_t>(group);
    }
    facts.digit_selects =
        (match_->state().game.graphics_flags & oa::ui::console::graphics_flag::switch_alt) != 0;
    name_keys(facts, selection_shortcuts_on());
    // The largest whole scale up to the HUD text's at which the bar fits.
    const auto area = overlay_area();
    const kit::Canvas measure = measuring_canvas(fonts);
    const int32_t least = bar::least_width(measure, facts);
    int scale = std::max(hud_text_scale(), 1);
    while (scale > 1 && least * scale > area.width)
        --scale;
    if (least * scale > area.width || bar::bar_height * scale > area.height / 2)
        return;
    const bar::Layout layout = bar::lay_out(measure, facts, area.width / scale);
    selection_bar_rect_ = {
        area.x + (area.width - layout.width * scale) / 2,
        area.y + area.height - bar::bar_height * scale,
        layout.width * scale,
        bar::bar_height * scale
    };
    for (const auto& button : layout.buttons)
        selection_bar_buttons_.push_back({
            selection_bar_rect_.x + button.x * scale,
            selection_bar_rect_.y + button.y * scale,
            button.width * scale,
            button.height * scale
        });
    paint_on(PaintLayer::battlefield);
    const auto corner = canvas_paint(selection_bar_rect_.x, selection_bar_rect_.y);
    kit::Canvas canvas;
    canvas.surface = &paint_target();
    canvas.placement.x = corner.x;
    canvas.placement.y = corner.y;
    canvas.placement.scale = scale;
    canvas.placement.clip = {0, 0, layout.width, bar::bar_height};
    canvas.fonts = fonts;
    bar::Pointer pointer;
    pointer.hovered = selection_bar_hovered_;
    pointer.held = selection_bar_held_;
    bar::draw(canvas, layout, facts, pointer);
}

bool Runtime::selection_bar_pointer(const SDL_Event& event, float x, float y) {
    if (!selection_bar_shows() || selection_bar_rect_.width <= 0) {
        selection_bar_hovered_ = bar::no_button;
        selection_bar_held_ = bar::no_button;
        selection_bar_pressed_ = false;
        return false;
    }
    // The button under the pointer, among the buttons as the bar was drawn
    // last.
    const auto button_under = [&]() -> int32_t {
        if (!selection_bar_covers(x, y))
            return bar::no_button;
        for (std::size_t button = 0; button < selection_bar_buttons_.size(); ++button) {
            const auto& rect = selection_bar_buttons_[button];
            if (x >= static_cast<float>(rect.x) && y >= static_cast<float>(rect.y) &&
                x < static_cast<float>(rect.x + rect.width) &&
                y < static_cast<float>(rect.y + rect.height))
                return static_cast<int32_t>(button);
        }
        return bar::no_button;
    };
    if (event.type == SDL_EVENT_MOUSE_MOTION) {
        selection_bar_hovered_ = button_under();
        return false;
    }
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        if (!selection_bar_covers(x, y))
            return false;
        selection_bar_pressed_ = true;
        selection_bar_hovered_ = button_under();
        selection_bar_held_ =
            event.button.button == SDL_BUTTON_LEFT ? selection_bar_hovered_ : bar::no_button;
        return true;
    }
    if (event.type != SDL_EVENT_MOUSE_BUTTON_UP || !selection_bar_pressed_)
        return false;
    selection_bar_pressed_ = false;
    const int32_t held = std::exchange(selection_bar_held_, bar::no_button);
    if (event.button.button != SDL_BUTTON_LEFT || held == bar::no_button)
        return true;
    selection_bar_hovered_ = button_under();
    if (selection_bar_hovered_ != held)
        return true;
    const bool shift = (input_modifiers(ModifierUse::selection) & SDL_KMOD_SHIFT) != 0;
    const bool ctrl = (input_modifiers(ModifierUse::keyboard) & SDL_KMOD_CTRL) != 0;
    if (held < static_cast<int32_t>(bar::group_count)) {
        if (ctrl)
            assign_squad(held + 1);
        else
            select_squad(held + 1, shift);
        return true;
    }
    play_match_interface_sound("Button");
    run_selection_command(
        bar_commands[static_cast<std::size_t>(held) - bar::group_count], shift
    );
    return true;
}

} // namespace oa::app
