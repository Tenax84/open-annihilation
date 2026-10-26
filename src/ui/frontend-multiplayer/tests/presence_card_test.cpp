// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The presence card over hand-made battle rooms: its words for the design's
// sample player (Kirby, on Ridge 4.8 rev 3 under a host on rev 4), for a
// development build, for an OA player whose record has not come and for the
// host's own card with a map pack in each state, and no card for a player on
// 3.1c; the kit's hover card that shows it, in a synthetic font; its place
// by a badge near each edge of the window at Compact, Regular and Large; and
// when it opens and closes.
//
// The synthetic font's glyph for byte b is 2 + b % 4 columns wide, so the
// widths the cutting tests read are sums of those columns.

#include "oa/data/languages.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/formats/fnt.hpp"
#include "oa/netgame/presence.hpp"
#include "oa/netgame/records.hpp"
#include "oa/test/check.hpp"
#include "oa/ui/frontend_multiplayer/lobby.hpp"
#include "oa/ui/frontend_multiplayer/lobby_net.hpp"
#include "oa/ui/frontend_multiplayer/presence_card.hpp"
#include "oa/ui/frontend_renderer.hpp"
#include "oa/ui/kit/controls.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/kit/theme.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace mp = oa::ui::frontend_multiplayer;
namespace ng = oa::netgame;
namespace kit = oa::ui::kit;
namespace renderer = oa::ui::frontend_renderer;

constexpr uint32_t kHostId = 0x100;
constexpr uint32_t kGuestId = 0x200;
constexpr uint32_t kThirdId = 0x300;

uint32_t clock_ticks = 1000;

uint32_t tick(void*) {
    return clock_ticks;
}

/// The words a test translation gives, by their English text.
std::map<std::string, std::string, std::less<>> translations;

/// Translates through `translations`, as LobbyServices::translate does.
///
/// @param text the English text
/// @return its translation; null for a text it does not know
const char* translate(void*, const char* text) {
    const auto found = translations.find(std::string_view(text));
    return found != translations.end() ? found->second.c_str() : nullptr;
}

/// A battle room with no layout: the local player in slot 0 and the
/// players of other machines seated after it.
struct Room {
    std::unique_ptr<oa::Game> game = std::make_unique<oa::Game>();
    std::unique_ptr<mp::Lobby> lobby = std::make_unique<mp::Lobby>();
    std::unique_ptr<mp::LoopbackNet> loopback = std::make_unique<mp::LoopbackNet>();

    /// @param hosting true seats the local player as the host
    /// @param local_name the local player's name
    Room(bool hosting, const char* local_name) {
        mp::loopback_reset(*loopback);
        mp::lobby_reset(*lobby, *game);
        lobby->net = mp::loopback_lobby_net(*loopback);
        lobby->services.tick = tick;
        lobby->local_version_major = 3;
        game->session_flags |= 1;
        loopback->opened = true;
        game->players[0].player_id = hosting ? kHostId : kGuestId;
        mp::lobby_seat_local(*lobby, 0, hosting, local_name);
    }

    /// Seats a player of another machine and applies its setup block,
    /// a human's, with the 'OA' signature or without, and the bytes after
    /// the signature zero, as every block carries them.
    ///
    /// @param id the player
    /// @param name the player's name
    /// @param open_annihilation the block carries the 'OA' signature
    /// @param host the player hosts
    /// @return the player's slot
    int32_t seat(uint32_t id, const char* name, bool open_annihilation, bool host = false) {
        mp::LobbyEvent joined{};
        joined.kind = mp::LobbyEventKind::player_joined;
        joined.player_id = id;
        std::snprintf(joined.name, sizeof joined.name, "%s", name);
        (void)mp::lobby_apply_event(*lobby, joined);
        ng::PlayerInfoRecord block{};
        block.player_id = id;
        const auto set = [&block](std::size_t offset, uint8_t value) {
            if (offset < sizeof block.info_head)
                block.info_head[offset] = value;
            else if (offset >= ng::player_info_tail_offset)
                block.info_tail[offset - ng::player_info_tail_offset] = value;
        };
        set(offsetof(mp::PlayerSetupInfo, state), mp::kInfoStatePlaying);
        if (host)
            set(offsetof(mp::PlayerSetupInfo, role), mp::kRoleHost);
        if (open_annihilation) {
            set(ng::player_info_engine_signature_offset, ng::engine_signature_first);
            set(ng::player_info_engine_signature_offset + 1, ng::engine_signature_second);
        }
        mp::LobbyEvent event{};
        event.kind = mp::LobbyEventKind::record;
        event.player_id = id;
        std::size_t written = 0;
        (void)ng::encode_record(block, event.data, sizeof event.data, &written);
        event.size = static_cast<uint16_t>(written);
        OA_CHECK(mp::lobby_apply_event(*lobby, event));
        const int32_t slot = mp::slot_for_player_id(*lobby, id);
        OA_CHECK(slot > 0);
        // The block can name the host; a test also sets it on the slot.
        if (host && slot > 0)
            mp::slot_info(*lobby, slot)->role = mp::kRoleHost;
        return slot;
    }

    /// Keeps a presence record a player of another machine sent.
    ///
    /// @param id the player
    /// @param record the record's bytes
    void receive(uint32_t id, const std::vector<uint8_t>& record) {
        mp::LobbyEvent event{};
        event.kind = mp::LobbyEventKind::record;
        event.player_id = id;
        std::copy(record.begin(), record.end(), event.data);
        event.size = static_cast<uint16_t>(record.size());
        OA_CHECK(mp::lobby_apply_event(*lobby, event));
    }

    /// Makes a record this machine's own, as the screens' binding does.
    ///
    /// @param record the record's bytes
    void own(const std::vector<uint8_t>& record) {
        auto& records = lobby->presence_records;
        std::copy(record.begin(), record.end(), records.source);
        records.source_size = static_cast<uint16_t>(record.size());
        mp::presence_refresh(*lobby);
    }
};

/// Encodes a presence record.
///
/// @param record the fields
/// @return the bytes
std::vector<uint8_t> encoded(const ng::PresenceRecord& record) {
    std::vector<uint8_t> bytes(ng::presence_record_max_bytes);
    std::size_t written = 0;
    OA_CHECK(
        ng::encode_presence(record, bytes.data(), bytes.size(), &written) == ng::WireError::ok
    );
    bytes.resize(written);
    return bytes;
}

/// Returns a sim hash of 32 equal bytes.
///
/// @param value each byte
/// @return the hash
ng::PresenceDigest filled_hash(uint8_t value) {
    ng::PresenceDigest hash{};
    hash.fill(value);
    return hash;
}

/// The game hacks of the sample: eight, three of them listed first.
const std::vector<std::string> sample_game_hacks{
    "economy.deterministic-wind",
    "ai.income-multipliers",
    "air.gunships-hover-to-strafe",
    "sea.subs-surface-to-fire",
    "economy.storage-caps",
    "combat.dgun-cost",
    "ai.attack-wave-size",
    "air.stall-speed"
};

