// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The loadout record: which mutators a game names, the one canonical text of
// that record, and the digests a recording or a save is built against. Nothing
// here starts a match or runs a mutator.
#pragma once

#include "oa/base/sha256.hpp"
#include "oa/base/threads.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/formats/oamut/manifest.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::data::mutators {

/// One option value a loadout names.
///
/// A toggle's value is `toggle`. A choice's value, a number or a string, is
/// `value` as the manifest types it. A number option's value is `value` as a
/// number with its canonical text.
struct LoadoutOption {
    /// The option's id, as the manifest names it.
    std::string id{};
    /// Toggle, choice or number.
    oa::formats::oamut::OptionType type{};
    /// A toggle's value. False for a choice or a number.
    bool toggle{};
    /// A choice's value, or a number option's value. Unused for a toggle.
    oa::formats::oamut::ChoiceValue value{};

    /// Returns whether both options are the same member of the canonical text.
    ///
    /// Equality is the canonical JSON value, not the stored fields: the ids
    /// are equal, and the values are the same JSON kind with the same value.
    /// Toggles compare `toggle`, string choices compare `value.text`, and
    /// anything holding a number (a number option or a numeric choice)
    /// compares the number's canonical text. So 1 and 1.0 are equal, a
    /// numeric choice equals a number option of the same number, and a
    /// number's stored `text` and integer mark are not compared. A string
    /// choice never equals a number, whatever its text.
    ///
    /// @param other the option to compare
    /// @return true when both write the same id and canonical value
    [[nodiscard]] bool operator==(const LoadoutOption& other) const;
};

/// Returns a loadout option holding one manifest option's default.
///
/// A toggle copies `toggle_default`. A choice copies `values[default_index]`
/// when that index is in range, and an empty value otherwise. A number copies
/// `number_default` and its canonical text.
///
/// @param option the manifest option
/// @return the default as a loadout option, with the same id and type
[[nodiscard]] LoadoutOption default_option(const oa::formats::oamut::Option& option);

/// One mutator a loadout names.
///
/// `package` is the package file's SHA-256, or, when `folder` is set, the
/// digest of the package folder. `options` is every option of the manifest,
/// defaults filled, in id byte order. `clashes` is the mode in force for
/// that mutator: the player's choice, else the manifest, else the preference.
struct LoadoutEntry {
    /// The mutator's id.
    std::string id{};
    /// The mutator's version text.
    std::string version{};
    /// The package file's SHA-256, or the folder digest when `folder` is set.
    base::sha256::Digest package{};
    /// True when the package was a folder named by path, not a package file.
    bool folder{};
    /// Every option, in id byte order.
    std::vector<LoadoutOption> options{};
    /// The clash mode in force. Replace when the caller leaves it unset.
    oa::formats::oamut::ClashMode clashes{oa::formats::oamut::ClashMode::replace};

    /// Returns whether both entries write the same canonical JSON object.
    ///
    /// The id, version, package digest and folder mark are equal, and the
    /// options are the same members of the canonical text, compared as
    /// LoadoutOption compares them, whatever order they are listed in.
    /// Two entries are equal exactly when the canonical text writes them
    /// alike.
    ///
    /// @param other the entry to compare
    /// @return true when both have the same canonical object
    [[nodiscard]] bool operator==(const LoadoutEntry& other) const;
};

/// The mutators a game names, in the order they play.
struct Loadout {
    /// The mutators, in list order. At most loadout_most_entries.
    std::vector<LoadoutEntry> entries{};

    /// Returns whether both loadouts have the same canonical text.
    ///
    /// The entries are compared in list order, each as LoadoutEntry compares
    /// them. Two loadouts are equal exactly when `canonical_loadout` writes
    /// the same bytes for both, so a record read back with `parse_loadout`
    /// equals the one written.
    ///
    /// @param other the loadout to compare
    /// @return true when both have the same canonical text
    [[nodiscard]] bool operator==(const Loadout& other) const;
};

/// The most mutators one loadout names.
inline constexpr std::size_t loadout_most_entries = 32;

/// The most a recording's mutators sector holds, so every loadout
/// parse_loadout accepts can be recorded.
inline constexpr std::size_t loadout_text_most_bytes = 65529;

