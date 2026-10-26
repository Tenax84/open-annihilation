// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Turns one pipeline result into the last mutator's verdict, and runs that
// pipeline on a store the job owns. Nothing here reads the clock, a runtime
// or a live asset store.
#include "oa/data/mutators/dry_run.hpp"

#include "incompatible.hpp"
#include "oa/formats/hpi.hpp"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::data::mutators {
namespace {

/// ASCII case fold, byte by byte. Enough for record names and paths.
std::string fold_key(std::string_view text) {
    std::string folded(text.size(), '\0');
    for (std::size_t index = 0; index < text.size(); ++index) {
        const unsigned char byte = static_cast<unsigned char>(text[index]);
        folded[index] = (byte >= 'A' && byte <= 'Z') ? static_cast<char>(byte - 'A' + 'a')
                                                     : static_cast<char>(byte);
    }
    return folded;
}

uint32_t count_of(std::size_t size) {
    constexpr std::size_t cap = static_cast<std::size_t>(UINT32_MAX);
    if (size > cap)
        return UINT32_MAX;
    return static_cast<uint32_t>(size);
}

/// Distinct records a collection added, removed or changed.
struct RecordSets {
    std::set<std::string> changed;
    std::set<std::string> added;
    std::set<std::string> removed;
};

/// Drops a changed mark once the same record was added or removed.
void drop_structural(RecordSets& sets) {
    for (const std::string& key : sets.added)
        sets.changed.erase(key);
    for (const std::string& key : sets.removed)
        sets.changed.erase(key);
}

bool values_differ(const JournalEntry& entry) {
    return entry.old_text != entry.new_text;
}

bool is_shared_add(const JournalEntry& entry) {
    return entry.change == Change::added && entry.new_text.has_value() &&
           *entry.new_text == "shared";
}

std::string record_key(const JournalEntry& entry) {
    if (!entry.record.empty())
        return fold_key(entry.record);
    return fold_key(entry.path);
}

std::string path_key(const JournalEntry& entry) {
    if (!entry.path.empty())
        return fold_key(entry.path);
    return fold_key(entry.record);
}

/// One menu entry: its file, then its section or button. Every download
/// file names its sections MENUENTRY1 and up, and one unit is a button on
/// several pages, so the name alone is not one entry.
std::string menu_key(const JournalEntry& entry) {
    std::string key = fold_key(entry.path);
    key.push_back('\n');
    key += fold_key(entry.record);
    return key;
}

/// Notes one journal line. A shared add is not an add and not a change.
void take_record(RecordSets& sets, const JournalEntry& entry, bool& effect) {
    if (is_shared_add(entry))
        return;
    const std::string key = record_key(entry);
    if (entry.change == Change::added) {
        sets.added.insert(key);
        effect = true;
        return;
    }
    if (entry.change == Change::removed) {
        sets.removed.insert(key);
        effect = true;
        return;
    }
    if ((entry.change == Change::set || entry.change == Change::removed_field) &&
        values_differ(entry)) {
        sets.changed.insert(key);
        effect = true;
    }
}

VerdictRecord labelled(Verdict verdict, std::string reason, std::string mod_label) {
    VerdictRecord record;
    record.verdict = verdict;
    record.reason = std::move(reason);
    record.mod_label = one_line_reason(mod_label);
    return record;
}

VerdictRecord failed_reason(std::string_view reason, const std::string& mod_label) {
    return labelled(Verdict::failed, one_line_reason(reason), mod_label);
}

/// A probe store. Its destructor drops a mutator layer the pipeline left mounted.
struct ProbeStore {
    oa::AssetStore store;

    explicit ProbeStore(std::vector<std::filesystem::path> roots) : store(std::move(roots)) {}

    ProbeStore(const ProbeStore&) = delete;
    ProbeStore& operator=(const ProbeStore&) = delete;

    ~ProbeStore() {
        if (store.mutator_layer_mounted())
            store.unmount_mutator_layer();
    }
};

} // namespace

namespace {

/// True when the list's last mutator, whose verdict this is, declares the
/// maps layer. A prerequisite's maps layer is that prerequisite's change.
///
/// @param mutators the list that ran
/// @return true when the last one does
bool declares_maps(std::span<const PipelineMutator> mutators) {
    if (mutators.empty())
        return false;
    for (const oa::formats::oamut::Layer layer : mutators.back().manifest.layers) {
        if (layer == oa::formats::oamut::Layer::maps)
            return true;
    }
    return false;
}

} // namespace

