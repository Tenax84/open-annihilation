// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The combination algebra: stacked factors, meta values, clamps, text,
// needs on fields a record leaves out, and the folders a target names.
// Expected numbers are written out; this file does not compute them.

#include "oa/data/defs/layout.hpp"
#include "oa/data/mutation_rules/apply.hpp"
#include "oa/formats/soft_mutator.hpp"
#include "oa/formats/tdf.hpp"
#include "oa/formats/tdf/edit.hpp"
#include "oa/test/check.hpp"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace rules = oa::data::mutation_rules;
namespace tdf = oa::formats::tdf;
namespace sm = oa::formats::soft_mutator;
namespace defs = oa::data::defs;

constexpr std::string_view k_ridgecom = R"([UNITINFO]
{
UnitName=RIDGECOM;
Commander=1;
BMcode=1;
Builder=1;
EnergyMake=25;
MetalMake=1;
BuildDistance=60;
WorkerTime=300;
}
)";

constexpr std::string_view k_ridgetank = R"([UNITINFO]
{
UnitName=RIDGETANK;
MaxDamage=15000;
MaxVelocity=2;
}
)";

constexpr std::string_view k_bounty = R"([*Group*]
{
[Needs]
{
Commander=1,1;
BMcode=1,1;
}
[Changes]
{
EnergyMake=^1000;
EnergyStorage=^3000;
MetalMake=^50;
MetalStorage=^1000;
BuildDistance=*1.5;
WorkerTime=*2;
}
}
)";

constexpr std::string_view k_bounty_deluxe = R"([*Group*]
{
[Needs]
{
Commander=1,1;
BMcode=1,1;
}
[Changes]
{
EnergyMake=^4000;
EnergyStorage=^12000;
MetalMake=^200;
MetalStorage=^4000;
BuildDistance=*2;
WorkerTime=*3;
}
}
)";

constexpr std::string_view k_swift_2 = R"([*Group*]
{
[Needs]
{
Builder=1,1;
WorkerTime=1,1000000;
}
[Changes]
{
WorkerTime=*2;
}
}
)";

constexpr std::string_view k_swift_4 = R"([*Group*]
{
[Needs]
{
Builder=1,1;
WorkerTime=1,1000000;
}
[Changes]
{
WorkerTime=*4;
}
}
)";

/// Restores the process layout when the test returns.
class LayoutGuard {
  public:

    LayoutGuard() : saved_(defs::data_layout()) {}

    ~LayoutGuard() { defs::use_data_layout(saved_); }

    LayoutGuard(const LayoutGuard&) = delete;

    LayoutGuard& operator=(const LayoutGuard&) = delete;

  private:

    defs::DataLayout saved_;
};

sm::ParseResult parse_sm(std::string_view text) {
    return sm::parse_rules(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size())
    );
}

rules::MutatorRules
rules_named(std::string mutator, std::string source_name, std::string_view text) {
    rules::MutatorRules parsed;
    parsed.mutator = std::move(mutator);
    rules::RuleSource source;
    source.name = std::move(source_name);
    sm::ParseResult result = parse_sm(text);
    OA_CHECK(!result.has_errors());
    source.rules = std::move(result.rules);
    parsed.sources.push_back(std::move(source));
    return parsed;
}

rules::MutatorRules rules_of(std::string mutator, std::string_view text) {
    return rules_named(std::move(mutator), "rules/legacy.sm", text);
}

bool add_text(rules::DocumentSet& docs, std::string path, std::string_view text) {
    std::string error;
    const bool added = docs.add(std::move(path), text, &error);
    OA_CHECK(added);
    OA_CHECK(error.empty());
    return added;
}

const tdf::Block* unit_section(rules::DocumentSet& docs, std::string_view path) {
    rules::LoadedDocument* loaded = docs.find(path);
    if (loaded == nullptr)
        return nullptr;
    return tdf::find_child(loaded->document.root(), "UNITINFO");
}

const char* unit_value(rules::DocumentSet& docs, std::string_view path, std::string_view key) {
    const tdf::Block* section = unit_section(docs, path);
    if (section == nullptr)
        return nullptr;
    const std::string key_text(key);
    return tdf::find_value(section, key_text.c_str());
}

const char* section_value(
    rules::DocumentSet& docs,
    std::string_view path,
    std::string_view section_name,
    std::string_view key
) {
    rules::LoadedDocument* loaded = docs.find(path);
    if (loaded == nullptr)
        return nullptr;
    const std::string section_text(section_name);
    const tdf::Block* section = tdf::find_child(loaded->document.root(), section_text.c_str());
    if (section == nullptr)
        return nullptr;
    const std::string key_text(key);
    return tdf::find_value(section, key_text.c_str());
}

const char* child_value(
    rules::DocumentSet& docs,
    std::string_view path,
    std::string_view section_name,
    std::string_view child_name,
    std::string_view key
) {
    rules::LoadedDocument* loaded = docs.find(path);
    if (loaded == nullptr)
        return nullptr;
    const std::string section_text(section_name);
    const tdf::Block* section = tdf::find_child(loaded->document.root(), section_text.c_str());
    if (section == nullptr)
        return nullptr;
    const std::string child_text(child_name);
    const tdf::Block* inner = tdf::find_child(section, child_text.c_str());
    if (inner == nullptr)
        return nullptr;
    const std::string key_text(key);
    return tdf::find_value(inner, key_text.c_str());
}

void expect_unit(
    rules::DocumentSet& docs, std::string_view path, std::string_view key, std::string_view want
) {
    const char* got = unit_value(docs, path, key);
    OA_CHECK(got != nullptr && std::string_view(got) == want);
}

void expect_absent_unit(rules::DocumentSet& docs, std::string_view path, std::string_view key) {
    OA_CHECK(unit_value(docs, path, key) == nullptr);
}

bool edited(rules::DocumentSet& docs, std::string_view path) {
    rules::LoadedDocument* loaded = docs.find(path);
    if (loaded == nullptr)
        return false;
    return tdf::document_edited(loaded->document.get());
}

const rules::FieldWrite* find_write(
    const rules::ApplyResult& result,
    std::string_view path,
    std::string_view block,
    std::string_view key
) {
    for (const rules::FieldWrite& write : result.writes) {
        if (write.path == path && write.block == block && write.key == key)
            return &write;
    }
    return nullptr;
}

const rules::RuleNote* find_note(const rules::ApplyResult& result, rules::NoteKind kind) {
    for (const rules::RuleNote& note : result.notes) {
        if (note.kind == kind)
            return &note;
    }
    return nullptr;
}

const rules::RuleNote*
note_on(const rules::ApplyResult& result, rules::NoteKind kind, std::string_view key) {
    for (const rules::RuleNote& note : result.notes) {
        if (note.kind == kind && note.key == key)
            return &note;
    }
    return nullptr;
}

int count_kind(const rules::ApplyResult& result, rules::NoteKind kind) {
    int found = 0;
    for (const rules::RuleNote& note : result.notes) {
        if (note.kind == kind)
            ++found;
    }
    return found;
}

bool mutators_are(const std::vector<std::string>& got, std::initializer_list<const char*> want) {
    if (got.size() != want.size())
        return false;
    size_t at = 0;
    for (const char* one : want) {
        if (got[at] != one)
            return false;
        ++at;
    }
    return true;
}

