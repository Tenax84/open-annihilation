// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "audio_test_support.hpp"
#include "oa/audio/cd_music.hpp"
#include "oa/audio/music_decoder.hpp"
#include "oa/audio/music_disc.hpp"
#include "oa/audio/sdl_music.hpp"
#include "oa/test/game_data.hpp"
#include "oa/test/scratch_directory.hpp"

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace oa::audio;
using audio_test::require;

namespace {

void write_tone(const std::filesystem::path& path, uint32_t frames) {
    std::vector<uint8_t> samples;
    for (uint32_t i = 0; i < frames; ++i)
        audio_test::put16(samples, static_cast<uint16_t>((i % 50) < 25 ? 3000 : -3000));
    const auto riff = audio_test::make_riff(22050, 16, 1, samples);
    std::ofstream(path, std::ios::binary)
        .write(
            reinterpret_cast<const char*>(riff.data()), static_cast<std::streamsize>(riff.size())
        );
}

// Pumps until `done` holds or the timeout (ms) passes.
template <typename Done>
bool wait_for(Done done, uint64_t timeout_ms) {
    const uint64_t start = SDL_GetTicks();
    while (SDL_GetTicks() - start < timeout_ms) {
        if (done())
            return true;
        SDL_Delay(5);
    }
    return done();
}

std::unique_ptr<Mixer> make_mixer(SdlMusicDevice* device, audio_test::FakeTimers& timers) {
    auto mixer = std::make_unique<Mixer>();
    mixer->music = sdl_music_device(device);
    mixer->timers = timers.table();
    mixer_init(*mixer);
    return mixer;
}

void generated_disc() {
    // Every music file, WAV included, plays through the decoder.
    if (!sdl_music_decoder_available())
        oa::test::skip_test("the generated music", "this build has no music decoder");
    const auto root = oa::test::make_scratch_directory("oa-sdl-music-test");
    std::filesystem::create_directories(root);
    write_tone(root / "2.wav", 22050 / 2);
    write_tone(root / "3.wav", 22050 * 2);
    const MusicDisc disc = music_disc_scan(root);
    require(disc.track_count == 3, "generated disc has a data track and two audio tracks");

    SdlMusicDevice* device = sdl_music_device_create(disc);
    audio_test::FakeTimers timers;
    auto mixer = make_mixer(device, timers);
    require(
        mixer->aux_device == 0 && mixer->music_volume == max_device_volume,
        "music line found at full volume"
    );
    require(cd_open(*mixer), sdl_music_device_error(device));
    require(cd_track_count(*mixer) == 2 && mixer->cd.first_track_offset == 1, "data track skipped");
    cd_set_play_mode(*mixer, static_cast<int32_t>(CdPlayMode::sequential));
    mixer_set_music_volume(*mixer, 0x8000, false);

    // Track 1 plays disc track 2 through to the disc end, crossing into 3.
    require(cd_play_track(*mixer, 1), sdl_music_device_error(device));
    require(cd_is_playing(*mixer), "playing");
    int32_t device_track = 0;
    require(
        wait_for(
            [&] {
                mixer->music.current_track(mixer->music.context, &device_track);
                return device_track == 3;
            },
            5000
        ),
        "range continues into the next file"
    );

    require(cd_set_paused(*mixer, true) && !cd_is_playing(*mixer), "paused");
    require(!sdl_music_device_poll_complete(device), "no completion while paused");
    require(cd_set_paused(*mixer, false) && cd_is_playing(*mixer), "resumed");

    // The hold keeps the track where it is, through the player's own pause
    // and resume, and lets it play on from there.
    sdl_music_device_hold(device, true);
    const double held_at = sdl_music_device_position(device);
    SDL_Delay(150);
    require(sdl_music_device_position(device) == held_at, "held where it is");
    require(cd_is_playing(*mixer), "a held track still counts as playing");
    require(!sdl_music_device_poll_complete(device), "no completion while held");
    require(cd_set_paused(*mixer, true) && cd_set_paused(*mixer, false), "paused and resumed held");
    SDL_Delay(150);
    require(sdl_music_device_position(device) == held_at, "a resume leaves the hold");
    sdl_music_device_hold(device, false);
    require(
        wait_for([&] { return sdl_music_device_position(device) > held_at; }, 5000),
        "plays on from where it was held"
    );

    require(
        wait_for([&] { return sdl_music_device_poll_complete(device); }, 5000),
        "completion reported at the disc end"
    );
    require(!cd_is_playing(*mixer), "stopped at the end");
    cd_on_play_complete(*mixer, true);
    require(
        cd_is_playing(*mixer) && cd_current_track(*mixer) == 2,
        "completion advances sequential play"
    );
    require(cd_stop(*mixer) && !cd_is_playing(*mixer), "stop");
    require(!sdl_music_device_poll_complete(device), "a stopped range reports nothing");

    // A track played during the hold starts silent and plays when it ends.
    sdl_music_device_hold(device, true);
    require(cd_play_track(*mixer, 1) && cd_is_playing(*mixer), "played while held");
    SDL_Delay(150);
    require(sdl_music_device_position(device) == 0.0, "a track played while held starts silent");
    sdl_music_device_hold(device, false);
    require(
        wait_for([&] { return sdl_music_device_position(device) > 0.0; }, 5000),
        "the track plays once the hold ends"
    );
    require(cd_stop(*mixer), "stopped after the hold");
    mixer_teardown(*mixer);
    sdl_music_device_destroy(device);
    std::filesystem::remove_all(root);
}

// The installed game's music folder: its sixteen tracks open and decode. An
// install that plays its music from the disc has no such folder.
void installed_disc(const std::filesystem::path& game_dir) {
    if (!sdl_music_decoder_available())
        oa::test::skip_test("the installed music", "this build has no music decoder");
    const auto directory = music_disc_directory(game_dir);
    const MusicDisc disc = music_disc_scan(directory);
    if (!music_disc_present(disc))
        oa::test::skip_test("the installed music", "the install has no music folder");
    require(disc.track_count == 17, "installed music: tracks 2..17");
    SdlMusicDevice* device = sdl_music_device_create(disc);
    audio_test::FakeTimers timers;
    auto mixer = make_mixer(device, timers);
    require(cd_open(*mixer), sdl_music_device_error(device));
    require(cd_track_count(*mixer) == 16, "sixteen playable tracks");
    for (int32_t track : {1, 16}) {
        require(cd_play_track(*mixer, track), sdl_music_device_error(device));
        require(
            wait_for([&] { return sdl_music_device_position(device) > 0.2; }, 5000),
            "installed track decodes"
        );
    }
    std::printf(
        "installed music: %s, %d tracks, id %08x\n",
        disc.directory.string().c_str(),
        disc.track_count,
        static_cast<unsigned>(disc.disc_id)
    );
    mixer_teardown(*mixer);
    sdl_music_device_destroy(device);
}

// A read of a music file that waits until the test lets it go, as a file
// iCloud is still downloading does. It gives up after slow_read_limit_ms,
// so a play that read the file on the calling thread returns late instead
// of never.
constexpr uint64_t slow_read_limit_ms = 3000;
// A call that returns within this did not wait for the read, which takes
// slow_read_limit_ms; the margin is for a busy test machine.
constexpr uint64_t no_wait_ms = 100;

struct SlowGate {
    std::atomic<int> entered{0};
    std::atomic<bool> open{false};
};

void slow_read(void* context) {
    auto& gate = *static_cast<SlowGate*>(context);
    gate.entered.fetch_add(1, std::memory_order_relaxed);
    const uint64_t start = SDL_GetTicks();
    while (!gate.open.load(std::memory_order_acquire) &&
           SDL_GetTicks() - start < slow_read_limit_ms)
        SDL_Delay(5);
}

struct ClearDelay {
    ~ClearDelay() { music_set_read_delay(nullptr, nullptr); }
};

// Runs `work` and tells whether it returned without waiting for a read.
template <typename Work>
bool without_waiting(Work work) {
    const uint64_t start = SDL_GetTicks();
    work();
    return SDL_GetTicks() - start < no_wait_ms;
}

// One frame's music work, as Runtime::step_music does it: the completion
// poll, the playing state and the position.
void music_frame(Mixer& mixer, SdlMusicDevice* device) {
    if (sdl_music_device_poll_complete(device))
        cd_on_play_complete(mixer, true);
    (void)cd_is_playing(mixer);
    (void)sdl_music_device_position(device);
}

// A match start, a track change and a stop while a music file is still
// being read: none waits for the read, the frames go on, and the music
// starts once the read ends. The device can then be destroyed while a read
// is still waiting.
void slow_track() {
    if (!sdl_music_decoder_available())
        oa::test::skip_test("a slow music file", "this build has no music decoder");
    const auto root = oa::test::make_scratch_directory("oa-sdl-music-slow-test");
    std::filesystem::create_directories(root);
    write_tone(root / "2.wav", 22050 / 2);
    write_tone(root / "3.wav", 22050 / 2);
    const MusicDisc disc = music_disc_scan(root);
    require(disc.track_count == 3, "slow disc has a data track and two audio tracks");
    SdlMusicDevice* device = sdl_music_device_create(disc);
    audio_test::FakeTimers timers;
    auto mixer = make_mixer(device, timers);
    require(cd_open(*mixer), sdl_music_device_error(device));
    const ClearDelay clear;

    SlowGate gate;
    music_set_read_delay(slow_read, &gate);
    const uint64_t started = SDL_GetTicks();
    require(
        without_waiting([&] { require(cd_play_track(*mixer, 1), "play returns"); }),
        "a match start does not wait for the music file"
    );
    require(cd_is_playing(*mixer), "a track still being read counts as playing");
    int frames = 0;
    bool frames_on_time = true;
    while (gate.entered.load(std::memory_order_relaxed) == 0 && SDL_GetTicks() - started < 2000) {
        frames_on_time = without_waiting([&] { music_frame(*mixer, device); }) && frames_on_time;
        ++frames;
        SDL_Delay(5);
    }
    require(gate.entered.load(std::memory_order_relaxed) > 0, "a read of the track started");
    const int read_from = frames;
    while (frames < read_from + 10) {
        frames_on_time = without_waiting([&] { music_frame(*mixer, device); }) && frames_on_time;
        ++frames;
        SDL_Delay(5);
    }
    // A track change while the first file is still being read.
    require(
        without_waiting([&] { require(cd_play_track(*mixer, 2), "track change returns"); }),
        "a track change does not wait for the music file"
    );
    while (frames < read_from + 20) {
        frames_on_time = without_waiting([&] { music_frame(*mixer, device); }) && frames_on_time;
        ++frames;
        SDL_Delay(5);
    }
    require(frames_on_time, "no frame's music work waits for the read");
    require(SDL_GetTicks() - started < slow_read_limit_ms, "the read was still waiting");
    require(sdl_music_device_position(device) == 0.0, "no music before the file is read");
    require(cd_is_playing(*mixer), "the changed track counts as playing while it is read");
    gate.open.store(true, std::memory_order_release);
    require(
        wait_for([&] { return sdl_music_device_position(device) > 0.0; }, 5000),
        "the music starts after the read"
    );
    int32_t device_track = 0;
    mixer->music.current_track(mixer->music.context, &device_track);
    require(device_track == 3, "the track changed to plays, not the one being read before");

    // A stop, as a match ends, while a read waits.
    SlowGate stop_gate;
    music_set_read_delay(slow_read, &stop_gate);
    require(cd_play_track(*mixer, 1), "play before the stop");
    require(
        wait_for([&] { return stop_gate.entered.load(std::memory_order_relaxed) > 0; }, 2000),
        "a read before the stop started"
    );
    require(
        without_waiting([&] { require(cd_stop(*mixer), "stop returns"); }),
        "a stop does not wait for the music file"
    );
    require(!cd_is_playing(*mixer), "stopped while the read waits");

    // Destroyed while a read still waits: the device waits for the read's
    // thread, which the gate lets go a little later.
    require(cd_play_track(*mixer, 2), "play before the device is destroyed");
    std::thread opener([&] {
        SDL_Delay(100);
        stop_gate.open.store(true, std::memory_order_release);
    });
    mixer_teardown(*mixer);
    sdl_music_device_destroy(device);
    opener.join();
    std::filesystem::remove_all(root);
}

// A track that cannot be read stops, as before, so the music session's
// next tick picks another; a later track plays.
void unreadable_track() {
    if (!sdl_music_decoder_available())
        oa::test::skip_test("an unreadable music file", "this build has no music decoder");
    const auto root = oa::test::make_scratch_directory("oa-sdl-music-unreadable-test");
    std::filesystem::create_directories(root);
    write_tone(root / "2.wav", 22050 / 2);
    write_tone(root / "3.wav", 22050 / 2);
    const MusicDisc disc = music_disc_scan(root);
    require(disc.track_count == 3, "unreadable disc has a data track and two audio tracks");
    std::filesystem::remove(root / "2.wav");
    SdlMusicDevice* device = sdl_music_device_create(disc);
    audio_test::FakeTimers timers;
    auto mixer = make_mixer(device, timers);
    require(cd_open(*mixer), sdl_music_device_error(device));
    (void)cd_play_track(*mixer, 1);
    require(
        wait_for([&] { return !cd_is_playing(*mixer); }, 2000), "a track that cannot be read stops"
    );
    require(*sdl_music_device_error(device) != '\0', "the device says why");
    require(cd_play_track(*mixer, 2), "the next track plays");
    require(
        wait_for([&] { return sdl_music_device_position(device) > 0.0; }, 5000),
        "the next track decodes"
    );
    require(cd_stop(*mixer), "stopped after the unreadable track");
    mixer_teardown(*mixer);
    sdl_music_device_destroy(device);
    std::filesystem::remove_all(root);
}

} // namespace

int main(int argc, char** argv) {
    const bool installed = oa::test::game_data_requested(argc, argv);
    const auto game_dir = installed ? oa::test::require_game_directory("the installed music")
                                    : std::filesystem::path{};
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    require(SDL_Init(0), "SDL init");
    if (installed)
        installed_disc(game_dir);
    else {
        generated_disc();
        slow_track();
        unreadable_track();
    }
    SDL_Quit();
    return 0;
}
