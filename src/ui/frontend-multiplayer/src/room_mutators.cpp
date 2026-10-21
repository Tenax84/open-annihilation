// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Whether the battle room may start, from the mutators each machine reports.
#include "oa/ui/frontend_multiplayer/lobby.hpp"

#include "oa/netgame/records.hpp"

#include <cstdint>
#include <iostream>
#include <ostream>
#include <span>

namespace oa::ui::frontend_multiplayer {

namespace {

/// Names a hold the way the host's log prints it.
///
/// @param hold the hold
/// @return its enumerator name
const char* hold_name(MutatorsHold hold) noexcept {
    switch (hold) {
    case MutatorsHold::none:
        return "none";
    case MutatorsHold::host_not_built:
        return "host_not_built";
    case MutatorsHold::host_failed:
        return "host_failed";
    case MutatorsHold::no_support:
        return "no_support";
    case MutatorsHold::receiving:
        return "receiving";
    case MutatorsHold::lacking:
        return "lacking";
    case MutatorsHold::failed:
        return "failed";
    case MutatorsHold::building:
        return "building";
    case MutatorsHold::other_result:
        return "other_result";
    case MutatorsHold::other_sim_hash:
        return "other_sim_hash";
    }
    return "none";
}

/// Tells whether a slot is an Open Annihilation human or watcher of another machine.
///
/// The same players lobby_content_allows_start walks, without the row badge:
/// remote, playing, and carrying the engine signature. A 3.1c player and this
/// machine's own players are not.
///
/// @param lobby Lobby state.
/// @param player_slot Player slot.
/// @return True when that slot would be asked before START.
bool other_machine_oa(Lobby& lobby, int32_t player_slot) noexcept {
    if (lobby.game == nullptr || player_slot < 0 || player_slot >= kSlotCount)
        return false;
    const auto& player = slot_player(lobby, player_slot);
    if (!slot_remote_playing(lobby, player))
        return false;
    const auto* info = player_info(lobby, player);
    if (info == nullptr)
        return false;
    return netgame::sent_by_open_annihilation(reinterpret_cast<const uint8_t*>(info));
}

/// The first mutator of a peer that is not installed at the list's release.
///
/// @param states the peer's state word
/// @param total how many mutators the host's list names
/// @param[out] position that mutator's position
/// @return True when one is not has.
bool first_not_has(uint64_t states, uint8_t total, uint8_t* position) noexcept {
    const auto limit = total < netgame::presence_mutators_max
                           ? total
                           : static_cast<uint8_t>(netgame::presence_mutators_max);
    for (uint8_t at = 0; at < limit; ++at) {
        if (netgame::presence_mutator_state(states, at) == 0)
            continue;
        *position = at;
        return true;
    }
    return false;
}

/// Why one peer holds START, or none.
///
/// @param host_state the host's mutator field
/// @param host_record the host's record
/// @param row_state the peer's mutator field; nothing when it sent none
/// @param row_record the peer's record
/// @param player_slot the peer's slot
/// @return The hold, or none.
MutatorsBlock peer_hold(
    const netgame::PresenceMutatorsState& host_state,
    std::span<const uint8_t> host_record,
    const netgame::PresenceMutatorsState* row_state,
    std::span<const uint8_t> row_record,
    int32_t player_slot
) noexcept {
    const uint8_t none = netgame::presence_mutator_index_none;
    if (row_state == nullptr)
        return {MutatorsHold::no_support, player_slot, none};
    if (row_state->digest != host_state.digest || row_state->held < host_state.total)
        return {MutatorsHold::receiving, player_slot, none};
    uint8_t lacking = 0;
    if (first_not_has(row_state->states, host_state.total, &lacking))
        return {MutatorsHold::lacking, player_slot, lacking};
    if (row_state->failed)
        return {MutatorsHold::failed, player_slot, row_state->index};
    if (!row_state->built)
        return {MutatorsHold::building, player_slot, none};
    if (row_state->layer_digest != host_state.layer_digest)
        return {MutatorsHold::other_result, player_slot, none};
    const auto host_hash = netgame::presence_sim_hash(host_record.data(), host_record.size());
    const auto row_hash = netgame::presence_sim_hash(row_record.data(), row_record.size());
    if (host_hash && row_hash && *host_hash != *row_hash)
        return {MutatorsHold::other_sim_hash, player_slot, none};
    return {};
}

/// The first reason START waits, looking at every remote player or at one.
///
/// @param lobby Lobby state.
/// @param only_slot One slot, or a negative value for every slot in order.
/// @return The first hold, or none.
MutatorsBlock block_for(Lobby& lobby, int32_t only_slot) noexcept {
    if (lobby.game == nullptr)
        return {};
    const auto host = lobby_host_slot(lobby);
    if (host == kNoSlot)
        return {};
    const auto host_record = lobby_presence_record(lobby, static_cast<int32_t>(host));
    const auto host_state =
        netgame::presence_mutators_state(host_record.data(), host_record.size());
    if (!host_state)
        return {};
    // A failed build is named even when the built flag is also clear.
    if (host_state->failed)
        return {MutatorsHold::host_failed, static_cast<int32_t>(host), host_state->index};
    if (!host_state->built) {
        return {
            MutatorsHold::host_not_built,
            static_cast<int32_t>(host),
            netgame::presence_mutator_index_none
        };
    }
    if (only_slot >= kSlotCount)
        return {};
    const int32_t first = only_slot < 0 ? 0 : only_slot;
    const int32_t end = only_slot < 0 ? kSlotCount : only_slot + 1;
    if (first < 0)
        return {};
    for (int32_t player_slot = first; player_slot < end; ++player_slot) {
        if (player_slot == static_cast<int32_t>(host))
            continue;
        if (!other_machine_oa(lobby, player_slot))
            continue;
        const auto row = lobby_presence_record(lobby, player_slot);
        const auto row_state = netgame::presence_mutators_state(row.data(), row.size());
        const auto hold = peer_hold(
            *host_state, host_record, row_state ? &*row_state : nullptr, row, player_slot
        );
        if (hold.hold != MutatorsHold::none)
            return hold;
    }
    return {};
}

/// Tells whether this machine hosts.
///
/// @param lobby Lobby state.
/// @return True when the local player's block says host.
bool hosting(Lobby& lobby) noexcept {
    return lobby.game != nullptr && (local_info(lobby).role & kRoleHost) != 0;
}

} // namespace

MutatorHave lobby_mutator_have(Lobby& lobby, int32_t slot, uint8_t position) noexcept {
    if (position >= netgame::presence_mutators_max)
        return MutatorHave::unknown;
    const auto record = lobby_presence_record(lobby, slot);
    const auto state = netgame::presence_mutators_state(record.data(), record.size());
    if (!state || position >= state->total)
        return MutatorHave::unknown;
    switch (netgame::presence_mutator_state(state->states, position)) {
    case 0:
        return MutatorHave::has;
    case 1:
        return MutatorHave::missing;
    case 2:
        return MutatorHave::other_release;
    default:
        return MutatorHave::unknown;
    }
}

MutatorsBlock lobby_mutators_block(Lobby& lobby) noexcept {
    return block_for(lobby, -1);
}

bool lobby_mutators_allow_start(Lobby& lobby) noexcept {
    return lobby_mutators_block(lobby).hold == MutatorsHold::none;
}

bool lobby_mutators_differ_from_host(Lobby& lobby, int32_t slot) noexcept {
    if (lobby.game == nullptr || slot < 0 || slot >= kSlotCount)
        return false;
    const auto host = lobby_host_slot(lobby);
    if (host == kNoSlot || host == slot)
        return false;
    const auto block = block_for(lobby, slot);
    return block.slot == slot && block.hold != MutatorsHold::none;
}

void lobby_log_mutators_hold(Lobby& lobby) noexcept {
    if (!hosting(lobby))
        return;
    const auto block = lobby_mutators_block(lobby);
    if (lobby.mutators_hold_seen && block.hold == lobby.mutators_hold_logged.hold &&
        block.slot == lobby.mutators_hold_logged.slot &&
        block.position == lobby.mutators_hold_logged.position)
        return;
    lobby.mutators_hold_seen = true;
    lobby.mutators_hold_logged = block;
    if (block.hold == MutatorsHold::none) {
        std::cerr << "multiplayer: mutators allow START\n";
    } else {
        std::cerr << "multiplayer: mutators hold START: " << hold_name(block.hold) << " slot "
                  << block.slot << " position " << static_cast<unsigned>(block.position) << "\n";
    }
    std::cerr.flush();
}

} // namespace oa::ui::frontend_multiplayer