/// Returns the design's sample record: OA 0.8.0 on Windows x64, Ridge 4.8
/// at packaging revision 3, eight game hacks, four view hacks, Developer
/// Mode off.
///
/// @param version the OA version text
/// @return the record
ng::PresenceRecord sample_record(const char* version = "0.8.0") {
    ng::PresenceRecord record;
    record.engine = ng::PresenceEngine{version, "Windows", "x64"};
    record.mod = ng::PresenceMod{"ridge", "Ridge", "4.8", 3};
    record.sim_hash = filled_hash(0x77);
    record.game_hacks = ng::PresenceHacks{8, sample_game_hacks};
    record.view_hacks = ng::PresenceHacks{4, {"ui.megamap", "ui.health-bars", "ui.whiteboard"}};
    record.developer = ng::PresenceDeveloper{false, 0, {}};
    return record;
}

/// Returns the sample host's record: Ridge 4.8 at packaging revision 4.
///
/// @return the record
ng::PresenceRecord host_record() {
    ng::PresenceRecord record;
    record.engine = ng::PresenceEngine{"0.8.0", "macOS", "arm64"};
    record.mod = ng::PresenceMod{"ridge", "Ridge", "4.8", 4};
    record.sim_hash = filled_hash(0x5a);
    record.game_hacks = ng::PresenceHacks{0, {}};
    record.view_hacks = ng::PresenceHacks{0, {}};
    record.developer = ng::PresenceDeveloper{false, 0, {}};
    return record;
}

/// Tells whether a card's line is as expected.
///
/// @param card the card
/// @param index the line's place, from 0
/// @param label its label
/// @param value its value
/// @param tone its colour
/// @return true when it is
bool line_is(
    const mp::PresenceCard& card,
    std::size_t index,
    std::string_view label,
    std::string_view value,
    mp::CardTone tone = mp::CardTone::plain
) {
    if (index >= card.lines.size()) {
        std::fprintf(stderr, "  no line %zu\n", index);
        return false;
    }
    const auto& line = card.lines[index];
    if (line.label == label && line.value == value && line.tone == tone)
        return true;
    std::fprintf(
        stderr, "  line %zu is \"%s: %s\"\n", index, line.label.c_str(), line.value.c_str()
    );
    return false;
}

/// Answers every map pack with one state.
///
/// @param state the state
/// @return the answer
std::function<mp::PackState(const ng::PresenceMapPack&)> answering(mp::PackState state) {
    return [state](const ng::PresenceMapPack&) { return state; };
}

/// The sample room: the local player hosts on Ridge rev 4, and Kirby, on
/// another machine, plays Ridge rev 3 by other rules.
struct SampleRoom {
    Room room{true, "Steve"};
    int32_t kirby{};

    SampleRoom() {
        room.own(encoded(host_record()));
        kirby = room.seat(kGuestId, "Kirby", true);
        room.receive(kGuestId, encoded(sample_record()));
    }
};

/// A record that names bundles, with the sample's engine line and one sim hash.
///
/// @param hash the sim hash
/// @param expansions the expansion ids
/// @param base the base game
/// @return the record
ng::PresenceRecord bundled_record(
    const ng::PresenceDigest& hash, std::vector<std::string> expansions, const char* base = "totala"
) {
    ng::PresenceRecord record;
    record.engine = ng::PresenceEngine{"0.8.0", "Windows", "x64"};
    record.sim_hash = hash;
    record.bundles = ng::PresenceBundles{base, std::move(expansions), false};
    record.language = "English";
    return record;
}

// Bundles add Base game, Expansions, No Mod and Language, and name one
// missing expansion. The Rules line stays the sim-hash comparison. A record
// without bundles keeps Mod at line 2 and Rules at line 3.
void the_card_names_the_content() {
    const auto hash = filled_hash(0x44);
    Room room(true, "Steve");
    const int32_t kirby = room.seat(kGuestId, "Kirby", true);
    auto host = bundled_record(
        hash,
        {"campaign", "core-contingency", "battle-tactics", "cavedog-units", "cavedog-tilesets"}
    );
    host.engine = ng::PresenceEngine{"0.8.0", "macOS", "arm64"};
    room.own(encoded(host));
    room.receive(
        kGuestId,
        encoded(bundled_record(
            hash, {"campaign", "core-contingency", "cavedog-units", "cavedog-tilesets", "music"}
        ))
    );
    const auto card = mp::presence_card(*room.lobby, kirby, {});
    OA_CHECK(card.has_value());
    if (!card)
        return;
    OA_CHECK(line_is(*card, 2, "Base game", "Total Annihilation 3.1c"));
    OA_CHECK(line_is(
        *card, 3, "Expansions", "Campaign, Core Contingency, Cavedog units, Cavedog tilesets"
    ));
    OA_CHECK(line_is(*card, 4, "Mod", "No Mod"));
    OA_CHECK(line_is(*card, 5, "Language", "English"));
    OA_CHECK(line_is(*card, 6, "Rules", "Same as the host's"));
    OA_CHECK(mp::lobby_rules_differ_from_host(*room.lobby, kirby));
    OA_CHECK(!mp::lobby_sim_hash_differs_from_host(*room.lobby, kirby));
    OA_CHECK(
        line_is(*card, 7, "", "Different from the host: no Battle Tactics.", mp::CardTone::amber)
    );
    OA_CHECK(line_is(
        *card,
        8,
        "",
        "The game cannot start until Kirby turns it on, or the host turns it off.",
        mp::CardTone::amber
    ));

    const auto own = mp::presence_card(*room.lobby, 0, {});
    OA_CHECK(own.has_value());
    if (own) {
        OA_CHECK(line_is(
            *own,
            3,
            "Expansions",
            "Campaign, Core Contingency, Battle Tactics, Cavedog units, Cavedog tilesets"
        ));
        OA_CHECK(own->lines.size() == 7);
    }
}

// The design's sample: every line of Kirby's card, the amber Rules line
// naming the host's revision, and the hacks.
void the_sample_card_says_what_the_record_says() {
    SampleRoom sample;
    const auto card = mp::presence_card(*sample.room.lobby, sample.kirby, {});
    OA_CHECK(card.has_value());
    if (!card)
        return;
    OA_CHECK(card->title == "KIRBY");
    OA_CHECK(card->lines.size() == 7);
    OA_CHECK(line_is(*card, 0, "Engine", "Open Annihilation 0.8.0"));
    OA_CHECK(line_is(*card, 1, "Platform", "Windows · x64"));
    OA_CHECK(line_is(*card, 2, "Mod", "Ridge 4.8 · rev 3"));
    OA_CHECK(line_is(*card, 3, "Rules", "Differ from the host's (rev 4)", mp::CardTone::amber));
    OA_CHECK(line_is(*card, 4, "Game", "8 hacks change the game"));
    OA_CHECK(line_is(*card, 5, "View", "4 view-only hacks"));
    OA_CHECK(line_is(*card, 6, "Dev mode", "Off"));
    OA_CHECK(
        card->hacks ==
        std::vector<std::string>(sample_game_hacks.begin(), sample_game_hacks.begin() + 3)
    );
    OA_CHECK(card->hacks_more == "· 5 more");

    // The host's own card: "The host's", and its own mod.
    const auto host = mp::presence_card(*sample.room.lobby, 0, {});
    OA_CHECK(host.has_value());
    if (host) {
        OA_CHECK(host->title == "STEVE");
        OA_CHECK(line_is(*host, 1, "Platform", "macOS · arm64"));
        OA_CHECK(line_is(*host, 2, "Mod", "Ridge 4.8 · rev 4"));
        OA_CHECK(line_is(*host, 3, "Rules", "The host's"));
        OA_CHECK(line_is(*host, 4, "Game", "No hacks change the game"));
        OA_CHECK(line_is(*host, 5, "View", "No view-only hacks"));
        OA_CHECK(host->hacks.empty() && host->hacks_more.empty());
    }
}

