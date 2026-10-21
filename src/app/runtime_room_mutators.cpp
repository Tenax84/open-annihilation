// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The battle room's mutator layer. Each machine builds the host's list on
// its own mod data, without the map, and says in its presence record what
// it built. The match mounts its own layer when it starts.

#include "oa/app/runtime.hpp"

#include "oa/base/sha256.hpp"
#include "oa/data/mutator_library/library.hpp"
#include "oa/data/mutators/choice.hpp"
#include "oa/data/mutators/loadout.hpp"
#include "oa/data/mutators/pipeline.hpp"
#include "oa/formats/oamut/manifest.hpp"
#include "oa/netgame/presence.hpp"
#include "oa/netgame/records.hpp"
#include "oa/ui/frontend_multiplayer/lobby.hpp"
#include "oa/ui/frontend_multiplayer/screens.hpp"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <new>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {

namespace mutators = oa::data::mutators;
namespace mp = oa::ui::frontend_multiplayer;

namespace {

/// The wire's two-bit codes (presence_mutator_state).
constexpr uint8_t kHave = 0;
constexpr uint8_t kMissing = 1;
constexpr uint8_t kOtherRelease = 2;
constexpr uint8_t kUnknown = 3;

/// One line, with breaks turned into spaces.
///
/// @param text the text
/// @return the text on one line
std::string one_line(std::string text) {
    for (char& byte : text) {
        if (byte == '\n' || byte == '\r')
            byte = ' ';
    }
    return text;
}

/// The first count of a digest's hexadecimal digits.
///
/// @param digest the digest
/// @param count how many digits, at most 64
/// @return the digits
std::string hex_digits(const oa::base::sha256::Digest& digest, std::size_t count) {
    const auto text = oa::base::sha256::to_hex(digest);
    if (count > text.size())
        count = text.size();
    return std::string(text.data(), count);
}

/// The position of the first mutator that did not apply.
///
/// @param chosen how many mutators were run
/// @param result the pipeline's result
/// @return that position, or none when the list was empty
uint8_t failing_position(std::size_t chosen, const mutators::PipelineResult& result) noexcept {
    if (result.mutators.empty())
        return chosen == 0 ? netgame::presence_mutator_index_none : static_cast<uint8_t>(0);
    for (std::size_t at = 0; at < result.mutators.size() && at < netgame::presence_mutators_max;
         ++at) {
        if (result.mutators[at].outcome != mutators::Outcome::applied)
            return static_cast<uint8_t>(at);
    }
    return chosen == 0 ? netgame::presence_mutator_index_none : static_cast<uint8_t>(0);
}

/// Tells whether this machine is hosting the battle room.
///
/// @return true when the local player has the host role
bool room_hosting() {
    if (!mp::multiplayer_showing())
        return false;
    mp::Lobby& lobby = mp::multiplayer_lobby();
    if (lobby.game == nullptr)
        return false;
    return (mp::local_info(lobby).role & mp::kRoleHost) != 0;
}

/// The first position whose code is not has.
///
/// @param codes one code per position
/// @return that position, or none
uint8_t first_not_has(const std::vector<uint8_t>& codes) noexcept {
    for (std::size_t at = 0; at < codes.size() && at < netgame::presence_mutators_max; ++at) {
        if (codes[at] != kHave)
            return static_cast<uint8_t>(at);
    }
    return netgame::presence_mutator_index_none;
}

/// Writes the host's mutators into a presence field.
///
/// An option the wire cannot carry is left out, so one value does not
/// refuse the whole field.
///
/// @param[out] field the field
/// @param chosen the mutators, top first
void fill_host_items(
    netgame::PresenceMutators& field, const std::vector<mutators::ResolvedMutator>& chosen
) {
    field.mutators.reserve(chosen.size());
    for (const mutators::ResolvedMutator& one : chosen) {
        netgame::PresenceMutator item;
        item.id = one.manifest.id;
        item.version = oa::formats::oamut::version_text(one.manifest.version);
        std::memcpy(item.sha256_prefix.data(), one.package.data(), item.sha256_prefix.size());
        for (const mutators::LoadoutOption& option : one.options) {
            if (item.options.size() >= netgame::presence_mutator_options_max)
                break;
            netgame::PresenceMutatorOption wire;
            wire.key = option.id;
            wire.value = mutators::option_text(option);
            if (wire.key.empty() || wire.key.size() > 32 || wire.value.size() > 32)
                continue;
            item.options.push_back(std::move(wire));
        }
        field.mutators.push_back(std::move(item));
    }
}

/// Greets again each peer that has built this digest, the first time it says so.
///
/// @param digest the loadout this machine built
/// @param[in,out] greeted the player ids already greeted for this digest
void regreet_built(const oa::base::sha256::Digest& digest, std::vector<uint32_t>& greeted) {
    if (!room_hosting())
        return;
    mp::Lobby& lobby = mp::multiplayer_lobby();
    if (lobby.game == nullptr)
        return;
    for (int32_t slot = 0; slot < mp::kSlotCount; ++slot) {
        const Player& player = mp::slot_player(lobby, slot);
        if (!mp::slot_remote_playing(lobby, player))
            continue;
        const mp::PlayerSetupInfo* info = mp::player_info(lobby, player);
        if (info == nullptr)
            continue;
        if (!netgame::sent_by_open_annihilation(reinterpret_cast<const uint8_t*>(info)))
            continue;
        const uint32_t player_id = player.player_id;
        if (player_id == 0)
            continue;
        const mp::PresenceRecords& records = lobby.presence_records;
        if (records.peer_size[slot] == 0 || records.peer_id[slot] != player_id)
            continue;
        const std::optional<netgame::PresenceMutatorsState> row = netgame::presence_mutators_state(
            records.peer[slot], records.peer_size[slot]
        );
        if (!row || !row->built || row->digest != digest)
            continue;
        bool seen = false;
        for (const uint32_t id : greeted) {
            if (id == player_id) {
                seen = true;
                break;
            }
        }
        if (seen)
            continue;
        greeted.push_back(player_id);
        mp::multiplayer_regreet(player_id);
    }
}

/// Reloads the battle room's unit headers. A failure leaves the table as it was.
void reload_units() noexcept {
    try {
        mp::multiplayer_units_changed();
    } catch (const std::exception&) {
    }
}

} // namespace

