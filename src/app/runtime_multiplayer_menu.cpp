// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Open Annihilation's Multiplayer screen on the OA layer
// (multiplayer_menu.hpp), multiplayer-menu, kept at the bottom of the layer
// while SELPROV.GUI or SELGAME.GUI shows (Runtime::keep_multiplayer_menu_screen,
// once a frame from OaLayer::tick).
//
// Over SELPROV.GUI it lists the connections the game offers and the address
// typed, which starts as the stored one. SELECT presses SELPROV.GUI's own
// Select; when the connection takes an address, TCP.GUI then opens under the
// screen, which hands it the address typed and presses its OK, so that the
// address is checked, stored and used as the game's own dialog does it.
// OPTIONS opens Settings, which hold what the game's Options set.
//
// Over SELGAME.GUI it shows the games SELGAME.GUI lists, each one's status
// and rules read from what its host tells, and the name and password fields
// of SELGAME.GUI, which it types into. Choosing a game chooses SELGAME.GUI's
// row, and NEW GAME, UPDATE, WATCH, JOIN and PREVIOUS MENU press SELGAME.GUI's
// own buttons (multiplayer_click), so that their checks, their sounds and
// the screens they lead to stay the game's. A game's map's picture is the
// minimap of the map of that name in the game data, when it has one.
//
// While a message or a dialog of the multiplayer screens shows, or a message
// box of the game's, the screen steps aside, and the game's screen shows
// under it.

#include "menu_paint.hpp"
#include "multiplayer_menu.hpp"
#include "oa_layer.hpp"

#include "oa/app/runtime.hpp"
#include "oa/formats/tnt.hpp"
#include "oa/ui/frontend_multiplayer/connect.hpp"
#include "oa/ui/frontend_multiplayer/lobby.hpp"
#include "oa/ui/frontend_multiplayer/panel.hpp"
#include "oa/ui/frontend_multiplayer/screens.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {

namespace {

namespace mp = oa::ui::frontend_multiplayer;

/// SELGAME.GUI's lists, one a column, which show the same game in a row.
constexpr std::array<std::string_view, 10> game_lists{
    "GAMENAME", "PLAYERS", "MAPNAME", "STATUS", "METAL",
    "ENERGY",   "PING",    "COMMANDER", "FULLMAP", "LOS"
};

/// The bytes of a game's entry its name takes, and then its map's name.
constexpr std::size_t game_name_bytes = 0x10;
/// The bytes of a game's entry its map's name takes.
constexpr std::size_t map_name_bytes = 0xf;

/// The columns of the right edge, and the rows of the bottom edge, of a
/// map's terrain that its minimap shows but the map does not cover.
constexpr int32_t unshown_columns = 32;
constexpr int32_t unshown_rows = 128;

/// Returns a text held in a fixed field, up to its terminator, without the
/// spaces that pad it.
///
/// @param text the field
/// @param size its bytes
/// @return the text
std::string field_text(const char* text, std::size_t size) {
    std::string value(text, std::find(text, text + size, '\0'));
    while (!value.empty() && value.back() == ' ')
        value.pop_back();
    return value;
}

/// Returns a button's quick key on a multiplayer panel.
///
/// @param panel the panel
/// @param name the button's name
/// @return the key; 0 for none
char quick_key_of(const mp::Panel& panel, std::string_view name) {
    const mp::Control* control = mp::panel_control(panel, name);
    return control != nullptr ? static_cast<char>(control->quick_key) : '\0';
}

/// Tells whether a button of a multiplayer panel takes a press.
///
/// @param panel the panel
/// @param name the button's name
/// @return true when it shows and is not grayed
bool pressable(const mp::Panel& panel, std::string_view name) {
    const mp::Control* control = mp::panel_control(panel, name);
    return control != nullptr && control->active != 0 && !control->grayed;
}

/// Returns what a connection does, by its kind.
///
/// @param kind the connection's kind
/// @return the words, in the language shown
std::string connection_about(mp::ProviderKind kind) {
    switch (kind) {
    case mp::ProviderKind::tcpip:
        return menu_paint::worded(
            "Join a game at the address you type, or leave the address blank to find the games "
            "on your network. To host a game, choose New Game on the next screen."
        );
    case mp::ProviderKind::ipx:
        return menu_paint::worded(
            "Find the games on the computers of your local network. To host a game, choose New "
            "Game on the next screen."
        );
    case mp::ProviderKind::modem:
    case mp::ProviderKind::serial:
        return menu_paint::worded("Modem and serial connections are not available in this build.");
    case mp::ProviderKind::other:
        break;
    }
    return menu_paint::worded(
        "Find the games this connection reaches. To host a game, choose New Game on the next "
        "screen."
    );
}

/// Returns the multiplayer field a Multiplayer screen's field types into.
///
/// @param field the screen's field
/// @return its control's name on SELGAME.GUI; empty for the address and none
std::string_view game_list_field(MultiplayerMenuField field) {
    switch (field) {
    case MultiplayerMenuField::nickname:
        return "NICKNAME";
    case MultiplayerMenuField::password:
        return "PASSWORD";
    default:
        return {};
    }
}

} // namespace