// The Rules line from the records' sim hashes: the same hash, no hash, a
// different hash on another mod, and a 3.1c host playing the plain rules.
void the_rules_line_compares_with_the_host() {
    SampleRoom sample;
    auto record = sample_record();
    record.sim_hash = host_record().sim_hash;
    sample.room.receive(kGuestId, encoded(record));
    auto card = mp::presence_card(*sample.room.lobby, sample.kirby, {});
    OA_CHECK(card && line_is(*card, 3, "Rules", "Same as the host's"));

    record.sim_hash.reset();
    sample.room.receive(kGuestId, encoded(record));
    card = mp::presence_card(*sample.room.lobby, sample.kirby, {});
    OA_CHECK(card && line_is(*card, 3, "Rules", "Unknown"));

    record = sample_record();
    record.mod = ng::PresenceMod{"stone", "Stone", "", 3};
    sample.room.receive(kGuestId, encoded(record));
    card = mp::presence_card(*sample.room.lobby, sample.kirby, {});
    OA_CHECK(card && line_is(*card, 2, "Mod", "Stone · rev 3"));
    OA_CHECK(card && line_is(*card, 3, "Rules", "Differ from the host's", mp::CardTone::amber));

    record.mod.reset();
    record.developer = ng::PresenceDeveloper{true, 2, {{"ai.attack-wave-size", true}}};
    record.game_hacks = ng::PresenceHacks{1, {"ai.attack-wave-size"}};
    record.view_hacks = ng::PresenceHacks{1, {"ui.megamap"}};
    sample.room.receive(kGuestId, encoded(record));
    card = mp::presence_card(*sample.room.lobby, sample.kirby, {});
    OA_CHECK(card && line_is(*card, 2, "Mod", "Total Annihilation 3.1c"));
    OA_CHECK(card && line_is(*card, 4, "Game", "1 hack changes the game"));
    OA_CHECK(card && line_is(*card, 5, "View", "1 view-only hack"));
    OA_CHECK(card && line_is(*card, 6, "Dev mode", "On · 2 overrides"));
    OA_CHECK(card && card->hacks.size() == 1 && card->hacks_more.empty());

    // Under a 3.1c host, which sends no record, the plain baseline's rules.
    Room joined(false, "Kirby");
    const int32_t host = joined.seat(kHostId, "OldSchool", false, true);
    OA_CHECK(mp::lobby_host_slot(*joined.lobby) == host);
    OA_CHECK(!mp::presence_card(*joined.lobby, host, {}).has_value());
    const auto& plain = joined.lobby->presence_records.plain_sim_hash;
    if (plain) {
        record = sample_record();
        record.sim_hash = *plain;
        joined.own(encoded(record));
        card = mp::presence_card(*joined.lobby, 0, {});
        OA_CHECK(card && line_is(*card, 3, "Rules", "Same as the host's"));
        record.sim_hash = filled_hash(0x13);
        joined.own(encoded(record));
        card = mp::presence_card(*joined.lobby, 0, {});
        OA_CHECK(card && line_is(*card, 3, "Rules", "Differ from the host's", mp::CardTone::amber));
    }
}

// A development build's version: its label becomes "(development build)".
void a_development_build_says_so() {
    SampleRoom sample;
    sample.room.receive(kGuestId, encoded(sample_record("0.8.0-dev")));
    const auto card = mp::presence_card(*sample.room.lobby, sample.kirby, {});
    OA_CHECK(card && line_is(*card, 0, "Engine", "Open Annihilation 0.8.0 (development build)"));
}

// An OA player whose record has not come, an OA build before 0.8: its
// block carries 'OA' and the five bytes after it zero, and the card has the
// title and one line. A record that comes later replaces it.
void a_player_without_a_record_gets_one_line() {
    Room room(true, "Steve");
    const int32_t slot = room.seat(kThirdId, "Mira", true);
    const auto* block = reinterpret_cast<const uint8_t*>(mp::slot_info(*room.lobby, slot));
    OA_CHECK(block != nullptr);
    if (block != nullptr) {
        OA_CHECK(ng::sent_by_open_annihilation(block));
        bool spare_zero = true;
        for (std::size_t offset = 0xaf; offset <= 0xb3; ++offset)
            spare_zero = spare_zero && block[offset] == 0;
        OA_CHECK(spare_zero);
    }
    auto card = mp::presence_card(*room.lobby, slot, {});
    OA_CHECK(card.has_value());
    if (card) {
        OA_CHECK(card->title == "MIRA");
        OA_CHECK(card->lines.size() == 1);
        OA_CHECK(line_is(*card, 0, "Engine", "Open Annihilation (before 0.8)"));
        OA_CHECK(card->hacks.empty() && card->hacks_more.empty());
    }
    room.receive(kThirdId, encoded(sample_record()));
    card = mp::presence_card(*room.lobby, slot, {});
    OA_CHECK(card && card->lines.size() == 7);
}

// Platform sits under Engine. One empty field is left out of the line, and
// both empty leaves the line out, so the next line is Mod.
void the_platform_line_leaves_out_an_empty_field() {
    SampleRoom sample;
    auto record = sample_record();
    record.engine->architecture.clear();
    sample.room.receive(kGuestId, encoded(record));
    auto card = mp::presence_card(*sample.room.lobby, sample.kirby, {});
    OA_CHECK(card && line_is(*card, 1, "Platform", "Windows"));
    record.engine->platform.clear();
    record.engine->architecture = "x64";
    sample.room.receive(kGuestId, encoded(record));
    card = mp::presence_card(*sample.room.lobby, sample.kirby, {});
    OA_CHECK(card && line_is(*card, 1, "Platform", "x64"));
    record.engine->architecture.clear();
    sample.room.receive(kGuestId, encoded(record));
    card = mp::presence_card(*sample.room.lobby, sample.kirby, {});
    OA_CHECK(card && card->lines.size() == 6);
    OA_CHECK(card && line_is(*card, 1, "Mod", "Ridge 4.8 · rev 3"));
}

