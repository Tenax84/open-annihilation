// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Lays a match's mutator hacks over the profile the match plays. Developer
// Mode's overrides are the baseline. The pipeline has already refused a hack
// that does not resolve, so this copies that resolution into the one profile
// the run already plays. The match's release puts the player's profile back.

#include "oa/app/runtime.hpp"

#include "engine_settings_state.hpp"
#include "mutator_state.hpp"
#include "oa/app/mod_profile_loader.hpp"
#include "oa/data/mod_profile.hpp"
#include "oa/data/mod_profile/value.hpp"
#include "oa/data/mutators/hacks.hpp"
#include "oa/data/mutators/pipeline.hpp"

#include <cstdint>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

namespace profiles = oa::data::mod_profile;
namespace mutators = oa::data::mutators;

/// The registry entry a parameter path names. Parameter names have no dot.
std::string entry_of(std::string_view path) {
    const std::size_t dot = path.rfind('.');
    if (dot == std::string_view::npos)
        return std::string{path};
    return std::string{path.substr(0, dot)};
}

/// Reports whether a resolve error names this parameter or its entry.
bool names_hack(const profiles::Diagnostic& diagnostic, std::string_view path) {
    if (diagnostic.path == "match " + std::string{path})
        return true;
    return diagnostic.path == entry_of(path);
}

/// The first resolve error that names one of a mutator's hacks.
std::string error_for(
    const std::vector<profiles::Diagnostic>& errors, const std::vector<mutators::MutatorHack>& hacks
) {
    for (const profiles::Diagnostic& diagnostic : errors) {
        for (const mutators::MutatorHack& hack : hacks) {
            if (!names_hack(diagnostic, hack.path))
                continue;
            return "hacks: " + entry_of(hack.path) + ": " + diagnostic.message;
        }
    }
    return {};
}

/// The entries a profile turns on before overrides and match choices.
std::vector<std::string> entries_on(const profiles::Resolution& resolution) {
    std::vector<std::string> on;
    if (const profiles::Value* hacks = profiles::find_member(resolution.effective, "hacks");
        hacks != nullptr) {
        for (const profiles::Member& member : hacks->members)
            on.push_back(member.key);
    }
    for (const profiles::Provenance& provenance : resolution.provenance) {
        if (!provenance.path.starts_with("limits.") || provenance.source == "baseline")
            continue;
        const std::string entry = entry_of(provenance.path);
        bool known = false;
        for (const std::string& id : on) {
            if (id == entry)
                known = true;
        }
        if (!known)
            on.push_back(entry);
    }
    return on;
}

/// Records one hack note on the conflict report. It is not a journal entry,
/// so the applied line does not count it as a rewritten file.
void record_note(mutators::PipelineResult& result, const mutators::HackNote& note) {
    mutators::Conflict conflict;
    conflict.severity = mutators::Severity::note;
    conflict.kind = mutators::ConflictKind::hack_note;
    conflict.record = "hacks:" + note.entry;
    conflict.detail = note.text;
    const auto slot_of = [&result](std::string_view id) -> std::optional<uint16_t> {
        for (std::size_t slot = 0; slot < result.mutators.size(); ++slot) {
            if (result.mutators[slot].id == id)
                return static_cast<uint16_t>(slot);
        }
        return std::nullopt;
    };
    if (!note.earlier.empty()) {
        if (const std::optional<uint16_t> earlier = slot_of(note.earlier); earlier.has_value())
            conflict.mutators.push_back(*earlier);
    }
    if (const std::optional<uint16_t> winner = slot_of(note.mutator); winner.has_value())
        conflict.mutators.push_back(*winner);
    result.report.conflicts.push_back(std::move(conflict));
}

/// Resolves the profile with these hacks as match choices that turn entries on.
profiles::ResolveResult resolve_with(
    const ProfileSource& source,
    const std::vector<profiles::HackOverride>& overrides,
    std::span<const mutators::MutatorHack> hacks
) {
    profiles::ResolveOptions options = source.options;
    options.overrides = overrides;
    options.match.clear();
    for (const mutators::MutatorHack& hack : hacks) {
        profiles::MatchOption option;
        option.path = hack.path;
        option.value = hack.value;
        option.source = hack.mutator;
        option.turn_on = true;
        options.match.push_back(std::move(option));
    }
    return profiles::resolve_profile(source.text, source.name, options);
}

