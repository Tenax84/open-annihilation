// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The battle room's mutators. USE THESE stores the picker's set as the
// room's list. Once a frame, a change of that list is posted in the chat,
// and the host names a player who newly lacks one. Nothing is sent.

#include "oa/app/runtime.hpp"

#include "oa/data/mutator_library/library.hpp"
#include "oa/data/mutators/choice.hpp"
#include "oa/formats/oamut/manifest.hpp"
#include "oa/netgame/presence.hpp"
#include "oa/netgame/unicode_chat.hpp"
#include "oa/ui/frontend_multiplayer/get_map.hpp"
#include "oa/ui/frontend_multiplayer/mutator_room.hpp"
#include "oa/ui/frontend_multiplayer/panel.hpp"
#include "oa/ui/frontend_multiplayer/room_mutators.hpp"
#include "oa/ui/frontend_multiplayer/screens.hpp"
#include "oa/ui/frontend_renderer/gadget_draw.hpp"
#include "oa/ui/mutator_picker/model.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

namespace picker = oa::ui::mutator_picker;
namespace mp = oa::ui::frontend_multiplayer;

/// The name a player record shows, within its field.
///
/// @param name the field
/// @param bytes how many bytes the field holds
/// @return the name
std::string name_of(const char* name, std::size_t bytes) {
    if (name == nullptr || bytes == 0)
        return {};
    return std::string(name, ::strnlen(name, bytes));
}

/// The name the chat shows for one resolved mutator.
///
/// @param one the mutator
/// @return its name, or its id when it has none
std::string shown_name(const oa::data::mutators::ResolvedMutator& one) {
    return one.manifest.name.empty() ? one.manifest.id : one.manifest.name;
}

} // namespace

void Runtime::apply_room_mutators(std::vector<picker::Chosen> set) {
    if (set.empty()) {
        set_room_mutators({});
        return;
    }
    // One listing of the Mutators folder serves the whole set.
    const oa::data::mutator_library::Listing listing = oa::data::mutator_library::list_installed(
        user_folder_ / std::string(oa::data::mutator_library::mutators_folder_name)
    );
    std::vector<oa::data::mutators::ResolvedMutator> resolved;
    resolved.reserve(set.size());
    for (const picker::Chosen& one : set) {
        oa::data::mutators::MutatorChoice choice;
        choice.target = one.id;
        choice.options = one.options;
        std::string error;
        std::optional<oa::data::mutators::ResolvedMutator> match =
            oa::data::mutators::resolve_choice(choice, listing, &error);
        if (!match) {
            // USE THESE answers from the picker's close: nothing may leave
            // here, and the room keeps the list it had.
            std::cerr << "open-annihilation: mutators: the room's list was not changed: "
                      << (error.empty() ? std::string("the mutators could not be read") : error)
                      << '\n';
            return;
        }
        if (one.clash) {
            match->clashes = *one.clash == picker::Clash::add
                                 ? oa::formats::oamut::ClashMode::add
                                 : oa::formats::oamut::ClashMode::replace;
        }
        resolved.push_back(std::move(*match));
    }
    set_room_mutators(std::move(resolved));
}

int32_t Runtime::chat_list_room() {
    ensure_gui_font();
    const mp::Control* output = mp::panel_control(mp::multiplayer_panel(), "OUTPUT");
    if (output == nullptr || output->width <= 2 || gui_font_.sequences.empty())
        return 0;
    return output->width - 2;
}

int32_t Runtime::chat_list_width(std::string_view line) {
    ensure_gui_font();
    if (unicode_chat_on())
        return gui_text_width(gui_font_, line);
    return oa::ui::frontend_renderer::measure_gadget_glyphs(
        gui_font_, oa::netgame::chat_code_page(line)
    );
}