// The host's own card names its map pack, and says what this machine has
// of it in each state; nobody else's card has a Map line.
void the_hosts_card_names_its_map_pack() {
    Room room(false, "Kirby");
    const int32_t host = room.seat(kHostId, "Steve", true, true);
    OA_CHECK(mp::lobby_host_slot(*room.lobby) == host);
    auto record = host_record();
    record.map_pack =
        ng::PresenceMapPack{"archipelago", 2, {}, "example-registry", "", "Archipelago", "1.2"};
    room.receive(kHostId, encoded(record));
    room.own(encoded(record));

    const struct {
        mp::PackState state;
        const char* value;
        mp::CardTone tone;
    } states[] = {
        {mp::PackState::installed, "Archipelago 1.2 ✓ installed", mp::CardTone::lime},
        {mp::PackState::other_release,
         "Archipelago 1.2 · another release here",
         mp::CardTone::amber},
        {mp::PackState::missing, "Archipelago 1.2 · not installed", mp::CardTone::plain},
        {mp::PackState::unknown, "Archipelago 1.2", mp::CardTone::plain},
    };

    for (const auto& expected : states) {
        const auto card = mp::presence_card(*room.lobby, host, answering(expected.state));
        OA_CHECK(card && line_is(*card, 3, "Rules", "The host's"));
        OA_CHECK(card && line_is(*card, 7, "Map", expected.value, expected.tone));
    }
    const auto unbound = mp::presence_card(*room.lobby, host, {});
    OA_CHECK(unbound && line_is(*unbound, 7, "Map", "Archipelago 1.2"));
    // A pack whose name did not fit is named by its key.
    record.map_pack->name.clear();
    record.map_pack->version.clear();
    room.receive(kHostId, encoded(record));
    const auto keyed = mp::presence_card(*room.lobby, host, answering(mp::PackState::missing));
    OA_CHECK(keyed && line_is(*keyed, 7, "Map", "archipelago · not installed"));
    // The local joiner's own record names the same pack, but it is not the host's.
    const auto own = mp::presence_card(*room.lobby, 0, answering(mp::PackState::installed));
    OA_CHECK(own && own->lines.size() == 7);
}

// No card for a player on 3.1c, an open slot or a slot out of range.
void a_31c_player_has_no_card() {
    Room room(true, "Steve");
    const int32_t old_school = room.seat(kThirdId, "OldSchool", false);
    OA_CHECK(!mp::presence_card(*room.lobby, old_school, {}).has_value());
    OA_CHECK(!mp::presence_card(*room.lobby, 5, {}).has_value());
    OA_CHECK(!mp::presence_card(*room.lobby, -1, {}).has_value());
    OA_CHECK(!mp::presence_card(*room.lobby, mp::kSlotCount, {}).has_value());
    // Not even with a record of its own: only the 'OA' signature gives a badge.
    room.receive(kThirdId, encoded(sample_record()));
    OA_CHECK(!mp::presence_card(*room.lobby, old_school, {}).has_value());
}

// Every label and fixed word goes through the lobby's translation.
void the_words_go_through_the_translation() {
    SampleRoom sample;
    translations = {
        {"Engine", "Moteur"},
        {"Platform", "Plateforme"},
        {"Open Annihilation {version}", "Open Annihilation version {version}"},
        {"{mod} · rev {revision}", "{mod} (révision {revision})"},
        {"Differ from the host's (rev {revision})", "Diffèrent de l'hôte ({revision})"},
        {"{count} hacks change the game", "{count} modifications du jeu"},
        {"Off", "Arrêt"},
        {"· {count} more", "· {count} de plus"},
    };
    sample.room.lobby->services.translate = translate;
    const auto card = mp::presence_card(*sample.room.lobby, sample.kirby, {});
    sample.room.lobby->services.translate = nullptr;
    translations.clear();
    OA_CHECK(card && line_is(*card, 0, "Moteur", "Open Annihilation version 0.8.0"));
    OA_CHECK(card && line_is(*card, 1, "Plateforme", "Windows · x64"));
    OA_CHECK(card && line_is(*card, 2, "Mod", "Ridge 4.8 (révision 3)"));
    OA_CHECK(card && line_is(*card, 3, "Rules", "Diffèrent de l'hôte (4)", mp::CardTone::amber));
    OA_CHECK(card && line_is(*card, 4, "Game", "8 modifications du jeu"));
    OA_CHECK(card && line_is(*card, 6, "Dev mode", "Arrêt"));
    OA_CHECK(card && card->hacks_more == "· 5 de plus");
}

/// The synthetic font, for both roles.
kit::Fonts block_fonts() {
    renderer::TextFont font;
    font.font.nominal_height = 8;
    constexpr uint8_t ink_index = 1;
    for (int code = 0; code < 256; ++code) {
        const auto width = static_cast<uint16_t>(2 + code % 4);
        constexpr uint16_t height = 8;
        const auto count = static_cast<std::size_t>(width) * height;
        std::vector<uint8_t> pixels(count, ink_index);
        std::vector<uint8_t> coverage(count, 1);
        font.font.glyphs[static_cast<std::size_t>(code)] =
            oa::formats::fnt::Glyph{width, height, 0, 0, std::move(pixels), std::move(coverage)};
    }
    font.ink[ink_index] = static_cast<uint16_t>(renderer::blend_opaque);
    return {font, font, {}, {}};
}

const kit::Fonts& fonts() {
    static const kit::Fonts loaded = block_fonts();
    return loaded;
}

// The kit's hover card for the sample: the mark and the title, nothing at
// the header's right, one row a line with the Rules value in the lock
// colour, and the hack ids and "· 5 more" as the block.
void the_look_shows_the_sample() {
    SampleRoom sample;
    const auto card = mp::presence_card(*sample.room.lobby, sample.kirby, {});
    OA_CHECK(card.has_value());
    if (!card)
        return;
    const kit::HoverCardLook look =
        mp::presence_card_look(*card, kit::SizeClass::compact, 640, fonts());
    OA_CHECK(look.mark);
    OA_CHECK(look.title == "KIRBY" && look.aside.empty());
    OA_CHECK(look.width == mp::presence_card_compact_width);
    std::size_t row = 0;
    for (const mp::PresenceCardLine& line : card->lines) {
        OA_CHECK(row < look.rows.size());
        if (row >= look.rows.size())
            break;
        OA_CHECK(look.rows[row].label == line.label);
        if (look.rows[row].value == line.value) {
            ++row;
            continue;
        }
        // A difference line wraps. Its rows, joined by spaces, are the line.
        OA_CHECK(line.label.empty());
        std::string joined = look.rows[row].value;
        std::size_t taken = 1;
        while (joined != line.value && row + taken < look.rows.size() &&
               taken < mp::presence_card_difference_lines) {
            OA_CHECK(look.rows[row + taken].label.empty());
            joined += ' ';
            joined += look.rows[row + taken].value;
            ++taken;
        }
        OA_CHECK(taken > 1 && joined == line.value);
        row += taken;
    }
    OA_CHECK(row == look.rows.size());
    bool rules_are_amber = false;
    for (const kit::HoverCardRow& shown : look.rows) {
        if (shown.label == "Rules")
            rules_are_amber = shown.value_colour == kit::colour::lock;
    }
    OA_CHECK(rules_are_amber);
    OA_CHECK(
        look.block ==
        (std::vector<std::string>{
            sample_game_hacks[0], sample_game_hacks[1], sample_game_hacks[2], "· 5 more"
        })
    );
    OA_CHECK(look.arrow == kit::Side::none);
    // The size is the kit's.
    const kit::Point size = kit::hover_card_size(fonts(), look);
    OA_CHECK(size.x == look.width);
    const int32_t shown_lines =
        static_cast<int32_t>(look.rows.size() + look.block.size()) + (look.mark ? 1 : 0);
    OA_CHECK(
        size.y ==
        shown_lines * kit::compact_metrics.small_line + 2 * kit::compact_metrics.hover_card_padding
    );
    OA_CHECK(size.y <= 480 - 2 * mp::presence_card_edge_gap);

    // Lime for an installed pack.
    mp::PresenceCard lime;
    lime.lines.push_back({"Map", "Archipelago 1.2 ✓ installed", mp::CardTone::lime});
    const auto lit = mp::presence_card_look(lime, kit::SizeClass::compact, 640, fonts());
    OA_CHECK(lit.rows.size() == 1 && lit.rows[0].value_colour == kit::colour::accent);
}

