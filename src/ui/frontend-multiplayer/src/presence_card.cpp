// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The presence card (presence_card.hpp): its words from a player's presence
// record, the kit's hover card that shows them, its place by the badge and
// when it opens.

#include "oa/ui/frontend_multiplayer/presence_card.hpp"

#include "oa/data/content_bundles.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/present/game_text.hpp"
#include "oa/ui/kit/theme.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::ui::frontend_multiplayer {

namespace {

/// Returns a word of the card in the language shown.
///
/// @param lobby the battle room, whose translation it goes through
/// @param english the word, in English
/// @return the word
std::string word(const Lobby& lobby, const char* english) {
    const char* shown = lobby_translated(lobby, english);
    return shown != nullptr ? std::string(shown) : std::string(english);
}

/// One placeholder of a phrase and what takes its place.
struct Filling {
    std::string_view placeholder{}; ///< the placeholder, such as "{count}"
    std::string_view value{};       ///< what takes its place
};

/// Returns a phrase of the card in the language shown, with values in place
/// of its placeholders.
///
/// @param lobby the battle room, whose translation it goes through
/// @param english the phrase, in English, holding the placeholders
/// @param fillings each placeholder and what takes its place
/// @return the phrase
std::string
filled(const Lobby& lobby, const char* english, std::initializer_list<Filling> fillings) {
    std::string phrase = word(lobby, english);
    for (const Filling& filling : fillings) {
        const std::size_t at = phrase.find(filling.placeholder);
        if (at != std::string::npos)
            phrase.replace(at, filling.placeholder.size(), filling.value);
    }
    return phrase;
}

/// Returns a mutator word of the card from the interface catalogue.
///
/// @param english the word, in English
/// @return the word
std::string catalogue_word(const char* english) {
    return std::string(oa::data::languages::interface_text(english));
}

/// Returns a mutator phrase of the card from the interface catalogue, with
/// values in place of its placeholders.
///
/// @param english the phrase, in English, holding the placeholders
/// @param fillings each placeholder and what takes its place
/// @return the phrase
std::string catalogue_filled(const char* english, std::initializer_list<Filling> fillings) {
    std::string phrase = catalogue_word(english);
    for (const Filling& filling : fillings) {
        const std::size_t at = phrase.find(filling.placeholder);
        if (at != std::string::npos)
            phrase.replace(at, filling.placeholder.size(), filling.value);
    }
    return phrase;
}

/// Returns a player's name as UTF-8 in capitals: a to z, and the small
/// letters of Latin-1 from à to þ (all but ÷), made capitals.
///
/// @param player the player
/// @return the name
std::string name_in_capitals(const Player& player) {
    std::size_t length = 0;
    while (length < sizeof player.name && player.name[length] != '\0')
        ++length;
    std::string name = oa::present::decode_game_text(
        std::string_view(player.name, length), oa::present::game_text_settings().utf8
    );
    constexpr auto latin_lead = static_cast<unsigned char>(0xc3);
    constexpr auto small_first = static_cast<unsigned char>(0xa0); // à
    constexpr auto small_last = static_cast<unsigned char>(0xbe);  // þ
    constexpr auto division = static_cast<unsigned char>(0xb7);    // ÷
    constexpr unsigned char capital_step = 0x20;
    for (std::size_t index = 0; index < name.size(); ++index) {
        const auto code = static_cast<unsigned char>(name[index]);
        if (code >= 'a' && code <= 'z') {
            name[index] = static_cast<char>(code - capital_step);
        } else if (code == latin_lead && index + 1 < name.size()) {
            const auto next = static_cast<unsigned char>(name[index + 1]);
            if (next >= small_first && next <= small_last && next != division)
                name[index + 1] = static_cast<char>(next - capital_step);
            ++index;
        }
    }
    return name;
}

/// Returns the words before and after a separator, either left out when empty.
///
/// @param first the first words
/// @param second the second words
/// @return "<first> · <second>", "<first>", "<second>" or nothing
std::string joined(std::string_view first, std::string_view second) {
    std::string text(first);
    if (!first.empty() && !second.empty())
        text += " · ";
    text += second;
    return text;
}

/// Returns a presence record's sim hash, or the plain baseline's for a host
/// on 3.1c, which sends no record and plays 3.1c's own rules.
///
/// @param lobby the battle room
/// @param slot the slot
/// @param record the slot's record; nothing when it has none
/// @return the hash; nothing when no record or rule tells it
std::optional<netgame::PresenceDigest>
known_sim_hash(Lobby& lobby, int32_t slot, const std::optional<netgame::PresenceRecord>& record) {
    if (record)
        return record->sim_hash;
    if (lobby_row_badge(lobby, slot).badge == RowBadge::open_annihilation)
        return std::nullopt;
    return lobby.presence_records.plain_sim_hash;
}

/// Reads the presence record of a slot's player.
///
/// @param lobby the battle room
/// @param slot the slot
/// @return the record; nothing when none came or it does not read
std::optional<netgame::PresenceRecord> record_of(Lobby& lobby, int32_t slot) {
    const auto bytes = lobby_presence_record(lobby, slot);
    if (bytes.empty())
        return std::nullopt;
    netgame::PresenceRecord record;
    if (netgame::decode_presence(bytes.data(), bytes.size(), &record) != netgame::WireError::ok)
        return std::nullopt;
    return record;
}

/// Returns the Engine line's value.
///
/// @param lobby the battle room
/// @param record the player's record
/// @return the value
std::string engine_value(const Lobby& lobby, const netgame::PresenceRecord& record) {
    if (!record.engine || record.engine->version.empty())
        return word(lobby, "Open Annihilation");
    const std::string& version = record.engine->version;
    const std::size_t label = version.find('-');
    if (label == std::string::npos)
        return filled(lobby, "Open Annihilation {version}", {{"{version}", version}});
    return filled(
        lobby,
        "Open Annihilation {version} (development build)",
        {{"{version}", std::string_view(version).substr(0, label)}}
    );
}

/// Returns the Rules line.
///
/// @param lobby the battle room
/// @param slot the player's slot
/// @param record the player's record
/// @return the line, its label left to the caller
PresenceCardLine rules_line(Lobby& lobby, int32_t slot, const netgame::PresenceRecord& record) {
    const uint8_t host = lobby_host_slot(lobby);
    if (host == static_cast<uint8_t>(slot))
        return {{}, word(lobby, "The host's"), CardTone::plain};
    if (host == kNoSlot)
        return {{}, word(lobby, "Unknown"), CardTone::plain};
    const auto host_record = record_of(lobby, host);
    if (lobby_sim_hash_differs_from_host(lobby, slot)) {
        if (record.mod && host_record && host_record->mod &&
            host_record->mod->id == record.mod->id &&
            host_record->mod->packaging_revision != record.mod->packaging_revision)
            return {
                {},
                filled(
                    lobby,
                    "Differ from the host's (rev {revision})",
                    {{"{revision}", std::to_string(host_record->mod->packaging_revision)}}
                ),
                CardTone::amber
            };
        return {{}, word(lobby, "Differ from the host's"), CardTone::amber};
    }
    const auto own_hash = record.sim_hash;
    const auto host_hash = known_sim_hash(lobby, host, host_record);
    if (own_hash && host_hash && *own_hash == *host_hash)
        return {{}, word(lobby, "Same as the host's"), CardTone::plain};
    return {{}, word(lobby, "Unknown"), CardTone::plain};
}

/// Returns a count as one of three phrases: none, one and more.
///
/// @param lobby the battle room
/// @param count the count
/// @param none the phrase for none
/// @param one the phrase for one
/// @param more the phrase for more, holding "{count}"
/// @return the phrase
std::string
counted(const Lobby& lobby, uint32_t count, const char* none, const char* one, const char* more) {
    if (count == 0)
        return word(lobby, none);
    if (count == 1)
        return word(lobby, one);
    return filled(lobby, more, {{"{count}", std::to_string(count)}});
}

/// Returns the Map line's value and colour.
///
/// @param lobby the battle room
/// @param pack the host's map pack
/// @param pack_state what this machine has of it; unset counts as unknown
/// @return the line, its label left to the caller
PresenceCardLine map_line(
    const Lobby& lobby,
    const netgame::PresenceMapPack& pack,
    const std::function<PackState(const netgame::PresenceMapPack&)>& pack_state
) {
    std::string named = pack.name.empty() ? pack.key : pack.name;
    if (!pack.version.empty())
        named += " " + pack.version;
    switch (pack_state ? pack_state(pack) : PackState::unknown) {
    case PackState::installed:
        return {{}, filled(lobby, "{pack} ✓ installed", {{"{pack}", named}}), CardTone::lime};
    case PackState::other_release:
        return {
            {}, filled(lobby, "{pack} · another release here", {{"{pack}", named}}), CardTone::amber
        };
    case PackState::missing:
        return {{}, filled(lobby, "{pack} · not installed", {{"{pack}", named}}), CardTone::plain};
    case PackState::unknown:
        break;
    }
    return {{}, std::move(named), CardTone::plain};
}

/// Returns a player's name as stored, decoded the way the card's title is.
///
/// @param player the player
/// @return the name
std::string stored_name(const Player& player) {
    std::size_t length = 0;
    while (length < sizeof player.name && player.name[length] != '\0')
        ++length;
    return oa::present::decode_game_text(
        std::string_view(player.name, length), oa::present::game_text_settings().utf8
    );
}

/// Returns the name a bundle's id is shown by. An id the table does not know is shown as itself.
///
/// @param lobby the battle room
/// @param id the bundle id
/// @return the name
std::string bundle_label(const Lobby& lobby, std::string_view id) {
    const auto* bundle = oa::data::content_bundles::find_bundle(id);
    if (bundle == nullptr)
        return std::string(id);
    const std::string english(bundle->english_name);
    return word(lobby, english.c_str());
}

/// Joins expansion names in the built-in order, leaving music out.
///
/// @param lobby the battle room
/// @param ids the expansion ids
/// @return the names, or "None" when there are none
std::string expansion_list(const Lobby& lobby, const std::vector<std::string>& ids) {
    std::string text;
    std::vector<char> taken(ids.size(), 0);
    const auto append = [&](std::string_view id) {
        if (!text.empty())
            text += ", ";
        text += bundle_label(lobby, id);
    };
    for (const auto& bundle : oa::data::content_bundles::bundles()) {
        if (bundle.kind != oa::data::content_bundles::Kind::expansion)
            continue;
        if (bundle.id == netgame::presence_uncompared_expansion)
            continue;
        for (std::size_t index = 0; index < ids.size(); ++index) {
            if (taken[index] == 0 && ids[index] == bundle.id) {
                taken[index] = 1;
                append(ids[index]);
            }
        }
    }
    for (std::size_t index = 0; index < ids.size(); ++index) {
        if (taken[index] == 0 && ids[index] != netgame::presence_uncompared_expansion)
            append(ids[index]);
    }
    if (text.empty())
        return word(lobby, "None");
    return text;
}

/// Joins the names of several expansion ids, in the built-in order.
///
/// @param lobby the battle room
/// @param ids the ids
/// @return the names
std::string joined_bundle_names(const Lobby& lobby, const std::vector<std::string>& ids) {
    const std::string listed = expansion_list(lobby, ids);
    return listed;
}

/// Adds the lines that name a difference from the host.
///
/// @param[in,out] card the card
/// @param lobby the battle room
/// @param slot the player's slot
/// @param record the player's record
void append_difference(
    PresenceCard& card, Lobby& lobby, int32_t slot, const netgame::PresenceRecord& record
) {
    const uint8_t host = lobby_host_slot(lobby);
    if (host == kNoSlot || host == static_cast<uint8_t>(slot) || !record.bundles)
        return;
    const auto host_record = record_of(lobby, host);
    if (!host_record)
        return;
    const auto gap = netgame::presence_content_gap(record, *host_record);
    if (!gap.comparable)
        return;
    const bool expansions_differ = !gap.missing.empty() || !gap.extra.empty();
    if (!gap.hashes_differ && !gap.bases_differ && !expansions_differ)
        return;
    const std::string player = stored_name(slot_player(lobby, slot));
    const bool one_missing =
        !gap.bases_differ && !gap.hashes_differ && gap.missing.size() == 1 && gap.extra.empty();
    const bool one_extra =
        !gap.bases_differ && !gap.hashes_differ && gap.extra.size() == 1 && gap.missing.empty();
    const bool named_set = !gap.bases_differ && !gap.hashes_differ &&
                           (gap.missing.empty() || gap.extra.empty()) && expansions_differ;
    if (gap.hashes_differ)
        card.lines.push_back(
            {{}, word(lobby, "Different from the host: a file does not match."), CardTone::amber}
        );
    if (gap.bases_differ) {
        card.lines.push_back(
            {{},
             filled(
                 lobby,
                 "Different from the host: another base game ({name}).",
                 {{"{name}", bundle_label(lobby, gap.player_base)}}
             ),
             CardTone::amber}
        );
    } else if (named_set && gap.extra.empty()) {
        card.lines.push_back(
            {{},
             filled(
                 lobby,
                 "Different from the host: no {name}.",
                 {{"{name}", joined_bundle_names(lobby, gap.missing)}}
             ),
             CardTone::amber}
        );
    } else if (named_set) {
        card.lines.push_back(
            {{},
             filled(
                 lobby,
                 "Different from the host: {name} is on.",
                 {{"{name}", joined_bundle_names(lobby, gap.extra)}}
             ),
             CardTone::amber}
        );
    }
    if (one_missing) {
        card.lines.push_back(
            {{},
             filled(
                 lobby,
                 "The game cannot start until {player} turns it on, or the host turns it off.",
                 {{"{player}", player}}
             ),
             CardTone::amber}
        );
    } else if (one_extra) {
        card.lines.push_back(
            {{},
             filled(
                 lobby,
                 "The game cannot start until the host turns it on, or {player} turns it off.",
                 {{"{player}", player}}
             ),
             CardTone::amber}
        );
    } else {
        card.lines.push_back(
            {{}, word(lobby, "The game cannot start until the content matches."), CardTone::amber}
        );
    }
}

/// Adds one row per mutator of the host's set, after Rules.
///
/// The first row is labelled "Mutators". The host's card marks every mutator
/// present. A player who has it reads "{item} ✓". One who does not reads
/// "{item}: not installed" or "{item}: another release here", in amber. A
/// row whose state is still unknown is left out.
///
/// @param[in,out] card the card
/// @param lobby the battle room
/// @param slot the player's slot
/// @param mutators the host's set
void append_mutators(
    PresenceCard& card, Lobby& lobby, int32_t slot, std::span<const CardMutator> mutators
) {
    if (mutators.empty())
        return;
    const bool host = lobby_host_slot(lobby) == static_cast<uint8_t>(slot);
    bool first = true;
    for (const CardMutator& item : mutators) {
        const CardMutatorHave have = host ? CardMutatorHave::has : item.have;
        PresenceCardLine line;
        if (have == CardMutatorHave::unknown)
            continue;
        if (first)
            line.label = catalogue_word("Mutators");
        first = false;
        switch (have) {
        case CardMutatorHave::has:
            line.value = catalogue_filled("{item} ✓", {{"{item}", item.name}});
            line.tone = CardTone::plain;
            break;
        case CardMutatorHave::other_release:
            line.value = catalogue_filled("{item}: another release here", {{"{item}", item.name}});
            line.tone = CardTone::amber;
            break;
        case CardMutatorHave::missing:
            line.value = catalogue_filled("{item}: not installed", {{"{item}", item.name}});
            line.tone = CardTone::amber;
            break;
        case CardMutatorHave::unknown:
            break;
        }
        card.lines.push_back(std::move(line));
    }
}

/// Adds the amber lines for a player who lacks a mutator of the host's set.
///
/// They follow the content difference lines. The host's card has none.
///
/// @param[in,out] card the card
/// @param lobby the battle room
/// @param slot the player's slot
/// @param mutators the host's set
void append_mutator_difference(
    PresenceCard& card, Lobby& lobby, int32_t slot, std::span<const CardMutator> mutators
) {
    if (mutators.empty())
        return;
    const uint8_t host = lobby_host_slot(lobby);
    if (host == kNoSlot || host == static_cast<uint8_t>(slot))
        return;
    bool any = false;
    for (const CardMutator& item : mutators) {
        if (item.have == CardMutatorHave::has || item.have == CardMutatorHave::unknown)
            continue;
        any = true;
        const char* phrase = item.have == CardMutatorHave::other_release
                                 ? "Different from the host: another release of {name}."
                                 : "Different from the host: no {name}.";
        card.lines.push_back(
            {{}, catalogue_filled(phrase, {{"{name}", item.name}}), CardTone::amber}
        );
    }
    if (!any)
        return;
    const std::string player = stored_name(slot_player(lobby, slot));
    card.lines.push_back(
        {{},
         catalogue_filled(
             "The game cannot start until {player} gets it, or the host turns it off.",
             {{"{player}", player}}
         ),
         CardTone::amber}
    );
}

/// Returns a line with its label.
///
/// @param label the label, translated
/// @param line the value and its colour
/// @return the line
PresenceCardLine labelled(std::string label, PresenceCardLine line) {
    line.label = std::move(label);
    return line;
}

/// Returns a colour token for a line's colour.
///
/// @param tone the line's colour
/// @return the token
kit::Colour tone_colour(CardTone tone) noexcept {
    switch (tone) {
    case CardTone::amber:
        return kit::colour::lock;
    case CardTone::lime:
        return kit::colour::accent;
    case CardTone::plain:
        break;
    }
    return kit::colour::text;
}

} // namespace

