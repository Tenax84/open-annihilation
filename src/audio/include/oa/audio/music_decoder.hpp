// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "oa/audio/resampler.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace oa::audio {

/// Sample rate of decoded music, in hertz.
inline constexpr uint32_t music_output_rate = 44100;
/// Channels of decoded music: left and right, interleaved.
inline constexpr uint32_t music_output_channels = 2;
/// Most channels a music file may have.
inline constexpr uint32_t music_max_source_channels = 8;
/// Frames read from a music file per decode() call.
inline constexpr uint32_t music_block_frames = 4096;

/// The encodings a music file may hold.
enum class MusicCodec {
    none,   ///< no file is open
    wave,   ///< RIFF WAVE: 8-, 16-, 24- or 32-bit integer or 32-bit float PCM
    vorbis, ///< Ogg Vorbis
    mp3,    ///< MPEG audio layer III (and layers I and II)
    flac,   ///< FLAC, native or in Ogg
};

/// Decodes one music file to interleaved stereo float samples at music_output_rate.
///
/// The encoding is recognised from the file's contents, not its name.
/// Samples are scaled to -1..1: an integer sample of b bits is divided by
/// 2^(b-1), and 8-bit samples are unsigned around 128. Other channel counts
/// are mixed to stereo:
///
///   - one channel plays on both sides at 1/sqrt(2);
///   - three channels are left, right and low-frequency;
///   - four are left, right, centre and back centre;
///   - five are left, right, centre, back left and back right;
///   - six add low-frequency before the back pair;
///   - seven are left, right, centre, low-frequency, back centre, side left
///     and side right;
///   - eight are left, right, centre, low-frequency, back left, back right,
///     side left and side right.
///
/// Ogg Vorbis files name their channels in their own order (left, centre,
/// right, ...), with three channels as left, centre and right. In the mix a
/// centre channel adds 1/sqrt(2) to each side, a back centre 1/2 to each,
/// a back or side channel 1/sqrt(2) to its own side, and the low-frequency
/// channel is left out. Other rates are converted by Resampler.
class MusicDecoder {
  public:

    /// Creates a decoder with no file open.
    MusicDecoder();
    /// Closes the open file, if any.
    ~MusicDecoder();
    MusicDecoder(const MusicDecoder&) = delete;
    MusicDecoder& operator=(const MusicDecoder&) = delete;

    /// Opens a music file, closing any file already open.
    ///
    /// @param path the music file
    /// @param[out] error why the file cannot be played; untouched on success
    /// @return true when the file is open and its first samples can be decoded
    [[nodiscard]] bool open(const std::filesystem::path& path, std::string& error);

    /// Appends the next block of decoded stereo samples.
    ///
    /// @param[in,out] out receives interleaved left and right samples
    /// @return false once the file is exhausted (after appending its last
    ///         samples), on a decoding error, or when no file is open
    [[nodiscard]] bool decode(std::vector<float>& out);

    /// Closes the open file, if any.
    void close() noexcept;

    /// Returns the encoding of the open file.
    ///
    /// @return MusicCodec::none when no file is open
    [[nodiscard]] MusicCodec codec() const noexcept;

    /// Returns the sample rate stored in the open file.
    ///
    /// @return the rate in hertz, 0 when no file is open
    [[nodiscard]] uint32_t source_rate() const noexcept;

    /// Returns the channel count stored in the open file.
    ///
    /// @return the channel count, 0 when no file is open
    [[nodiscard]] uint32_t source_channels() const noexcept;

    /// Interface of one encoding's reader, defined with the readers.
    class Source;

  private:

    std::unique_ptr<Source> source_;
    Resampler resampler_;
    std::vector<float> block_;  ///< frames read from the file, in its channels
    std::vector<float> stereo_; ///< the same frames mixed to stereo
    bool finished_{};
};

/// Runs at the start of each read of a music file.
using MusicReadDelay = void (*)(void* context);

/// Installs a wait that runs at the start of each read of a music file.
///
/// The game installs none, so a read takes only as long as the file does.
/// A test installs one so the read takes time while the rest of the game
/// continues. A null delay reads at once. The wait runs on the thread that
/// reads the file.
///
/// @param delay the wait, or null to read at once
/// @param context passed to the wait
void music_set_read_delay(MusicReadDelay delay, void* context) noexcept;

} // namespace oa::audio
