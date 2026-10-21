// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Mutator manifests: versions and conditions, the design's example, a manifest
// of every key written and read back, each rule refused once, and the summary
// cut on a character boundary.

#include "oa/formats/oamut/manifest.hpp"

#include "oa/formats/oamod.hpp"
#include "oa/test/check.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// OA_OAMUT_CASES is the path of src/formats/oamut/tests/version_cases.txt.
#ifndef OA_OAMUT_CASES
#error "OA_OAMUT_CASES names the version, range and condition case table"
#endif

namespace {

namespace mut = oa::formats::oamut;

using mut::ChoiceValue;
using mut::ClashMode;
using mut::Condition;
using mut::Layer;
using mut::Legacy;
using mut::Manifest;
using mut::Option;
using mut::OptionType;
using mut::Problem;
using mut::Translation;
using mut::Version;

/// Views a text's bytes.
///
/// @param text the text
/// @return its bytes
std::span<const uint8_t> as_bytes(std::string_view text) {
    return {reinterpret_cast<const uint8_t*>(text.data()), text.size()};
}

/// Where a marker sits in a text.
struct Place {
    uint32_t line{0};   ///< the 1-based line
    uint32_t column{0}; ///< the 1-based byte column
};

/// The line and column of one occurrence of a marker.
///
/// @param text the text
/// @param marker the marker
/// @param nth which occurrence, counting from 0
/// @param offset bytes after the marker's first byte
/// @return the place, or zeros when the marker is absent
Place place_of(
    std::string_view text, std::string_view marker, std::size_t nth, std::size_t offset
) {
    std::size_t from = 0;
    std::size_t at = std::string_view::npos;
    for (std::size_t seen = 0; seen <= nth; ++seen) {
        at = text.find(marker, from);
        if (at == std::string_view::npos)
            break;
        from = at + 1;
    }
    if (at == std::string_view::npos || (offset != 0 && at + offset >= text.size())) {
        std::fprintf(
            stderr,
            "marker missing (%zu): %.*s\n",
            nth,
            static_cast<int>(marker.size()),
            marker.data()
        );
        OA_CHECK(false);
        return {};
    }
    at += offset;
    Place place{1, 1};
    for (std::size_t index = 0; index < at; ++index) {
        if (text[index] == '\n') {
            ++place.line;
            place.column = 1;
        } else {
            ++place.column;
        }
    }
    return place;
}

/// Prints every problem.
///
/// @param problems the problems
void show_problems(const std::vector<Problem>& problems) {
    for (const Problem& problem : problems)
        std::fprintf(stderr, "  %s\n", mut::describe(problem).c_str());
}

/// A manifest that names every required key and the optional ones a refusal can sit beside.
///
/// @return the manifest's text
std::string base_text() {
    return R"(oamut: 1
id: steady-hands
name: "Steady Hands"
version: "2.0"
author:
  name: "A. Maker"
  email: "maker@example.com"
description: "Hands stay steady."
homepage: "https://example.com/steady"
icon: "icons/steady.png"
tags: ["economy", "gameplay"]
works-with: ["ridge >= 1"]
requires:
  engine: ">= 0.8.5"
  mutators: ["long-shadows >= 1"]
conflicts-with: ["deep-water"]
layers: ["units", "weapons"]
entry: "scripts/steady.lua"
legacy:
  - name: "First Original"
    prefix: "{}"
    compatibility: ["AB"]
    files: ["first.sm"]
)";
}

/// Replaces the one occurrence of a piece of the base manifest.
///
/// @param from the piece
/// @param to the replacement
/// @return the manifest
std::string edited(std::string_view from, std::string_view to) {
    std::string text = base_text();
    const std::size_t at = text.find(from);
    const std::size_t again = at == std::string::npos ? std::string::npos : text.find(from, at + 1);
    if (at == std::string::npos || again != std::string::npos) {
        std::fprintf(
            stderr, "edit is not unique: %.*s\n", static_cast<int>(from.size()), from.data()
        );
        OA_CHECK(false);
        return text;
    }
    text.replace(at, from.size(), to);
    return text;
}

/// Reads a manifest that must be refused, with one problem at a marker.
///
/// A problem already in the list stays there, and the manifest is left as it was.
///
/// @param yaml the manifest
/// @param key the problem's key
/// @param marker a piece of text at the value, or ending where `offset` says
/// @param message a piece of the problem's message
/// @param nth which occurrence of the marker
/// @param offset bytes from the marker to the value
void expect_one(
    std::string_view yaml,
    std::string_view key,
    std::string_view marker,
    std::string_view message,
    std::size_t nth = 0,
    std::size_t offset = 0
) {
    Manifest manifest;
    manifest.id = "sentinel";
    manifest.name = "Sentinel";
    manifest.entry = "sentinel.lua";
    std::vector<Problem> problems;
    problems.push_back(Problem{7, 7, "kept", "kept"});
    const bool read = mut::read_manifest(as_bytes(yaml), manifest, problems);
    const Place place = place_of(yaml, marker, nth, offset);
    bool matched = false;
    if (problems.size() == 2 && problems[0].key == "kept" && problems[0].message == "kept") {
        const Problem& problem = problems[1];
        matched = problem.key == key && problem.line == place.line &&
                  problem.column == place.column &&
                  problem.message.find(message) != std::string::npos;
    }
    if (read || manifest.id != "sentinel" || manifest.name != "Sentinel" ||
        manifest.entry != "sentinel.lua" || !matched) {
        std::fprintf(
            stderr,
            "expected one %.*s at %u:%u (%.*s)\n",
            static_cast<int>(key.size()),
            key.data(),
            place.line,
            place.column,
            static_cast<int>(message.size()),
            message.data()
        );
        show_problems(problems);
    }
    OA_CHECK(!read);
    OA_CHECK(manifest.id == "sentinel");
    OA_CHECK(manifest.name == "Sentinel");
    OA_CHECK(manifest.entry == "sentinel.lua");
    OA_CHECK(matched);
    const std::string described = problems.size() == 2 ? mut::describe(problems[1]) : std::string{};
    if (problems.size() == 2) {
        const std::string wanted = "oamut.yaml:" + std::to_string(problems[1].line) + ":" +
                                   std::to_string(problems[1].column) + ": " + problems[1].key +
                                   ": " + problems[1].message;
        OA_CHECK(described == wanted);
    }
}

/// Reads a manifest that must pass.
///
/// @param yaml the manifest
/// @param[out] manifest the manifest
/// @return true when it passed
bool read_ok(std::string_view yaml, Manifest& manifest) {
    std::vector<Problem> problems;
    Manifest read;
    const bool ok = mut::read_manifest(as_bytes(yaml), read, problems);
    if (!ok || !problems.empty()) {
        std::fprintf(stderr, "expected a manifest, got %zu problems\n", problems.size());
        show_problems(problems);
        OA_CHECK(false);
        return false;
    }
    manifest = std::move(read);
    return true;
}

/// Tells whether two versions were written the same way.
///
/// @param first the first version
/// @param second the second version
/// @return true when the value and the written parts agree
bool same_version(const Version& first, const Version& second) {
    return first == second && first.parts == second.parts &&
           mut::version_text(first) == mut::version_text(second);
}

/// Tells whether two conditions were written the same way.
///
/// @param first the first condition
/// @param second the second condition
/// @return true when the id, the range text and the comparisons agree
bool same_condition(const Condition& first, const Condition& second) {
    if (first.id != second.id || first.range_text != second.range_text)
        return false;
    if (first.range.has_value() != second.range.has_value())
        return false;
    if (!first.range.has_value())
        return true;
    if (first.range->terms.size() != second.range->terms.size())
        return false;
    for (std::size_t index = 0; index < first.range->terms.size(); ++index) {
        if (first.range->terms[index].first != second.range->terms[index].first)
            return false;
        if (!same_version(first.range->terms[index].second, second.range->terms[index].second))
            return false;
    }
    return true;
}

/// Tells whether two numbers are the same value.
///
/// @param first the first number
/// @param second the second number
/// @return true when they compare equal
bool same_number(
    const oa::formats::oamod::Number& first, const oa::formats::oamod::Number& second
) {
    return oa::formats::oamod::compare_numbers(first, second) == 0;
}

