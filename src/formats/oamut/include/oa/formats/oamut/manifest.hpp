// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A mutator's manifest, oamut.yaml. The installer, the catalogue and the
// Library read one description of a mutator without running any of its code.
#pragma once

#include "oa/formats/oamod/package_keys.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::formats::oamut {

/// The manifest format this build reads: the manifest's oamut value.
inline constexpr int64_t format_version = 1;

/// The name of a mutator's manifest.
inline constexpr std::string_view manifest_file = "oamut.yaml";

/// The entry script a manifest uses when it names none.
inline constexpr std::string_view default_entry = "mutator.lua";

/// The most bytes of a mutator id.
inline constexpr std::size_t most_id_bytes = 64;

/// The most bytes of a mutator's name.
inline constexpr std::size_t most_name_bytes = 64;

/// The most bytes of a mutator's description, line feeds included.
inline constexpr std::size_t most_description_bytes = 2000;

/// The most bytes of the summary cut from the description's first line.
inline constexpr std::size_t most_summary_bytes = 200;

/// The most tags a mutator carries.
inline constexpr std::size_t most_tags = 4;

/// The most mods and base games a mutator names in works-with.
inline constexpr std::size_t most_works_with = 32;

/// The most mutators one mutator requires.
inline constexpr std::size_t most_required = 16;

/// The most mutators one mutator refuses to run beside.
inline constexpr std::size_t most_conflicts = 32;

/// The most options a mutator offers.
inline constexpr std::size_t most_options = 16;

/// The most values a choice option lists.
inline constexpr std::size_t most_choice_values = 16;

/// The most bytes of a string choice value.
///
/// The value is an identifier. The words a player reads are its label.
inline constexpr std::size_t most_choice_value_bytes = 32;

/// Tells whether text is a string choice value.
///
/// A value is 1 to 32 bytes of ASCII letters, digits, `_`, `-` and `.`.
/// Callers accept a number before they ask this.
///
/// @param text the value's text
/// @return true when the length is in range and every byte is one of those
[[nodiscard]] bool choice_value_text(std::string_view text) noexcept;

/// Tells whether text is one of Lua 5.4's reserved words.
///
/// The match is the word itself, in that case. `format`, `ends` and `for_`
/// are not reserved. `For` differs in case and is not this word.
///
/// @param text the id
/// @return true for one of the 22 words
[[nodiscard]] bool lua_reserved_word(std::string_view text) noexcept;

/// Tells whether text is an option id a script can read as `ctx.options.<id>`.
///
/// A Lua name of 1 to 32 bytes: `[a-z][a-z0-9_]*`. One of Lua 5.4's
/// reserved words is not a name. A capital, as in `For`, is not a name.
///
/// @param text the id
/// @return true when a script can name it that way
[[nodiscard]] bool option_id(std::string_view text) noexcept;

/// Tells whether text is a mutator id.
///
/// Lower-case kebab-case of 1 to 64 bytes: words of `a`-`z` and `0`-`9`
/// joined by single hyphens, not starting or ending with a hyphen. The same
/// rule the manifest uses for `id`.
///
/// @param text the id
/// @return true when it is such an id
[[nodiscard]] bool mutator_id(std::string_view text) noexcept;

/// The most steps a number option's range may hold.
inline constexpr std::size_t most_number_steps = 1000;

/// The most languages a mutator translates into.
inline constexpr std::size_t most_languages = 16;

/// The most TA: Mutation originals one package lists.
inline constexpr std::size_t most_legacy_records = 16;

/// The most compatibility codes one original lists.
inline constexpr std::size_t most_legacy_codes = 8;

/// A mutator version: one to three parts, missing parts comparing as 0.
///
/// `parts` is how many were written, so `2` and `2.0` compare equal and
/// still write back as they were written.
struct Version {
    uint16_t major{}; ///< the first part
    uint16_t minor{}; ///< the second part, or 0 when unwritten
    uint16_t patch{}; ///< the third part, or 0 when unwritten
    uint8_t parts{};  ///< how many parts were written, from 1 to 3

    /// Orders two versions by major, then minor, then patch.
    ///
    /// A part that was not written compares as 0, so 2 equals 2.0.0.
    ///
    /// @param left the first version
    /// @param right the second version
    /// @return the order of their values
    friend std::strong_ordering operator<=>(const Version& left, const Version& right) noexcept {
        if (const std::strong_ordering major_order = left.major <=> right.major; major_order != 0)
            return major_order;
        if (const std::strong_ordering minor_order = left.minor <=> right.minor; minor_order != 0)
            return minor_order;
        return left.patch <=> right.patch;
    }

    /// Tells whether two versions are the same value.
    ///
    /// @param left the first version
    /// @param right the second version
    /// @return true when every part compares equal, missing parts as 0
    friend bool operator==(const Version& left, const Version& right) noexcept {
        return (left <=> right) == 0;
    }
};

/// A version range: one to four comparisons, all of which must hold.
struct VersionRange {
    /// The comparisons, in the order written.
    std::vector<std::pair<oamod::EngineComparison, Version>> terms{};
};

