// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// One ordered run of mutators: plan, files, parse, then each mutator's
// rules and hooks, then only the documents that changed. A mutator that
// fails ends the pass, and the run is repeated without it, so it leaves
// no file, rule or note.
#pragma once

#include "oa/data/defs/layout.hpp"
#include "oa/data/mutators/journal.hpp"
#include "oa/data/mutators/loadout.hpp"
#include "oa/data/script_host/script_host.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/formats/oamut/manifest.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace oa::data::mutators {

/// The most mutators one loadout may list.
inline constexpr std::size_t max_mutators = 32;

/// What the pipeline decided for one mutator.
enum class Outcome : uint8_t {
    applied,      ///< the mutator is in the layer that was returned
    failed,       ///< the mutator stopped, and nothing of it remains
    incompatible, ///< the manifest does not fit this mod or this engine
    excluded,     ///< the list was refused, or a mutator it requires did not apply
};

/// Why a mutator failed, was incompatible or was excluded.
enum class Cause : uint8_t {
    none,                ///< the mutator applied, or the whole list was refused
    script_error,        ///< the script stopped with an error
    instruction_budget,  ///< the script used its instruction allowance
    memory_budget,       ///< the script used its memory allowance
    time_limit,          ///< a phase of the script ran past its real-time limit
    string_length,       ///< the script made a string past the length cap
    invalid_plan,        ///< mutate did not return a plan this engine accepts
    undeclared_layer,    ///< the plan uses a layer the manifest does not declare
    files_refused,       ///< the files layer refused the mutator
    protected_path,      ///< a path is not one a mutator may change
    unreadable_rules,    ///< a rule file could not be read or parsed
    works_with,          ///< the mutator does not work with this mod
    engine_version,      ///< the engine is outside the mutator's requirement
    prerequisite_failed, ///< a mutator it requires did not apply
    engine_error,        ///< the engine stopped while running the mutator
    weapon_ids,          ///< a weapon this mutator brought could not be given an id
};

/// One mutator's outcome.
struct MutatorResult {
    /// The mutator's id.
    std::string id;
    /// The mutator's version, as its manifest writes it.
    std::string version;
    /// Applied, failed, incompatible or excluded.
    Outcome outcome{};
    /// The phase the outcome belongs to. An excluded mutator's phase is
    /// `loadout`: the list decided it, whether it was refused or a mutator
    /// it requires did not apply.
    Phase phase{};
    /// Why, or `none` when the mutator applied or the list was refused.
    Cause cause{};
    /// What happened, in plain words. Empty when there is nothing to add.
    std::string message;
};

/// The mod `ctx.mod` shows the scripts.
struct ModIdentity {
    /// The mod's id.
    std::string id;
    /// The mod's version text.
    std::string version;
    /// The mod's game id.
    std::string game;
};

/// One mutator in the loadout.
struct PipelineMutator {
    /// The installed package folder.
    std::filesystem::path package;
    /// The manifest. Its entry defaults to `mutator.lua`.
    oa::formats::oamut::Manifest manifest;
    /// The player's settings, in the loadout's own form. An option left out
    /// takes the manifest's default, as `default_option` gives it. An id the
    /// manifest does not define is ignored.
    std::vector<LoadoutOption> options;
};

/// What one run needs. The same input produces the same layer, journal and report.
struct PipelineInput {
    /// The mutators, top of the list first.
    std::vector<PipelineMutator> mutators;
    /// The mod the scripts see.
    ModIdentity mod;
    /// The engine version the manifest's requirement is compared with.
    std::string engine_version;
    /// Directory names. Null uses the layout in force.
    const oa::data::defs::DataLayout* layout{};
    /// Game-data variant word. Empty means the base files.
    std::string variant;
    /// The match seed. Each mutator's draws start from this and its id.
    uint64_t seed{};
    /// Instruction and memory allowances. The default is the mutator profile's.
    oa::data::script_host::Budget budget{};
};

/// What one run produced.
///
/// The store must hold no mutator layer when the run starts. A layer already
/// mounted is left as it was, and `refusal` says so. On return the final
/// layer is mounted when it holds at least one entry or mask. The caller
/// unmounts it with `unmount_mutator_layer`. An empty layer is not left
/// mounted.
struct PipelineResult {
    /// Why the whole list was refused. Empty when the run finished.
    std::string refusal;
    /// One result per input mutator, in list order.
    std::vector<MutatorResult> mutators;
    /// The layer left mounted, or empty when nothing was published.
    oa::MutatorLayerSpec layer;
    /// The changes the applied mutators made.
    Journal journal;
    /// The stacking notes of the applied mutators, and documents left unread.
    ConflictReport report;

    /// Reports whether every mutator applied and the list was not refused.
    ///
    /// @return true when the layer is the whole list
    [[nodiscard]] bool playable() const noexcept;
};

/// Runs the mutators in `input` against `store`.
///
/// The list is checked first. More than `max_mutators`, an id twice, a
/// requirement an earlier mutator does not meet, or two mutators that
/// conflict, refuses the list: every result is excluded, nothing is
/// mounted and no script runs. Each remaining manifest is checked against
/// the mod and the engine. The rest are planned, their files mounted, the
/// documents parsed from that layer. Record files are not mounted as whole
/// files: each mutator in list order applies the units, weapons and download
/// entries in its own copies, then its rules and then its hooks. A plan that
/// returns map hooks is
/// refused, so none run. Each pass over the list is a transaction. A
/// failure in a plan or a rule target is the mutator's own, and every one
/// is kept. Any later failure can depend on the mutators above it, so the
/// pass keeps only the one nearest the top of the list, and stops at the
/// first hook that fails: no later mutator runs on what a failed mutator left.
/// The mutator that failed, and every mutator that requires it, is then
/// dropped and the pass repeated from the store, so the result is the run
/// without them. Only a document the run changed is written back. After the
/// last hook, every weapon a mutator brought, by a record file or by
/// `weapons:clone`, is given an id. The game's and the mod's weapons keep
/// theirs. A requested `ID=` from 1 to 255 is kept when that id is free;
/// the rest take free ids from 1 to 255, in list order and then by name.
/// Slot 0 is never given out. When no id remains, the mutator whose weapon
/// could not be numbered fails in the integrity phase and the run is
/// repeated without it. The written weapon files carry the final ids. A plan
/// key this engine does not support yet is refused, not ignored.
///
/// The call does not throw. A Lua error stays inside the run. The result
/// names the mutator and the phase.
///
/// @param store the mod's files. It must hold no mutator layer
/// @param input the loadout, the mod, the engine version, the layout and the seed
/// @return each mutator's outcome, the layer, the journal and the conflicts
[[nodiscard]] PipelineResult run_pipeline(oa::AssetStore& store, const PipelineInput& input);

} // namespace oa::data::mutators