/// Tells whether two choice values were written the same way.
///
/// @param first the first value
/// @param second the second value
/// @return true when both are the same number or the same string
bool same_choice(const ChoiceValue& first, const ChoiceValue& second) {
    if (first.number != second.number || first.text != second.text)
        return false;
    if (!first.number)
        return true;
    return same_number(first.number_value, second.number_value) &&
           first.text == oa::formats::oamod::canonical_number_text(first.number_value);
}

/// Tells whether two options were written the same way.
///
/// @param first the first option
/// @param second the second option
/// @return true when every field that option uses agrees
bool same_option(const Option& first, const Option& second) {
    if (first.id != second.id || first.type != second.type || first.label != second.label)
        return false;
    if (first.type == OptionType::toggle)
        return first.toggle_default == second.toggle_default;
    if (first.type == OptionType::choice) {
        if (first.default_index != second.default_index ||
            first.values.size() != second.values.size() ||
            first.value_labels != second.value_labels)
            return false;
        for (std::size_t index = 0; index < first.values.size(); ++index)
            if (!same_choice(first.values[index], second.values[index]))
                return false;
        return true;
    }
    return same_number(first.minimum, second.minimum) &&
           same_number(first.maximum, second.maximum) && same_number(first.step, second.step) &&
           same_number(first.number_default, second.number_default);
}

/// Tells whether two translations were written the same way.
///
/// @param first the first translation
/// @param second the second translation
/// @return true when every field agrees
bool same_translation(const Translation& first, const Translation& second) {
    if (first.language != second.language || first.name != second.name ||
        first.description != second.description || first.options.size() != second.options.size() ||
        first.labels.size() != second.labels.size())
        return false;
    for (std::size_t index = 0; index < first.options.size(); ++index)
        if (first.options[index].id != second.options[index].id ||
            first.options[index].label != second.options[index].label)
            return false;
    for (std::size_t index = 0; index < first.labels.size(); ++index)
        if (first.labels[index].id != second.labels[index].id ||
            first.labels[index].labels != second.labels[index].labels)
            return false;
    return true;
}

/// Tells whether two originals were written the same way.
///
/// @param first the first original
/// @param second the second original
/// @return true when every field agrees
bool same_legacy(const Legacy& first, const Legacy& second) {
    return first.name == second.name && first.prefix == second.prefix &&
           first.compatibility == second.compatibility && first.files == second.files;
}

/// Tells whether two manifests were written the same way.
///
/// @param first the first manifest
/// @param second the second manifest
/// @return true when every field agrees, including a version's written parts
bool same_manifest(const Manifest& first, const Manifest& second) {
    if (first.format != second.format || first.id != second.id || first.name != second.name ||
        !same_version(first.version, second.version) || first.author.name != second.author.name ||
        first.author.email != second.author.email || first.description != second.description ||
        first.homepage != second.homepage || first.icon != second.icon ||
        first.tags != second.tags || first.requires_engine != second.requires_engine ||
        first.layers != second.layers || first.clashes != second.clashes ||
        first.entry != second.entry)
        return false;
    if (first.works_with.size() != second.works_with.size() ||
        first.requires_mutators.size() != second.requires_mutators.size() ||
        first.conflicts_with.size() != second.conflicts_with.size() ||
        first.options.size() != second.options.size() ||
        first.strings.size() != second.strings.size() ||
        first.legacy.size() != second.legacy.size())
        return false;
    for (std::size_t index = 0; index < first.works_with.size(); ++index)
        if (!same_condition(first.works_with[index], second.works_with[index]))
            return false;
    for (std::size_t index = 0; index < first.requires_mutators.size(); ++index)
        if (!same_condition(first.requires_mutators[index], second.requires_mutators[index]))
            return false;
    for (std::size_t index = 0; index < first.conflicts_with.size(); ++index)
        if (!same_condition(first.conflicts_with[index], second.conflicts_with[index]))
            return false;
    for (std::size_t index = 0; index < first.options.size(); ++index)
        if (!same_option(first.options[index], second.options[index]))
            return false;
    for (std::size_t index = 0; index < first.strings.size(); ++index)
        if (!same_translation(first.strings[index], second.strings[index]))
            return false;
    for (std::size_t index = 0; index < first.legacy.size(); ++index)
        if (!same_legacy(first.legacy[index], second.legacy[index]))
            return false;
    return true;
}

/// Writes a manifest and reads that text back.
///
/// @param manifest the manifest
/// @return true when the second read matches and a second write is the same text
bool round_trip(const Manifest& manifest) {
    const std::string once = mut::write_manifest(manifest);
    Manifest again;
    if (!read_ok(once, again) || !same_manifest(manifest, again)) {
        std::fprintf(stderr, "round trip lost a field:\n%s", once.c_str());
        OA_CHECK(false);
        return false;
    }
    OA_CHECK(mut::write_manifest(again) == once);
    return mut::write_manifest(again) == once;
}

/// A flow list of quoted ids.
///
/// @param count how many
/// @param prefix the text before each index
/// @return the flow sequence
std::string quoted_ids(int count, std::string_view prefix) {
    std::string out = "[";
    for (int index = 0; index < count; ++index) {
        if (index != 0)
            out += ", ";
        out += "\"";
        out += prefix;
        out += std::to_string(index);
        out += "\"";
    }
    out += "]";
    return out;
}

/// Inserts an options block in front of layers.
///
/// @param body the options block, including its key line and a trailing line break
/// @return the manifest
std::string with_options(std::string_view body) {
    return edited("layers:", std::string{body} + "layers:");
}

/// The choice, toggle and number the full manifest carries.
///
/// @return the options block
std::string sample_options() {
    return R"(options:
  factor:
    type: choice
    label: "Build speed"
    values: [2, 4, 8]
    labels: ["Double", "Four", "Eight"]
    default: 2
  announce:
    type: toggle
    label: "Announce"
    default: true
  pace:
    type: number
    label: "Pace"
    min: 0.5
    max: 2.5
    step: 0.5
    default: 1.5
)";
}

/// Splits a comma-separated list, keeping each item whole.
///
/// @param text the list; empty when there is none
/// @return the items
std::vector<std::string_view> split_commas(std::string_view text) {
    std::vector<std::string_view> items;
    if (text.empty())
        return items;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t comma = text.find(',', start);
        items.push_back(text.substr(
            start, comma == std::string_view::npos ? std::string_view::npos : comma - start
        ));
        if (comma == std::string_view::npos)
            break;
        start = comma + 1;
    }
    return items;
}

/// Prints a case that did not hold.
///
/// @param line the case
void fail_case(std::string_view line) {
    std::fprintf(stderr, "version case failed: %.*s\n", static_cast<int>(line.size()), line.data());
    OA_CHECK(false);
}

