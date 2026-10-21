// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The mutators a run chose, applied at every match start and released when
// the match ends or the start fails. An empty choice changes nothing.

#include "oa/app/runtime.hpp"

#include "oa/base/sha256.hpp"
#include "oa/data/content_bundles/content_choice.hpp"
#include "oa/data/defs/layout.hpp"
#include "oa/data/mod_profile/overrides.hpp"
#include "oa/data/mutator_library/library.hpp"
#include "oa/data/mutators/choice.hpp"
#include "oa/data/mutators/pipeline.hpp"
#include "oa/formats/oamut/manifest.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <new>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef OA_ENGINE_VERSION
#error "OA_ENGINE_VERSION names the engine for a mutator's requirement"
#endif

namespace oa::app {

namespace mutators = oa::data::mutators;

namespace {

/// The phase word a log line uses.
///
/// @param phase the phase
/// @return its name
const char* phase_name(mutators::Phase phase) {
    switch (phase) {
    case mutators::Phase::loadout:
        return "loadout";
    case mutators::Phase::manifest:
        return "manifest";
    case mutators::Phase::plan:
        return "plan";
    case mutators::Phase::files:
        return "files";
    case mutators::Phase::rules:
        return "rules";
    case mutators::Phase::hooks:
        return "hooks";
    case mutators::Phase::integrity:
        return "integrity";
    }
    return "loadout";
}

/// One line, with breaks turned into spaces.
///
/// @param text the text
/// @return the text on one line
std::string one_line(std::string text) {
    for (char& byte : text) {
        if (byte == '\n' || byte == '\r')
            byte = ' ';
    }
    return text;
}

/// The seed every machine shares for one mod and one mutator list.
///
/// The first eight bytes, little-endian, of the SHA-256 of the simulation
/// hash's hexadecimal digits followed by each mutator's id, a NUL, its
/// version and a NUL.
///
/// @param runtime the run
/// @param chosen the mutators, top first
/// @return the seed
uint64_t
mutator_seed(const Runtime& runtime, const std::vector<mutators::ResolvedMutator>& chosen) {
    std::string material = simulation_hash(runtime);
    for (const mutators::ResolvedMutator& one : chosen) {
        material += one.manifest.id;
        material.push_back('\0');
        material += oa::formats::oamut::version_text(one.manifest.version);
        material.push_back('\0');
    }
    std::vector<uint8_t> bytes;
    bytes.reserve(material.size());
    for (const char byte : material)
        bytes.push_back(static_cast<uint8_t>(static_cast<unsigned char>(byte)));
    const oa::base::sha256::Digest digest = oa::base::sha256::digest_of(bytes);
    uint64_t seed = 0;
    for (int index = 0; index < 8; ++index) {
        seed |= static_cast<uint64_t>(digest[static_cast<std::size_t>(index)])
                << (static_cast<unsigned>(index) * 8U);
    }
    return seed;
}

/// Why a result cannot be played.
///
/// A refusal is that text. Otherwise the first mutator that did not apply,
/// by its manifest name and its id.
///
/// @param chosen the mutators, in the order they were run
/// @param result the pipeline's result
/// @return the reason
std::string failure_text(
    const std::vector<mutators::ResolvedMutator>& chosen, const mutators::PipelineResult& result
) {
    if (!result.refusal.empty())
        return one_line(result.refusal);
    for (std::size_t index = 0; index < result.mutators.size(); ++index) {
        const mutators::MutatorResult& one = result.mutators[index];
        if (one.outcome == mutators::Outcome::applied)
            continue;
        std::string shown = one.id;
        std::string id = one.id;
        if (index < chosen.size()) {
            id = chosen[index].manifest.id;
            if (!chosen[index].manifest.name.empty())
                shown = chosen[index].manifest.name;
        }
        return shown + " (" + id + "): " + phase_name(one.phase) + ": " + one_line(one.message);
    }
    return "the mutators did not apply";
}

/// The outcome word a log line uses when a mutator left no message.
///
/// @param outcome the outcome
/// @return its name
const char* outcome_word(mutators::Outcome outcome) {
    switch (outcome) {
    case mutators::Outcome::applied:
        return "applied";
    case mutators::Outcome::failed:
        return "failed";
    case mutators::Outcome::incompatible:
        return "incompatible";
    case mutators::Outcome::excluded:
        return "excluded";
    }
    return "excluded";
}

/// Logs what one run applied, each conflict and each mutator that did not.
///
/// A refused list is one line and nothing about each mutator. The applied
/// line is written only when a mutator applied. A mutator with no message
/// is named by its outcome.
///
/// @param chosen the mutators, in the order they were run
/// @param result the pipeline's result
void log_applied(
    const std::vector<mutators::ResolvedMutator>& chosen, const mutators::PipelineResult& result
) {
    if (!result.refusal.empty()) {
        std::cerr << "open-annihilation: mutators: refused: " << one_line(result.refusal) << '\n';
        return;
    }
    std::string names;
    for (const mutators::MutatorResult& one : result.mutators) {
        if (one.outcome != mutators::Outcome::applied)
            continue;
        if (!names.empty())
            names += ", ";
        names += one.id;
        names += ' ';
        names += one.version;
    }
    if (!names.empty()) {
        std::size_t added = 0;
        std::size_t hidden = 0;
        std::set<std::string> rewritten;
        for (const mutators::JournalEntry& entry : result.journal.entries) {
            if (entry.change == mutators::Change::added) {
                ++added;
            } else if (entry.change == mutators::Change::hidden) {
                ++hidden;
            } else if (
                entry.change == mutators::Change::set ||
                entry.change == mutators::Change::removed_field ||
                entry.change == mutators::Change::removed
            ) {
                rewritten.insert(entry.path.empty() ? entry.record : entry.path);
            }
        }
        std::string line = "open-annihilation: mutators: applied ";
        line += names;
        line += " (";
        line += std::to_string(added);
        line += " files added, ";
        line += std::to_string(hidden);
        line += " hidden, ";
        line += std::to_string(rewritten.size());
        line += " rewritten)";
        std::cerr << line << '\n';
    }

    std::vector<std::string> ids;
    ids.reserve(chosen.size());
    for (const mutators::ResolvedMutator& one : chosen)
        ids.push_back(one.manifest.id);
    for (const mutators::Conflict& conflict : result.report.conflicts) {
        const std::string text = mutators::describe(conflict, ids);
        if (!text.empty())
            std::cerr << "open-annihilation: mutators: " << text << '\n';
    }
    for (const mutators::MutatorResult& one : result.mutators) {
        if (one.outcome == mutators::Outcome::applied)
            continue;
        std::string message = one_line(one.message);
        if (message.empty())
            message = outcome_word(one.outcome);
        std::cerr << "open-annihilation: mutator " << one.id << ": " << phase_name(one.phase)
                  << ": " << message << '\n';
    }
}

} // namespace

bool resolve_run_mutator(
    std::string_view text,
    std::string_view version,
    const std::filesystem::path& folder,
    mutators::ResolvedMutator* resolved,
    std::string* error
) {
    const auto fail = [&](std::string message) {
        if (error != nullptr)
            *error = std::move(message);
        return false;
    };
    std::string parse_error;
    const std::optional<mutators::MutatorChoice> choice =
        mutators::parse_choice(text, &parse_error);
    if (!choice)
        return fail(std::move(parse_error));
    std::string resolve_error;
    const std::optional<mutators::ResolvedMutator> found =
        mutators::resolve_choice(*choice, folder, &resolve_error);
    if (!found)
        return fail(std::move(resolve_error));
    if (!version.empty()) {
        const std::optional<oa::formats::oamut::Version> asked =
            oa::formats::oamut::parse_version(version);
        if (!asked || *asked != found->manifest.version) {
            return fail(
                "\"" + choice->target + "\" " + std::string(version) + " is not installed; " +
                oa::formats::oamut::version_text(found->manifest.version) + " is"
            );
        }
    }
    if (resolved != nullptr)
        *resolved = *found;
    return true;
}

struct Runtime::MutatorState {
    std::vector<mutators::ResolvedMutator> chosen{};
    std::optional<mutators::PipelineResult> result{};
};

void Runtime::destroy_mutator_state(MutatorState* state) noexcept {
    delete state;
}

void Runtime::choose_match_mutators(std::vector<mutators::ResolvedMutator> chosen) {
    if (chosen.empty()) {
        if (mutator_state_ != nullptr)
            mutator_state_->chosen.clear();
        return;
    }
    if (mutator_state_ == nullptr) {
        std::unique_ptr<MutatorState, void (*)(MutatorState*) noexcept> made{
            new MutatorState, destroy_mutator_state
        };
        made->chosen = std::move(chosen);
        mutator_state_ = std::move(made);
        return;
    }
    mutator_state_->chosen = std::move(chosen);
}

std::span<const mutators::ResolvedMutator> Runtime::chosen_match_mutators() const noexcept {
    if (mutator_state_ == nullptr)
        return {};
    return mutator_state_->chosen;
}

const mutators::PipelineResult* Runtime::match_mutators() const noexcept {
    if (mutator_state_ == nullptr || !mutator_state_->result.has_value())
        return nullptr;
    return &*mutator_state_->result;
}

void Runtime::take_command_line_mutators() {
    if (options_.mutators.empty()) {
        choose_match_mutators({});
        return;
    }
    const fs::path folder =
        user_folder_ / std::string(oa::data::mutator_library::mutators_folder_name);
    std::vector<mutators::ResolvedMutator> chosen;
    chosen.reserve(options_.mutators.size());
    for (std::size_t index = 0; index < options_.mutators.size(); ++index) {
        const std::string& value = options_.mutators[index];
        const std::string version = index < options_.mutator_versions.size()
                                        ? options_.mutator_versions[index]
                                        : std::string{};
        mutators::ResolvedMutator resolved;
        std::string error;
        if (!resolve_run_mutator(value, version, folder, &resolved, &error)) {
            if (index < options_.mutator_fields.size() && !options_.mutator_fields[index].empty())
                throw std::runtime_error(options_.mutator_fields[index] + ": " + error);
            throw std::runtime_error("--mutator: " + value + ": " + error);
        }
        chosen.push_back(std::move(resolved));
    }
    choose_match_mutators(std::move(chosen));
}

std::string Runtime::launch_mutator_refusal(
    std::string_view source,
    const std::vector<std::string>& texts,
    const std::vector<std::string>& versions,
    const std::vector<std::string>& fields
) const {
    const fs::path folder =
        user_folder_ / std::string(oa::data::mutator_library::mutators_folder_name);
    for (std::size_t index = 0; index < texts.size(); ++index) {
        const std::string version = index < versions.size() ? versions[index] : std::string{};
        const std::string field =
            index < fields.size() ? fields[index] : std::string("content.mutators");
        std::string error;
        if (!resolve_run_mutator(texts[index], version, folder, nullptr, &error))
            return std::string(source) + ": " + field + ": " + error;
    }
    return {};
}

std::string Runtime::take_launch_mutators(
    std::string_view source,
    std::vector<std::string> texts,
    std::vector<std::string> versions,
    std::vector<std::string> fields
) {
    const std::string refusal = launch_mutator_refusal(source, texts, versions, fields);
    if (!refusal.empty())
        return refusal;
    const fs::path folder =
        user_folder_ / std::string(oa::data::mutator_library::mutators_folder_name);
    std::vector<mutators::ResolvedMutator> chosen;
    chosen.reserve(texts.size());
    for (std::size_t index = 0; index < texts.size(); ++index) {
        const std::string version = index < versions.size() ? versions[index] : std::string{};
        mutators::ResolvedMutator resolved;
        if (!resolve_run_mutator(texts[index], version, folder, &resolved, nullptr))
            return std::string(source) + ": content.mutators: a mutator could not be read";
        chosen.push_back(std::move(resolved));
    }
    options_.mutators = std::move(texts);
    options_.mutator_versions = std::move(versions);
    options_.mutator_fields = std::move(fields);
    choose_match_mutators(std::move(chosen));
    return {};
}

void Runtime::apply_match_mutators(const MatchBootstrap&) {
    if (mutator_state_ == nullptr || mutator_state_->chosen.empty())
        return;
    const std::vector<mutators::ResolvedMutator> listed = mutator_state_->chosen;
    // A layer already mounted makes the pipeline refuse the list and leave
    // that layer in place.
    if (assets_.mutator_layer_mounted())
        release_match_mutators();

    mutators::PipelineInput input;
    input.mutators.reserve(listed.size());
    for (const mutators::ResolvedMutator& one : listed) {
        mutators::PipelineMutator item;
        item.package = one.folder;
        item.manifest = one.manifest;
        item.options = one.options;
        input.mutators.push_back(std::move(item));
    }
    if (const oa::data::mod_profile::ModProfile* profile = mod_profile()) {
        input.mod.id = profile->id;
        input.mod.version = profile->version;
    } else {
        input.mod.id = std::string(oa::data::mod_profile::base_game_id);
    }
    const PresenceAnnouncement announcement = presence_announcement();
    input.mod.game = announcement.base_game.empty()
                         ? std::string(oa::data::content_bundles::default_base_game_id)
                         : announcement.base_game;
    input.engine_version = OA_ENGINE_VERSION;
    input.layout = &oa::data::defs::data_layout();
    input.variant = side_files_language_;
    input.seed = mutator_seed(*this, listed);

    mutators::PipelineResult result = mutators::run_pipeline(assets_, input);
    if (mutator_state_ == nullptr) {
        release_match_mutators();
        throw std::runtime_error("the mutators did not apply");
    }
    mutator_state_->result = std::move(result);
    log_applied(listed, *mutator_state_->result);
    if (mutator_state_->result->playable())
        return;
    const std::string reason = failure_text(listed, *mutator_state_->result);
    release_match_mutators();
    throw std::runtime_error(reason);
}

void Runtime::release_match_mutators() noexcept {
    try {
        const bool held = mutator_state_ != nullptr && mutator_state_->result.has_value();
        bool unmounted = false;
        if (assets_.mutator_layer_mounted()) {
            if (!assets_.unmount_mutator_layer())
                return;
            unmounted = true;
        }
        if (!unmounted && !held)
            return;
        if (mutator_state_ != nullptr)
            mutator_state_->result.reset();
        std::cerr << "open-annihilation: mutators: released\n";
    } catch (...) {
    }
}

} // namespace oa::app