/// One fault per mutator whose hacks the resolution refused.
std::vector<mutators::HackCheckError>
faults_for(const profiles::ResolveResult& resolved, std::span<const mutators::MutatorHack> hacks) {
    std::vector<mutators::HackCheckError> faults;
    std::vector<std::string> named;
    const std::string fallback = resolved.errors.empty() ? std::string{"the hacks were refused"}
                                                         : resolved.errors.front().message;
    for (const mutators::MutatorHack& hack : hacks) {
        bool seen = false;
        for (const std::string& id : named) {
            if (id == hack.mutator)
                seen = true;
        }
        if (seen)
            continue;
        named.push_back(hack.mutator);
        std::vector<mutators::MutatorHack> own;
        for (const mutators::MutatorHack& one : hacks) {
            if (one.mutator == hack.mutator)
                own.push_back(one);
        }
        std::string message = error_for(resolved.errors, own);
        std::string path = entry_of(hack.path);
        if (message.empty())
            message = "hacks: " + path + ": " + fallback;
        else
            path = entry_of(own.front().path);
        for (const profiles::Diagnostic& diagnostic : resolved.errors) {
            for (const mutators::MutatorHack& one : own) {
                if (!names_hack(diagnostic, one.path))
                    continue;
                path = diagnostic.path;
                break;
            }
        }
        faults.push_back(
            mutators::HackCheckError{hack.mutator, std::move(path), std::move(message)}
        );
    }
    return faults;
}

/// The faults in a planned hack list, or none when the profile accepts them.
std::vector<mutators::HackCheckError> check_planned(
    bool can_change,
    const ProfileSource* source,
    const std::vector<profiles::HackOverride>& overrides,
    std::span<const mutators::MutatorHack> hacks
) {
    if (hacks.empty())
        return {};
    if (!can_change || source == nullptr) {
        std::vector<mutators::HackCheckError> faults;
        std::vector<std::string> named;
        for (const mutators::MutatorHack& hack : hacks) {
            bool seen = false;
            for (const std::string& id : named) {
                if (id == hack.mutator)
                    seen = true;
            }
            if (seen)
                continue;
            named.push_back(hack.mutator);
            const std::string entry = entry_of(hack.path);
            faults.push_back(
                mutators::HackCheckError{
                    hack.mutator,
                    entry,
                    "hacks: " + entry + ": the game's rules cannot change in this run"
                }
            );
        }
        return faults;
    }
    const profiles::ResolveResult resolved = resolve_with(*source, overrides, hacks);
    if (resolved.resolution.has_value())
        return {};
    return faults_for(resolved, hacks);
}

/// Fails one mutator for its hacks and drops the records.
void fail_for_hacks(mutators::MutatorResult& one, std::string message) {
    one.hacks.clear();
    if (one.outcome != mutators::Outcome::applied)
        return;
    one.outcome = mutators::Outcome::failed;
    one.phase = mutators::Phase::plan;
    one.cause = mutators::Cause::hack_refused;
    one.message = std::move(message);
}

} // namespace

void Runtime::bind_planned_hack_check(mutators::PipelineInput& input) {
    const EngineSettingsState* settings = engine_settings_.get();
    const bool can_change = settings != nullptr && settings->layered &&
                            settings->profile_source.has_value() && settings->played != nullptr;
    const ProfileSource* source = can_change ? &(*settings->profile_source) : nullptr;
    std::vector<profiles::HackOverride> overrides;
    if (can_change && settings->current.developer_mode)
        overrides = settings->current.hack_overrides;
    input.check_hacks = [can_change, source, overrides](
                            std::span<const mutators::MutatorHack> hacks
                        ) { return check_planned(can_change, source, overrides, hacks); };
}

