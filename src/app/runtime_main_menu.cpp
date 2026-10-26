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
//
// The game's own main menu is given LIBRARY and SETTINGS buttons of its own
// look (Runtime::add_main_menu_entries): copies of SINGLE and INTRO one
// button's step above them, each on a copy of its button's plate.

#include "campaign_menu.hpp"
#include "column_menu.hpp"
#include "engine_settings_state.hpp"
#include "briefing_menu.hpp"
#include "load_game_menu.hpp"
#include "multiplayer_menu.hpp"
#include "skirmish_menu.hpp"
#include "oa_layer.hpp"

#include "oa/app/runtime.hpp"
#include "oa/app/view_rules.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/ui/engine_settings.hpp"
#include "oa/ui/frontend/main_menu.hpp"
#include "oa/ui/frontend_dialogs.hpp"
#include "oa/ui/frontend_multiplayer/screens.hpp"
#include "oa/ui/frontend_state/main_menu.hpp"
#include "oa/ui/gui_layout.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace oa::app {

namespace {

namespace menu = oa::ui::frontend_state::main_menu;
namespace mp = oa::ui::frontend_multiplayer;

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

/// One button MAINMENU.GUI is given: the record it copies and stands one
/// button's step above, its record name, its caption in English and its
/// quick key.
struct AddedEntry {
    std::string_view copied{};
    std::string_view name{};
    std::string_view caption{};
    char quick_key{};
};

/// The buttons MAINMENU.GUI is given: LIBRARY above SINGLE, SETTINGS above
/// INTRO.
constexpr std::array<AddedEntry, 2> added_entries{{
    {menu::resource_name(menu::Button::single_player), Runtime::main_menu_library_record, "LIBRARY", 'L'},
    {menu::resource_name(menu::Button::intro), Runtime::main_menu_settings_record, "SETTINGS", 'T'},
}};

/// How far a button's plate on the menu's picture reaches past the button,
/// in the picture's pixels: FrontendX's plate around SINGLE stands from 10
/// columns left of it to 9 right of it, and from 7 rows above it to 6 below.
constexpr int32_t kPlateLeft = 10;
constexpr int32_t kPlateRight = 9;
constexpr int32_t kPlateTop = 7;
constexpr int32_t kPlateBottom = 6;
/// The rows from one main-menu button to the next when MAINMENU.GUI has no
/// MULTI under SINGLE to measure them by: SINGLE's 393 to MULTI's 430.
constexpr int32_t kEntryStep = 37;

/// Copies a rectangle of a picture onto another place of it, the colours and,
/// on a paletted picture, the indices.
///
/// @param picture the picture
/// @param x the rectangle's first column
/// @param y the rectangle's first row
/// @param width its columns
/// @param height its rows
/// @param to_y the first row it is copied to, in the same columns
void copy_rows(
    oa::Image& picture, int32_t x, int32_t y, int32_t width, int32_t height,
    int32_t to_y
) {
    const auto columns = static_cast<std::size_t>(picture.width);
    const bool indexed = picture.indices.size() == columns * picture.height;
    for (int32_t row = 0; row < height; ++row) {
        const std::size_t from = static_cast<std::size_t>(y + row) * columns + static_cast<std::size_t>(x);
        const std::size_t to = static_cast<std::size_t>(to_y + row) * columns + static_cast<std::size_t>(x);
        std::copy_n(picture.rgb.begin() + static_cast<std::ptrdiff_t>(from * 3U), static_cast<std::size_t>(width) * 3U,
                    picture.rgb.begin() + static_cast<std::ptrdiff_t>(to * 3U));
        if (indexed)
            std::copy_n(picture.indices.begin() + static_cast<std::ptrdiff_t>(from), width,
                        picture.indices.begin() + static_cast<std::ptrdiff_t>(to));
    }
}

} // namespace

