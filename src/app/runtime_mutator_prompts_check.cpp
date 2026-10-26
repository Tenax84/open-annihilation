// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-mutator-prompts: a skirmish one made-up mutator stops, START
// WITHOUT IT and BACK, the question after a crash, and Get mutators over
// the picker. Lantern's plan changes nothing. Slip's plan raises an error.
// The scratch user folder holds both. The get question uses the mutator
// picker's check feed. "The game did not start" is asked on a host whose
// guest lacks its release, or has it and built another layer.

#include "oa_layer.hpp"
#include "oa_layer_check.hpp"

#include "oa/app/game_directory.hpp"
#include "oa/app/mutators_memory.hpp"
#include "oa/app/runtime.hpp"
#include "oa/data/mutator_library/library.hpp"
#include "oa/data/mutators/choice.hpp"
#include "oa/data/mutators/loadout.hpp"
#include "oa/formats/oamut/manifest.hpp"
#include "oa/netgame/presence.hpp"
#include "oa/netgame/records.hpp"
#include "oa/ui/first_run/first_run.hpp"
#include "oa/ui/frontend_dialogs.hpp"
#include "oa/ui/frontend_multiplayer/lobby.hpp"
#include "oa/ui/frontend_multiplayer/room_mutators.hpp"
#include "oa/ui/frontend_multiplayer/screens.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/mutator_picker/screen.hpp"

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

namespace fs = std::filesystem;
namespace kit = oa::ui::kit;
namespace mp = oa::ui::frontend_multiplayer;
namespace picker = oa::ui::mutator_picker;
namespace dialogs = oa::ui::frontend_dialogs;
namespace layout = oa::ui::display_layout;
namespace mutators = oa::data::mutators;
namespace first_run = oa::ui::first_run;

/// Stops the check.
///
/// @param ok the step held
/// @param what what failed
void require(bool ok, std::string_view what) {
    if (!ok)
        throw std::runtime_error("mutator prompts check: " + std::string(what));
}

/// Writes a file whole.
///
/// @param file the file
/// @param bytes its bytes
void write_text(const fs::path& file, std::string_view bytes) {
    if (!file.parent_path().empty())
        fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(out), "could not write " + path_to_utf8(file));
}

/// Writes one made-up mutator: a manifest and a script.
///
/// @param folder the mutator's folder
/// @param id its id
/// @param name the name players see
/// @param script its mutator.lua
void write_package(
    const fs::path& folder, std::string_view id, std::string_view name, std::string_view script
) {
    oa::formats::oamut::Manifest manifest;
    manifest.format = oa::formats::oamut::format_version;
    manifest.id = std::string(id);
    manifest.name = std::string(name);
    manifest.version.major = 1;
    manifest.version.parts = 1;
    manifest.author.name = "Ridge";
    manifest.description = "A made-up mutator.";
    manifest.tags = {"economy"};
    manifest.works_with.push_back(oa::formats::oamut::Condition{"*", {}, {}});
    manifest.layers = {oa::formats::oamut::Layer::files};
    write_text(folder / "oamut.yaml", oa::formats::oamut::write_manifest(manifest));
    write_text(folder / "mutator.lua", script);
}

/// The canonical text of one installed mutator.
///
/// @param folder the player's Mutators folder
/// @param id the mutator's id
/// @return that one mutator's loadout entry
mutators::LoadoutEntry entry_of(const fs::path& folder, std::string_view id) {
    mutators::MutatorChoice choice;
    choice.target = std::string(id);
    std::string error;
    const std::optional<mutators::ResolvedMutator> resolved =
        mutators::resolve_choice(choice, folder, &error);
    require(resolved.has_value(), error.empty() ? std::string(id) + " did not resolve" : error);
    const mutators::ResolvedMutator held = *resolved;
    const mutators::Loadout loadout =
        mutators::loadout_of(std::span<const mutators::ResolvedMutator>(&held, 1));
    require(loadout.entries.size() == 1, std::string(id) + " did not resolve to one entry");
    return loadout.entries.front();
}

/// The question showing now, or null.
///
/// @param runtime the runtime
/// @return the question
const kit::Question* question_of(OaLayer& layer) {
    LayerScreen* screen = layer.find(QuestionScreen::screen_name);
    if (screen == nullptr)
        return nullptr;
    return &static_cast<const QuestionScreen*>(screen)->question();
}

/// Joins a question's paragraphs.
///
/// @param question the question
/// @return its text
std::string body_of(const kit::Question& question) {
    std::string body;
    for (const kit::Paragraph& paragraph : question.paragraphs) {
        if (!body.empty())
            body += ' ';
        body += paragraph.text;
    }
    return body;
}

} // namespace

