// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Installed mutators, read from each folder's manifest. The Library, the
// pipeline and the battle room share this list. It never reads or runs a
// script.
#pragma once

#include "oa/formats/oamut/manifest.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace oa::data::mutator_library {

/// The folder of the player's own folder that holds installed mutators.
inline constexpr std::string_view mutators_folder_name = "Mutators";

/// One installed mutator, as its folder's manifest and origin record state it.
///
/// The origin facts are empty, or 0, when the folder has no readable origin
/// record. A missing record is not a problem: the folder may have been made
/// by hand.
struct InstalledMutator {
    std::string folder_name{};               ///< the folder's name in Mutators
    std::filesystem::path folder{};          ///< the folder
    oa::formats::oamut::Manifest manifest{}; ///< its oamut.yaml
    std::string package_sha256{};            ///< the package file's SHA-256, or empty
    std::string registry{};                  ///< a catalogue origin's registry; empty otherwise
    std::string catalogue_id{};              ///< a catalogue origin's package key; empty otherwise
    int64_t release{};                       ///< a catalogue origin's release; 0 otherwise
};

/// What a Mutators folder holds, and the folders that could not be listed.
struct Listing {
    std::vector<InstalledMutator> mutators{}; ///< installed mutators, sorted by id
    std::vector<std::string> problems{};      ///< folders left out, and why
};

/// Lists the mutators installed in a Mutators folder.
///
/// A missing folder is an empty listing. A folder whose name starts with '.'
/// is the installer's own and is skipped, and so is a symbolic link, which
/// is never followed. A plain file at the top is neither listed nor opened.
/// Each real directory is read for `oamut.yaml` (at most 256 KiB) and
/// `.oa-origin.yaml` (at most 4,096 bytes) and for nothing else. A folder
/// with no readable manifest, or one with problems, is left out and named in
/// `problems`. Two folders with one id keep the folder named as the id, or
/// else the first by name, and the other is named in `problems`.
///
/// @param mutators_folder the Mutators folder
/// @return the mutators and the problems
[[nodiscard]] Listing list_installed(const std::filesystem::path& mutators_folder);

/// Finds a listed mutator by its id.
///
/// @param listing a listing from list_installed
/// @param id the mutator's id
/// @return the mutator in `listing`, or null when none has that id
[[nodiscard]] const InstalledMutator* find(const Listing& listing, std::string_view id) noexcept;

} // namespace oa::data::mutator_library