void Runtime::add_main_menu_entries() {
    main_menu_entries_ = false;
    if (options_.classic_main_menu || main_menu_overlay_)
        return;
    auto& gadgets = resources_.layout.gadgets;
    auto& picture = resources_.background;
    if (gadgets.size() + added_entries.size() > oa::ui::gui_layout::limit::gadgets ||
        picture.rgb.size() != static_cast<std::size_t>(picture.width) * picture.height * 3U)
        return;
    // The step between SINGLE and MULTI, as MAINMENU.GUI places them.
    int32_t step = kEntryStep;
    const auto single = gui_record_named(gadgets, menu::resource_name(menu::Button::single_player));
    const auto multi = gui_record_named(gadgets, menu::resource_name(menu::Button::multiplayer));
    if (single && multi && gadgets[*multi].common.y > gadgets[*single].common.y)
        step = gadgets[*multi].common.y - gadgets[*single].common.y;
    // Every button and its plate must find its place before any is added.
    std::array<std::size_t, added_entries.size()> copied{};
    for (std::size_t at = 0; at < added_entries.size(); ++at) {
        const auto found = gui_record_named(gadgets, added_entries[at].copied);
        if (!found || gui_record_named(gadgets, added_entries[at].name) ||
            !std::holds_alternative<oa::ui::gui_layout::ButtonFields>(gadgets[*found].fields))
            return;
        const auto& common = gadgets[*found].common;
        const int32_t left = common.x - kPlateLeft;
        const int32_t top = common.y - kPlateTop;
        if (left < 0 || top - step < 0 ||
            common.x + common.width + kPlateRight > static_cast<int32_t>(picture.width) ||
            common.y + common.height + kPlateBottom > static_cast<int32_t>(picture.height))
            return;
        copied[at] = *found;
    }
    for (std::size_t at = 0; at < added_entries.size(); ++at) {
        const AddedEntry& entry = added_entries[at];
        oa::ui::gui_layout::Gadget gadget = gadgets[copied[at]];
        const auto& common = gadget.common;
        // The plate goes up with its button; the pipes beside it stay.
        copy_rows(
            picture,
            common.x - kPlateLeft,
            common.y - kPlateTop,
            common.width + kPlateLeft + kPlateRight,
            common.height + kPlateTop + kPlateBottom,
            common.y - kPlateTop - step
        );
        gadget.common.name = std::string(entry.name);
        gadget.common.y = static_cast<int16_t>(common.y - step);
        gadget.common.active = 1;
        auto& button = std::get<oa::ui::gui_layout::ButtonFields>(gadget.fields);
        button.source_text = std::string(entry.caption);
        button.text = std::string(oa::data::languages::interface_text(entry.caption));
        button.quick_key = static_cast<int8_t>(entry.quick_key);
        button.status = 0;
        button.grayed_out = false;
        gadgets.push_back(std::move(gadget));
    }
    main_menu_entries_ = true;
}

bool Runtime::activate_main_menu_entry() {
    if (screen_ != Screen::main_menu || !main_menu_entries_ || selected_ < 0 ||
        static_cast<std::size_t>(selected_) >= resources_.layout.gadgets.size())
        return false;
    const std::string_view name = resources_.layout.gadgets[static_cast<std::size_t>(selected_)].common.name;
    const bool library = name == added_entries[0].name;
    if (!library && name != added_entries[1].name)
        return false;
    selected_ = -1;
    if (library)
        open_library(std::nullopt);
    else
        open_engine_settings_from_menu();
    return true;
}

bool Runtime::main_menu_opens_oa_screens() {
    return oa_main_menu_shown() ||
           (screen_ == Screen::main_menu && !options_.classic_main_menu &&
            !frame_owned_by_package() && oa_layer().screen_fonts() != nullptr);
}

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
    if (screen_ == Screen::load_game)
        return oa_load_game_menu_shown();
    if (screen_ == Screen::briefing)
        return oa_briefing_menu_shown();
    return false;
}

bool Runtime::main_menu_covers_front_end() {
    // The Multiplayer screen covers SELPROV.GUI, SELGAME.GUI and TCP.GUI
    // while it hands TCP.GUI the address.
    const auto id = screen_id(screen_);
    const bool multiplayer =
        id == mp::kScreenProviders || id == mp::kScreenTcp || id == mp::kScreenGameList;
    // The Load Game screen covers the load dialog over Single Player and
    // the end of a mission.
    const bool load_game = load_game_menu_covers();
    // The briefing screen covers the mission briefing.
    const bool briefing = briefing_menu_covers();
    if ((screen_ != Screen::main_menu && screen_ != Screen::single_player &&
         screen_ != Screen::new_campaign && screen_ != Screen::skirmish && !multiplayer &&
         !load_game && !briefing) ||
        sdl_.renderer == nullptr || frame_without_cursor_ || !oa_menu_may_show())
        return false;
    OaLayer& layer = oa_layer();
    const LayerScreen* menu = layer.find(
        multiplayer                        ? multiplayer_menu_screen_name
        : load_game                        ? load_game_menu_screen_name
        : briefing                         ? briefing_menu_screen_name
        : screen_ == Screen::main_menu     ? main_menu_screen_name
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
    return oa_menus_chosen_ && !options_.classic_main_menu && !frame_owned_by_package() &&
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
    // Menu style takes effect on the main menu once the settings dialog,
    // which puts each setting in effect as it changes, has closed.
    if (screen_ == Screen::main_menu && engine_settings_dialog() == nullptr)
        oa_menus_chosen_ = engine_settings_ && engine_settings_->current.menu_style ==
                                                   oa::ui::engine_settings::MenuStyle::open_annihilation;
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
