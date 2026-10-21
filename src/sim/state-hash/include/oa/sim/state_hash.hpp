// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The match-state digest: one 64-bit value over the state a savegame
// carries, so that two runs of a match, or a match and the same match saved
// and loaded again, compare at a tick.
#pragma once

#include "oa/base/game_loop.hpp"
#include "oa/sim/match_runtime.hpp"
#include "oa/sim/world_environment/meteor.hpp"

#include <cstdint>

namespace oa::sim::trace {

/// Digests the match state a savegame carries.
///
/// 64-bit FNV-1a from digest_basis over each value's bytes as the host holds
/// them, in this order: Game.tick; the timing's requested rate, actual rate,
/// adaptation and flags; for each player slot that is not free, its index,
/// status, energy, metal, shared storages, produced, requested and wasted
/// totals, kills, losses, next economy tick, unit count, resource flags,
/// alliances, side and colour; for each live unit with a type, in slot order,
/// the unit record fields a save keeps (the last attacker only while it
/// lives, the flag bits the unit writer packs), its economy, every weapon
/// slot, its exported script state, its movement block and its saved orders
/// and goals as their blobs hold them; each plot's metal and placing-player
/// bits; the sight words; the camera; the meteor state when there is one; and,
/// when the match keeps rule state, the profile's sim hash and every rule-state
/// table, and, when the match holds a mutator digest, those bytes, including
/// when it keeps no rule state (Match::fold_rule_state).
///
/// @param match match to digest; nothing in it changes, but its script
///        lookups take it mutable
/// @param timing the match clock
/// @param camera_x camera X position a save's Camera account holds
/// @param camera_z camera Z position a save's Camera account holds
/// @param meteor meteor storm state, or null when the match keeps none
/// @return the digest
uint64_t match_state_hash(
    sim::match_runtime::Match& match,
    const base::game_loop::Timing& timing,
    int32_t camera_x,
    int32_t camera_z,
    const sim::world_environment::MeteorState* meteor
);

} // namespace oa::sim::trace
