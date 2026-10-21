// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The battle room's mutator layer. Each machine builds the host's list on
// its own mod data, without the map, and says in its presence record what
// it built. The match mounts its own layer when it starts.

#include "oa/app/runtime.hpp"

#include "mutator_state.hpp"

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
#include <filesystem>
#include <iostream>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {

namespace mutators = oa::data::mutators;
namespace mp = oa::ui::frontend_multiplayer;
namespace oamut = oa::formats::oamut;

namespace {

/// Tells whether this machine is hosting the battle room.
///
/// The role outlives the screen: the start handler runs after the battle
/// room has stopped counting as showing.
///
/// @return true when the local player hosts
bool room_hosting() {
    return mp::lobby_hosts(mp::multiplayer_lobby());
}

/// The first position whose code is not has.
///
/// @param codes one code per position
/// @return that position, or none
uint8_t first_not_has(const std::vector<uint8_t>& codes) noexcept {
    const auto has = static_cast<uint8_t>(mp::MutatorHave::has);
    for (std::size_t at = 0; at < codes.size() && at < netgame::presence_mutators_max; ++at) {
        if (codes[at] != has)
            return static_cast<uint8_t>(at);
    }
    return netgame::presence_mutator_index_none;
}

/// Tells whether an option key fits the wire.
///
/// @param key the key
/// @return true when it is 1 to 32 ascii bytes
bool wire_key(std::string_view key) noexcept {
    if (key.empty() || key.size() > netgame::presence_mutator_options_max * 4)
        return false;
    if (key.size() > 32)
        return false;
    for (const unsigned char byte : key) {
        if (byte < 0x21 || byte > 0x7e)
            return false;
    }
    return true;
}

/// Tells whether an option value fits the wire.
///
/// @param value the value
/// @return true when it is at most 32 text bytes
bool wire_value(std::string_view value) noexcept {
    if (value.size() > 32)
        return false;
    for (const unsigned char byte : value) {
        if (byte < 0x20 || byte == 0x7f)
            return false;
    }
    return true;
}

/// The manifest option of an id, or null.
///
/// @param manifest the manifest
/// @param id the option id
/// @return the option, or null
const oamut::Option*
manifest_option(const oamut::Manifest& manifest, std::string_view id) noexcept {
    for (const oamut::Option& option : manifest.options) {
        if (option.id == id)
            return &option;
    }
    return nullptr;
}

/// Why the host's options do not fit the wire, or empty when they do.
///
/// Only a value that differs from the manifest's default is sent. More than
/// eight of those, or a key or value the wire refuses, names that mutator.
///
/// @param chosen the mutators, top first
/// @param[out] position the mutator, when the reason is not empty
/// @return the reason, or empty
std::string
options_refuse(const std::vector<mutators::ResolvedMutator>& chosen, uint8_t* position) {
    for (std::size_t at = 0; at < chosen.size() && at < netgame::presence_mutators_max; ++at) {
        const mutators::ResolvedMutator& one = chosen[at];
        std::size_t differ = 0;
        for (const mutators::LoadoutOption& option : one.options) {
            const oamut::Option* defined = manifest_option(one.manifest, option.id);
            if (defined == nullptr)
                continue;
            if (mutators::option_text(option) ==
                mutators::option_text(mutators::default_option(*defined)))
                continue;
            ++differ;
            if (differ > netgame::presence_mutator_options_max) {
                *position = static_cast<uint8_t>(at);
                return one.manifest.id + ": more than 8 options differ from their defaults";
            }
            if (!wire_key(option.id) || !wire_value(mutators::option_text(option))) {
                *position = static_cast<uint8_t>(at);
                return one.manifest.id + ": an option does not fit on the wire";
            }
        }
    }
    return {};
}

/// Writes the options that differ from the manifest into a presence field.
///
/// @param[out] field the field
/// @param chosen the mutators, top first
void fill_host_items(
    netgame::PresenceMutators& field, const std::vector<mutators::ResolvedMutator>& chosen
) {
    field.mutators.reserve(chosen.size());
    for (const mutators::ResolvedMutator& one : chosen) {
        if (field.mutators.size() >= netgame::presence_mutators_max)
            break;
        netgame::PresenceMutator item;
        item.id = one.manifest.id;
        item.version = oamut::version_text(one.manifest.version);
        std::memcpy(item.sha256_prefix.data(), one.package.data(), item.sha256_prefix.size());
        for (const mutators::LoadoutOption& option : one.options) {
            if (item.options.size() >= netgame::presence_mutator_options_max)
                break;
            const oamut::Option* defined = manifest_option(one.manifest, option.id);
            if (defined == nullptr)
                continue;
            const std::string value = mutators::option_text(option);
            if (value == mutators::option_text(mutators::default_option(*defined)))
                continue;
            if (!wire_key(option.id) || !wire_value(value))
                continue;
            netgame::PresenceMutatorOption wire;
            wire.key = option.id;
            wire.value = value;
            item.options.push_back(std::move(wire));
        }
        field.mutators.push_back(std::move(item));
    }
}

/// Reads the unit headers again.
///
/// @return the error, or empty when they were read
std::string reload_units() {
    try {
        mp::multiplayer_units_changed();
        return {};
    } catch (const std::exception& error) {
        return error.what() != nullptr ? error.what() : "the unit headers could not be read again";
    } catch (...) {
        return "the unit headers could not be read again";
    }
}

/// The latest last-write time of the Mutators folder and each named package.
///
/// @param folder the Mutators folder
/// @param assembly the host's list
/// @return that time, or the epoch when none of them could be read
std::filesystem::file_time_type mutators_stamp(
    const std::filesystem::path& folder, const netgame::PresenceLoadoutAssembly& assembly
) {
    std::filesystem::file_time_type latest{};
    std::error_code error;
    const auto root = std::filesystem::last_write_time(folder, error);
    if (!error)
        latest = root;
    for (const netgame::PresenceMutator& item : assembly.mutators) {
        error.clear();
        const auto when = std::filesystem::last_write_time(folder / item.id, error);
        if (!error && when > latest)
            latest = when;
    }
    return latest;
}

/// The first position whose outcome is not applied.
///
/// @param result the pipeline's result
/// @return that position, or none when every mutator applied
uint8_t dropped_position(const mutators::PipelineResult& result) noexcept {
    for (std::size_t at = 0; at < result.mutators.size() && at < netgame::presence_mutators_max;
         ++at) {
        if (result.mutators[at].outcome != mutators::Outcome::applied)
            return static_cast<uint8_t>(at);
    }
    return netgame::presence_mutator_index_none;
}

} // namespace

