// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Dry-run verdicts over the made-up Ridge packages. Each fixture verdict
// comes from run_pipeline, and run_dry_run agrees with it. Two threads
// share nothing but the job.
#include "oa/data/defs/layout.hpp"
#include "oa/data/mutators/dry_run.hpp"
#include "oa/formats/oamut/manifest.hpp"
#include "oa/test/check.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifndef OA_MUTATORS_TEST_DATA
#error OA_MUTATORS_TEST_DATA is the fixture folder
#endif

namespace {

namespace mutators = oa::data::mutators;
namespace oamut = oa::formats::oamut;
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

mutators::PipelineMutator load_mutator(const fs::path& package) {
    mutators::PipelineMutator mutator;
    mutator.package = package;
    const std::vector<uint8_t> bytes = read_bytes(package / "oamut.yaml");
    std::vector<oamut::Problem> problems;
    const bool read = oamut::read_manifest(bytes, mutator.manifest, problems);
    if (!read) {
        for (const oamut::Problem& problem : problems) {
            std::fprintf(stderr, "%s: %s\n", package.string().c_str(), problem.message.c_str());
        }
    }
    OA_CHECK(read);
    return mutator;
}

mutators::PipelineMutator load_id(std::string_view id) {
    return load_mutator(k_data / "dry-run" / std::string{id});
}

mutators::PipelineInput make_input(std::vector<mutators::PipelineMutator> list) {
    mutators::PipelineInput input;
    input.mutators = std::move(list);
    input.mod.id = "ridge";
    input.mod.version = "1.0";
    input.mod.game = "ridge";
    input.engine_version = "0.8.5";
    input.layout = &oa::data::defs::data_layout();
    input.seed = 1;
    return input;
}

mutators::DryRunJob job_of(std::vector<std::string_view> ids) {
    mutators::DryRunJob job;
    job.loose_roots.push_back(k_data / "ridge");
    job.mod_label = "Ridge 1.0";
    std::vector<mutators::PipelineMutator> list;
    for (const std::string_view id : ids)
        list.push_back(load_id(id));
    job.input = make_input(std::move(list));
    return job;
}

void release(oa::AssetStore& store) {
    if (store.mutator_layer_mounted())
        store.unmount_mutator_layer();
}

mutators::VerdictRecord direct_verdict(const mutators::DryRunJob& job) {
    oa::AssetStore store(k_data / "ridge");
    const mutators::PipelineResult result = mutators::run_pipeline(store, job.input);
    const mutators::VerdictRecord record =
        mutators::verdict_of(result, job.mod_label, job.input.mutators);
    release(store);
    return record;
}

void expect_record(
    std::string_view id,
    const mutators::VerdictRecord& record,
    mutators::Verdict verdict,
    uint32_t units_changed
) {
    OA_CHECK(record.verdict == verdict);
    OA_CHECK(record.counts.units_changed == units_changed);
    OA_CHECK(record.counts.units_added == 0);
    OA_CHECK(record.counts.units_removed == 0);
    OA_CHECK(record.counts.weapons_changed == 0);
    OA_CHECK(record.counts.weapons_added == 0);
    OA_CHECK(record.counts.weapons_removed == 0);
    OA_CHECK(record.counts.features_changed == 0);
    OA_CHECK(record.counts.features_added == 0);
    OA_CHECK(record.counts.features_removed == 0);
    OA_CHECK(record.counts.menu_entries_changed == 0);
    OA_CHECK(record.counts.files_added == 0);
    OA_CHECK(record.counts.files_hidden == 0);
    OA_CHECK(record.mod_label == "Ridge 1.0");
    if (record.verdict != verdict || record.counts.units_changed != units_changed) {
        std::fprintf(
            stderr,
            "%s: %s units %u (%s)\n",
            std::string{id}.c_str(),
            std::string{mutators::verdict_word(record.verdict)}.c_str(),
            record.counts.units_changed,
            record.reason.c_str()
        );
    }
}

void expect_run(std::string_view id, mutators::Verdict verdict, uint32_t units_changed) {
    const mutators::DryRunJob job = job_of({id});
    const std::atomic<bool> cancel{false};
    const mutators::VerdictRecord ran = mutators::run_dry_run(job, cancel);
    const mutators::VerdictRecord direct = direct_verdict(job);
    OA_CHECK(ran == direct);
    expect_record(id, ran, verdict, units_changed);
}

void test_fixture_verdicts() {
    expect_run("ridge-dry-bot", mutators::Verdict::compatible, 1);
    const mutators::VerdictRecord bot =
        mutators::run_dry_run(job_of({"ridge-dry-bot"}), std::atomic<bool>{false});
    OA_CHECK(bot.reason.empty());

    expect_run("ridge-dry-none", mutators::Verdict::no_effect, 0);
    const mutators::VerdictRecord none =
        mutators::run_dry_run(job_of({"ridge-dry-none"}), std::atomic<bool>{false});
    OA_CHECK(none.reason.empty());

    expect_run("ridge-dry-stop", mutators::Verdict::incompatible, 0);
    const mutators::VerdictRecord stop =
        mutators::run_dry_run(job_of({"ridge-dry-stop"}), std::atomic<bool>{false});
    OA_CHECK(stop.reason == "needs a second side");

    expect_run("ridge-dry-missing", mutators::Verdict::incompatible, 0);
    const mutators::VerdictRecord missing =
        mutators::run_dry_run(job_of({"ridge-dry-missing"}), std::atomic<bool>{false});
    OA_CHECK(!missing.reason.empty());
    if (missing.reason.find("nil") == std::string::npos &&
        missing.reason.find("NOUNIT") == std::string::npos) {
        std::fprintf(stderr, "ridge-dry-missing: %s\n", missing.reason.c_str());
    }
    OA_CHECK(
        missing.reason.find("nil") != std::string::npos ||
        missing.reason.find("NOUNIT") != std::string::npos
    );

    expect_run("ridge-dry-syntax", mutators::Verdict::failed, 0);
    const mutators::VerdictRecord syntax =
        mutators::run_dry_run(job_of({"ridge-dry-syntax"}), std::atomic<bool>{false});
    OA_CHECK(!syntax.reason.empty());

    expect_run("ridge-dry-menu", mutators::Verdict::incompatible, 0);
    const mutators::VerdictRecord menu =
        mutators::run_dry_run(job_of({"ridge-dry-menu"}), std::atomic<bool>{false});
    OA_CHECK(menu.reason.find("NOUNIT") != std::string::npos);
    if (menu.reason.find("NOUNIT") == std::string::npos)
        std::fprintf(stderr, "ridge-dry-menu: %s\n", menu.reason.c_str());

    expect_run("ridge-dry-twice", mutators::Verdict::compatible, 1);
    expect_run("ridge-dry-fields", mutators::Verdict::compatible, 1);

    expect_run("ridge-dry-first", mutators::Verdict::compatible, 1);
    const mutators::DryRunJob both = job_of({"ridge-dry-first", "ridge-dry-after"});
    const std::atomic<bool> cancel{false};
    const mutators::VerdictRecord last = mutators::run_dry_run(both, cancel);
    const mutators::VerdictRecord direct = direct_verdict(both);
    OA_CHECK(last == direct);
    OA_CHECK(last.verdict == mutators::Verdict::no_effect);
    OA_CHECK(last.reason.empty());
    OA_CHECK(last.counts.units_changed == 0);
    if (last.verdict != mutators::Verdict::no_effect) {
        std::fprintf(
            stderr,
            "ridge-dry-after: %s (%s)\n",
            std::string{mutators::verdict_word(last.verdict)}.c_str(),
            last.reason.c_str()
        );
    }

    expect_run("ridge-dry-side", mutators::Verdict::compatible, 0);

    expect_run("ridge-dry-maps", mutators::Verdict::compatible, 0);
    const mutators::VerdictRecord maps =
        mutators::run_dry_run(job_of({"ridge-dry-maps"}), std::atomic<bool>{false});
    OA_CHECK(maps.reason.empty());
    OA_CHECK(!mutators::any(maps.counts));

    // A maps prerequisite above a mutator that changes nothing is that
    // prerequisite's change, not the last mutator's.
    const mutators::DryRunJob below = job_of({"ridge-dry-maps", "ridge-dry-none"});
    const mutators::VerdictRecord quiet_below =
        mutators::run_dry_run(below, std::atomic<bool>{false});
    OA_CHECK(quiet_below == direct_verdict(below));
    OA_CHECK(quiet_below.verdict == mutators::Verdict::no_effect);
}

void test_threads_and_cancel() {
    const mutators::DryRunJob job = job_of({"ridge-dry-bot"});
    const mutators::VerdictRecord first = direct_verdict(job);
    mutators::VerdictRecord left;
    mutators::VerdictRecord right;
    std::thread one([&job, &left] {
        const std::atomic<bool> cancel{false};
        left = mutators::run_dry_run(job, cancel);
    });
    std::thread two([&job, &right] {
        const std::atomic<bool> cancel{false};
        right = mutators::run_dry_run(job, cancel);
    });
    one.join();
    two.join();
    OA_CHECK(left == right);
    OA_CHECK(left == first);

    mutators::DryRunJob cancelled = job;
    cancelled.loose_roots = {k_data / "dry-run" / "missing-store"};
    const std::atomic<bool> stop{true};
    const mutators::VerdictRecord stopped = mutators::run_dry_run(cancelled, stop);
    OA_CHECK(stopped.verdict == mutators::Verdict::failed);
    OA_CHECK(stopped.reason == mutators::dry_run_cancelled_reason);
    OA_CHECK(stopped.counts == mutators::ChangeCounts{});
    OA_CHECK(stopped.mod_label == "Ridge 1.0");

    mutators::DryRunJob empty_roots = job;
    empty_roots.loose_roots.clear();
    const std::atomic<bool> go{false};
    const mutators::VerdictRecord empty = mutators::run_dry_run(empty_roots, go);
    OA_CHECK(empty.verdict == mutators::Verdict::failed);
    OA_CHECK(empty.reason.find("folder") != std::string::npos);

    mutators::DryRunJob bad_mount = job;
    bad_mount.mounts.push_back(k_data / "dry-run" / "no-such.hpi");
    const mutators::VerdictRecord mounted = mutators::run_dry_run(bad_mount, go);
    OA_CHECK(mounted.verdict == mutators::Verdict::failed);
    OA_CHECK(mounted.reason != mutators::dry_run_cancelled_reason);
    OA_CHECK(!mounted.reason.empty());
}

void test_mapped_outcomes() {
    mutators::PipelineResult empty;
    const mutators::VerdictRecord none = mutators::verdict_of(empty, "");
    OA_CHECK(none.verdict == mutators::Verdict::failed);
    OA_CHECK(none.reason == "the list names no mutator");
    OA_CHECK(none.mod_label.empty());

    mutators::PipelineResult refused;
    refused.refusal = "the list names ridge-dry-bot twice";
    mutators::MutatorResult excluded;
    excluded.outcome = mutators::Outcome::excluded;
    excluded.phase = mutators::Phase::loadout;
    excluded.cause = mutators::Cause::none;
    refused.mutators.push_back(excluded);
    const mutators::VerdictRecord refusal = mutators::verdict_of(refused, "Ridge 1.0");
    OA_CHECK(refusal.verdict == mutators::Verdict::failed);
    OA_CHECK(refusal.reason == refused.refusal);

    mutators::MutatorResult blocked;
    blocked.outcome = mutators::Outcome::excluded;
    blocked.cause = mutators::Cause::prerequisite_failed;
    blocked.message = "ridge-dry-after needs ridge-dry-first, which failed";
    mutators::PipelineResult needs;
    needs.mutators.push_back(blocked);
    const mutators::VerdictRecord prerequisite = mutators::verdict_of(needs, "");
    OA_CHECK(prerequisite.verdict == mutators::Verdict::failed);
    OA_CHECK(prerequisite.reason == blocked.message);

    const auto one = [](mutators::Phase phase, mutators::Cause cause) {
        mutators::PipelineResult result;
        mutators::MutatorResult mutator;
        mutator.outcome = mutators::Outcome::failed;
        mutator.phase = phase;
        mutator.cause = cause;
        mutator.message = "stopped";
        mutators::JournalEntry entry;
        entry.mutators.push_back(0);
        entry.collection = mutators::Collection::units;
        entry.change = mutators::Change::set;
        entry.record = "RGBOT";
        entry.field = "maxdamage";
        entry.old_text = "1";
        entry.new_text = "2";
        result.journal.entries.push_back(entry);
        result.mutators.push_back(std::move(mutator));
        return mutators::verdict_of(result, "");
    };
    const mutators::VerdictRecord budget =
        one(mutators::Phase::hooks, mutators::Cause::instruction_budget);
    OA_CHECK(budget.verdict == mutators::Verdict::failed);
    OA_CHECK(budget.counts == mutators::ChangeCounts{});
    const mutators::VerdictRecord script =
        one(mutators::Phase::hooks, mutators::Cause::script_error);
    OA_CHECK(script.verdict == mutators::Verdict::incompatible);
    OA_CHECK(script.reason == "stopped");
    OA_CHECK(script.counts == mutators::ChangeCounts{});
    const mutators::VerdictRecord broken =
        one(mutators::Phase::integrity, mutators::Cause::integrity);
    OA_CHECK(broken.verdict == mutators::Verdict::incompatible);
    const mutators::VerdictRecord weapons =
        one(mutators::Phase::integrity, mutators::Cause::weapon_ids);
    OA_CHECK(weapons.verdict == mutators::Verdict::incompatible);
    const mutators::VerdictRecord plan = one(mutators::Phase::plan, mutators::Cause::script_error);
    OA_CHECK(plan.verdict == mutators::Verdict::failed);

    // Every download file names its sections MENUENTRY1 and up, and one unit
    // is a button on several pages: one name in two files is two entries,
    // and two fields of one button are one.
    mutators::PipelineResult menus;
    mutators::MutatorResult builder;
    builder.outcome = mutators::Outcome::applied;
    menus.mutators.push_back(builder);
    const auto menu_entry = [&menus](
                                mutators::Change change,
                                std::string_view path,
                                std::string_view record,
                                std::string_view field
                            ) {
        mutators::JournalEntry entry;
        entry.mutators.push_back(0);
        entry.collection = mutators::Collection::menus;
        entry.change = change;
        entry.path = std::string{path};
        entry.record = std::string{record};
        entry.field = std::string{field};
        if (change == mutators::Change::set) {
            entry.old_text = "0";
            entry.new_text = "1";
        }
        menus.journal.entries.push_back(std::move(entry));
    };
    menu_entry(mutators::Change::removed, "download/armhp.tdf", "MENUENTRY3", "");
    menu_entry(mutators::Change::removed, "download/corhp.tdf", "MENUENTRY3", "");
    menu_entry(mutators::Change::set, "guis/ARMCV2.GUI", "ARMAP", "grayedout");
    menu_entry(mutators::Change::set, "guis/ARMCV2.GUI", "ARMAP", "commonattribs");
    menu_entry(mutators::Change::set, "guis/ARMCOM2.GUI", "ARMAP", "grayedout");
    const mutators::VerdictRecord greyed = mutators::verdict_of(menus, "");
    OA_CHECK(greyed.verdict == mutators::Verdict::compatible);
    OA_CHECK(greyed.counts.menu_entries_changed == 4);
    if (greyed.counts.menu_entries_changed != 4) {
        std::fprintf(stderr, "menu entries: %u\n", greyed.counts.menu_entries_changed);
    }

    mutators::PipelineResult declared;
    mutators::MutatorResult named;
    named.outcome = mutators::Outcome::incompatible;
    named.cause = mutators::Cause::declared;
    named.message = "";
    declared.mutators.push_back(named);
    const mutators::VerdictRecord blank = mutators::verdict_of(declared, "");
    OA_CHECK(blank.verdict == mutators::Verdict::incompatible);
    OA_CHECK(blank.reason == "incompatible with this mod");
}

void test_duplicate_list_is_failed() {
    const mutators::DryRunJob job = job_of({"ridge-dry-bot", "ridge-dry-bot"});
    const std::atomic<bool> cancel{false};
    const mutators::VerdictRecord ran = mutators::run_dry_run(job, cancel);
    const mutators::VerdictRecord direct = direct_verdict(job);
    OA_CHECK(ran == direct);
    OA_CHECK(ran.verdict == mutators::Verdict::failed);
    OA_CHECK(!ran.reason.empty());
}

} // namespace

int main() {
    test_fixture_verdicts();
    test_threads_and_cancel();
    test_mapped_outcomes();
    test_duplicate_list_is_failed();
    return oa::test::check_exit_status();
}