/// The version, range and condition table, and the order of 2, 2.0, 2.0.1 and 2.10.
void test_versions() {
    const std::optional<Version> two = mut::parse_version("2");
    const std::optional<Version> two_zero = mut::parse_version("2.0");
    const std::optional<Version> two_zero_zero = mut::parse_version("2.0.0");
    const std::optional<Version> two_zero_one = mut::parse_version("2.0.1");
    const std::optional<Version> two_ten = mut::parse_version("2.10");
    OA_CHECK(two.has_value() && two_zero.has_value() && two_zero_zero.has_value());
    OA_CHECK(two_zero_one.has_value() && two_ten.has_value());
    if (two && two_zero && two_zero_zero && two_zero_one && two_ten) {
        OA_CHECK(*two == *two_zero && *two_zero == *two_zero_zero);
        OA_CHECK(*two_zero_zero < *two_zero_one && *two_zero_one < *two_ten);
        OA_CHECK(two->parts == 1 && two_zero->parts == 2 && two_zero_zero->parts == 3);
        OA_CHECK(mut::version_text(*two) == "2");
        OA_CHECK(mut::version_text(*two_zero) == "2.0");
        OA_CHECK(mut::version_text(*two_ten) == "2.10");
    }

    std::ifstream input{OA_OAMUT_CASES};
    OA_CHECK(input.is_open());
    if (!input.is_open())
        return;
    std::string line;
    int count = 0;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line.front() == '#')
            continue;
        ++count;
        const std::size_t first = line.find(' ');
        if (first == std::string::npos) {
            fail_case(line);
            continue;
        }
        const std::size_t second = line.find(' ', first + 1);
        const std::string kind = line.substr(0, first);
        const std::string status = second == std::string::npos
                                       ? line.substr(first + 1)
                                       : line.substr(first + 1, second - first - 1);
        const std::string text =
            second == std::string::npos ? std::string{} : line.substr(second + 1);
        if (status != "ok" && status != "bad") {
            fail_case(line);
            continue;
        }
        if (kind == "version") {
            const std::optional<Version> version = mut::parse_version(text);
            if (version.has_value() != (status == "ok"))
                fail_case(line);
            else if (version && mut::version_text(*version) != text)
                fail_case(line);
            continue;
        }
        constexpr std::string_view marker = " :: ";
        if (kind == "range" && status == "ok") {
            const std::size_t meet_at = text.find(marker);
            const std::size_t miss_at = meet_at == std::string::npos
                                            ? std::string::npos
                                            : text.find(marker, meet_at + marker.size());
            if (meet_at == std::string::npos || miss_at == std::string::npos) {
                fail_case(line);
                continue;
            }
            const std::string requirement = text.substr(0, meet_at);
            const std::string meet =
                text.substr(meet_at + marker.size(), miss_at - (meet_at + marker.size()));
            const std::string miss = text.substr(miss_at + marker.size());
            std::string error;
            const std::optional<mut::VersionRange> range =
                mut::parse_version_range(requirement, &error);
            if (!range) {
                fail_case(line);
                continue;
            }
            for (const std::string_view version_text : split_commas(meet)) {
                const std::optional<Version> version = mut::parse_version(version_text);
                if (!version || !mut::version_in_range(*range, *version))
                    fail_case(line);
            }
            for (const std::string_view version_text : split_commas(miss)) {
                const std::optional<Version> version = mut::parse_version(version_text);
                if (!version || mut::version_in_range(*range, *version))
                    fail_case(line);
            }
            continue;
        }
        if (kind == "range") {
            std::string error;
            if (mut::parse_version_range(text, &error) || error.empty())
                fail_case(line);
            continue;
        }
        if (kind == "condition" && status == "ok") {
            const std::size_t met_at = text.find(marker);
            const std::size_t missed_at = met_at == std::string::npos
                                              ? std::string::npos
                                              : text.find(marker, met_at + marker.size());
            if (met_at == std::string::npos || missed_at == std::string::npos) {
                fail_case(line);
                continue;
            }
            const std::string body = text.substr(0, met_at);
            const std::string met =
                text.substr(met_at + marker.size(), missed_at - (met_at + marker.size()));
            const std::string missed = text.substr(missed_at + marker.size());
            std::string error;
            const std::optional<Condition> condition = mut::parse_condition(body, &error);
            if (!condition) {
                fail_case(line);
                continue;
            }
            for (const std::string_view pair : split_commas(met)) {
                const std::size_t at = pair.find('@');
                if (at == std::string_view::npos ||
                    !mut::condition_met(*condition, pair.substr(0, at), pair.substr(at + 1)))
                    fail_case(line);
            }
            for (const std::string_view pair : split_commas(missed)) {
                const std::size_t at = pair.find('@');
                if (at == std::string_view::npos ||
                    mut::condition_met(*condition, pair.substr(0, at), pair.substr(at + 1)))
                    fail_case(line);
            }
            continue;
        }
        if (kind == "condition") {
            std::string error;
            if (mut::parse_condition(text, &error) || error.empty())
                fail_case(line);
            continue;
        }
        fail_case(line);
    }
    OA_CHECK(count >= 60);
}

/// The design's example, with a made-up id, and a manifest that uses every key.
void test_examples() {
    const char* design = R"(oamut: 1
id: steady-hands
name: "Steady Hands"
version: "2.0"
author:
  name: "A. Maker"
description: "Hands stay steady."
tags: ["economy"]
works-with: ["*"]
requires:
  engine: ">= 0.8.5"
layers: ["units"]
options:
  factor:
    type: choice
    label: "Build speed"
    values: [2, 4, 8]
    default: 2
)";
    Manifest manifest;
    OA_CHECK(read_ok(design, manifest));
    if (manifest.id.empty())
        return;
    OA_CHECK(manifest.format == mut::format_version);
    OA_CHECK(manifest.id == "steady-hands");
    OA_CHECK(manifest.name == "Steady Hands");
    OA_CHECK(manifest.version.parts == 2 && mut::version_text(manifest.version) == "2.0");
    OA_CHECK(manifest.author.name == "A. Maker" && manifest.author.email.empty());
    OA_CHECK(manifest.description == "Hands stay steady.");
    OA_CHECK(manifest.homepage.empty() && manifest.icon.empty());
    OA_CHECK(manifest.tags.size() == 1 && manifest.tags[0] == "economy");
    OA_CHECK(manifest.works_with.size() == 1 && manifest.works_with[0].id == "*");
    OA_CHECK(!manifest.works_with[0].range.has_value());
    OA_CHECK(manifest.requires_engine == ">= 0.8.5");
    OA_CHECK(manifest.requires_mutators.empty() && manifest.conflicts_with.empty());
    OA_CHECK(manifest.layers.size() == 1 && manifest.layers[0] == Layer::units);
    OA_CHECK(!manifest.clashes.has_value());
    OA_CHECK(manifest.entry == mut::default_entry);
    OA_CHECK(manifest.options.size() == 1 && manifest.options[0].id == "factor");
    OA_CHECK(manifest.options[0].type == OptionType::choice);
    OA_CHECK(manifest.options[0].label == "Build speed");
    OA_CHECK(manifest.options[0].values.size() == 3 && manifest.options[0].default_index == 0);
    OA_CHECK(manifest.options[0].values[0].number && manifest.options[0].values[0].text == "2");
    OA_CHECK(manifest.options[0].values[2].text == "8");
    OA_CHECK(manifest.legacy.empty() && manifest.strings.empty());
    OA_CHECK(mut::summary_of(manifest) == "Hands stay steady.");
    OA_CHECK(round_trip(manifest));
    OA_CHECK(mut::write_manifest(manifest).find("entry:") == std::string::npos);

    const std::string full = std::string{R"(oamut: 1
id: steady-hands
name: "Steady Hands"
version: "2.0"
author:
  name: "A. Maker"
  email: "maker@example.com"
description: "Hands stay steady.\nThe second line stays in the description."
homepage: "https://example.com/steady"
icon: "icons/steady.PNG"
tags: ["economy", "gameplay"]
works-with: ["ridge >= 1", "ta-3.1c"]
requires:
  engine: ">= 0.8.5"
  mutators: ["long-shadows >= 1"]
conflicts-with: ["deep-water"]
layers: ["files", "units", "weapons", "features", "sides", "tables", "menus", "maps", "hacks"]
clashes: add
)"} + sample_options() +
                             R"(strings:
  de:
    name: "Feste Haende"
    description: "Haende bleiben fest."
    options:
      factor: "Bautempo"
    labels:
      factor: ["Doppelt", "Vier", "Acht"]
  fr: {}
