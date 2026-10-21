// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// English texts Get Map passes to the interface catalogue. oa-tool joins
// this list with the other modules' lists.

#pragma once

#include <string_view>

namespace oa::ui::frontend_multiplayer {

/// English texts this module passes to the interface catalogue
/// (oa/data/languages/interface_text.hpp). oa-tool sorts the modules'
/// lists together and keeps one of each text.
inline constexpr std::string_view interface_words[] = {
    "*** {player} plays Total Annihilation 3.1c and cannot join a game with mutators.",
    "*** {player} plays Total Annihilation 3.1c, which cannot play with mutators.",
    "Add {registry} to get {pack}",
    "CANCEL",
    "COMMANDER",
    "Cancel",
    "Couldn't get {pack}: {reason}",
    "Different from the host: a file does not match.",
    "Different from the host: another base game ({name}).",
    "Different from the host: no {name}.",
    "Different from the host: {name} is on.",
    "Expansions",
    "Downloading {pack} · {done} of {size}",
    "Downloads from {registry} are off (Settings › Downloads).",
    "Easy",
    "GET MAP",
    "Hard",
    "LOAD",
    "Medium",
    "Mod",
    "OK",
    "Off",
    "Open Annihilation",
    "PLAYERS",
    "PREV",
    "Platform",
    "Previous",
    "Rules",
    "SELECT",
    "SETTINGS",
    "SIZE",
    "The game cannot start until the content matches.",
    "The game cannot start until the host turns it on, or {player} turns it off.",
    "The game cannot start until {player} turns it on, or the host turns it off.",
    "The host's release of {pack} is no longer offered by {registry}.",
    "Total Annihilation 3.1c",
    "UPDATE",
    "View",
    "You have release {mine} of {pack}; the host plays release {theirs}.",
    "{pack} {version} · {size} from {registry}",
};

} // namespace oa::ui::frontend_multiplayer
