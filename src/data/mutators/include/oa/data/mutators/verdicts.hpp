// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Verdicts a dry run gives a list of mutators against one mod, and the
// lines they are remembered as. Nothing here runs a mutator or reads a
// file. The dry run fills a record; the Library and the picker read it.
#pragma once

#include "oa/base/sha256.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::data::mutators {

/// Which result a dry run gave a list of mutators against one mod.
enum class Verdict : uint8_t {
    compatible,   ///< the list changes the mod
    no_effect,    ///< the list applies and changes nothing
    incompatible, ///< the list cannot run with this mod
    failed,       ///< the check itself did not finish
};

/// Returns the file and log word for a verdict.
///
/// The words are `compatible`, `no-effect`, `incompatible` and `failed`.
///
/// @param verdict the verdict
/// @return the word, stable for the life of the program
[[nodiscard]] std::string_view verdict_word(Verdict verdict) noexcept;

/// Reads a verdict word.
///
/// @param word the file's word, matched exactly
/// @return the verdict, or nullopt when the word is unknown
[[nodiscard]] std::optional<Verdict> parse_verdict_word(std::string_view word) noexcept;

/// How many records and files a compatible verdict changed, per collection.
struct ChangeCounts {
    /// Units whose stored text changed.
    uint32_t units_changed{};
    /// Units added.
    uint32_t units_added{};
    /// Units removed.
    uint32_t units_removed{};
    /// Weapons whose stored text changed.
    uint32_t weapons_changed{};
    /// Weapons added.
    uint32_t weapons_added{};
    /// Weapons removed.
    uint32_t weapons_removed{};
    /// Features whose stored text changed.
    uint32_t features_changed{};
    /// Features added.
    uint32_t features_added{};
    /// Features removed.
    uint32_t features_removed{};
    /// Menu entries changed.
    uint32_t menu_entries_changed{};
    /// Files added.
    uint32_t files_added{};
    /// Files hidden.
    uint32_t files_hidden{};

    /// Returns whether every count is equal.
    ///
    /// @param other the counts to compare
    /// @return true when every field matches
    [[nodiscard]] bool operator==(const ChangeCounts& other) const = default;
};

/// Returns whether any count is above zero.
///
/// @param counts the counts
/// @return true when one field is not zero
[[nodiscard]] bool any(const ChangeCounts& counts) noexcept;

/// The most bytes a reason or a mod label keeps.
inline constexpr std::size_t verdict_reason_most_bytes = 200;

/// Makes one line of at most 200 bytes from a reason or a label.
///
/// Line feeds, carriage returns and tabs become spaces. A run of spaces
/// becomes one space, and the ends are trimmed. The cut is at 200 bytes
/// and always on a whole UTF-8 character, and a space left at the end by
/// that cut is trimmed again. Bytes that are not UTF-8 are dropped, so
/// the result is text a verdict line can hold. The same text a second
/// time is unchanged.
///
/// @param text the text a check or a mod name supplied
/// @return the line, possibly empty
[[nodiscard]] std::string one_line_reason(std::string_view text);

/// One dry-run result for one list against one mod.
struct VerdictRecord {
    /// The verdict. A default record is `failed`.
    Verdict verdict{Verdict::failed};
    /// Counts for a compatible verdict. Other verdicts leave them zero.
    ChangeCounts counts{};
    /// Why, for `incompatible` and `failed`. Empty for the other two.
    std::string reason{};
    /// The mod's name and version as a player reads them. Empty for the
    /// base game.
    std::string mod_label{};

    /// Returns whether the verdict, the counts and both texts match.
    ///
    /// @param other the record to compare
    /// @return true when every field matches
    [[nodiscard]] bool operator==(const VerdictRecord& other) const = default;
};

/// What a remembered verdict is stored under.
///
/// The dry run fills each part: `loadout` is the digest of the whole
/// checked list, prerequisites included; `mod` is the mod key;
/// `engine_version` is the engine version text.
struct VerdictKey {
    /// Digest of the whole checked list, prerequisites included.
    base::sha256::Digest loadout{};
    /// The mod key.
    base::sha256::Digest mod{};
    /// The engine version text.
    std::string engine_version{};