std::optional<PresenceCard> presence_card(
    Lobby& lobby,
    int32_t slot,
    const std::function<PackState(const netgame::PresenceMapPack&)>& pack_state,
    std::span<const CardMutator> mutators
) {
    if (slot < 0 || slot >= kSlotCount ||
        lobby_row_badge(lobby, slot).badge != RowBadge::open_annihilation)
        return std::nullopt;
    PresenceCard card;
    card.title = name_in_capitals(slot_player(lobby, slot));
    const auto record = record_of(lobby, slot);
    if (!record) {
        // An OA build before 0.8 carries the signature and sends no record.
        card.lines.push_back(
            {word(lobby, "Engine"), word(lobby, "Open Annihilation (before 0.8)"), CardTone::plain}
        );
        return card;
    }
    card.lines.push_back({word(lobby, "Engine"), engine_value(lobby, *record), CardTone::plain});
    if (record->engine) {
        const std::string platform = joined(record->engine->platform, record->engine->architecture);
        if (!platform.empty())
            card.lines.push_back({word(lobby, "Platform"), platform, CardTone::plain});
    }
    if (record->bundles) {
        card.lines.push_back(
            {word(lobby, "Base game"),
             bundle_label(lobby, record->bundles->base_game),
             CardTone::plain}
        );
        card.lines.push_back(
            {word(lobby, "Expansions"),
             expansion_list(lobby, record->bundles->expansions),
             CardTone::plain}
        );
    }
    if (record->mod) {
        const auto& mod = *record->mod;
        const std::string named = mod.version.empty() ? mod.name : mod.name + " " + mod.version;
        const std::string revision = std::to_string(mod.packaging_revision);
        card.lines.push_back(
            {word(lobby, "Mod"),
             filled(lobby, "{mod} · rev {revision}", {{"{mod}", named}, {"{revision}", revision}}),
             CardTone::plain}
        );
    } else if (record->bundles) {
        card.lines.push_back({word(lobby, "Mod"), word(lobby, "No Mod"), CardTone::plain});
    } else {
        card.lines.push_back(
            {word(lobby, "Mod"), word(lobby, "Total Annihilation 3.1c"), CardTone::plain}
        );
    }
    if (record->language)
        card.lines.push_back({word(lobby, "Language"), *record->language, CardTone::plain});
    card.lines.push_back(labelled(word(lobby, "Rules"), rules_line(lobby, slot, *record)));
    append_mutators(card, lobby, slot, mutators);
    if (record->game_hacks)
        card.lines.push_back(
            {word(lobby, "Game"),
             counted(
                 lobby,
                 record->game_hacks->total,
                 "No hacks change the game",
                 "1 hack changes the game",
                 "{count} hacks change the game"
             ),
             CardTone::plain}
        );
    if (record->view_hacks)
        card.lines.push_back(
            {word(lobby, "View"),
             counted(
                 lobby,
                 record->view_hacks->total,
                 "No view-only hacks",
                 "1 view-only hack",
                 "{count} view-only hacks"
             ),
             CardTone::plain}
        );
    if (record->developer) {
        const auto& developer = *record->developer;
        std::string value = word(lobby, "Off");
        if (developer.on)
            value =
                counted(lobby, developer.total, "On", "On · 1 override", "On · {count} overrides");
        card.lines.push_back({word(lobby, "Dev mode"), std::move(value), CardTone::plain});
    }
    if (record->map_pack && lobby_host_slot(lobby) == static_cast<uint8_t>(slot))
        card.lines.push_back(
            labelled(word(lobby, "Map"), map_line(lobby, *record->map_pack, pack_state))
        );
    if (record->game_hacks) {
        const auto& hacks = *record->game_hacks;
        const std::size_t listed = std::min(hacks.ids.size(), presence_card_listed_hacks);
        card.hacks.assign(
            hacks.ids.begin(), hacks.ids.begin() + static_cast<std::ptrdiff_t>(listed)
        );
        const auto total = static_cast<std::size_t>(hacks.total);
        if (total > presence_card_listed_hacks)
            card.hacks_more = filled(
                lobby,
                "· {count} more",
                {{"{count}", std::to_string(total - presence_card_listed_hacks)}}
            );
    }
    append_difference(card, lobby, slot, *record);
    append_mutator_difference(card, lobby, slot, mutators);
    return card;
}

