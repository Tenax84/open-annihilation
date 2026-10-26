// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A mutator check without a match. The main thread prepares a job from the
// running game; any thread can run that job. The verdict file lives in the
// player's Cache folder and is read and written only here.
#include "oa/app/runtime.hpp"

#include "oa/app/game_directory.hpp"
#include "oa/app/user_folder.hpp"
#include "oa/base/sha256.hpp"
#include "oa/data/content_bundles/content_choice.hpp"
#include "oa/data/mod_profile.hpp"
#include "oa/data/mutators/dry_run.hpp"
#include "oa/data/mutators/loadout.hpp"
#include "oa/data/mutators/verdicts.hpp"
#include "oa/platform/files.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifndef OA_ENGINE_VERSION
#error "OA_ENGINE_VERSION names the engine a mutator verdict is stored under"
#endif

namespace oa::app {
namespace {

namespace mutators = oa::data::mutators;

/// Throws while a match is loading or running. The four check methods are
/// main-thread only; this is the part a caller can trip from that thread.
void refuse_during_match(bool match_held, bool loading, bool playing) {
    if (match_held || loading || playing) {
        throw std::logic_error("a mutator check cannot run while a match loads or runs");
    }
}

/// Appends one line for each folder and archive the dry run reads.
///
/// The simulation hash and the profile do not name those files: a mod
/// folder without `oamod.yaml` has no profile and plays 3.1c's rules, and a
/// game folder that is not the managed copy announces no expansions. Each
/// loose folder is `loose`, a tab and its path. Each archive, in mount
/// order, is `archive`, a tab, its path, a tab, its size in bytes, a tab
/// and its last write time in nanoseconds of the file clock, with `-` for
/// a size or a time that cannot be read. So an archive added, removed or
/// replaced asks again. An edit to a loose file does not.
void append_store_sources(
    std::string& message,
    std::span<const std::filesystem::path> loose_roots,
    std::span<const std::filesystem::path> mounts
) {
    for (const std::filesystem::path& root : loose_roots) {
        message += "loose\t";
        message += path_to_utf8(root);
        message.push_back('\n');
    }
    for (const std::filesystem::path& archive : mounts) {
        message += "archive\t";
        message += path_to_utf8(archive);
        message.push_back('\t');
        std::error_code size_error;
        const uintmax_t size = std::filesystem::file_size(archive, size_error);
        message += size_error ? std::string("-") : std::to_string(size);
        message.push_back('\t');
        std::error_code time_error;
        const std::filesystem::file_time_type written =
            std::filesystem::last_write_time(archive, time_error);
        const int64_t ticks = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(written.time_since_epoch()).count()
        );
        message += time_error ? std::string("-") : std::to_string(ticks);
        message.push_back('\n');
    }
}

/// The mod key's message. It is not the simulation hash alone.
///
/// `oa-mutator-mod-1` and a line feed, the 64 hex digits `simulation_hash`
/// returns, a line feed, then the mod's id, version and packaging revision
/// each on a line (the word `none` for each when no mod profile is loaded),
/// the base game id (`presence_announcement().base_game`, or `totala`),
/// each rule-changing expansion id in announcement order, one per line,
/// and last the lines `append_store_sources` writes for the probe store's
/// folders and archives.
std::string mod_key_message(
    std::string_view sim_hex,
    const oa::data::mod_profile::ModProfile* profile,
    const Runtime::PresenceAnnouncement& announcement,
    std::span<const std::filesystem::path> loose_roots,
    std::span<const std::filesystem::path> mounts
) {
    std::string message = "oa-mutator-mod-1\n";
    message.append(sim_hex);
    message.push_back('\n');
    if (profile == nullptr) {
        message += "none\nnone\nnone\n";
    } else {
        message += profile->id;
        message.push_back('\n');
        message += profile->version;
        message.push_back('\n');
        message += std::to_string(profile->packaging.revision);
        message.push_back('\n');
    }
    if (announcement.base_game.empty())
        message += std::string(oa::data::content_bundles::default_base_game_id);
    else
        message += announcement.base_game;
    message.push_back('\n');
    for (const std::string& expansion : announcement.expansions) {
        message += expansion;
        message.push_back('\n');
    }
    append_store_sources(message, loose_roots, mounts);
    return message;
}

oa::base::sha256::Digest digest_of_text(std::string_view text) {
    std::vector<uint8_t> bytes(text.size());
    for (std::size_t index = 0; index < text.size(); ++index)
        bytes[index] = static_cast<uint8_t>(static_cast<unsigned char>(text[index]));
    return oa::base::sha256::digest_of(bytes);
}

oa::base::sha256::Digest mod_digest_of(
    std::string_view sim_hex,
    const oa::data::mod_profile::ModProfile* profile,
    const Runtime::PresenceAnnouncement& announcement,
    std::span<const std::filesystem::path> loose_roots,
    std::span<const std::filesystem::path> mounts
) {
    return digest_of_text(mod_key_message(sim_hex, profile, announcement, loose_roots, mounts));
}

mutators::VerdictKey verdict_key_for(
    std::span<const mutators::ResolvedMutator> list,
    const mutators::PipelineInput& input,
    std::string_view sim_hex,
    const oa::data::mod_profile::ModProfile* profile,
    const Runtime::PresenceAnnouncement& announcement,
    std::span<const std::filesystem::path> loose_roots,
    std::span<const std::filesystem::path> mounts
) {
    mutators::VerdictKey key;
    key.loadout = mutators::loadout_digest(mutators::loadout_of(list, input.clash_default));
    key.mod = mod_digest_of(sim_hex, profile, announcement, loose_roots, mounts);
    key.engine_version = OA_ENGINE_VERSION;
    return key;
}

std::string mod_label_of(const oa::data::mod_profile::ModProfile* profile) {
    if (profile == nullptr)
        return {};
    std::string label = profile->name;
    if (!profile->version.empty()) {
        if (!label.empty())
            label.push_back(' ');
        label += profile->version;
    }
    return label;
}

std::filesystem::path verdicts_path(const std::filesystem::path& user_folder) {
    return user_folder / std::string(cache_folder_name) / std::string(mutators::verdicts_file_name);
}

std::vector<mutators::RememberedVerdict> read_verdict_file(const std::filesystem::path& path) {
    std::FILE* file = oa::platform::open_file(path, "rb");
    if (file == nullptr)
        return {};
    std::vector<uint8_t> bytes(mutators::verdicts_file_most_bytes + 1);
    const std::size_t got = std::fread(bytes.data(), 1, bytes.size(), file);
    const bool failed = std::ferror(file) != 0;
    std::fclose(file);
    if (failed)
        return {};
    return mutators::read_verdicts(std::span<const uint8_t>(bytes.data(), got));
}

bool cancelled_verdict(const mutators::VerdictRecord& record) {
    return record.verdict == mutators::Verdict::failed &&
           record.reason == mutators::dry_run_cancelled_reason;
}

} // namespace