entry: "scripts/steady.lua"
legacy:
  - name: "First Original"
    prefix: "{}"
    compatibility: ["AB"]
    files: ["first.sm"]
  - name: "Second Original"
    prefix: "}{"
    compatibility: ["*", "CD12"]
    files: ["second.sm", "notes.txt"]
  - name: "Third Original"
    prefix: "{}"
    compatibility: ["Z"]
    files: ["third.sm"]
)";
    Manifest packed;
    OA_CHECK(read_ok(full, packed));
    if (packed.id.empty())
        return;
    OA_CHECK(packed.icon == "icons/steady.PNG");
    OA_CHECK(packed.description == "Hands stay steady.\nThe second line stays in the description.");
    OA_CHECK(packed.layers.size() == 9);
    OA_CHECK(packed.layers[0] == Layer::files && packed.layers[8] == Layer::hacks);
    OA_CHECK(packed.clashes == ClashMode::add);
    OA_CHECK(mut::layer_name(Layer::hacks) == std::string_view{"hacks"});
    OA_CHECK(mut::parse_layer("sounds") == std::nullopt);
    OA_CHECK(packed.works_with.size() == 2 && packed.works_with[1].id == "ta-3.1c");
    OA_CHECK(packed.options.size() == 3 && packed.options[1].toggle_default);
    OA_CHECK(packed.options[0].value_labels.size() == 3);
    OA_CHECK(packed.options[2].type == OptionType::number);
    OA_CHECK(packed.strings.size() == 2 && packed.strings[0].language == "de");
    OA_CHECK(
        packed.strings[0].options.size() == 1 && packed.strings[0].options[0].label == "Bautempo"
    );
    OA_CHECK(
        packed.strings[0].labels.size() == 1 && packed.strings[0].labels[0].labels.size() == 3
    );
    OA_CHECK(packed.strings[1].language == "fr" && packed.strings[1].name.empty());
    OA_CHECK(packed.entry == "scripts/steady.lua");
    OA_CHECK(packed.legacy.size() == 3);
    OA_CHECK(packed.legacy[0].prefix == "{}" && packed.legacy[1].prefix == "}{");
    OA_CHECK(
        packed.legacy[1].compatibility.size() == 2 && packed.legacy[1].compatibility[0] == "*"
    );
    OA_CHECK(packed.legacy[2].name == "Third Original");
    OA_CHECK(mut::summary_of(packed) == "Hands stay steady.");
    OA_CHECK(round_trip(packed));

    Manifest plain;
    OA_CHECK(read_ok(base_text(), plain));
    OA_CHECK(plain.legacy.size() == 1 && plain.legacy[0].files.size() == 1);
    OA_CHECK(round_trip(plain));
    OA_CHECK(!plain.clashes.has_value());
    OA_CHECK(mut::write_manifest(plain).find("clashes:") == std::string::npos);

    const std::string wide(64, 'a');
    Manifest wide_id;
    OA_CHECK(read_ok(edited("id: steady-hands", "id: " + wide), wide_id));
    OA_CHECK(wide_id.id == wide);
    const std::string wide_name(64, 'B');
    Manifest wide_named;
    OA_CHECK(read_ok(edited("name: \"Steady Hands\"", "name: \"" + wide_name + "\""), wide_named));
    OA_CHECK(wide_named.name == wide_name);
}

/// Both clash modes, an absent key, and the values the key refuses.
void test_clashes() {
    const std::string layers = "layers: [\"units\", \"weapons\"]\n";
    Manifest adding;
    OA_CHECK(read_ok(edited(layers, "layers: [\"units\"]\nclashes: \"add\"\n"), adding));
    OA_CHECK(adding.clashes == ClashMode::add);
    OA_CHECK(mut::clash_mode_name(ClashMode::add) == std::string_view{"add"});
    OA_CHECK(round_trip(adding));
    OA_CHECK(mut::write_manifest(adding).find("clashes: \"add\"\n") != std::string::npos);

    Manifest replacing;
    OA_CHECK(read_ok(edited(layers, "layers: [\"weapons\"]\nclashes: \"replace\"\n"), replacing));
    OA_CHECK(replacing.clashes == ClashMode::replace);
    OA_CHECK(mut::clash_mode_name(ClashMode::replace) == std::string_view{"replace"});
    OA_CHECK(mut::parse_clash_mode("replace") == ClashMode::replace);
    OA_CHECK(mut::parse_clash_mode("Add") == std::nullopt);
    OA_CHECK(round_trip(replacing));

    expect_one(
        edited(layers, "layers: [\"units\"]\nclashes: \"Add\"\n"),
        "clashes",
        "\"Add\"",
        "clashes must be add or replace"
    );
    expect_one(
        edited(layers, "layers: [\"units\"]\nclashes: merge\n"),
        "clashes",
        "merge",
        "clashes must be add or replace"
    );
    expect_one(
        edited(layers, "layers: [\"units\"]\nclashes: 1\n"),
        "clashes",
        "clashes: 1",
        "clashes must be add or replace",
        0,
        9
    );
}

