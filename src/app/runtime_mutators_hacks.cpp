// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Lays a match's mutator hacks over the profile the match plays. Developer
// Mode's overrides are the baseline. The copy goes into the one profile
// object the run already plays; the match's teardown puts the player's
// profile back.

#include "oa/app/runtime.hpp"

#include "engine_settings_state.hpp"
#include "mutator_state.hpp"
#include "oa/app/mod_profile_loader.hpp"
#include "oa/data/mod_profile.hpp"
#include "oa/data/mod_profile/value.hpp"
#include "oa/data/mutators/hacks.hpp"
#include "oa/data/mutators/pipeline.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <optional>
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

/// Fails one mutator for its hacks and drops the records, so they are not played.
void fail_for_hacks(mutators::MutatorResult& one, mutators::Cause cause, std::string message) {
    one.hacks.clear();
    if (one.outcome != mutators::Outcome::applied)
        return;
    one.outcome = mutators::Outcome::failed;
    one.phase = mutators::Phase::plan;
    one.cause = cause;
    one.message = std::move(message);
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

/// Records one hack note on the journal and the conflict report.
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
    mutators::JournalEntry entry;
    entry.mutators = conflict.mutators;
    entry.phase = mutators::Phase::plan;
    entry.collection = mutators::Collection::other;
    entry.change = mutators::Change::set;
    entry.record = conflict.record;
    entry.new_text = note.text;
    result.report.conflicts.push_back(std::move(conflict));
    result.journal.entries.push_back(std::move(entry));
}

} // namespace

void Runtime::play_mutator_hacks(const std::vector<mutators::MutatorHack>& hacks) {
    if (hacks.empty() || mutator_state_ == nullptr || !mutator_state_->result.has_value())
        return;
    mutators::PipelineResult& result = *mutator_state_->result;
    const bool can_change = engine_settings_ != nullptr && engine_settings_->layered &&
                            engine_settings_->profile_source.has_value() &&
                            engine_settings_->played != nullptr;
    if (!can_change) {
        for (mutators::MutatorResult& one : result.mutators) {
            std::string id;
            for (const mutators::MutatorHack& hack : hacks) {
                if (hack.mutator != one.id)
                    continue;
                id = entry_of(hack.path);
                break;
            }
            if (id.empty())
                continue;
            fail_for_hacks(
                one,
                mutators::Cause::engine_error,
                "hacks: " + id + ": the game's rules cannot change in this run"
            );
        }
        return;
    }

    auto& settings = *engine_settings_;
    const ProfileSource& source = *settings.profile_source;
    std::vector<std::string> profile_on;
    {
        profiles::ResolveOptions bare = source.options;
        bare.match.clear();
        bare.overrides.clear();
        const profiles::ResolveResult resolved =
            profiles::resolve_profile(source.text, source.name, bare);
        if (resolved.resolution.has_value())
            profile_on = entries_on(*resolved.resolution);
    }

    const std::vector<profiles::HackOverride> overrides =
        settings.current.developer_mode ? settings.current.hack_overrides
                                        : std::vector<profiles::HackOverride>{};
    const auto resolve_with = [&](const std::vector<mutators::MutatorHack>& chosen) {
        profiles::ResolveOptions options = source.options;
        options.overrides = overrides;
        options.match.clear();
        for (const mutators::MutatorHack& hack : chosen) {
            profiles::MatchOption option;
            option.path = hack.path;
            option.value = hack.value;
            option.source = hack.mutator;
            option.turn_on = true;
            options.match.push_back(std::move(option));
        }
        return profiles::resolve_profile(source.text, source.name, options);
    };
    const auto still_applied = [&result](std::string_view mutator) {
        for (const mutators::MutatorResult& one : result.mutators) {
            if (one.id == mutator && one.outcome == mutators::Outcome::applied)
                return true;
        }
        return false;
    };
    const auto chosen_of = [&]() {
        std::vector<mutators::MutatorHack> chosen;
        for (const mutators::MutatorHack& hack : hacks) {
            if (still_applied(hack.mutator))
                chosen.push_back(hack);
        }
        return chosen;
    };

    std::vector<mutators::MutatorHack> chosen = chosen_of();
    profiles::ResolveResult resolved = resolve_with(chosen);
    if (!resolved.errors.empty()) {
        for (mutators::MutatorResult& one : result.mutators) {
            if (one.outcome != mutators::Outcome::applied)
                continue;
            std::vector<mutators::MutatorHack> own;
            for (const mutators::MutatorHack& hack : chosen) {
                if (hack.mutator == one.id)
                    own.push_back(hack);
            }
            std::string message = error_for(resolved.errors, own);
            if (message.empty())
                continue;
            fail_for_hacks(one, mutators::Cause::invalid_plan, std::move(message));
        }
        chosen = chosen_of();
        if (chosen.empty())
            return;
        resolved = resolve_with(chosen);
        if (!resolved.resolution.has_value()) {
            const std::string fallback = resolved.errors.empty()
                                             ? std::string{"the hacks were refused"}
                                             : resolved.errors.front().message;
            for (mutators::MutatorResult& one : result.mutators) {
                if (one.outcome != mutators::Outcome::applied)
                    continue;
                std::string id;
                for (const mutators::MutatorHack& hack : chosen) {
                    if (hack.mutator != one.id)
                        continue;
                    id = entry_of(hack.path);
                    break;
                }
                if (id.empty())
                    continue;
                std::vector<mutators::MutatorHack> own;
                for (const mutators::MutatorHack& hack : chosen) {
                    if (hack.mutator == one.id)
                        own.push_back(hack);
                }
                std::string message = error_for(resolved.errors, own);
                if (message.empty())
                    message = "hacks: " + id + ": " + fallback;
                fail_for_hacks(one, mutators::Cause::invalid_plan, std::move(message));
            }
            return;
        }
    }
    if (!resolved.resolution.has_value() || chosen.empty())
        return;

    *settings.played = resolved.resolution->profile;
    settings.plays_base_rules = false;

    const mutators::HackMerge merged = mutators::merge_hacks(chosen);
    const std::vector<mutators::HackNote> overlaps =
        mutators::developer_conflicts(merged.hacks, overrides, profile_on);
    for (const mutators::HackNote& note : merged.notes)
        record_note(result, note);
    for (const mutators::HackNote& note : overlaps)
        record_note(result, note);
    result.journal.order();

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