std::optional<mutators::VerdictRecord>
Runtime::remembered_verdict(std::span<const mutators::ResolvedMutator> list) const {
    refuse_during_match(match_ != nullptr, screen_ == Screen::loading, screen_ == Screen::match);
    const std::vector<mutators::ResolvedMutator> chosen(list.begin(), list.end());
    const mutators::PipelineInput input = mutator_pipeline_input(chosen);
    const mutators::VerdictKey key = verdict_key_for(
        chosen,
        input,
        simulation_hash(*this),
        mod_profile(),
        presence_announcement(),
        assets_.loose_roots(),
        assets_.mount_paths()
    );
    const std::vector<mutators::RememberedVerdict> stored =
        read_verdict_file(verdicts_path(user_folder_));
    const mutators::VerdictRecord* found = mutators::find_verdict(stored, key);
    if (found == nullptr)
        return std::nullopt;
    return *found;
}

oa::base::sha256::Digest Runtime::mutator_mod_digest() const {
    return mod_digest_of(
        simulation_hash(*this),
        mod_profile(),
        presence_announcement(),
        assets_.loose_roots(),
        assets_.mount_paths()
    );
}

mutators::VerdictKey Runtime::verdict_key_for_list(
    std::span<const mutators::ResolvedMutator> list, const oa::base::sha256::Digest& mod_digest
) const {
    mutators::VerdictKey key;
    key.loadout = mutators::loadout_digest(mutators::loadout_of(list, mutator_clash_default()));
    key.mod = mod_digest;
    key.engine_version = OA_ENGINE_VERSION;
    return key;
}

std::vector<mutators::RememberedVerdict> Runtime::read_mutator_verdicts() const {
    return read_verdict_file(verdicts_path(user_folder_));
}

mutators::DryRunJob
Runtime::prepare_mutator_check(std::span<const mutators::ResolvedMutator> list) const {
    ++mutator_prepares_;
    refuse_during_match(match_ != nullptr, screen_ == Screen::loading, screen_ == Screen::match);
    const std::vector<mutators::ResolvedMutator> chosen(list.begin(), list.end());
    mutators::DryRunJob job;
    job.loose_roots.assign(assets_.loose_roots().begin(), assets_.loose_roots().end());
    job.mounts.assign(assets_.mount_paths().begin(), assets_.mount_paths().end());
    job.input = mutator_pipeline_input(chosen);
    job.input.map.reset();
    bind_planned_hack_check(job.input);
    job.mod_label = mod_label_of(mod_profile());
    job.key = verdict_key_for(
        chosen,
        job.input,
        simulation_hash(*this),
        mod_profile(),
        presence_announcement(),
        job.loose_roots,
        job.mounts
    );
    return job;
}

void Runtime::remember_mutator_verdict(
    const mutators::VerdictKey& key, const mutators::VerdictRecord& record
) {
    refuse_during_match(match_ != nullptr, screen_ == Screen::loading, screen_ == Screen::match);
    if (cancelled_verdict(record))
        return;
    const std::filesystem::path path = verdicts_path(user_folder_);
    std::vector<mutators::RememberedVerdict> stored = read_verdict_file(path);
    mutators::RememberedVerdict verdict;
    verdict.key = key;
    verdict.record = record;
    mutators::remember_verdict(stored, std::move(verdict));
    const std::vector<uint8_t> bytes = mutators::write_verdicts(stored);
    std::string error;
    if (!oa::platform::replace_file(path, bytes, &error)) {
        std::cerr << "open-annihilation: mutator check: " << error << '\n';
    }
}

mutators::VerdictRecord Runtime::check_mutators(std::span<const mutators::ResolvedMutator> list) {
    refuse_during_match(match_ != nullptr, screen_ == Screen::loading, screen_ == Screen::match);
    if (const std::optional<mutators::VerdictRecord> stored = remembered_verdict(list))
        return *stored;
    const mutators::DryRunJob job = prepare_mutator_check(list);
    const std::atomic<bool> cancel{false};
    const mutators::VerdictRecord record = mutators::run_dry_run(job, cancel);
    ++mutator_dry_runs_;
    remember_mutator_verdict(job.key, record);
    if (!cancelled_verdict(record)) {
        const std::string id = list.empty() ? std::string("none") : list.back().manifest.id;
        const std::string label =
            record.mod_label.empty() ? std::string("No Mod") : record.mod_label;
        std::cerr << "open-annihilation: mutator check: " << id << " with " << label << ": "
                  << mutators::verdict_word(record.verdict) << '\n';
    }
    return record;
}

} // namespace oa::app