/// One broken rule of the manifest, each on its own.
void test_refusals() {
    expect_one(base_text() + "color: blue\n", "color", "blue", "is not a key of oamut.yaml");
    expect_one(base_text() + "7: 1\n", "7", "7: 1", "is not a key of oamut.yaml", 0, 3);
    const char* profiles[] = {
        "identity",
        "layout",
        "limits",
        "script-extensions",
        "data-keys",
        "hacks",
        "settings",
        "media",
        "packaging",
        "supportedGames",
        "requiredExpansions",
    };
    for (const char* profile : profiles) {
        const std::string line = std::string{profile} + ": 1\n";
        expect_one(
            base_text() + line,
            profile,
            line,
            "belongs in a mod's oamod.yaml",
            0,
            std::strlen(profile) + 2
        );
    }

    expect_one(edited("oamut: 1\n", ""), "oamut", "id:", "oamut must be 1");
    expect_one(edited("oamut: 1", "oamut: 1.0"), "oamut", "1.0", "oamut must be 1");
    expect_one(edited("oamut: 1", "oamut: \"1\""), "oamut", "\"1\"", "oamut must be 1");
    expect_one(edited("oamut: 1", "oamut: 2"), "oamut", "2", "oamut must be 1");

    expect_one(edited("id: steady-hands\n", ""), "id", "oamut:", "lower-case kebab-case");
    expect_one(edited("id: steady-hands", "id: Steady"), "id", "Steady", "lower-case kebab-case");
    expect_one(
        edited("id: steady-hands", "id: \"a--b\""), "id", "\"a--b\"", "lower-case kebab-case"
    );
    expect_one(edited("id: steady-hands", "id: \"-ab\""), "id", "\"-ab\"", "lower-case kebab-case");
    expect_one(edited("id: steady-hands", "id: \"ab-\""), "id", "\"ab-\"", "lower-case kebab-case");
    expect_one(
        edited("id: steady-hands", "id: \"" + std::string(65, 'a') + "\""),
        "id",
        "\"" + std::string(65, 'a'),
        "lower-case kebab-case"
    );

    expect_one(edited("name: \"Steady Hands\"\n", ""), "name", "oamut:", "name is required");
    expect_one(
        edited("name: \"Steady Hands\"", "name: \"\""), "name", "name: \"\"", "1 to 64", 0, 6
    );
    expect_one(
        edited("name: \"Steady Hands\"", "name: \"" + std::string(65, 'B') + "\""),
        "name",
        "\"" + std::string(65, 'B'),
        "1 to 64"
    );
    expect_one(
        edited("name: \"Steady Hands\"", "name: \"bad\\tname\""),
        "name",
        "\"bad\\tname\"",
        "control"
    );

    expect_one(
        edited("version: \"2.0\"", "version: 2.0"), "version", "2.0", "write the version in quotes"
    );
    expect_one(
        edited("version: \"2.0\"", "version: 2.0.1"),
        "version",
        "2.0.1",
        "write the version in quotes"
    );
    expect_one(
        edited("version: \"2.0\"", "version: \"01\""), "version", "\"01\"", "mutator version"
    );
    expect_one(
        edited("version: \"2.0\"", "version: \"v2\""), "version", "\"v2\"", "mutator version"
    );
    expect_one(edited("version: \"2.0\"\n", ""), "version", "oamut:", "version is required");
    expect_one(
        edited("version: \"2.0\"", "version: [1]"), "version", "[1]", "version must be a string"
    );

    expect_one(
        edited("author:\n  name: \"A. Maker\"\n  email: \"maker@example.com\"\n", ""),
        "author",
        "oamut:",
        "author is required"
    );
    expect_one(
        edited(
            "author:\n  name: \"A. Maker\"\n  email: \"maker@example.com\"", "author: \"A. Maker\""
        ),
        "author",
        "\"A. Maker\"",
        "must be an object"
    );
    expect_one(
        edited("author:\n  name: \"A. Maker\"\n  email: \"maker@example.com\"", "author: {}"),
        "author.name",
        "author: {}",
        "author.name",
        0,
        8
    );
    expect_one(
        edited("email: \"maker@example.com\"", "email: \"maker\""),
        "author.email",
        "\"maker\"",
        "e-mail"
    );
    expect_one(
        edited("email: \"maker@example.com\"", "title: \"x\"\n  email: \"maker@example.com\""),
        "author.title",
        "\"x\"",
        "is not a key of author"
    );

    expect_one(
        edited("description: \"Hands stay steady.\"", "description: 1"),
        "description",
        "description: 1",
        "must be a string",
        0,
        13
    );
    expect_one(
        edited("description: \"Hands stay steady.\"", "description: \"\""),
        "description",
        "description: \"\"",
        "1 to 2000",
        0,
        13
    );
    expect_one(
        edited("description: \"Hands stay steady.\"", "description: \"\\t\""),
        "description",
        "\"\\t\"",
        "control"
    );
    expect_one(
        edited(
            "description: \"Hands stay steady.\"", "description: \"" + std::string(2001, 'a') + "\""
        ),
        "description",
        "\"" + std::string(2001, 'a'),
        "1 to 2000"
    );

    expect_one(
        edited("homepage: \"https://example.com/steady\"", "homepage: \"ftp://example.com\""),
        "homepage",
        "\"ftp://example.com\"",
        "http or https"
    );
    expect_one(
        edited("engine: \">= 0.8.5\"", "engine: \">= 0.8\""),
        "requires.engine",
        "\">= 0.8\"",
        "expected '.'"
    );
    expect_one(
        edited(
            "requires:\n  engine: \">= 0.8.5\"\n  mutators: [\"long-shadows >= 1\"]",
            "requires: \">= 0.8.5\""
        ),
        "requires",
        "\">= 0.8.5\"",
        "must be an object"
    );
    expect_one(
        edited(
            "mutators: [\"long-shadows >= 1\"]",
            "mods: [\"ridge\"]\n  mutators: [\"long-shadows >= 1\"]"
        ),
        "requires.mods",
        "[\"ridge\"]",
        "is not a key of requires"
    );

    expect_one(
        edited("icon: \"icons/steady.png\"", "icon: \"/icons/steady.png\""),
        "icon",
        "\"/icons/steady.png\"",
        ".png"
    );
    expect_one(
        edited("icon: \"icons/steady.png\"", "icon: \"icons/../steady.png\""),
        "icon",
        "\"icons/../steady.png\"",
        ".png"
    );
    expect_one(
        edited("icon: \"icons/steady.png\"", "icon: \"icons/steady.jpg\""),
        "icon",
        "\"icons/steady.jpg\"",
        ".png"
    );
    expect_one(
        edited("icon: \"icons/steady.png\"", "icon: \"icons\\\\steady.png\""),
        "icon",
        "\"icons\\\\steady.png\"",
        ".png"
    );

    expect_one(
        edited("tags: [\"economy\", \"gameplay\"]\n", ""), "tags", "oamut:", "tags is required"
    );
    expect_one(
        edited("tags: [\"economy\", \"gameplay\"]", "tags: []"), "tags", "[]", "leave the key out"
    );
    expect_one(
        edited("tags: [\"economy\", \"gameplay\"]", "tags: [\"Economy\"]"),
        "tags[0]",
        "\"Economy\"",
        "kebab-case"
    );
    expect_one(
        edited("tags: [\"economy\", \"gameplay\"]", "tags: [\"economy\", \"economy\"]"),
        "tags[1]",
        "\"economy\"",
        "repeats",
        1
    );
    expect_one(
        edited(
            "tags: [\"economy\", \"gameplay\"]",
            "tags: [\"economy\", \"gameplay\", \"units\", \"maps\", \"ai\"]"
        ),
        "tags",
        "[\"economy\", \"gameplay\", \"units\"",
        "at most 4"
    );
    expect_one(
        edited(
            "tags: [\"economy\", \"gameplay\"]",
            "tags: [\"a0\", \"a1\", \"a2\", \"a3\", \"a4\", \"a5\", \"a6\", \"a7\", \"a8\"]"
        ),
        "tags",
        "[\"a0\", \"a1\"",
        "at most 8"
    );

    expect_one(
        edited("works-with: [\"ridge >= 1\"]\n", ""),
        "works-with",
        "oamut:",
        "works-with is required"
    );
    expect_one(
        edited("works-with: [\"ridge >= 1\"]", "works-with: []"),
        "works-with",
        "works-with: []",
        "must list",
        0,
        12
    );
    expect_one(
        edited("works-with: [\"ridge >= 1\"]", "works-with: ridge"),
        "works-with",
        "ridge",
        "must be a list"
    );
    expect_one(
        edited("works-with: [\"ridge >= 1\"]", "works-with: [\"ridge\", \"ridge\"]"),
        "works-with[1]",
        "\"ridge\"",
        "repeats",
        1
    );
    expect_one(
        edited("works-with: [\"ridge >= 1\"]", "works-with: [\"*\", \"ridge\"]"),
        "works-with[0]",
        "\"*\"",
        "only entry"
    );
    expect_one(
        edited("works-with: [\"ridge >= 1\"]", "works-with: [\"* >= 1\"]"),
        "works-with[0]",
        "\"* >= 1\"",
        "takes no version range"
    );
    {
        const std::string many = quoted_ids(33, "mod-");
        expect_one(
            edited("works-with: [\"ridge >= 1\"]", "works-with: " + many),
            "works-with",
            many,
            "must list"
        );
    }

    expect_one(
        edited("mutators: [\"long-shadows >= 1\"]", "mutators: []"),
        "requires.mutators",
        "mutators: []",
        "must list",
        0,
        10
    );
    expect_one(
        edited("mutators: [\"long-shadows >= 1\"]", "mutators: long-shadows"),
        "requires.mutators",
        "long-shadows",
        "must be a list"
    );
    expect_one(
        edited("mutators: [\"long-shadows >= 1\"]", "mutators: [\"*\"]"),
        "requires.mutators[0]",
        "\"*\"",
        "mutator id"
    );
    expect_one(
        edited("mutators: [\"long-shadows >= 1\"]", "mutators: [\"ta-3.1c\"]"),
        "requires.mutators[0]",
        "\"ta-3.1c\"",
        "mutator id"
    );
    expect_one(
        edited("mutators: [\"long-shadows >= 1\"]", "mutators: [\"steady-hands\"]"),
        "requires.mutators[0]",
        "\"steady-hands\"",
        "names this mutator"
    );
    expect_one(
        edited("mutators: [\"long-shadows >= 1\"]", "mutators: [\"ridge\", \"ridge\"]"),
        "requires.mutators[1]",
        "\"ridge\"",
        "repeats",
        1
    );
    {
        const std::string many = quoted_ids(17, "need-");
        expect_one(
            edited("mutators: [\"long-shadows >= 1\"]", "mutators: " + many),
            "requires.mutators",
            many,
            "must list"
        );
    }

    expect_one(
        edited("conflicts-with: [\"deep-water\"]", "conflicts-with: []"),
        "conflicts-with",
        "conflicts-with: []",
        "must list",
        0,
        16
    );
    expect_one(
        edited("conflicts-with: [\"deep-water\"]", "conflicts-with: [\"steady-hands\"]"),
        "conflicts-with[0]",
        "\"steady-hands\"",
        "names this mutator"
    );
    expect_one(
        edited("conflicts-with: [\"deep-water\"]", "conflicts-with: [\"long-shadows\"]"),
        "conflicts-with[0]",
        "[\"long-shadows\"]",
        "also names",
        0,
        1
    );
    expect_one(
        edited(
            "conflicts-with: [\"deep-water\"]", "conflicts-with: [\"deep-water\", \"deep-water\"]"
        ),
        "conflicts-with[1]",
        "\"deep-water\"",
        "repeats",
        1
    );
    {
        const std::string many = quoted_ids(33, "clash-");
        expect_one(
            edited("conflicts-with: [\"deep-water\"]", "conflicts-with: " + many),
            "conflicts-with",
            many,
            "must list"
        );
    }

    expect_one(
        edited("layers: [\"units\", \"weapons\"]\n", ""), "layers", "oamut:", "layers is required"
    );
    expect_one(
        edited("layers: [\"units\", \"weapons\"]", "layers: []"),
        "layers",
        "layers: []",
        "must list",
        0,
        8
    );
    expect_one(
        edited("layers: [\"units\", \"weapons\"]", "layers: units"), "layers", "units", "must list"
    );
    expect_one(
        edited("layers: [\"units\", \"weapons\"]", "layers: [\"sounds\"]"),
        "layers[0]",
        "\"sounds\"",
        "must name"
    );
    expect_one(
        edited("layers: [\"units\", \"weapons\"]", "layers: [\"units\", \"units\"]"),
        "layers[1]",
        "\"units\"",
        "repeats",
        1
    );
    expect_one(
        edited(
            "layers: [\"units\", \"weapons\"]",
            "layers: [\"units\", \"units\", \"units\", \"units\", \"units\", \"units\", \"units\", "
            "\"units\", \"units\", \"units\"]"
        ),
        "layers",
        "[\"units\", \"units\", \"units\", \"units\", \"units\", \"units\", \"units\", \"units\", "
        "\"units\", \"units\"]",
        "must list"
    );

    expect_one(with_options("options: []\n"), "options", "options: []", "must be an object", 0, 9);
    expect_one(
        with_options(
            "options:\n  Factor:\n    type: toggle\n    label: \"Announce\"\n    default: true\n"
        ),
        "options.Factor",
        "Factor",
        "Lua name"
    );
    expect_one(
        with_options(
            "options:\n  1factor:\n    type: toggle\n    label: \"Announce\"\n    default: true\n"
        ),
        "options.1factor",
        "1factor",
        "Lua name"
    );
    {
        const std::string id(33, 'a');
        expect_one(
            with_options(
                "options:\n  " + id +
                ":\n    type: toggle\n    label: \"Announce\"\n    default: true\n"
            ),
            "options." + id,
            id,
            "Lua name"
        );
    }
    expect_one(
        with_options(
            "options:\n  factor:\n    type: slider\n    label: \"Build speed\"\n    default: 1\n"
        ),
        "options.factor.type",
        "slider",
        "toggle, choice or number"
    );
    expect_one(
        with_options(
            "options:\n  factor:\n    type: choice\n    label: \"Build speed\"\n    values: [2, 4, "
            "8]\n    default: 2\n    hint: 1\n"
        ),
        "options.factor.hint",
        "hint: 1",
        "is not a key of options.factor",
        0,
        6
    );
    expect_one(
        with_options(
            "options:\n  factor:\n    type: choice\n    values: [2, 4, 8]\n    default: 2\n"
        ),
        "options.factor.label",
        "type: choice",
        "label is required"
    );
    expect_one(
        with_options(
            "options:\n  factor:\n    type: choice\n    label: \"" + std::string(65, 'L') +
            "\"\n    values: [2, 4, 8]\n    default: 2\n"
        ),
        "options.factor.label",
        "\"" + std::string(65, 'L'),
        "1 to 64"
    );
    expect_one(
        with_options(
            "options:\n  announce:\n    type: toggle\n    label: \"Announce\"\n    default: yes\n"
        ),
        "options.announce.default",
        "yes",
        "true or false"
    );
    expect_one(
        with_options(
            "options:\n  announce:\n    type: toggle\n    label: \"Announce\"\n    default: true\n "
            "   values: [1]\n"
        ),
        "options.announce.values",
        "values: [1]",
        "is not a key of options.announce",
        0,
        8
    );
    expect_one(
        with_options(
            "options:\n  factor:\n    type: choice\n    label: \"Build speed\"\n    values: [2]\n  "
            "  default: 2\n"
        ),
        "options.factor.values",
        "values: [2]",
        "must list",
        0,
        8
    );
    expect_one(
        with_options(
            "options:\n  factor:\n    type: choice\n    label: \"Build speed\"\n    values: [2, "
            "\"fast\"]\n    default: 2\n"
        ),
        "options.factor.values[1]",
        "\"fast\"",
        "all numbers or all strings"
    );
    expect_one(
        with_options(
            "options:\n  factor:\n    type: choice\n    label: \"Build speed\"\n    values: [2, "
            "2]\n    default: 2\n"
        ),
        "options.factor.values[1]",
        "values: [2, 2]",
        "repeats a value",
        0,
        12
    );
    expect_one(
        with_options(
            "options:\n  factor:\n    type: choice\n    label: \"Build speed\"\n    values: [2, "
            "4]\n    default: 3\n"
        ),
        "options.factor.default",
        "default: 3",
        "one of the values",
        0,
        9
    );
    expect_one(
        with_options(
            "options:\n  factor:\n    type: choice\n    label: \"Build speed\"\n    values: [2, "
            "4]\n    labels: [\"Only\"]\n    default: 2\n"
        ),
        "options.factor.labels",
        "[\"Only\"]",
        "one label for each value"
    );
    expect_one(
        with_options(
            "options:\n  factor:\n    type: choice\n    label: \"Build speed\"\n    values: "
            "[\"slow\", \"" +
            std::string(65, 's') + "\"]\n    default: \"slow\"\n"
        ),
        "options.factor.values[1]",
        "\"" + std::string(65, 's'),
        "1 to 32 letters, digits, _, - or ."
    );
    expect_one(
        with_options(
            "options:\n  factor:\n    type: choice\n    label: \"Build speed\"\n    values: "
            "[\"a b\", \"low\"]\n    default: \"low\"\n"
        ),
        "options.factor.values[0]",
        "\"a b\"",
        "1 to 32 letters, digits, _, - or ."
    );
    expect_one(
        with_options(
            "options:\n  factor:\n    type: choice\n    label: \"Build speed\"\n    values: "
            "[\"a,b\", \"low\"]\n    default: \"low\"\n"
        ),
        "options.factor.values[0]",
        "\"a,b\"",
        "1 to 32 letters, digits, _, - or ."
    );
    expect_one(
        with_options(
            "options:\n  factor:\n    type: choice\n    label: \"Build speed\"\n    values: "
            "[\"a=b\", \"low\"]\n    default: \"low\"\n"
        ),
        "options.factor.values[0]",
        "\"a=b\"",
        "1 to 32 letters, digits, _, - or ."
    );
    expect_one(
        with_options(
            "options:\n  factor:\n    type: choice\n    label: \"Build speed\"\n    values: "
            "[\"a:b\", \"low\"]\n    default: \"low\"\n"
        ),
        "options.factor.values[0]",
        "\"a:b\"",
        "1 to 32 letters, digits, _, - or ."
    );
    expect_one(
        with_options(
            "options:\n  factor:\n    type: choice\n    label: \"Build speed\"\n    values: "
            "[\"é\", \"low\"]\n    default: \"low\"\n"
        ),
        "options.factor.values[0]",
        "\"é\"",
        "1 to 32 letters, digits, _, - or ."
    );
    expect_one(
        with_options(
            "options:\n  factor:\n    type: choice\n    label: \"Build speed\"\n    values: "
            "[\"\", \"low\"]\n    default: \"low\"\n"
        ),
        "options.factor.values[0]",
        "\"\"",
        "1 to 32 letters, digits, _, - or ."
    );
    expect_one(
        with_options(
            "options:\n  factor:\n    type: choice\n    label: \"Build speed\"\n    values: [\"" +
            std::string(33, 'q') + "\", \"low\"]\n    default: \"low\"\n"
        ),
        "options.factor.values[0]",
        "\"" + std::string(33, 'q'),
        "1 to 32 letters, digits, _, - or ."
    );
    {
        std::string values = "[0";
        for (int index = 1; index < 17; ++index)
            values += ", " + std::to_string(index);
        values += "]";
        expect_one(
            with_options(
                "options:\n  factor:\n    type: choice\n    label: \"Build speed\"\n    values: " +
                values + "\n    default: 0\n"
            ),
            "options.factor.values",
            values,
            "must list"
        );
    }
    expect_one(
        with_options(
            "options:\n  pace:\n    type: number\n    label: \"Pace\"\n    min: 5\n    max: 4\n    "
            "step: 1\n    default: 4\n"
        ),
        "options.pace.min",
        "min: 5",
        "below max",
        0,
        5
    );
    expect_one(
        with_options(
            "options:\n  pace:\n    type: number\n    label: \"Pace\"\n    min: 0\n    max: 4\n    "
            "step: 0\n    default: 0\n"
        ),
        "options.pace.step",
        "step: 0",
        "above 0",
        0,
        6
    );
    expect_one(
        with_options(
            "options:\n  pace:\n    type: number\n    label: \"Pace\"\n    min: 0\n    max: 4\n    "
            "step: 3\n    default: 0\n"
        ),
        "options.pace.step",
        "step: 3",
        "whole number",
        0,
        6
    );
    expect_one(
        with_options(
            "options:\n  pace:\n    type: number\n    label: \"Pace\"\n    min: 0\n    max: 1001\n "
            "   step: 1\n    default: 0\n"
        ),
        "options.pace.step",
        "step: 1",
        "at most 1000",
        0,
        6
    );
    expect_one(
        with_options(
            "options:\n  pace:\n    type: number\n    label: \"Pace\"\n    min: 0\n    max: 4\n    "
            "step: 1\n    default: 1.5\n"
        ),
        "options.pace.default",
        "default: 1.5",
        "lie on a step",
        0,
        9
    );
    expect_one(
        with_options(
            "options:\n  pace:\n    type: number\n    label: \"Pace\"\n    min: 0\n    max: 4\n    "
            "default: 1\n"
        ),
        "options.pace.step",
        "type: number",
        "must be a number"
    );
    {
        std::string body = "options:\n";
        for (int index = 0; index < 17; ++index) {
            body += "  o" + std::to_string(index) + ":\n";
            body += "    type: toggle\n    label: \"On\"\n    default: true\n";
        }
        expect_one(with_options(body), "options", "o0:", "at most 16");
    }

    const std::string choice = sample_options();
    expect_one(
        with_options(choice + "strings: []\n"), "strings", "strings: []", "must be an object", 0, 9
    );
    expect_one(
        with_options(choice + "strings:\n  e:\n    name: \"X\"\n"),
        "strings.e",
        "  e:",
        "language tag",
        0,
        2
    );
    expect_one(
        with_options(choice + "strings:\n  de-:\n    name: \"X\"\n"),
        "strings.de-",
        "de-:",
        "language tag"
    );
    expect_one(
        with_options(choice + "strings:\n  de:\n    name: \"" + std::string(65, 'N') + "\"\n"),
        "strings.de.name",
        "\"" + std::string(65, 'N'),
        "1 to 64"
    );
    expect_one(
        with_options(choice + "strings:\n  de:\n    options:\n      missing: \"X\"\n"),
        "strings.de.options.missing",
        "\"X\"",
        "names no option"
    );
    expect_one(
        with_options(choice + "strings:\n  de:\n    labels:\n      announce: [\"On\"]\n"),
        "strings.de.labels.announce",
        "announce: [\"On\"]",
        "is not a choice",
        0,
        10
    );
    expect_one(
        with_options(choice + "strings:\n  de:\n    labels:\n      factor: [\"Only\"]\n"),
        "strings.de.labels.factor",
        "[\"Only\"]",
        "one label for each value"
    );
    {
        std::string body = choice + "strings:\n";
        for (int index = 0; index < 17; ++index)
            body += "  a" + std::string(1, static_cast<char>('a' + index)) + ": {}\n";
        expect_one(with_options(body), "strings", "aa:", "at most 16");
    }

    expect_one(
        edited("entry: \"scripts/steady.lua\"", "entry: \"files/steady.lua\""),
        "entry",
        "\"files/steady.lua\"",
        "not under"
    );
    expect_one(
        edited("entry: \"scripts/steady.lua\"", "entry: \"rules/steady.lua\""),
        "entry",
        "\"rules/steady.lua\"",
        "not under"
    );
    expect_one(
        edited("entry: \"scripts/steady.lua\"", "entry: \"lua/steady.lua\""),
        "entry",
        "\"lua/steady.lua\"",
        "not under"
    );
    expect_one(
        edited("entry: \"scripts/steady.lua\"", "entry: \"Files/steady.lua\""),
        "entry",
        "\"Files/steady.lua\"",
        "not under"
    );
    expect_one(
        edited("entry: \"scripts/steady.lua\"", "entry: \"scripts/steady.txt\""),
        "entry",
        "\"scripts/steady.txt\"",
        ".lua"
    );

    expect_one(
        edited(
            "legacy:\n  - name: \"First Original\"\n    prefix: \"{}\"\n    compatibility: "
            "[\"AB\"]\n    files: [\"first.sm\"]\n",
            "legacy:\n  name: \"First Original\"\n  prefix: \"{}\"\n  compatibility: [\"AB\"]\n  "
            "files: [\"first.sm\"]\n"
        ),
        "legacy",
        "name: \"First Original\"",
        "legacy is a list of records, one for each original"
    );
    expect_one(
        edited(
            "legacy:\n  - name: \"First Original\"\n    prefix: \"{}\"\n    compatibility: "
            "[\"AB\"]\n    files: [\"first.sm\"]",
            "legacy: []"
        ),
        "legacy",
        "legacy: []",
        "must list",
        0,
        8
    );
    expect_one(
        edited("prefix: \"{}\"", "prefix: {}"),
        "legacy[0].prefix",
        "prefix: {}",
        "\"{}\" or \"}{\"",
        0,
        8
    );
    expect_one(
        edited("compatibility: [\"AB\"]", "compatibility: [\"AB-\"]"),
        "legacy[0].compatibility[0]",
        "\"AB-\"",
        "letters and digits"
    );
    expect_one(
        edited("files: [\"first.sm\"]", "files: [\"a/b.sm\"]"),
        "legacy[0].files[0]",
        "\"a/b.sm\"",
        "file name"
    );
    expect_one(
        edited("compatibility: [\"AB\"]", "compatibility: []"),
        "legacy[0].compatibility",
        "compatibility: []",
        "must list",
        0,
        15
    );
    {
        std::string records = "legacy:\n";
        for (int index = 0; index < 17; ++index) {
            records += "  - name: \"N" + std::to_string(index) + "\"\n";
            records += "    prefix: \"{}\"\n    compatibility: [\"AB\"]\n    files: [\"n.sm\"]\n";
        }
        expect_one(
            edited(
                "legacy:\n  - name: \"First Original\"\n    prefix: \"{}\"\n    compatibility: "
                "[\"AB\"]\n    files: [\"first.sm\"]\n",
                records
            ),
            "legacy",
            "- name: \"N0\"",
            "must list"
        );
    }

    Manifest pace;
    OA_CHECK(read_ok(
        with_options(
            "options:\n  pace:\n    type: number\n    label: \"Pace\"\n    min: 0\n    max: 1000\n "
            "   step: 1\n    default: 0\n"
        ),
        pace
    ));
    OA_CHECK(pace.options.size() == 1);
    Manifest edges;
    OA_CHECK(read_ok(edited("works-with: [\"ridge >= 1\"]", "works-with: [\"*\"]"), edges));
    OA_CHECK(edges.works_with.size() == 1 && edges.works_with[0].id == "*");
    OA_CHECK(read_ok(
        edited(
            "description: \"Hands stay steady.\"", "description: \"" + std::string(2000, 'a') + "\""
        ),
        edges
    ));
    OA_CHECK(edges.description.size() == 2000);
}

