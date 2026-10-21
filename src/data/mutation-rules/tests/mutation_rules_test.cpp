// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Rules applied as steps. Each mutator reads the documents the mutator before
// it left, and each change is written before the next is read. Expected
// numbers are written out. One check multiplies three scales in file order
// and in bit order, and the step must follow the file.

#include "oa/data/defs/layout.hpp"
#include "oa/data/mutation_rules/apply.hpp"
#include "oa/formats/soft_mutator.hpp"
#include "oa/formats/tdf.hpp"
#include "oa/formats/tdf/edit.hpp"
#include "oa/formats/tdf/number.hpp"
#include "oa/test/check.hpp"

#include <algorithm>
#include <bit>
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

constexpr std::string_view k_ridgetank = R"([UNITINFO]
{
UnitName=RIDGETANK;
MaxDamage=15000;
MaxVelocity=2;
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

void expect_section(
    rules::DocumentSet& docs,
    std::string_view path,
    std::string_view section_name,
    std::string_view key,
    std::string_view want
) {
    const char* got = section_value(docs, path, section_name, key);
    OA_CHECK(got != nullptr && std::string_view(got) == want);
}

void expect_child(
    rules::DocumentSet& docs,
    std::string_view path,
    std::string_view section_name,
    std::string_view child_name,
    std::string_view key,
    std::string_view want
) {
    const char* got = child_value(docs, path, section_name, child_name, key);
    OA_CHECK(got != nullptr && std::string_view(got) == want);
}

bool has_exact_key(const tdf::Block* section, std::string_view want) {
    if (section == nullptr)
        return false;
    const uint32_t count = tdf::property_count(section);
    for (uint32_t place = 0; place < count; ++place) {
        const char* stored = tdf::property_key_at(section, static_cast<int32_t>(place));
        if (stored != nullptr && std::string_view(stored) == want)
            return true;
    }
    return false;
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

/// Applies mutators one step at a time, keeping lock marks between steps.
///
/// @param list mutators in list order
/// @param docs documents to edit
/// @param options layout and field check
/// @return the writes and notes of every step, in step order
rules::ApplyResult apply_list(
    const std::vector<rules::MutatorRules>& list,
    rules::DocumentSet& docs,
    const rules::ApplyOptions& options
) {
    rules::ApplyState state;
    rules::ApplyResult all;
    for (const rules::MutatorRules& one : list) {
        rules::ApplyResult step = rules::apply_step(one, docs, state, options);
        for (rules::FieldWrite& write : step.writes)
            all.writes.push_back(std::move(write));
        for (rules::RuleNote& note : step.notes)
            all.notes.push_back(std::move(note));
    }
    return all;
}

const rules::FieldWrite* last_write(
    const rules::ApplyResult& result,
    std::string_view path,
    std::string_view block,
    std::string_view key
) {
    const rules::FieldWrite* found = nullptr;
    for (const rules::FieldWrite& write : result.writes) {
        if (write.path == path && write.block == block && write.key == key)
            found = &write;
    }
    return found;
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
            "[UNITINFO]\n{\nUnitName=RIDGEFAB;\nBuilder=1;\nWorkerTime=300;\n}\n"
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
        const rules::ApplyResult result = apply_list(list, docs, {});
        expect_unit(docs, "units/RIDGEFAB.fbi", "WorkerTime", "2400");
        expect_unit(docs, "units/RIDGEIDLE.fbi", "WorkerTime", "10");
        OA_CHECK(edited(docs, "units/RIDGEFAB.fbi"));
        OA_CHECK(!edited(docs, "units/RIDGEIDLE.fbi"));
        OA_CHECK(result.writes.size() == 2);
        const rules::FieldWrite* write = last_write(result, "units/RIDGEFAB.fbi", "", "WorkerTime");
        OA_CHECK(write != nullptr);
        if (write == nullptr)
            return;
        OA_CHECK(write->record == "RIDGEFAB");
        OA_CHECK(write->new_text.has_value() && *write->new_text == "2400");
        if (!swapped)
            OA_CHECK(mutators_are(write->mutators, {"swift-hands-4x"}));
        else
            OA_CHECK(mutators_are(write->mutators, {"swift-hands-2x"}));
    };
    run(false);
    run(true);
}

/// Three scales in one mutator multiply in file order.
///
/// The product is the value each scale writes, read back, then scaled again.
/// Sorting the same factors by their bits is a different number, and the step
/// does not use that order.
void scales_in_file_order() {
    const std::string text = "[*Group*]\n{\n[Needs]\n{\nBuilder=1,1;\n}\n[Changes]\n{\n"
                             "WorkerTime=*0.000000000001;\nWorkerTime=*1000000000000;\n"
                             "WorkerTime=*100000000000;\n}\n}\n";
    const sm::ParseResult parsed = parse_sm(text);
    OA_CHECK(!parsed.has_errors());
    OA_CHECK(parsed.rules.blocks.size() == 1);
    if (parsed.has_errors() || parsed.rules.blocks.size() != 1 ||
        parsed.rules.blocks[0].changes.size() != 3)
        return;
    std::string stepped = "1";
    std::vector<double> by_bits;
    for (const sm::Change& change : parsed.rules.blocks[0].changes) {
        const double value = tdf::parse_double(stepped.c_str()) * change.value;
        const std::optional<std::string> spelled = tdf::format_number(value);
        OA_CHECK(spelled.has_value());
        if (!spelled.has_value())
            return;
        stepped = *spelled;
        by_bits.push_back(change.value);
    }
    std::stable_sort(by_bits.begin(), by_bits.end(), [](double left, double right) {
        return std::bit_cast<uint64_t>(left) < std::bit_cast<uint64_t>(right);
    });
    double bits_product = 1;
    for (const double factor : by_bits)
        bits_product *= factor;
    const std::optional<std::string> bits_spelled = tdf::format_number(bits_product);
    OA_CHECK(bits_spelled.has_value() && *bits_spelled != stepped);
    rules::DocumentSet docs;
    OA_CHECK(add_text(
        docs,
        "units/RIDGEFAB.fbi",
        "[UNITINFO]\n{\nUnitName=RIDGEFAB;\nBuilder=1;\nWorkerTime=1;\n}\n"
    ));
    std::vector<rules::MutatorRules> list;
    list.push_back(rules_of("wide-steps", text));
    static_cast<void>(apply_list(list, docs, {}));
    expect_unit(docs, "units/RIDGEFAB.fbi", "WorkerTime", stepped);
}

void storage_factors() {
    auto run = [](bool swapped) {
        rules::DocumentSet docs;
        OA_CHECK(add_text(
            docs,
            "units/RIDGECOM.fbi",
            "[UNITINFO]\n{\nUnitName=RIDGECOM;\nEnergyStorage=1000;\nMetalStorage=250;\n}\n"
        ));
        rules::MutatorRules two =
            rules_of("wide-hold-2x", "[*All*]\n{\nEnergyStorage=*2;\nMetalStorage=*2;\n}\n");
        rules::MutatorRules four =
            rules_of("wide-hold-4x", "[*All*]\n{\nEnergyStorage=*4;\nMetalStorage=*4;\n}\n");
        std::vector<rules::MutatorRules> list;
        if (!swapped) {
            list.push_back(std::move(two));
            list.push_back(std::move(four));
        } else {
            list.push_back(std::move(four));
            list.push_back(std::move(two));
        }
        static_cast<void>(apply_list(list, docs, {}));
        expect_unit(docs, "units/RIDGECOM.fbi", "EnergyStorage", "8000");
        expect_unit(docs, "units/RIDGECOM.fbi", "MetalStorage", "2000");
    };
    run(false);
    run(true);
}

std::string gate_text(const char* name) {
    const char* value = "800";
    if (std::string(name) == "ridge-frugal")
        value = "200";
    else if (std::string(name) == "ridge-moderate")
        value = "400";
    std::string text = "[*All*]\n{\nEnergyMake=";
    text += value;
    text += ";\n}\n";
    return text;
}

void gate_incomes() {
    auto pair = [](const char* first, const char* second, const char* want) {
        rules::DocumentSet docs;
        OA_CHECK(add_text(
            docs, "units/RIDGEGATE.fbi", "[UNITINFO]\n{\nUnitName=RIDGEGATE;\nEnergyMake=100;\n}\n"
        ));
        std::vector<rules::MutatorRules> list;
        list.push_back(rules_of(first, gate_text(first)));
        list.push_back(rules_of(second, gate_text(second)));
        const rules::ApplyResult result = apply_list(list, docs, {});
        expect_unit(docs, "units/RIDGEGATE.fbi", "EnergyMake", want);
        OA_CHECK(count_kind(result, rules::NoteKind::value_replaced) == 1);
        const rules::RuleNote* note = find_note(result, rules::NoteKind::value_replaced);
        OA_CHECK(note != nullptr && mutators_are(note->mutators, {first, second}));
    };
    pair("ridge-frugal", "ridge-moderate", "400");
    pair("ridge-moderate", "ridge-frugal", "200");
    {
        rules::DocumentSet docs;
        OA_CHECK(add_text(
            docs, "units/RIDGEGATE.fbi", "[UNITINFO]\n{\nUnitName=RIDGEGATE;\nEnergyMake=100;\n}\n"
        ));
        std::vector<rules::MutatorRules> list;
        list.push_back(rules_of("ridge-frugal", gate_text("ridge-frugal")));
        list.push_back(rules_of("ridge-moderate", gate_text("ridge-moderate")));
        list.push_back(rules_of("ridge-grand", gate_text("ridge-grand")));
        const rules::ApplyResult result = apply_list(list, docs, {});
        expect_unit(docs, "units/RIDGEGATE.fbi", "EnergyMake", "800");
        OA_CHECK(count_kind(result, rules::NoteKind::value_replaced) == 2);
        const rules::FieldWrite* write =
            last_write(result, "units/RIDGEGATE.fbi", "", "EnergyMake");
        OA_CHECK(write != nullptr && mutators_are(write->mutators, {"ridge-grand"}));
    }
}

void stacked_removals() {
    auto run = [](bool swapped) {
        rules::DocumentSet docs;
        OA_CHECK(add_text(
            docs,
            "units/RIDGEPLANE.fbi",
            "[UNITINFO]\n{\nUnitName=RIDGEPLANE;\nCopyright=Ridge;\n}\n"
        ));
        rules::MutatorRules sky = rules_of("clear-sky", "[*All*]\n{\nCopyright=\"\";\n}\n");
        rules::MutatorRules ground = rules_of("close-ground", "[*All*]\n{\nCopyright=\"\";\n}\n");
        std::vector<rules::MutatorRules> list;
        if (!swapped) {
            list.push_back(std::move(sky));
            list.push_back(std::move(ground));
        } else {
            list.push_back(std::move(ground));
            list.push_back(std::move(sky));
        }
        const rules::ApplyResult result = apply_list(list, docs, {});
        expect_absent_unit(docs, "units/RIDGEPLANE.fbi", "Copyright");
        OA_CHECK(edited(docs, "units/RIDGEPLANE.fbi"));
        OA_CHECK(result.writes.size() == 1);
        const rules::FieldWrite* write =
            find_write(result, "units/RIDGEPLANE.fbi", "", "Copyright");
        OA_CHECK(write != nullptr && !write->new_text.has_value());
        OA_CHECK(count_kind(result, rules::NoteKind::value_replaced) == 0);
    };
    run(false);
    run(true);
}

void weapon_text() {
    auto run = [](bool hot_last, const char* want) {
        rules::DocumentSet docs;
        OA_CHECK(add_text(
            docs, "units/RIDGEFAST.fbi", "[UNITINFO]\n{\nUnitName=RIDGEFAST;\nWeapon1=NONE;\n}\n"
        ));
        rules::MutatorRules zip = rules_of("ridge-zip", "[*All*]\n{\nWeapon1=\"RIDGE_ZIP\";\n}\n");
        rules::MutatorRules hot = rules_of("ridge-hot", "[*All*]\n{\nWeapon1=\"RIDGE_HOT\";\n}\n");
        std::vector<rules::MutatorRules> list;
        if (hot_last) {
            list.push_back(std::move(zip));
            list.push_back(std::move(hot));
        } else {
            list.push_back(std::move(hot));
            list.push_back(std::move(zip));
        }
        const rules::ApplyResult result = apply_list(list, docs, {});
        expect_unit(docs, "units/RIDGEFAST.fbi", "Weapon1", want);
        const rules::RuleNote* note = find_note(result, rules::NoteKind::value_replaced);
        OA_CHECK(note != nullptr);
        if (note == nullptr)
            return;
        if (hot_last)
            OA_CHECK(mutators_are(note->mutators, {"ridge-zip", "ridge-hot"}));
        else
            OA_CHECK(mutators_are(note->mutators, {"ridge-hot", "ridge-zip"}));
    };
    run(true, "RIDGE_HOT");
    run(false, "RIDGE_ZIP");
}

void later_band() {
    rules::DocumentSet docs;
    OA_CHECK(add_text(
        docs,
        "units/RIDGECOM.fbi",
        "[UNITINFO]\n{\nUnitName=RIDGECOM;\nBuildCostEnergy=34125;\nBuildCostMetal=29854;\n"
        "CloakCost=1;\nCloakCostMoving=2;\n}\n"
    ));
    std::vector<rules::MutatorRules> list;
    list.push_back(rules_of(
        "ridge-cloak",
        "[*Group*]\n{\n[Needs]\n{\nBuildCostEnergy=30000,40000;\n}\n[Changes]\n{\n"
        "CloakCost=4;\nCloakCostMoving=8;\n}\n}\n"
        "[*Group*]\n{\n[Needs]\n{\nBuildCostMetal=20000,40000;\n}\n[Changes]\n{\n"
        "CloakCost=5;\nCloakCostMoving=10;\n}\n}\n"
    ));
    static_cast<void>(apply_list(list, docs, {}));
    expect_unit(docs, "units/RIDGECOM.fbi", "CloakCost", "5");
    expect_unit(docs, "units/RIDGECOM.fbi", "CloakCostMoving", "10");
}

void clamp_bounds_its_own_scale() {
    auto run = [](bool clamp_first, const char* want) {
        rules::DocumentSet docs;
        OA_CHECK(add_text(
            docs,
            "units/RIDGECOM.fbi",
            "[UNITINFO]\n{\nUnitName=RIDGECOM;\nBuildCostEnergy=100;\n}\n"
        ));
        rules::MutatorRules bounded =
            rules_of("ridge-near", "[*All*]\n{\nBuildCostEnergy=*10(1,150);\n}\n");
        rules::MutatorRules open = rules_of("ridge-far", "[*All*]\n{\nBuildCostEnergy=*2;\n}\n");
        std::vector<rules::MutatorRules> list;
        if (clamp_first) {
            list.push_back(std::move(bounded));
            list.push_back(std::move(open));
        } else {
            list.push_back(std::move(open));
            list.push_back(std::move(bounded));
        }
        static_cast<void>(apply_list(list, docs, {}));
        expect_unit(docs, "units/RIDGECOM.fbi", "BuildCostEnergy", want);
    };
    run(true, "300");
    run(false, "150");
    {
        rules::DocumentSet docs;
        OA_CHECK(add_text(docs, "units/RIDGETANK.fbi", k_ridgetank));
        std::vector<rules::MutatorRules> list;
        list.push_back(rules_of("iron-skin", "[*All*]\n{\nMaxDamage=*2(1,29900);\n}\n"));
        static_cast<void>(apply_list(list, docs, {}));
        expect_unit(docs, "units/RIDGETANK.fbi", "MaxDamage", "29900");
    }
}

void lock_holds() {
    {
        rules::DocumentSet docs;
        OA_CHECK(add_text(
            docs,
            "units/RIDGECOM.fbi",
            "[UNITINFO]\n{\nUnitName=RIDGECOM;\nEnergyMake=25;\nWorkerTime=300;\n}\n"
        ));
        std::vector<rules::MutatorRules> list;
        list.push_back(rules_of("ridge-lock", "[*All*]\n{\nEnergyMake=^80;\n}\n"));
        list.push_back(
            rules_of("ridge-push", "[*All*]\n{\nEnergyMake=*2;\nEnergyMake=5;\nWorkerTime=*4;\n}\n")
        );
        const rules::ApplyResult result = apply_list(list, docs, {});
        expect_unit(docs, "units/RIDGECOM.fbi", "EnergyMake", "80");
        expect_unit(docs, "units/RIDGECOM.fbi", "WorkerTime", "1200");
        OA_CHECK(count_kind(result, rules::NoteKind::field_locked) == 2);
        for (const rules::RuleNote& note : result.notes) {
            if (note.kind != rules::NoteKind::field_locked)
                continue;
            OA_CHECK(mutators_are(note.mutators, {"ridge-lock", "ridge-push"}));
            OA_CHECK(note.key == "EnergyMake");
            OA_CHECK(note.message.find("ridge-lock") != std::string::npos);
            OA_CHECK(note.message.find("ridge-push") != std::string::npos);
        }
    }
    {
        rules::DocumentSet docs;
        OA_CHECK(add_text(
            docs, "units/RIDGECOM.fbi", "[UNITINFO]\n{\nUnitName=RIDGECOM;\nEnergyMake=25;\n}\n"
        ));
        std::vector<rules::MutatorRules> list;
        list.push_back(rules_of("ridge-push", "[*All*]\n{\nEnergyMake=5;\n}\n"));
        list.push_back(rules_of("ridge-lock", "[*All*]\n{\nEnergyMake=^80;\n}\n"));
        list.push_back(rules_of("ridge-after", "[*All*]\n{\nEnergyMake=*2;\n}\n"));
        const rules::ApplyResult result = apply_list(list, docs, {});
        expect_unit(docs, "units/RIDGECOM.fbi", "EnergyMake", "80");
        OA_CHECK(count_kind(result, rules::NoteKind::value_replaced) == 1);
        OA_CHECK(count_kind(result, rules::NoteKind::field_locked) == 1);
        const rules::RuleNote* replaced = find_note(result, rules::NoteKind::value_replaced);
        const rules::RuleNote* locked = find_note(result, rules::NoteKind::field_locked);
        OA_CHECK(
            replaced != nullptr && mutators_are(replaced->mutators, {"ridge-push", "ridge-lock"})
        );
        OA_CHECK(
            locked != nullptr && mutators_are(locked->mutators, {"ridge-lock", "ridge-after"})
        );
    }
    {
        // A lock of the value the field already holds still marks it.
        rules::DocumentSet docs;
        OA_CHECK(add_text(
            docs, "units/RIDGECOM.fbi", "[UNITINFO]\n{\nUnitName=RIDGECOM;\nEnergyMake=80;\n}\n"
        ));
        std::vector<rules::MutatorRules> list;
        list.push_back(rules_of("ridge-lock", "[*All*]\n{\nEnergyMake=^80;\n}\n"));
        list.push_back(rules_of("ridge-push", "[*All*]\n{\nEnergyMake=*2;\n}\n"));
        const rules::ApplyResult result = apply_list(list, docs, {});
        expect_unit(docs, "units/RIDGECOM.fbi", "EnergyMake", "80");
        OA_CHECK(result.writes.empty());
        OA_CHECK(!edited(docs, "units/RIDGECOM.fbi"));
        OA_CHECK(count_kind(result, rules::NoteKind::field_locked) == 1);
        const rules::RuleNote* locked = find_note(result, rules::NoteKind::field_locked);
        OA_CHECK(locked != nullptr && mutators_are(locked->mutators, {"ridge-lock", "ridge-push"}));
    }
}

void clamps_do_not_join() {
    auto value_of = [](std::vector<rules::MutatorRules> list) {
        rules::DocumentSet docs;
        OA_CHECK(add_text(
            docs, "units/RIDGETANK.fbi", "[UNITINFO]\n{\nUnitName=RIDGETANK;\nMaxDamage=40;\n}\n"
        ));
        static_cast<void>(apply_list(list, docs, {}));
        const char* got = unit_value(docs, "units/RIDGETANK.fbi", "MaxDamage");
        return got != nullptr ? std::string(got) : std::string();
    };
    std::vector<rules::MutatorRules> together;
    together.push_back(
        rules_of("ridge-plates", "[*All*]\n{\nMaxDamage=*2(1,50);\nMaxDamage=*3(1,1000);\n}\n")
    );
    std::vector<rules::MutatorRules> split;
    split.push_back(rules_of("ridge-plates-a", "[*All*]\n{\nMaxDamage=*2(1,50);\n}\n"));
    split.push_back(rules_of("ridge-plates-b", "[*All*]\n{\nMaxDamage=*3(1,1000);\n}\n"));
    OA_CHECK(value_of(std::move(together)) == "150");
    OA_CHECK(value_of(std::move(split)) == "150");
}

void text_rules() {
    {
        rules::DocumentSet docs;
        OA_CHECK(add_text(
            docs, "units/RIDGEFAB.fbi", "[UNITINFO]\n{\nUnitName=RIDGEFAB;\nMaxVelocity=1.6;\n}\n"
        ));
        std::vector<rules::MutatorRules> list;
        list.push_back(rules_of("stretch", "[*All*]\n{\nMaxVelocity=*1.5;\n}\n"));
        static_cast<void>(apply_list(list, docs, {}));
        expect_unit(docs, "units/RIDGEFAB.fbi", "MaxVelocity", "2.4");
        const tdf::Block* section = unit_section(docs, "units/RIDGEFAB.fbi");
        OA_CHECK(section != nullptr);
        if (section != nullptr)
            OA_CHECK(tdf::get_fixed(section, "MaxVelocity", 0) == 157286);
    }
    auto later_number = [](bool swapped) {
        rules::DocumentSet docs;
        OA_CHECK(add_text(
            docs, "units/RIDGEFAB.fbi", "[UNITINFO]\n{\nUnitName=RIDGEFAB;\nSightDistance=1;\n}\n"
        ));
        rules::MutatorRules low = rules_of("low", "[*All*]\n{\nSightDistance=10;\n}\n");
        rules::MutatorRules high = rules_of("high", "[*All*]\n{\nSightDistance=20;\n}\n");
        std::vector<rules::MutatorRules> list;
        const char* first = "low";
        const char* second = "high";
        const char* want = "20";
        if (!swapped) {
            list.push_back(std::move(low));
            list.push_back(std::move(high));
        } else {
            list.push_back(std::move(high));
            list.push_back(std::move(low));
            first = "high";
            second = "low";
            want = "10";
        }
        const rules::ApplyResult result = apply_list(list, docs, {});
        expect_unit(docs, "units/RIDGEFAB.fbi", "SightDistance", want);
        const rules::FieldWrite* write =
            last_write(result, "units/RIDGEFAB.fbi", "", "SightDistance");
        OA_CHECK(write != nullptr && mutators_are(write->mutators, {second}));
        OA_CHECK(count_kind(result, rules::NoteKind::value_replaced) == 1);
        const rules::RuleNote* note = find_note(result, rules::NoteKind::value_replaced);
        OA_CHECK(note != nullptr && mutators_are(note->mutators, {first, second}));
    };
    later_number(false);
    later_number(true);
    {
        rules::DocumentSet docs;
        OA_CHECK(add_text(
            docs, "units/RIDGETANK.fbi", "[UNITINFO]\n{\nUnitName=RIDGETANK;\nWeapon1=NONE;\n}\n"
        ));
        std::vector<rules::MutatorRules> list;
        list.push_back(rules_of("armoury", "[*All*]\n{\nWeapon1=\"A\";\n}\n"));
        static_cast<void>(apply_list(list, docs, {}));
        expect_unit(docs, "units/RIDGETANK.fbi", "Weapon1", "A");
    }
    {
        rules::DocumentSet docs;
        OA_CHECK(add_text(
            docs, "units/RIDGETANK.fbi", "[UNITINFO]\n{\nUnitName=RIDGETANK;\nCopyright=Ridge;\n}\n"
        ));
        std::vector<rules::MutatorRules> list;
        list.push_back(rules_of("one-clear", "[*All*]\n{\nCopyright=\"\";\n}\n"));
        const rules::ApplyResult result = apply_list(list, docs, {});
        expect_absent_unit(docs, "units/RIDGETANK.fbi", "Copyright");
        const rules::FieldWrite* write = find_write(result, "units/RIDGETANK.fbi", "", "Copyright");
        OA_CHECK(write != nullptr && !write->new_text.has_value());
        OA_CHECK(write != nullptr && write->old_text.has_value() && *write->old_text == "Ridge");
    }
    auto append = [](bool swapped, std::string_view want, std::string_view middle) {
        rules::DocumentSet docs;
        OA_CHECK(add_text(
            docs, "units/RIDGETANK.fbi", "[UNITINFO]\n{\nUnitName=RIDGETANK;\nDescription=R;\n}\n"
        ));
        rules::MutatorRules idge = rules_of("idge", "[*All*]\n{\nDescription=\"idge\"+;\n}\n");
        rules::MutatorRules com = rules_of("com", "[*All*]\n{\nDescription=\"com\"+;\n}\n");
        std::vector<rules::MutatorRules> list;
        const char* second = "com";
        if (!swapped) {
            list.push_back(std::move(idge));
            list.push_back(std::move(com));
        } else {
            list.push_back(std::move(com));
            list.push_back(std::move(idge));
            second = "idge";
        }
        const rules::ApplyResult result = apply_list(list, docs, {});
        expect_unit(docs, "units/RIDGETANK.fbi", "Description", want);
        OA_CHECK(result.writes.size() == 2);
        OA_CHECK(count_kind(result, rules::NoteKind::value_replaced) == 0);
        const rules::FieldWrite* first_write =
            find_write(result, "units/RIDGETANK.fbi", "", "Description");
        const rules::FieldWrite* write =
            last_write(result, "units/RIDGETANK.fbi", "", "Description");
        OA_CHECK(
            first_write != nullptr && first_write->new_text.has_value() &&
            *first_write->new_text == middle
        );
        OA_CHECK(write != nullptr && mutators_are(write->mutators, {second}));
    };
    append(false, "Ridgecom", "Ridge");
    append(true, "Rcomidge", "Rcom");
    {
        rules::DocumentSet docs;
        OA_CHECK(add_text(
            docs, "units/RIDGETANK.fbi", "[UNITINFO]\n{\nUnitName=RIDGETANK;\nWeapon1=Q;\n}\n"
        ));
        std::vector<rules::MutatorRules> list;
        list.push_back(rules_of("assigner", "[*All*]\n{\nWeapon1=\"A\";\n}\n"));
        list.push_back(rules_of("appender", "[*All*]\n{\nWeapon1=\"Z\"+;\n}\n"));
        const rules::ApplyResult result = apply_list(list, docs, {});
        expect_unit(docs, "units/RIDGETANK.fbi", "Weapon1", "AZ");
        OA_CHECK(result.writes.size() == 2);
        OA_CHECK(count_kind(result, rules::NoteKind::value_replaced) == 0);
        const rules::FieldWrite* write = last_write(result, "units/RIDGETANK.fbi", "", "Weapon1");
        OA_CHECK(write != nullptr && mutators_are(write->mutators, {"appender"}));
        OA_CHECK(write != nullptr && write->new_text.has_value() && *write->new_text == "AZ");
    }
    {
        rules::DocumentSet docs;
        OA_CHECK(add_text(
            docs, "units/RIDGETANK.fbi", "[UNITINFO]\n{\nUnitName=RIDGETANK;\nMaxDamage=100;\n}\n"
        ));
        std::vector<rules::MutatorRules> list;
        list.push_back(rules_of("scaler", "[*All*]\n{\nMaxDamage=*2;\n}\n"));
        list.push_back(rules_of("quoter", "[*All*]\n{\nMaxDamage=\"850\";\n}\n"));
        const rules::ApplyResult result = apply_list(list, docs, {});
        expect_unit(docs, "units/RIDGETANK.fbi", "MaxDamage", "850");
        OA_CHECK(count_kind(result, rules::NoteKind::value_replaced) == 1);
        const rules::RuleNote* note = find_note(result, rules::NoteKind::value_replaced);
        OA_CHECK(note != nullptr && mutators_are(note->mutators, {"scaler", "quoter"}));
        const rules::FieldWrite* write = last_write(result, "units/RIDGETANK.fbi", "", "MaxDamage");
        OA_CHECK(write != nullptr && mutators_are(write->mutators, {"quoter"}));
    }
    {
        rules::DocumentSet docs;
        OA_CHECK(add_text(
            docs, "units/RIDGECOM.fbi", "[UNITINFO]\n{\nUnitName=RIDGECOM;\nEnergyMake=25;\n}\n"
        ));
        std::vector<rules::MutatorRules> list;
        list.push_back(rules_of("meta", "[*All*]\n{\nEnergyMake=^1000;\n}\n"));
        list.push_back(rules_of("quoter", "[*All*]\n{\nEnergyMake=\"nope\";\n}\n"));
        const rules::ApplyResult result = apply_list(list, docs, {});
        expect_unit(docs, "units/RIDGECOM.fbi", "EnergyMake", "1000");
        OA_CHECK(count_kind(result, rules::NoteKind::field_locked) == 1);
        OA_CHECK(count_kind(result, rules::NoteKind::value_replaced) == 0);
        const rules::RuleNote* note = find_note(result, rules::NoteKind::field_locked);
        OA_CHECK(note != nullptr && mutators_are(note->mutators, {"meta", "quoter"}));
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
        static_cast<void>(apply_list(list, docs, {}));
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
    static_cast<void>(apply_list(list, docs, {}));
    expect_unit(docs, "units/RIDGESLOW.fbi", "WorkerTime", "10");
    expect_unit(docs, "units/RIDGEFAST.fbi", "WorkerTime", "10");
    expect_unit(docs, "units/RIDGESLOW.fbi", "BuildDistance", "5");
    expect_unit(docs, "units/RIDGEFAST.fbi", "BuildDistance", "5");
    expect_unit(docs, "units/RIDGESLOW.fbi", "CloakCost", "9");
    expect_unit(docs, "units/RIDGEFAST.fbi", "CloakCost", "9");
    // A lock sets the value, and a later change of the same mutator still applies.
    expect_unit(docs, "units/RIDGESLOW.fbi", "EnergyMake", "14");
    expect_unit(docs, "units/RIDGEFAST.fbi", "EnergyMake", "14");
    expect_unit(docs, "units/RIDGESLOW.fbi", "SightDistance", "120");
    expect_unit(docs, "units/RIDGEFAST.fbi", "SightDistance", "99.9996");
}

void needs_see_earlier_writes() {
    auto same = [](bool reversed) {
        rules::DocumentSet docs;
        OA_CHECK(add_text(
            docs,
            "units/RIDGETANK.fbi",
            "[UNITINFO]\n{\nUnitName=RIDGETANK;\nSightDistance=10;\n}\n"
        ));
        const std::string lift = "[*All*]\n{\nCanFly=1;\n}\n";
        const std::string spot =
            "[*Group*]\n{\n[Needs]\n{\nCanFly=1,1;\n}\n[Changes]\n{\nSightDistance=50;\n}\n}\n";
        std::vector<rules::MutatorRules> list;
        list.push_back(rules_of("ridge-lift", reversed ? spot + lift : lift + spot));
        const rules::ApplyResult result = apply_list(list, docs, {});
        expect_unit(docs, "units/RIDGETANK.fbi", "CanFly", "1");
        if (!reversed) {
            expect_unit(docs, "units/RIDGETANK.fbi", "SightDistance", "50");
            OA_CHECK(count_kind(result, rules::NoteKind::matched_nothing) == 0);
        } else {
            expect_unit(docs, "units/RIDGETANK.fbi", "SightDistance", "10");
            OA_CHECK(find_write(result, "units/RIDGETANK.fbi", "", "SightDistance") == nullptr);
            OA_CHECK(count_kind(result, rules::NoteKind::matched_nothing) == 1);
        }
    };
    same(false);
    same(true);
    {
        rules::DocumentSet docs;
        OA_CHECK(add_text(
            docs,
            "units/RIDGETANK.fbi",
            "[UNITINFO]\n{\nUnitName=RIDGETANK;\nSightDistance=10;\n}\n"
        ));
        std::vector<rules::MutatorRules> list;
        list.push_back(rules_of("ridge-lift", "[*All*]\n{\nCanFly=1;\n}\n"));
        list.push_back(rules_of(
            "ridge-spot",
            "[*Group*]\n{\n[Needs]\n{\nCanFly=1,1;\n}\n[Changes]\n{\nSightDistance=50;\n}\n}\n"
        ));
        const rules::ApplyResult result = apply_list(list, docs, {});
        expect_unit(docs, "units/RIDGETANK.fbi", "CanFly", "1");
        expect_unit(docs, "units/RIDGETANK.fbi", "SightDistance", "50");
        OA_CHECK(count_kind(result, rules::NoteKind::matched_nothing) == 0);
        const rules::FieldWrite* write =
            find_write(result, "units/RIDGETANK.fbi", "", "SightDistance");
        OA_CHECK(write != nullptr && mutators_are(write->mutators, {"ridge-spot"}));
    }
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
    const rules::ApplyResult result = apply_list(list, docs, {});
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
    static_cast<void>(apply_list(presence, present, {}));
    expect_unit(present, "units/RIDGEHAS.fbi", "HideDamage", "1");
    expect_absent_unit(present, "units/RIDGEHAS.fbi", "Amphibious");

    rules::DocumentSet only_factor;
    OA_CHECK(add_text(
        only_factor, "units/RIDGEFAB.fbi", "[UNITINFO]\n{\nUnitName=RIDGEFAB;\nBuilder=1;\n}\n"
    ));
    std::vector<rules::MutatorRules> factor;
    factor.push_back(rules_of("absent-factor", "[*All*]\n{\nWorkerTime=*2;\n}\n"));
    const rules::ApplyResult factor_result = apply_list(factor, only_factor, {});
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
    const rules::ApplyResult result = apply_list(list, docs, {});
    expect_child(docs, "weapons/ridge.tdf", "RIDGE_LASER", "DAMAGE", "default", "50");
    expect_child(docs, "weapons/ridge.tdf", "RIDGE_LASER", "DAMAGE", "RIDGECOM", "20");
    expect_child(docs, "weapons/ridge.tdf", "RIDGE_BOLT", "DAMAGE", "default", "40");
    expect_child(docs, "weapons/ridge.tdf", "RIDGE_BOLT", "DAMAGE", "RIDGETANK", "10");
    expect_section(docs, "weapons/ridge.tdf", "RIDGE_LASER", "range", "32000");
    expect_section(docs, "weapons/ridge.tdf", "RIDGE_LASER", "reloadtime", "8");
    OA_CHECK(section_value(docs, "weapons/ridge.tdf", "RIDGE_LASER", "burst") == nullptr);
    expect_section(docs, "weapons/ridge.tdf", "RIDGE_BOLT", "reloadtime", "9");
    expect_section(docs, "weapons/ridge.tdf", "RIDGE_BOLT", "burst", "3");
    expect_section(docs, "weapons/ridge.tdf", "RIDGE_LASER", "accuracy", "7");
    expect_section(docs, "weapons/ridge.tdf", "RIDGE_BOLT", "accuracy", "7");
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
        "folder=features\\corpses;\nextension=tdf;\n"
        "[*All*]\n{\nmetal=7;\n}\n"
        "[*Group*]\n{\n[Needs]\n{\nridge_tag=0,0;\n}\n[Changes]\n{\ndamage=1;\n}\n}\n"
        "[*Group*]\n{\n[Needs]\n{\nridge_tag=1,1;\n}\n[Changes]\n{\ndamage=5;\n}\n}\n"
        "[*Group*]\n{\n[Needs]\n{\nridge_tag=\"\";\n}\n[Changes]\n{\nenergy=1;\n}\n}\n"
        "[*Group*]\n{\n[Needs]\n{\nridge_tag=\"set\";\n}\n[Changes]\n{\nenergy=5;\n}\n}\n"
        "[*Group*]\n{\n[Needs]\n{\nmaxslope=255,255;\n}\n[Changes]\n{\nreclaimtime=9;\n}\n}\n"
        "[*Group*]\n{\n[Needs]\n{\nmaxslope=0,0;\n}\n[Changes]\n{\nresurrect=4;\n}\n}\n"
    ));
    const rules::ApplyResult result = apply_list(list, docs, {});
    expect_section(docs, "features/corpses/ridge.tdf", "RIDGE_WRECK", "metal", "7");
    expect_section(docs, "features/corpses/ridge.tdf", "RIDGE_WRECK", "damage", "1");
    expect_section(docs, "features/corpses/ridge.tdf", "RIDGE_WRECK", "energy", "1");
    expect_section(docs, "features/corpses/ridge.tdf", "RIDGE_WRECK", "resurrect", "4");
    OA_CHECK(
        section_value(docs, "features/corpses/ridge.tdf", "RIDGE_WRECK", "reclaimtime") == nullptr
    );
    expect_section(docs, "features/rocks/ridge.tdf", "RIDGE_STONE", "metal", "10");
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
    static_cast<void>(apply_list(move_rules, move, {}));
    expect_section(move, "gamedata/moveinfo.tdf", "RIDGE_MOVE", "crushstrength", "1");
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
    static_cast<void>(apply_list(side_rules, sides, {}));
    expect_section(sides, "gamedata/sidedata.tdf", "SIDE0", "commander", "RIDGECOM");

    rules::DocumentSet docs;
    OA_CHECK(add_text(
        docs, "units/RIDGECOM.fbi", "[UNITINFO]\n{\nUnitName=ridgecom;\nWorkerTime=10;\n}\n"
    ));
    std::vector<rules::MutatorRules> list;
    list.push_back(
        rules_of("by-name", "[RIDGECOM]\n{\nWorkerTime=*3;\n}\n[UNITINFO]\n{\nWorkerTime=99;\n}\n")
    );
    const rules::ApplyResult result = apply_list(list, docs, {});
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
    static_cast<void>(apply_list(list, docs, {}));
    expect_unit(docs, "machines/RIDGEFAB.fbi", "WorkerTime", "60");
    expect_unit(docs, "units/RIDGEFAB.fbi", "WorkerTime", "30");
    OA_CHECK(edited(docs, "machines/RIDGEFAB.fbi"));
    OA_CHECK(!edited(docs, "units/RIDGEFAB.fbi"));
}

