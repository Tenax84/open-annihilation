// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/audio/sdl_music.hpp"

#include "oa/audio/music_decoder.hpp"
#include "oa/audio/sound_output.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace oa::audio {
namespace {

constexpr int output_rate = static_cast<int>(music_output_rate);
constexpr int output_channels = static_cast<int>(music_output_channels);
constexpr int32_t music_aux_device = 0;
constexpr uint32_t full_aux_volume = 0xffffffffU;
// Decoded blocks kept ready for the callback, about a third of a second.
constexpr std::size_t decoded_blocks_ahead = 4;

} // namespace

struct SdlMusicDevice {
    enum class Phase { idle, need_open, decoding, quit };

    MusicDisc disc;
    std::unique_ptr<OutputStream> stream;
    std::string error;
    int32_t current{};  // disc track being decoded
    int32_t to_track{}; // exclusive end of the range, 0 for the disc end
    uint64_t frames{};  // decoded frames of the current track handed to the stream
    uint32_t aux_volume{full_aux_volume};
    bool active{}; // a range is loaded
    bool paused{};
    bool held{};       // sdl_music_device_hold: the stream stays paused
    bool range_done{}; // the decoder reached the end of the range
    // The first open of a play reports a failure by stopping. A later track
    // of the same range reports it by ending the range.
    bool opening_initial{};

    mutable std::mutex queue_mu;
    std::condition_variable queue_cv;
    std::deque<std::vector<float>> ready;
    std::filesystem::path job_path;
    uint64_t epoch{};
    Phase phase{Phase::idle};
    bool load_failed{};
    bool load_ended{};
    std::thread loader;
};

namespace {

// Holds the stream lock that the audio-thread callback runs under.
class StreamLock {
  public:

    explicit StreamLock(SdlMusicDevice& device) : stream_(device.stream.get()) {
        if (stream_ != nullptr)
            stream_->lock();
    }

    ~StreamLock() {
        if (stream_ != nullptr)
            stream_->unlock();
    }

    StreamLock(const StreamLock&) = delete;
    StreamLock& operator=(const StreamLock&) = delete;

  private:

    OutputStream* stream_;
};

bool track_in_disc(const SdlMusicDevice& device, int32_t track) {
    return track >= music_disc_first_audio_track && track <= device.disc.track_count;
}

// Opens and decodes on this thread. The callback only takes blocks that are
// already in the queue, and never waits for a read.
void load_music(SdlMusicDevice& device) {
    MusicDecoder decoder;
    for (;;) {
        std::filesystem::path path;
        uint64_t epoch = 0;
        SdlMusicDevice::Phase phase = SdlMusicDevice::Phase::idle;
        {
            std::unique_lock lock(device.queue_mu);
            device.queue_cv.wait(lock, [&] {
                if (device.phase == SdlMusicDevice::Phase::quit)
                    return true;
                if (device.phase == SdlMusicDevice::Phase::need_open)
                    return true;
                return device.phase == SdlMusicDevice::Phase::decoding &&
                       device.ready.size() < decoded_blocks_ahead;
            });
            if (device.phase == SdlMusicDevice::Phase::quit)
                return;
            phase = device.phase;
            epoch = device.epoch;
            path = device.job_path;
        }
        // The file is opened, read and closed with the lock released, so the
        // callback never waits for the disk.
        bool close_file = false;
        if (phase == SdlMusicDevice::Phase::need_open) {
            decoder.close();
            std::string reason;
            const bool opened = decoder.open(path, reason);
            {
                std::lock_guard lock(device.queue_mu);
                if (device.phase == SdlMusicDevice::Phase::quit)
                    return;
                if (device.epoch != epoch) {
                    close_file = true;
                } else if (!opened) {
                    device.error = std::move(reason);
                    device.load_failed = true;
                    device.phase = SdlMusicDevice::Phase::idle;
                } else {
                    device.phase = SdlMusicDevice::Phase::decoding;
                }
            }
        } else {
            std::vector<float> block;
            const bool more = decoder.decode(block);
            std::lock_guard lock(device.queue_mu);
            if (device.phase == SdlMusicDevice::Phase::quit)
                return;
            if (device.epoch != epoch) {
                close_file = true;
            } else {
                if (!block.empty())
                    device.ready.push_back(std::move(block));
                if (!more) {
                    device.load_ended = true;
                    device.phase = SdlMusicDevice::Phase::idle;
                    close_file = true;
                }
            }
        }
        if (close_file)
            decoder.close();
    }
}

void request_track(SdlMusicDevice& device, int32_t track, bool initial) {
    device.current = track;
    device.frames = 0;
    device.opening_initial = initial;
    {
        std::lock_guard lock(device.queue_mu);
        ++device.epoch;
        device.ready.clear();
        device.load_failed = false;
        device.load_ended = false;
        device.error.clear();
        device.job_path = device.disc.tracks[static_cast<std::size_t>(track)];
        device.phase = SdlMusicDevice::Phase::need_open;
    }
    device.queue_cv.notify_all();
}

void feed_stream(void* user, OutputStream& stream, int32_t additional) {
    auto& device = *static_cast<SdlMusicDevice*>(user);
    while (additional > 0 && device.active && !device.range_done) {
        std::vector<float> block;
        bool failed = false;
        bool ended = false;
        {
            std::lock_guard lock(device.queue_mu);
            if (!device.ready.empty()) {
                block = std::move(device.ready.front());
                device.ready.pop_front();
            } else if (device.load_failed) {
                failed = true;
                device.load_failed = false;
            } else if (device.load_ended) {
                ended = true;
                device.load_ended = false;
            } else {
                break;
            }
        }
        device.queue_cv.notify_all();
        if (!block.empty()) {
            const auto bytes = static_cast<int>(block.size() * sizeof(float));
            (void)stream.put(block.data(), bytes);
            additional -= bytes;
            device.frames += block.size() / output_channels;
            continue;
        }
        if (failed) {
            if (device.opening_initial)
                device.active = false;
            else
                device.range_done = true;
            break;
        }
        if (!ended)
            break;
        const int32_t next = device.current + 1;
        const bool in_range =
            device.to_track == 0 ? next <= device.disc.track_count : next < device.to_track;
        if (in_range && track_in_disc(device, next)) {
            request_track(device, next, false);
            break;
        }
        device.range_done = true;
    }
}

void apply_gain(SdlMusicDevice& device) {
    if (device.stream != nullptr)
        device.stream->set_gain(
            static_cast<float>(device.aux_volume & max_device_volume) /
            static_cast<float>(max_device_volume)
        );
}

bool device_open(void* context) {
    auto& device = *static_cast<SdlMusicDevice*>(context);
    if (device.stream != nullptr)
        return true;
    SoundOutput& output = sound_output();
    if (!output.start(device.error))
        return false;
    const StreamFormat format{
        SampleFormat::f32, static_cast<uint8_t>(output_channels), static_cast<uint32_t>(output_rate)
    };
    device.stream = output.open_stream(format, feed_stream, &device, device.error);
    if (device.stream == nullptr) {
        output.stop();
        return false;
    }
    apply_gain(device);
    return true;
}

bool device_stop(void* context) {
    auto& device = *static_cast<SdlMusicDevice*>(context);
    if (device.stream == nullptr)
        return true;
    device.stream->pause();
    StreamLock lock(device);
    device.active = false;
    device.paused = false;
    device.range_done = false;
    device.opening_initial = false;
    {
        std::lock_guard queue(device.queue_mu);
        ++device.epoch;
        device.ready.clear();
        device.load_failed = false;
        device.load_ended = false;
        device.phase = SdlMusicDevice::Phase::idle;
    }
    device.queue_cv.notify_all();
    device.stream->clear();
    return true;
}

void device_close(void* context) {
    auto& device = *static_cast<SdlMusicDevice*>(context);
    if (device.stream == nullptr)
        return;
    device_stop(context);
    device.stream.reset();
    sound_output().stop();
}

bool device_track_count(void* context, int32_t* count) {
    const auto& device = *static_cast<SdlMusicDevice*>(context);
    if (!music_disc_present(device.disc))
        return false;
    *count = device.disc.track_count;
    return true;
}

CdTrackKind device_first_track_kind(void* context) {
    const auto& device = *static_cast<SdlMusicDevice*>(context);
    return music_disc_present(device.disc) ? CdTrackKind::other : CdTrackKind::unavailable;
}

bool device_is_playing(void* context) {
    auto& device = *static_cast<SdlMusicDevice*>(context);
    if (device.stream == nullptr)
        return false;
    StreamLock lock(device);
    return device.active && !device.paused &&
           !(device.range_done && device.stream->queued_bytes() == 0);
}

bool device_current_track(void* context, int32_t* track) {
    auto& device = *static_cast<SdlMusicDevice*>(context);
    StreamLock lock(device);
    *track = device.current;
    return music_disc_present(device.disc);
}

bool device_play(void* context, int32_t from, int32_t to) {
    auto& device = *static_cast<SdlMusicDevice*>(context);
    if (device.stream == nullptr || !track_in_disc(device, from))
        return false;
    {
        StreamLock lock(device);
        device.stream->clear();
        device.to_track = to;
        device.paused = false;
        device.range_done = false;
        device.active = true;
        request_track(device, from, true);
    }
    if (!device.held)
        device.stream->resume();
    return true;
}

bool device_resume(void* context, int32_t to) {
    auto& device = *static_cast<SdlMusicDevice*>(context);
    if (device.stream == nullptr)
        return false;
    {
        StreamLock lock(device);
        if (!device.active)
            return false;
        device.to_track = to;
        device.paused = false;
    }
    if (!device.held)
        device.stream->resume();
    return true;
}

bool device_pause(void* context) {
    auto& device = *static_cast<SdlMusicDevice*>(context);
    if (device.stream == nullptr)
        return false;
    device.stream->pause();
    StreamLock lock(device);
    device.paused = true;
    return true;
}

bool device_disc_id(void* context, uint32_t* id) {
    const auto& device = *static_cast<SdlMusicDevice*>(context);
    if (!music_disc_present(device.disc))
        return false;
    *id = device.disc.disc_id;
    return true;
}

uint32_t device_find_foreign_player(void*) {
    return 0;
}

void device_close_foreign_player(void*, uint32_t) {
}

int32_t device_aux_count(void*) {
    return 1;
}

bool device_aux_is_cd_audio(void*, int32_t aux) {
    return aux == music_aux_device;
}

bool device_get_aux_volume(void* context, int32_t aux, uint32_t* packed) {
    if (aux != music_aux_device)
        return false;
    *packed = static_cast<SdlMusicDevice*>(context)->aux_volume;
    return true;
}

bool device_set_aux_volume(void* context, int32_t aux, uint32_t packed) {
    if (aux != music_aux_device)
        return false;
    auto& device = *static_cast<SdlMusicDevice*>(context);
    device.aux_volume = packed;
    apply_gain(device);
    return true;
}

} // namespace