void document_set() {
    rules::DocumentSet docs;
    std::string error;
    OA_CHECK(add_text(docs, "units/RIDGEFAB.fbi", "[UNITINFO]\n{\nUnitName=RIDGEFAB;\n}\n"));
    OA_CHECK(!docs.add("Units\\RIDGEFAB.FBI", "[UNITINFO]\n{\nUnitName=OTHER;\n}\n", &error));
    OA_CHECK(!error.empty());
    OA_CHECK(docs.size() == 1);
    error.clear();
    OA_CHECK(!docs.add("units/BROKEN.fbi", "[UNITINFO", &error));
    OA_CHECK(!error.empty());
    OA_CHECK(docs.size() == 1);
    OA_CHECK(add_text(docs, "features/corpses/a.tdf", "[RIDGE_WRECK]\n{\nmetal=1;\n}\n"));
    OA_CHECK(add_text(docs, "features/corpses/deep/b.tdf", "[DEEP]\n{\nmetal=1;\n}\n"));
    OA_CHECK(add_text(docs, "features/rocks/c.tdf", "[ROCK]\n{\nmetal=1;\n}\n"));
    const std::vector<size_t> corpses = docs.documents_in("features\\corpses", "tdf", false);
    OA_CHECK(corpses.size() == 1);
    const std::vector<size_t> deep = docs.documents_in("features/corpses", "TDF", true);
    OA_CHECK(deep.size() == 2);
    OA_CHECK(!corpses.empty());
    if (!corpses.empty())
        docs.at(corpses[0]).removed = true;
    OA_CHECK(docs.documents_in("features/corpses", "tdf", false).empty());
    OA_CHECK(docs.find("FEATURES/CORPSES/A.TDF") != nullptr);
    OA_CHECK(!docs.changed(0));
    docs.at(0).added = true;
    OA_CHECK(docs.changed(0));
    OA_CHECK(!docs.changed(99));
}

void targets() {
    std::vector<rules::MutatorRules> list;
    list.push_back(rules_of("one", "[*All*]\n{\nWorkerTime=*2;\n}\n"));
    list.push_back(rules_of("two", "folder=weapons;\nextension=tdf;\n[*All*]\n{\nrange=*2;\n}\n"));
    list.push_back(rules_of("three", "[RIDGECOM]\n{\nWorkerTime=*2;\n}\n"));
    const std::vector<sm::Target> found = rules::targets_of(list);
    OA_CHECK(found.size() == 2);
    if (found.size() != 2)
        return;
    OA_CHECK(found[0].folder == "units" && found[0].extension == "fbi");
    OA_CHECK(found[1].folder == "weapons" && found[1].extension == "tdf");
}

void stacking_factors() {
    auto run = [](bool swapped) {
        rules::DocumentSet docs;
        OA_CHECK(add_text(
            docs,
            "units/RIDGEFAB.fbi",
            "[UNITINFO]\n{\nUnitName=RIDGEFAB;\nBuilder=1;\nWorkerTime=30;\n}\n"
        ));
        OA_CHECK(add_text(
            docs, "units/RIDGEIDLE.fbi", "[UNITINFO]\n{\nUnitName=RIDGEIDLE;\nWorkerTime=10;\n}\n"
        ));
        std::vector<rules::MutatorRules> list;
        if (!swapped) {
            list.push_back(rules_of("swift-hands-2x", k_swift_2));
            list.push_back(rules_of("swift-hands-4x", k_swift_4));
        } else {
            list.push_back(rules_of("swift-hands-4x", k_swift_4));
            list.push_back(rules_of("swift-hands-2x", k_swift_2));
        }
        const rules::ApplyResult result = rules::apply_rules(list, docs, {});
        expect_unit(docs, "units/RIDGEFAB.fbi", "WorkerTime", "240");
        expect_unit(docs, "units/RIDGEIDLE.fbi", "WorkerTime", "10");
        OA_CHECK(edited(docs, "units/RIDGEFAB.fbi"));
        OA_CHECK(!edited(docs, "units/RIDGEIDLE.fbi"));
        OA_CHECK(result.writes.size() == 1);
        const rules::FieldWrite* write = find_write(result, "units/RIDGEFAB.fbi", "", "WorkerTime");
        OA_CHECK(write != nullptr);
        if (write == nullptr)
            return;
        OA_CHECK(write->record == "RIDGEFAB");
        OA_CHECK(write->old_text.has_value() && *write->old_text == "30");
        OA_CHECK(write->new_text.has_value() && *write->new_text == "240");
        if (!swapped)
            OA_CHECK(mutators_are(write->mutators, {"swift-hands-2x", "swift-hands-4x"}));
        else
            OA_CHECK(mutators_are(write->mutators, {"swift-hands-4x", "swift-hands-2x"}));
    };
    run(false);
    run(true);
}

void expect_bounty(rules::DocumentSet& docs, const rules::ApplyResult& result, bool with_swift) {
    expect_unit(docs, "units/RIDGECOM.fbi", "EnergyMake", "2500");
    expect_unit(docs, "units/RIDGECOM.fbi", "EnergyStorage", "7500");
    expect_unit(docs, "units/RIDGECOM.fbi", "MetalMake", "125");
    expect_unit(docs, "units/RIDGECOM.fbi", "MetalStorage", "2500");
    expect_unit(docs, "units/RIDGECOM.fbi", "BuildDistance", "180");
    expect_unit(docs, "units/RIDGECOM.fbi", "WorkerTime", with_swift ? "14400" : "1800");
    const rules::FieldWrite* energy = find_write(result, "units/RIDGECOM.fbi", "", "EnergyMake");
    const rules::FieldWrite* store = find_write(result, "units/RIDGECOM.fbi", "", "EnergyStorage");
    const rules::FieldWrite* metal = find_write(result, "units/RIDGECOM.fbi", "", "MetalMake");
    const rules::FieldWrite* metal_store =
        find_write(result, "units/RIDGECOM.fbi", "", "MetalStorage");
    const rules::FieldWrite* distance =
        find_write(result, "units/RIDGECOM.fbi", "", "BuildDistance");
    const rules::FieldWrite* worker = find_write(result, "units/RIDGECOM.fbi", "", "WorkerTime");
    OA_CHECK(energy != nullptr && store != nullptr && metal != nullptr && metal_store != nullptr);
    OA_CHECK(distance != nullptr && worker != nullptr);
    if (energy == nullptr || store == nullptr || worker == nullptr)
        return;
    OA_CHECK(mutators_are(energy->mutators, {"command-bounty", "command-bounty-deluxe"}));
    OA_CHECK(!store->old_text.has_value());
    OA_CHECK(store->new_text.has_value() && *store->new_text == "7500");
    OA_CHECK(metal_store != nullptr && !metal_store->old_text.has_value());
    OA_CHECK(
        metal_store != nullptr && metal_store->new_text.has_value() &&
        *metal_store->new_text == "2500"
    );
    if (with_swift) {
        OA_CHECK(mutators_are(
            worker->mutators,
            {"command-bounty", "command-bounty-deluxe", "swift-hands-2x", "swift-hands-4x"}
        ));
    } else {
        OA_CHECK(mutators_are(worker->mutators, {"command-bounty", "command-bounty-deluxe"}));
    }
}