struct Runtime::RoomMutatorState {
    bool seeded = false;
    bool built = false;
    bool failed = false;
    bool mounted = false;
    bool send = false;
    bool released_empty = false;
    bool over_limit = false;
    uint8_t failed_index = netgame::presence_mutator_index_none;
    std::vector<mutators::ResolvedMutator> chosen{};
    std::optional<oa::base::sha256::Digest> built_loadout{};
    std::optional<oa::base::sha256::Digest> failed_loadout{};
    std::optional<oa::base::sha256::Digest> layer{};
    netgame::PresenceMutators outgoing{};
    std::vector<uint32_t> regreeted{};
};

void Runtime::destroy_room_mutator_state(RoomMutatorState* state) noexcept {
    delete state;
}

Runtime::RoomMutatorState& Runtime::ensure_room_mutators() {
    if (room_mutator_state_ == nullptr) {
        std::unique_ptr<RoomMutatorState, void (*)(RoomMutatorState*) noexcept> made{
            new RoomMutatorState, destroy_room_mutator_state
        };
        room_mutator_state_ = std::move(made);
    }
    return *room_mutator_state_;
}

void Runtime::set_room_mutators(std::vector<mutators::ResolvedMutator> chosen) {
    RoomMutatorState& state = ensure_room_mutators();
    state.chosen = std::move(chosen);
    state.seeded = true;
    state.failed = false;
    state.failed_loadout.reset();
    state.failed_index = netgame::presence_mutator_index_none;
    state.over_limit = false;
}

std::span<const mutators::ResolvedMutator> Runtime::room_mutators() const noexcept {
    if (room_mutator_state_ == nullptr)
        return {};
    return room_mutator_state_->chosen;
}

std::optional<netgame::PresenceMutators> Runtime::room_mutators_presence() const {
    if (room_mutator_state_ == nullptr || !room_mutator_state_->send)
        return std::nullopt;
    return room_mutator_state_->outgoing;
}