/// One Open Annihilation peer and the list this machine last greeted it for.
struct GreetMark {
    uint32_t player_id = 0;
    bool saw = false;           ///< its record has carried a mutator field
    bool greeted_none = false;  ///< greeted after that field went away
    bool greeted_built = false; ///< greeted for built_for
    oa::base::sha256::Digest built_for{};
};

/// A guest resolve, kept until the digest, the folder or a second changes.
struct ResolveCache {
    oa::base::sha256::Digest digest{};
    bool valid = false;
    std::filesystem::file_time_type stamp{};
    std::chrono::steady_clock::time_point at{};
    bool ok = false;
    std::vector<mutators::ResolvedMutator> list{};
    std::vector<uint8_t> codes{};
    oa::base::sha256::Digest wanted{};
};

struct Runtime::RoomMutatorState {
    mp::RoomMutatorPhase phase = mp::RoomMutatorPhase::none;
    bool seeded = false;
    bool mounted = false;
    bool send = false;
    bool wanted_set = false;
    bool have_folded = false;
    bool assembly_known = false;
    bool over_logged = false;
    bool sync_logged = false;
    oa::base::sha256::Digest for_digest{};
    oa::base::sha256::Digest folded{};
    oa::base::sha256::Digest wanted{};
    std::string built_sim{};
    std::string wanted_sim{};
    uint8_t index = netgame::presence_mutator_index_none;
    uint8_t total = 0;
    uint8_t held = 0;
    uint32_t chosen_stamp = 1;
    uint32_t wanted_stamp = 0;
    uint32_t outgoing_generation = 0;
    uint32_t seen_generation = 0;
    std::vector<mutators::ResolvedMutator> chosen{};
    std::vector<mutators::ResolvedMutator> resolved{};
    std::vector<uint8_t> codes{};
    netgame::PresenceMutators outgoing{};
    netgame::PresenceLoadoutAssembly assembly{};
    std::vector<GreetMark> greets{};
    ResolveCache resolve{};
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
    ++state.chosen_stamp;
    state.over_logged = false;
}

