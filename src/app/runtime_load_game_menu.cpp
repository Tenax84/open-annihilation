// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Open Annihilation's Load Game screen on the OA layer (load_game_menu.hpp),
// load-game-menu, kept at the bottom of the layer while the load dialog,
// LOADGAME.GUI in its load role, shows over Single Player or the end of a
// mission (Runtime::keep_load_game_menu_screen, once a frame from
// OaLayer::tick). It lists the saves the dialog lists, and shows the chosen
// one's radar picture and what the dialog's preview tells of it: its kind,
// its campaign, its mission or map, how long it was played, the side and the
// difficulty. Choosing a save chooses the dialog's row, and LOAD and PREVIOUS
// MENU press the dialog's own LOAD and CANCEL (Runtime::press_load_dialog),
// so that its checks, its sounds and the screens it leads to stay the game's.
// Without saves the dialog opens without the message box that says so: the
// screen says it in its list. While a message box of the game's shows, the
// screen steps aside, and the dialog shows under the box. Over a match the
// game's own dialog shows.

#include "column_menu.hpp"
#include "load_game_menu.hpp"
#include "menu_paint.hpp"
#include "oa_layer.hpp"

#include "oa/app/runtime.hpp"
#include "oa/ui/gui_layout.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace oa::app {

namespace {

/// Returns a button's quick key on the GUI layout shown.
///
/// @param gadgets the layout's records
/// @param name the button's name
/// @return the key; 0 for none
char quick_key_of(std::span<const oa::ui::gui_layout::Gadget> gadgets, std::string_view name) {
    const auto found = gui_record_named(gadgets, name);
    if (!found)
        return 0;
    if (const auto* button = std::get_if<oa::ui::gui_layout::ButtonFields>(&gadgets[*found].fields))
        return static_cast<char>(button->quick_key);
    return 0;
}

} // namespace

bool Runtime::load_game_menu_covers() {
    return screen_ == Screen::load_game && !save_dialog_open() &&
           options_parent_ != Screen::match && oa_menu_may_show();
}

bool Runtime::oa_load_game_menu_shown() {
    return oa_layer().find(load_game_menu_screen_name) != nullptr;
}

void Runtime::keep_load_game_menu_screen() {
    const auto shows = [this] { return load_game_menu_covers(); };
    if (!shows())
        return;
    OaLayer& layer = oa_layer();
    if (layer.find(load_game_menu_screen_name) != nullptr)
        return;
    const auto facts = [this] {
        LoadGameMenuFacts now;
        if (auto shown = load_dialog_shown()) {
            now.saves = std::move(shown->saves);
            now.chosen = shown->chosen;
            const auto add = [&now](std::string_view label, std::string value) {
                if (!value.empty())
                    now.facts.push_back({menu_paint::worded(label), std::move(value)});
            };
            if (now.chosen) {
                // A campaign's save names its mission there, a skirmish's
                // its map.
                const bool campaign = !shown->campaign.empty();
                add("Type", menu_paint::worded(shown->type));
                add("Campaign", std::move(shown->campaign));
                add(campaign ? "Mission" : "Map", std::move(shown->place));
                add("Game time", std::move(shown->time));
                add("Side", std::move(shown->side));
                add("Difficulty", menu_paint::worded(shown->difficulty));
            }
        }
        now.can_load = now.chosen.has_value();
        const auto& gadgets = resources_.layout.gadgets;
        now.load_key = quick_key_of(gadgets, "LOAD");
        now.back_key = quick_key_of(gadgets, "CANCEL");
        now.version = menu_version_;
        return now;
    };
    LoadGameMenuActions actions;
    actions.choose = [this](std::size_t row) { choose_saved_game(row); };
    actions.load = [this] { press_load_dialog(true); };
    actions.back = [this] { press_load_dialog(false); };
    actions.picture = [this] { return saved_game_picture(); };
    layer.push_under(make_load_game_menu(shows, facts, std::move(actions)));
}

} // namespace oa::app