void Runtime::release_room_mutators() noexcept {
    RoomMutatorState* state = room_mutator_state_.get();
    if (state == nullptr)
        return;
    state->send = false;
    if (!state->mounted)
        return;
    try {
        if (!assets_.mutator_layer_mounted()) {
            state->mounted = false;
            state->built = false;
            state->built_loadout.reset();
            state->layer.reset();
            return;
        }
        if (!assets_.unmount_mutator_layer())
            return;
        state->mounted = false;
        state->built = false;
        state->built_loadout.reset();
        state->layer.reset();
        state->regreeted.clear();
        std::cerr << "open-annihilation: mutators: room released\n";
        std::cerr.flush();
    } catch (...) {
    }
}

bool Runtime::drop_room_layer() noexcept {
    RoomMutatorState* held = room_mutator_state_.get();
    if (held == nullptr || !held->mounted)
        return true;
    RoomMutatorState& state = *held;
    try {
        if (!assets_.mutator_layer_mounted()) {
            state.mounted = false;
            return true;
        }
        if (!assets_.unmount_mutator_layer())
            return false;
        state.mounted = false;
        std::cerr << "open-annihilation: mutators: room released\n";
        std::cerr.flush();
        return true;
    } catch (...) {
        return false;
    }
}

void Runtime::release_empty_room() {
    RoomMutatorState& state = ensure_room_mutators();
    state.send = false;
    if (state.released_empty && !state.mounted)
        return;
    const bool had = state.mounted || state.built || state.built_loadout.has_value();
    if (!had) {
        state.released_empty = true;
        state.built = false;
        state.failed = false;
        state.failed_loadout.reset();
        return;
    }
    if (!drop_room_layer())
        return;
    state.built = false;
    state.built_loadout.reset();
    state.layer.reset();
    state.failed = false;
    state.failed_loadout.reset();
    state.regreeted.clear();
    state.released_empty = true;
    state.send = false;
    reload_units();
}

bool Runtime::build_room_layer(
    const std::vector<mutators::ResolvedMutator>& resolved, const oa::base::sha256::Digest& wanted
) {
    RoomMutatorState& state = ensure_room_mutators();
    // The match's layer shares the store. Leave it, and do not record a failure.
    if (!state.mounted && assets_.mutator_layer_mounted())
        return false;
    if (!drop_room_layer())
        return false;
    const auto started = std::chrono::steady_clock::now();
    const auto fail = [&](std::string reason, uint8_t position) {
        try {
            if (assets_.mutator_layer_mounted()) {
                if (!assets_.unmount_mutator_layer())
                    state.mounted = true;
                else
                    state.mounted = false;
            } else {
                state.mounted = false;
            }
        } catch (...) {
            state.mounted = true;
        }
        state.built = false;
        state.layer.reset();
        state.failed = true;
        state.failed_loadout = wanted;
        state.failed_index = position;
        state.built_loadout.reset();
        state.regreeted.clear();
        std::cerr << "open-annihilation: mutators: room failed: " << one_line(std::move(reason))
                  << '\n';
        std::cerr.flush();
    };
    try {
        const mutators::PipelineInput input = mutator_pipeline_input(resolved);
        mutators::PipelineResult result = mutators::run_pipeline(assets_, input);
        if (!result.playable()) {
            fail(mutator_failure_text(resolved, result), failing_position(resolved.size(), result));
            return true;
        }
        const mutators::Loadout applied =
            mutators::applied_loadout(mutators::loadout_of(resolved), result);
        const oa::base::sha256::Digest layer =
            mutators::layer_digest(result.layer, [this](std::string_view path) {
                return assets_.read(path).bytes;
            });
        const oa::base::sha256::Digest folded = mutators::mutator_digest(applied, layer);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started
        );
        std::cerr << "open-annihilation: mutators: room built " << resolved.size() << " in "
                  << elapsed.count() << " ms loadout " << hex_digits(wanted, 16) << " layer "
                  << hex_digits(layer, 16) << '\n';
        const oa::base::sha256::Digest played = played_sim_hash(*this);
        const oa::base::sha256::Digest integrity = mutators::fold_sim_hash(played, folded);
        std::cerr << "open-annihilation: mutators: integrity " << hex_digits(integrity, 64) << '\n';
        std::cerr.flush();
        state.mounted = assets_.mutator_layer_mounted();
        state.built = true;
        state.failed = false;
        state.failed_loadout.reset();
        state.failed_index = netgame::presence_mutator_index_none;
        state.built_loadout = wanted;
        state.layer = layer;
        state.regreeted.clear();
        state.released_empty = false;
        reload_units();
        return true;
    } catch (const std::exception& error) {
        fail(
            error.what(),
            resolved.empty() ? netgame::presence_mutator_index_none : static_cast<uint8_t>(0)
        );
        return true;
    } catch (...) {
        fail(
            "the mutators did not apply",
            resolved.empty() ? netgame::presence_mutator_index_none : static_cast<uint8_t>(0)
        );
        return true;
    }
}

