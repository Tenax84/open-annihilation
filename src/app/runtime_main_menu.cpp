// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Open Annihilation's main menu on the OA layer: one column menu
// (column_menu.hpp), main-menu, kept at the bottom of the layer while the
// main menu shows (Runtime::keep_main_menu_screen, once a frame from
// OaLayer::tick). Under the OPEN ANNIHILATION title it has one column of
// seven buttons, Single Player, Multiplayer, Library, Settings, Intro,
// Credits and Exit, and under them a plate with the main menu's version
// label. The game's buttons are pressed as a click on MAINMENU.GUI's own
// presses them (Runtime::press_menu_gadget), so their sounds, their checks
// and the screens they lead to are the game's; Library and Settings open as
// the game's OA buttons open them. A button MAINMENU.GUI grays out or hides,
// as Intro and Credits are without movies, shows faded and takes no press.
// While a message box of the game's shows, the menu steps aside, and
// MAINMENU.GUI shows under the box.

#include "campaign_menu.hpp"
#include "column_menu.hpp"
#include "skirmish_menu.hpp"
#include "oa_layer.hpp"

#include "oa/app/runtime.hpp"
#include "oa/app/view_rules.hpp"
#include "oa/ui/frontend/main_menu.hpp"
#include "oa/ui/frontend_dialogs.hpp"
#include "oa/ui/frontend_state/main_menu.hpp"
#include "oa/ui/gui_layout.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <variant>

