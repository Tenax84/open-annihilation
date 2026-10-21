// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A mutator choice's text, and the package and typed options it names.

#include "oa/data/mutators/choice.hpp"

#include "oa/formats/oamod.hpp"
#include "oa/formats/oamut/manifest.hpp"
#include "oa/test/check.hpp"
#include "oa/test/scratch_directory.hpp"

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace mutators = oa::data::mutators;
namespace oamod = oa::formats::oamod;
namespace oamut = oa::formats::oamut;

/// A scratch folder, deleted when the test ends.
class Scratch {
  public:

    Scratch() : path_(oa::test::make_scratch_directory("oa-data-mutators-choice-")) {}

    ~Scratch() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }

    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;

    [[nodiscard]] const fs::path& path() const { return path_; }

  private:

    fs::path path_;
};

/// Writes a file's bytes.
///
/// @param file the file
/// @param text its text
void write_text(const fs::path& file, std::string_view text) {
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    OA_CHECK(static_cast<bool>(out));
}

/// Reads a number from its text.
///
/// @param text the number
/// @return the number
oamod::Number number_of(std::string_view text) {
    oamod::Number number{};
    OA_CHECK(oamod::parse_number(text, number) == oamod::NumberStatus::ok);
    return number;
}

/// One numeric choice value.
///
/// @param text the number, as the manifest writes it
/// @return the value
oamut::ChoiceValue number_choice(std::string_view text) {
    oamut::ChoiceValue value;
    value.number = true;
    value.number_value = number_of(text);
    value.text = oamod::canonical_number_text(value.number_value);
    return value;
}

/// One string choice value.
///
/// @param text the identifier
/// @return the value
oamut::ChoiceValue text_choice(std::string text) {
    oamut::ChoiceValue value;
    value.text = std::move(text);
    return value;
}

/// A manifest with one toggle, one numeric choice, one string choice and one number.
///
/// @param id the mutator's id
/// @return the manifest
oamut::Manifest swift_manifest(std::string id) {
    oamut::Manifest manifest;
    manifest.format = oamut::format_version;
    manifest.id = std::move(id);
    manifest.name = "Ridge Swift";
    manifest.version.major = 1;
    manifest.version.parts = 1;
    manifest.author.name = "Ridge Maker";
    manifest.tags = {"gameplay"};
    manifest.works_with.push_back(oamut::Condition{"*", {}, {}});
    manifest.layers = {oamut::Layer::units};

    oamut::Option ready;
    ready.id = "ready";
    ready.type = oamut::OptionType::toggle;
    ready.label = "Ready";
    ready.toggle_default = false;
    manifest.options.push_back(std::move(ready));

    oamut::Option factor;
    factor.id = "factor";
    factor.type = oamut::OptionType::choice;
    factor.label = "Factor";
    factor.values = {number_choice("1.5"), number_choice("2"), number_choice("4")};
    factor.default_index = 0;
    manifest.options.push_back(std::move(factor));

    oamut::Option pace;
    pace.id = "pace";
    pace.type = oamut::OptionType::choice;
    pace.label = "Pace";
    pace.values = {text_choice("slow"), text_choice("fast")};
    pace.default_index = 0;
    manifest.options.push_back(std::move(pace));

    oamut::Option health;
    health.id = "health";
    health.type = oamut::OptionType::number;
    health.label = "Health";
    health.minimum = number_of("0");
    health.maximum = number_of("30000");
    health.step = number_of("1000");
    health.number_default = number_of("4000");
    manifest.options.push_back(std::move(health));
    return manifest;
}

/// Writes a package folder.
///
/// @param folder the folder
/// @param manifest the manifest
void write_package(const fs::path& folder, const oamut::Manifest& manifest) {
    write_text(folder / "oamut.yaml", oamut::write_manifest(manifest));
    write_text(folder / "mutator.lua", "return {}\n");
}

/// The refusal `parse_choice` reports, or empty when the text is a choice.
///
/// @param text the choice
/// @return the refusal
std::string parse_error(std::string_view text) {
    std::string error;
    const std::optional<mutators::MutatorChoice> choice = mutators::parse_choice(text, &error);
    OA_CHECK(!choice.has_value());
    return error;
}

