// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The mutation pipeline over the made-up Ridge packages. Identical inputs
// are compared whole, and so is a run with a failing mutator against the
// run of the mutators that remain. Two rule-only mutators are compared by
// the bytes the layer serves: the owner of a written file and the layer's
// label follow the list, so those two fields are not the served bytes.
#include "oa/data/mutators/pipeline.hpp"
#include "oa/formats/oamut/manifest.hpp"
#include "oa/test/check.hpp"
#include "oa/test/scratch_directory.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef OA_MUTATORS_TEST_DATA
#error OA_MUTATORS_TEST_DATA is the fixture folder
#endif

namespace {

namespace mutators = oa::data::mutators;
namespace oamut = oa::formats::oamut;
namespace defs = oa::data::defs;
namespace fs = std::filesystem;

const fs::path k_data{OA_MUTATORS_TEST_DATA};

std::vector<uint8_t> read_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    OA_CHECK(in.good());
    if (!in.good())
        return {};
    in.seekg(0, std::ios::end);
    const std::streamoff length = in.tellg();
    in.seekg(0);
    if (length <= 0)
        return {};
    std::vector<uint8_t> bytes(static_cast<std::size_t>(length));
    in.read(reinterpret_cast<char*>(bytes.data()), length);
    return bytes;
}