void command_bounty() {
    {
        rules::DocumentSet docs;
        OA_CHECK(add_text(docs, "units/RIDGECOM.fbi", k_ridgecom));
        std::vector<rules::MutatorRules> list;
        list.push_back(rules_of("command-bounty", k_bounty));
        list.push_back(rules_of("command-bounty-deluxe", k_bounty_deluxe));
        list.push_back(rules_of("plain-income", "[*All*]\n{\nEnergyMake=5;\n}\n"));
        const rules::ApplyResult result = rules::apply_rules(list, docs, {});
        expect_bounty(docs, result, false);
        OA_CHECK(result.writes.size() == 6);
    }
    {
        rules::DocumentSet docs;
        OA_CHECK(add_text(docs, "units/RIDGECOM.fbi", k_ridgecom));
        std::vector<rules::MutatorRules> list;
        list.push_back(rules_of("command-bounty-deluxe", k_bounty_deluxe));
        list.push_back(rules_of("command-bounty", k_bounty));
        list.push_back(rules_of("plain-income", "[*All*]\n{\nEnergyMake=*10;\n}\n"));
        const rules::ApplyResult result = rules::apply_rules(list, docs, {});
        expect_unit(docs, "units/RIDGECOM.fbi", "EnergyMake", "2500");
        expect_unit(docs, "units/RIDGECOM.fbi", "WorkerTime", "1800");
        const rules::FieldWrite* energy =
            find_write(result, "units/RIDGECOM.fbi", "", "EnergyMake");
        OA_CHECK(energy != nullptr);
        if (energy != nullptr)
            OA_CHECK(mutators_are(energy->mutators, {"command-bounty-deluxe", "command-bounty"}));
    }
    {
        rules::DocumentSet docs;
        OA_CHECK(add_text(docs, "units/RIDGECOM.fbi", k_ridgecom));
        std::vector<rules::MutatorRules> list;
        list.push_back(rules_of("command-bounty", k_bounty));
        list.push_back(rules_of("command-bounty-deluxe", k_bounty_deluxe));
        list.push_back(rules_of("swift-hands-2x", k_swift_2));
        list.push_back(rules_of("swift-hands-4x", k_swift_4));
        const rules::ApplyResult result = rules::apply_rules(list, docs, {});
        expect_bounty(docs, result, true);
    }
}

void widened_clamp() {
    auto run = [](bool swapped) {
        rules::DocumentSet docs;
        OA_CHECK(add_text(docs, "units/RIDGETANK.fbi", k_ridgetank));
        rules::MutatorRules skin = rules_of("iron-skin", "[*All*]\n{\nMaxDamage=*2(1,29900);\n}\n");
        rules::MutatorRules arms =
            rules_of("long-arms", "[*All*]\n{\nMaxDamage=*1.5(1,60000);\n}\n");
        std::vector<rules::MutatorRules> list;
        if (!swapped) {
            list.push_back(std::move(skin));
            list.push_back(std::move(arms));
        } else {
            list.push_back(std::move(arms));
            list.push_back(std::move(skin));
        }
        const rules::ApplyResult result = rules::apply_rules(list, docs, {});
        expect_unit(docs, "units/RIDGETANK.fbi", "MaxDamage", "45000");
        OA_CHECK(count_kind(result, rules::NoteKind::clamp_widened) == 1);
        const rules::RuleNote* note = find_note(result, rules::NoteKind::clamp_widened);
        OA_CHECK(note != nullptr);
        if (note != nullptr) {
            OA_CHECK(mutators_are(note->mutators, {"iron-skin"}));
            OA_CHECK(note->message.find("iron-skin") != std::string::npos);
            OA_CHECK(note->message.find("45000") != std::string::npos);
            OA_CHECK(note->message.find("29900") != std::string::npos);
            OA_CHECK(note->message.find("60000") != std::string::npos);
        }
        const rules::FieldWrite* write = find_write(result, "units/RIDGETANK.fbi", "", "MaxDamage");
        OA_CHECK(write != nullptr && write->new_text.has_value() && *write->new_text == "45000");
    };
    run(false);
    run(true);
    {
        rules::DocumentSet docs;
        OA_CHECK(add_text(docs, "units/RIDGETANK.fbi", k_ridgetank));
        std::vector<rules::MutatorRules> list;
        list.push_back(rules_of("iron-skin", "[*All*]\n{\nMaxDamage=*2(1,29900);\n}\n"));
        const rules::ApplyResult result = rules::apply_rules(list, docs, {});
        expect_unit(docs, "units/RIDGETANK.fbi", "MaxDamage", "29900");
        OA_CHECK(count_kind(result, rules::NoteKind::clamp_widened) == 0);
    }
}

