// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/audio/music_decoder.hpp"
#include "oa/base/bytes.hpp"
#include "oa/platform/files.hpp"

#include "music_codecs.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <numbers>
#include <span>

namespace oa::audio {

namespace {

// music_set_read_delay: a test's wait before each read; none in the game.
std::atomic<MusicReadDelay> read_delay{nullptr};
std::atomic<void*> read_delay_context{nullptr};

void wait_for_music_read() {
    const MusicReadDelay delay = read_delay.load(std::memory_order_acquire);
    if (delay != nullptr)
        delay(read_delay_context.load(std::memory_order_acquire));
}

} // namespace

/// One encoding's reader: frames in the file's own channel layout and rate.
class MusicDecoder::Source {
  public:

    virtual ~Source() = default;

    /// Reads up to `frames` frames, interleaved in the order mix_roles() names.
    ///
    /// @param[out] samples room for frames * channels() samples
    /// @param frames frames wanted
    /// @return frames read; 0 at the end of the file, negative on an error
    [[nodiscard]] virtual int64_t read(float* samples, uint32_t frames) = 0;

    MusicCodec codec{MusicCodec::none};
    uint32_t rate{};
    uint32_t channels{};
    bool vorbis_order{}; ///< channels follow the Ogg Vorbis order rather than the WAVE order
};

namespace {

/// Returns a path's UTF-8 spelling: a name outside the system's code page
/// has no narrow spelling on Windows.
///
/// @param path the path
/// @return its UTF-8 spelling
std::string utf8_text(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

using base::bytes::load_le16;
using base::bytes::load_le32;

// Bytes read to recognise an encoding.
constexpr std::size_t signature_bytes = 36;
// A RIFF chunk header: a four-character tag and a 32-bit little-endian size.
constexpr std::size_t riff_chunk_header_bytes = 8;
// The RIFF header: "RIFF", the file size and "WAVE".
constexpr std::size_t riff_header_bytes = 12;
// WAVE format tags.
constexpr uint16_t wave_format_pcm = 1;
constexpr uint16_t wave_format_float = 3;
constexpr uint16_t wave_format_extensible = 0xfffe;
// The largest chunk skipped on the way to the samples: a seek moves at
// most this far on every system.
constexpr uint32_t max_skipped_chunk_bytes = 0x7ffffffe;
// Bytes of a format chunk: the basic fields, and with the extensible
// fields, which end in the sub-format whose first two bytes are its tag.
constexpr std::size_t wave_format_basic_bytes = 16;
constexpr std::size_t wave_format_extensible_bytes = 40;
constexpr std::size_t wave_subformat_offset = 24;
// Bytes of an Ogg page header before its segment table.
constexpr std::size_t ogg_page_header_bytes = 27;
// The offset of an Ogg page's segment count.
constexpr std::size_t ogg_segment_count_offset = 26;
// Scale of an integer sample of b bits to -1..1 is 2^-(b-1).
constexpr float scale_8 = 1.0F / 128.0F;
constexpr float scale_16 = 1.0F / 32768.0F;
constexpr float scale_32 = 1.0F / 2147483648.0F;
constexpr uint8_t unsigned_8_zero = 0x80;
// Lowest and highest sample rates a music file may have, in hertz.
constexpr uint32_t min_source_rate = resampler_min_rate;
constexpr uint32_t max_source_rate = resampler_max_rate;

// An open file, closed with its owner.
class File {
  public:

    explicit File(std::FILE* file) : file_(file) {}

    ~File() {
        if (file_ != nullptr)
            std::fclose(file_);
    }

    File(const File&) = delete;
    File& operator=(const File&) = delete;

    std::FILE* get() const { return file_; }

    // Hands the file to an owner that closes it.
    std::FILE* release() {
        std::FILE* file = file_;
        file_ = nullptr;
        return file;
    }

  private:

    std::FILE* file_{};
};

bool rate_in_range(uint32_t rate) {
    return rate >= min_source_rate && rate <= max_source_rate;
}

bool channels_in_range(uint32_t channels) {
    return channels >= 1 && channels <= music_max_source_channels;
}

// Reads a file as the decoders' read callbacks ask.
size_t file_read(void* file, void* buffer, size_t bytes) {
    wait_for_music_read();
    return std::fread(buffer, 1, bytes, static_cast<std::FILE*>(file));
}

// Moves through a file as the decoders' seek callbacks ask.
bool file_seek(void* file, int offset, bool from_start, bool from_current) {
    const int whence = from_start ? SEEK_SET : (from_current ? SEEK_CUR : SEEK_END);
    return std::fseek(static_cast<std::FILE*>(file), offset, whence) == 0;
}

// Reports a file's position as the decoders' tell callbacks ask.
bool file_tell(void* file, int64_t& cursor) {
    const long position = std::ftell(static_cast<std::FILE*>(file));
    if (position < 0)
        return false;
    cursor = position;
    return true;
}

// An integer-or-float PCM RIFF WAVE file.
class WaveSource final : public MusicDecoder::Source {
  public:

    bool open(File& file, std::string& error) {
        std::array<uint8_t, riff_header_bytes> header{};
        if (std::fread(header.data(), 1, header.size(), file.get()) != header.size() ||
            std::memcmp(header.data(), "RIFF", 4) != 0 ||
            std::memcmp(header.data() + 8, "WAVE", 4) != 0) {
            error = "not a RIFF WAVE file";
            return false;
        }
        bool have_format = false;
        for (;;) {
            std::array<uint8_t, riff_chunk_header_bytes> chunk{};
            if (std::fread(chunk.data(), 1, chunk.size(), file.get()) != chunk.size()) {
                error = have_format ? "no data chunk" : "no format chunk";
                return false;
            }
            const uint32_t size = load_le32(chunk.data() + 4);
            if (std::memcmp(chunk.data(), "fmt ", 4) == 0) {
                if (!read_format(file, size, error))
                    return false;
                have_format = true;
            } else if (std::memcmp(chunk.data(), "data", 4) == 0) {
                if (!have_format) {
                    error = "data chunk before the format chunk";
                    return false;
                }
                remaining_ = size;
                break;
            } else if (
                size > max_skipped_chunk_bytes ||
                std::fseek(file.get(), static_cast<long>(size + (size & 1U)), SEEK_CUR) != 0
            ) {
                // Each chunk skipped moves forward, so the walk ends.
                error = "truncated chunk";
                return false;
            }
        }
        file_ = file.release();
        return true;
    }

    ~WaveSource() override {
        if (file_ != nullptr)
            std::fclose(file_);
    }

    int64_t read(float* samples, uint32_t frames) override {
        const uint32_t frame_bytes = bytes_per_sample_ * channels;
        uint64_t wanted = static_cast<uint64_t>(frames) * frame_bytes;
        wanted = std::min<uint64_t>(wanted, remaining_ - remaining_ % frame_bytes);
        raw_.resize(static_cast<std::size_t>(wanted));
        const std::size_t got = std::fread(raw_.data(), 1, raw_.size(), file_);
        const std::size_t whole = got / frame_bytes;
        remaining_ -= static_cast<uint32_t>(got);
        if (got < raw_.size())
            remaining_ = 0; // the data chunk runs past the end of the file
        const std::size_t count = whole * channels;
        const uint8_t* bytes = raw_.data();
        for (std::size_t i = 0; i < count; ++i, bytes += bytes_per_sample_)
            samples[i] = convert(bytes);
        return static_cast<int64_t>(whole);
    }

  private:

    bool read_format(File& file, uint32_t size, std::string& error) {
        if (size < wave_format_basic_bytes || size > wave_format_extensible_bytes + 64) {
            error = "format chunk of unexpected size";
            return false;
        }
        std::vector<uint8_t> format(size + (size & 1U));
        if (std::fread(format.data(), 1, format.size(), file.get()) != format.size()) {
            error = "truncated format chunk";
            return false;
        }
        uint16_t tag = load_le16(format.data());
        channels = load_le16(format.data() + 2);
        rate = load_le32(format.data() + 4);
        const uint16_t bits = load_le16(format.data() + 14);
        if (tag == wave_format_extensible) {
            if (size < wave_format_extensible_bytes) {
                error = "extensible format chunk too short";
                return false;
            }
            tag = load_le16(format.data() + wave_subformat_offset);
        }
        float_samples_ = tag == wave_format_float;
        if ((tag != wave_format_pcm && !float_samples_) ||
            (float_samples_ ? bits != 32 : (bits != 8 && bits != 16 && bits != 24 && bits != 32))) {
            error = "unsupported WAVE sample format";
            return false;
        }
        if (!channels_in_range(channels) || !rate_in_range(rate)) {
            error = "WAVE channels or rate out of range";
            return false;
        }
        bytes_per_sample_ = bits / 8U;
        codec = MusicCodec::wave;
        return true;
    }

    float convert(const uint8_t* bytes) const {
        switch (bytes_per_sample_) {
        case 1:
            return static_cast<float>(static_cast<int32_t>(bytes[0]) - unsigned_8_zero) * scale_8;
        case 2:
            return static_cast<float>(static_cast<int16_t>(load_le16(bytes))) * scale_16;
        case 3:
            return static_cast<float>(static_cast<int32_t>(
                       (static_cast<uint32_t>(bytes[0]) << 8) |
                       (static_cast<uint32_t>(bytes[1]) << 16) |
                       (static_cast<uint32_t>(bytes[2]) << 24)
                   )) *
                   scale_32;
        default:
            if (float_samples_) {
                const uint32_t word = load_le32(bytes);
                float value = 0.0F;
                std::memcpy(&value, &word, sizeof(value));
                return value;
            }
            return static_cast<float>(static_cast<int32_t>(load_le32(bytes))) * scale_32;
        }
    }

    std::FILE* file_{};
    std::vector<uint8_t> raw_;
    uint32_t remaining_{}; ///< bytes left in the data chunk
    uint32_t bytes_per_sample_{};
    bool float_samples_{};
};

// An Ogg Vorbis file.
class VorbisSource final : public MusicDecoder::Source {
  public:

    bool open(File& file, std::string& error) {
        int code = 0;
        // The decoder leaves the file open, failing or not; the source closes it.
        vorbis_ = stb_vorbis_open_file(file.get(), 0, &code, nullptr);
        if (vorbis_ == nullptr) {
            error = "cannot decode Ogg Vorbis (error " + std::to_string(code) + ")";
            return false;
        }
        file_ = file.release();
        const stb_vorbis_info info = stb_vorbis_get_info(vorbis_);
        rate = info.sample_rate;
        channels = static_cast<uint32_t>(info.channels);
        if (!channels_in_range(channels) || !rate_in_range(rate)) {
            error = "Ogg Vorbis channels or rate out of range";
            return false;
        }
        codec = MusicCodec::vorbis;
        vorbis_order = true;
        return true;
    }

    ~VorbisSource() override {
        if (vorbis_ != nullptr)
            stb_vorbis_close(vorbis_);
        if (file_ != nullptr)
            std::fclose(file_);
    }

    int64_t read(float* samples, uint32_t frames) override {
        uint32_t done = 0;
        while (done < frames) {
            if (pending_offset_ == pending_frames_) {
                int file_channels = 0;
                float** output = nullptr;
                const int got = stb_vorbis_get_frame_float(vorbis_, &file_channels, &output);
                if (got <= 0)
                    break;
                pending_.resize(static_cast<std::size_t>(got) * channels);
                for (int frame = 0; frame < got; ++frame)
                    for (uint32_t channel = 0; channel < channels; ++channel)
                        pending_[static_cast<std::size_t>(frame) * channels + channel] =
                            output[channel][frame];
                pending_frames_ = static_cast<uint32_t>(got);
                pending_offset_ = 0;
            }
            const uint32_t take = std::min(frames - done, pending_frames_ - pending_offset_);
            std::copy_n(
                pending_.data() + static_cast<std::size_t>(pending_offset_) * channels,
                static_cast<std::size_t>(take) * channels,
                samples + static_cast<std::size_t>(done) * channels
            );
            pending_offset_ += take;
            done += take;
        }
        return done;
    }

  private:

    stb_vorbis* vorbis_{};
    std::FILE* file_{};
    std::vector<float> pending_; ///< a decoded Vorbis frame not yet returned
    uint32_t pending_frames_{};
    uint32_t pending_offset_{};
};

// An MPEG audio file.
class Mp3Source final : public MusicDecoder::Source {
  public:

    bool open(File& file, std::string& error) {
        if (!drmp3_init(&mp3_, file_read, seek, tell, nullptr, file.get(), nullptr)) {
            error = "cannot decode MPEG audio";
            return false;
        }
        file_ = file.release();
        initialised_ = true;
        rate = mp3_.sampleRate;
        channels = mp3_.channels;
        if (!channels_in_range(channels) || !rate_in_range(rate)) {
            error = "MPEG audio channels or rate out of range";
            return false;
        }
        codec = MusicCodec::mp3;
        return true;
    }

    ~Mp3Source() override {
        if (initialised_)
            drmp3_uninit(&mp3_);
        if (file_ != nullptr)
            std::fclose(file_);
    }

    int64_t read(float* samples, uint32_t frames) override {
        return static_cast<int64_t>(drmp3_read_pcm_frames_f32(&mp3_, frames, samples));
    }

  private:

    static drmp3_bool32 seek(void* file, int offset, drmp3_seek_origin origin) {
        return file_seek(file, offset, origin == DRMP3_SEEK_SET, origin == DRMP3_SEEK_CUR)
                   ? DRMP3_TRUE
                   : DRMP3_FALSE;
    }

    static drmp3_bool32 tell(void* file, drmp3_int64* cursor) {
        int64_t position = 0;
        if (!file_tell(file, position))
            return DRMP3_FALSE;
        *cursor = position;
        return DRMP3_TRUE;
    }

    drmp3 mp3_{};
    std::FILE* file_{};
    bool initialised_{};
};

// A FLAC file, native or in Ogg.
class FlacSource final : public MusicDecoder::Source {
  public:

    bool open(File& file, std::string& error) {
        flac_ = drflac_open(file_read, seek, tell, file.get(), nullptr);
        if (flac_ == nullptr) {
            error = "cannot decode FLAC";
            return false;
        }
        file_ = file.release();
        rate = flac_->sampleRate;
        channels = flac_->channels;
        if (!channels_in_range(channels) || !rate_in_range(rate)) {
            error = "FLAC channels or rate out of range";
            return false;
        }
        codec = MusicCodec::flac;
        return true;
    }

    ~FlacSource() override {
        if (flac_ != nullptr)
            drflac_close(flac_);
        if (file_ != nullptr)
            std::fclose(file_);
    }

    int64_t read(float* samples, uint32_t frames) override {
        whole_.resize(static_cast<std::size_t>(frames) * channels);
        const auto got = drflac_read_pcm_frames_s32(flac_, frames, whole_.data());
        // Samples come left-aligned in 32 bits whatever their depth.
        const std::size_t count = static_cast<std::size_t>(got) * channels;
        for (std::size_t i = 0; i < count; ++i)
            samples[i] = static_cast<float>(whole_[i]) * scale_32;
        return static_cast<int64_t>(got);
    }

  private:

    static drflac_bool32 seek(void* file, int offset, drflac_seek_origin origin) {
        return file_seek(file, offset, origin == DRFLAC_SEEK_SET, origin == DRFLAC_SEEK_CUR)
                   ? DRFLAC_TRUE
                   : DRFLAC_FALSE;
    }

    static drflac_bool32 tell(void* file, drflac_int64* cursor) {
        int64_t position = 0;
        if (!file_tell(file, position))
            return DRFLAC_FALSE;
        *cursor = position;
        return DRFLAC_TRUE;
    }

    drflac* flac_{};
    std::FILE* file_{};
    std::vector<int32_t> whole_;
};

// What a channel carries, which sets its share of each side of the mix.
enum class ChannelRole {
    front_left,
    front_right,
    centre,
    low_frequency,
    back_left,
    back_right,
    back_centre,
    side_left,
    side_right,
};

using enum ChannelRole;

// The roles of each channel count, in the WAVE order.
constexpr std::
    array<std::array<ChannelRole, music_max_source_channels>, music_max_source_channels + 1>
        wave_roles{{
            {},
            {centre},
            {front_left, front_right},
            {front_left, front_right, low_frequency},
            {front_left, front_right, centre, back_centre},
            {front_left, front_right, centre, back_left, back_right},
            {front_left, front_right, centre, low_frequency, back_left, back_right},
            {front_left, front_right, centre, low_frequency, back_centre, side_left, side_right},
            {front_left,
             front_right,
             centre,
             low_frequency,
             back_left,
             back_right,
             side_left,
             side_right},
        }};

// The roles of each channel count, in the Ogg Vorbis order.
constexpr std::
    array<std::array<ChannelRole, music_max_source_channels>, music_max_source_channels + 1>
        vorbis_roles{{
            {},
            {centre},
            {front_left, front_right},
            {front_left, centre, front_right},
            {front_left, front_right, back_left, back_right},
            {front_left, centre, front_right, back_left, back_right},
            {front_left, centre, front_right, back_left, back_right, low_frequency},
            {front_left, centre, front_right, side_left, side_right, back_centre, low_frequency},
            {front_left,
             centre,
             front_right,
             side_left,
             side_right,
             back_left,
             back_right,
             low_frequency},
        }};

// A channel's share of the left and right sides.
struct SideShares {
    float left{};
    float right{};
};

constexpr float half_power = static_cast<float>(std::numbers::sqrt2 / 2.0);
constexpr float half = 0.5F;

SideShares role_shares(ChannelRole role) {
    switch (role) {
    case front_left:
        return {1.0F, 0.0F};
    case front_right:
        return {0.0F, 1.0F};
    case centre:
        return {half_power, half_power};
    case back_left:
    case side_left:
        return {half_power, 0.0F};
    case back_right:
    case side_right:
        return {0.0F, half_power};
    case back_centre:
        return {half, half};
    case low_frequency:
        break;
    }
    return {};
}

// Mixes frames of a source's channels to stereo, appending to `stereo`.
void mix_to_stereo(
    const MusicDecoder::Source& source, std::span<const float> samples, std::vector<float>& stereo
) {
    const uint32_t channels = source.channels;
    const std::size_t frames = samples.size() / channels;
    if (channels == 2) {
        stereo.insert(
            stereo.end(), samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(frames * 2)
        );
        return;
    }
    // Shares in the order the WAVE layout lists the roles, so the sums add
    // up in one order whatever the file's own order.
    std::array<SideShares, music_max_source_channels> shares{};
    std::array<uint32_t, music_max_source_channels> order{};
    const auto& roles = (source.vorbis_order ? vorbis_roles : wave_roles)[channels];
    for (uint32_t channel = 0; channel < channels; ++channel) {
        shares[channel] = role_shares(roles[channel]);
        order[channel] = channel;
    }
    std::stable_sort(order.begin(), order.begin() + channels, [&](uint32_t a, uint32_t b) {
        return static_cast<int>(roles[a]) < static_cast<int>(roles[b]);
    });
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const float* in = samples.data() + frame * channels;
        float left = 0.0F;
        float right = 0.0F;
        for (uint32_t i = 0; i < channels; ++i) {
            const uint32_t channel = order[i];
            if (shares[channel].left != 0.0F)
                left += in[channel] * shares[channel].left;
            if (shares[channel].right != 0.0F)
                right += in[channel] * shares[channel].right;
        }
        stereo.push_back(left);
        stereo.push_back(right);
    }
}

// Recognises an encoding from the first bytes of a file.
MusicCodec recognise(const uint8_t* bytes, std::size_t size) {
    if (size >= riff_header_bytes && std::memcmp(bytes, "RIFF", 4) == 0 &&
        std::memcmp(bytes + 8, "WAVE", 4) == 0)
        return MusicCodec::wave;
    if (size >= 4 && std::memcmp(bytes, "fLaC", 4) == 0)
        return MusicCodec::flac;
    if (size > ogg_page_header_bytes && std::memcmp(bytes, "OggS", 4) == 0) {
        // The first page's first segment says which codec follows: Vorbis's
        // identification header begins "\x01vorbis".
        const std::size_t first_segment = ogg_page_header_bytes + bytes[ogg_segment_count_offset];
        if (first_segment + 7 <= size && std::memcmp(bytes + first_segment, "\x01vorbis", 7) == 0)
            return MusicCodec::vorbis;
        return MusicCodec::flac;
    }
    return MusicCodec::mp3;
}

template <typename Reader>
std::unique_ptr<MusicDecoder::Source> open_source(File& file, std::string& error) {
    auto reader = std::make_unique<Reader>();
    if (!reader->open(file, error))
        return nullptr;
    return reader;
}

} // namespace

MusicDecoder::MusicDecoder() = default;

MusicDecoder::~MusicDecoder() {
    close();
}

bool MusicDecoder::open(const std::filesystem::path& path, std::string& error) {
    close();
    // By its path's wide spelling on Windows, so that any name the file
    // system allows opens.
    File file(platform::open_file(path, "rb"));
    if (file.get() == nullptr) {
        error = "cannot open " + utf8_text(path);
        return false;
    }
    std::array<uint8_t, signature_bytes> signature{};
    wait_for_music_read();
    const std::size_t got = std::fread(signature.data(), 1, signature.size(), file.get());
    if (std::fseek(file.get(), 0, SEEK_SET) != 0) {
        error = "cannot read " + utf8_text(path);
        return false;
    }
    std::string reason;
    std::unique_ptr<Source> source;
    switch (recognise(signature.data(), got)) {
    case MusicCodec::wave:
        source = open_source<WaveSource>(file, reason);
        break;
    case MusicCodec::vorbis:
        source = open_source<VorbisSource>(file, reason);
        break;
    case MusicCodec::flac:
        source = open_source<FlacSource>(file, reason);
        break;
    case MusicCodec::mp3:
    case MusicCodec::none:
        source = open_source<Mp3Source>(file, reason);
        break;
    }
    if (source == nullptr) {
        error = reason + ": " + utf8_text(path);
        return false;
    }
    if (!resampler_.configure(source->rate, music_output_rate, music_output_channels)) {
        error = "cannot convert the sample rate of " + utf8_text(path);
        return false;
    }
    source_ = std::move(source);
    finished_ = false;
    return true;
}

bool MusicDecoder::decode(std::vector<float>& out) {
    if (source_ == nullptr || finished_)
        return false;
    block_.resize(static_cast<std::size_t>(music_block_frames) * source_->channels);
    const int64_t frames = source_->read(block_.data(), music_block_frames);
    if (frames > 0) {
        stereo_.clear();
        mix_to_stereo(
            *source_,
            std::span<const float>(
                block_.data(), static_cast<std::size_t>(frames) * source_->channels
            ),
            stereo_
        );
        resampler_.process(stereo_, out);
        return true;
    }
    resampler_.finish(out);
    finished_ = true;
    return false;
}

void MusicDecoder::close() noexcept {
    source_.reset();
    resampler_.reset();
    finished_ = false;
}

MusicCodec MusicDecoder::codec() const noexcept {
    return source_ != nullptr ? source_->codec : MusicCodec::none;
}

uint32_t MusicDecoder::source_rate() const noexcept {
    return source_ != nullptr ? source_->rate : 0;
}

uint32_t MusicDecoder::source_channels() const noexcept {
    return source_ != nullptr ? source_->channels : 0;
}

void music_set_read_delay(MusicReadDelay delay, void* context) noexcept {
    if (delay == nullptr) {
        read_delay.store(nullptr, std::memory_order_release);
        read_delay_context.store(nullptr, std::memory_order_release);
        return;
    }
    read_delay_context.store(context, std::memory_order_release);
    read_delay.store(delay, std::memory_order_release);
}

} // namespace oa::audio