// The width by class, never wider than the window less 16 points.
void the_width_follows_the_class() {
    const mp::PresenceCard card{"KIRBY", {{"Engine", "Open Annihilation 0.8.0", {}}}, {}, {}};
    OA_CHECK(mp::presence_card_look(card, kit::SizeClass::compact, 640, fonts()).width == 240);
    OA_CHECK(mp::presence_card_look(card, kit::SizeClass::regular, 960, fonts()).width == 280);
    OA_CHECK(mp::presence_card_look(card, kit::SizeClass::large, 1280, fonts()).width == 320);
    OA_CHECK(mp::presence_card_look(card, kit::SizeClass::compact, 200, fonts()).width == 184);
    OA_CHECK(mp::presence_card_look(card, kit::SizeClass::large, 300, fonts()).width == 284);
}

// A value or a block line wider than its room is cut with "..." at a
// character, through the kit's measure; one that fits is kept whole.
void a_long_value_is_cut() {
    const std::string long_name(200, 'w');
    mp::PresenceCard card;
    card.lines.push_back({"Mod", long_name + " · rev 3", {}});
    card.lines.push_back({"Dev mode", "Off", {}});
    card.hacks.push_back("economy." + long_name);
    const auto look = mp::presence_card_look(card, kit::SizeClass::compact, 640, fonts());
    const int32_t padding = kit::compact_metrics.hover_card_padding;
    const int32_t label_column =
        kit::text_width(fonts(), kit::FontRole::small, "Dev mode") + padding;
    const int32_t value_room = look.width - 2 * padding - label_column;
    OA_CHECK(look.rows.size() == 2 && look.block.size() == 1);
    if (look.rows.size() != 2 || look.block.size() != 1)
        return;
    const std::string& cut = look.rows[0].value;
    OA_CHECK(cut.size() > 3 && cut.ends_with("..."));
    OA_CHECK(long_name.starts_with(cut.substr(0, cut.size() - 3)));
    OA_CHECK(kit::text_width(fonts(), kit::FontRole::small, cut) <= value_room);
    OA_CHECK(
        cut == kit::cut_to_width(fonts(), kit::FontRole::small, long_name + " · rev 3", value_room)
    );
    OA_CHECK(look.rows[1].value == "Off");
    const std::string& block = look.block[0];
    OA_CHECK(block.ends_with("...") && block.starts_with("economy."));
    OA_CHECK(kit::text_width(fonts(), kit::FontRole::small, block) <= look.width - 2 * padding);
}

/// The rows a difference line shows on a Compact card in a 640-point window.
///
/// @param value the line
/// @return the card's rows
std::vector<kit::HoverCardRow> difference_rows(const std::string& value) {
    mp::PresenceCard card;
    card.title = "VISITOR";
    card.lines.push_back({"", value, mp::CardTone::amber});
    const kit::HoverCardLook look =
        mp::presence_card_look(card, kit::SizeClass::compact, 640, fonts());
    OA_CHECK(kit::hover_card_size(fonts(), look).y <= 480 - 2 * mp::presence_card_edge_gap);
    return look.rows;
}

// A difference line wider than the card wraps, as the kit wraps it, onto at
// most presence_card_difference_lines lines. Those lines are whole. Only the
// last is cut with "..." when the rest does not fit. The card stays inside a
// 640x480 window.
void a_difference_line_wraps() {
    const int32_t room =
        mp::presence_card_compact_width - 2 * kit::compact_metrics.hover_card_padding;
    const auto measure = [](std::string_view text) {
        return kit::text_width(fonts(), kit::FontRole::small, text);
    };
    const auto whole = [&](const std::string& value, std::size_t lines) {
        const std::vector<std::string> wrapped = kit::wrap(value, room, measure, {});
        OA_CHECK(wrapped.size() == lines);
        const std::vector<kit::HoverCardRow> rows = difference_rows(value);
        OA_CHECK(rows.size() == wrapped.size());
        for (std::size_t index = 0; index < rows.size() && index < wrapped.size(); ++index) {
            OA_CHECK(rows[index].label.empty());
            OA_CHECK(rows[index].value == wrapped[index]);
            OA_CHECK(!rows[index].value.ends_with("..."));
            OA_CHECK(measure(rows[index].value) <= room);
            OA_CHECK(rows[index].value_colour == kit::colour::lock);
        }
    };
    whole("Different from the host: no Ridge.", 1);
    whole(
        "The game cannot start until Visitor gets it, or the host turns it off. "
        "The visitor still has to fetch it.",
        2
    );
    whole(
        "The game cannot start until Visitor gets it, or the host turns it off. "
        "The visitor still has to fetch it from a registry that offers the host's release, "
        "or the host turns it off before START.",
        3
    );

    const std::vector<kit::HoverCardRow> cut = difference_rows(
        std::string(80, 'w') + " " + std::string(80, 'm') + " " + std::string(80, 'n')
    );
    OA_CHECK(cut.size() == mp::presence_card_difference_lines);
    for (std::size_t index = 0; index < cut.size(); ++index) {
        OA_CHECK(cut[index].value.ends_with("...") == (index + 1 == cut.size()));
        OA_CHECK(measure(cut[index].value) <= room);
    }
}

/// One window the card is placed in.
struct Window {
    const char* name{};
    int32_t points_width{};  ///< its width in points: its pixels over the scale
    int32_t points_height{}; ///< its height in points
    kit::SizeClass size_class{};
};