bool Runtime::resolve_guest_loadout(
    const netgame::PresenceLoadoutAssembly& assembly,
    std::vector<mutators::ResolvedMutator>& resolved,
    std::vector<uint8_t>& codes,
    oa::base::sha256::Digest& wanted
) {
    const fs::path folder =
        user_folder_ / std::string(oa::data::mutator_library::mutators_folder_name);
    resolved.clear();
    codes.assign(assembly.mutators.size(), kHave);
    bool all_has = true;
    for (std::size_t at = 0; at < assembly.mutators.size(); ++at) {
        const netgame::PresenceMutator& item = assembly.mutators[at];
        mutators::MutatorChoice choice;
        choice.target = item.id;
        choice.by_path = false;
        choice.options.reserve(item.options.size());
        for (const netgame::PresenceMutatorOption& option : item.options)
            choice.options.emplace_back(option.key, option.value);
        const std::optional<mutators::ResolvedMutator> found =
            mutators::resolve_choice(choice, folder, nullptr);
        if (!found) {
            codes[at] = kMissing;
            all_has = false;
            continue;
        }
        const bool version_differs =
            oa::formats::oamut::version_text(found->manifest.version) != item.version;
        const bool prefix_differs = std::memcmp(
                                         found->package.data(), item.sha256_prefix.data(),
                                         item.sha256_prefix.size()
                                     ) != 0;
        if (version_differs || prefix_differs) {
            codes[at] = kOtherRelease;
            all_has = false;
            continue;
        }
        resolved.push_back(*found);
    }
    if (!all_has)
        return false;
    wanted = mutators::loadout_digest(mutators::loadout_of(resolved));
    if (wanted == assembly.digest)
        return true;
    if (!codes.empty())
        codes[0] = kOtherRelease;
    return false;
}

void Runtime::publish_host_mutators(const oa::base::sha256::Digest& digest) {
    RoomMutatorState& state = ensure_room_mutators();
    netgame::PresenceMutators field;
    field.built = state.built;
    field.failed = state.failed;
    field.digest = digest;
    if (state.built && state.layer)
        field.layer_digest = *state.layer;
    field.index = state.failed ? state.failed_index : netgame::presence_mutator_index_none;
    const std::size_t count = state.chosen.size() < netgame::presence_mutators_max
                                  ? state.chosen.size()
                                  : netgame::presence_mutators_max;
    field.total = static_cast<uint8_t>(count);
    field.held = field.total;
    fill_host_items(field, state.chosen);
    state.outgoing = std::move(field);
    state.send = true;
}

void Runtime::publish_guest_mutators(
    const oa::base::sha256::Digest& digest,
    uint8_t total,
    uint8_t held,
    const std::vector<uint8_t>& codes,
    bool lacking,
    uint8_t lacking_at
) {
    RoomMutatorState& state = ensure_room_mutators();
    netgame::PresenceMutators field;
    const bool failed_this =
        state.failed && state.failed_loadout && *state.failed_loadout == digest;
    const bool built_this =
        state.built && state.built_loadout && *state.built_loadout == digest && !failed_this;
    field.built = built_this && !lacking;
    field.lacking = lacking;
    field.failed = failed_this && !lacking;
    field.digest = digest;
    if (field.built && state.layer)
        field.layer_digest = *state.layer;
    if (lacking)
        field.index = lacking_at;
    else if (field.failed)
        field.index = state.failed_index;
    else
        field.index = netgame::presence_mutator_index_none;
    field.total = total;
    field.held = held;
    const uint8_t limit = total < netgame::presence_mutators_max
                              ? total
                              : static_cast<uint8_t>(netgame::presence_mutators_max);
    for (uint8_t at = 0; at < limit; ++at) {
        const uint8_t code = at < codes.size() ? codes[at] : kUnknown;
        netgame::presence_mutator_set_state(field.states, at, code);
    }
    state.outgoing = std::move(field);
    state.send = true;
}