    /// Returns whether the digests and the version match.
    ///
    /// @param other the key to compare
    /// @return true when every field matches
    [[nodiscard]] bool operator==(const VerdictKey& other) const = default;

    /// Orders by loadout, then mod, then engine version.
    ///
    /// @param other the key to compare
    /// @return the order of the two keys
    [[nodiscard]] auto operator<=>(const VerdictKey& other) const = default;
};

/// A verdict remembered under its key.
struct RememberedVerdict {
    /// The list, the mod and the engine version.
    VerdictKey key{};
    /// The verdict itself.
    VerdictRecord record{};
};

/// How many verdicts the file keeps. Older ones leave first.
inline constexpr std::size_t verdicts_most_lines = 1024;

/// The most bytes of one line, not counting its ending line feed.
inline constexpr std::size_t verdict_line_most_bytes = 2048;

/// The most bytes of the whole file. A longer span is read as empty.
inline constexpr std::size_t verdicts_file_most_bytes = 4 * 1024 * 1024;

/// The file's name. Where it lives is the dry run's choice.
inline constexpr std::string_view verdicts_file_name = "mutator-verdicts.tsv";

/// Reads remembered verdicts from the file's bytes.
///
/// The bytes are oldest first. A span longer than `verdicts_file_most_bytes`
/// reads as empty. Each line is UTF-8 and ends with a line feed (the last
/// line may omit it). Fields are separated by tabs:
///
/// `1`, the loadout digest as 64 lower-case hex digits, the mod key
/// likewise, the engine version (printable ASCII, no space, 1 to 64
/// bytes), the verdict word, the twelve counts in `ChangeCounts` order as
/// unsigned decimals without leading zeros joined by `,`, the mod label,
/// the reason.
///
/// A line is dropped when it has another field count, a form other than
/// `1`, a digest that is not 64 lower-case hex digits, a bad engine
/// version, an unknown verdict word, a count that is not a decimal within
/// `uint32_t`, a reason on a compatible or no-effect verdict, more than
/// `verdict_line_most_bytes` bytes, a carriage return, or bytes that are
/// not UTF-8. When two lines share a key, the later record replaces the
/// earlier and keeps the earlier line's place in age. At most the newest
/// `verdicts_most_lines` are kept; the oldest past that are dropped.
///
/// @param bytes the file contents
/// @return the verdicts kept, oldest first, at most `verdicts_most_lines`
[[nodiscard]] std::vector<RememberedVerdict> read_verdicts(std::span<const uint8_t> bytes);

/// Writes remembered verdicts as file bytes.
///
/// At most the newest `verdicts_most_lines` are written, oldest first.
/// The label and the reason pass through `one_line_reason`. A compatible
/// or no-effect verdict is written with an empty reason. A key whose
/// engine version is not the form the reader accepts is left out, so
/// every line written is a line the reader keeps.
///
/// @param verdicts the verdicts, oldest first
/// @return the file bytes, each line ending with a line feed
[[nodiscard]] std::vector<uint8_t> write_verdicts(std::span<const RememberedVerdict> verdicts);

/// Remembers one verdict, newest last.
///
/// A verdict with the same key replaces the stored one and moves to the
/// newest place. Any other verdict is appended. The oldest verdicts are
/// then dropped while more than `verdicts_most_lines` remain.
///
/// @param[in,out] verdicts the list, oldest first
/// @param verdict the verdict to remember
void remember_verdict(std::vector<RememberedVerdict>& verdicts, RememberedVerdict verdict);

/// Finds the record stored under a key.
///
/// @param verdicts the list to search
/// @param key the key
/// @return the record, or null when the key is absent
[[nodiscard]] const VerdictRecord*
find_verdict(std::span<const RememberedVerdict> verdicts, const VerdictKey& key) noexcept;

} // namespace oa::data::mutators
