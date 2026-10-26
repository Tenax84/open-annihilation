// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The oamut kind: a mutator's manifest, what a folder of Mutators holds,
// where a mutator installs, and the words the player is shown.
#pragma once

#include "oa/app/package_install.hpp"
#include "oa/app/package_install/prompts.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace oa::app::package_install::oamut {

/// The extension of a mutator package, matched without case.
inline constexpr std::string_view oamut_extension = ".oamut";

/// The folder of the player's own folder that holds installed mutators.
///
/// The same name as oa::data::mutator_library::mutators_folder_name. This
/// header does not include that library: the module links it privately, and
/// a public header would then fail to compile for a target that does not.
inline constexpr std::string_view mutators_folder_name = "Mutators";

/// The most bytes of a mutator's icon. A larger file is refused.
inline constexpr uint64_t most_icon_bytes = uint64_t{256} << 10;

/// The most pixels on one side of a mutator's icon.
inline constexpr uint32_t most_icon_side = 256;

/// The most bytes of a Lua source a mutator may hold.
///
/// H02's max_source_bytes is 1 MiB. This module does not link the script
/// host, so the number is written here.
inline constexpr uint64_t most_script_bytes = uint64_t{1} << 20;

/// Reads an oamut package's manifest, checks the files it may hold, that
/// every Lua file is source of at most 1 MiB, that its icon is a square PNG
/// within the limits, and that requires.engine is met by this build. Fills
/// package.incoming. No script is parsed or run.
///
/// @param manifest the manifest's bytes
/// @param context the options, the source name and a reader for the scripts
///        and the icon
/// @param[in,out] package receives what the mutator installs
/// @param[out] problem why it was refused
/// @return true when the package can be installed
[[nodiscard]] bool read_manifest(
    std::span<const uint8_t> manifest,
    const ManifestContext& context,
    Package& package,
    Problem& problem
);

/// Reads what a folder in Mutators holds: its oamut.yaml's id, name and
/// version.
///
/// @param folder the folder
/// @return what it holds; other when it is not a folder of a mutator
[[nodiscard]] InstalledPackage read_installed(const std::filesystem::path& folder);

/// Reads what a mutator's .backup holds.
///
/// @param folder the mutator's folder
/// @return nothing when it has no .backup; else what it holds, of kind
///         package only when it is a folder, not a link, whose oamut.yaml
///         gives the folder's own id
[[nodiscard]] std::optional<InstalledPackage> read_backup(const std::filesystem::path& folder);

/// Plans an install from what Mutators/<id> holds and from both origin records.
///
/// A missing folder is installed with no question when the package came from
/// a catalogue, and asks first when the player opened the file. Nothing is
/// installed alongside. A catalogue package whose target's record names the
/// same registry replaces that folder with no question. The same id from
/// another registry asks. The same version asks to reinstall, another version
/// to update. A folder without this mutator's oamut.yaml is left alone and
/// the install refused. The incoming revision is always 0.
///
/// @param incoming the mutator the package installs, origin included
/// @param hooks what the folder holds
/// @return the plan
[[nodiscard]] InstallPlan plan_install(const Incoming& incoming, const FolderHooks& hooks);

/// Returns the prompt shown while a mutator is checked, unpacked or put in
/// place (INSTALLING MUTATOR).
///
/// @param incoming the mutator
/// @param done_bytes the bytes hashed or unpacked so far
/// @param total_bytes the package file's size, or the bytes it unpacks to
/// @param phase what the prompt says
/// @param file_name the package's name, which the checking phase names
/// @return the prompt: CANCEL
[[nodiscard]] PackagePrompt installing_prompt(
    const Incoming& incoming,
    uint64_t done_bytes,
    uint64_t total_bytes,
    InstallingPhase phase,
    std::string_view file_name
);

/// Returns the question a plan asks.
///
/// A new mutator the player opened asks "Install the mutator {name}
/// {version}?" with Install and Cancel. An update and a reinstall name the
/// versions, and a replace across registries names both. Nothing offers
/// Install alongside.
///
/// @param plan the plan, one of the ask kinds
/// @param incoming the mutator
/// @param file_name the package's name
/// @param root the Mutators folder
/// @param played unused; a mutator is never what the game plays
/// @return the prompt
[[nodiscard]] PackagePrompt question_prompt(
    const InstallPlan& plan,
    const Incoming& incoming,
    std::string_view file_name,
    const std::filesystem::path& root,
    bool played
);

/// Returns the prompt that says a mutator was installed (MUTATOR INSTALLED).
///
/// It tells the player to turn the mutator on with the Mutators button when setting up
/// a game. PLAY NOW is not offered: installing never switches a mutator on.
///
/// @param incoming the mutator
/// @param folder its folder
/// @param play_now unused; PLAY NOW is not offered
/// @return the prompt: OPEN FOLDER, OK
[[nodiscard]] PackagePrompt
installed_prompt(const Incoming& incoming, const std::filesystem::path& folder, bool play_now);

/// Returns the prompt that says a mutator's folder was updated or installed
/// again (MUTATOR UPDATED).
///
/// @param change what was done
/// @param now what the folder holds now
/// @param before what it held before
/// @param folder the folder
/// @param result what the change did
/// @param play_now unused; PLAY NOW is not offered
/// @return the prompt: OPEN FOLDER, OK
[[nodiscard]] PackagePrompt updated_prompt(
    Change change,
    const Incoming& now,
    const InstalledPackage& before,
    const std::filesystem::path& folder,
    const ChangeResult& result,
    bool play_now
);

/// Returns the sentence that says why a mutator was not installed.
///
/// @param problem the problem
/// @return the sentence
[[nodiscard]] std::string refusal_text(const Problem& problem);

/// Returns the prompt that says a mutator was not installed (MUTATOR NOT
/// INSTALLED): the reason, and at most three of its manifest's diagnostics.
///
/// @param file_name the package's name
/// @param problem why
/// @param change_failed a change was tried and undone
/// @return the prompt: OK
[[nodiscard]] PackagePrompt
refused_prompt(std::string_view file_name, const Problem& problem, bool change_failed);

} // namespace oa::app::package_install::oamut
