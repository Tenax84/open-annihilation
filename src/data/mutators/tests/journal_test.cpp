// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Journal order, entries_of, and the stacking sentences. The notes are
// hand-made: this test does not run a mutator.
#include "oa/data/mutators/journal.hpp"
#include "oa/test/check.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace mutators = oa::data::mutators;
namespace rules = oa::data::mutation_rules;

const std::vector<std::string> k_ids{"ridge-tough", "ridge-tougher"};

mutators::JournalEntry make_entry(
    mutators::Phase phase,
    std::vector<uint16_t> mutators_of_entry,
    std::string path,
    std::string record = {},
    std::string field = {}
) {
    mutators::JournalEntry item;
    item.phase = phase;
    item.mutators = std::move(mutators_of_entry);
    item.path = std::move(path);
    item.record = std::move(record);
    item.field = std::move(field);
    item.collection = mutators::Collection::units;
    item.change = mutators::Change::set;
    return item;
}

void test_order() {
    mutators::Journal journal;
    journal.entries.push_back(make_entry(mutators::Phase::hooks, {1}, "z"));
    journal.entries.push_back(make_entry(mutators::Phase::hooks, {0}, "h"));
    journal.entries.push_back(make_entry(mutators::Phase::rules, {0}, "b"));
    journal.entries.push_back(make_entry(mutators::Phase::rules, {0}, "a", "B"));
    journal.entries.push_back(make_entry(mutators::Phase::rules, {0}, "a", "a"));
    journal.entries.push_back(make_entry(mutators::Phase::rules, {0}, "a", "a", "max"));
    journal.entries.push_back(make_entry(mutators::Phase::rules, {0}, "a", "a", "Max"));
    journal.entries.push_back(make_entry(mutators::Phase::rules, {2}, "a"));
    journal.entries.push_back(make_entry(mutators::Phase::rules, {2, 0}, "c"));
    journal.entries.push_back(make_entry(mutators::Phase::rules, {}, "a"));
    journal.entries.push_back(make_entry(mutators::Phase::files, {0}, "m"));
    journal.order();

    OA_CHECK(journal.entries.size() == 11);
    if (journal.entries.size() != 11)
        return;
    const mutators::JournalEntry* ordered = journal.entries.data();
    OA_CHECK(ordered[0].phase == mutators::Phase::files && ordered[0].path == "m");
    OA_CHECK(ordered[1].record == "a" && ordered[1].field.empty());
    OA_CHECK(ordered[2].field == "Max");
    OA_CHECK(ordered[3].field == "max");
    OA_CHECK(ordered[4].record == "B");
    OA_CHECK(ordered[5].path == "b" && ordered[5].phase == mutators::Phase::rules);
    // One mutator's rules come before its hooks, and that mutator comes
    // before the next mutator's rules.
    OA_CHECK(ordered[6].phase == mutators::Phase::hooks && ordered[6].path == "h");
    OA_CHECK(ordered[7].phase == mutators::Phase::hooks && ordered[7].path == "z");
    OA_CHECK(
        ordered[8].mutators.size() == 1 && ordered[8].mutators[0] == 2 && ordered[8].path == "a"
    );
    OA_CHECK(ordered[9].path == "c" && ordered[9].mutators.size() == 2);
    OA_CHECK(ordered[10].mutators.empty() && ordered[10].phase == mutators::Phase::rules);

    const std::vector<mutators::JournalEntry> of_tough = journal.entries_of(0);
    OA_CHECK(of_tough.size() == 8);
    if (of_tough.size() == 8) {
        OA_CHECK(of_tough[0].path == "m");
        OA_CHECK(of_tough[6].phase == mutators::Phase::hooks && of_tough[6].path == "h");
        OA_CHECK(of_tough[7].path == "c");
    }
    const std::vector<mutators::JournalEntry> of_later = journal.entries_of(2);
    OA_CHECK(of_later.size() == 2);
    OA_CHECK(journal.entries_of(1).size() == 1);
    OA_CHECK(journal.entries_of(3).empty());
}

mutators::Conflict
file_conflict(mutators::FileNoteKind kind, std::string winner, std::string earlier) {
    mutators::FileNote note;
    note.kind = kind;
    note.path = "unitpics/RGBOT.pcx";
    note.mutator = std::move(winner);
    note.other = std::move(earlier);
    return mutators::conflict_from_file(note, k_ids);
}