/// Two broken rules are both reported, and a problem already listed stays.
void test_several() {
    const std::string both = edited("layers: [\"units\", \"weapons\"]", "layers: [\"sounds\"]");
    const std::string yaml_both = [&] {
        std::string text = both;
        const std::string from = "version: \"2.0\"";
        const std::size_t at = text.find(from);
        OA_CHECK(at != std::string::npos);
        if (at != std::string::npos)
            text.replace(at, from.size(), "version: 2.0");
        return text;
    }();
    Manifest manifest;
    manifest.id = "sentinel";
    std::vector<Problem> problems;
    problems.push_back(Problem{4, 4, "kept", "kept"});
    const bool read = mut::read_manifest(as_bytes(yaml_both), manifest, problems);
    OA_CHECK(!read);
    OA_CHECK(manifest.id == "sentinel");
    OA_CHECK(problems.size() == 3);
    OA_CHECK(problems[0].key == "kept");
    bool saw_version = false;
    bool saw_layer = false;
    for (std::size_t index = 1; index < problems.size(); ++index) {
        if (problems[index].key == "version")
            saw_version = problems[index].message.find("quotes") != std::string::npos;
        if (problems[index].key == "layers[0]")
            saw_layer = problems[index].message.find("sounds") != std::string::npos ||
                        problems[index].message.find("must name") != std::string::npos;
    }
    if (!saw_version || !saw_layer)
        show_problems(problems);
    OA_CHECK(saw_version && saw_layer);

    const std::string noisy = edited(
        "description: \"Hands stay steady.\"", "description: \"\\t" + std::string(2000, 'a') + "\""
    );
    std::vector<Problem> pair;
    Manifest untouched;
    untouched.id = "sentinel";
    OA_CHECK(!mut::read_manifest(as_bytes(noisy), untouched, pair));
    OA_CHECK(untouched.id == "sentinel");
    OA_CHECK(pair.size() == 2);
    OA_CHECK(pair[0].key == "description" && pair[1].key == "description");
    OA_CHECK(pair[0].line == pair[1].line && pair[0].column == pair[1].column);
}

