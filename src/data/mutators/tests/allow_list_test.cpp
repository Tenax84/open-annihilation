// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The mutable-path allow-list: every allowed and refused form, a layout
// that renames directories, every invalid path, and the masks.

#include "oa/data/defs/layout.hpp"
#include "oa/data/mutators/allow_list.hpp"
#include "oa/test/check.hpp"

#include <cstdio>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace mutators = oa::data::mutators;
namespace defs = oa::data::defs;

struct Expectation {
    const char* path = nullptr;
    mutators::PathRule rule = mutators::PathRule::unknown_folder;
};

/// True when the name is the Ridge factory, in either case.
///
/// @param name the unit name from the path
/// @return true for ridgefab
bool is_ridge_factory(std::string_view name) {
    constexpr std::string_view kUnit = "ridgefab";
    if (name.size() != kUnit.size())
        return false;
    for (std::size_t index = 0; index < name.size(); ++index) {
        unsigned char byte = static_cast<unsigned char>(name[index]);
        if (byte >= 'A' && byte <= 'Z')
            byte = static_cast<unsigned char>(byte - 'A' + 'a');
        if (static_cast<char>(byte) != kUnit[index])
            return false;
    }
    return true;
}

/// Checks one path or mask, including that the message starts with it.
///
/// @param path the path or pattern
/// @param rule the expected rule
/// @param context the layout and the unit-name test
/// @param mask true to judge a mask
void expect_rule(
    std::string_view path, mutators::PathRule rule, const mutators::PathContext& context, bool mask
) {
    const mutators::PathVerdict verdict = mask ? mutators::check_mutable_mask(path, context)
                                               : mutators::check_mutable_path(path, context);
    if (verdict.rule != rule || verdict.allowed() != (rule == mutators::PathRule::allowed)) {
        const int length = static_cast<int>(path.size());
        std::fprintf(
            stderr,
            "rule for '%.*s': got %.*s wanted %.*s (%s)\n",
            length,
            path.data(),
            static_cast<int>(mutators::rule_name(verdict.rule).size()),
            mutators::rule_name(verdict.rule).data(),
            static_cast<int>(mutators::rule_name(rule).size()),
            mutators::rule_name(rule).data(),
            verdict.message.c_str()
        );
    }
    OA_CHECK(verdict.rule == rule);
    OA_CHECK(verdict.allowed() == (rule == mutators::PathRule::allowed));
    OA_CHECK(verdict.message.size() >= path.size() + 2);
    OA_CHECK(verdict.message.compare(0, path.size(), path) == 0);
    OA_CHECK(verdict.message[path.size()] == ':');
}

/// Checks a table of paths.
///
/// @param cases the paths and the rules they must get
/// @param context the layout and the unit-name test
/// @param mask true to judge masks
void expect_all(
    const std::vector<Expectation>& cases, const mutators::PathContext& context, bool mask
) {
    for (const Expectation& item : cases)
        expect_rule(item.path, item.rule, context, mask);
}

/// Checks that a message contains a sentence.
///
/// @param path the path or pattern
/// @param sentence the words that must follow the path
/// @param context the layout and the unit-name test
/// @param mask true to judge a mask
void expect_sentence(
    std::string_view path,
    std::string_view sentence,
    const mutators::PathContext& context,
    bool mask
) {
    const mutators::PathVerdict verdict = mask ? mutators::check_mutable_mask(path, context)
                                               : mutators::check_mutable_path(path, context);
    const std::string prefix = std::string{path} + ": " + std::string{sentence};
    if (verdict.message != prefix) {
        std::fprintf(
            stderr, "message '%s'\n wanted '%s'\n", verdict.message.c_str(), prefix.c_str()
        );
    }
    OA_CHECK(verdict.message == prefix);
}

} // namespace