VerdictRecord verdict_of(
    const PipelineResult& result, std::string mod_label, std::span<const PipelineMutator> mutators
) {
    if (result.mutators.empty()) {
        const std::string_view reason =
            result.refusal.empty() ? std::string_view{"the list names no mutator"} : result.refusal;
        return failed_reason(reason, mod_label);
    }
    const MutatorResult& last = result.mutators.back();
    if (!result.refusal.empty()) {
        const std::string& text = !last.message.empty() ? last.message : result.refusal;
        const std::string_view reason =
            text.empty() ? std::string_view{"the check failed"} : std::string_view{text};
        return failed_reason(reason, mod_label);
    }
    if (last.outcome == Outcome::excluded)
        return failed_reason(last.message, mod_label);

    const bool hook_script = last.outcome == Outcome::failed && last.phase == Phase::hooks &&
                             last.cause == Cause::script_error;
    const bool integrity = last.outcome == Outcome::failed && last.phase == Phase::integrity;
    if (last.outcome == Outcome::incompatible || hook_script || integrity) {
        std::string reason = one_line_reason(last.message);
        if (reason.empty())
            reason = std::string(incompatible_default_reason);
        return labelled(Verdict::incompatible, std::move(reason), mod_label);
    }
    if (last.outcome != Outcome::applied)
        return failed_reason(last.message, mod_label);

    const auto last_index = static_cast<uint16_t>(result.mutators.size() - 1);
    RecordSets units;
    RecordSets weapons;
    RecordSets features;
    std::set<std::string> menus;
    std::set<std::string> files_added;
    std::set<std::string> files_hidden;
    bool effect = false;
    for (const JournalEntry& entry : result.journal.entries_of(last_index)) {
        if (entry.collection == Collection::files) {
            if (entry.change == Change::added) {
                files_added.insert(path_key(entry));
                effect = true;
            } else if (entry.change == Change::hidden || entry.change == Change::removed) {
                files_hidden.insert(path_key(entry));
                effect = true;
            }
            continue;
        }
        if (entry.collection == Collection::menus) {
            const bool structural =
                entry.change == Change::added || entry.change == Change::removed;
            const bool field =
                (entry.change == Change::set || entry.change == Change::removed_field) &&
                values_differ(entry);
            if (structural || field) {
                menus.insert(menu_key(entry));
                effect = true;
            }
            continue;
        }
        if (entry.collection == Collection::units)
            take_record(units, entry, effect);
        else if (entry.collection == Collection::weapons)
            take_record(weapons, entry, effect);
        else if (entry.collection == Collection::features)
            take_record(features, entry, effect);
        else if (
            entry.change == Change::added || entry.change == Change::removed ||
            entry.change == Change::hidden ||
            ((entry.change == Change::set || entry.change == Change::removed_field) &&
             values_differ(entry))
        ) {
            // Sides, tables and anything else are not a ChangeCounts field.
            if (!is_shared_add(entry))
                effect = true;
        }
    }
    drop_structural(units);
    drop_structural(weapons);
    drop_structural(features);
    if (!effect) {
        if (declares_maps(mutators))
            return labelled(Verdict::compatible, {}, mod_label);
        return labelled(Verdict::no_effect, {}, mod_label);
    }

    VerdictRecord record = labelled(Verdict::compatible, {}, mod_label);
    record.counts.units_changed = count_of(units.changed.size());
    record.counts.units_added = count_of(units.added.size());
    record.counts.units_removed = count_of(units.removed.size());
    record.counts.weapons_changed = count_of(weapons.changed.size());
    record.counts.weapons_added = count_of(weapons.added.size());
    record.counts.weapons_removed = count_of(weapons.removed.size());
    record.counts.features_changed = count_of(features.changed.size());
    record.counts.features_added = count_of(features.added.size());
    record.counts.features_removed = count_of(features.removed.size());
    record.counts.menu_entries_changed = count_of(menus.size());
    record.counts.files_added = count_of(files_added.size());
    record.counts.files_hidden = count_of(files_hidden.size());
    return record;
}

VerdictRecord run_dry_run(const DryRunJob& job, const std::atomic<bool>& cancel) {
    if (cancel.load())
        return labelled(Verdict::failed, std::string(dry_run_cancelled_reason), job.mod_label);
    if (job.loose_roots.empty())
        return failed_reason("an asset store needs a folder", job.mod_label);
    try {
        ProbeStore probe(job.loose_roots);
        for (const std::filesystem::path& archive : job.mounts) {
            std::string error;
            if (!probe.store.try_mount(archive, &error)) {
                const std::string_view reason =
                    error.empty() ? std::string_view{"a mount failed"} : std::string_view{error};
                return failed_reason(reason, job.mod_label);
            }
        }
        if (cancel.load())
            return labelled(Verdict::failed, std::string(dry_run_cancelled_reason), job.mod_label);
        const PipelineResult result = run_pipeline(probe.store, job.input);
        if (probe.store.mutator_layer_mounted())
            probe.store.unmount_mutator_layer();
        return verdict_of(result, job.mod_label, job.input.mutators);
    } catch (const std::exception& ex) {
        const char* text = ex.what();
        return failed_reason(
            text != nullptr && text[0] != '\0' ? std::string_view{text}
                                               : std::string_view{"the check failed"},
            job.mod_label
        );
    } catch (...) {
        return failed_reason("the check failed", job.mod_label);
    }
}

} // namespace oa::data::mutators