void text_rules() {
    {
        rules::DocumentSet docs;
        OA_CHECK(add_text(
            docs, "units/RIDGEFAB.fbi", "[UNITINFO]\n{\nUnitName=RIDGEFAB;\nMaxVelocity=1.6;\n}\n"
        ));
        std::vector<rules::MutatorRules> list;
        list.push_back(rules_of("stretch", "[*All*]\n{\nMaxVelocity=*1.5;\n}\n"));
        static_cast<void>(rules::apply_rules(list, docs, {}));
        expect_unit(docs, "units/RIDGEFAB.fbi", "MaxVelocity", "2.4");
        const tdf::Block* section = unit_section(docs, "units/RIDGEFAB.fbi");
        OA_CHECK(section != nullptr);
        if (section != nullptr)
            OA_CHECK(tdf::get_fixed(section, "MaxVelocity", 0) == 157286);
    }
    auto average = [](bool swapped) {
        rules::DocumentSet docs;
        OA_CHECK(add_text(
            docs, "units/RIDGEFAB.fbi", "[UNITINFO]\n{\nUnitName=RIDGEFAB;\nSightDistance=1;\n}\n"
        ));
        std::vector<rules::MutatorRules> list;
        if (!swapped) {
            list.push_back(rules_of("low", "[*All*]\n{\nSightDistance=10;\n}\n"));
            list.push_back(rules_of("high", "[*All*]\n{\nSightDistance=20;\n}\n"));
        } else {
            list.push_back(rules_of("high", "[*All*]\n{\nSightDistance=20;\n}\n"));
            list.push_back(rules_of("low", "[*All*]\n{\nSightDistance=10;\n}\n"));
        }
        const rules::ApplyResult result = rules::apply_rules(list, docs, {});
        expect_unit(docs, "units/RIDGEFAB.fbi", "SightDistance", "15");
        const rules::FieldWrite* write =
            find_write(result, "units/RIDGEFAB.fbi", "", "SightDistance");
        OA_CHECK(write != nullptr);
        if (write == nullptr)
            return;
        if (!swapped)
            OA_CHECK(mutators_are(write->mutators, {"low", "high"}));
        else
            OA_CHECK(mutators_are(write->mutators, {"high", "low"}));
    };
    average(false);
    average(true);
    {
        rules::DocumentSet docs;
        OA_CHECK(add_text(
            docs, "units/RIDGETANK.fbi", "[UNITINFO]\n{\nUnitName=RIDGETANK;\nWeapon1=NONE;\n}\n"
        ));
        std::vector<rules::MutatorRules> list;
        list.push_back(rules_of("armoury", "[*All*]\n{\nWeapon1=\"A\";\n}\n"));
        static_cast<void>(rules::apply_rules(list, docs, {}));
        expect_unit(docs, "units/RIDGETANK.fbi", "Weapon1", "A");
    }
    {
        rules::DocumentSet docs;
        OA_CHECK(add_text(
            docs, "units/RIDGETANK.fbi", "[UNITINFO]\n{\nUnitName=RIDGETANK;\nWeapon1=NONE;\n}\n"
        ));
        std::vector<rules::MutatorRules> list;
        list.push_back(rules_of("armoury", "[*All*]\n{\nWeapon1=\"A\";\n}\n"));
        list.push_back(rules_of("other-armoury", "[*All*]\n{\nWeapon1=\"B\";\n}\n"));
        const rules::ApplyResult result = rules::apply_rules(list, docs, {});
        expect_unit(docs, "units/RIDGETANK.fbi", "Weapon1", "NONE");
        OA_CHECK(result.writes.empty());
        OA_CHECK(!edited(docs, "units/RIDGETANK.fbi"));
        OA_CHECK(count_kind(result, rules::NoteKind::text_assignments_cancel) == 1);
    }
    {
        rules::DocumentSet docs;
        OA_CHECK(add_text(
            docs, "units/RIDGETANK.fbi", "[UNITINFO]\n{\nUnitName=RIDGETANK;\nCopyright=Ridge;\n}\n"
        ));
        std::vector<rules::MutatorRules> list;
        list.push_back(rules_of("one-clear", "[*All*]\n{\nCopyright=\"\";\n}\n"));
        list.push_back(rules_of("other-clear", "[*All*]\n{\nCopyright=\"\";\n}\n"));
        const rules::ApplyResult result = rules::apply_rules(list, docs, {});
        expect_unit(docs, "units/RIDGETANK.fbi", "Copyright", "Ridge");
        OA_CHECK(result.writes.empty());
        OA_CHECK(!edited(docs, "units/RIDGETANK.fbi"));
        OA_CHECK(count_kind(result, rules::NoteKind::text_assignments_cancel) == 1);
    }
    {
        rules::DocumentSet docs;
        OA_CHECK(add_text(
            docs, "units/RIDGETANK.fbi", "[UNITINFO]\n{\nUnitName=RIDGETANK;\nCopyright=Ridge;\n}\n"
        ));
        std::vector<rules::MutatorRules> list;
        list.push_back(rules_of("one-clear", "[*All*]\n{\nCopyright=\"\";\n}\n"));
        const rules::ApplyResult result = rules::apply_rules(list, docs, {});
        expect_absent_unit(docs, "units/RIDGETANK.fbi", "Copyright");
        const rules::FieldWrite* write = find_write(result, "units/RIDGETANK.fbi", "", "Copyright");
        OA_CHECK(write != nullptr && !write->new_text.has_value());
        OA_CHECK(write != nullptr && write->old_text.has_value() && *write->old_text == "Ridge");
    }
    auto append = [](bool swapped, std::string_view want) {
        rules::DocumentSet docs;
        OA_CHECK(add_text(
            docs, "units/RIDGETANK.fbi", "[UNITINFO]\n{\nUnitName=RIDGETANK;\nDescription=R;\n}\n"
        ));
        rules::MutatorRules idge = rules_of("idge", "[*All*]\n{\nDescription=\"idge\"+;\n}\n");
        rules::MutatorRules com = rules_of("com", "[*All*]\n{\nDescription=\"com\"+;\n}\n");
        std::vector<rules::MutatorRules> list;
        if (!swapped) {
            list.push_back(std::move(idge));
            list.push_back(std::move(com));
        } else {
            list.push_back(std::move(com));
            list.push_back(std::move(idge));
        }
        const rules::ApplyResult result = rules::apply_rules(list, docs, {});
        expect_unit(docs, "units/RIDGETANK.fbi", "Description", want);
        const rules::FieldWrite* write =
            find_write(result, "units/RIDGETANK.fbi", "", "Description");
        OA_CHECK(write != nullptr);
        if (write == nullptr)
            return;
        if (!swapped)
            OA_CHECK(mutators_are(write->mutators, {"idge", "com"}));
        else
            OA_CHECK(mutators_are(write->mutators, {"com", "idge"}));
    };
    append(false, "Ridgecom");
    append(true, "Rcomidge");
    {
        rules::DocumentSet docs;
        OA_CHECK(add_text(
            docs, "units/RIDGETANK.fbi", "[UNITINFO]\n{\nUnitName=RIDGETANK;\nWeapon1=Q;\n}\n"
        ));
        std::vector<rules::MutatorRules> list;
        list.push_back(rules_of("assigner", "[*All*]\n{\nWeapon1=\"A\";\n}\n"));
        list.push_back(rules_of("appender", "[*All*]\n{\nWeapon1=\"Z\"+;\n}\n"));
        const rules::ApplyResult result = rules::apply_rules(list, docs, {});
        expect_unit(docs, "units/RIDGETANK.fbi", "Weapon1", "QZ");
        OA_CHECK(count_kind(result, rules::NoteKind::text_assignment_lost_to_append) == 1);
        const rules::FieldWrite* write = find_write(result, "units/RIDGETANK.fbi", "", "Weapon1");
        OA_CHECK(write != nullptr && mutators_are(write->mutators, {"appender"}));
    }
    {
        rules::DocumentSet docs;
        OA_CHECK(add_text(
            docs, "units/RIDGETANK.fbi", "[UNITINFO]\n{\nUnitName=RIDGETANK;\nMaxDamage=100;\n}\n"
        ));
        std::vector<rules::MutatorRules> list;
        list.push_back(rules_of("scaler", "[*All*]\n{\nMaxDamage=*2;\n}\n"));
        list.push_back(rules_of("quoter", "[*All*]\n{\nMaxDamage=\"850\";\n}\n"));
        const rules::ApplyResult result = rules::apply_rules(list, docs, {});
        expect_unit(docs, "units/RIDGETANK.fbi", "MaxDamage", "200");
        OA_CHECK(count_kind(result, rules::NoteKind::text_cancelled_by_number) == 1);
        const rules::FieldWrite* write = find_write(result, "units/RIDGETANK.fbi", "", "MaxDamage");
        OA_CHECK(write != nullptr && mutators_are(write->mutators, {"scaler"}));
    }
    {
        rules::DocumentSet docs;
        OA_CHECK(add_text(
            docs, "units/RIDGECOM.fbi", "[UNITINFO]\n{\nUnitName=RIDGECOM;\nEnergyMake=25;\n}\n"
        ));
        std::vector<rules::MutatorRules> list;
        list.push_back(rules_of("meta", "[*All*]\n{\nEnergyMake=^1000;\n}\n"));
        list.push_back(rules_of("quoter", "[*All*]\n{\nEnergyMake=\"nope\";\n}\n"));
        const rules::ApplyResult result = rules::apply_rules(list, docs, {});
        expect_unit(docs, "units/RIDGECOM.fbi", "EnergyMake", "1000");
        OA_CHECK(count_kind(result, rules::NoteKind::text_cancelled_by_number) == 1);
        const rules::FieldWrite* write = find_write(result, "units/RIDGECOM.fbi", "", "EnergyMake");
        OA_CHECK(write != nullptr && mutators_are(write->mutators, {"meta"}));
    }
    {
        rules::DocumentSet docs;
        OA_CHECK(add_text(
            docs, "units/RIDGETANK.fbi", "[UNITINFO]\n{\nUnitName=RIDGETANK;\nMaxDamage=100;\n}\n"
        ));
        std::vector<rules::MutatorRules> list;
        list.push_back(rules_of("quoter", "[*All*]\n{\nMaxDamage=\"850\";\n}\n"));
        static_cast<void>(rules::apply_rules(list, docs, {}));
        expect_unit(docs, "units/RIDGETANK.fbi", "MaxDamage", "850");
    }
}