std::span<const mutators::ResolvedMutator> Runtime::room_mutators() const noexcept {
    if (room_mutator_state_ == nullptr)
        return {};
    if (room_hosting())
        return room_mutator_state_->chosen;
    return room_mutator_state_->resolved;
}

std::span<const mutators::ResolvedMutator> Runtime::room_match_mutators() const {
    // The battle room clears its showing flag before the start handler runs,
    // so this answer cannot depend on that flag. Leaving the screens releases
    // the layer, and a phase other than built answers nothing.
    if (room_mutator_state_ == nullptr)
        return {};
    const RoomMutatorState& state = *room_mutator_state_;
    if (state.phase != mp::RoomMutatorPhase::built)
        return {};
    const std::string sim = simulation_hash(*this);
    if (state.built_sim != sim)
        return {};
    if (room_hosting()) {
        if (!state.wanted_set || state.for_digest != state.wanted)
            return {};
        return state.chosen;
    }
    if (state.assembly.total == 0 || state.for_digest != state.assembly.digest)
        return {};
    return state.resolved;
}

bool Runtime::room_followed() const noexcept {
    return room_mutator_state_ != nullptr;
}

bool Runtime::room_match_unbuilt() const {
    // See room_match_mutators: the start asks after the room has stopped showing.
    if (room_hosting()) {
        if (room_mutator_state_ == nullptr || room_mutator_state_->chosen.empty())
            return false;
        return room_match_mutators().empty();
    }
    mp::Lobby& lobby = mp::multiplayer_lobby();
    if (lobby.game == nullptr)
        return false;
    const auto host = mp::lobby_host_slot(lobby);
    if (host == mp::kNoSlot)
        return false;
    const auto record = mp::lobby_presence_record(lobby, static_cast<int32_t>(host));
    if (!netgame::presence_mutators_state(record.data(), record.size()))
        return false;
    return room_match_mutators().empty();
}

const netgame::PresenceMutators* Runtime::room_mutators_presence() const noexcept {
    if (room_mutator_state_ == nullptr || !room_mutator_state_->send)
        return nullptr;
    return &room_mutator_state_->outgoing;
}

uint32_t Runtime::room_mutators_generation() const noexcept {
    if (room_mutator_state_ == nullptr)
        return 0;
    return room_mutator_state_->outgoing_generation;
}