void Runtime::follow_mutator_room_chat() {
    keep_get_mutators_screen();
    if (!mp::multiplayer_battle_room_open()) {
        mutator_room_chat_names_.clear();
        mutator_room_chat_ids_.clear();
        mutator_room_chat_held_ = false;
        mutator_room_lacks_.clear();
        guest_lack_ids_.clear();
        guest_lack_held_ = false;
        return;
    }
    // A dialog over the room is still this room. Post nothing, and keep
    // what was posted, so closing the dialog does not say it again.
    mp::BattleRoomMap map;
    if (!mp::multiplayer_battle_room_map(&map))
        return;
    mp::Lobby& lobby = mp::multiplayer_lobby();
    if (lobby.game == nullptr)
        return;

    std::vector<std::string> ids;
    std::vector<std::string> names;
    if (map.joiner) {
        // A guest follows the host's list as its records carry it, not the
        // list this machine resolved: a mutator it lacks is still in the
        // room. A mutator installed here shows that copy's name. Any other
        // shows the catalogue's name, or its id when no name is known.
        oa::netgame::PresenceLoadoutAssembly assembly;
        if (mp::multiplayer_room_mutators(&assembly)) {
            if (!oa::netgame::presence_assembly_whole(assembly))
                return;
            const std::vector<HostMutatorFetch> fetched = host_mutators_fetch();
            const oa::data::mutator_library::Listing& listing = installed_mutators();
            ids.reserve(assembly.mutators.size());
            names.reserve(assembly.mutators.size());
            for (std::size_t index = 0; index < assembly.mutators.size(); ++index) {
                const oa::netgame::PresenceMutator& item = assembly.mutators[index];
                const oa::data::mutator_library::InstalledMutator* found =
                    oa::data::mutator_library::find(listing, item.id);
                const std::string_view installed =
                    found != nullptr ? std::string_view(found->manifest.name) : std::string_view();
                const std::string_view catalogue = index < fetched.size()
                                                       ? std::string_view(fetched[index].offer.name)
                                                       : std::string_view();
                ids.push_back(item.id);
                names.push_back(mp::mutator_shown_name(installed, catalogue, item.id));
            }
        }
    } else {
        guest_lack_ids_.clear();
        guest_lack_held_ = false;
        const auto held = room_mutators();
        ids.reserve(held.size());
        names.reserve(held.size());
        for (const oa::data::mutators::ResolvedMutator& one : held) {
            ids.push_back(one.manifest.id);
            names.push_back(shown_name(one));
        }
    }

    // A player's name is game text: a 3.1c player's in the code page, such
    // as "J\xF6rg". The lines are UTF-8 until they are posted, so a name is
    // read as UTF-8 first and converted with the line once.
    std::string host;
    const uint8_t host_slot = mp::lobby_host_slot(lobby);
    if (host_slot != mp::kNoSlot) {
        const Player& player = mp::slot_player(lobby, static_cast<int32_t>(host_slot));
        host = oa::netgame::chat_utf8(name_of(player.name, sizeof player.name));
    }
    const auto post = [&](const std::vector<std::string>& lines) {
        if (lines.empty())
            return;
        // The chat list draws one stored line on one row. A row wider than
        // the list is clipped, so a long line is posted as several, the way
        // a typed line is. With Unicode chat off the row is the 3.1c font's
        // code page, so an accent is one glyph.
        const int32_t room = chat_list_room();
        const auto measure = [this](std::string_view text) { return chat_list_width(text); };
        for (const std::string& line : lines) {
            const std::vector<std::string> fitted = mp::fit_chat_line(line, room, measure);
            for (const std::string& piece : fitted) {
                if (unicode_chat_on()) {
                    mp::lobby_post_chat(lobby, piece.c_str());
                    continue;
                }
                const std::string bytes = oa::netgame::chat_code_page(piece);
                mp::lobby_post_chat(lobby, bytes.c_str());
            }
        }
    };
    if (!mutator_room_chat_held_) {
        mutator_room_chat_held_ = true;
        if (!names.empty())
            post(mp::mutator_chat_lines(host, {}, names));
    } else if (ids != mutator_room_chat_ids_) {
        post(mp::mutator_chat_lines(host, mutator_room_chat_names_, names));
    }
    mutator_room_chat_ids_ = std::move(ids);
    mutator_room_chat_names_ = std::move(names);
    if (map.joiner) {
        std::vector<mp::GuestLack> lacking;
        const std::vector<HostMutatorFetch> fetch = host_mutators_fetch();
        lacking.reserve(fetch.size());
        for (const HostMutatorFetch& item : fetch) {
            if (item.state == mp::MutatorFetch::have)
                continue;
            mp::GuestLack lack;
            lack.id = item.mutator.id;
            lack.name = item.offer.name.empty() ? item.mutator.id : item.offer.name;
            // GET MUTATORS is promised only where a registry offers the
            // host's release; otherwise the line says what is missing alone.
            const bool offered = item.state == mp::MutatorFetch::get ||
                                 item.state == mp::MutatorFetch::other_release_get ||
                                 item.state == mp::MutatorFetch::downloading ||
                                 item.state == mp::MutatorFetch::failed;
            if (offered) {
                lack.registry = item.offer.registry_name.empty() ? item.offer.registry_id
                                                                 : item.offer.registry_name;
            }
            lacking.push_back(std::move(lack));
        }
        if (!guest_lack_held_) {
            guest_lack_held_ = true;
            post(mp::guest_lack_lines({}, lacking));
        } else {
            post(mp::guest_lack_lines(guest_lack_ids_, lacking));
        }
        guest_lack_ids_.clear();
        guest_lack_ids_.reserve(lacking.size());
        for (const mp::GuestLack& lack : lacking)
            guest_lack_ids_.push_back(lack.id.empty() ? lack.name : lack.id);
        return;
    }

    std::vector<RoomLackNote> current;
    const std::size_t count = std::min(mutator_room_chat_names_.size(), std::size_t{32});
    for (int32_t slot = 0; slot < mp::kSlotCount; ++slot) {
        const Player& player = mp::slot_player(lobby, slot);
        if (!mp::slot_active(player))
            continue;
        for (std::size_t position = 0; position < count; ++position) {
            const mp::MutatorHave have =
                mp::lobby_mutator_have(lobby, slot, static_cast<uint8_t>(position));
            if (have != mp::MutatorHave::missing && have != mp::MutatorHave::other_release)
                continue;
            RoomLackNote note;
            note.slot = slot;
            note.position = static_cast<uint8_t>(position);
            note.name = mutator_room_chat_names_[position];
            current.push_back(std::move(note));
        }
    }
    std::vector<mp::PlayerLack> fresh;
    for (const RoomLackNote& note : current) {
        bool seen = false;
        for (const RoomLackNote& posted : mutator_room_lacks_) {
            if (posted == note)
                seen = true;
        }
        if (seen)
            continue;
        const Player& player = mp::slot_player(lobby, note.slot);
        mp::PlayerLack lack;
        lack.player = oa::netgame::chat_utf8(name_of(player.name, sizeof player.name));
        lack.name = note.name;
        fresh.push_back(std::move(lack));
    }
    post(mp::lacking_lines(fresh));
    mutator_room_lacks_ = std::move(current);
}

} // namespace oa::app
