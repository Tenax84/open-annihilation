// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// CD music: numbered music files play as the game disc; the CD player,
// session and mood run on them from the menus through a match.
#include "oa/app/runtime.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/app/view_rules.hpp"
#include "oa/audio/music_session.hpp"
#include "oa/audio/sdl_music.hpp"
#include "oa/platform/dataless.hpp"
#include "oa/ui/services/timers.hpp"
#include "oa/ui/frontend/options.hpp"
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace oa::app {

namespace {

namespace audio = oa::audio;

constexpr uint32_t engine_clock_rate = 30; // engine ticks per second
constexpr std::string_view track_types_key = "CDLISTS";
constexpr std::string_view no_disc_text = "NO DISC";
// The ALLSOUND section the victory announcement plays.
constexpr std::string_view victory_announcement_sound = "Victory Condition";
constexpr uint8_t cd_mode_by_kind = static_cast<uint8_t>(audio::CdPlayMode::by_kind);
// The rand() generator: x = x * 214013 + 2531011, drawing bits 16..30.
constexpr uint32_t lcg_multiplier = 214013U;
constexpr uint32_t lcg_increment = 2531011U;

char hex_digit(uint8_t nibble) {
    return static_cast<char>(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
}

int hex_value(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

bool music_enabled(const oa::Game& game) {
    return (game.music_flags & audio::music_flag_enabled) != 0;
}

} // namespace

struct Runtime::MusicHost {
    audio::SdlMusicDevice* device{};
    std::unique_ptr<audio::Mixer> mixer = std::make_unique<audio::Mixer>();
    std::unique_ptr<oa::Game> game = std::make_unique<oa::Game>();
    audio::MusicSession session{};
    oa::ui::services::EngineClock clock{oa::ui::services::host_clock(), engine_clock_rate};
    oa::ui::services::TimerTable timers{};
    uint32_t random_state{};
    bool in_match{};
    bool menu_paused{};
    bool on_panel{};
    // Files macOS has moved out of the player's folders, fetched without
    // holding startup.
    oa::platform::EvictedFetch evicted;

    // CDLISTS is saved as kinds change; the runtime's settings may already
    // be gone here.
    ~MusicHost() {
        audio::mixer_restore_volumes(*mixer);
        audio::cd_close(*mixer);
        audio::sdl_music_device_destroy(device);
    }
};

void Runtime::destroy_music_host(MusicHost* host) noexcept {
    delete host;
}

void Runtime::music_start() {
    if (music_ || options_.mute || options_.headless_check)
        return;
    music_.reset(new MusicHost{});
    auto& host = *music_;
    host.random_state = static_cast<uint32_t>(SDL_GetTicks());
    oa::ui::services::timers_reset(&host.timers, &host.clock);

    // The profile's display rules may take the disc's tracks from numbered
    // MP3 files from 1.mp3 or from every MP3 file of the folder instead.
    const auto source = view_rules::music_source(ui_rules());
    const auto scan = [source](const fs::path& directory) {
        switch (source) {
        case view_rules::MusicSource::numbered_mp3:
            return audio::music_disc_scan_numbered(directory);
        case view_rules::MusicSource::folder_scan:
            return audio::music_disc_scan_folder(directory);
        case view_rules::MusicSource::disc:
            break;
        }
        return audio::music_disc_scan(directory);
    };
    // The first game folder with a music folder plays it, a mod folder's
    // before the game folder's.
    fs::path music_folder = options_.game_dir;
    for (const auto& root : assets_.loose_roots())
        if (audio::music_disc_present(scan(audio::music_disc_directory(root)))) {
            music_folder = root;
            break;
        }
    const auto disc = scan(audio::music_disc_directory(music_folder));
    if (!audio::music_disc_present(disc))
        std::cerr << "music: no tracks in " << path_to_utf8(disc.directory)
                  << "; CD music is silent\n";
    else if (!audio::sdl_music_decoder_available())
        std::cerr << "music: this build cannot decode music files\n";
    host.device = audio::sdl_music_device_create(disc);
    // The player's folders were asked for at startup. The music folder is
    // known only once a game folder is mounted.
    host.evicted.start({audio::music_disc_directory(music_folder)}, "in the music folder");

    auto& mixer = *host.mixer;
    mixer.music = audio::sdl_music_device(host.device);
    mixer.timers.context = &host;
    mixer.timers.add =
        [](void* context, uint32_t interval, audio::TimerCallback callback, void* user) {
            auto& owner = *static_cast<MusicHost*>(context);
            return oa::ui::services::timers_add(
                &owner.timers, &owner.clock, static_cast<int32_t>(interval), user, callback
            );
        };
    // A handle that names no timer has nothing to free.
    mixer.timers.remove = [](void* context, int32_t handle) {
        std::ignore =
            oa::ui::services::timers_remove(&static_cast<MusicHost*>(context)->timers, handle);
    };
    mixer.random.context = &host;
    mixer.random.next = [](void* context) {
        auto& state = static_cast<MusicHost*>(context)->random_state;
        state = state * lcg_multiplier + lcg_increment;
        return static_cast<int32_t>((state >> 16) & 0x7fffU);
    };
    audio::mixer_init(mixer);

    auto& game = *host.game;
    game.music_volume = preferences_.music_volume;
    game.music_flags = preferences_.music_flags;
    game.cd_mode = preferences_.cd_mode;
    game.local_player_index = match_view_player();

    auto& session = host.session;
    session.mixer = &mixer;
    session.game = &game;
    session.settings.context = this;
    session.settings.read_blob = [](void* context, const char*, uint8_t* bytes, uint32_t* size) {
        auto& runtime = *static_cast<Runtime*>(context);
        const auto text = runtime.read_string(
            init::general_section, track_types_key, static_cast<std::size_t>(*size) * 2 + 1
        );
        if (!text || text->size() != static_cast<std::size_t>(*size) * 2)
            return false;
        for (uint32_t i = 0; i < *size; ++i) {
            const int high = hex_value((*text)[2 * i]);
            const int low = hex_value((*text)[2 * i + 1]);
            if (high < 0 || low < 0)
                return false;
            bytes[i] = static_cast<uint8_t>(high << 4 | low);
        }
        return true;
    };
    session.settings.write_blob =
        [](void* context, const char*, const uint8_t* bytes, uint32_t size) {
            auto& runtime = *static_cast<Runtime*>(context);
            std::string text;
            text.reserve(static_cast<std::size_t>(size) * 2);
            for (uint32_t i = 0; i < size; ++i) {
                text.push_back(hex_digit(static_cast<uint8_t>(bytes[i] >> 4)));
                text.push_back(hex_digit(static_cast<uint8_t>(bytes[i] & 0xfU)));
            }
            runtime.write_string(init::general_section, track_types_key, text);
            runtime.flush_preferences();
        };
    if (!audio::music_session_start(session))
        std::cerr << "music device unavailable: " << audio::sdl_music_device_error(host.device)
                  << '\n';
}

void Runtime::music_main_menu() {
    music_start();
    if (!music_)
        return;
    auto& host = *music_;
    if (host.in_match) {
        audio::music_session_end_match(host.session);
        audio::cd_save_disc_cache(host.session.cache, *host.mixer, host.session.settings);
        host.in_match = false;
        host.menu_paused = false;
    }
    audio::music_session_main_menu(host.session);
}

bool Runtime::music_foreign_player() const {
    return music_ && audio::cd_foreign_player_present(*music_->mixer);
}

void Runtime::music_close_foreign_player() {
    if (music_)
        audio::cd_close_foreign_player(*music_->mixer);
}

bool Runtime::music_no_driver() const {
    return music_ && audio::mixer_no_driver(*music_->mixer);
}

void Runtime::music_begin_match() {
    music_start();
    if (!music_)
        return;
    auto& host = *music_;
    host.game->local_player_index = match_view_player();
    audio::music_session_begin_match(host.session);
    host.in_match = true;
    host.menu_paused = false;
}

void Runtime::music_end_game() {
    if (!music_ || !ui_rules().audio.enabled)
        return;
    std::ignore = audio::music_set_paused(music_->session, true);
}

void Runtime::announce_victory() {
    if (!match_ || !ui_rules().audio.enabled)
        return;
    if (view_rules::victory_announcement_due(match_->state().game.tick, victory_banner_tick_))
        play_match_interface_sound(victory_announcement_sound);
}

void Runtime::apply_saved_volumes() {
    apply_saved_gamma();
    audio_player_.set_volume(wave_volume_, preferences_.fx_volume);
    if (!music_)
        return;
    music_->game->music_volume = preferences_.music_volume;
    audio::music_apply_volume(music_->session);
}

void Runtime::step_music() {
    if (!music_)
        return;
    auto& host = *music_;
    auto& session = host.session;
    auto& game = *host.game;
    oa::ui::services::timers_tick(&host.timers, &host.clock);
    if (audio::sdl_music_device_poll_complete(host.device))
        audio::cd_on_play_complete(*host.mixer, true);

    // Preference changes made by the options screens (slider, UNDO, CANCEL).
    if (game.music_volume != preferences_.music_volume) {
        game.music_volume = preferences_.music_volume;
        audio::music_apply_volume(session);
    }
    if (game.music_flags != preferences_.music_flags) {
        const bool was_enabled = music_enabled(game);
        game.music_flags = preferences_.music_flags;
        audio::cd_set_enabled(*host.mixer, game.music_flags & audio::music_flag_enabled);
        if (!was_enabled && music_enabled(game))
            audio::cd_advance(*host.mixer);
    }
    if (game.cd_mode != preferences_.cd_mode) {
        game.cd_mode = preferences_.cd_mode;
        audio::cd_set_play_mode(*host.mixer, game.cd_mode);
    }

    // The music panel is the Music screen, or the MUSIC tab of the
    // preferences a match opens; leaving either resumes the music.
    const bool on_panel = screen_ == Screen::music || match_music_panel_open();
    if (host.on_panel && !on_panel)
        audio::music_panel_leave(session);
    host.on_panel = on_panel;

    if (!host.in_match || !match_)
        return;
    auto& match = *match_;
    if (match.combat_activity.context != &host) {
        match.combat_activity.context = &host;
        match.combat_activity.hit = [](void* context, uint8_t attacker, uint8_t target) {
            audio::music_note_hit(static_cast<MusicHost*>(context)->session, attacker, target);
        };
        match.combat_activity.kill = [](void* context, uint8_t killer) {
            audio::music_note_kill(static_cast<MusicHost*>(context)->session, killer);
        };
    }
    // The ARMOPT menu (and screens opened from it) holds the game and pauses
    // the music until it closes.
    const bool menu_open = match_paused_ && !match_finished_;
    if (menu_open != host.menu_paused) {
        audio::music_set_paused(session, menu_open);
        host.menu_paused = menu_open;
    }
    if (match_paused_)
        return;
    game.local_player_index = match_view_player();
    if (match_view_player() < match.world().players.size())
        game.players[match_view_player()].unit_count =
            match.world().players[match_view_player()].current_count;
    audio::music_session_tick(session, match_timing_.tick);
}

void Runtime::hold_music_while_inactive() {
    // 3.1c closes the CD player as it goes inactive, and a new track starts
    // when it is active again; the hold plays on from where it stopped.
    if (music_)
        audio::sdl_music_device_hold(music_->device, !application_active_);
}

bool Runtime::music_cd_play(int32_t track) {
    music_start();
    return music_ && audio::music_command_cd_play(music_->session, track);
}

bool Runtime::music_cd_stop() {
    music_start();
    return music_ && audio::music_command_cd_stop(music_->session);
}

void Runtime::music_mode(int32_t kind) {
    music_start();
    if (music_)
        audio::music_command_music_mode(music_->session, kind);
}

namespace {

/// Refreshes the music panel's track display.
///
/// TRACKNUM shows the panel track or NO DISC, TRACKTYPE its kind (editable
/// only with music on in by-kind mode); in selected mode the shown track
/// becomes the playlist track.
///
/// @param[in,out] panel music panel
/// @param[in,out] session music session whose panel track is shown
void refresh_music_panel(ui::frontend::Panel& panel, audio::MusicSession& session) {
    const auto& game = *session.game;
    const bool on = music_enabled(game);
    ui::frontend::panel_set_grayed(panel, "TRACKTYPE", !(on && game.cd_mode == cd_mode_by_kind));
    ui::frontend::panel_set_stage(
        panel, "TRACKTYPE", audio::cd_track_type(*session.mixer, session.panel_track)
    );
    const std::string number = std::to_string(session.panel_track);
    ui::frontend::panel_set_text(
        panel, "TRACKNUM", session.panel_track == 0 ? no_disc_text : number
    );
    ui::frontend::panel_set_disabled(panel, "TRACKNUM", !on);
    audio::music_panel_sync_playlist(session);
}

} // namespace

void Runtime::music_panel_entered(
    ui::frontend::Panel& panel, const ui::frontend::OptionsContext& context
) {
    music_start();
    if (!music_)
        return;
    auto& session = music_->session;
    audio::music_panel_enter(session);
    ui::frontend::options_update_cd_controls(panel, context);
    refresh_music_panel(panel, session);
}

bool Runtime::music_panel_clicked(
    ui::frontend::Panel& panel, const ui::frontend::OptionsContext& context
) {
    if (!music_ || panel.selected < 1 || panel.selected > panel.count)
        return false;
    auto& host = *music_;
    auto& session = host.session;
    auto& game = *host.game;
    const auto& control = panel.controls[static_cast<std::size_t>(panel.selected)];
    const std::string_view name = ui::frontend::control_name(control);
    if (name == "NOTRAK") {
        audio::music_panel_set_enabled(session, control.stage != 0);
    } else if (name == "TRACKMODE") {
        const auto* type = ui::frontend::panel_control(panel, "TRACKTYPE");
        audio::music_panel_set_mode(session, control.stage, type != nullptr ? type->stage : 0);
    } else if (name == "TRACKTYPE") {
        audio::music_panel_set_track_type(session, control.stage);
        audio::cd_save_disc_cache(session.cache, *host.mixer, session.settings);
    } else if (name == "CDPLAY") {
        audio::music_panel_play(session);
    } else if (name == "CDSTOP") {
        audio::music_panel_stop(session);
    } else if (name == "CDNEXT" || name == "CDPREV") {
        audio::music_panel_step(session, name == "CDNEXT");
    } else if (name == "RESTORE") {
        audio::music_panel_restore(session);
        preferences_.music_volume = game.music_volume;
        if (auto* volume = ui::frontend::panel_control(panel, "MUSICVOL"))
            ui::frontend::slider_set_value(volume->slider, static_cast<int32_t>(game.music_volume));
    } else {
        return false;
    }
    preferences_.music_flags = game.music_flags;
    preferences_.cd_mode = game.cd_mode;
    ui::frontend::options_update_cd_controls(panel, context);
    refresh_music_panel(panel, session);
    panel.selected = ui::frontend::kNoSelection;
    return true;
}

} // namespace oa::app