void Runtime::release_room_mutators() noexcept {
    RoomMutatorState* state = room_mutator_state_.get();
    if (state == nullptr)
        return;
    // The host's list and its resolve belong to the room left; the next
    // follow copies the lobby's list again, whatever its generation.
    state->assembly = {};
    state->assembly_known = false;
    state->resolve.valid = false;
    if (state->mounted && !drop_room_layer())
        return;
    state->phase = mp::RoomMutatorPhase::none;
    state->for_digest = {};
    state->folded = {};
    state->have_folded = false;
    state->index = netgame::presence_mutator_index_none;
    state->codes.clear();
    state->total = 0;
    state->held = 0;
    state->built_sim.clear();
    state->greets.clear();
    state->sync_logged = false;
    state->resolved.clear();
    if (!state->send)
        return;
    state->send = false;
    state->outgoing = {};
    ++state->outgoing_generation;
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
    if (state.phase == mp::RoomMutatorPhase::none && !state.mounted && !state.send)
        return;
    const bool reload = state.phase == mp::RoomMutatorPhase::built || state.mounted;
    if (state.mounted && !drop_room_layer())
        return;
    state.phase = mp::RoomMutatorPhase::none;
    state.for_digest = {};
    state.folded = {};
    state.have_folded = false;
    state.index = netgame::presence_mutator_index_none;
    state.codes.clear();
    state.total = 0;
    state.held = 0;
    state.built_sim.clear();
    state.resolved.clear();
    state.sync_logged = false;
    if (state.send) {
        state.send = false;
        state.outgoing = {};
        ++state.outgoing_generation;
    }
    if (!reload)
        return;
    const std::string error = reload_units();
    if (error.empty())
        return;
    std::cerr << "open-annihilation: mutators: room failed: the unit headers could not be read "
                 "again: "
              << one_line(error) << '\n';
    std::cerr.flush();
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
        state.phase = mp::RoomMutatorPhase::failed;
        state.for_digest = wanted;
        state.folded = {};
        state.have_folded = false;
        state.index = position;
        state.built_sim = simulation_hash(*this);
        state.sync_logged = false;
        state.total = resolved.empty() ? static_cast<uint8_t>(1)
                                       : static_cast<uint8_t>(
                                             resolved.size() < netgame::presence_mutators_max
                                                 ? resolved.size()
                                                 : netgame::presence_mutators_max
                                         );
        state.held = state.total;
        state.codes.assign(state.total, static_cast<uint8_t>(mp::MutatorHave::has));
        std::cerr << "open-annihilation: mutators: room failed: " << one_line(std::move(reason))
                  << '\n';
        std::cerr.flush();
    };
    try {
        uint8_t option_at = netgame::presence_mutator_index_none;
        const std::string options = options_refuse(resolved, &option_at);
        if (!options.empty()) {
            fail(options, option_at);
            return true;
        }
        mutators::PipelineInput input = mutator_pipeline_input(resolved);
        bind_planned_hack_check(input);
        mutators::PipelineResult result = mutators::run_pipeline(assets_, input);
        const std::optional<std::size_t> stopper = mutators::first_blocking(result, input.mutators);
        if (!result.refusal.empty() || stopper) {
            uint8_t position = netgame::presence_mutator_index_none;
            if (result.refusal.empty() && stopper && *stopper < netgame::presence_mutators_max)
                position = static_cast<uint8_t>(*stopper);
            fail(mutator_failure_text(resolved, result), position);
            return true;
        }
        // The match start's clash default, so the digest is the one it makes.
        const mutators::Loadout applied =
            mutators::applied_loadout(mutators::loadout_of(resolved, input.clash_default), result);
        const oa::base::sha256::Digest layer =
            mutators::layer_digest(result.layer, [this](std::string_view path) {
                return assets_.read(path).bytes;
            });
        const oa::base::sha256::Digest folded = mutators::mutator_digest(applied, layer);
        const uint8_t dropped = dropped_position(result);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started
        );
        std::cerr << "open-annihilation: mutators: room built " << resolved.size() << " in "
                  << elapsed.count() << " ms loadout " << hex_digits(wanted, 16) << " layer "
                  << hex_digits(layer, 16) << " mutators " << hex_digits(folded, 16) << " dropped ";
        if (dropped == netgame::presence_mutator_index_none)
            std::cerr << "none";
        else
            std::cerr << static_cast<unsigned>(dropped);
        std::cerr << '\n';
        std::cerr.flush();
        state.mounted = assets_.mutator_layer_mounted();
        state.phase = mp::RoomMutatorPhase::built;
        state.for_digest = wanted;
        state.folded = folded;
        state.have_folded = true;
        state.index = dropped;
        state.built_sim = simulation_hash(*this);
        state.sync_logged = false;
        state.total = static_cast<uint8_t>(
            resolved.size() < netgame::presence_mutators_max ? resolved.size()
                                                             : netgame::presence_mutators_max
        );
        state.held = state.total;
        state.codes.assign(state.total, static_cast<uint8_t>(mp::MutatorHave::has));
        const std::string reloaded = reload_units();
        if (!reloaded.empty()) {
            fail(
                "the unit headers could not be read again: " + reloaded,
                netgame::presence_mutator_index_none
            );
            return true;
        }
        return true;
    } catch (const std::exception& error) {
        fail(error.what(), netgame::presence_mutator_index_none);
        return true;
    } catch (...) {
        fail("the mutators did not apply", netgame::presence_mutator_index_none);
        return true;
    }
}