int main() {
    const defs::DataLayout base;
    const mutators::PathContext plain{&base, {}};
    const mutators::PathContext ridge{&base, is_ridge_factory};

    const std::vector<Expectation> allowed = {
        {"units/RIDGEA.fbi", mutators::PathRule::allowed},
        {"UNITS\\RIDGEA.FBI", mutators::PathRule::allowed},
        {"units-German/RIDGEA.FBI", mutators::PathRule::allowed},
        {"Units-German\\RIDGEA.FBI", mutators::PathRule::allowed},
        {"units/extra/RIDGEA.fbi", mutators::PathRule::allowed},
        {"Weapons/LASER.tdf", mutators::PathRule::allowed},
        {"weapons\\laser.TDF", mutators::PathRule::allowed},
        {"Weapons-extra/LASER.tdf", mutators::PathRule::allowed},
        {"ai/ridge.txt", mutators::PathRule::allowed},
        {"AI\\RIDGE.TXT", mutators::PathRule::allowed},
        {"unitpics/RIDGEA.pcx", mutators::PathRule::allowed},
        {"download/ridge.tdf", mutators::PathRule::allowed},
        {"features/rocks/rock.tdf", mutators::PathRule::allowed},
        {"FEATURES\\rock.tdf", mutators::PathRule::allowed},
        {"features-ice/rock.tdf", mutators::PathRule::allowed},
        {"objects3d/ridge.3do", mutators::PathRule::allowed},
        {"scripts/ridge.cob", mutators::PathRule::allowed},
        {"SCRIPTS\\RIDGE.BOS", mutators::PathRule::allowed},
        {"gamedata/MOVEINFO.TDF", mutators::PathRule::allowed},
        {"gamedata\\moveinfo.tdf", mutators::PathRule::allowed},
        {"gamedata/sidedata.tdf", mutators::PathRule::allowed},
        {"gamedata/LOS.TDF", mutators::PathRule::allowed},
        {"gamedata\\los.tdf", mutators::PathRule::allowed},
        {"gamedata/meteor.tdf", mutators::PathRule::allowed},
        {"GAMEDATA\\METEOR.TDF", mutators::PathRule::allowed},
        {"gamedata-German/los.tdf", mutators::PathRule::allowed},
        {"anims/ridgefab1.gaf", mutators::PathRule::allowed},
        {"anims\\ridgefab1.gaf", mutators::PathRule::allowed},
        {"anims/RIDGEFAB12.GAF", mutators::PathRule::allowed},
        {"Anims-German/ridgefab1.gaf", mutators::PathRule::allowed},
        {"anims/RIDGEFAB_gadget.gaf", mutators::PathRule::allowed},
        {"ANIMS\\ridgefab_GADGET.GAF", mutators::PathRule::allowed},
        {"Anims-German/ridgefab_gadget.gaf", mutators::PathRule::allowed},
    };
    expect_all(allowed, ridge, false);
    // A download button is refused when the unit is unknown, the name is
    // empty, the file is not one level down, or the suffix is not exactly
    // _gadget.gaf.
    expect_all(
        {{"anims/ridgejet_gadget.gaf", mutators::PathRule::interface_art},
         {"anims/_gadget.gaf", mutators::PathRule::interface_art},
         {"anims/x/ridgefab_gadget.gaf", mutators::PathRule::interface_art},
         {"anims/ridgefab_gadget1.gaf", mutators::PathRule::interface_art},
         {"anims/ridgefab_gadget.gaf.bak", mutators::PathRule::interface_art},
         {"anims/ridgefab_gadgets.gaf", mutators::PathRule::interface_art}},
        ridge,
        false
    );

    const std::vector<Expectation> refused = {
        {"guis/RIDGEFAB1.GUI", mutators::PathRule::menus_only},
        {"GUIS\\MAINMENU.GUI", mutators::PathRule::menus_only},
        {"guis-German/RIDGEFAB1.GUI", mutators::PathRule::menus_only},
        {"maps/ridge.tnt", mutators::PathRule::maps_only},
        {"MAPS\\ridge.ota", mutators::PathRule::maps_only},
        {"gamedata/sound.tdf", mutators::PathRule::read_once},
        {"gamedata\\SOUND.TDF", mutators::PathRule::read_once},
        {"gamedata/translate.tdf", mutators::PathRule::not_confirmed},
        {"gamedata/version.tdf", mutators::PathRule::not_confirmed},
        {"gamedata/extra/los.tdf", mutators::PathRule::not_confirmed},
        {"textures/ridge.dds", mutators::PathRule::not_confirmed},
        {"TEXTURES\\ridge.dds", mutators::PathRule::not_confirmed},
        {"sounds/ridge.wav", mutators::PathRule::read_once},
        {"SOUNDS\\RIDGE.WAV", mutators::PathRule::read_once},
        {"sounds-German/ridge.wav", mutators::PathRule::read_once},
        {"music/ridge.wav", mutators::PathRule::read_once},
        {"palettes/palette.pal", mutators::PathRule::read_once},
        {"fonts/comix.fnt", mutators::PathRule::read_once},
        {"bitmaps/ridge.bmp", mutators::PathRule::read_once},
        {"camps/ridge.txt", mutators::PathRule::read_once},
        {"anims/ridgefab1.gaf", mutators::PathRule::interface_art},
        {"anims/cursor.gaf", mutators::PathRule::interface_art},
        {"anims/ridgefab.gaf", mutators::PathRule::interface_art},
        {"anims/9.gaf", mutators::PathRule::interface_art},
        {"anims/ridgefab1.txt", mutators::PathRule::interface_art},
        {"anims/extra/ridgefab1.gaf", mutators::PathRule::interface_art},
        {"readme.txt", mutators::PathRule::unknown_folder},
        {"extras/readme.txt", mutators::PathRule::unknown_folder},
        {"units", mutators::PathRule::unknown_folder},
    };
    // The build picture is refused when the caller knows no unit of that name.
    expect_all(
        {{"anims/ridgefab1.gaf", mutators::PathRule::interface_art},
         {"anims/armlab1.gaf", mutators::PathRule::interface_art}},
        plain,
        false
    );
    expect_all(refused, plain, false);
    expect_sentence(
        "guis/RIDGEFAB1.GUI",
        "build pages and screens change only through a mutator's menus",
        plain,
        false
    );
    expect_sentence(
        "maps/ridge.tnt", "maps change only through a mutator's maps hook", plain, false
    );
    expect_sentence(
        "gamedata/translate.tdf", "not yet shown to be read again for every match", plain, false
    );
    expect_sentence("sounds/ridge.wav", "this is loaded once when the game starts", plain, false);

    defs::DataLayout renamed = base;
    renamed.directories[static_cast<std::size_t>(defs::DataDirectory::units)] = "machines";
    renamed.directories[static_cast<std::size_t>(defs::DataDirectory::weapons)] = "arms";
    renamed.directories[static_cast<std::size_t>(defs::DataDirectory::gamedata)] = "tables";
    renamed.directories[static_cast<std::size_t>(defs::DataDirectory::guis)] = "screens";
    const mutators::PathContext renamed_context{&renamed, is_ridge_factory};
    const std::vector<Expectation> renamed_paths = {
        {"machines/RIDGEA.fbi", mutators::PathRule::allowed},
        {"machines-German/RIDGEA.fbi", mutators::PathRule::allowed},
        {"units/RIDGEA.fbi", mutators::PathRule::unknown_folder},
        {"units-German/RIDGEA.fbi", mutators::PathRule::unknown_folder},
        {"arms/LASER.tdf", mutators::PathRule::allowed},
        {"Weapons/LASER.tdf", mutators::PathRule::unknown_folder},
        {"tables/los.tdf", mutators::PathRule::allowed},
        {"tables/sound.tdf", mutators::PathRule::read_once},
        {"tables/translate.tdf", mutators::PathRule::not_confirmed},
        {"gamedata/los.tdf", mutators::PathRule::unknown_folder},
        {"screens/RIDGEFAB1.GUI", mutators::PathRule::menus_only},
        {"guis/RIDGEFAB1.GUI", mutators::PathRule::unknown_folder},
    };
    expect_all(renamed_paths, renamed_context, false);

    // A null layout is the layout in use, which a test leaves at the base game's.
    expect_rule("units/RIDGEA.fbi", mutators::PathRule::allowed, {}, false);
    expect_rule("machines/RIDGEA.fbi", mutators::PathRule::unknown_folder, {}, false);

    const std::vector<Expectation> invalid = {
        {"", mutators::PathRule::invalid_path},
        {"/units/RIDGEA.fbi", mutators::PathRule::invalid_path},
        {"\\units\\RIDGEA.fbi", mutators::PathRule::invalid_path},
        {"C:/units/RIDGEA.fbi", mutators::PathRule::invalid_path},
        {"C:units/RIDGEA.fbi", mutators::PathRule::invalid_path},
        {"units/../RIDGEA.fbi", mutators::PathRule::invalid_path},
        {"units/./RIDGEA.fbi", mutators::PathRule::invalid_path},
        {"../units/RIDGEA.fbi", mutators::PathRule::invalid_path},
        {"./units/RIDGEA.fbi", mutators::PathRule::invalid_path},
        {"..", mutators::PathRule::invalid_path},
        {".", mutators::PathRule::invalid_path},
        {"units/", mutators::PathRule::invalid_path},
        {"units//RIDGEA.fbi", mutators::PathRule::invalid_path},
    };
    expect_all(invalid, plain, false);
    expect_sentence("", "the path is empty", plain, false);
    expect_sentence("/units/RIDGEA.fbi", "the path is absolute", plain, false);
    expect_sentence("C:/units/RIDGEA.fbi", "the path is drive-qualified", plain, false);
    expect_sentence("units/../RIDGEA.fbi", "the path contains a '.' or '..' segment", plain, false);

    std::string nul_path = "units/RIDGE";
    nul_path.push_back('\0');
    nul_path += "A.fbi";
    expect_rule(nul_path, mutators::PathRule::invalid_path, plain, false);
    expect_sentence(nul_path, "the path contains a NUL", plain, false);

    expect_rule(std::string(4096, 'a'), mutators::PathRule::unknown_folder, plain, false);
    expect_rule(std::string(4097, 'a'), mutators::PathRule::invalid_path, plain, false);
    expect_sentence(std::string(4097, 'b'), "the path is over-long", plain, false);

    const std::vector<Expectation> masks = {
        {"units/*.fbi", mutators::PathRule::allowed},
        {"UNITS\\*.FBI", mutators::PathRule::allowed},
        {"units-German/*.fbi", mutators::PathRule::allowed},
        {"ai/**", mutators::PathRule::allowed},
        {"features/**/rock*.tdf", mutators::PathRule::allowed},
        {"weapons/laser?.tdf", mutators::PathRule::allowed},
        {"gamedata/*.tdf", mutators::PathRule::not_confirmed},
        {"anims/*.gaf", mutators::PathRule::interface_art},
        {"anims/*_gadget.gaf", mutators::PathRule::interface_art},
        {"anims/RIDGEFAB_gadget.gaf", mutators::PathRule::allowed},
        {"**/x.fbi", mutators::PathRule::unknown_folder},
        {"*.fbi", mutators::PathRule::unknown_folder},
        {"guis/**", mutators::PathRule::menus_only},
        {"../units/*", mutators::PathRule::invalid_path},
        {"units/../x.fbi", mutators::PathRule::invalid_path},
        {"gamedata/los.tdf", mutators::PathRule::allowed},
        {"gamedata/sound.tdf", mutators::PathRule::read_once},
        {"sounds/*.wav", mutators::PathRule::read_once},
        {"maps/*", mutators::PathRule::maps_only},
        {"textures/**", mutators::PathRule::not_confirmed},
        {"readme.txt", mutators::PathRule::unknown_folder},
        {"", mutators::PathRule::invalid_path},
    };
    expect_all(masks, ridge, true);
    expect_sentence("gamedata/*.tdf", "a mask here must name one file", plain, true);
    expect_sentence("anims/*.gaf", "a mask here must name one file", plain, true);
    expect_sentence(
        "guis/**", "build pages and screens change only through a mutator's menus", plain, true
    );

    const std::vector<Expectation> renamed_masks = {
        {"machines/*.fbi", mutators::PathRule::allowed},
        {"units/*.fbi", mutators::PathRule::unknown_folder},
        {"arms/**", mutators::PathRule::allowed},
        {"Weapons/*.tdf", mutators::PathRule::unknown_folder},
        {"tables/*.tdf", mutators::PathRule::not_confirmed},
        {"screens/**", mutators::PathRule::menus_only},
    };
    expect_all(renamed_masks, renamed_context, true);

    constexpr mutators::PathRule kRules[] = {
        mutators::PathRule::allowed,
        mutators::PathRule::invalid_path,
        mutators::PathRule::menus_only,
        mutators::PathRule::maps_only,
        mutators::PathRule::read_once,
        mutators::PathRule::not_confirmed,
        mutators::PathRule::interface_art,
        mutators::PathRule::unknown_folder,
    };
    OA_CHECK(mutators::rule_name(mutators::PathRule::allowed) == "allowed");
    OA_CHECK(mutators::rule_name(mutators::PathRule::invalid_path) == "invalid-path");
    OA_CHECK(mutators::rule_name(mutators::PathRule::menus_only) == "menus-only");
    OA_CHECK(mutators::rule_name(mutators::PathRule::maps_only) == "maps-only");
    OA_CHECK(mutators::rule_name(mutators::PathRule::read_once) == "read-once");
    OA_CHECK(mutators::rule_name(mutators::PathRule::not_confirmed) == "not-confirmed");
    OA_CHECK(mutators::rule_name(mutators::PathRule::interface_art) == "interface-art");
    OA_CHECK(mutators::rule_name(mutators::PathRule::unknown_folder) == "unknown-folder");
    for (std::size_t left = 0; left < std::size(kRules); ++left) {
        OA_CHECK(!mutators::rule_name(kRules[left]).empty());
        for (std::size_t right = left + 1; right < std::size(kRules); ++right)
            OA_CHECK(mutators::rule_name(kRules[left]) != mutators::rule_name(kRules[right]));
    }

    return oa::test::check_exit_status();
}
