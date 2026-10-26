// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// One mutator named on a command line or in a launch file: the text
// `id-or-path[:option=value,...]`, and the installed package that text
// names with every option filled in. Nothing here starts a match.
#pragma once

#include "oa/base/sha256.hpp"
#include "oa/data/mutators/loadout.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::data::mutator_library {
struct InstalledMutator;
struct Listing;
} // namespace oa::data::mutator_library

namespace oa::data::mutators {

/// One mutator a player named, before it is looked up.
///
/// `target` is an id when it holds no `/` and no `\`. Anything else is a
/// path to an unpacked package folder. A target that holds `:` is a path
/// only when it names an existing folder or file. `options` are the
/// settings the player wrote, in the order written. An option the text
/// leaves out takes the manifest's default when the choice is resolved.
struct MutatorChoice {
    /// The id, or the path, as the player wrote it.
    std::string target{};
    /// True when `target` is a path. False when it is an id.
    bool by_path{false};
    /// The named settings, each a name and the text of its value.
    std::vector<std::pair<std::string, std::string>> options{};
};

/// Reads one mutator choice.
///
/// An id holds no `/` and no `\`. Anything else is a path to a package
/// folder. On every platform, a text that starts with a drive letter, `:`
/// and `/` or `\`, as `C:/Mods/pack` does, keeps that `:` in the path. A
/// target that holds any other `:` is a path only when it names an existing
/// folder or file; otherwise the text is refused. Options follow the first
/// `:` after any drive that is followed by a name and `=`, and every name is
/// an option id and every value an identifier. An empty text, an empty name
/// or value, a repeated option, and a `,` or `=` inside a value are refused.
/// The text is not trimmed.
///
/// @param text the choice, as written
/// @param[out] error what is wrong, when the text is not a choice; may be null
/// @return the choice, or nothing when the text is not one
[[nodiscard]] std::optional<MutatorChoice>
parse_choice(std::string_view text, std::string* error = nullptr);

/// Writes a choice as `parse_choice` reads it.
///
/// The target comes first. When options were named, `:` and the pairs in
/// stored order follow, joined by `,`, with no spaces and no escaping.
///
/// @param choice the choice
/// @return its text
[[nodiscard]] std::string format_choice(const MutatorChoice& choice);

/// One mutator a choice named, with its package and its typed options.
///
/// `options` is every option of the manifest, in the manifest's order,
/// defaults filled and the named ones written over them. `package` is the
/// package file's SHA-256 from the installer's origin record, or, when
/// `folder` is set, the digest of the package folder.
struct ResolvedMutator {
    /// The choice that named it, as the player wrote it.
    MutatorChoice choice{};
    /// The package folder.
    std::filesystem::path package_folder{};
    /// The package's manifest.
    oa::formats::oamut::Manifest manifest{};
    /// Every option, defaults filled, in the manifest's order.
    std::vector<LoadoutOption> options{};
    /// The package file's SHA-256, or the folder digest when `folder` is set.
    base::sha256::Digest package{};
    /// True when the package is named by its folder digest: a path, or an
    /// installed folder whose origin record names no SHA-256.
    bool folder{false};
    /// The player's clash choice. Empty when the player did not choose,
    /// including a mutator named with `--mutator`. The picker's choice can
    /// fill it later. `resolve_loadout` sets it from the loadout entry, so
    /// a guest plays the mode the host recorded.
    std::optional<oa::formats::oamut::ClashMode> clashes{};
};

/// Finds the package a choice names and types its options.
///
/// An id is looked up in the installed listing of `mutators_folder`. A path
/// is the package folder itself, absolute or relative to the working
/// directory; the listing is not used. The manifest is `oamut.yaml`, matched
/// without ASCII case, and a symbolic link is not followed. An id keeps the
/// SHA-256 the origin record stored. When that record names no SHA-256, the
/// id keeps the folder digest of the installed folder and sets `folder`. A
/// recorded SHA-256 that is not 64 hexadecimal digits is refused. A path
/// keeps the folder digest, and sets `folder`, and the folder digest's
/// refusal is the choice's refusal. A missing mutator, an option the
/// manifest does not have, a value of the wrong kind and a value outside
/// the option's choices are refused. The words name the player's text.
///
/// @param choice the choice
/// @param mutators_folder the player's Mutators folder, for an id
/// @param[out] error what is wrong, when the choice cannot be used; may be null
/// @return the package and its typed options, or nothing when it cannot be used
[[nodiscard]] std::optional<ResolvedMutator> resolve_choice(
    const MutatorChoice& choice,
    const std::filesystem::path& mutators_folder,
    std::string* error = nullptr
);

/// Finds the package a choice names in a listing already made.
///
/// The same as `resolve_choice` with the Mutators folder, for a caller that
/// resolves several choices from one listing of that folder: an id is
/// looked up in `listing`, and a path is read as there.
///
/// @param choice the choice
/// @param listing the Mutators folder's listing, for an id
/// @param[out] error what is wrong, when the choice cannot be used; may be null
/// @return the package and its typed options, or nothing when it cannot be used
[[nodiscard]] std::optional<ResolvedMutator> resolve_choice(
    const MutatorChoice& choice,
    const oa::data::mutator_library::Listing& listing,
    std::string* error = nullptr
);

/// Types the options of an installed mutator a choice already names.
///
/// This is the id branch of `resolve_choice` after the Mutators folder has
/// been listed. It does not list that folder again. The package keeps the
/// SHA-256 the origin record stored. When that record names no SHA-256, it
/// keeps the folder digest and sets `folder`. A recorded SHA-256 that is not
/// 64 hexadecimal digits, a folder digest that cannot be made, an option the
/// manifest does not have, a value of the wrong kind and a value outside the
/// option's choices are refused. The words name the player's text. With
/// `digests`, the folder digest is that cache's, which reads the folder
/// again only when it changed.
///
/// @param choice the choice; its target is the mutator's id
/// @param installed the package the listing named for that id
/// @param[out] error what is wrong, when the choice cannot be used; may be null
/// @param digests the folder digests kept so far; null makes the digest afresh
/// @return the package and its typed options, or nothing when it cannot be used
[[nodiscard]] std::optional<ResolvedMutator> resolve_installed(
    const MutatorChoice& choice,
    const oa::data::mutator_library::InstalledMutator& installed,
    std::string* error = nullptr,
    FolderDigests* digests = nullptr
);

} // namespace oa::data::mutators