kit::HoverCardLook presence_card_look(
    const PresenceCard& card,
    kit::SizeClass size_class,
    int32_t window_points_width,
    const kit::Fonts& fonts
) {
    const kit::Metrics& metrics = kit::metrics_of(size_class);
    int32_t width = presence_card_compact_width;
    if (size_class == kit::SizeClass::regular)
        width = presence_card_regular_width;
    else if (size_class == kit::SizeClass::large)
        width = presence_card_large_width;
    width = std::max(std::min(width, window_points_width - 2 * presence_card_edge_gap), int32_t{1});

    kit::HoverCardLook look;
    look.width = width;
    look.mark = true;
    look.title = card.title;
    const int32_t room = std::max(width - 2 * metrics.hover_card_padding, int32_t{0});
    int32_t widest_label = 0;
    bool labelled_rows = false;
    for (const PresenceCardLine& line : card.lines) {
        if (line.label.empty())
            continue;
        labelled_rows = true;
        widest_label =
            std::max(widest_label, kit::text_width(fonts, kit::FontRole::small, line.label));
    }
    const int32_t label_column = labelled_rows ? widest_label + metrics.hover_card_padding : 0;
    const auto measure = [&](std::string_view text) {
        return kit::text_width(fonts, kit::FontRole::small, text);
    };
    for (const PresenceCardLine& line : card.lines) {
        const int32_t value_room =
            std::max(room - (line.label.empty() ? 0 : label_column), int32_t{0});
        const kit::Colour ink = tone_colour(line.tone);
        // A difference line has no label. It wraps onto at most
        // presence_card_difference_lines lines. The last is cut with "..."
        // when the rest does not fit, and the lines before it are left whole.
        // A labelled value stays one row, cut when it must.
        if (!line.label.empty() || measure(line.value) <= value_room) {
            look.rows.push_back(
                {line.label,
                 kit::cut_to_width(fonts, kit::FontRole::small, line.value, value_room),
                 ink}
            );
            continue;
        }
        kit::WrapRules rules;
        rules.most_lines = presence_card_difference_lines;
        rules.shorten_last = [&fonts, value_room](std::string_view last) {
            return kit::cut_to_width(fonts, kit::FontRole::small, last, value_room);
        };
        for (std::string& wrapped :
             kit::wrap(line.value, std::max(value_room, int32_t{1}), measure, rules))
            look.rows.push_back({{}, std::move(wrapped), ink});
    }
    for (const std::string& hack : card.hacks)
        look.block.push_back(kit::cut_to_width(fonts, kit::FontRole::small, hack, room));
    if (!card.hacks_more.empty())
        look.block.push_back(kit::cut_to_width(fonts, kit::FontRole::small, card.hacks_more, room));
    return look;
}

