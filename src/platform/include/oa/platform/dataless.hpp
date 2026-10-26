// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// Files the system has moved out of local storage. On macOS that is a
// dataless file (st_flags & SF_DATALESS): reading it waits while the file
// downloads. Elsewhere no file is reported that way.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace oa::platform {

/// Where one file's bytes are.
struct FileResidence {
    bool dataless{}; ///< the bytes are not on this computer yet
};

/// Asks where one file's bytes are.
///
/// @param path the file
/// @param[out] residence set when the function returns true
/// @param context the pointer given to count_evicted_files
/// @return false when the file cannot be asked about; it is then not counted
using FileResidenceProbe =
    bool (*)(const std::filesystem::path& path, FileResidence& residence, void* context);

/// Regular files a probe answered, and how many of them are not stored here.
struct EvictedFiles {
    uint64_t files{};   ///< regular files the probe answered
    uint64_t evicted{}; ///< of those, the ones whose bytes are not here
};

/// Counts regular files under `roots` whose probe says the bytes are not here.
///
/// A root that is missing, or that cannot be listed, adds nothing. A root
/// that is itself a file is asked about on its own. Directories reached
/// through a link are not listed. When `evicted` is not null, it receives
/// the paths whose bytes are not here. When `stop` is set the walk ends at
/// the next file, with what it has counted so far.
///
/// @param roots folders and files to look through
/// @param probe how one file is asked about; null counts nothing
/// @param context passed to the probe
/// @param[out] evicted paths whose bytes are not here; may be null
/// @param stop set by another thread to end the walk early; may be null
/// @return how many files were answered about, and how many are not here
[[nodiscard]] EvictedFiles count_evicted_files(
    std::span<const std::filesystem::path> roots,
    FileResidenceProbe probe,
    void* context,
    std::vector<std::filesystem::path>* evicted = nullptr,
    const std::atomic<bool>* stop = nullptr
);

/// Tells whether a file's stat flags (st_flags) say its bytes are not here.
///
/// On macOS that is SF_DATALESS. On every other system no flags do.
///
/// @param flags the file's st_flags
/// @return true when the flags mark the file dataless
[[nodiscard]] bool stat_flags_dataless(uint32_t flags) noexcept;

/// Reports whether this system can keep a file's bytes somewhere else.
///
/// True on macOS, where a dataless file downloads when it is read. False
/// everywhere else, so a line about that download is never shown there.
///
/// @return true on macOS
[[nodiscard]] bool host_reports_dataless_files() noexcept;

/// Asks the operating system where a file's bytes are.
///
/// On macOS a file whose stat flags carry SF_DATALESS is dataless
/// (stat_flags_dataless). On every other system the file is reported as
/// present.
///
/// @param path the file
/// @param[out] residence set when the file can be asked about
/// @param context unused
/// @return false when the file cannot be asked about
[[nodiscard]] bool
system_file_residence(const std::filesystem::path& path, FileResidence& residence, void* context);

/// Starts one background fetch of the player's folders, including Games and
/// Expansions. A second call does nothing. On every system but macOS it does
/// nothing.
///
/// @param roots folders to look through
void start_startup_evicted_fetch(std::vector<std::filesystem::path> roots);

/// Asks macOS to fetch files that are not on this computer, without waiting.
///
/// The count of those files is written as one line to the standard error,
/// then each is asked for in the background. The call returns before that
/// work finishes. On every other system, iOS included, it does nothing.
class EvictedFetch {
  public:

    /// Creates a fetch that has not started.
    EvictedFetch();
    /// Ends a started fetch at its next file and waits for its thread.
    ~EvictedFetch();
    EvictedFetch(const EvictedFetch&) = delete;
    EvictedFetch& operator=(const EvictedFetch&) = delete;

    /// Starts one fetch of `roots`. A second call does nothing.
    ///
    /// @param roots folders to look through
    /// @param where the folders named in the log line
    void start(std::vector<std::filesystem::path> roots, std::string_view where);

  private:

    struct State;
    std::unique_ptr<State> state_;
};

} // namespace oa::platform