bool Runtime::oa_multiplayer_menu_shown() {
    return oa_layer().find(multiplayer_menu_screen_name) != nullptr;
}

void Runtime::keep_multiplayer_menu_screen() {
    MultiplayerMenuMemory& memory = multiplayer_menu_memory_;
    const auto shown = screen_id(screen_);
    // TCP.GUI takes the address typed as it opens, and its OK checks it.
    if (shown == mp::kScreenTcp) {
        if (memory.address_waits && mp::multiplayer_modal_kind() == mp::ModalKind::tcp &&
            mp::multiplayer_message().empty()) {
            memory.address_waits = false;
            memory.address_sent = true;
            if (mp::Panel* modal = mp::multiplayer_modal()) {
                mp::panel_set_text(*modal, "ADDRESS", memory.address);
                auto context = screen_context();
                mp::multiplayer_click(&context, "OK");
            }
        }
    } else if (shown != mp::kScreenProviders) {
        memory.address_waits = false;
        memory.address_sent = false;
    } else {
        memory.address_sent = false;
    }
    const auto shows = [this] {
        if (!oa_menu_may_show() || !mp::multiplayer_showing() ||
            !mp::multiplayer_message().empty())
            return false;
        const auto now = screen_id(screen_);
        if (now == mp::kScreenProviders || now == mp::kScreenGameList)
            return mp::multiplayer_modal() == nullptr;
        if (now == mp::kScreenTcp)
            return multiplayer_menu_memory_.address_waits || multiplayer_menu_memory_.address_sent;
        return false;
    };
    if (!shows())
        return;
    OaLayer& layer = oa_layer();
    if (layer.find(multiplayer_menu_screen_name) != nullptr)
        return;
    if (!memory.address_read) {
        memory.address_read = true;
        const mp::ConnectState& connect = mp::multiplayer_connect();
        char stored[128] = "";
        if (connect.settings.read_address != nullptr &&
            connect.settings.read_address(connect.settings.context, stored, sizeof(stored)))
            memory.address = field_text(stored, sizeof(stored));
    }

    const auto facts = [this] {
        MultiplayerMenuFacts now;
        now.version = menu_version_;
        const mp::ConnectState& connect = mp::multiplayer_connect();
        const mp::Panel& panel = mp::multiplayer_panel();
        const auto shown_now = screen_id(screen_);
        for (int32_t at = 0; at < connect.provider_count; ++at) {
            const mp::Provider& provider = connect.providers[at];
            const mp::ProviderKind kind = mp::provider_kind(provider.guid);
            now.connections.push_back(
                {field_text(provider.name, sizeof(provider.name)), connection_about(kind),
                 kind == mp::ProviderKind::tcpip}
            );
        }
        if (shown_now != mp::kScreenGameList) {
            now.step = MultiplayerMenuStep::connection;
            int32_t chosen = connect.provider;
            if (shown_now == mp::kScreenProviders)
                if (const mp::Control* list = mp::panel_control(panel, "DPLAY"))
                    chosen = list->list_selection;
            if (chosen >= 0 && chosen < connect.provider_count)
                now.connection = static_cast<std::size_t>(chosen);
            now.address = multiplayer_menu_memory_.address;
            if (shown_now == mp::kScreenProviders) {
                now.select_key = quick_key_of(panel, "SELECT");
                now.options_key = quick_key_of(panel, "SETTINGS");
                now.back_key = quick_key_of(panel, "PREVMENU");
            }
            return now;
        }

        now.step = MultiplayerMenuStep::games;
        if (connect.provider >= 0 && connect.provider < connect.provider_count) {
            const mp::Provider& provider = connect.providers[connect.provider];
            now.connection_name = field_text(provider.name, sizeof(provider.name));
            const std::string address = field_text(connect.address, sizeof(connect.address));
            now.connection_line =
                mp::provider_kind(provider.guid) == mp::ProviderKind::tcpip && !address.empty()
                    ? menu_paint::filled(menu_paint::worded("Address: {address}"), "{address}", address)
                    : menu_paint::worded("Searching the local network");
        }
        if (const mp::Control* nickname = mp::panel_control(panel, "NICKNAME"))
            now.nickname = std::string(mp::control_text(*nickname));
        if (const mp::Control* password = mp::panel_control(panel, "PASSWORD")) {
            now.password = std::string(mp::control_text(*password));
            now.password_asked = password->active != 0;
        }
        const mp::Lobby& lobby = mp::multiplayer_lobby();
        const mp::Control* names = mp::panel_control(panel, "GAMENAME");
        const mp::Control* maps = mp::panel_control(panel, "MAPNAME");
        for (int32_t row = 0; row < connect.session_count; ++row) {
            const mp::SessionEntry& session = connect.sessions[row];
            mp::PlayerSetupInfo info{};
            std::memcpy(
                reinterpret_cast<uint8_t*>(&info) + mp::kSessionUserInfoOffset, session.user,
                sizeof(session.user)
            );
            const auto place = static_cast<std::size_t>(row);
            MultiplayerMenuGame game;
            game.name = names != nullptr && place < names->items.size()
                            ? names->items[place]
                            : field_text(session.name, game_name_bytes);
            game.map_file = field_text(session.name + game_name_bytes, map_name_bytes);
            game.map = maps != nullptr && place < maps->items.size() ? maps->items[place]
                                                                     : game.map_file;
            const int32_t seated = info.options & mp::option::player_count_mask;
            const auto most = static_cast<int32_t>(session.max_players);
            game.players = std::to_string(seated) + " / " + std::to_string(most);
            game.ping = menu_paint::filled(
                menu_paint::worded("{n} ms"), "{n}", std::to_string(info.lowest_latency)
            );
            const uint32_t options = info.options;
            if (!mp::info_version_compatible(info, lobby.local_version_major, lobby.wire_rules))
                game.status = menu_paint::worded("Other version");
            else if ((options & mp::option::game_closed) != 0)
                game.status = menu_paint::worded("Closed");
            else if ((options & mp::option::started) != 0)
                game.status = menu_paint::worded("In battle");
            else if ((info.status & mp::status::launch_only) != 0)
                game.status = menu_paint::worded("Boneyards only");
            else if (most > 0 && seated >= most)
                game.status = menu_paint::worded("Full");
            else {
                game.status = menu_paint::worded("Open");
                game.open = true;
            }
            const uint32_t commander = options & mp::option::commander_mask;
            const char* commander_text = commander == 0 ? "Game continues"
                                         : commander == mp::option::commander_step ? "Game ends"
                                                                                   : "Death match";
            const char* sight = (options & mp::option::los_limited) == 0 ? "Permanent"
                                : (options & mp::option::los_true) != 0  ? "True"
                                                                         : "Circular";
            game.facts = {
                {menu_paint::worded("Players"), game.players},
                {menu_paint::worded("Ping"), game.ping},
                {menu_paint::worded("Commander"), menu_paint::worded(commander_text)},
                {menu_paint::worded("Line of sight"), menu_paint::worded(sight)},
                {menu_paint::worded("Mapping"),
                 menu_paint::worded((options & mp::option::unmapped) != 0 ? "Unmapped" : "Mapped")},
                {menu_paint::worded("Metal"), std::to_string(info.metal_hundreds * 100)},
                {menu_paint::worded("Energy"), std::to_string(info.energy_hundreds * 100)},
                {menu_paint::worded("Watching"),
                 menu_paint::worded(
                     (options & mp::option::watching_allowed) != 0 ? "Allowed" : "Not allowed"
                 )},
                {menu_paint::worded("Password"),
                 menu_paint::worded((info.status & mp::status::password) != 0 ? "Asked" : "None")},
            };
            now.games.push_back(std::move(game));
        }
        if (names != nullptr && connect.session_count > 0)
            now.game = static_cast<std::size_t>(
                std::clamp<int32_t>(names->list_selection, 0, connect.session_count - 1)
            );
        now.can_join = pressable(panel, "JOINGAME");
        now.can_watch = pressable(panel, "WATCH");
        now.new_key = quick_key_of(panel, "STARTNEW");
        now.update_key = quick_key_of(panel, "UPDATE");
        now.watch_key = quick_key_of(panel, "WATCH");
        now.join_key = quick_key_of(panel, "JOINGAME");
        now.back_key = quick_key_of(panel, "PREVMENU");
        return now;
    };

    MultiplayerMenuActions actions;
    actions.choose_connection = [this](std::size_t connection) {
        if (screen_id(screen_) != mp::kScreenProviders)
            return;
        if (mp::Control* list = mp::panel_control(mp::multiplayer_panel(), "DPLAY"))
            if (connection < list->items.size())
                list->list_selection = static_cast<int16_t>(connection);
        mp::multiplayer_panel().dirty = true;
    };
    actions.choose_game = [this](std::size_t game) {
        if (screen_id(screen_) != mp::kScreenGameList)
            return;
        mp::Panel& panel = mp::multiplayer_panel();
        mp::ConnectState& connect = mp::multiplayer_connect();
        if (game >= static_cast<std::size_t>(std::max(connect.session_count, 0)))
            return;
        for (const std::string_view name : game_lists)
            if (mp::Control* list = mp::panel_control(panel, name))
                list->list_selection = static_cast<int16_t>(game);
        mp::game_list_refresh_join(mp::multiplayer_lobby(), connect, panel);
    };
    actions.press = [this](MultiplayerMenuButton button) {
        const char* name = nullptr;
        switch (button) {
        case MultiplayerMenuButton::select: {
            const mp::ConnectState& connect = mp::multiplayer_connect();
            const mp::Control* list = mp::panel_control(mp::multiplayer_panel(), "DPLAY");
            const int32_t chosen = list != nullptr ? list->list_selection : -1;
            multiplayer_menu_memory_.address_waits =
                chosen >= 0 && chosen < connect.provider_count &&
                mp::provider_kind(connect.providers[chosen].guid) == mp::ProviderKind::tcpip;
            name = "SELECT";
            break;
        }
        case MultiplayerMenuButton::back:
            name = "PREVMENU";
            break;
        case MultiplayerMenuButton::new_game:
            name = "STARTNEW";
            break;
        case MultiplayerMenuButton::update:
            name = "UPDATE";
            break;
        case MultiplayerMenuButton::watch:
            name = "WATCH";
            break;
        case MultiplayerMenuButton::join:
            name = "JOINGAME";
            break;
        }
        auto context = screen_context();
        mp::multiplayer_click(&context, name);
    };
    actions.options = [this] { open_engine_settings_from_menu(); };
    actions.type = [this](MultiplayerMenuField field, std::string_view typed) {
        if (field == MultiplayerMenuField::address) {
            std::string& address = multiplayer_menu_memory_.address;
            for (const char character : typed)
                if (character >= 0x20 && character < 0x7f &&
                    address.size() + 1 < mp::kControlTextBytes)
                    address += character;
            return;
        }
        const std::string_view name = game_list_field(field);
        mp::Panel& panel = mp::multiplayer_panel();
        const int32_t index = name.empty() ? mp::kNoControl : mp::panel_find(panel, name);
        if (index == mp::kNoControl)
            return;
        // SELGAME.GUI's own typing keeps its limits and characters; its
        // focus stays where it was.
        const int32_t focus = panel.focus;
        panel.focus = index;
        const std::string text(typed);
        auto context = screen_context();
        mp::multiplayer_type(&context, text.c_str());
        panel.focus = focus;
    };
    actions.erase = [this](MultiplayerMenuField field) {
        if (field == MultiplayerMenuField::address) {
            std::string& address = multiplayer_menu_memory_.address;
            if (!address.empty())
                address.pop_back();
            return;
        }
        const std::string_view name = game_list_field(field);
        mp::Panel& panel = mp::multiplayer_panel();
        mp::Control* control = name.empty() ? nullptr : mp::panel_control(panel, name);
        if (control == nullptr)
            return;
        std::string text(mp::control_text(*control));
        if (text.empty())
            return;
        text.pop_back();
        mp::set_control_text(*control, text);
        panel.dirty = true;
    };
    // The textures' palette colours the minimaps; it is read once, when first
    // asked for.
    auto palette = std::make_shared<std::optional<oa::PaletteBytes>>();
    actions.map_picture = [this, palette](std::string_view map) {
        WindowIcon picture;
        const auto bytes = read("maps/" + std::string(map) + ".tnt");
        if (!bytes)
            return picture;
        auto parsed = oa::formats::tnt::parse(*bytes);
        if (!parsed.ok() || !parsed.map->minimap)
            return picture;
        const auto& terrain = *parsed.map;
        const auto& minimap = *terrain.minimap;
        const int64_t wide = static_cast<int64_t>(terrain.attribute_width) * 16 - unshown_columns;
        const int64_t high = static_cast<int64_t>(terrain.attribute_height) * 16 - unshown_rows;
        if (wide <= 0 || high <= 0 || minimap.width == 0 || minimap.height == 0 ||
            minimap.palette_indices.size() < static_cast<std::size_t>(minimap.width) * minimap.height)
            return picture;
        if (!*palette) {
            oa::PaletteBytes colours = screen_palette();
            if (const auto read_bytes = read("palettes/palette.pal");
                read_bytes && read_bytes->size() == colours.size())
                std::copy(read_bytes->begin(), read_bytes->end(), colours.begin());
            *palette = colours;
        }
        // The map's longer side fills the minimap; the rest of the other
        // side is not the map's.
        uint32_t columns = minimap.width;
        uint32_t rows = minimap.height;
        if (wide < high)
            columns = static_cast<uint32_t>(minimap.width * wide / high);
        else
            rows = static_cast<uint32_t>(minimap.height * high / wide);
        columns = std::clamp<uint32_t>(columns, 1, minimap.width);
        rows = std::clamp<uint32_t>(rows, 1, minimap.height);
        const oa::PaletteBytes& colours = **palette;
        picture.width = columns;
        picture.height = rows;
        picture.pixels.resize(static_cast<std::size_t>(columns) * rows * 4U);
        for (uint32_t y = 0; y < rows; ++y)
            for (uint32_t x = 0; x < columns; ++x) {
                const std::size_t colour =
                    static_cast<std::size_t>(
                        minimap.palette_indices[static_cast<std::size_t>(y) * minimap.width + x]
                    ) *
                    4U;
                uint8_t* pixel =
                    picture.pixels.data() + (static_cast<std::size_t>(y) * columns + x) * 4U;
                pixel[0] = colours[colour];
                pixel[1] = colours[colour + 1U];
                pixel[2] = colours[colour + 2U];
                pixel[3] = 255;
            }
        return picture;
    };
    layer.push_under(make_multiplayer_menu(shows, facts, std::move(actions)));
}

} // namespace oa::app