void created_spelling() {
    auto run = [](bool upper_first) {
        rules::DocumentSet docs;
        OA_CHECK(add_text(docs, "units/RIDGECOM.fbi", "[UNITINFO]\n{\nUnitName=RIDGECOM;\n}\n"));
        rules::MutatorRules lower = rules_of("lower", "[*All*]\n{\nenergystorage=10;\n}\n");
        rules::MutatorRules upper = rules_of("upper", "[*All*]\n{\nEnergyStorage=30;\n}\n");
        std::vector<rules::MutatorRules> list;
        const char* spelling = "energystorage";
        const char* want = "30";
        const char* first = "lower";
        const char* second = "upper";
        if (!upper_first) {
            list.push_back(std::move(lower));
            list.push_back(std::move(upper));
        } else {
            list.push_back(std::move(upper));
            list.push_back(std::move(lower));
            spelling = "EnergyStorage";
            want = "10";
            first = "upper";
            second = "lower";
        }
        const rules::ApplyResult result = apply_list(list, docs, {});
        expect_unit(docs, "units/RIDGECOM.fbi", "EnergyStorage", want);
        const tdf::Block* section = unit_section(docs, "units/RIDGECOM.fbi");
        OA_CHECK(has_exact_key(section, spelling));
        OA_CHECK(!has_exact_key(section, upper_first ? "energystorage" : "EnergyStorage"));
        const rules::FieldWrite* write = last_write(result, "units/RIDGECOM.fbi", "", spelling);
        OA_CHECK(write != nullptr && write->key == spelling);
        OA_CHECK(write != nullptr && write->new_text.has_value() && *write->new_text == want);
        OA_CHECK(count_kind(result, rules::NoteKind::value_replaced) == 1);
        const rules::RuleNote* note = find_note(result, rules::NoteKind::value_replaced);
        OA_CHECK(note != nullptr && mutators_are(note->mutators, {first, second}));
    };
    run(false);
    run(true);
}