void test_file_notes() {
    const mutators::Conflict replaced =
        file_conflict(mutators::FileNoteKind::add_replaced, "ridge-tougher", "ridge-tough");
    OA_CHECK(replaced.kind == mutators::ConflictKind::same_path_added);
    OA_CHECK(replaced.severity == mutators::Severity::note);
    OA_CHECK(
        replaced.mutators.size() == 2 && replaced.mutators[0] == 0 && replaced.mutators[1] == 1
    );
    OA_CHECK(
        mutators::describe(replaced, k_ids) ==
        "ridge-tougher wins unitpics/RGBOT.pcx over ridge-tough"
    );

    const mutators::Conflict masked =
        file_conflict(mutators::FileNoteKind::add_masked, "ridge-tougher", "ridge-tough");
    OA_CHECK(masked.kind == mutators::ConflictKind::added_path_hidden);
    OA_CHECK(masked.severity == mutators::Severity::warning);
    OA_CHECK(
        mutators::describe(masked, k_ids) ==
        "ridge-tougher hides unitpics/RGBOT.pcx added by ridge-tough"
    );

    mutators::FileNote missing;
    missing.kind = mutators::FileNoteKind::add_folder_missing;
    missing.path = "files/missing";
    missing.mutator = "ridge-tough";
    const mutators::Conflict folder = mutators::conflict_from_file(missing, k_ids);
    OA_CHECK(folder.kind == mutators::ConflictKind::add_folder_missing);
    OA_CHECK(folder.severity == mutators::Severity::note);
    OA_CHECK(folder.mutators.size() == 1 && folder.mutators[0] == 0);
    OA_CHECK(
        mutators::describe(folder, k_ids) ==
        "ridge-tough adds a folder that is not in the package: files/missing"
    );

    const mutators::Conflict unknown =
        file_conflict(mutators::FileNoteKind::add_replaced, "ridge-tough", "not-listed");
    OA_CHECK(unknown.mutators.size() == 1 && unknown.mutators[0] == 0);
    OA_CHECK(mutators::describe(unknown, k_ids) == "ridge-tough wins unitpics/RGBOT.pcx");
}

rules::RuleNote rule_note(rules::NoteKind kind) {
    rules::RuleNote note;
    note.kind = kind;
    note.mutators = {"ridge-tough", "ridge-tougher"};
    note.path = "units/rgcom.fbi";
    note.record = "RGCOM";
    note.key = "MaxDamage";
    note.message = "kept";
    return note;
}

void test_rule_notes() {
    rules::RuleNote absent = rule_note(rules::NoteKind::factor_on_absent_field);
    absent.message = "a factor did nothing because the field is absent";
    const mutators::Conflict factor = mutators::conflict_from_rule(absent, k_ids);
    OA_CHECK(factor.kind == mutators::ConflictKind::text_change_dropped);
    OA_CHECK(factor.severity == mutators::Severity::note);
    OA_CHECK(factor.field == "maxdamage");
    OA_CHECK(
        mutators::describe(factor, k_ids) ==
        "ridge-tough, ridge-tougher found no value to scale on units/rgcom.fbi maxdamage"
    );

    const mutators::Conflict dropped =
        mutators::conflict_from_rule(rule_note(rules::NoteKind::not_a_number), k_ids);
    OA_CHECK(dropped.kind == mutators::ConflictKind::text_change_dropped);
    OA_CHECK(dropped.severity == mutators::Severity::note);
    OA_CHECK(dropped.field == "maxdamage");
    OA_CHECK(
        mutators::describe(dropped, k_ids) ==
        "ridge-tough, ridge-tougher dropped a text change on units/rgcom.fbi maxdamage"
    );

    const rules::NoteKind refused[] = {rules::NoteKind::not_finite, rules::NoteKind::check_refused};
    for (const rules::NoteKind kind : refused) {
        const mutators::Conflict conflict = mutators::conflict_from_rule(rule_note(kind), k_ids);
        OA_CHECK(conflict.kind == mutators::ConflictKind::write_refused);
        OA_CHECK(conflict.severity == mutators::Severity::note);
        OA_CHECK(
            mutators::describe(conflict, k_ids) ==
            "ridge-tough, ridge-tougher refused a write on units/rgcom.fbi maxdamage: kept"
        );
    }

    // A later mutator replaced the earlier one's value: the earlier comes
    // first and the later, whose value stands, last.
    const mutators::Conflict replaced =
        mutators::conflict_from_rule(rule_note(rules::NoteKind::value_replaced), k_ids);
    OA_CHECK(replaced.kind == mutators::ConflictKind::value_replaced);
    OA_CHECK(replaced.severity == mutators::Severity::note);
    OA_CHECK(replaced.mutators.size() == 2 && replaced.mutators[0] == 0);
    OA_CHECK(replaced.mutators.size() == 2 && replaced.mutators[1] == 1);
    OA_CHECK(replaced.field == "maxdamage");
    OA_CHECK(
        mutators::describe(replaced, k_ids) ==
        "ridge-tougher replaced ridge-tough's value on units/rgcom.fbi maxdamage"
    );

    // An earlier mutator locked the field, so the later one's change was left
    // undone: the locker comes first.
    const mutators::Conflict locked =
        mutators::conflict_from_rule(rule_note(rules::NoteKind::field_locked), k_ids);
    OA_CHECK(locked.kind == mutators::ConflictKind::field_locked);
    OA_CHECK(locked.severity == mutators::Severity::note);
    OA_CHECK(locked.mutators.size() == 2 && locked.mutators[0] == 0);
    OA_CHECK(locked.mutators.size() == 2 && locked.mutators[1] == 1);
    OA_CHECK(locked.field == "maxdamage");
    OA_CHECK(
        mutators::describe(locked, k_ids) ==
        "ridge-tough locked units/rgcom.fbi maxdamage, so ridge-tougher left it alone"
    );

    const mutators::Conflict nothing =
        mutators::conflict_from_rule(rule_note(rules::NoteKind::matched_nothing), k_ids);
    OA_CHECK(nothing.kind == mutators::ConflictKind::matched_nothing);
    OA_CHECK(nothing.severity == mutators::Severity::note);
    OA_CHECK(mutators::describe(nothing, k_ids) == "ridge-tough, ridge-tougher matched nothing");

    const mutators::Conflict warned =
        mutators::conflict_from_rule(rule_note(rules::NoteKind::check_warned), k_ids);
    OA_CHECK(warned.kind == mutators::ConflictKind::value_past_limit);
    OA_CHECK(warned.severity == mutators::Severity::warning);
    OA_CHECK(warned.detail == "kept");
    OA_CHECK(
        mutators::describe(warned, k_ids) ==
        "ridge-tough, ridge-tougher kept units/rgcom.fbi maxdamage past its limit"
    );
}