void within_mutator() {
    rules::DocumentSet docs;
    OA_CHECK(add_text(
        docs,
        "units/RIDGESLOW.fbi",
        "[UNITINFO]\n{\nUnitName=RIDGESLOW;\nDescription=slow;\nWorkerTime=30;\n"
        "BuildDistance=80;\nCloakCost=3;\nEnergyMake=25;\nSightDistance=100;\n}\n"
    ));
    OA_CHECK(add_text(
        docs,
        "units/RIDGEFAST.fbi",
        "[UNITINFO]\n{\nUnitName=RIDGEFAST;\nDescription=fast;\nWorkerTime=30;\n"
        "BuildDistance=80;\nCloakCost=3;\nEnergyMake=25;\nSightDistance=100;\n}\n"
    ));
    std::vector<rules::MutatorRules> list;
    list.push_back(rules_of(
        "compound",
        "[*All*]\n{\nSightDistance=*1.2;\nWorkerTime=5;\nWorkerTime=*2;\n"
        "BuildDistance=*2;\nBuildDistance=5;\nCloakCost=4;\nEnergyMake=^7;\nEnergyMake=*2;\n}\n"
        "[*All*]\n{\nCloakCost=9;\n}\n"
        "[*Group*]\n{\n[Needs]\n{\nDescription=\"fast\";\n}\n[Changes]\n{\nSightDistance=*0.83333;"
        "\n}\n}\n"
    ));
    static_cast<void>(rules::apply_rules(list, docs, {}));
    expect_unit(docs, "units/RIDGESLOW.fbi", "WorkerTime", "10");
    expect_unit(docs, "units/RIDGEFAST.fbi", "WorkerTime", "10");
    expect_unit(docs, "units/RIDGESLOW.fbi", "BuildDistance", "5");
    expect_unit(docs, "units/RIDGEFAST.fbi", "BuildDistance", "5");
    expect_unit(docs, "units/RIDGESLOW.fbi", "CloakCost", "9");
    expect_unit(docs, "units/RIDGEFAST.fbi", "CloakCost", "9");
    expect_unit(docs, "units/RIDGESLOW.fbi", "EnergyMake", "7");
    expect_unit(docs, "units/RIDGEFAST.fbi", "EnergyMake", "7");
    expect_unit(docs, "units/RIDGESLOW.fbi", "SightDistance", "120");
    expect_unit(docs, "units/RIDGEFAST.fbi", "SightDistance", "99.9996");
}

void needs_before_writes() {
    rules::DocumentSet docs;
    OA_CHECK(add_text(
        docs, "units/RIDGETANK.fbi", "[UNITINFO]\n{\nUnitName=RIDGETANK;\nSightDistance=10;\n}\n"
    ));
    std::vector<rules::MutatorRules> list;
    list.push_back(rules_of("lifter", "[*All*]\n{\nCanFly=1;\n}\n"));
    list.push_back(rules_of(
        "spotter",
        "[*Group*]\n{\n[Needs]\n{\nCanFly=1,1;\n}\n[Changes]\n{\nSightDistance=50;\n}\n}\n"
    ));
    const rules::ApplyResult result = rules::apply_rules(list, docs, {});
    expect_unit(docs, "units/RIDGETANK.fbi", "CanFly", "1");
    expect_unit(docs, "units/RIDGETANK.fbi", "SightDistance", "10");
    OA_CHECK(find_write(result, "units/RIDGETANK.fbi", "", "SightDistance") == nullptr);
    OA_CHECK(count_kind(result, rules::NoteKind::matched_nothing) == 1);
}

