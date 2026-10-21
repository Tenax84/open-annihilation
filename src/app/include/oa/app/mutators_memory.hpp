// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The last mutator set of each setup, and the crash record kept beside the
// preferences file while a match with mutators runs. A set is the canonical
// loadout text and nothing else. A file the player broke is read as no set,
// or as a crash record with no mutators, and is never fatal.
#pragma once

#include "oa/data/mutators/loadout.hpp"
#include "oa/platform/preferences.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace oa::app::mutators_memory {

/// Which setup remembered a set. A match no setup started has none.
enum class Setup : uint8_t {
    skirmish,    ///< a skirmish setup
    multiplayer, ///< the multiplayer host's setup
    campaign,    ///< a campaign's launch
};

/// The preferences key of one setup's last set.
///
/// The key starts `open-annihilation.mutators.` and holds no `|`. The value
/// is `canonical_loadout` of the set. Absent, or text `parse_loadout` refuses,
/// means the setup has no set.
///
/// @param setup the setup
/// @return its key
[[nodiscard]] constexpr std::string_view preference_key(Setup setup) noexcept {
    switch (setup) {
    case Setup::skirmish:
        return "open-annihilation.mutators.skirmish";
    case Setup::multiplayer:
        return "open-annihilation.mutators.multiplayer";
    case Setup::campaign:
        return "open-annihilation.mutators.campaign";
    }
    return "open-annihilation.mutators.skirmish";
}

/// What a crash record names: the setup that started the match, if one did,
/// and the mutators that were in play.
///
/// `setup` is empty for a match no setup started: a saved game, a replay, a
/// guest's game, or a list from the command line.
struct Crash {
    std::optional<Setup> setup{};
    oa::data::mutators::Loadout loadout{};
};

/// The crash record's two keys.
///
/// `setup` is `skirmish`, `multiplayer`, `campaign` or `none`. `mutators`
/// is `canonical_loadout` of the loadout. `read_crash` requires exactly
/// these two, and `preferences::save` writes them as the sentinel file.
///
/// @param crash the record
/// @return the keys `setup` and `mutators`
[[nodiscard]] oa::platform::preferences::Values crash_values(const Crash& crash);

/// Reads a crash record from the two values `crash_values` gives.
///
/// `setup` is `skirmish`, `multiplayer`, `campaign` or `none`. `mutators` is
/// read with `parse_loadout`, which refuses anything other than the canonical
/// loadout text. Any other text is no record.
///
/// @param values the file's keys and values
/// @return the record, or nothing when the values are not one
[[nodiscard]] std::optional<Crash> read_crash(const oa::platform::preferences::Values& values);

/// Name of the crash record's file, beside the preferences file.
inline constexpr std::string_view sentinel_file_name = "mutators-sentinel.conf";

/// The crash record's file in one folder.
///
/// A failure to read or write it is logged once on standard error, as
/// `open-annihilation: mutators: …`, and is never thrown. `disabled` reads
/// and writes nothing, for a run with no preferences file.
class Sentinel {
  public:

    /// Reads a leftover file from a folder.
    ///
    /// No file is no leftover. A file that cannot be read, or whose text is
    /// not a crash record, is a leftover with an empty loadout, logged once.
    /// Nothing is written.
    ///
    /// @param folder the folder that holds the preferences file
    /// @return the record of that folder
    static Sentinel open_folder(const std::filesystem::path& folder);

    /// A record that reads and writes nothing.
    ///
    /// @return the record
    static Sentinel disabled();

    /// The leftover `open_folder` found. Empty when there was no file, and
    /// always empty for `disabled`.
    ///
    /// @return the leftover
    [[nodiscard]] const std::optional<Crash>& leftover() const noexcept { return leftover_; }

    /// Replaces the file with the record through a temporary file. A failed
    /// write keeps the last complete record, and is logged once.
    ///
    /// `disabled` writes nothing.
    ///
    /// @param crash the record
    void write(const Crash& crash);

    /// Deletes the file. A failure is logged once. `disabled` deletes nothing.
    void clear();

  private:

    /// Starts a record that reads and writes nothing.
    Sentinel() = default;

    /// Writes one failure, once for this record.
    ///
    /// @param text the line after `open-annihilation: mutators: `
    void log_failure(std::string_view text);

    bool enabled_{};
    std::filesystem::path folder_{};
    std::optional<Crash> leftover_{};
    bool failure_logged_{};
};

} // namespace oa::app::mutators_memory