/// Writes a loadout as its one canonical text.
///
/// The text is the canonical JSON of `{"mutators":[...],"oaloadout":1}`.
/// Each entry is `clashes`, `folder`, `id`, `options`, `sha256` and
/// `version`, in list order. `clashes` is `add` or `replace`. `sha256` is
/// 64 lower-case hex digits. Options are members of one
/// object: a toggle is a boolean, a number is a number, and a choice is a
/// number or a string as the manifest types it. The object order does not
/// follow the order the options were given in, and no option is left out.
///
/// @param loadout the loadout
/// @return the canonical text, with no white space
[[nodiscard]] std::string canonical_loadout(const Loadout& loadout);

/// Reads a loadout from its canonical text.
///
/// Accepts only the text `canonical_loadout` writes: at most
/// loadout_text_most_bytes bytes and loadout_most_entries mutators, the known
/// keys only, `oaloadout` 1, lower-case hex, and no text before or after the
/// value. An option's JSON kind tells its type: true or false is a toggle, a
/// number is a number option (a numeric choice is written as a number too, so
/// it reads back as a number option with that number), and a string is a
/// choice. Writing the result again must give the same bytes, and the result
/// compares equal to any loadout with this canonical text.
///
/// @param text the text
/// @param[out] error what was refused, naming the part; may be null
/// @return the loadout, or nothing when the text is refused
[[nodiscard]] std::optional<Loadout> parse_loadout(std::string_view text, std::string* error);

/// Returns the SHA-256 of a loadout's canonical text.
///
/// @param loadout the loadout
/// @return the digest of `canonical_loadout`
[[nodiscard]] base::sha256::Digest loadout_digest(const Loadout& loadout);

/// Returns the digest of the regular files in a folder that a package carries.
///
/// The files are sorted by their relative path, compared bytewise. That
/// path is the UTF-8 spelling, with `/` between its parts, of the names as
/// the file system stores them, with no Unicode normalisation, so a name
/// stored composed and one stored decomposed give different digests. Each
/// file contributes that path, a NUL, its size as eight little-endian bytes,
/// and the SHA-256 of its bytes. The same names `oa-tool mutator pack` leaves
/// out are left out here: `__MACOSX` and `.backup` only as the first part,
/// `.git` and `.oa-origin.yaml` at any depth, and `.DS_Store` (any ASCII
/// case) and a name starting with `._` at any depth. A name that starts with
/// '.' and is none of those is hashed. A symbolic link, including one the
/// folder path itself is, anything that is neither a regular file nor a
/// folder (a pipe, a socket or a device), or a file that cannot be read
/// refuses the digest, including when that path would otherwise be left out.
/// The folder is only read.
///
/// @param folder the folder
/// @param[out] error the path, in UTF-8, and what was wrong; may be null
/// @return the digest, or nothing when one of those was found
[[nodiscard]] std::optional<base::sha256::Digest>
folder_digest(const std::filesystem::path& folder, std::string* error);

/// Folder digests, each kept until its folder changes.
///
/// `digest` answers what `folder_digest` answers. A folder it digested
/// before is read again only when an entry in it was added, removed or
/// renamed, or changed kind, size or last write time: one walk of the
/// folder that opens no file compares those first. An edit that keeps a
/// file's size and its last write time is not seen. A refused digest is
/// not kept. Several threads may share one cache: a folder is read outside
/// its lock, so two threads that ask for one folder at once may both read it.
class FolderDigests {
  public:

    /// The folder's digest, as `folder_digest` makes it.
    ///
    /// @param folder the folder
    /// @param[out] error the path, in UTF-8, and what was wrong; may be null
    /// @return the digest, or nothing when `folder_digest` refuses it
    [[nodiscard]] std::optional<base::sha256::Digest>
    digest(const std::filesystem::path& folder, std::string* error = nullptr);

    /// How many times a folder's files were read for its digest. A check reads this.
    ///
    /// @return the count
    [[nodiscard]] uint64_t reads() const noexcept { return reads_.load(); }

  private:

    /// A digest and the walk it was made after.
    struct Kept {
        std::string stamp{};           ///< each entry's path, kind, size and write time
        base::sha256::Digest digest{}; ///< the folder digest
    };

    base::threads::Mutex mutex_{};                 ///< guards `kept_`
    std::map<std::filesystem::path, Kept> kept_{}; ///< by folder
    std::atomic<uint64_t> reads_{0};               ///< folders read for a digest
};