void fields_left_out() {
    rules::DocumentSet docs;
    OA_CHECK(add_text(docs, "units/RIDGETANK.fbi", k_ridgetank));
    std::vector<rules::MutatorRules> list;
    list.push_back(rules_of(
        "defaults",
        "[*All*]\n{\nEnergyStorage=9;\nDamageModifier=*2;\n}\n"
        "[*Group*]\n{\n[Needs]\n{\nCanFly=1,1;\n}\n[Changes]\n{\nSightDistance=11;\n}\n}\n"
        "[*Group*]\n{\n[Needs]\n{\nCanFly=0,0;\n}\n[Changes]\n{\nRadarDistance=22;\n}\n}\n"
        "[*Group*]\n{\n[Needs]\n{\nCanFly=!1,1;\n}\n[Changes]\n{\nSonarDistance=33;\n}\n}\n"
        "[*Group*]\n{\n[Needs]\n{\nEnergyUse=-99999999999,0;\n}\n[Changes]\n{\nEnergyUse=0;\n}\n}\n"
        "[*Group*]\n{\n[Needs]\n{\nEnergyUse=-99999999999,-1;\n}\n[Changes]\n{\nEnergyUse=5;\n}\n}"
        "\n"
        "[*Group*]\n{\n[Needs]\n{\nBuildCostMetal=0,99;\n}\n[Changes]\n{\nBuildDistance=44;\n}\n}\n"
        "[*Group*]\n{\n[Needs]\n{\nBankScale=1,1;\n}\n[Changes]\n{\nBrakeRate=55;\n}\n}\n"
        "[*Group*]\n{\n[Needs]\n{\nMoveRate1=4,4;\n}\n[Changes]\n{\nAcceleration=66;\n}\n}\n"
        "[*Group*]\n{\n[Needs]\n{\nStandingFireOrder=2,2;\n}\n[Changes]\n{\nFootprintX=77;\n}\n}\n"
        "[*Group*]\n{\n[Needs]\n{\nwpri_badTargetCategory=\"none\";\n}\n[Changes]\n{\nFootprintZ="
        "88;\n}\n}\n"
        "[*Group*]\n{\n[Needs]\n{\nDescription=\"tank\";\n}\n[Changes]\n{\nCruiseAlt=99;\n}\n}\n"
        "[*Group*]\n{\n[Needs]\n{\nDescription=\"tank\"*;\n}\n[Changes]\n{\nRidgeContains=1;\n}\n}"
        "\n"
        "[*Group*]\n{\n[Needs]\n{\nDescription=!\"tank\";\n}\n[Changes]\n{\nCloakCost=1;\n}\n}\n"
        "[*Group*]\n{\n[Needs]\n{\nDescription=!\"tank\"*;\n}\n[Changes]\n{\nStealth=1;\n}\n}\n"
        "[*Group*]\n{\n[Needs]\n{\nBankScale=\"\"*;\n}\n[Changes]\n{\nHideDamage=1;\n}\n}\n"
        "[*Group*]\n{\n[Needs]\n{\nBankScale=!\"\"*;\n}\n[Changes]\n{\nAmphibious=1;\n}\n}\n"
    ));
    const rules::ApplyResult result = rules::apply_rules(list, docs, {});
    expect_unit(docs, "units/RIDGETANK.fbi", "EnergyStorage", "9");
    expect_absent_unit(docs, "units/RIDGETANK.fbi", "DamageModifier");
    OA_CHECK(note_on(result, rules::NoteKind::factor_on_absent_field, "DamageModifier") != nullptr);
    OA_CHECK(find_write(result, "units/RIDGETANK.fbi", "", "DamageModifier") == nullptr);
    const rules::FieldWrite* created =
        find_write(result, "units/RIDGETANK.fbi", "", "EnergyStorage");
    OA_CHECK(created != nullptr && !created->old_text.has_value());
    OA_CHECK(created != nullptr && created->new_text.has_value() && *created->new_text == "9");
    expect_absent_unit(docs, "units/RIDGETANK.fbi", "SightDistance");
    expect_unit(docs, "units/RIDGETANK.fbi", "RadarDistance", "22");
    expect_unit(docs, "units/RIDGETANK.fbi", "SonarDistance", "33");
    expect_unit(docs, "units/RIDGETANK.fbi", "EnergyUse", "0");
    expect_unit(docs, "units/RIDGETANK.fbi", "BuildDistance", "44");
    expect_unit(docs, "units/RIDGETANK.fbi", "BrakeRate", "55");
    expect_unit(docs, "units/RIDGETANK.fbi", "Acceleration", "66");
    expect_unit(docs, "units/RIDGETANK.fbi", "FootprintX", "77");
    expect_unit(docs, "units/RIDGETANK.fbi", "FootprintZ", "88");
    expect_absent_unit(docs, "units/RIDGETANK.fbi", "CruiseAlt");
    expect_absent_unit(docs, "units/RIDGETANK.fbi", "RidgeContains");
    expect_unit(docs, "units/RIDGETANK.fbi", "CloakCost", "1");
    expect_unit(docs, "units/RIDGETANK.fbi", "Stealth", "1");
    expect_absent_unit(docs, "units/RIDGETANK.fbi", "HideDamage");
    expect_unit(docs, "units/RIDGETANK.fbi", "Amphibious", "1");

    rules::DocumentSet present;
    OA_CHECK(add_text(
        present, "units/RIDGEHAS.fbi", "[UNITINFO]\n{\nUnitName=RIDGEHAS;\nBankScale=3;\n}\n"
    ));
    std::vector<rules::MutatorRules> presence;
    presence.push_back(rules_of(
        "presence",
        "[*Group*]\n{\n[Needs]\n{\nBankScale=\"\"*;\n}\n[Changes]\n{\nHideDamage=1;\n}\n}\n"
        "[*Group*]\n{\n[Needs]\n{\nBankScale=!\"\"*;\n}\n[Changes]\n{\nAmphibious=1;\n}\n}\n"
    ));
    static_cast<void>(rules::apply_rules(presence, present, {}));
    expect_unit(present, "units/RIDGEHAS.fbi", "HideDamage", "1");
    expect_absent_unit(present, "units/RIDGEHAS.fbi", "Amphibious");

    rules::DocumentSet only_factor;
    OA_CHECK(add_text(
        only_factor, "units/RIDGEFAB.fbi", "[UNITINFO]\n{\nUnitName=RIDGEFAB;\nBuilder=1;\n}\n"
    ));
    std::vector<rules::MutatorRules> factor;
    factor.push_back(rules_of("absent-factor", "[*All*]\n{\nWorkerTime=*2;\n}\n"));
    const rules::ApplyResult factor_result = rules::apply_rules(factor, only_factor, {});
    expect_absent_unit(only_factor, "units/RIDGEFAB.fbi", "WorkerTime");
    OA_CHECK(count_kind(factor_result, rules::NoteKind::factor_on_absent_field) == 1);
    OA_CHECK(factor_result.writes.empty());
    OA_CHECK(!edited(only_factor, "units/RIDGEFAB.fbi"));
}

void weapons() {
    rules::DocumentSet docs;
    OA_CHECK(add_text(
        docs,
        "weapons/ridge.tdf",
        "[RIDGE_LASER]\n{\nrange=32000;\nreloadtime=8;\n[DAMAGE]\n{\ndefault=100;\nRIDGECOM=40;\n}"
        "\n}\n"
        "[RIDGE_BOLT]\n{\nreloadtime=4;\n[DAMAGE]\n{\ndefault=80;\nRIDGETANK=20;\n}\n}\n"
    ));
    std::vector<rules::MutatorRules> list;
    list.push_back(rules_of(
        "damage-halver",
        "folder=weapons;\nextension=tdf;\n"
        "[*All*]\n{\n<AllDepth2>=*0.5;\n}\n"
        "[*Group*]\n{\n[Needs]\n{\nrange=!32000,32000;\n}\n[Changes]\n{\nreloadtime=9;\n}\n}\n"
        "[*Group*]\n{\n[Needs]\n{\nrange=32767,32767;\n}\n[Changes]\n{\nburst=3;\n}\n}\n"
        "[*Group*]\n{\n[Needs]\n{\nid=-1,-1;\n}\n[Changes]\n{\naccuracy=7;\n}\n}\n"
    ));
    const rules::ApplyResult result = rules::apply_rules(list, docs, {});
    const char* laser_default =
        child_value(docs, "weapons/ridge.tdf", "RIDGE_LASER", "DAMAGE", "default");
    const char* laser_com =
        child_value(docs, "weapons/ridge.tdf", "RIDGE_LASER", "DAMAGE", "RIDGECOM");
    const char* bolt_default =
        child_value(docs, "weapons/ridge.tdf", "RIDGE_BOLT", "DAMAGE", "default");
    const char* bolt_tank =
        child_value(docs, "weapons/ridge.tdf", "RIDGE_BOLT", "DAMAGE", "RIDGETANK");
    OA_CHECK(laser_default != nullptr && std::string_view(laser_default) == "50");
    OA_CHECK(laser_com != nullptr && std::string_view(laser_com) == "20");
    OA_CHECK(bolt_default != nullptr && std::string_view(bolt_default) == "40");
    OA_CHECK(bolt_tank != nullptr && std::string_view(bolt_tank) == "10");
    OA_CHECK(
        std::string_view(section_value(docs, "weapons/ridge.tdf", "RIDGE_LASER", "range")) ==
        "32000"
    );
    OA_CHECK(
        std::string_view(section_value(docs, "weapons/ridge.tdf", "RIDGE_LASER", "reloadtime")) ==
        "8"
    );
    OA_CHECK(section_value(docs, "weapons/ridge.tdf", "RIDGE_LASER", "burst") == nullptr);
    OA_CHECK(
        std::string_view(section_value(docs, "weapons/ridge.tdf", "RIDGE_BOLT", "reloadtime")) ==
        "9"
    );
    OA_CHECK(
        std::string_view(section_value(docs, "weapons/ridge.tdf", "RIDGE_BOLT", "burst")) == "3"
    );
    OA_CHECK(
        std::string_view(section_value(docs, "weapons/ridge.tdf", "RIDGE_LASER", "accuracy")) == "7"
    );
    OA_CHECK(
        std::string_view(section_value(docs, "weapons/ridge.tdf", "RIDGE_BOLT", "accuracy")) == "7"
    );
    OA_CHECK(find_write(result, "weapons/ridge.tdf", "", "range") == nullptr);
    const rules::FieldWrite* halved = find_write(result, "weapons/ridge.tdf", "DAMAGE", "default");
    OA_CHECK(halved != nullptr && halved->block == "DAMAGE");
    OA_CHECK(result.writes.size() == 8);
}