std::optional<uint16_t> Runtime::mutator_units_per_player() const noexcept {
    if (mutator_state_ == nullptr || !mutator_state_->hacks_played)
        return std::nullopt;
    return mutator_state_->units_per_player;
}

void Runtime::play_mutator_hacks() {
    if (mutator_state_ == nullptr || !mutator_state_->result.has_value())
        return;
    mutators::PipelineResult& result = *mutator_state_->result;
    std::vector<mutators::MutatorHack> hacks;
    for (const mutators::MutatorResult& one : result.mutators) {
        if (one.outcome != mutators::Outcome::applied)
            continue;
        hacks.insert(hacks.end(), one.hacks.begin(), one.hacks.end());
    }
    if (hacks.empty())
        return;

    // The pipeline judged these same records. A refusal here means the two
    // checks disagreed, so the start stops instead of keeping a half-applied
    // mutator.
    const EngineSettingsState* settings_now = engine_settings_.get();
    const bool can_change = settings_now != nullptr && settings_now->layered &&
                            settings_now->profile_source.has_value() &&
                            settings_now->played != nullptr;
    const ProfileSource* source = can_change ? &(*settings_now->profile_source) : nullptr;
    const std::vector<profiles::HackOverride> overrides =
        can_change && settings_now->current.developer_mode ? settings_now->current.hack_overrides
                                                           : std::vector<profiles::HackOverride>{};
    const std::vector<mutators::HackCheckError> faults =
        check_planned(can_change, source, overrides, hacks);
    if (!faults.empty()) {
        for (const mutators::HackCheckError& fault : faults) {
            for (mutators::MutatorResult& one : result.mutators) {
                if (one.id == fault.mutator)
                    fail_for_hacks(one, fault.message);
            }
        }
        const std::string reason = faults.front().mutator + ": plan: " + faults.front().message;
        play_latest_profile();
        release_match_mutators();
        throw std::runtime_error(reason);
    }

    auto& settings = *engine_settings_;
    std::vector<std::string> profile_on;
    {
        profiles::ResolveOptions bare = settings.profile_source->options;
        bare.match.clear();
        bare.overrides.clear();
        const profiles::ResolveResult plain = profiles::resolve_profile(
            settings.profile_source->text, settings.profile_source->name, bare
        );
        if (plain.resolution.has_value())
            profile_on = entries_on(*plain.resolution);
    }
    const profiles::ResolveResult resolved =
        resolve_with(*settings.profile_source, overrides, hacks);
    if (!resolved.resolution.has_value()) {
        const std::vector<mutators::HackCheckError> again = faults_for(resolved, hacks);
        const std::string reason = again.empty()
                                       ? std::string{"the hacks were refused"}
                                       : again.front().mutator + ": plan: " + again.front().message;
        play_latest_profile();
        release_match_mutators();
        throw std::runtime_error(reason);
    }

    *settings.played = resolved.resolution->profile;
    settings.plays_base_rules = false;
    mutator_state_->hacks_played = true;
    mutator_state_->units_per_player.reset();
    for (const profiles::Provenance& provenance : resolved.resolution->provenance) {
        if (provenance.path != "limits.units-per-player.default")
            continue;
        if (!provenance.source.starts_with("match"))
            continue;
        mutator_state_->units_per_player = settings.played->limits.units_per_player.default_limit;
        break;
    }

    const mutators::HackMerge merged = mutators::merge_hacks(hacks);
    const std::vector<mutators::HackNote> overlaps =
        mutators::developer_conflicts(merged.hacks, overrides, profile_on);
    for (const mutators::HackNote& note : merged.notes)
        record_note(result, note);
    for (const mutators::HackNote& note : overlaps)
        record_note(result, note);

    std::string line = "open-annihilation: mutators: hacks ";
    for (std::size_t slot = 0; slot < merged.hacks.size(); ++slot) {
        if (slot != 0)
            line += ", ";
        line += merged.hacks[slot].path;
        line += '=';
        line += merged.hacks[slot].value;
    }
    std::cerr << line << '\n';
}

} // namespace oa::app