// The card's place by a badge near each edge of the window at Compact
// (640x480 at 1x), Regular (1920x1080 at 2x) and Large (2560x1440 at 2x):
// below the badge, above it near the window's foot, always inside the
// window less 8 points, its arrow from the badge's side at its centre and
// reaching the badge.
void the_card_lies_by_its_badge() {
    SampleRoom sample;
    const auto card = mp::presence_card(*sample.room.lobby, sample.kirby, {});
    OA_CHECK(card.has_value());
    if (!card)
        return;
    const int32_t arrow = kit::compact_metrics.hover_card_arrow;
    for (const Window& window : {
             Window{"Compact", 640, 480, kit::SizeClass::compact},
             Window{"Regular", 960, 540, kit::SizeClass::regular},
             Window{"Large", 1280, 720, kit::SizeClass::large},
         }) {
        const kit::Rect safe{0, 0, window.points_width, window.points_height};
        const kit::Rect frame = kit::grown(safe, -mp::presence_card_edge_gap);
        kit::HoverCardLook look =
            mp::presence_card_look(*card, window.size_class, window.points_width, fonts());
        const kit::Point size = kit::hover_card_size(fonts(), look);
        constexpr int32_t side = 18;
        const int32_t right = window.points_width - side - 2;
        const int32_t foot = window.points_height - side - 2;
        const int32_t middle_x = window.points_width / 2;
        const int32_t middle_y = window.points_height / 3;

        const struct {
            kit::Rect badge;
            bool below;
        } badges[] = {
            {{2, 2, side, side}, true},
            {{right, 2, side, side}, true},
            {{2, foot, side, side}, false},
            {{right, foot, side, side}, false},
            {{middle_x, middle_y, side, side}, true},
            {{middle_x, foot, side, side}, false},
        };

        for (const auto& placed : badges) {
            const kit::Rect badge = placed.badge;
            const kit::Rect rect = mp::presence_card_place(badge, size, safe, arrow);
            mp::presence_card_point(look, rect, badge);
            const bool inside = kit::wholly_in(rect, frame);
            if (!inside)
                std::fprintf(
                    stderr,
                    "  %s: the card at (%d, %d) leaves the window\n",
                    window.name,
                    rect.x,
                    rect.y
                );
            OA_CHECK(inside);
            OA_CHECK(rect.width == size.x && rect.height == size.y);
            if (placed.below) {
                OA_CHECK(look.arrow == kit::Side::top);
                OA_CHECK(rect.y - arrow == badge.y + badge.height);
            } else {
                OA_CHECK(look.arrow == kit::Side::bottom);
                OA_CHECK(rect.y + rect.height + arrow == badge.y);
            }
            OA_CHECK(look.arrow_at == badge.x + badge.width / 2 - rect.x);
            // Its left at the badge's, unless the window's edge moves it in.
            OA_CHECK(
                rect.x == std::max(std::min(badge.x, frame.x + frame.width - rect.width), frame.x)
            );
        }
    }
    // Inside the safe area, wherever it lies.
    const kit::Rect safe{40, 20, 560, 440};
    const kit::Rect rect = mp::presence_card_place({44, 24, 18, 18}, {240, 144}, safe, arrow);
    OA_CHECK(kit::wholly_in(rect, kit::grown(safe, -mp::presence_card_edge_gap)));
}

// The pointer resting on a badge: closed at 399 ms, open at 400 ms; it stays
// over the badge and the card and closes once over neither.
void hover_opens_after_the_kits_delay() {
    mp::PresenceCardTrigger trigger;
    OA_CHECK(!trigger.pointer(1, 1000));
    OA_CHECK(!trigger.pointer(1, 1000 + kit::hover_card_delay_ms - 1));
    OA_CHECK(trigger.open_slot() == mp::presence_card_no_slot);
    OA_CHECK(trigger.pointer(1, 1000 + kit::hover_card_delay_ms));
    OA_CHECK(trigger.open_slot() == 1);
    OA_CHECK(!trigger.pointer(1, 2000));
    // Onto the card, its arrow first, then off it.
    trigger.over_card(true);
    OA_CHECK(!trigger.pointer(mp::presence_card_no_slot, 2100));
    OA_CHECK(trigger.open_slot() == 1);
    trigger.over_card(false);
    OA_CHECK(trigger.pointer(mp::presence_card_no_slot, 2200));
    OA_CHECK(trigger.open_slot() == mp::presence_card_no_slot);
    // Straight off the badge closes it too.
    OA_CHECK(!trigger.pointer(1, 3000));
    OA_CHECK(trigger.pointer(1, 3400));
    trigger.over_card(false);
    OA_CHECK(trigger.pointer(mp::presence_card_no_slot, 3500));
    OA_CHECK(trigger.open_slot() == mp::presence_card_no_slot);
    // A pass over a badge shorter than the wait opens nothing.
    OA_CHECK(!trigger.pointer(2, 4000));
    OA_CHECK(!trigger.pointer(mp::presence_card_no_slot, 4300));
    OA_CHECK(!trigger.pointer(2, 4500));
    OA_CHECK(!trigger.pointer(2, 4899));
    OA_CHECK(trigger.pointer(2, 4900));
}

// Moving to another badge closes the card and opens the other's after its
// own wait.
void moving_between_badges_waits_again() {
    mp::PresenceCardTrigger trigger;
    (void)trigger.pointer(1, 0);
    OA_CHECK(trigger.pointer(1, 400));
    OA_CHECK(trigger.pointer(3, 450));
    OA_CHECK(trigger.open_slot() == mp::presence_card_no_slot);
    OA_CHECK(!trigger.pointer(3, 849));
    OA_CHECK(trigger.pointer(3, 850));
    OA_CHECK(trigger.open_slot() == 3);
}

// F1 toggles the focused row's card and follows the focus; Escape closes a
// card and is taken only while one is open. A card closed by a key stays
// closed while the pointer rests on its badge.
void the_keys_open_and_close_the_card() {
    mp::PresenceCardTrigger trigger;
    OA_CHECK(!trigger.key_info(mp::presence_card_no_slot));
    OA_CHECK(!trigger.key_close());
    OA_CHECK(trigger.key_info(2));
    OA_CHECK(trigger.open_slot() == 2);
    // The pointer resting elsewhere leaves a card the key opened.
    OA_CHECK(!trigger.pointer(mp::presence_card_no_slot, 100));
    OA_CHECK(!trigger.pointer(mp::presence_card_no_slot, 900));
    OA_CHECK(trigger.open_slot() == 2);
    OA_CHECK(trigger.key_info(2));
    OA_CHECK(trigger.open_slot() == mp::presence_card_no_slot);
    OA_CHECK(trigger.key_info(4));
    OA_CHECK(trigger.key_info(1));
    OA_CHECK(trigger.open_slot() == 1);
    OA_CHECK(trigger.focus(3));
    OA_CHECK(trigger.open_slot() == 3);
    OA_CHECK(!trigger.focus(3));
    OA_CHECK(trigger.focus(mp::presence_card_no_slot));
    OA_CHECK(trigger.open_slot() == mp::presence_card_no_slot);
    OA_CHECK(trigger.key_info(3));
    OA_CHECK(trigger.key_info(mp::presence_card_no_slot));
    OA_CHECK(trigger.open_slot() == mp::presence_card_no_slot);
    OA_CHECK(trigger.key_info(3));
    OA_CHECK(trigger.key_close());
    OA_CHECK(trigger.open_slot() == mp::presence_card_no_slot);
    OA_CHECK(!trigger.key_close());

    // A hovered card closed by Escape, with the pointer still on its badge.
    (void)trigger.pointer(0, 5000);
    OA_CHECK(trigger.pointer(0, 5400));
    OA_CHECK(trigger.key_close());
    OA_CHECK(!trigger.pointer(0, 6000));
    OA_CHECK(!trigger.pointer(0, 9000));
    OA_CHECK(trigger.open_slot() == mp::presence_card_no_slot);
    OA_CHECK(!trigger.pointer(mp::presence_card_no_slot, 9100));
    OA_CHECK(!trigger.pointer(0, 9200));
    OA_CHECK(trigger.pointer(0, 9600));
    // Hover moves only by the pointer: the focus leaves it.
    OA_CHECK(!trigger.focus(4));
    OA_CHECK(trigger.open_slot() == 0);
    // An OA screen over the card closes it.
    trigger.close();
    OA_CHECK(trigger.open_slot() == mp::presence_card_no_slot);
}