void features_and_moveinfo() {
    rules::DocumentSet docs;
    OA_CHECK(add_text(docs, "features/corpses/ridge.tdf", "[RIDGE_WRECK]\n{\nmetal=10;\n}\n"));
    OA_CHECK(add_text(docs, "features/rocks/ridge.tdf", "[RIDGE_STONE]\n{\nmetal=10;\n}\n"));
    std::vector<rules::MutatorRules> list;
    list.push_back(rules_of(
        "corpses",
        "folder=features\\corpses;\n"
        "[*All*]\n{\nmetal=7;\n}\n"
        "[*Group*]\n{\n[Needs]\n{\nridge_tag=0,0;\n}\n[Changes]\n{\ndamage=1;\n}\n}\n"
        "[*Group*]\n{\n[Needs]\n{\nridge_tag=1,1;\n}\n[Changes]\n{\ndamage=5;\n}\n}\n"
        "[*Group*]\n{\n[Needs]\n{\nridge_tag=\"\";\n}\n[Changes]\n{\nenergy=1;\n}\n}\n"
        "[*Group*]\n{\n[Needs]\n{\nridge_tag=\"set\";\n}\n[Changes]\n{\nenergy=5;\n}\n}\n"
        "[*Group*]\n{\n[Needs]\n{\nmaxslope=255,255;\n}\n[Changes]\n{\nreclaimtime=9;\n}\n}\n"
        "[*Group*]\n{\n[Needs]\n{\nmaxslope=0,0;\n}\n[Changes]\n{\nresurrect=4;\n}\n}\n"
    ));
    const rules::ApplyResult result = rules::apply_rules(list, docs, {});
    OA_CHECK(
        std::string_view(
            section_value(docs, "features/corpses/ridge.tdf", "RIDGE_WRECK", "metal")
        ) == "7"
    );
    OA_CHECK(
        std::string_view(
            section_value(docs, "features/corpses/ridge.tdf", "RIDGE_WRECK", "damage")
        ) == "1"
    );
    OA_CHECK(
        std::string_view(
            section_value(docs, "features/corpses/ridge.tdf", "RIDGE_WRECK", "energy")
        ) == "1"
    );
    OA_CHECK(
        std::string_view(
            section_value(docs, "features/corpses/ridge.tdf", "RIDGE_WRECK", "resurrect")
        ) == "4"
    );
    OA_CHECK(
        section_value(docs, "features/corpses/ridge.tdf", "RIDGE_WRECK", "reclaimtime") == nullptr
    );
    OA_CHECK(
        std::string_view(section_value(docs, "features/rocks/ridge.tdf", "RIDGE_STONE", "metal")) ==
        "10"
    );
    OA_CHECK(section_value(docs, "features/rocks/ridge.tdf", "RIDGE_STONE", "damage") == nullptr);
    OA_CHECK(!edited(docs, "features/rocks/ridge.tdf"));
    OA_CHECK(edited(docs, "features/corpses/ridge.tdf"));
    OA_CHECK(result.writes.size() == 4);

    rules::DocumentSet move;
    OA_CHECK(add_text(move, "gamedata/moveinfo.tdf", "[RIDGE_MOVE]\n{\nmaxwaterdepth=20;\n}\n"));
    std::vector<rules::MutatorRules> move_rules;
    move_rules.push_back(rules_of(
        "slopes",
        "folder=gamedata;\nextension=tdf;\n"
        "[*Group*]\n{\n[Needs]\n{\nmaxslope=255,255;\n}\n[Changes]\n{\ncrushstrength=1;\n}\n}\n"
    ));
    static_cast<void>(rules::apply_rules(move_rules, move, {}));
    OA_CHECK(
        std::string_view(
            section_value(move, "gamedata/moveinfo.tdf", "RIDGE_MOVE", "crushstrength")
        ) == "1"
    );
}

void sidedata_and_names() {
    rules::DocumentSet sides;
    OA_CHECK(
        add_text(sides, "gamedata/sidedata.tdf", "[SIDE0]\n{\nname=Ridge;\ncommander=OLDCOM;\n}\n")
    );
    std::vector<rules::MutatorRules> side_rules;
    side_rules.push_back(rules_of(
        "side-name",
        "folder=gamedata;\nextension=sidedata.tdf;\n[SIDE0]\n{\ncommander=\"RIDGECOM\";\n}\n"
    ));
    static_cast<void>(rules::apply_rules(side_rules, sides, {}));
    OA_CHECK(
        std::string_view(section_value(sides, "gamedata/sidedata.tdf", "SIDE0", "commander")) ==
        "RIDGECOM"
    );

    rules::DocumentSet docs;
    OA_CHECK(add_text(
        docs, "units/RIDGECOM.fbi", "[UNITINFO]\n{\nUnitName=ridgecom;\nWorkerTime=10;\n}\n"
    ));
    std::vector<rules::MutatorRules> list;
    list.push_back(
        rules_of("by-name", "[RIDGECOM]\n{\nWorkerTime=*3;\n}\n[UNITINFO]\n{\nWorkerTime=99;\n}\n")
    );
    const rules::ApplyResult result = rules::apply_rules(list, docs, {});
    expect_unit(docs, "units/RIDGECOM.fbi", "WorkerTime", "30");
    const rules::FieldWrite* write = find_write(result, "units/RIDGECOM.fbi", "", "WorkerTime");
    OA_CHECK(write != nullptr && write->record == "ridgecom");
    OA_CHECK(count_kind(result, rules::NoteKind::matched_nothing) == 1);
}

void layout_rename() {
    const LayoutGuard guard;
    defs::DataLayout layout = defs::data_layout();
    layout.directories[static_cast<size_t>(defs::DataDirectory::units)] = "machines";
    defs::use_data_layout(layout);
    rules::DocumentSet docs;
    OA_CHECK(add_text(
        docs, "machines/RIDGEFAB.fbi", "[UNITINFO]\n{\nUnitName=RIDGEFAB;\nWorkerTime=30;\n}\n"
    ));
    OA_CHECK(add_text(
        docs, "units/RIDGEFAB.fbi", "[UNITINFO]\n{\nUnitName=RIDGEFAB;\nWorkerTime=30;\n}\n"
    ));
    std::vector<rules::MutatorRules> list;
    list.push_back(rules_of("renamed", "[*All*]\n{\nWorkerTime=*2;\n}\n"));
    static_cast<void>(rules::apply_rules(list, docs, {}));
    expect_unit(docs, "machines/RIDGEFAB.fbi", "WorkerTime", "60");
    expect_unit(docs, "units/RIDGEFAB.fbi", "WorkerTime", "30");
    OA_CHECK(edited(docs, "machines/RIDGEFAB.fbi"));
    OA_CHECK(!edited(docs, "units/RIDGEFAB.fbi"));
}