namespace oa::app {

namespace {

namespace menu = oa::ui::frontend_state::main_menu;

/// The menu's buttons, top to bottom.
enum class MenuEntry : uint8_t {
    single_player,
    multiplayer,
    library,
    settings,
    intro,
    credits,
    exit,
};

/// How many buttons the menu has.
constexpr std::size_t entry_count = 7;

/// Each button, in MenuEntry's order.
constexpr std::array<ColumnMenuEntry, entry_count> entries{{
    {"single-player", "SINGLE PLAYER", {}, false},
    {"multiplayer", "MULTIPLAYER", {}, false},
    {"library", "LIBRARY", {}, false},
    {"settings", "SETTINGS", {}, false},
    {"intro", "INTRO", {}, false},
    {"credits", "CREDITS", {}, false},
    {"exit", "EXIT", {}, false},
}};

/// Returns what the menu holds and where its parts stand, in rows of a
/// window 1080 rows high.
///
/// @return the look
ColumnMenuLook main_menu_look() {
    ColumnMenuLook look;
    look.screen_name = main_menu_screen_name;
    look.entries = entries;
    look.title_top = 70.0;
    look.title_columns = 960.0;
    look.first_button_top = 400.0;
    return look;
}

} // namespace

bool Runtime::oa_main_menu_shown() {
    return oa_layer().find(main_menu_screen_name) != nullptr;
}

bool Runtime::oa_menu_shown() {
    if (screen_ == Screen::main_menu)
        return oa_main_menu_shown();
    if (screen_ == Screen::single_player)
        return oa_single_player_menu_shown();
    if (screen_ == Screen::new_campaign)
        return oa_new_campaign_menu_shown();
    if (screen_ == Screen::skirmish)
        return oa_skirmish_menu_shown();
    return false;
}

bool Runtime::main_menu_covers_front_end() {
    if ((screen_ != Screen::main_menu && screen_ != Screen::single_player &&
         screen_ != Screen::new_campaign && screen_ != Screen::skirmish) ||
        sdl_.renderer == nullptr || frame_without_cursor_ || !oa_menu_may_show())
        return false;
    OaLayer& layer = oa_layer();
    const LayerScreen* menu = layer.find(
        screen_ == Screen::main_menu       ? main_menu_screen_name
        : screen_ == Screen::single_player ? single_player_menu_screen_name
        : screen_ == Screen::new_campaign  ? new_campaign_menu_screen_name
                                           : skirmish_menu_screen_name
    );
    if (menu == nullptr || !menu->opaque())
        return false;
    const LayerView seen = layer.view();
    const auto& shown = menu->placement(seen).shown;
    const auto& picture = seen.picture;
    return shown.width > 0 && shown.height > 0 && shown.x <= picture.x && shown.y <= picture.y &&
           shown.x + shown.width >= picture.x + picture.width &&
           shown.y + shown.height >= picture.y + picture.height;
}

bool Runtime::oa_menu_may_show() {
    // The game's message boxes are drawn in the front end's picture, which
    // the menus cover: while one shows, the menu steps aside.
    return !options_.classic_main_menu && !frame_owned_by_package() &&
           oa::ui::frontend_dialogs::dialog_count() == 0 && oa_layer().screen_fonts() != nullptr;
}

bool Runtime::press_menu_gadget(Screen shown, std::string_view name, int32_t mouse_button) {
    if (screen_ != shown)
        return false;
    const auto found = gui_record_named(resources_.layout.gadgets, name);
    if (!found || resources_.layout.gadgets[*found].common.active == 0 ||
        !frontend_gadget_pressable(*found))
        return false;
    // As a click released on the record: the record is the selection while
    // the screen's handler reads it. NEWGAME.GUI's handler reads the record
    // under the pointer, which the record is then as well.
    const auto pointed = hovered_;
    selected_ = static_cast<int32_t>(*found);
    if (shown == Screen::new_campaign)
        hovered_ = *found;
    event_button_ = mouse_button;
    activate();
    event_button_ = 1;
    selected_ = -1;
    if (shown == Screen::new_campaign)
        hovered_ = screen_ == shown ? pointed : std::nullopt;
    return true;
}

void Runtime::keep_main_menu_screen() {
    const auto shows = [this] {
        return screen_ == Screen::main_menu && !main_menu_overlay_ && oa_menu_may_show();
    };
    if (!shows())
        return;
    OaLayer& layer = oa_layer();
    if (layer.find(main_menu_screen_name) != nullptr)
        return;
    // The record each button of the game's presses, in MenuEntry's order;
    // Library and Settings press none.
    const auto record_of = [this](MenuEntry entry) -> std::string_view {
        switch (entry) {
        case MenuEntry::single_player:
            return menu::resource_name(menu::Button::single_player);
        case MenuEntry::multiplayer:
            return menu::resource_name(menu::Button::multiplayer);
        case MenuEntry::intro:
            return menu::resource_name(menu::Button::intro);
        case MenuEntry::credits:
            return view_rules::credits_gadget(mod_profile());
        case MenuEntry::exit:
            return menu::resource_name(menu::Button::exit);
        case MenuEntry::library:
        case MenuEntry::settings:
            break;
        }
        return {};
    };
    const auto facts = [this, record_of] {
        ColumnMenuFacts now = column_menu_facts(entry_count);
        // Every button shows; one MAINMENU.GUI hides shows faded.
        now.shown.assign(entry_count, true);
        const auto& gadgets = resources_.layout.gadgets;
        for (std::size_t index = 0; index < entry_count; ++index) {
            const auto entry = static_cast<MenuEntry>(index);
            if (entry == MenuEntry::library || entry == MenuEntry::settings) {
                now.enabled[index] = true;
                continue;
            }
            const auto found = gui_record_named(gadgets, record_of(entry));
            if (!found || gadgets[*found].common.active == 0)
                continue;
            now.enabled[index] = frontend_gadget_pressable(*found);
            if (const auto* button =
                    std::get_if<oa::ui::gui_layout::ButtonFields>(&gadgets[*found].fields))
                now.quick_keys[index] = static_cast<char>(button->quick_key);
        }
        const auto label = gui_record_named(gadgets, oa::ui::frontend::kMainMenuVersionControl);
        if (label && gadgets[*label].common.active != 0)
            if (const auto* text =
                    std::get_if<oa::ui::gui_layout::LabelFields>(&gadgets[*label].fields))
                now.version = text->text;
        // The Single Player menu shows the same label (menu_version_).
        menu_version_ = now.version;
        return now;
    };
    const auto press = [this, record_of](std::size_t index) {
        const auto entry = static_cast<MenuEntry>(index);
        if (entry == MenuEntry::library)
            open_library(std::nullopt);
        else if (entry == MenuEntry::settings)
            open_engine_settings_from_menu();
        else
            press_menu_gadget(Screen::main_menu, record_of(entry));
    };
    layer.push_under(make_column_menu(main_menu_look(), shows, facts, press));
}

} // namespace oa::app