/// The words automation reads for one drawn line.
///
/// @param label the row's label; empty for a value alone
/// @param value the value, as drawn
/// @return the line's text
std::string line_words(std::string_view label, std::string_view value) {
    if (label.empty())
        return std::string(value);
    return std::string(label) + ": " + std::string(value);
}

// The open tip lists each drawn line as a named control, in order, with
// that line's words. The endpoint adds oa. in front of the name.
void the_open_tip_lists_its_lines() {
    Room room(true, "Steve");
    const int32_t kirby = room.seat(kGuestId, "Kirby", true);
    const auto hash = filled_hash(0x44);
    auto host =
        bundled_record(hash, {"campaign", "core-contingency", "battle-tactics", "cavedog-units"});
    room.own(encoded(host));
    room.receive(
        kGuestId, encoded(bundled_record(hash, {"campaign", "core-contingency", "cavedog-units"}))
    );
    const auto card = mp::presence_card(*room.lobby, kirby, {});
    OA_CHECK(card.has_value());
    if (!card)
        return;
    const kit::HoverCardLook look =
        mp::presence_card_look(*card, kit::SizeClass::compact, 640, fonts());
    const kit::Rect placed{12, 40, look.width, 200};
    kit::DisplayList list;
    kit::add_hover_card(list, placed, look);
    mp::presence_card_controls(list, placed, look);
    OA_CHECK(kit::name_problem(list).empty());
    const std::vector<kit::AutomationEntry> entries = kit::automation_parts(list, {});
    const std::size_t lines = look.rows.size() + look.block.size();
    OA_CHECK(entries.size() == lines);
    for (std::size_t index = 0; index < entries.size() && index < look.rows.size(); ++index) {
        const kit::AutomationEntry& entry = entries[index];
        OA_CHECK(entry.name == "battleroom.tip.line." + std::to_string(index));
        OA_CHECK(entry.text == line_words(look.rows[index].label, look.rows[index].value));
        OA_CHECK(entry.kind == kit::ControlKind::area);
        OA_CHECK(!entry.enabled && !entry.focusable && entry.shown);
        OA_CHECK(
            entry.rect.y == placed.y + kit::compact_metrics.hover_card_padding +
                                kit::compact_metrics.small_line +
                                static_cast<int32_t>(index) * kit::compact_metrics.small_line
        );
    }
    std::string shown;
    for (const kit::AutomationEntry& entry : entries)
        shown += entry.text;
    OA_CHECK(shown.find("Different from the host: no") != std::string::npos);
    OA_CHECK(shown.find("Battle Tactics.") != std::string::npos);
    OA_CHECK(shown.find("cannot start until") != std::string::npos);

    SampleRoom sample;
    const auto sampled = mp::presence_card(*sample.room.lobby, sample.kirby, {});
    OA_CHECK(sampled.has_value());
    if (!sampled)
        return;
    const kit::HoverCardLook sample_look =
        mp::presence_card_look(*sampled, kit::SizeClass::regular, 960, fonts());
    kit::DisplayList sample_list;
    mp::presence_card_controls(sample_list, {0, 0, sample_look.width, 400}, sample_look);
    const std::vector<kit::AutomationEntry> sample_entries = kit::automation_parts(sample_list, {});
    OA_CHECK(sample_entries.size() == sample_look.rows.size() + sample_look.block.size());
    if (sample_entries.size() == sample_look.rows.size() + sample_look.block.size()) {
        for (std::size_t index = 0; index < sample_look.block.size(); ++index) {
            const std::size_t at = sample_look.rows.size() + index;
            OA_CHECK(sample_entries[at].name == "battleroom.tip.line." + std::to_string(at));
            OA_CHECK(sample_entries[at].text == sample_look.block[index]);
        }
    }
}

// A tap on a badge toggles its card and is taken; a tap elsewhere closes it
// and goes on.
void taps_toggle_the_card() {
    mp::PresenceCardTrigger trigger;
    OA_CHECK(!trigger.tap(mp::presence_card_no_slot));
    OA_CHECK(trigger.tap(1));
    OA_CHECK(trigger.open_slot() == 1);
    OA_CHECK(trigger.tap(1));
    OA_CHECK(trigger.open_slot() == mp::presence_card_no_slot);
    OA_CHECK(trigger.tap(2));
    OA_CHECK(trigger.tap(5));
    OA_CHECK(trigger.open_slot() == 5);
    OA_CHECK(!trigger.tap(mp::presence_card_no_slot));
    OA_CHECK(trigger.open_slot() == mp::presence_card_no_slot);
}