/// Writes of one field are listed in the order they were applied, even when a
/// later change creates the field again under another spelling.
void writes_in_applied_order() {
    rules::DocumentSet docs;
    OA_CHECK(add_text(
        docs, "units/RIDGEFAB.fbi", "[UNITINFO]\n{\nUnitName=RIDGEFAB;\nworkertime=5;\n}\n"
    ));
    OA_CHECK(add_text(
        docs, "units/RIDGEAAA.fbi", "[UNITINFO]\n{\nUnitName=RIDGEAAA;\nWORKERTIME=1;\n}\n"
    ));
    std::vector<rules::MutatorRules> list;
    list.push_back(
        rules_of("ridge-reset", "[*All*]\n{\nworkertime=\"\";\nWorkerTime=7;\nWORKERTIME=*2;\n}\n")
    );
    const rules::ApplyResult result = apply_list(list, docs, {});
    expect_unit(docs, "units/RIDGEFAB.fbi", "WorkerTime", "14");
    OA_CHECK(has_exact_key(unit_section(docs, "units/RIDGEFAB.fbi"), "WorkerTime"));
    std::vector<const rules::FieldWrite*> chain;
    for (const rules::FieldWrite& write : result.writes) {
        if (write.path == "units/RIDGEFAB.fbi")
            chain.push_back(&write);
    }
    OA_CHECK(chain.size() == 3);
    if (chain.size() != 3)
        return;
    // The removal, then the creation under the new spelling, then the factor.
    OA_CHECK(chain[0]->key == "workertime");
    OA_CHECK(chain[0]->old_text.has_value() && *chain[0]->old_text == "5");
    OA_CHECK(!chain[0]->new_text.has_value());
    OA_CHECK(chain[1]->key == "WorkerTime");
    OA_CHECK(!chain[1]->old_text.has_value());
    OA_CHECK(chain[1]->new_text.has_value() && *chain[1]->new_text == "7");
    OA_CHECK(chain[2]->key == "WorkerTime");
    OA_CHECK(chain[2]->old_text.has_value() && *chain[2]->old_text == "7");
    OA_CHECK(chain[2]->new_text.has_value() && *chain[2]->new_text == "14");
    // The other unit's writes sort before RIDGEFAB's, all of them together.
    OA_CHECK(result.writes.size() == 6);
    if (result.writes.size() == 6) {
        OA_CHECK(result.writes[0].path == "units/RIDGEAAA.fbi");
        OA_CHECK(result.writes[2].path == "units/RIDGEAAA.fbi");
        OA_CHECK(result.writes[3].path == "units/RIDGEFAB.fbi");
    }
}

