// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The battle room's mutator gate, the host's list parts and the unit-sync restart.
#include "oa/netgame/presence.hpp"
#include "oa/netgame/records.hpp"
#include "oa/ui/frontend_multiplayer/lobby.hpp"
#include "oa/ui/frontend_multiplayer/lobby_net.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <malloc.h>
#endif

namespace mp = oa::ui::frontend_multiplayer;
namespace ng = oa::netgame;

namespace {

std::atomic<int> g_allocations{0};
std::atomic<bool> g_count_allocations{false};

void note_allocation() noexcept {
    if (g_count_allocations.load(std::memory_order_relaxed))
        g_allocations.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

void* operator new(std::size_t size) {
    note_allocation();
    if (size == 0)
        size = 1;
    if (void* block = std::malloc(size))
        return block;
    throw std::bad_alloc();
}

void* operator new(std::size_t size, std::align_val_t alignment) {
    note_allocation();
    if (size == 0)
        size = 1;
    const auto align = static_cast<std::size_t>(alignment);
#if defined(_WIN32)
    if (void* block = _aligned_malloc(size, align == 0 ? 1 : align))
        return block;
#else
    void* block = nullptr;
    const auto used = align < sizeof(void*) ? sizeof(void*) : align;
    if (posix_memalign(&block, used, size) == 0)
        return block;
#endif
    throw std::bad_alloc();
}

void operator delete(void* block) noexcept {
    std::free(block);
}

void operator delete(void* block, std::size_t) noexcept {
    std::free(block);
}

void operator delete(void* block, std::align_val_t) noexcept {
#if defined(_WIN32)
    _aligned_free(block);
#else
    std::free(block);
#endif
}

void operator delete(void* block, std::size_t, std::align_val_t) noexcept {
    ::operator delete(block, std::align_val_t{0});
}

void* operator new[](std::size_t size) {
    return ::operator new(size);
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
    return ::operator new(size, alignment);
}

void operator delete[](void* block) noexcept {
    ::operator delete(block);
}

void operator delete[](void* block, std::size_t) noexcept {
    ::operator delete(block);
}

void operator delete[](void* block, std::align_val_t alignment) noexcept {
    ::operator delete(block, alignment);
}

void operator delete[](void* block, std::size_t, std::align_val_t alignment) noexcept {
    ::operator delete(block, alignment);
}

namespace {

int g_failures = 0;

void expect(bool condition, const char* what) {
    if (condition)
        return;
    std::fprintf(stderr, "FAILED: %s\n", what);
    ++g_failures;
}

bool map_selected(void*) {
    return true;
}

const char* map_name(void*) {
    return "Ridge";
}

/// A battle room: the local player in slot 0, another machine in slot 1.
struct Room {
    std::unique_ptr<oa::Game> game = std::make_unique<oa::Game>();
    mp::Lobby lobby{};
    mp::LoopbackNet loopback{};
    mp::Panel panel{};

    /// @param hosting true seats the local player as the host
    explicit Room(bool hosting) {
        mp::loopback_reset(loopback);
        mp::lobby_reset(lobby, *game);
        lobby.net = mp::loopback_lobby_net(loopback);
        lobby.maps.selected = map_selected;
        lobby.maps.name = map_name;
        const uint32_t local_id = hosting ? 0x100u : 0x200u;
        const uint32_t other_id = hosting ? 0x200u : 0x100u;
        game->players[0].player_id = local_id;
        mp::lobby_seat_local(lobby, 0, hosting, hosting ? "Host" : "Guest");
        seat(1, other_id, true, !hosting);
    }

    /// Seats a remote player.
    ///
    /// @param player_slot the slot
    /// @param id the player id
    /// @param oa whether the setup block carries the engine signature
    /// @param host_role whether the block says host
    void seat(int32_t player_slot, uint32_t id, bool oa, bool host_role) {
        auto& player = mp::slot_player(lobby, player_slot);
        player.in_use = 1;
        player.player_id = id;
        player.index = static_cast<uint8_t>(player_slot);
        std::snprintf(player.name, sizeof player.name, "P%u", id);
        mp::slot_set_status(lobby, player, mp::kSlotRemote);
        auto* info = mp::slot_info(lobby, player_slot);
        info->state = mp::kInfoStatePlaying;
        info->net_id = id;
        if (host_role)
            info->role = static_cast<uint8_t>(info->role | mp::kRoleHost);
        if (oa) {
            info->engine_signature[0] = ng::engine_signature_first;
            info->engine_signature[1] = ng::engine_signature_second;
        }
        lobby.presence_records.block_player[player_slot] = id;
    }

    /// Seats a computer player of this machine.
    ///
    /// @param player_slot the slot
    void seat_computer(int32_t player_slot) {
        auto& player = mp::slot_player(lobby, player_slot);
        player.in_use = 1;
        player.player_id = 0x201;
        player.index = static_cast<uint8_t>(player_slot);
        mp::slot_set_status(lobby, player, mp::kSlotComputer);
    }
};

/// A mutator field with the room's digest and layer, and no items yet.
///
/// @param built the built flag
/// @param failed the failed flag
/// @param index the mutator those flags name
/// @param total how many mutators the list names
/// @param held how many the sender holds
/// @return the field
ng::PresenceMutators field_of(bool built, bool failed, uint8_t index, uint8_t total, uint8_t held) {
    ng::PresenceMutators field;
    field.built = built;
    field.failed = failed;
    field.index = index;
    field.total = total;
    field.held = held;
    field.digest.fill(0x11);
    field.layer_digest.fill(0x22);
    return field;
}

/// Encodes a presence record that carries a mutator field and, optionally, a sim hash.
///
/// @param field the mutator field
/// @param sim the sim hash; null omits it
/// @param mark the byte the sim hash is filled with
/// @param capacity the most bytes the record may take
/// @return the record; empty when it does not encode
std::vector<uint8_t> encode_field(
    const ng::PresenceMutators& field, bool sim, uint8_t mark, std::size_t capacity
) {
    ng::PresenceRecord record;
    record.mutators = field;
    if (sim) {
        record.sim_hash = ng::PresenceDigest{};
        record.sim_hash->fill(mark);
    }
    std::vector<uint8_t> bytes(ng::presence_record_max_bytes);
    std::size_t written = 0;
    if (ng::encode_presence(record, bytes.data(), capacity, &written) != ng::WireError::ok)
        return {};
    bytes.resize(written);
    return bytes;
}

/// Stores a record as the one this machine sends.
void put_local(mp::Lobby& lobby, const std::vector<uint8_t>& bytes) {
    std::memcpy(lobby.presence_records.local, bytes.data(), bytes.size());
    lobby.presence_records.local_size = static_cast<uint16_t>(bytes.size());
}

/// Stores a record as the one a remote slot sent.
void put_peer(mp::Lobby& lobby, int32_t player_slot, uint32_t id, const std::vector<uint8_t>& bytes) {
    std::memcpy(lobby.presence_records.peer[player_slot], bytes.data(), bytes.size());
    lobby.presence_records.peer_size[player_slot] = static_cast<uint16_t>(bytes.size());
    lobby.presence_records.peer_id[player_slot] = id;
}

/// The host's built list of two mutators, every position installed.
ng::PresenceMutators host_ready() {
    auto field = field_of(true, false, ng::presence_mutator_index_none, 2, 2);
    return field;
}

/// Installs the host's record on the local player and returns the room.
Room hosted(const ng::PresenceMutators& field, bool sim = false, uint8_t mark = 0x5a) {
    Room room(true);
    put_local(room.lobby, encode_field(field, sim, mark, ng::presence_record_max_bytes));
    return room;
}

void expect_hold(const mp::MutatorsBlock& block, mp::MutatorsHold hold, int32_t slot, int position) {
    expect(block.hold == hold, "the hold");
    expect(block.slot == slot, "the slot the hold names");
    expect(block.position == static_cast<uint8_t>(position), "the position the hold names");
}

void test_no_field_allows_start() {
    Room room(true);
    const auto block = mp::lobby_mutators_block(room.lobby);
    expect_hold(block, mp::MutatorsHold::none, -1, ng::presence_mutator_index_none);
    expect(mp::lobby_mutators_allow_start(room.lobby), "no mutator field allows START");
}

void test_each_hold() {
    {
        auto field = field_of(false, false, ng::presence_mutator_index_none, 1, 1);
        Room room = hosted(field);
        const auto block = mp::lobby_mutators_block(room.lobby);
        expect_hold(block, mp::MutatorsHold::host_not_built, 0, ng::presence_mutator_index_none);
    }
    {
        auto field = field_of(false, true, 1, 2, 2);
        Room room = hosted(field);
        const auto block = mp::lobby_mutators_block(room.lobby);
        expect_hold(block, mp::MutatorsHold::host_failed, 0, 1);
    }
    {
        Room room = hosted(host_ready());
        const auto block = mp::lobby_mutators_block(room.lobby);
        expect_hold(block, mp::MutatorsHold::no_support, 1, ng::presence_mutator_index_none);
    }
    {
        Room room = hosted(host_ready());
        auto peer = host_ready();
        peer.held = 1;
        put_peer(room.lobby, 1, 0x200, encode_field(peer, false, 0, ng::presence_record_max_bytes));
        const auto block = mp::lobby_mutators_block(room.lobby);
        expect_hold(block, mp::MutatorsHold::receiving, 1, ng::presence_mutator_index_none);
    }
    {
        Room room = hosted(host_ready());
        auto peer = host_ready();
        ng::presence_mutator_set_state(peer.states, 1, 1);
        put_peer(room.lobby, 1, 0x200, encode_field(peer, false, 0, ng::presence_record_max_bytes));
        const auto block = mp::lobby_mutators_block(room.lobby);
        expect_hold(block, mp::MutatorsHold::lacking, 1, 1);
    }
    {
        Room room = hosted(host_ready());
        auto peer = host_ready();
        peer.failed = true;
        peer.index = 0;
        put_peer(room.lobby, 1, 0x200, encode_field(peer, false, 0, ng::presence_record_max_bytes));
        const auto block = mp::lobby_mutators_block(room.lobby);
        expect_hold(block, mp::MutatorsHold::failed, 1, 0);
    }
    {
        Room room = hosted(host_ready());
        auto peer = host_ready();
        peer.built = false;
        put_peer(room.lobby, 1, 0x200, encode_field(peer, false, 0, ng::presence_record_max_bytes));
        const auto block = mp::lobby_mutators_block(room.lobby);
        expect_hold(block, mp::MutatorsHold::building, 1, ng::presence_mutator_index_none);
    }
    {
        Room room = hosted(host_ready(), true, 0x5a);
        auto peer = host_ready();
        peer.layer_digest.fill(0x33);
        put_peer(room.lobby, 1, 0x200, encode_field(peer, true, 0x5a, ng::presence_record_max_bytes));
        const auto block = mp::lobby_mutators_block(room.lobby);
        expect_hold(block, mp::MutatorsHold::other_result, 1, ng::presence_mutator_index_none);
    }
    {
        Room room = hosted(host_ready(), true, 0x5a);
        auto peer = host_ready();
        put_peer(room.lobby, 1, 0x200, encode_field(peer, true, 0x6b, ng::presence_record_max_bytes));
        const auto block = mp::lobby_mutators_block(room.lobby);
        expect_hold(block, mp::MutatorsHold::other_sim_hash, 1, ng::presence_mutator_index_none);
    }
    {
        Room room = hosted(host_ready(), true, 0x5a);
        put_peer(
            room.lobby, 1, 0x200, encode_field(host_ready(), true, 0x5a, ng::presence_record_max_bytes)
        );
        const auto block = mp::lobby_mutators_block(room.lobby);
        expect_hold(block, mp::MutatorsHold::none, -1, ng::presence_mutator_index_none);
        expect(mp::lobby_mutators_allow_start(room.lobby), "a matching room allows START");
    }
}

void test_slot_order_and_who_is_skipped() {
    Room room = hosted(host_ready());
    room.seat(3, 0x300, true, false);
    auto lacking = host_ready();
    ng::presence_mutator_set_state(lacking.states, 0, 1);
    auto failed = host_ready();
    failed.failed = true;
    failed.index = 1;
    put_peer(room.lobby, 1, 0x200, encode_field(lacking, false, 0, ng::presence_record_max_bytes));
    put_peer(room.lobby, 3, 0x300, encode_field(failed, false, 0, ng::presence_record_max_bytes));
    expect_hold(mp::lobby_mutators_block(room.lobby), mp::MutatorsHold::lacking, 1, 0);
    put_peer(room.lobby, 1, 0x200, encode_field(failed, false, 0, ng::presence_record_max_bytes));
    put_peer(room.lobby, 3, 0x300, encode_field(lacking, false, 0, ng::presence_record_max_bytes));
    expect_hold(mp::lobby_mutators_block(room.lobby), mp::MutatorsHold::failed, 1, 1);

    // A 3.1c player and this machine's computer never hold START.
    Room plain = hosted(host_ready());
    plain.seat(1, 0x200, false, false);
    plain.seat_computer(2);
    auto missing = host_ready();
    ng::presence_mutator_set_state(missing.states, 0, 1);
    put_peer(plain.lobby, 1, 0x200, encode_field(missing, false, 0, ng::presence_record_max_bytes));
    expect(mp::lobby_mutators_allow_start(plain.lobby), "a 3.1c row does not hold START");
    expect(!mp::lobby_mutators_differ_from_host(plain.lobby, 1), "a 3.1c row has no mutator dot");
    expect(!mp::lobby_mutators_differ_from_host(plain.lobby, 2), "a computer row has no mutator dot");
    expect(!mp::lobby_mutators_differ_from_host(plain.lobby, 0), "the host row has no mutator dot");

    Room dotted = hosted(host_ready());
    dotted.seat(3, 0x300, true, false);
    put_peer(dotted.lobby, 1, 0x200, encode_field(host_ready(), false, 0, ng::presence_record_max_bytes));
    put_peer(dotted.lobby, 3, 0x300, encode_field(missing, false, 0, ng::presence_record_max_bytes));
    expect(mp::lobby_rules_differ_from_host(dotted.lobby, 3), "the dot follows the player who lacks one");
    expect(!mp::lobby_rules_differ_from_host(dotted.lobby, 1), "a player who matches has no dot");
    expect(!mp::lobby_rules_differ_from_host(dotted.lobby, 0), "the host's own row has no dot");
    expect(
        mp::lobby_mutators_differ_from_host(dotted.lobby, 3),
        "the later slot is named on its own"
    );
}

void test_have_codes() {
    Room room = hosted(host_ready());
    auto field = field_of(true, false, ng::presence_mutator_index_none, 4, 4);
    ng::presence_mutator_set_state(field.states, 1, 1);
    ng::presence_mutator_set_state(field.states, 2, 2);
    ng::presence_mutator_set_state(field.states, 3, 3);
    put_peer(room.lobby, 1, 0x200, encode_field(field, false, 0, ng::presence_record_max_bytes));
    expect(mp::lobby_mutator_have(room.lobby, 1, 0) == mp::MutatorHave::has, "has");
    expect(mp::lobby_mutator_have(room.lobby, 1, 1) == mp::MutatorHave::missing, "missing");
    expect(mp::lobby_mutator_have(room.lobby, 1, 2) == mp::MutatorHave::other_release, "other release");
    expect(mp::lobby_mutator_have(room.lobby, 1, 3) == mp::MutatorHave::unknown, "unknown");
    expect(mp::lobby_mutator_have(room.lobby, 1, 4) == mp::MutatorHave::unknown, "past the list");
    expect(mp::lobby_mutator_have(room.lobby, 3, 0) == mp::MutatorHave::unknown, "no record");
    room.seat_computer(2);
    put_local(room.lobby, encode_field(field, false, 0, ng::presence_record_max_bytes));
    expect(
        mp::lobby_mutator_have(room.lobby, 2, 1) == mp::MutatorHave::missing,
        "this machine's computer reads its own record"
    );
}

void test_block_allocates_nothing() {
    Room room = hosted(host_ready());
    put_peer(room.lobby, 1, 0x200, encode_field(host_ready(), true, 0x5a, ng::presence_record_max_bytes));
    g_allocations.store(0);
    g_count_allocations.store(true);
    const auto block = mp::lobby_mutators_block(room.lobby);
    const auto again = mp::lobby_mutators_allow_start(room.lobby);
    g_count_allocations.store(false);
    expect(g_allocations.load() == 0, "the gate allocates nothing");
    expect(block.hold == mp::MutatorsHold::none && again, "the counted calls still answer");
}

/// One wide mutator, 40 bytes on the wire.
ng::PresenceMutator wide_mutator(int number) {
    ng::PresenceMutator item;
    char digits[3] = {};
    std::snprintf(digits, sizeof digits, "%02d", number);
    item.id = std::string("mutator-sample") + digits;
    item.version = "1234567890123";
    item.sha256_prefix.fill(static_cast<uint8_t>(number));
    return item;
}

/// The 32-mutator list that does not fit in the host's 533-byte source.
ng::PresenceMutators wide_loadout() {
    ng::PresenceMutators field;
    field.built = true;
    field.total = 32;
    field.held = 32;
    field.digest.fill(0x44);
    field.layer_digest.fill(0x55);
    field.mutators.reserve(32);
    for (int number = 0; number < 32; ++number)
        field.mutators.push_back(wide_mutator(number));
    return field;
}

/// A guest's report of how much of that list it holds, with no items.
std::vector<uint8_t> held_record(uint8_t held) {
    auto field = wide_loadout();
    field.mutators.clear();
    field.built = false;
    field.held = held;
    return encode_field(field, false, 0, ng::presence_record_max_bytes);
}

/// Binds the wide list as the host's source, cut at 533 bytes, and the full list for parts.
void bind_wide_source(Room& room) {
    const auto full = wide_loadout();
    room.lobby.presence_records.part_source = full;
    auto bytes = encode_field(full, false, 0, 533);
    std::memcpy(room.lobby.presence_records.source, bytes.data(), bytes.size());
    room.lobby.presence_records.source_size = static_cast<uint16_t>(bytes.size());
}

/// The part on the record the host would send, if it appended one.
std::optional<ng::PresenceMutatorPart> sent_part(Room& room) {
    mp::presence_refresh(room.lobby);
    ng::PresenceRecord decoded;
    if (ng::decode_presence(
            room.lobby.presence_records.local, room.lobby.presence_records.local_size, &decoded
        ) != ng::WireError::ok)
        return std::nullopt;
    return decoded.mutator_part;
}

void test_parts() {
    expect(wide_mutator(0).id.size() == 16, "a wide mutator's id is 16 bytes");
    Room room(true);
    bind_wide_source(room);
    expect(!sent_part(room), "a peer with no mutator field gets no part");

    put_peer(room.lobby, 1, 0x200, held_record(32));
    expect(!sent_part(room), "a peer that holds the whole list gets no part");

    put_peer(room.lobby, 1, 0x200, held_record(11));
    room.seat(3, 0x300, true, false);
    put_peer(room.lobby, 3, 0x300, held_record(4));
    const auto earliest = sent_part(room);
    expect(earliest && earliest->first == 4, "the part starts at the smallest held");

    room.lobby.presence_records.peer_size[3] = 0;
    put_peer(room.lobby, 1, 0x200, held_record(11));
    room.lobby.presence_records.pack_size = 150;
    std::memset(room.lobby.presence_records.pack, 0xab, 150);
    const auto beside = sent_part(room);
    expect(
        beside && beside->first == 11 && beside->mutators.size() == 7,
        "eleven already held, then seven, beside a 150-byte map pack"
    );

    // The guest keeps each record the host refreshes. Three of them finish the list.
    Room guest(false);
    const auto take = [&](const Room& host) {
        mp::LobbyEvent event{};
        event.kind = mp::LobbyEventKind::record;
        event.player_id = 0x100;
        event.size = host.lobby.presence_records.local_size;
        std::memcpy(event.data, host.lobby.presence_records.local, event.size);
        expect(mp::lobby_apply_event(guest.lobby, event), "the guest keeps the host's record");
    };
    take(room);
    expect(guest.lobby.presence_records.host_loadout.mutators.size() == 18, "the head and the first part");
    put_peer(room.lobby, 1, 0x200, held_record(18));
    expect(sent_part(room) && sent_part(room)->mutators.size() == 7, "the second part is seven");
    take(room);
    expect(guest.lobby.presence_records.host_loadout.mutators.size() == 25, "twenty-five after the second part");
    put_peer(room.lobby, 1, 0x200, held_record(25));
    expect(sent_part(room) && sent_part(room)->mutators.size() == 7, "the third part is seven");
    take(room);
    expect(
        ng::presence_assembly_whole(guest.lobby.presence_records.host_loadout) &&
            guest.lobby.presence_records.host_loadout.mutators.size() == 32,
        "three refreshes carry the whole 32-mutator list"
    );

    // A full map pack leaves no room until the head is shortened.
    room.lobby.presence_records.pack_size = 488;
    std::memset(room.lobby.presence_records.pack, 0xcd, 488);
    put_peer(room.lobby, 1, 0x200, held_record(11));
    const auto packed = sent_part(room);
    expect(packed && packed->first == 11 && !packed->mutators.empty(), "a full map pack still carries a part");
}

void test_part_refused_once() {
    Room room(true);
    auto field = wide_loadout();
    auto bytes = encode_field(field, false, 0, 533);
    std::memcpy(room.lobby.presence_records.source, bytes.data(), bytes.size());
    room.lobby.presence_records.source_size = static_cast<uint16_t>(bytes.size());
    field.mutators.clear();
    room.lobby.presence_records.part_source = field;
    put_peer(room.lobby, 1, 0x200, held_record(0));
    mp::presence_refresh(room.lobby);
    expect(
        room.lobby.presence_records.part_refused_digest &&
            *room.lobby.presence_records.part_refused_digest == field.digest,
        "a part that cannot be drawn is remembered"
    );
    mp::presence_refresh(room.lobby);
    expect(
        room.lobby.presence_records.part_refused_digest &&
            *room.lobby.presence_records.part_refused_digest == field.digest,
        "the same digest is not logged again"
    );
}

/// Adds a READY0 button the battle room can grey and click.
void add_ready(mp::Panel& panel) {
    panel.count = 2;
    auto& control = panel.controls[1];
    control = {};
    std::snprintf(control.name.data(), control.name.size(), "READY0");
    control.type = mp::ControlType::button;
    control.active = 1;
    control.hot = true;
    control.attributes = mp::kAttributeCheckbox;
    control.value = 1;
}

void test_ready() {
    Room guest(false);
    put_peer(guest.lobby, 1, 0x100, encode_field(host_ready(), false, 0, ng::presence_record_max_bytes));
    auto own = host_ready();
    own.built = false;
    own.mutators.clear();
    put_local(guest.lobby, encode_field(own, false, 0, ng::presence_record_max_bytes));
    mp::local_info(guest.lobby).options =
        static_cast<uint16_t>(mp::local_info(guest.lobby).options | mp::option::ready);
    add_ready(guest.panel);
    mp::lobby_update_status(guest.lobby, guest.panel);
    expect((mp::local_info(guest.lobby).options & mp::option::ready) == 0, "READY is cleared");
    expect(guest.panel.controls[1].grayed, "READY stays greyed");
    expect(guest.panel.controls[1].value == 0, "the button is off");

    guest.panel.controls[1].grayed = false;
    guest.panel.controls[1].value = 1;
    guest.panel.selected = 1;
    (void)mp::lobby_handle_event(guest.lobby, guest.panel);
    expect(guest.panel.controls[1].value == 0, "a click does not turn READY on");
    expect((mp::local_info(guest.lobby).options & mp::option::ready) == 0, "the ready bit stays off");

    Room host(true);
    auto unbuilt = host_ready();
    unbuilt.built = false;
    put_local(host.lobby, encode_field(unbuilt, false, 0, ng::presence_record_max_bytes));
    mp::local_info(host.lobby).options =
        static_cast<uint16_t>(mp::local_info(host.lobby).options | mp::option::ready);
    add_ready(host.panel);
    mp::lobby_update_status(host.lobby, host.panel);
    expect((mp::local_info(host.lobby).options & mp::option::ready) != 0, "the host's READY stays");
    expect(!host.panel.controls[1].grayed, "the host's READY is not greyed");

    Room open(false);
    ng::PresenceRecord bare;
    bare.engine = ng::PresenceEngine{"0.8.0", "macOS", "arm64"};
    std::vector<uint8_t> bytes(ng::presence_record_max_bytes);
    std::size_t written = 0;
    expect(
        ng::encode_presence(bare, bytes.data(), bytes.size(), &written) == ng::WireError::ok,
        "a record with no mutators encodes"
    );
    bytes.resize(written);
    put_peer(open.lobby, 1, 0x100, bytes);
    put_local(open.lobby, encode_field(own, false, 0, ng::presence_record_max_bytes));
    mp::local_info(open.lobby).options =
        static_cast<uint16_t>(mp::local_info(open.lobby).options | mp::option::ready);
    add_ready(open.panel);
    mp::lobby_update_status(open.lobby, open.panel);
    expect(
        (mp::local_info(open.lobby).options & mp::option::ready) != 0,
        "no mutator field leaves READY as it was"
    );
}

/// The units a sync table is seeded from. Their checksums are already known.
mp::LobbyUnit test_units[3] = {
    {},
    {"A", "ARM", 0, 0, 0, 11, "A", 0x1111, 0},
    {"B", "CORE", 0, 0, 0, 22, "B", 0x2222, 0},
};

void use_units(Room& room) {
    room.lobby.units = test_units;
    room.lobby.unit_count = 3;
}

/// Tells whether a greeting was sent to a player.
bool sent_greeting_to(const Room& room, uint32_t id) {
    for (int32_t index = 0; index < room.loopback.sent_count; ++index)
        if (room.loopback.sent[index][0] == 0x1a && room.loopback.sent[index][1] == 0 &&
            room.loopback.sent_to[index] == id)
            return true;
    return false;
}

/// The last handshake the loopback sent, or nothing.
bool last_handshake(const Room& room, uint8_t* subtype, uint32_t* key, uint32_t* value) {
    for (int32_t index = room.loopback.sent_count - 1; index >= 0; --index) {
        if (room.loopback.sent[index][0] != 0x1a)
            continue;
        *subtype = room.loopback.sent[index][1];
        std::memcpy(key, room.loopback.sent[index] + 6, 4);
        std::memcpy(value, room.loopback.sent[index] + 10, 4);
        return true;
    }
    return false;
}

void test_greeting_restarts_the_guest() {
    Room guest(false);
    use_units(guest);
    mp::unit_sync_create(guest.lobby, false);
    guest.lobby.sync.records_handled = 4;
    guest.lobby.sync.next_unit = 2;
    uint8_t greeting[14] = {0x1a, 0};
    mp::unit_sync_receive(guest.lobby, greeting, 1);
    expect(guest.lobby.sync.next_unit == 0, "a greeting starts the report again");
    guest.loopback.sent_count = 0;
    mp::unit_sync_tick(guest.lobby);
    uint8_t subtype = 0xff;
    uint32_t key = 0;
    uint32_t value = 0;
    expect(
        last_handshake(guest, &subtype, &key, &value) && subtype == 1 && value == 2 &&
            guest.lobby.sync.next_unit == 1,
        "the next tick sends the unit count"
    );
    const auto from = guest.loopback.sent_count;
    mp::unit_sync_tick(guest.lobby);
    expect(from < guest.loopback.sent_count, "checksums follow the count");
    if (from < guest.loopback.sent_count) {
        std::memcpy(&key, guest.loopback.sent[from] + 6, 4);
        std::memcpy(&value, guest.loopback.sent[from] + 10, 4);
        expect(
            guest.loopback.sent[from][1] == 2 && key == 11 && value == 0x1111,
            "the first checksum is the first unit"
        );
    }
}

/// Feeds one peer's count, checksums and acknowledgement.
void report_units(Room& room, int32_t player_slot, uint32_t id) {
    uint8_t count[14] = {0x1a, 1};
    const uint32_t expected = static_cast<uint32_t>(room.lobby.unit_count - 1);
    std::memcpy(count + 10, &expected, 4);
    mp::unit_sync_receive(room.lobby, count, static_cast<uint8_t>(player_slot));
    for (int32_t type = 1; type < room.lobby.unit_count; ++type) {
        uint8_t checksum[14] = {0x1a, 2};
        const auto unit_key = room.lobby.units[type].fbi_hash;
        const auto sum = room.lobby.units[type].content_checksum;
        std::memcpy(checksum + 6, &unit_key, 4);
        std::memcpy(checksum + 10, &sum, 4);
        mp::unit_sync_receive(room.lobby, checksum, static_cast<uint8_t>(player_slot));
    }
    const mp::UnitSyncPeer* peer = nullptr;
    for (int32_t index = 0; index < room.lobby.sync.peer_count; ++index)
        if (room.lobby.sync.peers[index].player_id == id)
            peer = &room.lobby.sync.peers[index];
    uint8_t ack[14] = {0x1a, 4};
    const uint32_t sent = peer != nullptr ? peer->sent : 0;
    std::memcpy(ack + 10, &sent, 4);
    mp::unit_sync_receive(room.lobby, ack, static_cast<uint8_t>(player_slot));
}

bool both_complete(Room& room) {
    return mp::unit_sync_peer_complete(room.lobby, 0x200) &&
           mp::unit_sync_peer_complete(room.lobby, 0x300);
}

void test_rebuild_keeps_a_finished_legacy_peer() {
    Room host(true);
    host.seat(1, 0x200, false, false);
    host.seat(3, 0x300, true, false);
    use_units(host);
    mp::unit_sync_create(host.lobby, true);
    mp::unit_sync_tick(host.lobby);
    report_units(host, 1, 0x200);
    report_units(host, 3, 0x300);
    expect(both_complete(host), "both peers finish before the list changes");

    mp::unit_sync_rebuild(host.lobby);
    expect(
        host.lobby.sync.peer_count == 1 && host.lobby.sync.peers[0].player_id == 0x200 &&
            mp::unit_sync_peer_complete(host.lobby, 0x200),
        "a finished 3.1c peer stays complete"
    );
    expect(!mp::unit_sync_peer_complete(host.lobby, 0x300), "the OA peer is dropped");
    bool remote_kept = host.lobby.sync.record_count == 2;
    for (int32_t index = 0; index < host.lobby.sync.record_count; ++index)
        remote_kept = remote_kept && host.lobby.sync.records[index].remote == 1;
    expect(remote_kept, "every unit the table kept still counts the 3.1c peer");

    host.loopback.sent_count = 0;
    mp::unit_sync_tick(host.lobby);
    expect(sent_greeting_to(host, 0x300), "the OA peer is greeted again");
    report_units(host, 3, 0x300);
    expect(both_complete(host), "both peers finish with the new units");

    mp::unit_sync_regreet(host.lobby, 0x300);
    expect(!mp::unit_sync_peer_complete(host.lobby, 0x300), "regreet drops that peer");
    host.loopback.sent_count = 0;
    mp::unit_sync_tick(host.lobby);
    expect(sent_greeting_to(host, 0x300), "the next tick greets the dropped peer");
    report_units(host, 3, 0x300);
    expect(both_complete(host), "the greeted peer finishes again");

    // Mutators off: the same rebuild, and the 3.1c peer still does not report again.
    mp::unit_sync_rebuild(host.lobby);
    expect(
        mp::unit_sync_peer_complete(host.lobby, 0x200) &&
            !mp::unit_sync_peer_complete(host.lobby, 0x300),
        "turning mutators off keeps the 3.1c peer and drops the OA peer"
    );
    mp::unit_sync_tick(host.lobby);
    report_units(host, 3, 0x300);
    expect(both_complete(host), "after mutators on and off every peer is complete");
}

} // namespace

int main() {
    test_no_field_allows_start();
    test_each_hold();
    test_slot_order_and_who_is_skipped();
    test_have_codes();
    test_block_allocates_nothing();
    test_parts();
    test_part_refused_once();
    test_ready();
    test_greeting_restarts_the_guest();
    test_rebuild_keeps_a_finished_legacy_peer();
    if (g_failures != 0)
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