/// Tells whether a parsed choice matches the fields a round trip must keep.
///
/// @param choice the choice
/// @param target the target
/// @param by_path whether the target is a path
/// @param options the named settings
/// @return true when every field matches
bool same_choice(
    const mutators::MutatorChoice& choice,
    std::string_view target,
    bool by_path,
    const std::vector<std::pair<std::string, std::string>>& options
) {
    return choice.target == target && choice.by_path == by_path && choice.options == options;
}

/// Parses a choice that must be accepted.
///
/// @param text the choice
/// @return the choice
mutators::MutatorChoice parsed(std::string_view text) {
    std::string error;
    const std::optional<mutators::MutatorChoice> choice = mutators::parse_choice(text, &error);
    OA_CHECK(choice.has_value());
    if (!choice)
        return {};
    OA_CHECK(error.empty());
    return *choice;
}

/// Parses a choice, writes it back and parses that again, which must give
/// the same choice and the same text.
///
/// @param text the choice
void check_round_trip(std::string_view text) {
    const mutators::MutatorChoice choice = parsed(text);
    const std::string written = mutators::format_choice(choice);
    std::string error;
    const std::optional<mutators::MutatorChoice> again = mutators::parse_choice(written, &error);
    OA_CHECK(again.has_value());
    if (!again)
        return;
    OA_CHECK(again->target == choice.target);
    OA_CHECK(again->by_path == choice.by_path);
    OA_CHECK(again->options == choice.options);
    OA_CHECK(mutators::format_choice(*again) == written);
}

/// Makes a folder the working folder until it goes out of scope.
class WorkingFolder {
  public:

    explicit WorkingFolder(const fs::path& folder) : previous_(fs::current_path()) {
        fs::current_path(folder);
    }

    ~WorkingFolder() {
        std::error_code ignored;
        fs::current_path(previous_, ignored);
    }

    WorkingFolder(const WorkingFolder&) = delete;
    WorkingFolder& operator=(const WorkingFolder&) = delete;

  private:

    fs::path previous_;
};

