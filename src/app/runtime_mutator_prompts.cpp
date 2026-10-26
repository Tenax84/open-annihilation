// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The four mutator questions, raised. "Start without X?" is pushed over the
// setup when a mutator stops a skirmish or a campaign mission and no match
// was built. "Forget the last set?" is queued once the main menu shows,
// when the crash record named a set. "The game did not start" is pushed
// where a network start was refused because the machines' results differ.
// "Get mutators" is raised by the picker host.
#include "oa_layer.hpp"

#include "oa/app/mutators_memory.hpp"
#include "oa/app/runtime.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/data/mutator_library/library.hpp"
#include "oa/data/mutators/choice.hpp"
#include "oa/formats/oamut/manifest.hpp"
#include "oa/netgame/presence.hpp"
#include "oa/ui/frontend_multiplayer/host_mutators.hpp"
#include "oa/ui/frontend_multiplayer/lobby.hpp"
#include "oa/ui/frontend_multiplayer/room_mutators.hpp"
#include "oa/ui/frontend_multiplayer/screens.hpp"
#include "oa/ui/library/prerequisites.hpp"
#include "oa/ui/library/text.hpp"
#include "oa/ui/mutator_picker/prompts.hpp"
#include "oa/ui/mutator_picker/text.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <exception>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

namespace picker = oa::ui::mutator_picker;
namespace library = oa::ui::library;
namespace mp = oa::ui::frontend_multiplayer;
namespace kit = oa::ui::kit;
namespace fs = std::filesystem;

/// Returns one of the interface's words in the language shown.
///
/// @param context unused
/// @param english the word, in English
/// @return its translation, or the English
std::string_view shown_word(void* /*context*/, std::string_view english) {
    return oa::data::languages::interface_text(english);
}

/// The hooks the questions look words up through.
///
/// @return the hooks
library::TextHooks text_hooks() {
    return library::TextHooks{nullptr, shown_word};
}

/// The name a player sees for an installed mutator, or its id.
///
/// @param folder the Mutators folder
/// @param id the mutator's id
/// @return the name
std::string display_name(const fs::path& folder, std::string_view id) {
    oa::data::mutators::MutatorChoice choice;
    choice.target = std::string(id);
    const std::optional<oa::data::mutators::ResolvedMutator> resolved =
        oa::data::mutators::resolve_choice(choice, folder, nullptr);
    if (!resolved || resolved->manifest.name.empty())
        return std::string(id);
    return resolved->manifest.name;
}

/// The ids an installed mutator needs, by its manifest.
///
/// @param folder the Mutators folder
/// @param id the mutator's id
/// @return those ids
std::vector<std::string> needs_of(const fs::path& folder, std::string_view id) {
    std::vector<std::string> needs;
    oa::data::mutators::MutatorChoice choice;
    choice.target = std::string(id);
    const std::optional<oa::data::mutators::ResolvedMutator> resolved =
        oa::data::mutators::resolve_choice(choice, folder, nullptr);
    if (!resolved)
        return needs;
    for (const oa::formats::oamut::Condition& condition : resolved->manifest.requires_mutators) {
        if (!condition.id.empty() && condition.id != "*")
            needs.push_back(condition.id);
    }
    return needs;
}

/// The room's release of one mutator, as the host's record names it.
///
/// The same id, version and SHA-256 prefix the host sends for its list.
///
/// @param listed the mutator, resolved on the host
/// @return its id, version and prefix
mp::HostMutator room_release(const oa::data::mutators::ResolvedMutator& listed) {
    oa::netgame::PresenceMutator item;
    item.id = listed.manifest.id;
    item.version = oa::formats::oamut::version_text(listed.manifest.version);
    std::memcpy(item.sha256_prefix.data(), listed.package.data(), item.sha256_prefix.size());
    return mp::host_mutator_of(item);
}

/// A slot's name, or empty when the slot names none.
///
/// @param lobby the battle room
/// @param slot the slot; negative names none
/// @return the name
std::string player_name(mp::Lobby& lobby, int32_t slot) {
    if (slot < 0)
        return {};
    const Player& player = mp::slot_player(lobby, slot);
    return {player.name, ::strnlen(player.name, sizeof(player.name))};
}

} // namespace

void Runtime::remember_mutator_start_stops(
    std::optional<mutators_memory::Setup> setup, std::vector<MutatorStartStop> stops
) {
    mutator_start_setup_ = setup;
    mutator_start_stops_ = std::move(stops);
    mutator_start_pending_ = !mutator_start_stops_.empty();
}

