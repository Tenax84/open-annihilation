// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Weapon ids for weapons a mutator brought. Base weapons keep their ids.
// A requested id is kept when it is free. Nothing is written when the
// table has no id left.
#include "oa/data/mutators/weapon_ids.hpp"
#include "oa/formats/tdf.hpp"
#include "oa/test/check.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace mutators = oa::data::mutators;
namespace rules = oa::data::mutation_rules;
namespace tdf = oa::formats::tdf;
namespace defs = oa::data::defs;

bool add_doc(rules::DocumentSet& documents, std::string path, std::string_view text) {
    std::string error;
    const bool added = documents.add(std::move(path), text, &error);
    if (!added)
        std::fprintf(stderr, "weapon ids: %s\n", error.c_str());
    OA_CHECK(added);
    return added;
}

std::string id_text(rules::DocumentSet& documents, std::string_view path, std::string_view weapon) {
    rules::LoadedDocument* loaded = documents.find(path);
    OA_CHECK(loaded != nullptr);
    if (loaded == nullptr)
        return {};
    const tdf::Block* root = loaded->document.get()->root;
    const std::string name{weapon};
    const tdf::Block* section = root != nullptr ? tdf::find_child(root, name.c_str()) : nullptr;
    const char* value = section != nullptr ? tdf::find_value(section, "ID") : nullptr;
    return value != nullptr ? std::string{value} : std::string{};
}

mutators::BroughtWeapon brought_of(std::string name, uint16_t mutator, std::string path) {
    mutators::BroughtWeapon weapon;
    weapon.name = std::move(name);
    weapon.mutator = mutator;
    weapon.path = std::move(path);
    return weapon;
}

void test_request_is_honoured_and_base_id_stays() {
    rules::DocumentSet documents;
    add_doc(documents, "weapons/ridge.tdf", "[RGGUN]\n{\nID=1;\n}\n[RGALPHA]\n{\nID=5;\n}\n");
    std::vector<mutators::BroughtWeapon> brought;
    brought.push_back(brought_of("RGALPHA", 0, "weapons/ridge.tdf"));
    mutators::WeaponIdResult result;
    OA_CHECK(mutators::assign_weapon_ids(documents, defs::data_layout(), brought, result));
    OA_CHECK(result.ok);
    OA_CHECK(brought.size() == 1 && brought[0].assigned_id == 5);
    OA_CHECK(id_text(documents, "weapons/ridge.tdf", "RGALPHA") == "5");
    OA_CHECK(id_text(documents, "weapons/ridge.tdf", "RGGUN") == "1");
}

void test_one_id_goes_to_the_earlier_mutator() {
    rules::DocumentSet documents;
    add_doc(
        documents,
        "weapons/extra.tdf",
        "[RGALPHA]\n{\nID=5;\n}\n[RGBETA]\n{\nID=5;\n}\n[RGGUN]\n{\nID=1;\n}\n"
    );
    std::vector<mutators::BroughtWeapon> brought;
    brought.push_back(brought_of("RGALPHA", 0, "weapons/extra.tdf"));
    brought.push_back(brought_of("RGBETA", 1, "weapons/extra.tdf"));
    mutators::WeaponIdResult result;
    OA_CHECK(mutators::assign_weapon_ids(documents, defs::data_layout(), brought, result));
    OA_CHECK(brought[0].assigned_id == 5);
    OA_CHECK(brought[1].assigned_id == 2);
    OA_CHECK(id_text(documents, "weapons/extra.tdf", "RGBETA") == "2");
    OA_CHECK(id_text(documents, "weapons/extra.tdf", "RGGUN") == "1");
}

void test_same_mutator_orders_by_name() {
    rules::DocumentSet documents;
    add_doc(documents, "Weapons/extra.tdf", "[ZETA]\n{\n}\n[ALPHA]\n{\n}\n");
    std::vector<mutators::BroughtWeapon> brought;
    brought.push_back(brought_of("ZETA", 0, "Weapons/extra.tdf"));
    brought.push_back(brought_of("ALPHA", 0, "Weapons/extra.tdf"));
    mutators::WeaponIdResult result;
    OA_CHECK(mutators::assign_weapon_ids(documents, defs::data_layout(), brought, result));
    OA_CHECK(brought[0].assigned_id == 2);
    OA_CHECK(brought[1].assigned_id == 1);
    OA_CHECK(brought[0].assigned_id != 0);
    OA_CHECK(id_text(documents, "Weapons/extra.tdf", "ALPHA") == "1");
    OA_CHECK(id_text(documents, "Weapons/extra.tdf", "ZETA") == "2");
}

void test_zero_and_out_of_range_are_not_requests() {
    rules::DocumentSet documents;
    add_doc(
        documents,
        "weapons/extra.tdf",
        "[RGZERO]\n{\nID=0;\n}\n[RGWIDE]\n{\nID=300;\n}\n[RGTEXT]\n{\nID=5abc;\n}\n"
        "[RGGUN]\n{\nID=1;\n}\n"
    );
    std::vector<mutators::BroughtWeapon> brought;
    brought.push_back(brought_of("RGWIDE", 0, "weapons/extra.tdf"));
    brought.push_back(brought_of("RGZERO", 0, "weapons/extra.tdf"));
    brought.push_back(brought_of("RGTEXT", 1, "weapons/extra.tdf"));
    mutators::WeaponIdResult result;
    OA_CHECK(mutators::assign_weapon_ids(documents, defs::data_layout(), brought, result));
    // RGTEXT asks for 5. The others take free ids from 1, skipping the base id
    // and skipping 0. RGWIDE is the earlier mutator and sorts first by name
    // against RGZERO, so it takes 2 and RGZERO takes 3.
    OA_CHECK(brought[2].assigned_id == 5);
    OA_CHECK(brought[0].assigned_id == 2);
    OA_CHECK(brought[1].assigned_id == 3);
    OA_CHECK(id_text(documents, "weapons/extra.tdf", "RGGUN") == "1");
    OA_CHECK(id_text(documents, "weapons/extra.tdf", "RGZERO") == "3");
}

void test_full_table_writes_nothing() {
    std::string text;
    for (int id = 1; id <= 255; ++id) {
        text += "[BASE";
        text += std::to_string(id);
        text += "]\n{\nID=";
        text += std::to_string(id);
        text += ";\n}\n";
    }
    text += "[RGMORE]\n{\nID=4;\n}\n";
    rules::DocumentSet documents;
    add_doc(documents, "weapons/full.tdf", text);
    std::vector<mutators::BroughtWeapon> brought;
    brought.push_back(brought_of("RGMORE", 3, "weapons/full.tdf"));
    mutators::WeaponIdResult result;
    OA_CHECK(!mutators::assign_weapon_ids(documents, defs::data_layout(), brought, result));
    OA_CHECK(!result.ok);
    OA_CHECK(result.mutator == 3);
    OA_CHECK(result.weapon == "RGMORE");
    OA_CHECK(brought[0].assigned_id == 0);
    OA_CHECK(id_text(documents, "weapons/full.tdf", "RGMORE") == "4");
    OA_CHECK(id_text(documents, "weapons/full.tdf", "BASE1") == "1");
    OA_CHECK(id_text(documents, "weapons/full.tdf", "BASE255") == "255");
}

} // namespace

int main() {
    test_request_is_honoured_and_base_id_stays();
    test_one_id_goes_to_the_earlier_mutator();
    test_same_mutator_orders_by_name();
    test_zero_and_out_of_range_are_not_requests();
    test_full_table_writes_nothing();
    return oa::test::check_exit_status();
}