void test_parse() {
    const mutators::MutatorChoice id = parsed("ridge-swift");
    OA_CHECK(same_choice(id, "ridge-swift", false, {}));

    const mutators::MutatorChoice relative = parsed("packages/ridge-tough");
    OA_CHECK(same_choice(relative, "packages/ridge-tough", true, {}));

    const mutators::MutatorChoice relative_option = parsed("./pack:factor=4");
    OA_CHECK(same_choice(relative_option, "./pack", true, {{"factor", "4"}}));

    const mutators::MutatorChoice absolute = parsed("/tmp/ridge-swift");
    OA_CHECK(same_choice(absolute, "/tmp/ridge-swift", true, {}));

    // A drive letter's colon is part of the path on every platform, whether
    // or not the folder is there, and the options follow a later colon.
    const mutators::MutatorChoice drive_choice = parsed("C:\\pkg");
    OA_CHECK(same_choice(drive_choice, "C:\\pkg", true, {}));

    const mutators::MutatorChoice drive_option = parsed("C:\\pkg:factor=4");
    OA_CHECK(same_choice(drive_option, "C:\\pkg", true, {{"factor", "4"}}));

    const mutators::MutatorChoice several = parsed("ridge-swift:factor=4,health=2000");
    OA_CHECK(same_choice(several, "ridge-swift", false, {{"factor", "4"}, {"health", "2000"}}));

    const mutators::MutatorChoice drive_several = parsed("C:\\pkg:factor=4,health=2000");
    OA_CHECK(same_choice(drive_several, "C:\\pkg", true, {{"factor", "4"}, {"health", "2000"}}));

    const mutators::MutatorChoice forward = parsed("C:/Mods/pack");
    OA_CHECK(same_choice(forward, "C:/Mods/pack", true, {}));

    const mutators::MutatorChoice lower = parsed("d:\\mods\\pack:pace=fast");
    OA_CHECK(same_choice(lower, "d:\\mods\\pack", true, {{"pace", "fast"}}));

    const mutators::MutatorChoice equals_choice = parsed("C:\\a=b\\pkg");
    OA_CHECK(same_choice(equals_choice, "C:\\a=b\\pkg", true, {}));

    // With no slash after it, a letter and a colon are an id and its options.
    const mutators::MutatorChoice letter = parsed("c:factor=4");
    OA_CHECK(same_choice(letter, "c", false, {{"factor", "4"}}));

    OA_CHECK(parse_error("") == "a mutator needs an id or a path");
    OA_CHECK(parse_error(":factor=4") == "a mutator needs an id or a path");
    OA_CHECK(parse_error("ridge-swift:factor=") == "an option needs a value");
    OA_CHECK(parse_error("ridge-swift:=4") == "an option needs a name");
    OA_CHECK(parse_error("ridge-swift:health=1,health=2") == "option health is repeated");
    OA_CHECK(parse_error("ridge-swift:factor=4,") == "an option is not name=value");
    OA_CHECK(parse_error("ridge-swift:factor=4,,health=2") == "an option is not name=value");
    OA_CHECK(parse_error("ridge-swift:a=b=c") == "an option is not name=value");
    OA_CHECK(parse_error("ridge-swift:factor=4,health") == "an option is not name=value");
    OA_CHECK(parse_error("ridge-swift:factor=4 5") == "an option is not name=value");
    OA_CHECK(parse_error("C:\\pkg:factor=") == "an option needs a value");

    // Any other colon in a target needs the entry to be there.
    OA_CHECK(parse_error("ridge-tough:") == "\"ridge-tough:\" is not an id or a path");
    OA_CHECK(parse_error("ridge:tough") == "\"ridge:tough\" is not an id or a path");
    OA_CHECK(
        parse_error("C:\\ridge-nothere\\pkg:") ==
        "\"C:\\ridge-nothere\\pkg:\" is not an id or a path"
    );
    OA_CHECK(
        parse_error("C:\\ridge-nothere\\a:b:factor=4") ==
        "\"C:\\ridge-nothere\\a:b\" is not an id or a path"
    );
}

void test_round_trip() {
    const std::string_view texts[] = {
        "ridge-swift",
        "packages/ridge-tough",
        "/tmp/ridge-swift",
        "C:\\pkg",
        "C:\\pkg:factor=4",
        "C:\\pkg:factor=4,health=2000",
        "C:/Mods/pack:pace=fast",
        "ridge-swift:ready=true,factor=2.0,pace=fast,health=2000",
    };
    for (const std::string_view text : texts)
        check_round_trip(text);
}

#if !defined(_WIN32)
/// A folder whose own name holds a colon, which a Windows file system does
/// not allow.
///
/// @param scratch the test's folder
/// @param name the folder's own name
/// @return the folder
fs::path colon_folder(const Scratch& scratch, std::string_view name) {
    const fs::path folder = scratch.path() / std::string(name);
    std::error_code failure;
    fs::create_directories(folder, failure);
    OA_CHECK(!failure);
    return folder;
}

void test_colon_folders(const Scratch& scratch) {
    // A colon other than a drive's keeps the target a path when the entry is
    // there, and the options follow a later colon.
    const std::string folder = colon_folder(scratch, "ridge:pkg").string();
    OA_CHECK(same_choice(parsed(folder), folder, true, {}));
    OA_CHECK(same_choice(parsed(folder + ":factor=4"), folder, true, {{"factor", "4"}}));
    OA_CHECK(same_choice(
        parsed(folder + ":factor=4,health=2000"),
        folder,
        true,
        {{"factor", "4"}, {"health", "2000"}}
    ));
    check_round_trip(folder);
    check_round_trip(folder + ":factor=4");
    OA_CHECK(parse_error(folder + "-gone") == "\"" + folder + "-gone\" is not an id or a path");
}
#endif

/// The option of an id, or null.
///
/// @param resolved the package
/// @param id the option's id
/// @return the option
const mutators::LoadoutOption*
option_of(const mutators::ResolvedMutator& resolved, std::string_view id) {
    for (const mutators::LoadoutOption& option : resolved.options) {
        if (option.id == id)
            return &option;
    }
    return nullptr;
}