void Runtime::follow_room_mutators() {
    if (match_running())
        return;
    if (!mp::multiplayer_showing()) {
        RoomMutatorState* state = room_mutator_state_.get();
        if (state == nullptr)
            return;
        const bool had = state->mounted;
        if (had)
            release_room_mutators();
        state->send = false;
        if (had && !state->mounted)
            reload_units();
        return;
    }

    RoomMutatorState& state = ensure_room_mutators();
    const bool hosting = room_hosting();

    if (hosting && !state.seeded) {
        const auto listed = chosen_match_mutators();
        state.chosen.assign(listed.begin(), listed.end());
        state.seeded = true;
    }

    if (!hosting) {
        netgame::PresenceLoadoutAssembly assembly;
        if (!mp::multiplayer_room_mutators(&assembly) || assembly.total == 0) {
            if (!state.seeded)
                state.chosen.clear();
            release_empty_room();
            return;
        }
        if (assembly.total > netgame::presence_mutators_max ||
            assembly.mutators.size() > assembly.total) {
            state.send = false;
            return;
        }
        if (!netgame::presence_assembly_whole(assembly)) {
            state.released_empty = false;
            publish_guest_mutators(
                assembly.digest, assembly.total, static_cast<uint8_t>(assembly.mutators.size()),
                {}, false, netgame::presence_mutator_index_none
            );
            return;
        }
        if (state.built && state.mounted && state.built_loadout &&
            *state.built_loadout == assembly.digest)
            return;
        if (state.failed && state.failed_loadout && *state.failed_loadout == assembly.digest &&
            state.send) {
            drop_room_layer();
            return;
        }
        std::vector<mutators::ResolvedMutator> resolved;
        std::vector<uint8_t> codes;
        oa::base::sha256::Digest wanted{};
        if (!resolve_guest_loadout(assembly, resolved, codes, wanted)) {
            const bool drop = !state.built_loadout || *state.built_loadout != assembly.digest;
            const bool had = state.mounted;
            if (drop && had) {
                if (drop_room_layer()) {
                    state.built = false;
                    state.built_loadout.reset();
                    state.layer.reset();
                    reload_units();
                }
            }
            state.built = false;
            state.failed = false;
            state.chosen.clear();
            state.released_empty = false;
            publish_guest_mutators(
                assembly.digest, assembly.total, assembly.total, codes, true, first_not_has(codes)
            );
            return;
        }
        state.released_empty = false;
        state.chosen = resolved;
        if (!(state.built && state.mounted && state.built_loadout &&
              *state.built_loadout == wanted)) {
            if (!build_room_layer(resolved, wanted))
                return;
        }
        std::vector<uint8_t> has_codes(assembly.total, kHave);
        publish_guest_mutators(wanted, assembly.total, assembly.total, has_codes, false, 0);
        return;
    }

    if (state.chosen.empty()) {
        release_empty_room();
        return;
    }
    if (state.chosen.size() > netgame::presence_mutators_max) {
        if (!state.over_limit) {
            state.over_limit = true;
            std::cerr << "open-annihilation: mutators: room failed: the list names more than "
                      << netgame::presence_mutators_max << " mutators\n";
            std::cerr.flush();
        }
        state.send = false;
        return;
    }
    state.over_limit = false;
    state.released_empty = false;
    const oa::base::sha256::Digest wanted =
        mutators::loadout_digest(mutators::loadout_of(state.chosen));
    if (state.built && state.mounted && state.built_loadout && *state.built_loadout == wanted) {
        regreet_built(*state.built_loadout, state.regreeted);
        return;
    }
    if (state.failed && state.failed_loadout && *state.failed_loadout == wanted && state.send) {
        drop_room_layer();
        return;
    }
    const std::vector<mutators::ResolvedMutator> resolved = state.chosen;
    if (!build_room_layer(resolved, wanted))
        return;
    publish_host_mutators(wanted);
    if (state.built && state.built_loadout)
        regreet_built(*state.built_loadout, state.regreeted);
}

} // namespace oa::app
