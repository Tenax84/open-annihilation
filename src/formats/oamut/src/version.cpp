// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/formats/oamut/manifest.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace oa::formats::oamut {
namespace {

/// The most bytes a mutator version holds.
constexpr std::size_t most_version_bytes = 32;

/// The most bytes a version range holds.
constexpr std::size_t most_range_bytes = 64;

/// The most comparisons a version range holds.
constexpr std::size_t most_range_terms = 4;

/// The largest number a version part holds.
constexpr uint32_t version_part_max = 65535;

/// The most bytes of a condition's id.
constexpr std::size_t most_condition_id_bytes = 64;

/// The base game with no mod, as works-with names it.
constexpr std::string_view base_game = "ta-3.1c";

/// The 1-based byte a 0-based index names, for a range's error.
///
/// @param index the index
/// @return its byte, counted from 1
std::string byte_of(std::size_t index) {
    return std::to_string(index + 1);
}

/// Writes what a parse gets wrong, when the caller asked.
///
/// @param error the caller's string, or null
/// @param message what is wrong
void set_error(std::string* error, std::string message) {
    if (error != nullptr)
        *error = std::move(message);
}

/// Tells whether a byte is a decimal digit.
///
/// @param character the byte
/// @return true for 0-9
bool digit(char character) noexcept {
    return character >= '0' && character <= '9';
}

/// Reads one version part and advances past it.
///
/// @param text the version or range
/// @param[in,out] index the part's first byte
/// @param[out] part the part
/// @param[out] error what is wrong, when the part is not one
/// @return true when a part was read
bool read_version_part(
    std::string_view text, std::size_t& index, uint16_t& part, std::string& error
) {
    if (index >= text.size() || !digit(text[index])) {
        error = "expected a version part at byte " + byte_of(index);
        return false;
    }
    if (text[index] == '0' && index + 1 < text.size() && digit(text[index + 1])) {
        error = "a version part has a leading zero at byte " + byte_of(index);
        return false;
    }
    uint32_t value = 0;
    const std::size_t start = index;
    while (index < text.size() && digit(text[index])) {
        value = value * 10u + static_cast<uint32_t>(text[index] - '0');
        ++index;
        if (value > version_part_max) {
            error = "a version part is above 65535 at byte " + byte_of(start);
            return false;
        }
    }
    part = static_cast<uint16_t>(value);
    return true;
}

/// Reads one to three version parts starting at `index`.
///
/// @param text the version or range
/// @param[in,out] index the version's first byte; the first byte after it on success
/// @param[out] version the version
/// @param[out] error what is wrong, when the text there is not a version
/// @return true when a version was read
bool read_version_at(
    std::string_view text, std::size_t& index, Version& version, std::string& error
) {
    uint16_t major = 0;
    uint16_t minor = 0;
    uint16_t patch = 0;
    if (!read_version_part(text, index, major, error))
        return false;
    uint8_t count = 1;
    if (index < text.size() && text[index] == '.') {
        ++index;
        if (!read_version_part(text, index, minor, error))
            return false;
        count = 2;
        if (index < text.size() && text[index] == '.') {
            ++index;
            if (!read_version_part(text, index, patch, error))
                return false;
            count = 3;
        }
    }
    if (index < text.size() && text[index] == '.') {
        error = "a version has at most three parts at byte " + byte_of(index);
        return false;
    }
    version = Version{major, minor, patch, count};
    return true;
}

/// The operator at `index`, and how many bytes it occupies.
///
/// @param text the range
/// @param index where an operator would start
/// @param[out] comparison the operator
/// @param[out] length its bytes
/// @return true when an operator is there
bool read_operator(
    std::string_view text,
    std::size_t index,
    oamod::EngineComparison& comparison,
    std::size_t& length
) noexcept {
    const std::string_view rest = text.substr(index);
    if (rest.starts_with(">=")) {
        comparison = oamod::EngineComparison::at_least;
        length = 2;
        return true;
    }
    if (rest.starts_with("<=")) {
        comparison = oamod::EngineComparison::at_most;
        length = 2;
        return true;
    }
    if (rest.starts_with(">")) {
        comparison = oamod::EngineComparison::above;
        length = 1;
        return true;
    }
    if (rest.starts_with("<")) {
        comparison = oamod::EngineComparison::below;
        length = 1;
        return true;
    }
    if (rest.starts_with("=")) {
        comparison = oamod::EngineComparison::exactly;
        length = 1;
        return true;
    }
    return false;
}

/// The operator as it is written, for an error that follows it.
///
/// @param comparison the operator
/// @return its spelling
std::string_view operator_text(oamod::EngineComparison comparison) noexcept {
    switch (comparison) {
    case oamod::EngineComparison::at_least:
        return ">=";
    case oamod::EngineComparison::above:
        return ">";
    case oamod::EngineComparison::at_most:
        return "<=";
    case oamod::EngineComparison::below:
        return "<";
    case oamod::EngineComparison::exactly:
        return "=";
    }
    return {};
}

/// Skips ASCII spaces.
///
/// @param text the range
/// @param[in,out] index the first space, or the next other byte
void skip_spaces(std::string_view text, std::size_t& index) noexcept {
    while (index < text.size() && text[index] == ' ')
        ++index;
}

/// Tells whether one comparison holds for a version.
///
/// @param comparison the operator
/// @param version the version being tested
/// @param required the version the comparison names
/// @return true when the comparison holds
bool comparison_holds(
    oamod::EngineComparison comparison, const Version& version, const Version& required
) noexcept {
    const std::strong_ordering order = version <=> required;
    switch (comparison) {
    case oamod::EngineComparison::at_least:
        return order >= 0;
    case oamod::EngineComparison::above:
        return order > 0;
    case oamod::EngineComparison::at_most:
        return order <= 0;
    case oamod::EngineComparison::below:
        return order < 0;
    case oamod::EngineComparison::exactly:
        return order == 0;
    }
    return false;
}

/// Tells whether an id is lower-case kebab-case of 1 to 64 bytes.
///
/// Words of a-z and 0-9 joined by single hyphens, not starting or ending
/// with a hyphen. The same rule as a catalogue package key.
///
/// @param text the id
/// @return true when it is such an id
bool kebab_id(std::string_view text) noexcept {
    if (text.empty() || text.size() > most_condition_id_bytes || text.front() == '-' ||
        text.back() == '-')
        return false;
    char previous = 0;
    for (const char character : text) {
        const bool word =
            (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9');
        if (!word && character != '-')
            return false;
        if (character == '-' && previous == '-')
            return false;
        previous = character;
    }
    return true;
}

/// Tells whether a condition's id token is one the grammar accepts.
///
/// @param text the token
/// @return true for kebab-case, "*" or "ta-3.1c"
bool condition_id(std::string_view text) noexcept {
    return text == "*" || text == base_game || kebab_id(text);
}

} // namespace

std::optional<Version> parse_version(std::string_view text) noexcept {
    if (text.empty() || text.size() > most_version_bytes)
        return std::nullopt;
    Version version{};
    std::string error;
    std::size_t index = 0;
    if (!read_version_at(text, index, version, error) || index != text.size())
        return std::nullopt;
    return version;
}

std::string version_text(const Version& version) {
    std::string text = std::to_string(version.major);
    if (version.parts >= 2)
        text += "." + std::to_string(version.minor);
    if (version.parts >= 3)
        text += "." + std::to_string(version.patch);
    return text;
}

std::optional<VersionRange> parse_version_range(std::string_view text, std::string* error) {
    if (text.size() > most_range_bytes) {
        set_error(error, "a version range is longer than 64 bytes");
        return std::nullopt;
    }
    if (text.empty()) {
        set_error(error, "a version range is empty");
        return std::nullopt;
    }
    VersionRange range{};
    std::size_t index = 0;
    for (;;) {
        if (range.terms.size() == most_range_terms) {
            set_error(error, "at most 4 comparisons at byte " + byte_of(index));
            return std::nullopt;
        }
        oamod::EngineComparison comparison{};
        std::size_t length = 0;
        if (!read_operator(text, index, comparison, length)) {
            set_error(error, "expected a comparison at byte " + byte_of(index));
            return std::nullopt;
        }
        index += length;
        skip_spaces(text, index);
        const std::size_t version_at = index;
        const bool version_started = version_at < text.size() && digit(text[version_at]);
        Version version{};
        std::string version_error;
        if (!read_version_at(text, index, version, version_error)) {
            if (!version_started)
                set_error(
                    error,
                    "expected a version after '" + std::string{operator_text(comparison)} +
                        "' at byte " + byte_of(version_at)
                );
            else
                set_error(error, std::move(version_error));
            return std::nullopt;
        }
        range.terms.emplace_back(comparison, version);
        if (index == text.size())
            return range;
        const std::size_t after = index;
        skip_spaces(text, index);
        if (index >= text.size() || text[index] != ',') {
            set_error(error, "unexpected text at byte " + byte_of(after));
            return std::nullopt;
        }
        ++index;
        skip_spaces(text, index);
        if (index >= text.size()) {
            set_error(error, "expected a comparison at byte " + byte_of(index));
            return std::nullopt;
        }
    }
}

bool version_in_range(const VersionRange& range, const Version& version) noexcept {
    for (const auto& [comparison, required] : range.terms) {
        if (!comparison_holds(comparison, version, required))
            return false;
    }
    return true;
}

std::optional<Condition> parse_condition(std::string_view text, std::string* error) {
    if (text.empty()) {
        set_error(error, "a condition is empty");
        return std::nullopt;
    }
    std::size_t end = 0;
    while (end < text.size() && text[end] != ' ')
        ++end;
    const std::string_view id = text.substr(0, end);
    if (!condition_id(id)) {
        set_error(error, "expected a package id at byte 1");
        return std::nullopt;
    }
    std::size_t index = end;
    skip_spaces(text, index);
    Condition condition{};
    condition.id = std::string{id};
    if (index == text.size())
        return condition;
    if (id == "*" || id == base_game) {
        set_error(error, std::string{id} + " takes no version range");
        return std::nullopt;
    }
    const std::string_view range_text = text.substr(index);
    std::string range_error;
    std::optional<VersionRange> range = parse_version_range(range_text, &range_error);
    if (!range) {
        set_error(error, std::move(range_error));
        return std::nullopt;
    }
    condition.range_text = std::string{range_text};
    condition.range = std::move(*range);
    return condition;
}

bool condition_met(const Condition& condition, std::string_view id, std::string_view version_text) {
    if (condition.id != id)
        return false;
    if (!condition.range.has_value())
        return true;
    const std::optional<Version> version = parse_version(version_text);
    if (!version.has_value())
        return false;
    return version_in_range(*condition.range, *version);
}

} // namespace oa::formats::oamut