/// One works-with, requires or conflicts entry.
///
/// `range` is set only when a range was written and parsed. `range_text` is
/// that range as written, spaces included, and empty when the entry is an
/// id alone.
struct Condition {
    std::string id{};                    ///< the mutator or mod id, "*", or "ta-3.1c"
    std::string range_text{};            ///< the range as written; empty when there is none
    std::optional<VersionRange> range{}; ///< the parsed range, when one was written
};

/// A layer a mutator declares, in the order the Library shows them.
enum class Layer : uint8_t {
    files,    ///< loose files and archives
    units,    ///< unit definitions
    weapons,  ///< weapon definitions
    features, ///< wrecks and feature definitions
    sides,    ///< side data
    tables,   ///< other game-data tables
    menus,    ///< build menus
    maps,     ///< map settings
    hacks,    ///< engine hacks for one match
};

/// How a mutator treats a unit or weapon that already has that name.
///
/// Add keeps the existing record and brings this mutator's beside it.
/// Replace puts this mutator's record in its place. The game chooses when
/// the manifest names neither.
enum class ClashMode : uint8_t {
    add,     ///< add the unit or weapon beside the one already there
    replace, ///< replace the unit or weapon that already has that name
};

/// Returns a clash mode's manifest name.
///
/// @param mode the mode
/// @return its name: add or replace
[[nodiscard]] const char* clash_mode_name(ClashMode mode) noexcept;

/// Reads a clash mode.
///
/// @param text the name, compared byte for byte
/// @return the mode, or nothing when the text is not add or replace
[[nodiscard]] std::optional<ClashMode> parse_clash_mode(std::string_view text) noexcept;

/// How a player sets one option.
enum class OptionType : uint8_t {
    toggle, ///< on or off
    choice, ///< one of a fixed list
    number, ///< a number on a step between a minimum and a maximum
};

/// One value a choice option lists.
///
/// A number keeps the exact value and its canonical text. A string keeps
/// its text and `number` is false.
struct ChoiceValue {
    bool number{false};           ///< true when the value is a number
    oamod::Number number_value{}; ///< the number, when `number` is set
    std::string text{};           ///< the string, or the number's canonical text
};

/// The label a translation gives one option.
struct OptionLabel {
    std::string id{};    ///< the option's id
    std::string label{}; ///< the label in this language
};

/// The value labels a translation gives one choice option, in value order.
struct ValueLabels {
    std::string id{};                  ///< the choice option's id
    std::vector<std::string> labels{}; ///< one label per value
};

/// One language's names for the mutator and its options.
struct Translation {
    std::string language{};             ///< the language tag
    std::string name{};                 ///< the mutator's name; empty when unwritten
    std::string description{};          ///< the description; empty when unwritten
    std::vector<OptionLabel> options{}; ///< option labels, in the order written
    std::vector<ValueLabels> labels{};  ///< choice value labels, in the order written
};

/// Who wrote the mutator.
struct Author {
    std::string name{};  ///< the author or team
    std::string email{}; ///< an e-mail address; empty when unwritten
};

/// One TA: Mutation original this package was imported from.
struct Legacy {
    std::string name{};                       ///< the original's name
    std::string prefix{};                     ///< "{}" or "}{"
    std::vector<std::string> compatibility{}; ///< compatibility codes, or "*"
    std::vector<std::string> files{};         ///< the original's file names, in order
};

/// One setting the player picks before a match.
struct Option {
    std::string id{};                        ///< a Lua name, read as ctx.options.<id>
    OptionType type{OptionType::toggle};     ///< toggle, choice or number
    std::string label{};                     ///< the words the picker shows
    bool toggle_default{false};              ///< a toggle's default
    std::vector<ChoiceValue> values{};       ///< a choice's values
    std::vector<std::string> value_labels{}; ///< a choice's labels; empty when unwritten
    std::size_t default_index{0};            ///< which choice value is the default
    oamod::Number minimum{};                 ///< a number option's minimum
    oamod::Number maximum{};                 ///< a number option's maximum
    oamod::Number step{};                    ///< a number option's step
    oamod::Number number_default{};          ///< a number option's default
};

/// A mutator's manifest, as oamut.yaml states it.
struct Manifest {
    int64_t format{};                    ///< oamut: the format, format_version
    std::string id{};                    ///< the mutator's id
    std::string name{};                  ///< the name players see
    Version version{};                   ///< the version other mutators name in ranges
    Author author{};                     ///< who wrote it
    std::string description{};           ///< the description; empty when unwritten
    std::string homepage{};              ///< an http or https address; empty when unwritten
    std::string icon{};                  ///< a square PNG in the package; empty when unwritten
    std::vector<std::string> tags{};     ///< one to four tags, in the order written
    std::vector<Condition> works_with{}; ///< mods and the base game it fits
    std::string requires_engine{};       ///< the engine requirement, as written
    std::vector<Condition> requires_mutators{}; ///< prerequisites, in the order they apply
    std::vector<Condition> conflicts_with{};    ///< mutators it cannot run beside
    std::vector<Layer> layers{};                ///< the layers it uses, in the order written
    /// How units and weapons that already exist are treated. Absent when
    /// the manifest names none; the game's default then applies.
    std::optional<ClashMode> clashes{};
    std::vector<Option> options{};                 ///< the player's settings, in the order written
    std::vector<Translation> strings{};            ///< translations, in the order written
    std::string entry{std::string{default_entry}}; ///< the entry script
    std::vector<Legacy> legacy{};                  ///< the originals, in the order written
};