/// Resolves a choice that must be accepted.
///
/// @param text the choice
/// @param mutators the Mutators folder
/// @return the package
mutators::ResolvedMutator resolved_text(std::string_view text, const fs::path& mutators) {
    const mutators::MutatorChoice choice = parsed(text);
    std::string error;
    const std::optional<mutators::ResolvedMutator> resolved =
        mutators::resolve_choice(choice, mutators, &error);
    OA_CHECK(resolved.has_value());
    if (!resolved)
        return {};
    return *resolved;
}

/// The refusal `resolve_choice` reports.
///
/// @param text the choice
/// @param mutators the Mutators folder
/// @return the refusal
std::string resolve_error(std::string_view text, const fs::path& mutators) {
    const mutators::MutatorChoice choice = parsed(text);
    std::string error;
    const std::optional<mutators::ResolvedMutator> resolved =
        mutators::resolve_choice(choice, mutators, &error);
    OA_CHECK(!resolved.has_value());
    return error;
}

void test_resolve(const Scratch& scratch) {
    const fs::path mutators = scratch.path() / "Mutators";
    write_package(mutators / "ridge-swift", swift_manifest("ridge-swift"));
    const fs::path by_path = scratch.path() / "loose" / "ridge-swift";
    write_package(by_path, swift_manifest("ridge-swift"));
    write_text(
        scratch.path() / "cased" / "OAMUT.YAML",
        oamut::write_manifest(swift_manifest("ridge-cased"))
    );
    write_text(scratch.path() / "cased" / "mutator.lua", "return {}\n");

    const mutators::ResolvedMutator installed = resolved_text("ridge-swift", mutators);
    OA_CHECK(installed.manifest.id == "ridge-swift");
    OA_CHECK(installed.package_folder == mutators / "ridge-swift");
    OA_CHECK(installed.options.size() == installed.manifest.options.size());
    for (std::size_t index = 0; index < installed.options.size(); ++index)
        OA_CHECK(installed.options[index].id == installed.manifest.options[index].id);
    const mutators::LoadoutOption* ready = option_of(installed, "ready");
    const mutators::LoadoutOption* factor = option_of(installed, "factor");
    const mutators::LoadoutOption* pace = option_of(installed, "pace");
    const mutators::LoadoutOption* health = option_of(installed, "health");
    OA_CHECK(ready != nullptr && ready->type == oamut::OptionType::toggle && !ready->toggle);
    OA_CHECK(factor != nullptr && factor->value.text == "1.5");
    OA_CHECK(pace != nullptr && pace->value.text == "slow");
    OA_CHECK(
        health != nullptr && health->value.number &&
        oamod::compare_numbers(health->value.number_value, number_of("4000")) == 0
    );

    const mutators::ResolvedMutator named =
        resolved_text("ridge-swift:ready=true,factor=2.0,pace=fast,health=2000", mutators);
    const mutators::LoadoutOption* named_ready = option_of(named, "ready");
    const mutators::LoadoutOption* named_factor = option_of(named, "factor");
    const mutators::LoadoutOption* named_pace = option_of(named, "pace");
    const mutators::LoadoutOption* named_health = option_of(named, "health");
    OA_CHECK(named_ready != nullptr && named_ready->toggle);
    OA_CHECK(named_factor != nullptr && named_factor->value.text == "2");
    OA_CHECK(
        named_factor != nullptr &&
        oamod::compare_numbers(named_factor->value.number_value, number_of("2")) == 0
    );
    OA_CHECK(
        named_pace != nullptr && named_pace->value.text == "fast" && !named_pace->value.number
    );
    OA_CHECK(named_health != nullptr && named_health->value.text == "2000");

    {
        // A relative path is read from the working folder. The scratch folder
        // is the working folder here, so the path is relative on every
        // platform, even where the temporary folder is on another drive.
        const WorkingFolder working(scratch.path());
        const mutators::ResolvedMutator from_relative =
            resolved_text("loose/ridge-swift", mutators);
        OA_CHECK(from_relative.manifest.id == "ridge-swift");
        OA_CHECK(from_relative.choice.by_path);
        const mutators::ResolvedMutator dotted =
            resolved_text("./loose/ridge-swift:factor=4", mutators);
        OA_CHECK(dotted.manifest.id == "ridge-swift");
        const mutators::LoadoutOption* dotted_factor = option_of(dotted, "factor");
        OA_CHECK(dotted_factor != nullptr && dotted_factor->value.text == "4");

        // A drive path is a path whether or not it is there, and one that
        // holds no package is refused when it is resolved.
        OA_CHECK(resolve_error("C:\\pkg", mutators) == "\"C:\\pkg\" is not a mutator package");
        OA_CHECK(
            resolve_error("C:\\pkg:factor=4", mutators) == "\"C:\\pkg\" is not a mutator package"
        );
    }

    const mutators::ResolvedMutator from_absolute =
        resolved_text(fs::absolute(by_path).string(), mutators);
    OA_CHECK(from_absolute.manifest.id == "ridge-swift");
    OA_CHECK(
        option_of(resolved_text(fs::absolute(by_path).string() + ":health=0", mutators), "health")
            ->value.text == "0"
    );
    OA_CHECK(
        option_of(
            resolved_text(fs::absolute(by_path).string() + ":health=30000", mutators), "health"
        )
            ->value.text == "30000"
    );

    const mutators::ResolvedMutator cased =
        resolved_text(fs::absolute(scratch.path() / "cased").string(), mutators);
    OA_CHECK(cased.manifest.id == "ridge-cased");

    OA_CHECK(
        resolve_error("ridge-nothere", mutators) == "\"ridge-nothere\" is not an installed mutator"
    );
    OA_CHECK(resolve_error("ridge-swift:speed=2", mutators) == "ridge-swift has no option speed");
    OA_CHECK(
        resolve_error("ridge-swift:factor=9", mutators) ==
        "ridge-swift: factor: 9 is not one of 1.5, 2, 4"
    );
    OA_CHECK(
        resolve_error("ridge-swift:factor=true", mutators) ==
        "ridge-swift: factor: true is not one of 1.5, 2, 4"
    );
    OA_CHECK(
        resolve_error("ridge-swift:pace=Fast", mutators) ==
        "ridge-swift: pace: Fast is not one of slow, fast"
    );
    OA_CHECK(
        resolve_error("ridge-swift:ready=TRUE", mutators) ==
        "ridge-swift: ready: TRUE is not true or false"
    );
    OA_CHECK(
        resolve_error("ridge-swift:ready=1", mutators) ==
        "ridge-swift: ready: 1 is not true or false"
    );
    OA_CHECK(
        resolve_error("ridge-swift:health=fast", mutators) ==
        "ridge-swift: health: fast is not a number"
    );
    OA_CHECK(
        resolve_error("ridge-swift:health=30001", mutators) ==
        "ridge-swift: health: 30001 is outside 0 to 30000"
    );
    OA_CHECK(
        resolve_error("ridge-swift:health=-1", mutators) ==
        "ridge-swift: health: -1 is outside 0 to 30000"
    );
    OA_CHECK(
        resolve_error("ridge-swift:health=1234", mutators) ==
        "ridge-swift: health: 1234 does not lie on a step of 1000 from 0"
    );
    OA_CHECK(
        resolve_error("ridge-swift:health=500", mutators) ==
        "ridge-swift: health: 500 does not lie on a step of 1000 from 0"
    );

    const std::string missing = (scratch.path() / "no-such-package").string();
    OA_CHECK(resolve_error(missing, mutators) == "\"" + missing + "\" is not a mutator package");
    OA_CHECK(!option_of(resolved_text("ridge-swift:ready=false", mutators), "ready")->toggle);
}

} // namespace

int main() {
    const Scratch scratch;
    test_parse();
    test_round_trip();
#if !defined(_WIN32)
    test_colon_folders(scratch);
#endif
    test_resolve(scratch);
    return oa::test::check_exit_status();
}