bool Runtime::resolve_guest_loadout(
    const netgame::PresenceLoadoutAssembly& assembly,
    std::vector<mutators::ResolvedMutator>& resolved,
    std::vector<uint8_t>& codes,
    oa::base::sha256::Digest& wanted
) {
    const std::filesystem::path folder =
        user_folder_ / std::string(oa::data::mutator_library::mutators_folder_name);
    resolved.clear();
    codes.assign(assembly.mutators.size(), static_cast<uint8_t>(mp::MutatorHave::has));
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
            codes[at] = static_cast<uint8_t>(mp::MutatorHave::missing);
            all_has = false;
            continue;
        }
        const bool version_differs = oamut::version_text(found->manifest.version) != item.version;
        const bool prefix_differs =
            std::memcmp(
                found->package.data(), item.sha256_prefix.data(), item.sha256_prefix.size()
            ) != 0;
        if (version_differs || prefix_differs) {
            codes[at] = static_cast<uint8_t>(mp::MutatorHave::other_release);
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
        codes[0] = static_cast<uint8_t>(mp::MutatorHave::other_release);
    return false;
}

void Runtime::publish_current(bool include_items) {
    RoomMutatorState& state = ensure_room_mutators();
    if (state.phase == mp::RoomMutatorPhase::none) {
        if (!state.send)
            return;
        state.send = false;
        state.outgoing = {};
        ++state.outgoing_generation;
        return;
    }
    netgame::PresenceMutators field;
    if (!mp::lobby_room_mutator_field(
            state.phase,
            state.for_digest,
            state.folded,
            state.index,
            state.total,
            state.held,
            state.codes,
            &field
        )) {
        if (!state.send)
            return;
        state.send = false;
        state.outgoing = {};
        ++state.outgoing_generation;
        return;
    }
    if (include_items)
        fill_host_items(field, state.chosen);
    if (state.send && state.outgoing == field)
        return;
    state.outgoing = std::move(field);
    state.send = true;
    ++state.outgoing_generation;
}

void Runtime::greet_room_peers() {
    if (!room_hosting())
        return;
    RoomMutatorState& state = ensure_room_mutators();
    mp::Lobby& lobby = mp::multiplayer_lobby();
    if (lobby.game == nullptr)
        return;
    std::vector<uint32_t> live;
    live.reserve(static_cast<std::size_t>(mp::kSlotCount));
    for (int32_t slot = 0; slot < mp::kSlotCount; ++slot) {
        if (!mp::lobby_remote_oa_player(lobby, slot))
            continue;
        const Player& player = mp::slot_player(lobby, slot);
        if (player.player_id == 0)
            continue;
        live.push_back(player.player_id);
        GreetMark* mark = nullptr;
        for (GreetMark& one : state.greets) {
            if (one.player_id == player.player_id) {
                mark = &one;
                break;
            }
        }
        if (mark == nullptr) {
            state.greets.push_back(GreetMark{});
            mark = &state.greets.back();
            mark->player_id = player.player_id;
        }
        const mp::PresenceRecords& records = lobby.presence_records;
        std::optional<netgame::PresenceMutatorsState> row;
        if (records.peer_size[slot] != 0 && records.peer_id[slot] == player.player_id)
            row = netgame::presence_mutators_state(records.peer[slot], records.peer_size[slot]);
        if (row) {
            mark->saw = true;
            mark->greeted_none = false;
            if (state.phase == mp::RoomMutatorPhase::built && row->built &&
                row->digest == state.for_digest) {
                if (!mark->greeted_built || mark->built_for != state.for_digest) {
                    mp::multiplayer_regreet(player.player_id);
                    mark->greeted_built = true;
                    mark->built_for = state.for_digest;
                }
            } else {
                mark->greeted_built = false;
            }
        } else {
            mark->greeted_built = false;
            if (state.phase == mp::RoomMutatorPhase::none && mark->saw && !mark->greeted_none) {
                mp::multiplayer_regreet(player.player_id);
                mark->greeted_none = true;
            }
        }
    }
    std::vector<GreetMark> still;
    still.reserve(live.size());
    for (const GreetMark& one : state.greets) {
        for (const uint32_t id : live) {
            if (id != one.player_id)
                continue;
            still.push_back(one);
            break;
        }
    }
    state.greets = std::move(still);
}