/// Returns the digest of one mutator layer.
///
/// The message is `oa-mutator-layer-1` and a line feed, then each path the
/// layer answers for, sorted by its lower-case `/` form: that form, a NUL,
/// `F` and the SHA-256 of the bytes `served_bytes` gives for the path as the
/// layer stores it, or `H` and nothing further for a hidden entry. Then each
/// mask pattern, lower-cased with `/` separators and sorted: `M`, the
/// pattern and a NUL. The table's order, owners, archives and label are not
/// part of the message. `served_bytes` is not called for a hidden entry.
///
/// @param layer the layer
/// @param served_bytes the bytes a served path shows; called with the path as stored
/// @return the digest
[[nodiscard]] base::sha256::Digest layer_digest(
    const oa::MutatorLayerSpec& layer,
    const std::function<std::vector<uint8_t>(std::string_view path)>& served_bytes
);

/// Returns the digest of a loadout together with its layer.
///
/// The message is `oa-mutators-1` and a line feed, the canonical loadout and
/// the layer digest.
///
/// @param loadout the loadout
/// @param layer the layer digest
/// @return the digest
[[nodiscard]] base::sha256::Digest
mutator_digest(const Loadout& loadout, const base::sha256::Digest& layer);

/// Folds a mutator digest into a simulation hash.
///
/// With no digest the simulation hash is returned unchanged. Otherwise the
/// result is the SHA-256 of `oa-sim-hash-mutators-1` and a line feed, the
/// simulation hash and the mutator digest.
///
/// @param sim_hash the simulation hash before mutators
/// @param mutators the mutator digest, or nothing when no mutator plays
/// @return `sim_hash` when `mutators` is empty, otherwise the folded digest
[[nodiscard]] base::sha256::Digest fold_sim_hash(
    const base::sha256::Digest& sim_hash, const std::optional<base::sha256::Digest>& mutators
);

/// Writes one option's value as the options text spells it.
///
/// A toggle is `true` or `false`. Anything holding a number (a number
/// option or a numeric choice) is the number's canonical text, the same
/// digits the canonical loadout writes, whatever `value.text` holds. A
/// string choice is its text, unescaped. The option's id is not part of it.
///
/// @param option the option
/// @return the value text, the part after `=` in `options_text`
[[nodiscard]] std::string option_text(const LoadoutOption& option);

/// Writes an entry's options as `name=value` pairs joined by `,`.
///
/// Names are in byte order. Each value is `option_text` of the option. No
/// options write nothing. This is the text that follows `id:` on
/// `--mutator`.
///
/// @param entry the entry
/// @return the text, or empty when the entry has no options
[[nodiscard]] std::string options_text(const LoadoutEntry& entry);

struct ResolvedMutator;
struct PipelineResult;

/// Builds the loadout a chosen list names.
///
/// Every mutator is listed, in the list's order. Each option is that
/// mutator's typed value, defaults already filled, stored in id byte order.
/// The package digest and the folder mark are copied from the resolved
/// mutator. `clashes` is the mode in force: the player's choice, else the
/// manifest's `clashes`, else `clash_default`. No file is read.
///
/// @param chosen the mutators, top first
/// @param clash_default the preference, used when the player and the manifest
///        both leave the mode unset
/// @return the loadout
[[nodiscard]] Loadout loadout_of(
    std::span<const ResolvedMutator> chosen,
    oa::formats::oamut::ClashMode clash_default = oa::formats::oamut::ClashMode::replace
);

/// Keeps the mutators a pipeline run applied.
///
/// The result lists one outcome per mutator of `chosen`, in the same order.
/// An applied mutator stays, in that order. One that failed, was
/// incompatible or was excluded is left out.
///
/// @param chosen the loadout the run was given
/// @param result what the run decided
/// @return the applied mutators
[[nodiscard]] Loadout applied_loadout(const Loadout& chosen, const PipelineResult& result);

/// Finds the installed packages a loadout names.
///
/// Each entry is looked up in `mutators_folder` by id. The installed
/// manifest's version text must be the entry's. A package entry's SHA-256
/// must be the origin record's. A folder entry is accepted when the
/// installed folder of that id has the same folder digest, and refused,
/// naming it as a package folder, when it does not. The options are typed
/// as `resolve_choice` types them from `options_text`. The entry's
/// `clashes` is copied onto the resolved mutator, so it wins over the
/// reader's preference. A missing package, another version and another
/// SHA-256 are refused, and the error names the entry.
///
/// @param loadout the loadout
/// @param mutators_folder the player's Mutators folder
/// @param[out] error what was refused, naming the entry; may be null
/// @return the packages, in the loadout's order, or nothing when one entry is refused
[[nodiscard]] std::optional<std::vector<ResolvedMutator>> resolve_loadout(
    const Loadout& loadout,
    const std::filesystem::path& mutators_folder,
    std::string* error = nullptr
);

} // namespace oa::data::mutators