void schema_kinds() {
    tdf::OwnedDocument doc;
    OA_CHECK(doc.parse("[RIDGE_MOVE]\n{\nmaxwaterdepth=20;\n}\n"));
    const tdf::Block* section = tdf::child_at(doc.root(), 0);
    const sm::ParseResult parsed = parse_sm(
        "[*Group*]\n{\n[Needs]\n{\nmaxslope=255,255;\nrange=32767,32767;\n}\n[Changes]\n{\nkept=1;"
        "\n}\n}\n"
    );
    OA_CHECK(!parsed.has_errors());
    if (parsed.rules.blocks.size() != 1 || parsed.rules.blocks[0].needs.size() < 2)
        return;
    const sm::Need& slope = parsed.rules.blocks[0].needs[0];
    const sm::Need& range = parsed.rules.blocks[0].needs[1];
    const defs::DataLayout& layout = defs::data_layout();
    OA_CHECK(!rules::need_met(section, rules::TargetKind::other, slope));
    OA_CHECK(
        rules::need_met(section, rules::TargetKind::other, "gamedata/moveinfo.tdf", layout, slope)
    );
    OA_CHECK(!rules::need_met(section, rules::TargetKind::units, range));
    OA_CHECK(rules::need_met(section, rules::TargetKind::weapons, range));
    OA_CHECK(
        rules::need_met(section, rules::TargetKind::weapons, "gamedata/moveinfo.tdf", layout, range)
    );
    OA_CHECK(
        !rules::need_met(section, rules::TargetKind::other, "gamedata/moveinfo.tdf", layout, range)
    );
}

void notes() {
    {
        rules::DocumentSet docs;
        OA_CHECK(add_text(
            docs, "units/RIDGETANK.fbi", "[UNITINFO]\n{\nUnitName=RIDGETANK;\nWorkerTime=30;\n}\n"
        ));
        std::vector<rules::MutatorRules> list;
        list.push_back(rules_named(
            "nobody",
            "rules/none.sm",
            "folder=units;\n"
            "[*Group*]\n{\n[Needs]\n{\nBuilder=1,1;\n}\n[Changes]\n{\nWorkerTime=*2;\n}\n}\n"
        ));
        const rules::ApplyResult result = rules::apply_rules(list, docs, {});
        expect_unit(docs, "units/RIDGETANK.fbi", "WorkerTime", "30");
        OA_CHECK(!edited(docs, "units/RIDGETANK.fbi"));
        const rules::RuleNote* note = find_note(result, rules::NoteKind::matched_nothing);
        OA_CHECK(note != nullptr);
        if (note != nullptr) {
            OA_CHECK(note->line == 2);
            OA_CHECK(note->source == "rules/none.sm");
            OA_CHECK(note->path.empty() && note->record.empty() && note->key.empty());
            OA_CHECK(mutators_are(note->mutators, {"nobody"}));
        }
    }
    {
        rules::DocumentSet docs;
        OA_CHECK(add_text(
            docs, "units/RIDGEFAB.fbi", "[UNITINFO]\n{\nUnitName=RIDGEFAB;\nWorkerTime=fast;\n}\n"
        ));
        std::vector<rules::MutatorRules> list;
        list.push_back(rules_of("scaler", "[*All*]\n{\nWorkerTime=*2;\n}\n"));
        const rules::ApplyResult result = rules::apply_rules(list, docs, {});
        expect_unit(docs, "units/RIDGEFAB.fbi", "WorkerTime", "fast");
        OA_CHECK(result.writes.empty());
        OA_CHECK(!edited(docs, "units/RIDGEFAB.fbi"));
        OA_CHECK(count_kind(result, rules::NoteKind::not_a_number) == 1);
    }
    {
        // 1 followed by 308 zeros is 1e308. The rule grammar has no exponent,
        // so each factor is spelled in digits. The two factors overflow.
        std::string digits(309, '0');
        digits[0] = '1';
        const std::string text =
            "[*All*]\n{\nWorkerTime=*" + digits + ";\nWorkerTime=*" + digits + ";\n}\n";
        rules::DocumentSet docs;
        OA_CHECK(add_text(
            docs, "units/RIDGEFAB.fbi", "[UNITINFO]\n{\nUnitName=RIDGEFAB;\nWorkerTime=10;\n}\n"
        ));
        std::vector<rules::MutatorRules> list;
        list.push_back(rules_of("overflow", text));
        const rules::ApplyResult result = rules::apply_rules(list, docs, {});
        expect_unit(docs, "units/RIDGEFAB.fbi", "WorkerTime", "10");
        OA_CHECK(result.writes.empty());
        OA_CHECK(!edited(docs, "units/RIDGEFAB.fbi"));
        OA_CHECK(count_kind(result, rules::NoteKind::not_finite) == 1);
    }
    {
        rules::DocumentSet docs;
        OA_CHECK(add_text(
            docs,
            "units/RIDGETANK.fbi",
            "[UNITINFO]\n{\nUnitName=RIDGETANK;\nMaxDamage=15000;\nWorkerTime=30;\n}\n"
        ));
        rules::ApplyOptions options;
        options.check = [](const rules::FieldRef& field, const std::optional<std::string>&) {
            rules::CheckResult verdict;
            if (field.key == "MaxDamage") {
                verdict.outcome = rules::CheckOutcome::refuse;
                verdict.reason = "too high";
                return verdict;
            }
            if (field.key == "WorkerTime") {
                verdict.outcome = rules::CheckOutcome::warn;
                verdict.reason = "fast builder";
                return verdict;
            }
            verdict.outcome = rules::CheckOutcome::ok;
            return verdict;
        };
        std::vector<rules::MutatorRules> list;
        list.push_back(rules_of("checker", "[*All*]\n{\nMaxDamage=9;\nWorkerTime=9;\n}\n"));
        const rules::ApplyResult result = rules::apply_rules(list, docs, options);
        expect_unit(docs, "units/RIDGETANK.fbi", "MaxDamage", "15000");
        expect_unit(docs, "units/RIDGETANK.fbi", "WorkerTime", "9");
        OA_CHECK(find_write(result, "units/RIDGETANK.fbi", "", "MaxDamage") == nullptr);
        OA_CHECK(find_write(result, "units/RIDGETANK.fbi", "", "WorkerTime") != nullptr);
        const rules::RuleNote* refused = find_note(result, rules::NoteKind::check_refused);
        const rules::RuleNote* warned = find_note(result, rules::NoteKind::check_warned);
        OA_CHECK(
            refused != nullptr && refused->key == "MaxDamage" && refused->message == "too high"
        );
        OA_CHECK(
            warned != nullptr && warned->key == "WorkerTime" && warned->message == "fast builder"
        );
    }
    {
        rules::DocumentSet docs;
        OA_CHECK(add_text(
            docs, "units/RIDGETANK.fbi", "[UNITINFO]\n{\nUnitName=RIDGETANK;\nWeapon1=LASER;\n}\n"
        ));
        std::vector<rules::MutatorRules> list;
        list.push_back(rules_of("broken", "[*All*]\n{\nWeapon1=\"a;b\";\n}\n"));
        const rules::ApplyResult result = rules::apply_rules(list, docs, {});
        expect_unit(docs, "units/RIDGETANK.fbi", "Weapon1", "LASER");
        OA_CHECK(result.writes.empty());
        OA_CHECK(!edited(docs, "units/RIDGETANK.fbi"));
        const rules::RuleNote* refused = find_note(result, rules::NoteKind::check_refused);
        OA_CHECK(refused != nullptr);
        if (refused != nullptr)
            OA_CHECK(refused->message.find("would not read back") != std::string::npos);
    }
}

} // namespace

int main() {
    document_set();
    targets();
    stacking_factors();
    command_bounty();
    widened_clamp();
    text_rules();
    within_mutator();
    needs_before_writes();
    fields_left_out();
    weapons();
    features_and_moveinfo();
    sidedata_and_names();
    layout_rename();
    schema_kinds();
    notes();
    return oa::test::check_exit_status();
}
