// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Clash mode, the private name an added record takes, and the comparison
// that says two weapon sections are the same weapon.
#include "oa/data/mutators/clashes.hpp"

#include "oa/formats/tdf.hpp"
#include "oa/formats/tdf/edit.hpp"
#include "oa/test/check.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace {

namespace mutators = oa::data::mutators;
namespace oamut = oa::formats::oamut;
namespace tdf = oa::formats::tdf;

const tdf::Block* first_section(const tdf::OwnedDocument& document) {
    const tdf::Block* root = document.root();
    OA_CHECK(root != nullptr);
    if (root == nullptr)
        return nullptr;
    return tdf::child_at(root, 0);
}

void test_effective_mode() {
    using Mode = oamut::ClashMode;
    OA_CHECK(mutators::effective_clash(Mode::replace, Mode::add, Mode::add) == Mode::replace);
    OA_CHECK(mutators::effective_clash(Mode::add, Mode::replace, Mode::replace) == Mode::add);
    OA_CHECK(mutators::effective_clash(std::nullopt, Mode::add, Mode::replace) == Mode::add);
    OA_CHECK(mutators::effective_clash(std::nullopt, Mode::replace, Mode::add) == Mode::replace);
    OA_CHECK(mutators::effective_clash(std::nullopt, std::nullopt, Mode::add) == Mode::add);
    OA_CHECK(mutators::effective_clash(std::nullopt, std::nullopt, Mode::replace) == Mode::replace);
    OA_CHECK(mutators::clash_from_setting("add") == Mode::add);
    OA_CHECK(mutators::clash_from_setting("replace") == Mode::replace);
    OA_CHECK(mutators::clash_from_setting("Add") == Mode::replace);
    OA_CHECK(mutators::clash_from_setting("") == Mode::replace);
    OA_CHECK(mutators::clash_setting_default == Mode::replace);
    OA_CHECK(mutators::clash_default_preference == "mutators.clash-default");
}

void test_record_names() {
    OA_CHECK(!mutators::record_name_ok(""));
    OA_CHECK(!mutators::record_name_ok(std::string(32, 'A')));
    OA_CHECK(mutators::record_name_ok(std::string(31, 'A')));
    OA_CHECK(!mutators::record_name_ok("A/B"));
    OA_CHECK(!mutators::record_name_ok("A\\B"));
    OA_CHECK(!mutators::record_name_ok("A:B"));
    OA_CHECK(!mutators::record_name_ok("A.B"));
    OA_CHECK(mutators::record_name_ok("A B"));

    const std::string long_name(31, 'A');
    const std::optional<std::string> first =
        mutators::private_record_name(long_name, [](std::string_view) { return false; });
    OA_CHECK(first.has_value());
    if (first.has_value()) {
        OA_CHECK(first->size() == 31);
        OA_CHECK(*first == std::string(29, 'A') + "_2");
    }

    const std::optional<std::string> skipped =
        mutators::private_record_name(long_name, [](std::string_view candidate) {
            return candidate == std::string(29, 'A') + "_2";
        });
    OA_CHECK(skipped.has_value() && *skipped == std::string(29, 'A') + "_3");

    const std::optional<std::string> grown =
        mutators::private_record_name(long_name, [](std::string_view candidate) {
            return candidate.size() == 31 && candidate.size() > 29 && candidate[29] == '_';
        });
    OA_CHECK(grown.has_value() && *grown == std::string(28, 'A') + "_10");

    OA_CHECK(mutators::weapon_reference_key("Weapon1"));
    OA_CHECK(mutators::weapon_reference_key("explodeas"));
    OA_CHECK(mutators::weapon_reference_key("SelfDestructAs"));
    OA_CHECK(!mutators::weapon_reference_key("weapon4"));
    OA_CHECK(mutators::unit_list_key("canbuild"));
    OA_CHECK(mutators::unit_list_key("canbuild12"));
    OA_CHECK(mutators::unit_list_key("Commander"));
    OA_CHECK(!mutators::unit_list_key("canbuildX"));
}

void test_identical_weapons_ignore_id() {
    tdf::OwnedDocument left;
    tdf::OwnedDocument right;
    tdf::OwnedDocument different;
    OA_CHECK(left.parse("[SPARK]\n{\nID=1;\nRange=10;\n}\n", nullptr));
    OA_CHECK(right.parse("[SPARK]\n{\nRange=10;\nID=9;\n}\n", nullptr));
    OA_CHECK(different.parse("[SPARK]\n{\nID=1;\nRange=20;\n}\n", nullptr));
    const tdf::Block* left_section = first_section(left);
    const tdf::Block* right_section = first_section(right);
    const tdf::Block* different_section = first_section(different);
    OA_CHECK(left_section != nullptr && right_section != nullptr && different_section != nullptr);
    OA_CHECK(mutators::records_identical(left_section, right_section, true));
    OA_CHECK(!mutators::records_identical(left_section, right_section, false));
    OA_CHECK(!mutators::records_identical(left_section, different_section, true));
    OA_CHECK(mutators::records_identical(nullptr, nullptr, true));
    OA_CHECK(!mutators::records_identical(left_section, nullptr, true));

    tdf::OwnedDocument lower_key;
    tdf::OwnedDocument other_case;
    OA_CHECK(lower_key.parse("[UNITINFO]\n{\nname=Ridge bot;\n}\n", nullptr));
    OA_CHECK(other_case.parse("[UNITINFO]\n{\nName=Ridge Bot;\n}\n", nullptr));
    tdf::OwnedDocument display;
    OA_CHECK(display.parse("[UNITINFO]\n{\nName=Ridge bot;\n}\n", nullptr));
    const tdf::Block* display_section = first_section(display);
    const tdf::Block* lower_section = first_section(lower_key);
    const tdf::Block* other_section = first_section(other_case);
    OA_CHECK(display_section != nullptr && lower_section != nullptr && other_section != nullptr);
    OA_CHECK(mutators::records_identical(display_section, lower_section, false));
    OA_CHECK(!mutators::records_identical(display_section, other_section, false));
}

} // namespace

int main() {
    test_effective_mode();
    test_record_names();
    test_identical_weapons_ignore_id();
    return oa::test::check_exit_status();
}
