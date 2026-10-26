// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A dry run of one mutator list against one mod's files, with no match.
// The job is everything the run needs. The caller keeps the live store.
#pragma once

#include "oa/data/mutators/pipeline.hpp"
#include "oa/data/mutators/verdicts.hpp"

#include <atomic>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::data::mutators {

/// The reason a cancelled dry run reports. A remembered file skips this one.
inline constexpr std::string_view dry_run_cancelled_reason = "the check was cancelled";

/// The verdict of the last mutator in a pipeline result.
///
/// A list's verdict is that mutator's alone. A prerequisite is in the list
/// because it changes what the last mutator does, and its own changes are
/// not this verdict. Counts are filled only for a compatible verdict, and
/// they count distinct records and paths, not journal lines: two writes of
/// one field are one record. A record that was added or removed is not also
/// counted as changed. A shared add is neither. A sides or tables write
/// whose text differs is compatible and leaves every count at zero.
/// `mod_label` passes through `one_line_reason`. A dry run has no map, so a
/// maps hook never runs. When the run changed nothing and the last mutator
/// of `mutators` declares the maps layer, the verdict is compatible with
/// every count at zero: the map changes when a match starts. It is not
/// `no_effect`. A prerequisite that declares maps does not make it so.
///
/// @param result the pipeline's result
/// @param mod_label the mod's name and version, empty for the base game
/// @param mutators the list that ran, so a maps layer can be seen; empty
///        leaves a run that changed nothing as `no_effect`
/// @return the last mutator's verdict
[[nodiscard]] VerdictRecord verdict_of(
    const PipelineResult& result,
    std::string mod_label,
    std::span<const PipelineMutator> mutators = {}
);

/// One dry run, ready for any thread. It holds no runtime and no live store.
struct DryRunJob {
    /// Loose folders, highest precedence first. Empty cannot build a store.
    std::vector<std::filesystem::path> loose_roots;
    /// Archives to mount, in order, after the loose folders.
    std::vector<std::filesystem::path> mounts;
    /// The pipeline input. A dry run leaves `map` unset.
    PipelineInput input;
    /// The loadout, the mod and the engine version this verdict is stored under.
    VerdictKey key;
    /// The mod's name and version as a player reads them. Empty for the base game.
    std::string mod_label;
};

/// Runs one dry run on a store built from the job, then unmounts that store.
///
/// The job is the only input. Cancel checked before the store is built, and
/// again before the pipeline runs, returns `failed` with
/// `dry_run_cancelled_reason` and does not run the pipeline. A cancel that
/// arrives during the pipeline does not replace the verdict. An empty root
/// list or a mount that cannot be opened is `failed`, and nothing is thrown.
///
/// @param job the prepared run
/// @param cancel set when the caller no longer wants the result
/// @return the last mutator's verdict, or a cancelled or failed record
[[nodiscard]] VerdictRecord run_dry_run(const DryRunJob& job, const std::atomic<bool>& cancel);

} // namespace oa::data::mutators