/// A document the strict reader refuses is one problem, and the summary cuts on a boundary.
void test_document_and_summary() {
    std::vector<Problem> problems;
    Manifest manifest;
    manifest.id = "sentinel";
    OA_CHECK(!mut::read_manifest(as_bytes("oamut: 1\n\tid: steady-hands\n"), manifest, problems));
    OA_CHECK(problems.size() == 1 && problems[0].key == "document");
    OA_CHECK(problems[0].line == 2 && problems[0].column == 1);
    OA_CHECK(problems[0].message.find("tab") != std::string::npos);
    OA_CHECK(manifest.id == "sentinel");

    problems.clear();
    OA_CHECK(!mut::read_manifest(as_bytes("- item\n"), manifest, problems));
    OA_CHECK(problems.size() == 1 && problems[0].key == "document");
    OA_CHECK(problems[0].message.find("mapping") != std::string::npos);

    Manifest summary;
    OA_CHECK(read_ok(
        edited("description: \"Hands stay steady.\"", "description: \"one\\ntwo\""), summary
    ));
    OA_CHECK(mut::summary_of(summary) == "one");
    summary.description = std::string(200, 'a');
    OA_CHECK(mut::summary_of(summary).size() == 200);
    summary.description = std::string(201, 'a');
    OA_CHECK(mut::summary_of(summary).size() == 200);
    summary.description = std::string(199, 'a') + "\xc3\xa9";
    OA_CHECK(mut::summary_of(summary).size() == 199);
    summary.description = std::string(198, 'a') + "\xe4\xbd\xa0";
    OA_CHECK(mut::summary_of(summary) == std::string(198, 'a'));
    summary.description = std::string(200, 'a') + "\nrest";
    OA_CHECK(mut::summary_of(summary).size() == 200);
}

} // namespace