/// One broken rule, where it is written.
struct Problem {
    uint32_t line{};       ///< the 1-based line of the value
    uint32_t column{};     ///< the 1-based byte column of the value
    std::string key{};     ///< the path of the value, such as options.factor.default
    std::string message{}; ///< what is wrong
};

/// Reads a mutator version.
///
/// One to three parts joined by `.`, each a decimal from 0 to 65535 with no
/// leading zero. The whole text is the version and is at most 32 bytes.
///
/// @param text the version
/// @return the version, or nothing when the text is not one
[[nodiscard]] std::optional<Version> parse_version(std::string_view text) noexcept;

/// Writes a version with the parts that were written and no leading zeros.
///
/// @param version the version
/// @return its text, such as 2, 2.0 or 2.0.1
[[nodiscard]] std::string version_text(const Version& version);

/// Reads a version range.
///
/// The same grammar as an engine requirement, with mutator versions in place
/// of three-part releases: one to four comparisons (`>=`, `>`, `<=`, `<` or
/// `=`), separated by commas that spaces may surround, at most 64 bytes.
///
/// @param text the range, as written
/// @param[out] error what is wrong and the 1-based byte where, when the text
///        is not a range; may be null
/// @return the range, or nothing when the text is not one
[[nodiscard]] std::optional<VersionRange>
parse_version_range(std::string_view text, std::string* error = nullptr);

/// Tells whether a version meets every comparison of a range.
///
/// @param range the range
/// @param version the version
/// @return true when every comparison holds
[[nodiscard]] bool version_in_range(const VersionRange& range, const Version& version) noexcept;

/// Reads a condition: an id alone, or an id, spaces and a version range.
///
/// An id is lower-case kebab-case of 1 to 64 bytes, `*` or `ta-3.1c`. `*`
/// and `ta-3.1c` take no range. A range is read by parse_version_range.
///
/// @param text the condition, as written
/// @param[out] error what is wrong, when the text is not a condition; may be null
/// @return the condition, or nothing when the text is not one
[[nodiscard]] std::optional<Condition>
parse_condition(std::string_view text, std::string* error = nullptr);

/// Tells whether an id at a version meets a condition.
///
/// The ids are compared byte for byte. A condition with no range is met by
/// its id at any version text. A range is met only when the version text is
/// a mutator version inside the range, so a mod version such as `5b` misses.
///
/// @param condition the condition
/// @param id the id to test
/// @param version_text the version text to test against a range
/// @return true when the id matches and any range is met
[[nodiscard]] bool
condition_met(const Condition& condition, std::string_view id, std::string_view version_text);

/// Returns a layer's manifest name.
///
/// @param layer the layer
/// @return its name: files, units, weapons, features, sides, tables, menus, maps or hacks
[[nodiscard]] const char* layer_name(Layer layer) noexcept;

/// Reads a layer name.
///
/// @param text the name, compared byte for byte
/// @return the layer, or nothing when the text is not one of the nine
[[nodiscard]] std::optional<Layer> parse_layer(std::string_view text) noexcept;

/// Reads a manifest.
///
/// Collects every broken rule. A document that is not the strict YAML of a
/// mod profile is one problem. `homepage`, `tags` and `requires.engine`
/// follow the shared package-key rules, and tags are then limited to four.
/// The manifest is left unchanged when any rule is broken.
///
/// @param bytes the manifest's bytes
/// @param[out] manifest the manifest; left unchanged when any rule is broken
/// @param[in,out] problems one entry appended for each broken rule
/// @return true when the manifest was read and `problems` gained nothing
[[nodiscard]] bool
read_manifest(std::span<const uint8_t> bytes, Manifest& manifest, std::vector<Problem>& problems);

/// Writes a manifest as strict YAML.
///
/// Keys are written in a fixed order, with two-space indentation. Every
/// string is double-quoted. Numbers use their canonical text. Short lists
/// are flow sequences. An optional key that is empty is left out, and so is
/// an entry left at its default.
///
/// @param manifest the manifest
/// @return its text, ending in a line break
[[nodiscard]] std::string write_manifest(const Manifest& manifest);

/// Returns the summary the Library shows: the description's first line.
///
/// The line is cut to most_summary_bytes at a UTF-8 character boundary.
///
/// @param manifest the manifest
/// @return the first line, cut to fit
[[nodiscard]] std::string summary_of(const Manifest& manifest);

/// Describes a problem the way a manifest's diagnostics read.
///
/// @param problem the problem
/// @return `oamut.yaml:<line>:<column>: <key>: <message>`
[[nodiscard]] std::string describe(const Problem& problem);

} // namespace oa::formats::oamut