bool Runtime::offer_mutator_start_question() {
    if (!mutator_start_pending_ || match_ != nullptr || screen_ == Screen::match)
        return false;
    // Only a setup's own set can start without a mutator, and only a stop
    // that names one can be left out. A refusal of the whole list, or the
    // run's own list, keeps the start's message instead.
    const bool named = std::any_of(
        mutator_start_stops_.begin(), mutator_start_stops_.end(), [](const MutatorStartStop& stop) {
            return !stop.id.empty();
        }
    );
    if (!mutator_start_setup_ || !named) {
        mutator_start_pending_ = false;
        mutator_start_stops_.clear();
        mutator_start_setup_.reset();
        return false;
    }
    std::vector<picker::Failed> failures;
    failures.reserve(mutator_start_stops_.size());
    for (const MutatorStartStop& stop : mutator_start_stops_) {
        picker::Failed failed;
        failed.name = stop.name;
        failed.reason = stop.reason;
        failures.push_back(std::move(failed));
    }
    const Screen over = screen_;
    Runtime* runtime = this;
    QuestionScreen::Host host;
    host.answer = [runtime](int32_t button) {
        if (button == 1)
            runtime->start_without_stopped_mutators();
        else {
            runtime->mutator_start_pending_ = false;
            runtime->mutator_start_stops_.clear();
            runtime->mutator_start_setup_.reset();
        }
        return true;
    };
    host.tick = [runtime, over]() {
        if (runtime->match_ != nullptr || runtime->screen_ == Screen::match)
            return true;
        return runtime->screen_ != over;
    };
    oa_layer().push(
        std::make_unique<QuestionScreen>(
            oa_layer(),
            over,
            picker::start_without_question(failures, text_hooks()),
            std::move(host)
        )
    );
    return true;
}

void Runtime::start_without_stopped_mutators() {
    const std::optional<mutators_memory::Setup> setup = mutator_start_setup_;
    const std::vector<MutatorStartStop> stops = std::move(mutator_start_stops_);
    mutator_start_stops_.clear();
    mutator_start_setup_.reset();
    mutator_start_pending_ = false;
    if (!setup)
        return;
    std::vector<picker::Chosen>* chosen = nullptr;
    if (*setup == mutators_memory::Setup::skirmish)
        chosen = &skirmish_mutators_;
    else if (*setup == mutators_memory::Setup::campaign)
        chosen = &campaign_mutators_;
    if (chosen != nullptr) {
        const fs::path folder =
            user_folder_ / std::string(oa::data::mutator_library::mutators_folder_name);
        std::vector<std::string> among;
        std::vector<library::Need> graph;
        among.reserve(chosen->size());
        graph.reserve(chosen->size());
        for (const picker::Chosen& one : *chosen) {
            among.push_back(one.id);
            library::Need need;
            need.id = one.id;
            need.needs = needs_of(folder, one.id);
            graph.push_back(std::move(need));
        }
        std::set<std::string> drop;
        for (const MutatorStartStop& stop : stops) {
            if (stop.id.empty())
                continue;
            drop.insert(stop.id);
            for (const std::string& dependant : library::dependants_of(graph, among, stop.id))
                drop.insert(dependant);
        }
        std::erase_if(*chosen, [&](const picker::Chosen& one) { return drop.contains(one.id); });
    }
    try {
        if (*setup == mutators_memory::Setup::campaign) {
            start_campaign_mission();
        } else if (*setup == mutators_memory::Setup::skirmish) {
            // The rest of START's own start: the map, the seats and the
            // match, as the setup's START runs it after the hand-over.
            hand_chosen_mutators(*setup);
            const bool started = entry::start_selected_skirmish(
                state_, skirmish_settings_, entry::MenuHandle{kFrontendMenuHandle}, *this
            );
            if (started && match_ && !altitude_sight_blocked_)
                enter_match_view();
        }
    } catch (const std::exception& failure) {
        if (*setup == mutators_memory::Setup::skirmish)
            note_failed_skirmish_start(failure);
    }
}

