// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Whether the battle room may start, from the mutators each machine reports.
#pragma once

#include "oa/netgame/presence.hpp"

#include <cstdint>

namespace oa::ui::frontend_multiplayer {

struct Lobby;

/// Whether one machine has one mutator of the room's list.
///
/// The values are the two-bit codes of a presence record's mutator states.
enum class MutatorHave : uint8_t {
    has,            ///< installed at the list's SHA-256 and version
    missing,        ///< not installed
    other_release,  ///< another release is installed
    unknown,        ///< the list is not whole yet, or the record does not say
};

/// Why the host's START waits on mutators, or none when it does not.
enum class MutatorsHold : uint8_t {
    none,            ///< nothing about mutators holds START
    host_not_built,  ///< the host's layer is not built yet
    host_failed,     ///< the host's build stopped with an error
    no_support,      ///< an OA player sent no mutator field
    receiving,       ///< an OA player is still taking the list, or names another one
    lacking,         ///< an OA player does not have a mutator of the list
    failed,          ///< an OA player's build stopped with an error
    building,        ///< an OA player has the list and has not built yet
    other_result,    ///< an OA player built a different layer
    other_sim_hash,  ///< an OA player plays another sim hash
};

/// Why START waits, and the player and mutator it names.
struct MutatorsBlock {
    MutatorsHold hold{}; ///< none when START is not waiting on mutators
    int32_t slot{-1};    ///< the player it names; -1 when it names none
    /// The mutator it names, or presence_mutator_index_none.
    uint8_t position{netgame::presence_mutator_index_none};
};

/// Returns whether one slot has one mutator of the room's list.
///
/// This machine's players are read from its own record. A record with no
/// mutator field, or a position past that field's total, is unknown.
/// Allocates nothing.
///
/// @param lobby Lobby state.
/// @param slot Player slot, 0..9.
/// @param position The mutator's position, 0 to 31.
/// @return The two-bit code, or unknown when the record does not say.
[[nodiscard]] MutatorHave
lobby_mutator_have(Lobby& lobby, int32_t slot, uint8_t position) noexcept;

/// Returns the first reason the host's START waits on mutators.
///
/// None when the host's record has no mutator field. Otherwise the host's
/// own build, then each Open Annihilation human or watcher of another
/// machine in slot order: no mutator field, another list or a list still
/// arriving, a mutator they do not have, a failed build, a build not
/// finished, another layer, another sim hash. Reads the fixed part of each
/// mutator field and the sim hash only. Allocates nothing.
///
/// @param lobby Lobby state.
/// @return The first hold, or none.
[[nodiscard]] MutatorsBlock lobby_mutators_block(Lobby& lobby) noexcept;

/// Tells whether mutators allow the host to start.
///
/// @param lobby Lobby state.
/// @return True when nothing about mutators holds START.
[[nodiscard]] bool lobby_mutators_allow_start(Lobby& lobby) noexcept;

/// Tells whether one slot alone would hold START.
///
/// The host's own row does not. A 3.1c player and this machine's computer
/// players do not. Another slot's problem does not hide this one.
/// Allocates nothing.
///
/// @param lobby Lobby state.
/// @param slot Player slot, 0..9; any other gives false.
/// @return True when this slot's mutators would hold START.
[[nodiscard]] bool lobby_mutators_differ_from_host(Lobby& lobby, int32_t slot) noexcept;

/// Logs a change of the host's mutator hold, including the first one seen.
///
/// The host logs `multiplayer: mutators hold START: <hold> slot <n>
/// position <p>` or `multiplayer: mutators allow START`. Another machine
/// logs nothing. The last hold is kept on the lobby, not in a function
/// local, so a test can reset it with the lobby.
///
/// @param[in,out] lobby Lobby state.
void lobby_log_mutators_hold(Lobby& lobby) noexcept;

} // namespace oa::ui::frontend_multiplayer