void Runtime::remember_wanted() {
    RoomMutatorState& state = ensure_room_mutators();
    const std::string sim = simulation_hash(*this);
    if (state.wanted_set && state.wanted_stamp == state.chosen_stamp && state.wanted_sim == sim)
        return;
    state.wanted = mutators::loadout_digest(mutators::loadout_of(state.chosen));
    state.wanted_sim = std::move(sim);
    state.wanted_stamp = state.chosen_stamp;
    state.wanted_set = true;
}

void Runtime::refresh_room_assembly() {
    RoomMutatorState& state = ensure_room_mutators();
    const uint32_t generation = mp::multiplayer_room_mutators_generation();
    if (state.assembly_known && state.seen_generation == generation)
        return;
    state.assembly_known = true;
    state.seen_generation = generation;
    netgame::PresenceLoadoutAssembly fresh;
    if (mp::multiplayer_room_mutators(&fresh))
        state.assembly = std::move(fresh);
    else
        state.assembly = {};
}

void Runtime::note_unit_sync() {
    RoomMutatorState& state = ensure_room_mutators();
    if (state.sync_logged || state.phase != mp::RoomMutatorPhase::built || !room_hosting())
        return;
    mp::Lobby& lobby = mp::multiplayer_lobby();
    if (lobby.sync.peer_count <= 0 || !mp::unit_sync_complete(lobby))
        return;
    char detail[96];
    const char* diagnostic = mp::unit_sync_diagnostic(lobby, detail, sizeof detail);
    if (diagnostic == nullptr || std::string_view(diagnostic) != "OK")
        return;
    state.sync_logged = true;
    std::cerr << "multiplayer: unit sync OK\n";
    std::cerr.flush();
}