void Runtime::check_mutator_prompts() {
    require(sdl_.window != nullptr && sdl_.renderer != nullptr, "needs the SDL presenter");
    require(
        options_.user_folder && !options_.user_folder->empty() && !user_folder_.empty(),
        "the run has no folder of the player's own (--user-folder)"
    );
    require(static_cast<bool>(options_.preferences_file), "needs --preferences-file");
    require(!preference_path_.empty(), "the run has no preferences file");

    const fs::path mutators_folder =
        user_folder_ / std::string(oa::data::mutator_library::mutators_folder_name);
    write_package(
        mutators_folder / "lantern",
        "lantern",
        "Lantern",
        "function mutate(ctx)\n  return {}\nend\n"
    );
    write_package(
        mutators_folder / "slip",
        "slip",
        "Slip",
        "function mutate(ctx)\n  error(\"the ridge gave way\")\nend\n"
    );
    preference_values_[std::string(first_run::preference_key)] =
        std::string(first_run::preference_done);
    preferences_dirty_ = true;
    save_preferences();

    require(
        SDL_SetWindowSize(sdl_.window, 1280, 720) && SDL_SyncWindow(sdl_.window),
        "the window did not take 1280x720"
    );
    apply_output_mode();

    bool running = true;
    const auto frames = [&](int32_t count) {
        for (int32_t frame = 0; frame < count; ++frame) {
            run_frame(running);
            require(running, "a frame ended the run");
        }
    };
    const auto named = [this](std::string_view name) -> std::optional<AutomationControl> {
        std::vector<AutomationControl> controls;
        oa_layer().automation_controls(controls);
        for (const AutomationControl& control : controls)
            if (control.name == name && control.visible && control.window_width > 0 &&
                control.window_height > 0)
                return control;
        return std::nullopt;
    };
    const auto require_named = [&](std::string_view name, std::string_view why) {
        const std::optional<AutomationControl> control = named(name);
        require(control.has_value(), why);
        return *control;
    };
    const auto wait_named = [&](std::string_view name, std::string_view why) {
        for (int32_t frame = 0; frame < 30 && !named(name).has_value(); ++frame)
            frames(1);
        return require_named(name, why);
    };
    const auto pointer_at = [&](layout::Point pixel, SDL_EventType type, uint8_t button) {
        SDL_Event event = window_pointer_event(sdl_.window, sdl_.renderer, type, pixel, button);
        dispatch_event(event, running);
        require(running, "a pointer event ended the run");
    };
    const auto click_named = [&](std::string_view name) {
        const AutomationControl control = require_named(name, std::string(name) + " is not shown");
        const layout::Point pixel{
            control.window_x + control.window_width / 2,
            control.window_y + control.window_height / 2
        };
        pointer_at(pixel, SDL_EVENT_MOUSE_MOTION, 0);
        pointer_at(pixel, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT);
        pointer_at(pixel, SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_LEFT);
        frames(1);
    };
    const auto close_covering = [&] {
        dialogs::reset_dialogs();
        if (LayerScreen* notice = oa_layer().find(NoticeScreen::screen_name))
            oa_layer().close(notice);
        if (LayerScreen* question = oa_layer().find(QuestionScreen::screen_name))
            oa_layer().close(question);
        if (LayerScreen* welcome = oa_layer().find("first-run"))
            oa_layer().close(welcome);
        if (LayerScreen* library_screen = oa_layer().find("library"))
            oa_layer().close(library_screen);
        if (LayerScreen* picker_open = oa_layer().find(picker::screen_name))
            oa_layer().close(picker_open);
        frames(1);
    };
    const auto open_skirmish = [&] {
        load(Screen::main_menu);
        frames(2);
        close_covering();
        exercise_click(menu::resource_name(menu::Button::single_player));
        frames(1);
        exercise_click(entry::resource_name(entry::Button::skirmish));
        frames(2);
        close_covering();
        require(screen_ == Screen::skirmish, "SKIRMISH did not open");
    };
    const auto choose_both = [&] {
        std::vector<picker::Chosen> set;
        picker::Chosen lantern;
        lantern.id = "lantern";
        picker::Chosen slip;
        slip.id = "slip";
        set.push_back(std::move(lantern));
        set.push_back(std::move(slip));
        replace_setup_mutators(mutators_memory::Setup::skirmish, std::move(set));
    };
    const auto holds_id = [this](std::string_view id) {
        for (const picker::Chosen& one : skirmish_mutators_)
            if (one.id == id)
                return true;
        return false;
    };

    open_skirmish();
    choose_both();
    exercise_click(skirmish::resource_name(skirmish::Button::start));
    wait_named("oa.prompt.start-without", "START did not ask to start without Slip");
    const kit::Question* stopped = question_of(oa_layer());
    require(stopped != nullptr, "the start question is not on the layer");
    const std::string stopped_body = body_of(*stopped);
    require(
        stopped_body.find("Slip") != std::string::npos,
        "the start question does not name Slip (" + stopped_body + ")"
    );
    require(
        stopped->failure.find("the ridge gave way") != std::string::npos,
        "the failure line does not hold the reason (" + stopped->failure + ")"
    );
    std::cout << "mutator prompts check: START asks to start without Slip (" << stopped->failure
              << ")\n";
    click_named("oa.prompt.start-without");
    for (int32_t frame = 0; frame < 8 && screen_ != Screen::match; ++frame)
        frames(1);
    require(
        screen_ == Screen::match && match_ != nullptr,
        "START WITHOUT IT did not start the skirmish (" + status_ + ")"
    );
    bool lantern_handed = false;
    bool slip_handed = false;
    for (const mutators::ResolvedMutator& one : chosen_match_mutators()) {
        if (one.choice.target == "lantern")
            lantern_handed = true;
        if (one.choice.target == "slip")
            slip_handed = true;
    }
    require(lantern_handed && !slip_handed, "START WITHOUT IT did not start with Lantern only");
    std::cout << "mutator prompts check: START WITHOUT IT started with Lantern\n";

    leave_match();
    open_skirmish();
    choose_both();
    exercise_click(skirmish::resource_name(skirmish::Button::start));
    wait_named("oa.prompt.back", "the second start did not ask again");
    click_named("oa.prompt.back");
    require(
        screen_ == Screen::skirmish && match_ == nullptr, "BACK left SKIRMISH (" + status_ + ")"
    );
    require(holds_id("lantern") && holds_id("slip"), "BACK dropped a chosen mutator");
    std::cout << "mutator prompts check: BACK leaves SKIRMISH with Lantern and Slip\n";

    // The run's own list has no setup set to leave Slip out of: the start
    // keeps its message and asks nothing.
    {
        mutators::MutatorChoice choice;
        choice.target = "slip";
        std::string error;
        const std::optional<mutators::ResolvedMutator> slip_only =
            mutators::resolve_choice(choice, mutators_folder, &error);
        require(slip_only.has_value(), error.empty() ? "slip did not resolve" : error);
        load(Screen::main_menu);
        frames(1);
        close_covering();
        choose_match_mutators({*slip_only});
        bool entered = true;
        try {
            start_benchmark_skirmish_as_chosen();
        } catch (const std::exception&) {
            entered = false;
        }
        frames(1);
        require(!entered, "the run's own list started with Slip");
        require(
            question_of(oa_layer()) == nullptr,
            "a start with no setup set asked to start without Slip"
        );
        require(
            status_.find("Slip") != std::string::npos,
            "the start's message does not name Slip (" + status_ + ")"
        );
        choose_match_mutators({});
        std::cout << "mutator prompts check: the run's own list keeps the start's message\n";
    }

    mutators::Loadout remembered;
    remembered.entries.push_back(entry_of(mutators_folder, "lantern"));
    remembered.entries.push_back(entry_of(mutators_folder, "slip"));
    remember_mutators(mutators_memory::Setup::skirmish, remembered);
    mutators_memory::Crash crash;
    crash.setup = mutators_memory::Setup::skirmish;
    crash.loadout = remembered;
    {
        mutators_memory::Sentinel sentinel =
            mutators_memory::Sentinel::open_folder(preference_path_.parent_path());
        sentinel.write(crash);
    }
    mutator_crash_offered_ = false;
    open_mutator_sentinel();
    load(Screen::main_menu);
    frames(1);
    dialogs::reset_dialogs();
    if (LayerScreen* welcome = oa_layer().find("first-run"))
        oa_layer().close(welcome);
    wait_named("oa.prompt.forget-it", "the main menu did not ask to forget the set");
    std::cout << "mutator prompts check: the main menu asks to forget the last set\n";
    click_named("oa.prompt.forget-it");
    require(
        !remembered_mutators(mutators_memory::Setup::skirmish).has_value(),
        "FORGET IT left the skirmish set"
    );
    require(
        fs::is_directory(mutators_folder / "lantern") && fs::is_directory(mutators_folder / "slip"),
        "FORGET IT removed a mutator"
    );
    std::cout << "mutator prompts check: FORGET IT emptied the set and uninstalled nothing\n";

    mutators_memory::Crash own_list;
    own_list.loadout = remembered;
    {
        mutators_memory::Sentinel sentinel =
            mutators_memory::Sentinel::open_folder(preference_path_.parent_path());
        sentinel.write(own_list);
    }
    mutator_crash_offered_ = false;
    open_mutator_sentinel();
    load(Screen::main_menu);
    frames(1);
    dialogs::reset_dialogs();
    if (LayerScreen* welcome = oa_layer().find("first-run"))
        oa_layer().close(welcome);
    for (int32_t frame = 0; frame < 8; ++frame)
        frames(1);
    require(question_of(oa_layer()) == nullptr, "a crash with no setup asked to forget the set");
    require(!named("oa.prompt.forget-it").has_value(), "a crash with no setup showed FORGET IT");
    std::cout << "mutator prompts check: a crash with no setup asks nothing\n";

    close_covering();
    install_mutator_picker_check_feed();
    bool used = false;
    std::vector<picker::Chosen> handed;
    {
        MutatorPickerRequest request;
        request.setup = picker::Setup::skirmish;
        picker::Chosen grounded;
        grounded.id = "grounded";
        request.set.push_back(std::move(grounded));
        request.used = [&](std::vector<picker::Chosen> set) {
            handed = std::move(set);
            used = true;
        };
        require(open_mutator_picker(std::move(request)), "the picker did not open");
    }
    frames(1);
    require(oa_layer().find(picker::screen_name) != nullptr, "the picker is not on the layer");
    click_named("oa.mutators.use");
    wait_named("oa.prompt.cancel", "USE THESE did not ask to get mutators");
    const kit::Question* getting = question_of(oa_layer());
    require(getting != nullptr, "Get mutators is not on the layer");
    require(
        body_of(*getting).find("Grounded") != std::string::npos,
        "Get mutators does not name Grounded"
    );
    require(!used, "Get mutators handed the set before Grounded was installed");
    std::cout << "mutator prompts check: USE THESE asks to get Grounded\n";
    fail_mutator_picker_check_download("grounded", "the ridge did not answer.");
    wait_named("oa.prompt.retry", "the stopped download did not ask RETRY");
    click_named("oa.prompt.retry");
    require(mutator_picker_check_retried("grounded"), "RETRY did not ask the feed to retry");
    require(!used, "RETRY handed the set");
    std::cout << "mutator prompts check: RETRY asks for Grounded again\n";
    mark_mutator_picker_check_installed("grounded");
    for (int32_t frame = 0; frame < 8 && oa_layer().find(picker::screen_name) != nullptr; ++frame)
        frames(1);
    require(used, "finishing Grounded did not hand the set");
    require(
        oa_layer().find(picker::screen_name) == nullptr, "finishing Grounded left the picker open"
    );
    require(
        handed.size() == 1 && handed.front().id == "grounded", "the handed set is not Grounded"
    );
    std::cout << "mutator prompts check: Grounded installed and the set was handed\n";

    // "The game did not start". Only the host's START is ever refused, so
    // this machine hosts the room as Kai and plays Lantern, release 1. Ren,
    // an Open Annihilation guest in slot 1, sends what a guest's record
    // carries: whether it has each mutator of the list, and no release. The
    // refusal is the block START reads. No registry offers Lantern here.
    {
        mutators::MutatorChoice choice;
        choice.target = "lantern";
        const std::optional<mutators::ResolvedMutator> lantern =
            mutators::resolve_choice(choice, mutators_folder, nullptr);
        require(lantern.has_value(), "Lantern did not resolve for the refused start");
        set_room_mutators({*lantern});
    }
    mp::Lobby& lobby = mp::multiplayer_lobby();
    oa::Game* const previous_game = lobby.game;
    static oa::Game game{};
    constexpr uint32_t host_id = 0x100;
    constexpr uint32_t guest_id = 0x200;
    constexpr uint8_t none = oa::netgame::presence_mutator_index_none;
    // A mutator field for the room's one-mutator list. A built field carries
    // the layer digest its build made.
    const auto field_of = [](bool built, uint8_t layer, uint8_t index) {
        oa::netgame::PresenceMutators field;
        field.built = built;
        field.index = index;
        field.total = 1;
        field.held = 1;
        field.digest.fill(0x11);
        if (built)
            field.layer_digest.fill(layer);
        return field;
    };
    const auto record_of = [](const oa::netgame::PresenceMutators& field) {
        oa::netgame::PresenceRecord record;
        record.mutators = field;
        std::vector<uint8_t> bytes(oa::netgame::presence_record_max_bytes);
        std::size_t written = 0;
        require(
            oa::netgame::encode_presence(record, bytes.data(), bytes.size(), &written) ==
                oa::netgame::WireError::ok,
            "a presence record did not encode"
        );
        bytes.resize(written);
        return bytes;
    };
    const auto ask = [&](const oa::netgame::PresenceMutators& rens, mp::MutatorsHold expected) {
        if (LayerScreen* open = oa_layer().find(QuestionScreen::screen_name))
            oa_layer().close(open);
        mp::lobby_reset(lobby, game);
        game.players[0].player_id = host_id;
        mp::lobby_seat_local(lobby, 0, true, "Kai");
        auto& ren = mp::slot_player(lobby, 1);
        ren.in_use = 1;
        ren.player_id = guest_id;
        ren.index = 1;
        constexpr std::string_view ren_name = "Ren";
        require(ren_name.size() < sizeof ren.name, "Ren's name does not fit");
        std::memcpy(ren.name, ren_name.data(), ren_name.size());
        ren.name[ren_name.size()] = '\0';
        mp::slot_set_status(lobby, ren, mp::kSlotRemote);
        mp::PlayerSetupInfo* info = mp::slot_info(lobby, 1);
        require(info != nullptr, "Ren has no setup block");
        info->state = mp::kInfoStatePlaying;
        info->net_id = guest_id;
        info->engine_signature[0] = oa::netgame::engine_signature_first;
        info->engine_signature[1] = oa::netgame::engine_signature_second;
        lobby.presence_records.block_player[1] = guest_id;
        const std::vector<uint8_t> hosts = record_of(field_of(true, 0x22, none));
        std::memcpy(lobby.presence_records.local, hosts.data(), hosts.size());
        lobby.presence_records.local_size = static_cast<uint16_t>(hosts.size());
        const std::vector<uint8_t> guests = record_of(rens);
        std::memcpy(lobby.presence_records.peer[1], guests.data(), guests.size());
        lobby.presence_records.peer_size[1] = static_cast<uint16_t>(guests.size());
        lobby.presence_records.peer_id[1] = guest_id;
        require(mp::lobby_hosts(lobby), "this machine does not host the room");
        const mp::MutatorsBlock block = mp::lobby_mutators_block(lobby);
        require(
            block.hold == expected && block.slot == 1 && block.position == 0,
            "START was not held on Ren and Lantern"
        );
        mp::lobby_note_refused_start(block);
        offer_differing_results_question();
        const kit::Question* asked = question_of(oa_layer());
        require(asked != nullptr, "The game did not start was not asked");
        const std::string failure = asked->failure;
        const std::string body = body_of(*asked);
        if (LayerScreen* open = oa_layer().find(QuestionScreen::screen_name))
            oa_layer().close(open);
        return std::pair<std::string, std::string>{failure, body};
    };
    // Ren's record says only that another release is installed, or none.
    // Kai cannot name Ren's release, and no registry offers Kai's, so the
    // line is the one without a release and no paragraph names one.
    for (const mp::MutatorHave have : {mp::MutatorHave::other_release, mp::MutatorHave::missing}) {
        oa::netgame::PresenceMutators rens = field_of(false, 0, none);
        rens.lacking = true;
        oa::netgame::presence_mutator_set_state(rens.states, 0, static_cast<uint8_t>(have));
        const auto lacking = ask(rens, mp::MutatorsHold::lacking);
        require(
            lacking.first == "Lantern: Ren does not have it.",
            "a guest without the host's release was given a release (" + lacking.first + ")"
        );
        require(
            lacking.second.find("release") == std::string::npos,
            "a release no registry offers was named (" + lacking.second + ")"
        );
    }
    std::cout << "mutator prompts check: a guest without the host's release is named without one\n";
    // Ren has the host's release and built another layer: both machines
    // play release 1, and GET MUTATORS has nothing to fetch.
    const auto same = ask(field_of(true, 0x33, 0), mp::MutatorsHold::other_result);
    require(
        same.first == "Lantern: release 1 here, release 1 on Ren's machine.",
        "a guest with the host's release did not show the release line (" + same.first + ")"
    );
    require(
        same.second.find("GET MUTATORS") == std::string::npos,
        "a guest with the host's release was told to fetch it (" + same.second + ")"
    );
    std::cout << "mutator prompts check: a guest with the host's release shows the release line\n";
    lobby.game = previous_game;
}

} // namespace oa::app