kit::Rect presence_card_place(
    kit::Rect badge, kit::Point size, kit::Rect safe_area, int32_t /*arrow_depth*/
) {
    const kit::Rect frame = kit::grown(safe_area, -presence_card_edge_gap);
    return kit::hover_card_rect(badge, frame, size);
}

void presence_card_controls(
    kit::DisplayList& list, kit::Rect card, const kit::HoverCardLook& look
) {
    // The hover card is drawn with the Compact metrics at every size class.
    const kit::Metrics& metrics = kit::compact_metrics;
    const int32_t padding = metrics.hover_card_padding;
    const int32_t row_height = metrics.small_line;
    const bool header = look.mark || !look.title.empty() || !look.aside.empty();
    int32_t row = card.y + padding + (header ? row_height : 0);
    const int32_t left = card.x + padding;
    const int32_t width = std::max(card.width - 2 * padding, int32_t{0});
    int32_t index = 0;
    const auto add = [&](std::string words) {
        kit::Control control;
        control.id = index + 1;
        control.rect = {left, row, width, row_height};
        control.enabled = false;
        control.focusable = false;
        control.name = "battleroom.tip.line." + std::to_string(index);
        control.kind = kit::ControlKind::area;
        control.text = std::move(words);
        list.controls.push_back(std::move(control));
        ++index;
        row += row_height;
    };
    for (const kit::HoverCardRow& entry : look.rows) {
        if (entry.label.empty())
            add(entry.value);
        else
            add(entry.label + ": " + entry.value);
    }
    for (const std::string& text : look.block)
        add(text);
}