void write_bytes(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    OA_CHECK(out.good());
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

mutators::PipelineMutator load_mutator(const fs::path& package) {
    mutators::PipelineMutator mutator;
    mutator.package = package;
    const std::vector<uint8_t> bytes = read_bytes(package / "oamut.yaml");
    std::vector<oamut::Problem> problems;
    const bool read = oamut::read_manifest(bytes, mutator.manifest, problems);
    if (!read) {
        for (const oamut::Problem& problem : problems) {
            std::fprintf(
                stderr,
                "%s: %s: %s\n",
                package.string().c_str(),
                problem.key.c_str(),
                problem.message.c_str()
            );
        }
    }
    OA_CHECK(read);
    return mutator;
}

mutators::PipelineMutator load_id(std::string_view id) {
    return load_mutator(k_data / "packages" / std::string{id});
}

mutators::PipelineInput make_input(std::vector<mutators::PipelineMutator> list) {
    mutators::PipelineInput input;
    input.mutators = std::move(list);
    input.mod.id = "ridge";
    input.mod.version = "1.0";
    input.mod.game = "ridge";
    input.engine_version = "0.8.5";
    input.layout = &defs::data_layout();
    input.seed = 1;
    return input;
}

mutators::PipelineInput input_of(std::vector<std::string_view> ids) {
    std::vector<mutators::PipelineMutator> list;
    for (const std::string_view id : ids)
        list.push_back(load_id(id));
    return make_input(std::move(list));
}

oa::AssetStore open_ridge() {
    return oa::AssetStore(k_data / "ridge");
}

void release(oa::AssetStore& store) {
    if (store.mutator_layer_mounted())
        store.unmount_mutator_layer();
    OA_CHECK(!store.mutator_layer_mounted());
}

std::string text_of(const std::vector<uint8_t>& bytes) {
    return std::string(bytes.begin(), bytes.end());
}

std::string served_text(const oa::MutatorLayerEntry& entry) {
    if (entry.contents == nullptr)
        return {};
    return text_of(*entry.contents);
}

bool same_contents(
    const std::shared_ptr<const std::vector<uint8_t>>& left,
    const std::shared_ptr<const std::vector<uint8_t>>& right
) {
    if (left == nullptr || right == nullptr)
        return left == nullptr && right == nullptr;
    return *left == *right;
}

/// Served paths, kinds, payloads and mask patterns. Owners and the label
/// follow list order, so a reversed rule list is not the same spec.
bool payloads_match(const oa::MutatorLayerSpec& left, const oa::MutatorLayerSpec& right) {
    if (left.entries.size() != right.entries.size() || left.masks.size() != right.masks.size() ||
        left.archives.size() != right.archives.size())
        return false;
    for (std::size_t index = 0; index < left.masks.size(); ++index) {
        if (left.masks[index].pattern != right.masks[index].pattern)
            return false;
    }
    for (std::size_t index = 0; index < left.entries.size(); ++index) {
        const oa::MutatorLayerEntry& one = left.entries[index];
        const oa::MutatorLayerEntry& other = right.entries[index];
        if (one.path != other.path || one.kind != other.kind ||
            !same_contents(one.contents, other.contents))
            return false;
    }
    return true;
}

bool specs_match(const oa::MutatorLayerSpec& left, const oa::MutatorLayerSpec& right) {
    if (left.label != right.label || !payloads_match(left, right))
        return false;
    for (std::size_t index = 0; index < left.masks.size(); ++index) {
        if (left.masks[index].owner != right.masks[index].owner)
            return false;
    }
    for (std::size_t index = 0; index < left.entries.size(); ++index) {
        const oa::MutatorLayerEntry& one = left.entries[index];
        const oa::MutatorLayerEntry& other = right.entries[index];
        if (one.owner != other.owner || one.host != other.host || one.archive != other.archive ||
            one.archive_path != other.archive_path)
            return false;
    }
    for (std::size_t index = 0; index < left.archives.size(); ++index) {
        if (left.archives[index].owner != right.archives[index].owner ||
            left.archives[index].location != right.archives[index].location)
            return false;
    }
    return true;
}

/// The ids that `slots` name in `run`'s own list. Journal entries and
/// conflicts name mutators by their index in the list a run was given, so
/// two runs over different lists are compared by the ids those indices name.
std::vector<std::string>
ids_of(const std::vector<uint16_t>& slots, const mutators::PipelineResult& run) {
    std::vector<std::string> ids;
    for (const uint16_t slot : slots) {
        OA_CHECK(slot < run.mutators.size());
        if (slot < run.mutators.size())
            ids.push_back(run.mutators[slot].id);
    }
    return ids;
}

bool same_entry(
    const mutators::JournalEntry& left,
    const mutators::PipelineResult& left_run,
    const mutators::JournalEntry& right,
    const mutators::PipelineResult& right_run
) {
    return ids_of(left.mutators, left_run) == ids_of(right.mutators, right_run) &&
           left.phase == right.phase && left.collection == right.collection &&
           left.change == right.change && left.record == right.record &&
           left.block == right.block && left.field == right.field &&
           left.old_text == right.old_text && left.new_text == right.new_text &&
           left.path == right.path;
}

/// Compares two runs' journals entry by entry, naming mutators by id.
bool journals_match(const mutators::PipelineResult& left, const mutators::PipelineResult& right) {
    if (left.journal.entries.size() != right.journal.entries.size())
        return false;
    for (std::size_t index = 0; index < left.journal.entries.size(); ++index) {
        if (!same_entry(left.journal.entries[index], left, right.journal.entries[index], right))
            return false;
    }
    return true;
}

bool conflicts_match(
    const mutators::Conflict& left,
    const mutators::PipelineResult& left_run,
    const mutators::Conflict& right,
    const mutators::PipelineResult& right_run
) {
    return left.severity == right.severity && left.kind == right.kind &&
           ids_of(left.mutators, left_run) == ids_of(right.mutators, right_run) &&
           left.record == right.record && left.field == right.field && left.path == right.path &&
           left.detail == right.detail;
}

/// Compares two runs' conflict reports, naming mutators by id.
bool reports_match(const mutators::PipelineResult& left, const mutators::PipelineResult& right) {
    if (left.report.unread != right.report.unread ||
        left.report.conflicts.size() != right.report.conflicts.size())
        return false;
    for (std::size_t index = 0; index < left.report.conflicts.size(); ++index) {
        if (!conflicts_match(
                left.report.conflicts[index], left, right.report.conflicts[index], right
            ))
            return false;
    }
    return true;
}

bool lists_name(
    const oa::AssetStore& store,
    std::string_view directory,
    std::string_view extension,
    std::string_view needle
) {
    const std::vector<std::string> names =
        store.list_effective_in_mount_order(directory, extension);
    for (const std::string& name : names) {
        if (name.find(needle) != std::string::npos)
            return true;
    }
    return false;
}

/// Checks that a run with failing mutators is the run of the ones that
/// remain: the same layer, journal and report, with mutators named by id.
void check_same_run(
    const mutators::PipelineResult& with_failures, const mutators::PipelineResult& survivors
) {
    OA_CHECK(with_failures.refusal.empty());
    OA_CHECK(survivors.playable());
    OA_CHECK(specs_match(with_failures.layer, survivors.layer));
    OA_CHECK(journals_match(with_failures, survivors));
    OA_CHECK(reports_match(with_failures, survivors));
}

/// Checks that no journal entry or conflict names `slot`.
void check_no_trace(const mutators::PipelineResult& result, uint16_t slot) {
    for (const mutators::JournalEntry& entry : result.journal.entries) {
        for (const uint16_t named : entry.mutators)
            OA_CHECK(named != slot);
    }
    for (const mutators::Conflict& conflict : result.report.conflicts) {
        for (const uint16_t named : conflict.mutators)
            OA_CHECK(named != slot);
    }
}

/// Counts the paths in `paths` that end with `tail`, ignoring ASCII case.
std::size_t count_paths(const std::vector<std::string>& paths, std::string_view tail) {
    std::size_t found = 0;
    for (const std::string& path : paths) {
        if (path.size() < tail.size())
            continue;
        bool same = true;
        for (std::size_t offset = 0; offset < tail.size(); ++offset) {
            const char left = path[path.size() - tail.size() + offset];
            const char lower =
                left >= 'A' && left <= 'Z' ? static_cast<char>(left - 'A' + 'a') : left;
            if (lower != tail[offset])
                same = false;
        }
        if (same)
            ++found;
    }
    return found;
}

const mutators::Conflict*
conflict_of(const mutators::PipelineResult& result, mutators::ConflictKind kind) {
    for (const mutators::Conflict& conflict : result.report.conflicts) {
        if (conflict.kind == kind)
            return &conflict;
    }
    return nullptr;
}

std::size_t count_kind(const mutators::PipelineResult& result, mutators::ConflictKind kind) {
    std::size_t count = 0;
    for (const mutators::Conflict& conflict : result.report.conflicts) {
        if (conflict.kind == kind)
            ++count;
    }
    return count;
}

void test_swift_writes_one_file() {
    oa::AssetStore store = open_ridge();
    const mutators::PipelineResult result =
        mutators::run_pipeline(store, input_of({"ridge-swift"}));
    OA_CHECK(result.refusal.empty());
    OA_CHECK(result.playable());
    OA_CHECK(
        result.mutators.size() == 1 && result.mutators[0].outcome == mutators::Outcome::applied
    );
    OA_CHECK(result.layer.entries.size() == 1);
    if (result.layer.entries.size() == 1) {
        OA_CHECK(result.layer.entries[0].path.find("rgbot.fbi") != std::string::npos);
        OA_CHECK(result.layer.entries[0].kind == oa::MutatorSourceKind::contents);
        OA_CHECK(served_text(result.layer.entries[0]).find("2.4") != std::string::npos);
    }
    const oa::AssetData bot = store.read("units/RGBOT.fbi");
    OA_CHECK(text_of(bot.bytes).find("2.4") != std::string::npos);
    const oa::AssetData lab = store.read("units/RGLAB.fbi");
    OA_CHECK(text_of(lab.bytes).find("WorkerTime=100") != std::string::npos);
    release(store);
}

void test_rules_are_order_independent() {
    oa::AssetStore forward_store = open_ridge();
    oa::AssetStore backward_store = open_ridge();
    const mutators::PipelineResult forward =
        mutators::run_pipeline(forward_store, input_of({"ridge-swift", "ridge-swift-sm"}));
    const mutators::PipelineResult backward =
        mutators::run_pipeline(backward_store, input_of({"ridge-swift-sm", "ridge-swift"}));
    OA_CHECK(forward.playable() && backward.playable());
    OA_CHECK(payloads_match(forward.layer, backward.layer));
    OA_CHECK(forward.layer.label == "mutators ridge-swift ridge-swift-sm");
    OA_CHECK(backward.layer.label == "mutators ridge-swift-sm ridge-swift");
    if (!forward.layer.entries.empty() && !backward.layer.entries.empty()) {
        OA_CHECK(forward.layer.entries[0].owner == "ridge-swift-sm");
        OA_CHECK(backward.layer.entries[0].owner == "ridge-swift");
        OA_CHECK(served_text(forward.layer.entries[0]).find("3.6") != std::string::npos);
        OA_CHECK(served_text(backward.layer.entries[0]).find("3.6") != std::string::npos);
    } else {
        OA_CHECK(false);
    }
    const oa::AssetData forward_bot = forward_store.read("units/RGBOT.fbi");
    const oa::AssetData backward_bot = backward_store.read("units/RGBOT.fbi");
    OA_CHECK(forward_bot.bytes == backward_bot.bytes);
    OA_CHECK(text_of(forward_bot.bytes).find("3.6") != std::string::npos);
    release(forward_store);
    release(backward_store);
}

void test_hooks_follow_the_list() {
    oa::AssetStore rising = open_ridge();
    oa::AssetStore falling = open_ridge();
    const mutators::PipelineResult tougher_last =
        mutators::run_pipeline(rising, input_of({"ridge-tough", "ridge-tougher"}));
    const mutators::PipelineResult tough_last =
        mutators::run_pipeline(falling, input_of({"ridge-tougher", "ridge-tough"}));
    OA_CHECK(tougher_last.playable() && tough_last.playable());
    const oa::AssetData high = rising.read("units/RGCOM.fbi");
    const oa::AssetData low = falling.read("units/RGCOM.fbi");
    OA_CHECK(text_of(high.bytes).find("5000") != std::string::npos);
    OA_CHECK(text_of(low.bytes).find("4000") != std::string::npos);
    const mutators::Conflict* high_note =
        conflict_of(tougher_last, mutators::ConflictKind::same_field_hooked);
    const mutators::Conflict* low_note =
        conflict_of(tough_last, mutators::ConflictKind::same_field_hooked);
    OA_CHECK(high_note != nullptr && low_note != nullptr);
    if (high_note != nullptr) {
        const std::vector<std::string> ids{"ridge-tough", "ridge-tougher"};
        OA_CHECK(high_note->field == "maxdamage");
        OA_CHECK(
            mutators::describe(*high_note, ids) ==
            "ridge-tougher wins " + high_note->path + " " + high_note->field + " over ridge-tough"
        );
    }
    if (low_note != nullptr) {
        const std::vector<std::string> ids{"ridge-tougher", "ridge-tough"};
        OA_CHECK(
            mutators::describe(*low_note, ids) ==
            "ridge-tough wins " + low_note->path + " " + low_note->field + " over ridge-tougher"
        );
    }
    release(rising);
    release(falling);
}

void test_same_value_writes_nothing() {
    oa::AssetStore store = open_ridge();
    const mutators::PipelineResult result = mutators::run_pipeline(store, input_of({"ridge-same"}));
    OA_CHECK(result.playable());
    OA_CHECK(result.layer.entries.empty());
    OA_CHECK(result.layer.masks.empty());
    OA_CHECK(!store.mutator_layer_mounted());
    for (const mutators::JournalEntry& entry : result.journal.entries)
        OA_CHECK(entry.field != "maxdamage");
    release(store);
}

void test_failed_mutator_leaves_no_trace() {
    oa::AssetStore with_boom_store = open_ridge();
    oa::AssetStore clean_store = open_ridge();
    const mutators::PipelineResult with_boom = mutators::run_pipeline(
        with_boom_store, input_of({"ridge-tough", "ridge-boom", "ridge-swift"})
    );
    const mutators::PipelineResult clean =
        mutators::run_pipeline(clean_store, input_of({"ridge-tough", "ridge-swift"}));
    OA_CHECK(with_boom.refusal.empty());
    OA_CHECK(!with_boom.playable());
    OA_CHECK(with_boom.mutators.size() == 3);
    if (with_boom.mutators.size() == 3) {
        OA_CHECK(with_boom.mutators[1].id == "ridge-boom");
        OA_CHECK(with_boom.mutators[1].outcome == mutators::Outcome::failed);
        OA_CHECK(with_boom.mutators[1].phase == mutators::Phase::hooks);
        OA_CHECK(with_boom.mutators[1].cause == mutators::Cause::script_error);
        OA_CHECK(with_boom.mutators[1].message.find("boom") != std::string::npos);
        OA_CHECK(with_boom.mutators[0].outcome == mutators::Outcome::applied);
        OA_CHECK(with_boom.mutators[2].outcome == mutators::Outcome::applied);
    }
    OA_CHECK(specs_match(with_boom.layer, clean.layer));
    OA_CHECK(journals_match(with_boom, clean));
    OA_CHECK(reports_match(with_boom, clean));
    for (const mutators::JournalEntry& entry : with_boom.journal.entries) {
        for (const uint16_t slot : entry.mutators)
            OA_CHECK(slot != 1);
    }
    const oa::AssetData bot = with_boom_store.read("units/RGBOT.fbi");
    const oa::AssetData commander = with_boom_store.read("units/RGCOM.fbi");
    OA_CHECK(text_of(bot.bytes).find("2.4") != std::string::npos);
    OA_CHECK(text_of(commander.bytes).find("4000") != std::string::npos);
    release(with_boom_store);
    release(clean_store);
}

void test_budgets() {
    oa::AssetStore loop_store = open_ridge();
    mutators::PipelineInput loop_input = input_of({"ridge-loop"});
    loop_input.budget.instructions = 2000000;
    const mutators::PipelineResult looped = mutators::run_pipeline(loop_store, loop_input);
    OA_CHECK(looped.mutators.size() == 1);
    if (looped.mutators.size() == 1) {
        OA_CHECK(looped.mutators[0].outcome == mutators::Outcome::failed);
        OA_CHECK(looped.mutators[0].cause == mutators::Cause::instruction_budget);
        OA_CHECK(looped.mutators[0].phase == mutators::Phase::hooks);
    }
    OA_CHECK(!loop_store.mutator_layer_mounted());
    release(loop_store);

    oa::AssetStore hog_store = open_ridge();
    mutators::PipelineInput hog_input = input_of({"ridge-hog"});
    hog_input.budget.memory_bytes = std::size_t{1} << 20;
    const mutators::PipelineResult hog = mutators::run_pipeline(hog_store, hog_input);
    OA_CHECK(hog.mutators.size() == 1);
    if (hog.mutators.size() == 1) {
        OA_CHECK(hog.mutators[0].outcome == mutators::Outcome::failed);
        OA_CHECK(hog.mutators[0].cause == mutators::Cause::memory_budget);
        OA_CHECK(hog.mutators[0].phase == mutators::Phase::hooks);
    }
    release(hog_store);
}

void test_plan_failures() {
    oa::AssetStore undeclared_store = open_ridge();
    const mutators::PipelineResult undeclared =
        mutators::run_pipeline(undeclared_store, input_of({"ridge-undeclared"}));
    OA_CHECK(undeclared.mutators.size() == 1);
    if (undeclared.mutators.size() == 1) {
        OA_CHECK(undeclared.mutators[0].outcome == mutators::Outcome::failed);
        OA_CHECK(undeclared.mutators[0].phase == mutators::Phase::plan);
        OA_CHECK(undeclared.mutators[0].cause == mutators::Cause::undeclared_layer);
        OA_CHECK(undeclared.mutators[0].message.find("units") != std::string::npos);
    }
    release(undeclared_store);

    oa::AssetStore wreck_store = open_ridge();
    const mutators::PipelineResult wreck =
        mutators::run_pipeline(wreck_store, input_of({"ridge-wreck-undeclared"}));
    OA_CHECK(wreck.mutators.size() == 1);
    if (wreck.mutators.size() == 1) {
        OA_CHECK(wreck.mutators[0].outcome == mutators::Outcome::failed);
        OA_CHECK(wreck.mutators[0].phase == mutators::Phase::hooks);
        OA_CHECK(wreck.mutators[0].cause == mutators::Cause::script_error);
        OA_CHECK(wreck.mutators[0].message.find("features") != std::string::npos);
    }
    release(wreck_store);

    oa::AssetStore case_store = open_ridge();
    const mutators::PipelineResult folded =
        mutators::run_pipeline(case_store, input_of({"ridge-case"}));
    OA_CHECK(folded.mutators.size() == 1);
    if (folded.mutators.size() == 1) {
        OA_CHECK(folded.mutators[0].outcome == mutators::Outcome::failed);
        OA_CHECK(folded.mutators[0].phase == mutators::Phase::plan);
        OA_CHECK(folded.mutators[0].cause == mutators::Cause::script_error);
        OA_CHECK(folded.mutators[0].message.find("lua/util.lua") != std::string::npos);
    }
    release(case_store);

    oa::AssetStore protected_store = open_ridge();
    const mutators::PipelineResult refused =
        mutators::run_pipeline(protected_store, input_of({"ridge-protected"}));
    OA_CHECK(refused.mutators.size() == 1);
    if (refused.mutators.size() == 1) {
        OA_CHECK(refused.mutators[0].outcome == mutators::Outcome::failed);
        OA_CHECK(refused.mutators[0].phase == mutators::Phase::files);
        OA_CHECK(refused.mutators[0].cause == mutators::Cause::files_refused);
    }
    release(protected_store);
}

void test_files_layer() {
    oa::AssetStore art_store = open_ridge();
    const mutators::PipelineResult art =
        mutators::run_pipeline(art_store, input_of({"ridge-art-a", "ridge-art-b"}));
    OA_CHECK(art.playable());
    const oa::AssetData picture = art_store.read("unitpics/RGBOT.pcx");
    OA_CHECK(text_of(picture.bytes) == "B\n");
    const mutators::Conflict* replaced = conflict_of(art, mutators::ConflictKind::same_path_added);
    OA_CHECK(replaced != nullptr);
    if (replaced != nullptr) {
        const std::vector<std::string> ids{"ridge-art-a", "ridge-art-b"};
        OA_CHECK(
            mutators::describe(*replaced, ids) ==
            "ridge-art-b wins " + replaced->path + " over ridge-art-a"
        );
    }
    release(art_store);

    oa::AssetStore mask_store = open_ridge();
    const mutators::PipelineResult masked =
        mutators::run_pipeline(mask_store, input_of({"ridge-art-a", "ridge-art-mask"}));
    OA_CHECK(masked.playable());
    OA_CHECK(!masked.layer.masks.empty());
    OA_CHECK(!lists_name(mask_store, "unitpics", ".pcx", "rgbot.pcx"));
    const mutators::Conflict* hidden =
        conflict_of(masked, mutators::ConflictKind::added_path_hidden);
    OA_CHECK(hidden != nullptr && hidden->severity == mutators::Severity::warning);
    if (hidden != nullptr) {
        const std::vector<std::string> ids{"ridge-art-a", "ridge-art-mask"};
        OA_CHECK(
            mutators::describe(*hidden, ids) ==
            "ridge-art-mask hides " + hidden->path + " added by ridge-art-a"
        );
    }
    release(mask_store);

    oa::AssetStore added_store = open_ridge();
    const mutators::PipelineResult added =
        mutators::run_pipeline(added_store, input_of({"ridge-add-unit", "ridge-edit-new"}));
    OA_CHECK(added.playable());
    const oa::AssetData created = added_store.read("units/RGNEW.fbi");
    OA_CHECK(text_of(created.bytes).find("2222") != std::string::npos);
    release(added_store);

    oa::AssetStore removed_store = open_ridge();
    const mutators::PipelineResult removed =
        mutators::run_pipeline(removed_store, input_of({"ridge-remove-bot"}));
    OA_CHECK(removed.playable());
    OA_CHECK(!lists_name(removed_store, "units", ".fbi", "rgbot.fbi"));
    // The hook's remove() hid the files, so the journal says the hooks did.
    std::size_t hidden_files = 0;
    for (const mutators::JournalEntry& entry : removed.journal.entries) {
        if (entry.change != mutators::Change::hidden)
            continue;
        ++hidden_files;
        OA_CHECK(entry.phase == mutators::Phase::hooks);
        OA_CHECK(entry.collection == mutators::Collection::files);
    }
    OA_CHECK(hidden_files >= 1);
    release(removed_store);
    OA_CHECK(lists_name(removed_store, "units", ".fbi", "rgbot.fbi"));
}

void test_list_rules() {
    oa::AssetStore early_store = open_ridge();
    const mutators::PipelineResult early =
        mutators::run_pipeline(early_store, input_of({"ridge-needs-swift", "ridge-swift"}));
    OA_CHECK(early.refusal.find("above it") != std::string::npos);
    OA_CHECK(!early.playable());
    OA_CHECK(!early_store.mutator_layer_mounted());
    OA_CHECK(early.layer.entries.empty());
    for (const mutators::MutatorResult& mutator : early.mutators)
        OA_CHECK(mutator.outcome == mutators::Outcome::excluded);
    release(early_store);

    oa::AssetStore late_store = open_ridge();
    const mutators::PipelineResult late =
        mutators::run_pipeline(late_store, input_of({"ridge-swift", "ridge-needs-swift"}));
    OA_CHECK(late.refusal.empty());
    OA_CHECK(late.playable());
    OA_CHECK(late.mutators.size() == 2);
    release(late_store);

    oa::AssetStore boom_store = open_ridge();
    const mutators::PipelineResult boom =
        mutators::run_pipeline(boom_store, input_of({"ridge-boom", "ridge-needs-boom"}));
    OA_CHECK(boom.mutators.size() == 2);
    if (boom.mutators.size() == 2) {
        OA_CHECK(boom.mutators[0].outcome == mutators::Outcome::failed);
        OA_CHECK(boom.mutators[1].outcome == mutators::Outcome::excluded);
        OA_CHECK(boom.mutators[1].cause == mutators::Cause::prerequisite_failed);
        OA_CHECK(boom.mutators[1].phase == mutators::Phase::loadout);
        OA_CHECK(
            boom.mutators[1].message.find("needs ridge-boom, which failed") != std::string::npos
        );
    }
    release(boom_store);

    oa::AssetStore conflict_store = open_ridge();
    const mutators::PipelineResult conflict =
        mutators::run_pipeline(conflict_store, input_of({"ridge-swift", "ridge-against-swift"}));
    OA_CHECK(conflict.refusal.find("conflicts") != std::string::npos);
    OA_CHECK(!conflict_store.mutator_layer_mounted());
    release(conflict_store);

    oa::AssetStore duplicate_store = open_ridge();
    mutators::PipelineInput duplicate =
        make_input({load_id("ridge-swift"), load_id("ridge-swift")});
    const mutators::PipelineResult twice = mutators::run_pipeline(duplicate_store, duplicate);
    OA_CHECK(twice.refusal.find("listed twice") != std::string::npos);
    OA_CHECK(!duplicate_store.mutator_layer_mounted());
    release(duplicate_store);

    const fs::path scratch = oa::test::make_scratch_directory("V01-loadout");
    std::vector<mutators::PipelineMutator> many;
    for (int number = 0; number < 33; ++number) {
        std::string id = "mutator-";
        if (number < 10)
            id += "0";
        id += std::to_string(number);
        const fs::path package = scratch / id;
        const std::string yaml =
            "oamut: 1\nid: " + id + "\nname: \"Loadout " + id +
            "\"\nversion: \"1.0\"\nauthor:\n  name: \"Ridge Maker\"\ntags: [\"gameplay\"]\n"
            "works-with: [\"*\"]\nlayers: [\"files\"]\n";
        write_bytes(package / "oamut.yaml", yaml);
        write_bytes(package / "mutator.lua", "function mutate(ctx)\n  return {}\nend\n");
        many.push_back(load_mutator(package));
    }
    oa::AssetStore crowd_store = open_ridge();
    const mutators::PipelineResult crowd =
        mutators::run_pipeline(crowd_store, make_input(std::move(many)));
    OA_CHECK(crowd.refusal.find("32") != std::string::npos);
    OA_CHECK(!crowd_store.mutator_layer_mounted());
    OA_CHECK(crowd.layer.entries.empty());
    release(crowd_store);
    fs::remove_all(scratch);
}

void test_compatibility_and_repeat() {
    oa::AssetStore elsewhere_store = open_ridge();
    const mutators::PipelineResult elsewhere =
        mutators::run_pipeline(elsewhere_store, input_of({"ridge-elsewhere"}));
    OA_CHECK(elsewhere.mutators.size() == 1);
    if (elsewhere.mutators.size() == 1) {
        OA_CHECK(elsewhere.mutators[0].outcome == mutators::Outcome::incompatible);
        OA_CHECK(elsewhere.mutators[0].phase == mutators::Phase::manifest);
        OA_CHECK(elsewhere.mutators[0].cause == mutators::Cause::works_with);
        OA_CHECK(elsewhere.mutators[0].message.find("some-other-mod") != std::string::npos);
        OA_CHECK(elsewhere.mutators[0].message.find("ridge") != std::string::npos);
    }
    release(elsewhere_store);

    oa::AssetStore future_store = open_ridge();
    const mutators::PipelineResult future =
        mutators::run_pipeline(future_store, input_of({"ridge-future"}));
    OA_CHECK(future.mutators.size() == 1);
    if (future.mutators.size() == 1) {
        OA_CHECK(future.mutators[0].outcome == mutators::Outcome::incompatible);
        OA_CHECK(future.mutators[0].phase == mutators::Phase::manifest);
        OA_CHECK(future.mutators[0].cause == mutators::Cause::engine_version);
        OA_CHECK(future.mutators[0].message.find("99.0") != std::string::npos);
        OA_CHECK(future.mutators[0].message.find("0.8.5") != std::string::npos);
    }
    release(future_store);

    oa::AssetStore first_store = open_ridge();
    oa::AssetStore second_store = open_ridge();
    const mutators::PipelineInput input = input_of({"ridge-tough", "ridge-swift"});
    const mutators::PipelineResult first = mutators::run_pipeline(first_store, input);
    const mutators::PipelineResult second = mutators::run_pipeline(second_store, input);
    OA_CHECK(first.playable() && second.playable());
    OA_CHECK(specs_match(first.layer, second.layer));
    OA_CHECK(journals_match(first, second));
    OA_CHECK(reports_match(first, second));
    release(first_store);
    release(second_store);

    oa::AssetStore mounted = open_ridge();
    const mutators::PipelineResult kept =
        mutators::run_pipeline(mounted, input_of({"ridge-swift"}));
    OA_CHECK(mounted.mutator_layer_mounted());
    const std::string label = kept.layer.label;
    const oa::AssetData before = mounted.read("units/RGBOT.fbi");
    const mutators::PipelineResult refused =
        mutators::run_pipeline(mounted, input_of({"ridge-swift"}));
    OA_CHECK(refused.refusal.find("already mounted") != std::string::npos);
    OA_CHECK(mounted.mutator_layer_mounted());
    OA_CHECK(kept.layer.label == label);
    const oa::AssetData after = mounted.read("units/RGBOT.fbi");
    OA_CHECK(before.bytes == after.bytes);
    release(mounted);
}

void test_failure_ends_the_pass() {
    // A hook that fails ends the pass before any mutator below it runs.
    // The retry is that lower mutator on its own.
    oa::AssetStore rule_store = open_ridge();
    oa::AssetStore picky_store = open_ridge();
    const mutators::PipelineResult rule_boom =
        mutators::run_pipeline(rule_store, input_of({"ridge-rule-boom", "ridge-picky"}));
    const mutators::PipelineResult picky_alone =
        mutators::run_pipeline(picky_store, input_of({"ridge-picky"}));
    OA_CHECK(rule_boom.mutators.size() == 2);
    if (rule_boom.mutators.size() == 2) {
        OA_CHECK(rule_boom.mutators[0].outcome == mutators::Outcome::failed);
        OA_CHECK(rule_boom.mutators[0].phase == mutators::Phase::hooks);
        OA_CHECK(rule_boom.mutators[0].cause == mutators::Cause::script_error);
        OA_CHECK(rule_boom.mutators[0].message.find("rule-boom hook") != std::string::npos);
        OA_CHECK(rule_boom.mutators[1].outcome == mutators::Outcome::applied);
    }
    check_same_run(rule_boom, picky_alone);
    check_no_trace(rule_boom, 0);
    OA_CHECK(text_of(rule_store.read("units/RGBOT.fbi").bytes).find("1234") != std::string::npos);
    OA_CHECK(
        text_of(rule_store.read("units/RGCOM.fbi").bytes).find("MaxDamage=3000") !=
        std::string::npos
    );
    release(rule_store);
    release(picky_store);

    // A failing hook's own files never reach the hooks below it.
    oa::AssetStore files_store = open_ridge();
    oa::AssetStore no_extra_store = open_ridge();
    const mutators::PipelineResult files_boom =
        mutators::run_pipeline(files_store, input_of({"ridge-files-boom", "ridge-no-extra"}));
    const mutators::PipelineResult no_extra_alone =
        mutators::run_pipeline(no_extra_store, input_of({"ridge-no-extra"}));
    OA_CHECK(files_boom.mutators.size() == 2);
    if (files_boom.mutators.size() == 2) {
        OA_CHECK(files_boom.mutators[0].outcome == mutators::Outcome::failed);
        OA_CHECK(files_boom.mutators[0].phase == mutators::Phase::hooks);
        OA_CHECK(files_boom.mutators[0].message.find("files-boom hook") != std::string::npos);
        OA_CHECK(files_boom.mutators[1].outcome == mutators::Outcome::applied);
    }
    check_same_run(files_boom, no_extra_alone);
    check_no_trace(files_boom, 0);
    OA_CHECK(!lists_name(files_store, "units", ".fbi", "rgxtra.fbi"));
    OA_CHECK(text_of(files_store.read("units/RGLAB.fbi").bytes).find("222") != std::string::npos);
    release(files_store);
    release(no_extra_store);

    // The same with the failing mutator's dependant between them.
    oa::AssetStore chain_store = open_ridge();
    oa::AssetStore chain_alone_store = open_ridge();
    const mutators::PipelineResult chain = mutators::run_pipeline(
        chain_store, input_of({"ridge-rule-boom", "ridge-needs-rule-boom", "ridge-picky"})
    );
    const mutators::PipelineResult chain_alone =
        mutators::run_pipeline(chain_alone_store, input_of({"ridge-picky"}));
    OA_CHECK(chain.mutators.size() == 3);
    if (chain.mutators.size() == 3) {
        OA_CHECK(chain.mutators[0].outcome == mutators::Outcome::failed);
        OA_CHECK(chain.mutators[1].outcome == mutators::Outcome::excluded);
        OA_CHECK(chain.mutators[1].cause == mutators::Cause::prerequisite_failed);
        OA_CHECK(chain.mutators[1].phase == mutators::Phase::loadout);
        OA_CHECK(
            chain.mutators[1].message.find("needs ridge-rule-boom, which failed") !=
            std::string::npos
        );
        OA_CHECK(chain.mutators[2].outcome == mutators::Outcome::applied);
    }
    check_same_run(chain, chain_alone);
    check_no_trace(chain, 0);
    check_no_trace(chain, 1);
    OA_CHECK(text_of(chain_store.read("units/RGBOT.fbi").bytes).find("777") == std::string::npos);
    release(chain_store);
    release(chain_alone_store);
}

void test_dependants_of_dependants() {
    oa::AssetStore with_store = open_ridge();
    oa::AssetStore swift_store = open_ridge();
    const mutators::PipelineResult with_chain = mutators::run_pipeline(
        with_store,
        input_of({"ridge-boom", "ridge-needs-boom", "ridge-needs-needs-boom", "ridge-swift"})
    );
    const mutators::PipelineResult swift_alone =
        mutators::run_pipeline(swift_store, input_of({"ridge-swift"}));
    OA_CHECK(with_chain.mutators.size() == 4);
    if (with_chain.mutators.size() == 4) {
        OA_CHECK(with_chain.mutators[0].outcome == mutators::Outcome::failed);
        OA_CHECK(with_chain.mutators[1].outcome == mutators::Outcome::excluded);
        OA_CHECK(with_chain.mutators[1].phase == mutators::Phase::loadout);
        OA_CHECK(
            with_chain.mutators[1].message == "ridge-needs-boom needs ridge-boom, which failed"
        );
        OA_CHECK(with_chain.mutators[2].outcome == mutators::Outcome::excluded);
        OA_CHECK(with_chain.mutators[2].phase == mutators::Phase::loadout);
        OA_CHECK(with_chain.mutators[2].cause == mutators::Cause::prerequisite_failed);
        OA_CHECK(
            with_chain.mutators[2].message ==
            "ridge-needs-needs-boom needs ridge-needs-boom, which did not apply"
        );
        OA_CHECK(with_chain.mutators[3].outcome == mutators::Outcome::applied);
    }
    check_same_run(with_chain, swift_alone);
    release(with_store);
    release(swift_store);
}

void test_plan_tables_are_plain() {
    // The plan is read by raw access inside the plan phase. A files table
    // with a metatable is refused there, so its __index never runs: neither
    // its error nor its endless loop can reach the engine.
    for (const std::string_view id : {"ridge-files-raise", "ridge-files-loop"}) {
        oa::AssetStore store = open_ridge();
        mutators::PipelineInput input = input_of({id});
        input.budget.instructions = 2000000;
        const mutators::PipelineResult result = mutators::run_pipeline(store, input);
        OA_CHECK(result.refusal.empty());
        OA_CHECK(result.mutators.size() == 1);
        if (result.mutators.size() == 1) {
            OA_CHECK(result.mutators[0].outcome == mutators::Outcome::failed);
            OA_CHECK(result.mutators[0].phase == mutators::Phase::plan);
            OA_CHECK(result.mutators[0].cause == mutators::Cause::invalid_plan);
            OA_CHECK(result.mutators[0].message.find("metatable") != std::string::npos);
        }
        OA_CHECK(!store.mutator_layer_mounted());
        release(store);
    }
}

void test_mask_is_journaled() {
    oa::AssetStore store = open_ridge();
    const mutators::PipelineResult masked =
        mutators::run_pipeline(store, input_of({"ridge-mask-lab"}));
    OA_CHECK(masked.playable());
    OA_CHECK(masked.layer.masks.size() == 1);
    OA_CHECK(!lists_name(store, "units", ".fbi", "rglab.fbi"));
    const std::vector<mutators::JournalEntry> own = masked.journal.entries_of(0);
    OA_CHECK(own.size() == 1);
    if (own.size() == 1) {
        OA_CHECK(own[0].change == mutators::Change::hidden);
        OA_CHECK(own[0].phase == mutators::Phase::files);
        OA_CHECK(own[0].collection == mutators::Collection::files);
        OA_CHECK(count_paths({own[0].path}, "units/rglab.fbi") == 1);
        OA_CHECK(own[0].mutators == std::vector<uint16_t>{0});
    }
    release(store);
}

void test_string_cap_is_its_own_cause() {
    oa::AssetStore store = open_ridge();
    const mutators::PipelineResult result =
        mutators::run_pipeline(store, input_of({"ridge-long-text"}));
    OA_CHECK(result.mutators.size() == 1);
    if (result.mutators.size() == 1) {
        OA_CHECK(result.mutators[0].outcome == mutators::Outcome::failed);
        OA_CHECK(result.mutators[0].phase == mutators::Phase::hooks);
        OA_CHECK(result.mutators[0].cause == mutators::Cause::string_length);
    }
    release(store);
}

void test_unread_document_warned_once() {
    const fs::path scratch = oa::test::make_scratch_directory("V01-unread");
    const fs::path copy = scratch / "ridge";
    fs::copy(k_data / "ridge", copy, fs::copy_options::recursive);
    write_bytes(copy / "units" / "RGBAD.fbi", "[UNITINFO]\n{\nUnitName=RGBAD;\n");
    {
        oa::AssetStore store(copy);
        const mutators::PipelineResult result =
            mutators::run_pipeline(store, input_of({"ridge-swift"}));
        OA_CHECK(result.playable());
        OA_CHECK(count_paths(result.report.unread, "units/rgbad.fbi") == 1);
        release(store);
    }
    fs::remove_all(scratch);
}

/// Two assignments of one field. The later mutator wins, in either order,
/// and the report names that pair once even though every unit was written.
void test_later_assignment_wins() {
    oa::AssetStore forward_store = open_ridge();
    oa::AssetStore backward_store = open_ridge();
    const mutators::PipelineResult forward =
        mutators::run_pipeline(forward_store, input_of({"ridge-mark-low", "ridge-mark-high"}));
    const mutators::PipelineResult backward =
        mutators::run_pipeline(backward_store, input_of({"ridge-mark-high", "ridge-mark-low"}));
    OA_CHECK(forward.playable() && backward.playable());
    for (const char* path : {"units/RGCOM.fbi", "units/RGBOT.fbi", "units/RGLAB.fbi"}) {
        const std::string high = text_of(forward_store.read(path).bytes);
        const std::string low = text_of(backward_store.read(path).bytes);
        OA_CHECK(high.find("2500") != std::string::npos);
        OA_CHECK(high.find("1500") == std::string::npos);
        OA_CHECK(low.find("1500") != std::string::npos);
        OA_CHECK(low.find("2500") == std::string::npos);
    }
    OA_CHECK(count_kind(forward, mutators::ConflictKind::value_replaced) == 1);
    OA_CHECK(count_kind(backward, mutators::ConflictKind::value_replaced) == 1);
    const mutators::Conflict* replaced =
        conflict_of(forward, mutators::ConflictKind::value_replaced);
    OA_CHECK(replaced != nullptr);
    if (replaced != nullptr) {
        const std::vector<std::string> ids{"ridge-mark-low", "ridge-mark-high"};
        OA_CHECK(replaced->field == "maxdamage");
        OA_CHECK(replaced->mutators.size() == 2 && replaced->mutators[0] == 0);
        OA_CHECK(replaced->mutators.size() == 2 && replaced->mutators[1] == 1);
        OA_CHECK(
            mutators::describe(*replaced, ids) ==
            "ridge-mark-high replaced ridge-mark-low's value on " + replaced->path + " maxdamage"
        );
    }
    release(forward_store);
    release(backward_store);
}

/// A rule below a hook reads the value that hook wrote. Rules of every
/// mutator first would scale before the hook, and the bot would stay at 1000.
void test_rule_sees_the_hook_above() {
    oa::AssetStore store = open_ridge();
    const mutators::PipelineResult stepped =
        mutators::run_pipeline(store, input_of({"ridge-stamp", "ridge-double-stamp"}));
    OA_CHECK(stepped.playable());
    const std::string bot = text_of(store.read("units/RGBOT.fbi").bytes);
    const std::string commander = text_of(store.read("units/RGCOM.fbi").bytes);
    OA_CHECK(bot.find("2000") != std::string::npos);
    OA_CHECK(bot.find("1000") == std::string::npos);
    OA_CHECK(commander.find("6000") != std::string::npos);
    release(store);

    oa::AssetStore reversed_store = open_ridge();
    const mutators::PipelineResult rules_first =
        mutators::run_pipeline(reversed_store, input_of({"ridge-double-stamp", "ridge-stamp"}));
    OA_CHECK(rules_first.playable());
    const std::string untouched = text_of(reversed_store.read("units/RGBOT.fbi").bytes);
    OA_CHECK(untouched.find("1000") != std::string::npos);
    OA_CHECK(untouched.find("2000") == std::string::npos);
    release(reversed_store);
}

/// A hook that sets one field on every unit replaces the rule above it, and
/// the report names that pair once.
void test_hook_replaces_a_rule() {
    oa::AssetStore store = open_ridge();
    const mutators::PipelineResult result =
        mutators::run_pipeline(store, input_of({"ridge-mark-low", "ridge-hook-mark"}));
    OA_CHECK(result.playable());
    for (const char* path : {"units/RGCOM.fbi", "units/RGBOT.fbi", "units/RGLAB.fbi"}) {
        const std::string text = text_of(store.read(path).bytes);
        OA_CHECK(text.find("4242") != std::string::npos);
        OA_CHECK(text.find("1500") == std::string::npos);
    }
    OA_CHECK(count_kind(result, mutators::ConflictKind::same_field_hooked) == 1);
    const mutators::Conflict* hooked =
        conflict_of(result, mutators::ConflictKind::same_field_hooked);
    OA_CHECK(hooked != nullptr);
    if (hooked != nullptr) {
        const std::vector<std::string> ids{"ridge-mark-low", "ridge-hook-mark"};
        OA_CHECK(hooked->field == "maxdamage");
        OA_CHECK(hooked->mutators.size() == 2 && hooked->mutators[0] == 0);
        OA_CHECK(hooked->mutators.size() == 2 && hooked->mutators[1] == 1);
        OA_CHECK(
            mutators::describe(*hooked, ids) ==
            "ridge-hook-mark wins " + hooked->path + " maxdamage over ridge-mark-low"
        );
    }
    release(store);
}

/// Each step names the mutator who last wrote the field. A rule under a hook
/// names that hook, and a hook under a later rule names that rule.
void test_each_step_names_the_last_writer() {
    oa::AssetStore store = open_ridge();
    const mutators::PipelineResult stepped = mutators::run_pipeline(
        store, input_of({"ridge-mark-low", "ridge-hook-mark", "ridge-mark-high"})
    );
    OA_CHECK(stepped.playable());
    OA_CHECK(text_of(store.read("units/RGCOM.fbi").bytes).find("2500") != std::string::npos);
    OA_CHECK(count_kind(stepped, mutators::ConflictKind::same_field_hooked) == 1);
    OA_CHECK(count_kind(stepped, mutators::ConflictKind::value_replaced) == 1);
    const std::vector<std::string> three{"ridge-mark-low", "ridge-hook-mark", "ridge-mark-high"};
    const mutators::Conflict* hooked =
        conflict_of(stepped, mutators::ConflictKind::same_field_hooked);
    const mutators::Conflict* replaced =
        conflict_of(stepped, mutators::ConflictKind::value_replaced);
    OA_CHECK(hooked != nullptr && replaced != nullptr);
    if (hooked != nullptr) {
        OA_CHECK(hooked->mutators.size() == 2 && hooked->mutators[0] == 0);
        OA_CHECK(hooked->mutators.size() == 2 && hooked->mutators[1] == 1);
        OA_CHECK(
            mutators::describe(*hooked, three) ==
            "ridge-hook-mark wins " + hooked->path + " maxdamage over ridge-mark-low"
        );
    }
    if (replaced != nullptr) {
        OA_CHECK(replaced->mutators.size() == 2 && replaced->mutators[0] == 1);
        OA_CHECK(replaced->mutators.size() == 2 && replaced->mutators[1] == 2);
        OA_CHECK(
            mutators::describe(*replaced, three) ==
            "ridge-mark-high replaced ridge-hook-mark's value on " + replaced->path + " maxdamage"
        );
    }
    release(store);

    oa::AssetStore chain_store = open_ridge();
    const mutators::PipelineResult chain = mutators::run_pipeline(
        chain_store,
        input_of({"ridge-mark-low", "ridge-hook-mark", "ridge-mark-high", "ridge-stamp"})
    );
    OA_CHECK(chain.playable());
    const std::string bot = text_of(chain_store.read("units/RGBOT.fbi").bytes);
    OA_CHECK(bot.find("1000") != std::string::npos);
    OA_CHECK(bot.find("2500") == std::string::npos);
    OA_CHECK(count_kind(chain, mutators::ConflictKind::same_field_hooked) == 2);
    const std::vector<std::string> four{
        "ridge-mark-low", "ridge-hook-mark", "ridge-mark-high", "ridge-stamp"
    };
    bool stamp_names_the_rule = false;
    bool stamp_names_the_hook = false;
    for (const mutators::Conflict& conflict : chain.report.conflicts) {
        if (conflict.kind != mutators::ConflictKind::same_field_hooked)
            continue;
        const std::string line = mutators::describe(conflict, four);
        if (line.find("ridge-stamp wins ") == std::string::npos)
            continue;
        if (line.find("over ridge-mark-high") != std::string::npos)
            stamp_names_the_rule = true;
        if (line.find("over ridge-hook-mark") != std::string::npos)
            stamp_names_the_hook = true;
    }
    OA_CHECK(stamp_names_the_rule);
    OA_CHECK(!stamp_names_the_hook);
    release(chain_store);
}

/// A lock holds against a later mutator's rules and is noted. That mutator's
/// hook still writes the field, and the report names both.
void test_lock_holds_against_rules_not_hooks() {
    oa::AssetStore store = open_ridge();
    const mutators::PipelineResult result =
        mutators::run_pipeline(store, input_of({"ridge-lock-com", "ridge-past-lock"}));
    OA_CHECK(result.playable());
    const std::string commander = text_of(store.read("units/RGCOM.fbi").bytes);
    OA_CHECK(commander.find("7777") != std::string::npos);
    OA_CHECK(commander.find("9999") == std::string::npos);
    OA_CHECK(commander.find("1111") == std::string::npos);
    OA_CHECK(count_kind(result, mutators::ConflictKind::field_locked) == 1);
    OA_CHECK(count_kind(result, mutators::ConflictKind::same_field_hooked) == 1);
    const std::vector<std::string> ids{"ridge-lock-com", "ridge-past-lock"};
    const mutators::Conflict* locked = conflict_of(result, mutators::ConflictKind::field_locked);
    OA_CHECK(locked != nullptr);
    if (locked != nullptr) {
        OA_CHECK(locked->field == "maxdamage");
        OA_CHECK(
            mutators::describe(*locked, ids) ==
            "ridge-lock-com locked " + locked->path + " maxdamage, so ridge-past-lock left it alone"
        );
    }
    const mutators::Conflict* hooked =
        conflict_of(result, mutators::ConflictKind::same_field_hooked);
    OA_CHECK(hooked != nullptr);
    if (hooked != nullptr) {
        OA_CHECK(hooked->field == "maxdamage");
        OA_CHECK(
            mutators::describe(*hooked, ids) ==
            "ridge-past-lock wins " + hooked->path + " maxdamage over ridge-lock-com"
        );
    }
    bool wrote_blocked_value = false;
    for (const mutators::JournalEntry& entry : result.journal.entries) {
        if (entry.new_text.has_value() && *entry.new_text == "9999")
            wrote_blocked_value = true;
    }
    OA_CHECK(!wrote_blocked_value);
    release(store);
}

void test_player_option() {
    mutators::PipelineMutator swift = load_id("ridge-swift");
    for (const oamut::Option& option : swift.manifest.options) {
        if (option.id != "factor")
            continue;
        mutators::LoadoutOption chosen = mutators::default_option(option);
        for (const oamut::ChoiceValue& value : option.values) {
            if (value.text == "2")
                chosen.value = value;
        }
        swift.options.push_back(chosen);
    }
    OA_CHECK(swift.options.size() == 1);
    std::vector<mutators::PipelineMutator> list;
    list.push_back(std::move(swift));
    oa::AssetStore store = open_ridge();
    const mutators::PipelineResult result =
        mutators::run_pipeline(store, make_input(std::move(list)));
    OA_CHECK(result.playable());
    OA_CHECK(text_of(store.read("units/RGBOT.fbi").bytes).find("3.2") != std::string::npos);
    release(store);
}

} // namespace

int main() {
    test_swift_writes_one_file();
    test_rules_are_order_independent();
    test_later_assignment_wins();
    test_rule_sees_the_hook_above();
    test_hook_replaces_a_rule();
    test_each_step_names_the_last_writer();
    test_lock_holds_against_rules_not_hooks();
    test_hooks_follow_the_list();
    test_same_value_writes_nothing();
    test_failed_mutator_leaves_no_trace();
    test_budgets();
    test_plan_failures();
    test_files_layer();
    test_list_rules();
    test_compatibility_and_repeat();
    test_failure_ends_the_pass();
    test_dependants_of_dependants();
    test_plan_tables_are_plain();
    test_mask_is_journaled();
    test_string_cap_is_its_own_cause();
    test_unread_document_warned_once();
    test_player_option();
    return oa::test::check_exit_status();
}