void Runtime::follow_room_mutators() {
    if (match_running())
        return;
    if (!mp::multiplayer_showing()) {
        RoomMutatorState* held = room_mutator_state_.get();
        if (held == nullptr)
            return;
        const bool reload = held->mounted || held->phase == mp::RoomMutatorPhase::built;
        held->seeded = false;
        held->chosen.clear();
        held->resolved.clear();
        ++held->chosen_stamp;
        release_room_mutators();
        if (!reload || held->mounted)
            return;
        const std::string error = reload_units();
        if (error.empty())
            return;
        std::cerr << "open-annihilation: mutators: room failed: the unit headers could not be "
                     "read again: "
                  << one_line(error) << '\n';
        std::cerr.flush();
        return;
    }

    RoomMutatorState& state = ensure_room_mutators();
    if (!room_hosting()) {
        state.seeded = false;
        if (!state.chosen.empty()) {
            state.chosen.clear();
            ++state.chosen_stamp;
        }
        refresh_room_assembly();
        if (state.assembly.total == 0) {
            release_empty_room();
            return;
        }
        if (state.assembly.total > netgame::presence_mutators_max ||
            state.assembly.mutators.size() > state.assembly.total) {
            if (state.phase != mp::RoomMutatorPhase::failed ||
                state.for_digest != state.assembly.digest ||
                state.index != netgame::presence_mutator_index_none) {
                std::cerr << "open-annihilation: mutators: room failed: the list names more than "
                          << netgame::presence_mutators_max << " mutators\n";
                std::cerr.flush();
                state.phase = mp::RoomMutatorPhase::failed;
                state.for_digest = state.assembly.digest;
                state.index = netgame::presence_mutator_index_none;
                state.total = static_cast<uint8_t>(netgame::presence_mutators_max);
                state.held = state.total;
                state.codes.clear();
                state.folded = {};
                state.have_folded = false;
                state.built_sim.clear();
                state.resolved.clear();
            }
            publish_current(false);
            return;
        }
        if (!netgame::presence_assembly_whole(state.assembly)) {
            state.phase = mp::RoomMutatorPhase::receiving;
            state.for_digest = state.assembly.digest;
            state.index = netgame::presence_mutator_index_none;
            state.total = state.assembly.total;
            state.held = static_cast<uint8_t>(state.assembly.mutators.size());
            state.codes.clear();
            state.folded = {};
            state.have_folded = false;
            publish_current(false);
            return;
        }
        const std::string sim = simulation_hash(*this);
        const bool same = state.for_digest == state.assembly.digest && state.built_sim == sim;
        if (state.phase == mp::RoomMutatorPhase::built && same) {
            publish_current(false);
            return;
        }
        if (state.phase == mp::RoomMutatorPhase::failed && same) {
            drop_room_layer();
            publish_current(false);
            return;
        }
        const std::filesystem::path folder =
            user_folder_ / std::string(oa::data::mutator_library::mutators_folder_name);
        const auto stamp = mutators_stamp(folder, state.assembly);
        const auto now = std::chrono::steady_clock::now();
        const bool cached = state.resolve.valid && state.resolve.digest == state.assembly.digest &&
                            state.resolve.stamp == stamp &&
                            now - state.resolve.at < std::chrono::seconds(1);
        if (!cached) {
            std::cerr << "open-annihilation: mutators: room resolve "
                      << hex_digits(state.assembly.digest, 16) << '\n';
            std::cerr.flush();
            state.resolve.digest = state.assembly.digest;
            state.resolve.stamp = stamp;
            state.resolve.at = now;
            state.resolve.valid = true;
            state.resolve.ok = resolve_guest_loadout(
                state.assembly, state.resolve.list, state.resolve.codes, state.resolve.wanted
            );
        }
        if (!state.resolve.ok) {
            if (state.mounted && state.for_digest != state.assembly.digest) {
                if (!drop_room_layer())
                    return;
                const std::string error = reload_units();
                if (!error.empty()) {
                    std::cerr << "open-annihilation: mutators: room failed: the unit headers could "
                                 "not be read again: "
                              << one_line(error) << '\n';
                    std::cerr.flush();
                }
            }
            state.phase = mp::RoomMutatorPhase::lacking;
            state.for_digest = state.assembly.digest;
            state.index = first_not_has(state.resolve.codes);
            state.total = state.assembly.total;
            state.held = state.assembly.total;
            state.codes = state.resolve.codes;
            state.folded = {};
            state.have_folded = false;
            state.resolved.clear();
            publish_current(false);
            return;
        }
        state.resolved = state.resolve.list;
        if (!(state.phase == mp::RoomMutatorPhase::built &&
              state.for_digest == state.resolve.wanted && state.built_sim == sim)) {
            if (!build_room_layer(state.resolved, state.resolve.wanted))
                return;
        }
        publish_current(false);
        return;
    }

    if (!state.seeded) {
        const auto listed = chosen_match_mutators();
        state.chosen.assign(listed.begin(), listed.end());
        state.seeded = true;
        ++state.chosen_stamp;
    }
    if (state.chosen.empty()) {
        release_empty_room();
        greet_room_peers();
        return;
    }
    remember_wanted();
    if (state.chosen.size() > netgame::presence_mutators_max) {
        if (!state.over_logged || state.phase != mp::RoomMutatorPhase::failed ||
            state.for_digest != state.wanted) {
            state.over_logged = true;
            std::cerr << "open-annihilation: mutators: room failed: the list names more than "
                      << netgame::presence_mutators_max << " mutators\n";
            std::cerr.flush();
            if (state.mounted)
                drop_room_layer();
            state.phase = mp::RoomMutatorPhase::failed;
            state.for_digest = state.wanted;
            state.index = netgame::presence_mutator_index_none;
            state.total = static_cast<uint8_t>(netgame::presence_mutators_max);
            state.held = state.total;
            state.codes.clear();
            state.folded = {};
            state.have_folded = false;
            state.built_sim = state.wanted_sim;
        }
        publish_current(false);
        greet_room_peers();
        return;
    }
    state.over_logged = false;
    const bool same = state.for_digest == state.wanted && state.built_sim == state.wanted_sim;
    if (state.phase == mp::RoomMutatorPhase::built && same) {
        publish_current(true);
        greet_room_peers();
        note_unit_sync();
        return;
    }
    if (state.phase == mp::RoomMutatorPhase::failed && same) {
        drop_room_layer();
        publish_current(true);
        greet_room_peers();
        return;
    }
    if (!build_room_layer(state.chosen, state.wanted))
        return;
    publish_current(true);
    greet_room_peers();
    note_unit_sync();
}

} // namespace oa::app
