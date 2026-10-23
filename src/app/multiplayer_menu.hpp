// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Open Annihilation's Multiplayer screen on the OA layer
// (runtime_multiplayer_menu.cpp keeps it over the multiplayer screens). It
// covers the whole window with the main menu's art (menu_paint.hpp): under a
// smaller OPEN ANNIHILATION title and a MULTIPLAYER heading it shows one of
// two steps, and the version plate under them.
//
// - The connection step, over SELPROV.GUI: a CONNECTION panel with a plate
//   for each connection the game offers, the chosen one in amber, and under
//   them the address field when the chosen connection takes one; a DETAILS
//   panel telling what the chosen connection does; OPTIONS, SELECT and
//   PREVIOUS MENU.
// - The game step, over SELGAME.GUI: a PLAYER panel with the connection, the
//   player's name and the password; a GAMES panel with a row for every game
//   found, its name, map, players, ping and status, the chosen one in amber;
//   a DETAILS panel with the chosen game's map picture and rules; NEW GAME,
//   UPDATE, WATCH, JOIN and PREVIOUS MENU.
//
// The screen is not modal: the screens over it take what they show over it,
// and nothing under it sees the pointer. While one of its fields holds the
// focus it takes every key that types; otherwise the arrows choose, Return
// presses SELECT or JOIN, and every other key goes on to the game's screen
// under it, whose quick keys and Escape stay the game's.
#pragma once

#include "oa_layer.hpp"

#include "oa/app/window_icon.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app {

/// The Multiplayer screen's name on the OA layer.
inline constexpr std::string_view multiplayer_menu_screen_name = "multiplayer-menu";

/// Which step the Multiplayer screen shows.
enum class MultiplayerMenuStep : uint8_t {
    connection, ///< the connections, over SELPROV.GUI
    games,      ///< the games found, over SELGAME.GUI
};

/// A field the player types into.
enum class MultiplayerMenuField : uint8_t {
    none,     ///< no field holds the focus
    address,  ///< the address to look for games at
    nickname, ///< the player's name
    password, ///< the password the chosen game asks for
};

/// One connection the game offers.
struct MultiplayerMenuConnection {
    std::string name{};  ///< its name, as the game lists it
    std::string about{}; ///< what it does, in the language shown
    bool takes_address{}; ///< it looks for games at an address the player types

    friend bool operator==(const MultiplayerMenuConnection&, const MultiplayerMenuConnection&) =
        default;
};

/// One fact about a game: what it is and its value.
struct MultiplayerMenuFact {
    std::string label{}; ///< what it is, in the language shown
    std::string value{}; ///< its value, in the language shown

    friend bool operator==(const MultiplayerMenuFact&, const MultiplayerMenuFact&) = default;
};

/// One game found.
struct MultiplayerMenuGame {
    std::string name{};    ///< its name
    std::string map{};     ///< its map's name, as the game list shows it
    std::string map_file{}; ///< its map's name in the game data, as its host gave it
    std::string players{}; ///< the players in it and the most it takes, such as 3 / 10
    std::string ping{};    ///< how long a message takes to its host, such as 24 ms
    std::string status{};  ///< open, full, closed, playing or of another version
    bool open{};           ///< a player may join it
    std::vector<MultiplayerMenuFact> facts{}; ///< its rules, for DETAILS

    friend bool operator==(const MultiplayerMenuGame&, const MultiplayerMenuGame&) = default;
};

/// What the Multiplayer screen shows of the game's screen under it now.
struct MultiplayerMenuFacts {
    MultiplayerMenuStep step{}; ///< which step shows

    std::vector<MultiplayerMenuConnection> connections{}; ///< the connections offered
    std::optional<std::size_t> connection{}; ///< the chosen one; none for none
    std::string address{};                    ///< the address typed

    std::string connection_name{}; ///< the connection the games are found over
    std::string connection_line{}; ///< where they are looked for
    std::string nickname{};        ///< the player's name
    std::string password{};        ///< the password typed
    bool password_asked{};         ///< the chosen game asks for a password
    std::vector<MultiplayerMenuGame> games{}; ///< the games found, in the game's order
    std::optional<std::size_t> game{};        ///< the chosen one; none for none
    bool can_join{};               ///< JOIN takes a press
    bool can_watch{};              ///< WATCH takes a press

    char select_key{};  ///< SELECT's quick key; 0 for none
    char options_key{}; ///< OPTIONS' quick key; 0 for none
    char new_key{};     ///< NEW GAME's quick key; 0 for none
    char update_key{};  ///< UPDATE's quick key; 0 for none
    char watch_key{};   ///< WATCH's quick key; 0 for none
    char join_key{};    ///< JOIN's quick key; 0 for none
    char back_key{};    ///< PREVIOUS MENU's quick key; 0 for none
    std::string version{}; ///< the version label; empty for no plate

    friend bool operator==(const MultiplayerMenuFacts&, const MultiplayerMenuFacts&) = default;
};

/// A button of the game's screen the Multiplayer screen presses.
enum class MultiplayerMenuButton : uint8_t {
    select,   ///< SELECT, the connection chosen
    back,     ///< PREVIOUS MENU
    new_game, ///< NEW GAME
    update,   ///< UPDATE
    watch,    ///< WATCH
    join,     ///< JOIN
};

/// What the Multiplayer screen's presses do.
struct MultiplayerMenuActions {
    /// Chooses a connection, by its place.
    std::function<void(std::size_t)> choose_connection{};
    /// Chooses a game, by its place.
    std::function<void(std::size_t)> choose_game{};
    /// Presses a button of the game's screen.
    std::function<void(MultiplayerMenuButton)> press{};
    /// Opens Settings, which hold what the game's Options set.
    std::function<void()> options{};
    /// Adds text typed into a field to its end.
    std::function<void(MultiplayerMenuField, std::string_view)> type{};
    /// Takes the last character of a field away.
    std::function<void(MultiplayerMenuField)> erase{};
    /// Returns a map's picture, the part of its minimap the map covers, by
    /// its name in the game data; an empty picture for none. Asked again
    /// when the chosen game's map changes.
    std::function<WindowIcon(std::string_view)> map_picture{};
};

/// Makes the Multiplayer screen.
///
/// @param shows tells whether the screen shows; it closes when it does not
/// @param facts reads what the game's screen under it shows now
/// @param actions what its presses do
/// @return the screen
[[nodiscard]] std::unique_ptr<LayerScreen> make_multiplayer_menu(
    std::function<bool()> shows,
    std::function<MultiplayerMenuFacts()> facts,
    MultiplayerMenuActions actions
);

} // namespace oa::app