void presence_card_point(kit::HoverCardLook& look, kit::Rect card, kit::Rect badge) noexcept {
    const int32_t badge_bottom = badge.y + badge.height;
    bool below = card.y >= badge_bottom;
    // A card the window pressed over its badge points from the side nearer it.
    if (!below && card.y + card.height > badge.y)
        below = 2 * card.y + card.height > 2 * badge.y + badge.height;
    look.arrow = below ? kit::Side::top : kit::Side::bottom;
    look.arrow_at = badge.x + badge.width / 2 - card.x;
}

bool PresenceCardTrigger::pointer(int32_t slot, uint64_t now_ms) noexcept {
    const int32_t before = open_;
    const bool moved = slot != resting_;
    resting_ = slot;
    if (held_off_ && slot != held_slot_)
        held_off_ = false;
    const kit::ControlId hovered =
        slot != presence_card_no_slot && !held_off_ ? kit::ControlId{slot} : kit::no_control;
    const bool due = kit::hover_card_due(timer_, hovered, now_ms);
    if (open_ != presence_card_no_slot) {
        if (slot == open_ || (slot == presence_card_no_slot && over_card_))
            pointer_inside_ = true;
        else if (slot != presence_card_no_slot ? moved : pointer_inside_)
            drop();
    }
    if (open_ == presence_card_no_slot && due)
        open(slot, Opener::hover);
    return open_ != before;
}