// Mutator rows follow Rules. The host's card marks every one present. A
// guest's card names what that player has, and the amber difference lines
// follow the content lines. An empty set adds no rows.
void the_mutator_rows() {
    SampleRoom sample;
    const auto plain = mp::presence_card(*sample.room.lobby, sample.kirby, {});
    OA_CHECK(plain && plain->lines.size() == 7);
    OA_CHECK(
        plain && line_is(*plain, 3, "Rules", "Differ from the host's (rev 4)", mp::CardTone::amber)
    );
    OA_CHECK(plain && line_is(*plain, 4, "Game", "8 hacks change the game"));

    const std::vector<mp::CardMutator> set{
        {"Ridge", mp::CardMutatorHave::has},
        {"Keep", mp::CardMutatorHave::missing},
        {"Watch", mp::CardMutatorHave::other_release},
    };
    const auto guest = mp::presence_card(*sample.room.lobby, sample.kirby, {}, set);
    OA_CHECK(guest.has_value());
    if (!guest)
        return;
    OA_CHECK(line_is(*guest, 3, "Rules", "Differ from the host's (rev 4)", mp::CardTone::amber));
    OA_CHECK(line_is(*guest, 4, "Mutators", "Ridge ✓"));
    OA_CHECK(line_is(*guest, 5, "", "Keep: not installed", mp::CardTone::amber));
    OA_CHECK(line_is(*guest, 6, "", "Watch: another release here", mp::CardTone::amber));
    OA_CHECK(line_is(*guest, 7, "Game", "8 hacks change the game"));
    OA_CHECK(line_is(*guest, 8, "View", "4 view-only hacks"));
    OA_CHECK(line_is(*guest, 9, "Dev mode", "Off"));
    OA_CHECK(line_is(*guest, 10, "", "Different from the host: no Keep.", mp::CardTone::amber));
    OA_CHECK(line_is(
        *guest, 11, "", "Different from the host: another release of Watch.", mp::CardTone::amber
    ));
    OA_CHECK(line_is(
        *guest,
        12,
        "",
        "The game cannot start until Kirby gets it, or the host turns it off.",
        mp::CardTone::amber
    ));
    OA_CHECK(guest->lines.size() == 13);

    const auto host = mp::presence_card(*sample.room.lobby, 0, {}, set);
    OA_CHECK(host.has_value());
    if (host) {
        OA_CHECK(line_is(*host, 3, "Rules", "The host's"));
        OA_CHECK(line_is(*host, 4, "Mutators", "Ridge ✓"));
        OA_CHECK(line_is(*host, 5, "", "Keep ✓"));
        OA_CHECK(line_is(*host, 6, "", "Watch ✓"));
        OA_CHECK(line_is(*host, 7, "Game", "No hacks change the game"));
        bool names_a_difference = false;
        for (const mp::PresenceCardLine& line : host->lines) {
            if (line.value.find("Different from the host") != std::string::npos ||
                line.value.find("cannot start") != std::string::npos)
                names_a_difference = true;
        }
        OA_CHECK(!names_a_difference);
    }

    const auto hash = filled_hash(0x44);
    Room room(true, "Steve");
    const int32_t kirby = room.seat(kGuestId, "Kirby", true);
    auto host_record_bytes = bundled_record(
        hash,
        {"campaign", "core-contingency", "battle-tactics", "cavedog-units", "cavedog-tilesets"}
    );
    host_record_bytes.engine = ng::PresenceEngine{"0.8.0", "macOS", "arm64"};
    room.own(encoded(host_record_bytes));
    room.receive(
        kGuestId,
        encoded(bundled_record(
            hash, {"campaign", "core-contingency", "cavedog-units", "cavedog-tilesets", "music"}
        ))
    );
    const std::vector<mp::CardMutator> one{{"Ridge", mp::CardMutatorHave::missing}};
    const auto mixed = mp::presence_card(*room.lobby, kirby, {}, one);
    OA_CHECK(mixed && line_is(*mixed, 6, "Rules", "Same as the host's"));
    OA_CHECK(mixed && line_is(*mixed, 7, "Mutators", "Ridge: not installed", mp::CardTone::amber));
    OA_CHECK(
        mixed &&
        line_is(*mixed, 8, "", "Different from the host: no Battle Tactics.", mp::CardTone::amber)
    );
    OA_CHECK(
        mixed && line_is(
                     *mixed,
                     9,
                     "",
                     "The game cannot start until Kirby turns it on, or the host turns it off.",
                     mp::CardTone::amber
                 )
    );
    OA_CHECK(
        mixed && line_is(*mixed, 10, "", "Different from the host: no Ridge.", mp::CardTone::amber)
    );
    OA_CHECK(
        mixed && line_is(
                     *mixed,
                     11,
                     "",
                     "The game cannot start until Kirby gets it, or the host turns it off.",
                     mp::CardTone::amber
                 )
    );

    const std::vector<mp::CardMutator> pending{
        {"Ridge", mp::CardMutatorHave::unknown},
        {"Keep", mp::CardMutatorHave::missing},
    };
    const auto waiting = mp::presence_card(*sample.room.lobby, sample.kirby, {}, pending);
    OA_CHECK(waiting.has_value());
    if (waiting) {
        bool names_ridge_mutator = false;
        bool says_keep_missing = false;
        for (const mp::PresenceCardLine& line : waiting->lines) {
            // The mod line already says "Ridge 4.8". This asks only whether
            // the unknown mutator is drawn as installed, missing or lacking.
            if (line.value.find("Ridge:") != std::string::npos ||
                line.value.find("Ridge ✓") != std::string::npos ||
                line.value.find("no Ridge") != std::string::npos)
                names_ridge_mutator = true;
            if (line.value == "Keep: not installed")
                says_keep_missing = true;
        }
        OA_CHECK(!names_ridge_mutator);
        OA_CHECK(says_keep_missing);
    }
    const std::vector<mp::CardMutator> unknown_only{{"Ridge", mp::CardMutatorHave::unknown}};
    const auto quiet = mp::presence_card(*sample.room.lobby, sample.kirby, {}, unknown_only);
    OA_CHECK(plain && quiet && quiet->lines.size() == plain->lines.size());
    if (quiet) {
        for (const mp::PresenceCardLine& line : quiet->lines)
            OA_CHECK(line.value.find("not installed") == std::string::npos);
    }

    translations = {{"Mutators", "LOBBY"}, {"Game", "SPIEL"}};
    sample.room.lobby->services.translate = translate;
    const auto lobby_words = mp::presence_card(*sample.room.lobby, sample.kirby, {}, set);
    sample.room.lobby->services.translate = nullptr;
    translations.clear();
    OA_CHECK(lobby_words && line_is(*lobby_words, 4, "Mutators", "Ridge ✓"));
    OA_CHECK(lobby_words && line_is(*lobby_words, 7, "SPIEL", "8 hacks change the game"));

    oa::data::languages::InterfaceText catalogue;
    OA_CHECK(catalogue.add(
        "[Mutators]\n{\nde=Mutatoren;\n}\n"
        "[{item}: not installed]\n{\nde={item}: nicht installiert;\n}\n"
    ));
    const oa::data::languages::Language* german = oa::data::languages::find_by_tag("de");
    OA_CHECK(german != nullptr);
    if (german != nullptr) {
        oa::data::languages::set_interface_language(&catalogue, *german);
        const auto said = mp::presence_card(*sample.room.lobby, sample.kirby, {}, set);
        oa::data::languages::set_interface_language(nullptr, oa::data::languages::english());
        OA_CHECK(said && line_is(*said, 4, "Mutatoren", "Ridge ✓"));
        OA_CHECK(said && line_is(*said, 5, "", "Keep: nicht installiert", mp::CardTone::amber));
    }
}

} // namespace

int main() {
    the_card_names_the_content();
    the_sample_card_says_what_the_record_says();
    the_rules_line_compares_with_the_host();
    a_development_build_says_so();
    a_player_without_a_record_gets_one_line();
    the_platform_line_leaves_out_an_empty_field();
    the_hosts_card_names_its_map_pack();
    a_31c_player_has_no_card();
    the_words_go_through_the_translation();
    the_mutator_rows();
    the_look_shows_the_sample();
    the_width_follows_the_class();
    a_long_value_is_cut();
    a_difference_line_wraps();
    the_card_lies_by_its_badge();
    hover_opens_after_the_kits_delay();
    moving_between_badges_waits_again();
    the_keys_open_and_close_the_card();
    taps_toggle_the_card();
    the_open_tip_lists_its_lines();
    return oa::test::check_exit_status();
}