void depth_case() {
    rules::DocumentSet docs;
    OA_CHECK(add_text(
        docs,
        "weapons/ridge.tdf",
        "[RIDGE_LASER]\n{\n[DAMAGE]\n{\ndefault=100;\nDEFAULT=60;\n}\n}\n"
    ));
    std::vector<rules::MutatorRules> list;
    list.push_back(
        rules_of("halver", "folder=weapons;\nextension=tdf;\n[*All*]\n{\n<AllDepth2>=*0.5;\n}\n")
    );
    const rules::ApplyResult result = apply_list(list, docs, {});
    expect_child(docs, "weapons/ridge.tdf", "RIDGE_LASER", "DAMAGE", "DEFAULT", "30");
    OA_CHECK(result.writes.size() == 1);
    const rules::FieldWrite* write = find_write(result, "weapons/ridge.tdf", "DAMAGE", "DEFAULT");
    OA_CHECK(write != nullptr && write->old_text.has_value() && *write->old_text == "60");
    OA_CHECK(write != nullptr && write->new_text.has_value() && *write->new_text == "30");
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
        const rules::ApplyResult result = apply_list(list, docs, {});
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
        const rules::ApplyResult result = apply_list(list, docs, {});
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
        const rules::ApplyResult result = apply_list(list, docs, {});
        expect_unit(docs, "units/RIDGEFAB.fbi", "WorkerTime", "10");
        OA_CHECK(result.writes.empty());
        OA_CHECK(!edited(docs, "units/RIDGEFAB.fbi"));
        OA_CHECK(count_kind(result, rules::NoteKind::not_finite) == 2);
    }
    {
        // A clamp is not allowed to pull a non-finite product back into range.
        std::string digits(309, '0');
        digits[0] = '1';
        const std::string text = "[*All*]\n{\nWorkerTime=*" + digits + "(0,5);\n}\n";
        rules::DocumentSet docs;
        OA_CHECK(add_text(
            docs, "units/RIDGEFAB.fbi", "[UNITINFO]\n{\nUnitName=RIDGEFAB;\nWorkerTime=10;\n}\n"
        ));
        std::vector<rules::MutatorRules> list;
        list.push_back(rules_of("overflow-clamp", text));
        const rules::ApplyResult result = apply_list(list, docs, {});
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
        const rules::ApplyResult result = apply_list(list, docs, options);
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
        const rules::ApplyResult result = apply_list(list, docs, {});
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
    scales_in_file_order();
    storage_factors();
    gate_incomes();
    stacked_removals();
    weapon_text();
    later_band();
    clamp_bounds_its_own_scale();
    lock_holds();
    clamps_do_not_join();
    text_rules();
    within_mutator();
    needs_see_earlier_writes();
    fields_left_out();
    weapons();
    features_and_moveinfo();
    sidedata_and_names();
    layout_rename();
    created_spelling();
    writes_in_applied_order();
    depth_case();
    schema_kinds();
    notes();
    return oa::test::check_exit_status();
}