bool PresenceCardTrigger::key_info(int32_t focused_slot) noexcept {
    if (open_ != presence_card_no_slot) {
        if (focused_slot != presence_card_no_slot && focused_slot != open_)
            open(focused_slot, Opener::key);
        else
            shut();
        return true;
    }
    if (focused_slot == presence_card_no_slot)
        return false;
    open(focused_slot, Opener::key);
    return true;
}

bool PresenceCardTrigger::key_close() noexcept {
    if (open_ == presence_card_no_slot)
        return false;
    shut();
    return true;
}

bool PresenceCardTrigger::tap(int32_t slot) noexcept {
    if (slot == presence_card_no_slot) {
        if (open_ != presence_card_no_slot)
            shut();
        return false;
    }
    if (open_ == slot)
        shut();
    else
        open(slot, Opener::tap);
    return true;
}

void PresenceCardTrigger::over_card(bool over) noexcept {
    over_card_ = over && open_ != presence_card_no_slot;
    if (over_card_)
        pointer_inside_ = true;
}

bool PresenceCardTrigger::focus(int32_t focused_slot) noexcept {
    if (open_ == presence_card_no_slot || opened_by_ != Opener::key || focused_slot == open_)
        return false;
    if (focused_slot != presence_card_no_slot)
        open(focused_slot, Opener::key);
    else
        shut();
    return true;
}

void PresenceCardTrigger::close() noexcept {
    if (open_ != presence_card_no_slot)
        shut();
}

void PresenceCardTrigger::open(int32_t slot, Opener by) noexcept {
    open_ = slot;
    opened_by_ = by;
    over_card_ = false;
    pointer_inside_ = resting_ == slot;
    held_off_ = false;
}

void PresenceCardTrigger::drop() noexcept {
    open_ = presence_card_no_slot;
    over_card_ = false;
    pointer_inside_ = false;
}

void PresenceCardTrigger::shut() noexcept {
    drop();
    timer_ = {};
    held_off_ = resting_ != presence_card_no_slot;
    held_slot_ = resting_;
}

} // namespace oa::ui::frontend_multiplayer