mutators::HookNote hook_note(mutators::HookNoteKind kind) {
    mutators::HookNote note;
    note.kind = kind;
    note.mutators = {"ridge-tough", "ridge-tougher"};
    note.path = "units/rgcom.fbi";
    note.record = "RGCOM";
    note.key = "MaxDamage";
    note.message = "replaced";
    return note;
}

void test_hook_notes() {
    const mutators::Conflict warned =
        mutators::conflict_from_hook(hook_note(mutators::HookNoteKind::check_warned), k_ids);
    OA_CHECK(warned.kind == mutators::ConflictKind::value_past_limit);
    OA_CHECK(warned.severity == mutators::Severity::warning);
    OA_CHECK(
        mutators::describe(warned, k_ids) ==
        "ridge-tough, ridge-tougher kept units/rgcom.fbi maxdamage past its limit"
    );

    const mutators::Conflict overwrote =
        mutators::conflict_from_hook(hook_note(mutators::HookNoteKind::hook_overwrote), k_ids);
    OA_CHECK(overwrote.kind == mutators::ConflictKind::same_field_hooked);
    OA_CHECK(overwrote.severity == mutators::Severity::note);
    OA_CHECK(overwrote.mutators.size() == 2 && overwrote.mutators[1] == 1);
    OA_CHECK(
        mutators::describe(overwrote, k_ids) ==
        "ridge-tougher wins units/rgcom.fbi maxdamage over ridge-tough"
    );

    const mutators::HookNoteKind dropped[] = {
        mutators::HookNoteKind::not_a_number,
        mutators::HookNoteKind::download_first_entry_changed,
    };
    for (const mutators::HookNoteKind kind : dropped) {
        const mutators::Conflict conflict = mutators::conflict_from_hook(hook_note(kind), k_ids);
        OA_CHECK(conflict.kind == mutators::ConflictKind::text_change_dropped);
        OA_CHECK(conflict.severity == mutators::Severity::note);
        OA_CHECK(conflict.field == "maxdamage");
    }

    mutators::Conflict unnamed;
    unnamed.kind = mutators::ConflictKind::matched_nothing;
    unnamed.mutators.push_back(9);
    OA_CHECK(mutators::describe(unnamed, k_ids) == "a mutator matched nothing");
}

} // namespace

int main() {
    test_order();
    test_file_notes();
    test_rule_notes();
    test_hook_notes();
    return oa::test::check_exit_status();
}