/// Refuses each word Lua reserves, and keeps a name that only resembles one.
void test_reserved_option_ids() {
    constexpr std::array<std::string_view, 22> reserved{
        "and",      "break",  "do",   "else", "elseif", "end",   "false", "for",
        "function", "goto",   "if",   "in",   "local",  "nil",   "not",   "or",
        "repeat",   "return", "then", "true", "until",  "while",
    };
    for (const std::string_view word : reserved) {
        const std::string quoted = "\"" + std::string{word} + "\":";
        const std::string yaml = with_options(
            "options:\n  " + quoted +
            "\n    type: toggle\n    label: \"Announce\"\n    default: true\n"
        );
        const std::string key = "options." + std::string{word};
        const std::string marker = "\n  " + quoted;
        expect_one(yaml, key, marker, "is a word Lua reserves", 0, 3);
    }

    expect_one(
        with_options(
            "options:\n  true:\n    type: toggle\n    label: \"Announce\"\n    default: true\n"
        ),
        "document",
        "true:",
        "mapping keys must be strings or integers"
    );
    expect_one(
        with_options(
            "options:\n  For:\n    type: toggle\n    label: \"Announce\"\n    default: true\n"
        ),
        "options.For",
        "For",
        "Lua name"
    );

    for (const char* name : {"format", "ends", "for_"}) {
        Manifest manifest;
        OA_CHECK(read_ok(
            with_options(
                "options:\n  " + std::string{name} +
                ":\n    type: toggle\n    label: \"Announce\"\n    default: true\n"
            ),
            manifest
        ));
        OA_CHECK(manifest.options.size() == 1);
        if (manifest.options.size() == 1)
            OA_CHECK(manifest.options[0].id == name);
    }
}

/// Reads string identifiers and number choices whose text stays within 32 bytes.
void test_choice_values() {
    OA_CHECK(mut::most_choice_value_bytes == 32);
    OA_CHECK(mut::choice_value_text("low"));
    OA_CHECK(mut::choice_value_text("x.1"));
    OA_CHECK(mut::choice_value_text("A_b-2"));
    OA_CHECK(mut::choice_value_text(std::string(32, 'n')));
    OA_CHECK(!mut::choice_value_text("a b"));
    OA_CHECK(!mut::choice_value_text(""));
    OA_CHECK(!mut::choice_value_text(std::string(33, 'n')));

    const std::string wide(32, 'n');
    Manifest identifiers;
    OA_CHECK(read_ok(
        with_options(
            "options:\n  factor:\n    type: choice\n    label: \"Slow, fast.\"\n    values: "
            "[\"low\", \"x.1\", \"A_b-2\", \"" +
            wide + "\"]\n    default: \"x.1\"\n"
        ),
        identifiers
    ));
    OA_CHECK(identifiers.options.size() == 1);
    if (identifiers.options.size() == 1) {
        const Option& option = identifiers.options[0];
        OA_CHECK(option.label == "Slow, fast.");
        OA_CHECK(option.values.size() == 4);
        if (option.values.size() == 4) {
            OA_CHECK(!option.values[0].number && option.values[0].text == "low");
            OA_CHECK(!option.values[1].number && option.values[1].text == "x.1");
            OA_CHECK(!option.values[2].number && option.values[2].text == "A_b-2");
            OA_CHECK(!option.values[3].number && option.values[3].text == wide);
            OA_CHECK(option.values[3].text.size() == mut::most_choice_value_bytes);
        }
        OA_CHECK(option.default_index == 1);
    }

    Manifest numbers;
    OA_CHECK(read_ok(
        with_options(
            "options:\n  factor:\n    type: choice\n    label: \"Build speed\"\n    values: "
            "[1e-300, 0.1234567890123]\n    default: 1e-300\n"
        ),
        numbers
    ));
    OA_CHECK(numbers.options.size() == 1);
    if (numbers.options.size() == 1) {
        const Option& option = numbers.options[0];
        OA_CHECK(option.values.size() == 2);
        if (option.values.size() == 2) {
            OA_CHECK(option.values[0].number && option.values[0].text == "1e-300");
            OA_CHECK(option.values[1].number && option.values[1].text == "0.1234567890123");
            for (const ChoiceValue& value : option.values)
                OA_CHECK(value.text.size() <= mut::most_choice_value_bytes);
        }
    }
}

int main() {
    test_versions();
    test_examples();
    test_clashes();
    test_refusals();
    test_several();
    test_document_and_summary();
    test_choice_values();
    test_reserved_option_ids();
    return oa::test::check_exit_status();
}