SdlMusicDevice* sdl_music_device_create(MusicDisc disc) {
    auto* device = new (std::nothrow) SdlMusicDevice{};
    if (device == nullptr)
        return nullptr;
    device->disc = std::move(disc);
    try {
        device->loader = std::thread(load_music, std::ref(*device));
    } catch (...) {
        delete device;
        return nullptr;
    }
    return device;
}

void sdl_music_device_destroy(SdlMusicDevice* device) noexcept {
    if (device == nullptr)
        return;
    device_close(device);
    {
        std::lock_guard lock(device->queue_mu);
        device->phase = SdlMusicDevice::Phase::quit;
    }
    device->queue_cv.notify_all();
    if (device->loader.joinable())
        device->loader.join();
    delete device;
}

MusicDevice sdl_music_device(SdlMusicDevice* device) noexcept {
    MusicDevice music{};
    music.context = device;
    music.open = device_open;
    music.close = device_close;
    music.stop = device_stop;
    music.track_count = device_track_count;
    music.first_track_kind = device_first_track_kind;
    music.is_playing = device_is_playing;
    music.current_track = device_current_track;
    music.play = device_play;
    music.resume = device_resume;
    music.pause = device_pause;
    music.disc_id = device_disc_id;
    music.find_foreign_player = device_find_foreign_player;
    music.close_foreign_player = device_close_foreign_player;
    music.aux_count = device_aux_count;
    music.aux_is_cd_audio = device_aux_is_cd_audio;
    music.get_aux_volume = device_get_aux_volume;
    music.set_aux_volume = device_set_aux_volume;
    return music;
}

bool sdl_music_device_poll_complete(SdlMusicDevice* device) noexcept {
    if (device == nullptr || device->stream == nullptr)
        return false;
    StreamLock lock(*device);
    if (!device->active || device->paused || !device->range_done ||
        device->stream->queued_bytes() != 0)
        return false;
    device->active = false;
    return true;
}

void sdl_music_device_hold(SdlMusicDevice* device, bool held) noexcept {
    if (device == nullptr || held == device->held)
        return;
    device->held = held;
    if (device->stream == nullptr)
        return;
    // A paused stream keeps the decoded samples queued, so the track plays
    // on from where it stopped.
    if (held) {
        device->stream->pause();
        return;
    }
    bool playing = false;
    {
        StreamLock lock(*device);
        playing = device->active && !device->paused;
    }
    if (playing)
        device->stream->resume();
}

bool sdl_music_decoder_available() noexcept {
    return true;
}

const char* sdl_music_device_error(const SdlMusicDevice* device) noexcept {
    if (device == nullptr)
        return "";
    thread_local std::string copy;
    std::lock_guard lock(device->queue_mu);
    copy = device->error;
    return copy.c_str();
}

double sdl_music_device_position(SdlMusicDevice* device) noexcept {
    if (device == nullptr)
        return 0.0;
    StreamLock lock(*device);
    return static_cast<double>(device->frames) / output_rate;
}

} // namespace oa::audio