void Runtime::offer_mutator_crash_question() {
    if (mutator_crash_offered_)
        return;
    if (match_ != nullptr || screen_ == Screen::match)
        return;
    mutator_crash_offered_ = true;
    const std::optional<mutators_memory::Crash> crash = take_mutator_crash();
    if (!crash || crash->loadout.entries.empty())
        return;
    // A run's own list names no setup, so there is nothing to forget. The
    // sentinel's reader already wrote the log line. The question would say
    // MUTATORS… opens empty and then do nothing.
    if (!crash->setup)
        return;
    const fs::path folder =
        user_folder_ / std::string(oa::data::mutator_library::mutators_folder_name);
    std::vector<std::string> names;
    names.reserve(crash->loadout.entries.size());
    for (const oa::data::mutators::LoadoutEntry& entry : crash->loadout.entries)
        names.push_back(display_name(folder, entry.id));
    const std::optional<mutators_memory::Setup> setup = crash->setup;
    Runtime* runtime = this;
    QuestionScreen::Host host;
    host.answer = [runtime, setup](int32_t button) {
        // KEEP IT leaves the remembered set. FORGET IT clears that setup's
        // key and uninstalls nothing. The sentinel file was already removed
        // when the record was read.
        if (button == 1 && setup)
            runtime->forget_mutators(*setup);
        return true;
    };
    host.tick = [runtime]() {
        return runtime->match_ != nullptr || runtime->screen_ == Screen::match;
    };
    auto question = std::make_unique<QuestionScreen>(
        oa_layer(),
        Screen::main_menu,
        picker::forget_set_question(names, text_hooks()),
        std::move(host)
    );
    question->show_on_any_menu();
    oa_layer().show_when_free(std::move(question));
}

void Runtime::offer_differing_results_question() {
    if (match_ != nullptr || screen_ == Screen::match) {
        (void)mp::lobby_take_refused_start();
        return;
    }
    if (oa_layer().find(kit::question_word) != nullptr) {
        (void)mp::lobby_take_refused_start();
        return;
    }
    const std::optional<mp::MutatorsBlock> block = mp::lobby_take_refused_start();
    if (!block)
        return;
    if (block->hold != mp::MutatorsHold::other_result && block->hold != mp::MutatorsHold::lacking)
        return;
    mp::Lobby& lobby = mp::multiplayer_lobby();
    if (block->hold == mp::MutatorsHold::lacking) {
        const mp::MutatorHave have = mp::lobby_mutator_have(lobby, block->slot, block->position);
        if (have != mp::MutatorHave::missing && have != mp::MutatorHave::other_release)
            return;
    }
    picker::Differs differs;
    differs.player = player_name(lobby, block->slot);
    const std::span<const oa::data::mutators::ResolvedMutator> listed = room_mutators();
    if (block->position != oa::netgame::presence_mutator_index_none &&
        static_cast<std::size_t>(block->position) < listed.size()) {
        const oa::formats::oamut::Manifest& manifest =
            listed[static_cast<std::size_t>(block->position)].manifest;
        differs.name = manifest.name.empty() ? manifest.id : manifest.name;
        differs.here = oa::formats::oamut::version_text(manifest.version);
    }
    // Only the host's START can be refused, so the host asks, and `here` is
    // the room's release. A guest's record says only whether it has that
    // release, has another or has none, never which other one it has.
    // `there` is the release on {player}'s machine: the room's when that
    // player has it, and empty when the player lacks it or has another, so
    // the line says the player does not have it. `fetches` is the release
    // {player}'s GET MUTATORS button fetches: the room's own, named only
    // while that player lacks it and this machine's registries offer it by
    // the rule the button follows. A machine that does not host says
    // neither, and neither ever names a release the host does not play.
    if (!differs.name.empty() && !differs.here.empty() && mp::lobby_hosts(lobby)) {
        const mp::MutatorHave have = mp::lobby_mutator_have(lobby, block->slot, block->position);
        if (have == mp::MutatorHave::has) {
            differs.there = differs.here;
        } else if (have == mp::MutatorHave::missing || have == mp::MutatorHave::other_release) {
            const mp::HostMutator room =
                room_release(listed[static_cast<std::size_t>(block->position)]);
            mp::LocalMutator theirs;
            theirs.installed = have == mp::MutatorHave::other_release;
            const mp::MutatorFetch fetch =
                mp::mutator_fetch_state(room, theirs, mutator_offer(room), false, false);
            if (fetch == mp::MutatorFetch::get || fetch == mp::MutatorFetch::other_release_get)
                differs.fetches = differs.here;
        }
    }
    const Screen over = screen_;
    Runtime* runtime = this;
    QuestionScreen::Host host;
    host.answer = [](int32_t) { return true; };
    host.tick = [runtime, over]() {
        if (runtime->match_ != nullptr || runtime->screen_ == Screen::match)
            return true;
        return runtime->screen_ != over;
    };
    const picker::Differs listed_differs[] = {differs};
    oa_layer().push(
        std::make_unique<QuestionScreen>(
            oa_layer(),
            over,
            picker::did_not_start_question(listed_differs, text_hooks()),
            std::move(host)
        )
    );
}

} // namespace oa::app
