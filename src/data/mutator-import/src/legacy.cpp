// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Turns TA: Mutation files into mutator packages. The .tam is Windows-1252.
// Zip names stay as the streamed reader decoded them.

#include "oa/data/mutator_import/legacy.hpp"

#include "oa/data/mutators/allow_list.hpp"
#include "oa/data/script_host/script_host.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/formats/oamod/package_keys.hpp"
#include "oa/formats/soft_mutator.hpp"
#include "oa/formats/zip.hpp"
#include "oa/formats/zip/stream.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <exception>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace oa::data::mutator_import {
namespace {

namespace oamut = oa::formats::oamut;
namespace soft = oa::formats::soft_mutator;
namespace zip = oa::formats::zip;

/// The manifest reader allows these and does not publish them.
constexpr std::size_t most_legacy_name_bytes = 128;
constexpr std::size_t most_author_name_bytes = 128;
constexpr std::size_t most_code_bytes = 8;
constexpr std::size_t most_legacy_files = 16;
constexpr std::size_t most_file_name_bytes = 255;

/// The characters of Windows-1252 at 0x80-0x9F. The same values as
/// code_page_marks in src/present/src/game_text.cpp. The data layer does not
/// link present, so the table is repeated. The five undefined bytes stay the
/// control characters of the same value.
constexpr std::array<char32_t, 32> code_page_marks{
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};

/// One code point of Windows-1252.
///
/// Bytes 0x00-0x7F and 0xA0-0xFF are the same character. The rest use the table.
///
/// @param byte the byte
/// @return the character
char32_t code_page_character(uint8_t byte) noexcept {
    if (byte >= 0x80 && byte < 0xA0)
        return code_page_marks[byte - 0x80];
    return byte;
}

/// Appends one Unicode character as UTF-8.
///
/// @param[in,out] out the text
/// @param character the character
void append_utf8(std::string& out, char32_t character) {
    if (character < 0x80) {
        out.push_back(static_cast<char>(character));
        return;
    }
    if (character < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (character >> 6)));
        out.push_back(static_cast<char>(0x80 | (character & 0x3F)));
        return;
    }
    out.push_back(static_cast<char>(0xE0 | (character >> 12)));
    out.push_back(static_cast<char>(0x80 | ((character >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (character & 0x3F)));
}

/// Decodes Windows-1252 as UTF-8.
///
/// @param bytes the file
/// @return the text
std::string decode_code_page(std::span<const uint8_t> bytes) {
    std::string text;
    text.reserve(bytes.size());
    for (const uint8_t byte : bytes)
        append_utf8(text, code_page_character(byte));
    return text;
}

/// Tells whether two texts are equal with ASCII letters folded.
///
/// @param text one text
/// @param other the other
/// @return true when they are equal
bool same_ascii(std::string_view text, std::string_view other) noexcept {
    if (text.size() != other.size())
        return false;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const auto fold = [](unsigned char byte) noexcept {
            return byte >= 'A' && byte <= 'Z' ? static_cast<unsigned char>(byte - 'A' + 'a') : byte;
        };
        if (fold(static_cast<unsigned char>(text[index])) !=
            fold(static_cast<unsigned char>(other[index])))
            return false;
    }
    return true;
}

/// Folds ASCII letters.
///
/// @param text the text
/// @return the folded text
std::string lower_ascii(std::string_view text) {
    std::string folded(text);
    for (char& character : folded) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte >= 'A' && byte <= 'Z')
            character = static_cast<char>(byte - 'A' + 'a');
    }
    return folded;
}

/// Tells whether a byte is an ASCII letter or digit.
///
/// @param byte the byte
/// @return true for a letter or a digit
bool letter_or_digit(unsigned char byte) noexcept {
    return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
           (byte >= '0' && byte <= '9');
}

/// Tells whether an id is lower-case kebab-case of 1 to 64 bytes.
///
/// @param text the id
/// @return true when the manifest would accept it
bool mutator_id(std::string_view text) noexcept {
    if (text.empty() || text.size() > oamut::most_id_bytes || text.front() == '-' ||
        text.back() == '-')
        return false;
    char previous = 0;
    for (const char character : text) {
        const auto byte = static_cast<unsigned char>(character);
        const bool word = (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9');
        if (!word && character != '-')
            return false;
        if (character == '-' && previous == '-')
            return false;
        previous = character;
    }
    return true;
}

/// Tells whether a works-with entry is `*`, `ta-3.1c` or a mutator id.
///
/// @param text the entry
/// @return true when the manifest would accept it
bool works_with_id(std::string_view text) noexcept {
    return text == "*" || text == "ta-3.1c" || mutator_id(text);
}

/// Tells whether a character breaks a line of plain text.
///
/// A C0 control, delete, a C1 control, a line separator or a paragraph
/// separator. U+000A is a control here; the caller may allow it.
///
/// @param character the character
/// @return true when plain text refuses it
bool breaks_line(char32_t character) noexcept {
    return character < 0x20 || (character >= 0x7F && character <= 0x9F) || character == 0x2028 ||
           character == 0x2029;
}

/// Reads one UTF-8 character.
///
/// An ill-formed byte is consumed on its own and reported as not UTF-8.
///
/// @param text the text
/// @param[in,out] index the first byte; moved past the character
/// @param[out] character the character, or the raw byte when it is not UTF-8
/// @return true when a well-formed character was read
bool next_utf8(std::string_view text, std::size_t& index, char32_t& character) noexcept {
    if (index >= text.size())
        return false;
    const auto lead = static_cast<unsigned char>(text[index]);
    std::size_t length = 1;
    char32_t value = lead;
    if (lead >= 0x80) {
        if ((lead & 0xE0U) == 0xC0U) {
            length = 2;
            value = lead & 0x1FU;
        } else if ((lead & 0xF0U) == 0xE0U) {
            length = 3;
            value = lead & 0x0FU;
        } else if ((lead & 0xF8U) == 0xF0U) {
            length = 4;
            value = lead & 0x07U;
        } else {
            character = lead;
            ++index;
            return false;
        }
    }
    if (index + length > text.size()) {
        character = lead;
        ++index;
        return false;
    }
    for (std::size_t extra = 1; extra < length; ++extra) {
        const auto byte = static_cast<unsigned char>(text[index + extra]);
        if ((byte & 0xC0U) != 0x80U) {
            character = lead;
            ++index;
            return false;
        }
        value = (value << 6) | (byte & 0x3FU);
    }
    const bool overlong = (length == 2 && value < 0x80) || (length == 3 && value < 0x800) ||
                          (length == 4 && value < 0x10000);
    if (overlong || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
        character = lead;
        ++index;
        return false;
    }
    character = value;
    index += length;
    return true;
}

/// Tells whether a text holds a control character.
///
/// Ill-formed UTF-8 counts as a control character.
///
/// @param text the text
/// @param allow_line_feed whether U+000A may appear
/// @return true when a control character other than that line feed appears
bool has_control(std::string_view text, bool allow_line_feed) noexcept {
    std::size_t index = 0;
    while (index < text.size()) {
        char32_t character = 0;
        if (!next_utf8(text, index, character))
            return true;
        if (allow_line_feed && character == U'\n')
            continue;
        if (breaks_line(character))
            return true;
    }
    return false;
}

/// Cuts a text at a UTF-8 boundary.
///
/// @param text the text
/// @param most the most bytes
/// @return the text, shortened when it was longer
std::string cut_utf8(std::string text, std::size_t most) {
    if (text.size() <= most)
        return text;
    std::size_t cut = most;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0U) == 0x80U)
        --cut;
    text.resize(cut);
    return text;
}

/// Drops spaces, tabs and asterisks from both ends.
///
/// @param text the text
/// @return the middle
std::string trim_decorations(std::string_view text) {
    auto trimmed = [](unsigned char byte) noexcept {
        return byte == ' ' || byte == '\t' || byte == '*';
    };
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && trimmed(static_cast<unsigned char>(text[begin])))
        ++begin;
    while (end > begin && trimmed(static_cast<unsigned char>(text[end - 1])))
        --end;
    return std::string{text.substr(begin, end - begin)};
}

/// Drops spaces, tabs and CR from both ends.
///
/// @param text the text
/// @return the middle
std::string_view trim_space(std::string_view text) noexcept {
    auto space = [](unsigned char byte) noexcept {
        return byte == ' ' || byte == '\t' || byte == '\r';
    };
    while (!text.empty() && space(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    while (!text.empty() && space(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);
    return text;
}

/// Appends the ASCII a letter becomes, when the character is a letter or a digit.
///
/// Latin-1 letters lose their accents. U+00E6 and U+00C6 become `ae`, U+00DF
/// becomes `ss`, and U+00FE and U+00DE become `th`. U+00D7 and U+00F7 are not
/// letters.
///
/// @param[in,out] out the id so far
/// @param character the character
/// @return true when characters were appended
bool append_word(std::string& out, char32_t character) {
    if (character >= '0' && character <= '9') {
        out.push_back(static_cast<char>(character));
        return true;
    }
    if (character >= 'A' && character <= 'Z') {
        out.push_back(static_cast<char>(character - 'A' + 'a'));
        return true;
    }
    if (character >= 'a' && character <= 'z') {
        out.push_back(static_cast<char>(character));
        return true;
    }
    switch (character) {
    case 0x00C0:
    case 0x00C1:
    case 0x00C2:
    case 0x00C3:
    case 0x00C4:
    case 0x00C5:
    case 0x00E0:
    case 0x00E1:
    case 0x00E2:
    case 0x00E3:
    case 0x00E4:
    case 0x00E5:
        out.push_back('a');
        return true;
    case 0x00C6:
    case 0x00E6:
        out.append("ae");
        return true;
    case 0x00C7:
    case 0x00E7:
        out.push_back('c');
        return true;
    case 0x00C8:
    case 0x00C9:
    case 0x00CA:
    case 0x00CB:
    case 0x00E8:
    case 0x00E9:
    case 0x00EA:
    case 0x00EB:
        out.push_back('e');
        return true;
    case 0x00CC:
    case 0x00CD:
    case 0x00CE:
    case 0x00CF:
    case 0x00EC:
    case 0x00ED:
    case 0x00EE:
    case 0x00EF:
        out.push_back('i');
        return true;
    case 0x00D0:
    case 0x00F0:
        out.push_back('d');
        return true;
    case 0x00D1:
    case 0x00F1:
        out.push_back('n');
        return true;
    case 0x00D2:
    case 0x00D3:
    case 0x00D4:
    case 0x00D5:
    case 0x00D6:
    case 0x00D8:
    case 0x00F2:
    case 0x00F3:
    case 0x00F4:
    case 0x00F5:
    case 0x00F6:
    case 0x00F8:
        out.push_back('o');
        return true;
    case 0x00D9:
    case 0x00DA:
    case 0x00DB:
    case 0x00DC:
    case 0x00F9:
    case 0x00FA:
    case 0x00FB:
    case 0x00FC:
        out.push_back('u');
        return true;
    case 0x00DD:
    case 0x00FD:
    case 0x00FF:
        out.push_back('y');
        return true;
    case 0x00DE:
    case 0x00FE:
        out.append("th");
        return true;
    case 0x00DF:
        out.append("ss");
        return true;
    default:
        return false;
    }
}

/// The directory and the last component of a legacy path.
struct Location {
    std::string directory; ///< everything before the last slash; empty at the top
    std::string name;      ///< the last component
};

/// Splits a legacy path. A backslash is a slash.
///
/// @param raw the name as given
/// @return the directory and the file name
Location locate(std::string_view raw) {
    std::string text{raw};
    for (char& character : text) {
        if (character == '\\')
            character = '/';
    }
    const auto slash = text.rfind('/');
    if (slash == std::string::npos)
        return {"", std::move(text)};
    return {text.substr(0, slash), text.substr(slash + 1)};
}

/// Tells whether a file name ends in a suffix, ignoring ASCII case.
///
/// @param name the file name
/// @param suffix the suffix, including the dot
/// @return true when it ends that way
bool ends_with_ascii(std::string_view name, std::string_view suffix) noexcept {
    return name.size() >= suffix.size() &&
           same_ascii(name.substr(name.size() - suffix.size()), suffix);
}

/// Tells whether a legacy name is a .tam.
///
/// @param name the name or path
/// @return true when the last component ends in .tam
bool is_tam_name(std::string_view name) {
    return ends_with_ascii(locate(name).name, ".tam");
}

/// Tells whether a zip entry is a file the import reads.
///
/// @param name the entry name
/// @return true for a `.tam`, a `.sm` or a `.ccx`
bool payload_name(std::string_view name) {
    // locate returns by value. The name has to outlive the suffix checks.
    const Location location = locate(name);
    return ends_with_ascii(location.name, ".tam") || ends_with_ascii(location.name, ".sm") ||
           ends_with_ascii(location.name, ".ccx");
}

/// The stem of a legacy file name, without a `{}` or `}{` prefix or a suffix.
///
/// @param name the file name
/// @return the stem
std::string stem_of(std::string_view name) {
    std::string_view stem = name;
    if (stem.starts_with("}{"))
        stem.remove_prefix(2);
    else if (stem.starts_with("{}"))
        stem.remove_prefix(2);
    const auto dot = stem.rfind('.');
    if (dot != std::string_view::npos)
        stem = stem.substr(0, dot);
    return std::string{stem};
}

/// Formats a description and says whether it was cut.
///
/// @param description the description line
/// @param[out] cut true when the text was longer than 2,000 bytes
/// @return the manifest text
std::string format_description(std::string_view description, bool& cut) {
    std::string raw;
    for (std::size_t index = 0; index < description.size();) {
        if (description[index] == '#') {
            std::size_t marks = 0;
            while (index < description.size() && description[index] == '#') {
                ++marks;
                ++index;
            }
            raw.append((marks + 1) / 2, '\n');
            continue;
        }
        if (description[index] == ' ' || description[index] == '\t') {
            const std::size_t start = index;
            bool tab = false;
            while (index < description.size() &&
                   (description[index] == ' ' || description[index] == '\t')) {
                if (description[index] == '\t')
                    tab = true;
                ++index;
            }
            if (tab)
                raw.push_back(' ');
            else
                raw.append(description.substr(start, index - start));
            continue;
        }
        raw.push_back(description[index]);
        ++index;
    }
    std::string text;
    std::size_t start = 0;
    bool first = true;
    for (std::size_t index = 0; index <= raw.size(); ++index) {
        if (index != raw.size() && raw[index] != '\n')
            continue;
        std::string_view line = trim_space(std::string_view{raw}.substr(start, index - start));
        if (!first)
            text.push_back('\n');
        first = false;
        text.append(line);
        start = index + 1;
    }
    cut = text.size() > oamut::most_description_bytes;
    return cut_utf8(std::move(text), oamut::most_description_bytes);
}

/// Matches one credit phrase at the start of a line.
///
/// Words may have spaces or tabs between them. After the phrase the line must
/// end, or continue with a space or a colon, so `by` does not match `bypass`.
///
/// @param line the last line
/// @param phrase the phrase, in lower case, words split by one space
/// @param[out] name_at where the name starts
/// @return true when the line starts with the phrase
bool phrase_at(std::string_view line, std::string_view phrase, std::size_t& name_at) noexcept {
    std::size_t cursor = 0;
    std::size_t phrase_at_index = 0;
    while (phrase_at_index < phrase.size()) {
        if (phrase[phrase_at_index] == ' ') {
            if (cursor >= line.size())
                return false;
            const auto byte = static_cast<unsigned char>(line[cursor]);
            if (byte != ' ' && byte != '\t')
                return false;
            while (cursor < line.size()) {
                const auto more = static_cast<unsigned char>(line[cursor]);
                if (more != ' ' && more != '\t')
                    break;
                ++cursor;
            }
            ++phrase_at_index;
            continue;
        }
        if (cursor >= line.size())
            return false;
        auto have = static_cast<unsigned char>(line[cursor]);
        auto want = static_cast<unsigned char>(phrase[phrase_at_index]);
        if (have >= 'A' && have <= 'Z')
            have = static_cast<unsigned char>(have - 'A' + 'a');
        if (want >= 'A' && want <= 'Z')
            want = static_cast<unsigned char>(want - 'A' + 'a');
        if (have != want)
            return false;
        ++cursor;
        ++phrase_at_index;
    }
    if (cursor < line.size()) {
        const auto byte = static_cast<unsigned char>(line[cursor]);
        if (byte != ' ' && byte != '\t' && byte != ':')
            return false;
    }
    while (cursor < line.size()) {
        const auto byte = static_cast<unsigned char>(line[cursor]);
        if (byte != ' ' && byte != '\t')
            break;
        ++cursor;
    }
    if (cursor < line.size() && line[cursor] == ':') {
        ++cursor;
        while (cursor < line.size()) {
            const auto byte = static_cast<unsigned char>(line[cursor]);
            if (byte != ' ' && byte != '\t')
                break;
            ++cursor;
        }
    }
    name_at = cursor;
    return true;
}

/// Finds a companion by directory and file name, ignoring ASCII case.
///
/// @param files the legacy files
/// @param directory the .tam's directory
/// @param name the file name
/// @return the index, or nothing
std::optional<std::size_t> find_companion(
    std::span<const LegacyFile> files, std::string_view directory, std::string_view name
) {
    for (std::size_t index = 0; index < files.size(); ++index) {
        const Location location = locate(files[index].name);
        if (same_ascii(location.directory, directory) && same_ascii(location.name, name))
            return index;
    }
    return std::nullopt;
}

/// Tells whether a compatibility code can be stored.
///
/// @param code the code
/// @return true for `*` or 1 to 8 letters and digits
bool code_ok(std::string_view code) noexcept {
    if (code.empty() || code.size() > most_code_bytes)
        return false;
    if (code == "*")
        return true;
    for (const char character : code) {
        if (!letter_or_digit(static_cast<unsigned char>(character)))
            return false;
    }
    return true;
}

/// Tells whether a legacy file name can be stored.
///
/// @param name the file name
/// @return true when it is 1 to 255 bytes, with no slash and no control character
bool file_name_ok(std::string_view name) noexcept {
    return !name.empty() && name.size() <= most_file_name_bytes &&
           name.find('/') == std::string_view::npos && name.find('\\') == std::string_view::npos &&
           !has_control(name, false);
}

/// The caller's mapping for a code, when it is not a built-in code.
///
/// @param options the caller's choices
/// @param code the code, as written
/// @return the works-with entry, or an empty view
std::string_view mapped_entry(const ImportOptions& options, std::string_view code) noexcept {
    for (const auto& pair : options.code_map) {
        if (same_ascii(pair.first, code))
            return pair.second;
    }
    return {};
}

/// Checks the caller's choices once for the whole run.
///
/// @param options the choices
/// @param[out] problems a problem, when a choice is refused
/// @return true when the import may continue
bool options_ok(const ImportOptions& options, std::vector<std::string>& problems) {
    if (!options.id.empty() && !mutator_id(options.id)) {
        problems.emplace_back("the id override is not lower-case kebab-case");
        return false;
    }
    if (!options.version.empty() && !oamut::parse_version(options.version)) {
        problems.emplace_back("the version is not a mutator version");
        return false;
    }
    if (options.tags.size() > oamut::most_tags) {
        problems.emplace_back("there are more than 4 tags");
        return false;
    }
    for (const std::string& tag : options.tags) {
        if (!oa::formats::oamod::tag_valid(tag)) {
            problems.emplace_back("a tag is not lower-case kebab-case");
            return false;
        }
    }
    if (options.engine.empty()) {
        problems.emplace_back("the engine requirement is empty");
        return false;
    }
    if (!oa::formats::oamod::parse_engine_range(options.engine)) {
        problems.emplace_back("the engine requirement cannot be read");
        return false;
    }
    for (const auto& pair : options.code_map) {
        if (same_ascii(pair.first, "OTA") || same_ascii(pair.first, "TACC") || pair.first == "*")
            continue;
        if (!works_with_id(pair.second)) {
            problems.emplace_back("a code mapping is not a works-with id");
            return false;
        }
    }
    return true;
}

/// An id with a numeric suffix, shortened so the whole id is at most 64 bytes.
///
/// @param base the shared id
/// @param number the suffix, from 2
/// @return the id
std::string with_suffix(std::string_view base, int number) {
    const std::string suffix = "-" + std::to_string(number);
    std::string trimmed{base};
    if (trimmed.size() + suffix.size() > oamut::most_id_bytes) {
        const std::size_t room = oamut::most_id_bytes - suffix.size();
        if (trimmed.size() > room)
            trimmed.resize(room);
        const auto hyphen = trimmed.rfind('-');
        if (hyphen != std::string::npos && hyphen > 0)
            trimmed.resize(hyphen);
        while (!trimmed.empty() && trimmed.back() == '-')
            trimmed.pop_back();
        if (trimmed.empty())
            trimmed = "mutator";
        if (trimmed.size() + suffix.size() > oamut::most_id_bytes)
            trimmed.resize(oamut::most_id_bytes - suffix.size());
        while (!trimmed.empty() && trimmed.back() == '-')
            trimmed.pop_back();
    }
    trimmed += suffix;
    return trimmed;
}

/// Reserves an id, adding -2, -3 and so on when the base is already used.
///
/// @param base the id from the title or the override
/// @param[in,out] used ids already given out
/// @param[out] suffixed true when a suffix was added
/// @return the id, or an empty string when none could be made
std::string unique_id(const std::string& base, std::set<std::string>& used, bool& suffixed) {
    suffixed = false;
    if (mutator_id(base) && !used.contains(base)) {
        used.insert(base);
        return base;
    }
    suffixed = true;
    for (int number = 2; number < 10000; ++number) {
        const std::string candidate = with_suffix(base, number);
        if (mutator_id(candidate) && !used.contains(candidate)) {
            used.insert(candidate);
            return candidate;
        }
    }
    return {};
}

/// Marks the layers one rule target reaches.
///
/// @param target the block's folder and suffix
/// @param[in,out] reached one flag per layer, in layer order
void reach_target(const soft::Target& target, bool reached[]) {
    std::string_view folder = target.folder;
    const auto slash = folder.find('/');
    if (slash != std::string_view::npos)
        folder = folder.substr(0, slash);
    if (folder == "units")
        reached[static_cast<std::size_t>(oamut::Layer::units)] = true;
    else if (folder == "weapons")
        reached[static_cast<std::size_t>(oamut::Layer::weapons)] = true;
    else if (folder == "features")
        reached[static_cast<std::size_t>(oamut::Layer::features)] = true;
    else if (folder == "maps")
        reached[static_cast<std::size_t>(oamut::Layer::maps)] = true;
    else if (folder == "gamedata") {
        constexpr std::string_view sidedata = "sidedata.tdf";
        if (sidedata.ends_with(target.extension))
            reached[static_cast<std::size_t>(oamut::Layer::sides)] = true;
        if (target.extension != sidedata)
            reached[static_cast<std::size_t>(oamut::Layer::tables)] = true;
    } else {
        reached[static_cast<std::size_t>(oamut::Layer::tables)] = true;
    }
}

/// The stem of a unit file path, lower case, when the path is under units/.
///
/// @param path the archive path
/// @return the stem, or an empty string when the path is not a unit file
std::string unit_stem(std::string_view path) {
    std::string text{path};
    for (char& character : text) {
        if (character == '\\')
            character = '/';
    }
    const auto slash = text.find('/');
    if (slash == std::string::npos || slash == 0)
        return {};
    if (!same_ascii(std::string_view{text}.substr(0, slash), "units"))
        return {};
    const auto last = text.rfind('/');
    std::string file = text.substr(last + 1);
    const auto dot = file.rfind('.');
    if (dot != std::string::npos)
        file.resize(dot);
    return lower_ascii(file);
}

/// One archive entry, kept or left out.
struct HeldEntry {
    std::string path;           ///< the path inside the archive
    std::vector<uint8_t> bytes; ///< the decoded bytes
    bool keep{false};           ///< true when the path may be written
    std::string rule;           ///< the allow-list rule, when the entry is left out
};

/// One archive opened for the allow-list.
struct HeldArchive {
    std::string name;               ///< the legacy file name
    std::vector<uint8_t> original;  ///< the archive as it was
    std::vector<HeldEntry> entries; ///< its files, in archive order
};

/// Opens one archive and lists its entries.
///
/// @param name the legacy file name, for a problem
/// @param bytes the archive
/// @param[out] held the entries
/// @param[out] problem what is wrong, when the archive cannot be read
/// @return true when the entries were read
bool open_archive(
    const std::string& name,
    const std::vector<uint8_t>& bytes,
    HeldArchive& held,
    std::string& problem
) {
    held.name = name;
    held.original = bytes;
    auto opened = oa::HpiArchive::open(std::vector<uint8_t>(bytes));
    if (!opened.ok() || !opened.value) {
        problem = "cannot read " + name;
        return false;
    }
    const oa::HpiArchive& archive = *opened.value;
    for (const oa::ArchiveEntry& entry : archive.entries()) {
        auto decoded = archive.read(entry.path);
        if (!decoded.ok() || !decoded.value) {
            problem = "cannot read " + name;
            return false;
        }
        HeldEntry held_entry;
        held_entry.path = entry.path;
        held_entry.bytes = std::move(*decoded.value);
        held.entries.push_back(std::move(held_entry));
    }
    return true;
}

/// Leaves protected entries out of each archive.
///
/// An archive with none is kept byte for byte. An archive with some is
/// written again from the entries that remain. An archive with none left is
/// not written.
///
/// @param archives the opened archives
/// @param[out] warnings one line per entry left out
/// @param[out] written the archives that still have an entry
/// @param[out] problem what is wrong, when an archive cannot be written
/// @return true when the archives were filtered
bool filter_archives(
    std::vector<HeldArchive>& archives,
    std::vector<std::string>& warnings,
    std::vector<std::vector<uint8_t>>& written,
    std::string& problem
) {
    std::set<std::string> stems;
    for (const HeldArchive& archive : archives) {
        for (const HeldEntry& entry : archive.entries) {
            const std::string stem = unit_stem(entry.path);
            if (!stem.empty())
                stems.insert(stem);
        }
    }
    oa::data::mutators::PathContext context;
    context.is_unit_name = [&stems](std::string_view unit) {
        return stems.contains(lower_ascii(unit));
    };
    for (HeldArchive& archive : archives) {
        bool omitted = false;
        for (HeldEntry& entry : archive.entries) {
            const oa::data::mutators::PathVerdict verdict =
                oa::data::mutators::check_mutable_path(entry.path, context);
            if (verdict.allowed()) {
                entry.keep = true;
                continue;
            }
            omitted = true;
            entry.rule = std::string{oa::data::mutators::rule_name(verdict.rule)};
            warnings.push_back(
                "left out " + archive.name + ": " + entry.path + " (" + entry.rule + ")"
            );
        }
        if (!omitted) {
            written.push_back(std::move(archive.original));
            continue;
        }
        std::vector<oa::HpiWriteFile> kept;
        for (HeldEntry& entry : archive.entries) {
            if (!entry.keep)
                continue;
            oa::HpiWriteFile file;
            file.path = entry.path;
            file.bytes = std::move(entry.bytes);
            kept.push_back(std::move(file));
        }
        if (kept.empty())
            continue;
        try {
            written.push_back(oa::write_hpi(kept));
        } catch (const std::exception&) {
            problem = "cannot write " + archive.name;
            return false;
        }
    }
    return true;
}

/// Escapes a title for one Lua comment line.
///
/// A backslash or a quote is escaped. A line break becomes a space.
///
/// @param title the title
/// @return the comment text
std::string lua_title(std::string_view title) {
    std::string text;
    for (const char character : title) {
        if (character == '\\' || character == '"') {
            text.push_back('\\');
            text.push_back(character);
        } else if (character == '\n' || character == '\r') {
            text.push_back(' ');
        } else {
            text.push_back(character);
        }
    }
    return text;
}

/// The script that adds the imported files.
///
/// @param title the original title
/// @param id the mutator id
/// @param files whether the plan adds the files folder
/// @param rules whether the plan runs a rule file
/// @return the script
std::string mutator_script(std::string_view title, std::string_view id, bool files, bool rules) {
    std::string script;
    script += "-- Made by oa-tool mutator import from the TA: Mutation mutator \"";
    script += lua_title(title);
    script += "\".\n";
    script +=
        "-- Its rule file is in rules/ and its archives in files/; files OA protects are left "
        "out.\n";
    script += "function mutate(ctx)\n";
    script += "  return {\n";
    if (files) {
        script += "    files = { add = { \"files\" } },\n";
    }
    if (rules) {
        script += "    rules = { sm \"rules/";
        script += id;
        script += ".sm\" },\n";
    }
    script += "  }\n";
    script += "end\n";
    return script;
}

/// Copies text into a byte vector.
///
/// @param text the text
/// @return the bytes
std::vector<uint8_t> bytes_of(std::string_view text) {
    return {text.begin(), text.end()};
}

/// Imports one .tam. Companions that are found are marked claimed.
///
/// @param files the legacy files
/// @param index the .tam's index
/// @param options the caller's choices
/// @param[in,out] used ids already given out
/// @param[in,out] claimed one flag per legacy file
/// @param[out] imported the package, when the mutator is imported
/// @param[out] problem what is wrong, when it is refused
/// @return true when `imported` was filled
bool import_one(
    std::span<const LegacyFile> files,
    std::size_t index,
    const ImportOptions& options,
    std::set<std::string>& used,
    std::vector<char>& claimed,
    Imported& imported,
    std::string& problem
) {
    const Location location = locate(files[index].name);
    const std::string& shown = location.name;
    if (files[index].bytes.size() > most_tam_bytes)
        return problem = shown + ": the .tam is larger than 64 KiB", false;
    Tam tam;
    std::string tam_problem;
    if (!read_tam(files[index].bytes, tam, tam_problem))
        return problem = shown + ": " + tam_problem, false;

    const bool hard = shown.starts_with("}{");
    const bool soft = shown.starts_with("{}");
    if (!hard && !soft)
        return problem = shown + ": the name does not start with {} or }{", false;
    const std::string prefix = hard ? "}{" : "{}";
    const std::string stem = stem_of(shown);
    imported.warnings = tam.warnings;

    std::vector<std::string> legacy_names;
    if (!file_name_ok(shown))
        return problem = shown + ": a file name cannot be stored", false;
    legacy_names.push_back(shown);

    const std::vector<uint8_t>* rule_bytes = nullptr;
    std::string rule_name;
    if (soft) {
        rule_name = prefix + stem + ".sm";
        const std::optional<std::size_t> found =
            find_companion(files, location.directory, rule_name);
        if (!found)
            return problem = shown + ": missing rule file " + rule_name, false;
        claimed[*found] = 1;
        const std::string stored = locate(files[*found].name).name;
        if (!file_name_ok(stored))
            return problem = shown + ": a file name cannot be stored", false;
        legacy_names.push_back(stored);
        rule_bytes = &files[*found].bytes;
        if (!tam.archive_name.empty()) {
            imported.warnings.emplace_back("ignored an extra line after the companion count");
        }
    }

    std::vector<const LegacyFile*> archives;
    std::vector<std::string> archive_names;
    if (hard) {
        std::string main = prefix + stem + ".ccx";
        if (!tam.archive_name.empty())
            main = locate(tam.archive_name).name;
        archive_names.push_back(std::move(main));
    }
    for (int number = 1; number <= tam.companion_count; ++number)
        archive_names.push_back(prefix + stem + "(" + std::to_string(number) + ").ccx");
    for (const std::string& archive_name : archive_names) {
        const std::optional<std::size_t> found =
            find_companion(files, location.directory, archive_name);
        if (!found)
            return problem = shown + ": missing archive " + archive_name, false;
        claimed[*found] = 1;
        const std::string stored = locate(files[*found].name).name;
        if (!file_name_ok(stored))
            return problem = shown + ": a file name cannot be stored", false;
        bool repeated = false;
        for (const LegacyFile* earlier : archives) {
            if (earlier == &files[*found])
                repeated = true;
        }
        if (repeated)
            continue;
        bool named = false;
        for (const std::string& earlier : legacy_names) {
            if (same_ascii(earlier, stored))
                named = true;
        }
        if (!named)
            legacy_names.push_back(stored);
        archives.push_back(&files[*found]);
    }
    if (legacy_names.size() > most_legacy_files)
        return problem = shown + ": too many legacy file names", false;

    soft::RuleFile rules;
    if (rule_bytes != nullptr) {
        if (rule_bytes->size() > soft::max_input_bytes)
            return problem = shown + ": the rule file is too long", false;
        const soft::ParseResult parsed = soft::parse_rules(*rule_bytes);
        if (parsed.has_errors()) {
            for (const soft::Diagnostic& diagnostic : parsed.diagnostics) {
                if (diagnostic.severity != soft::Severity::error)
                    continue;
                problem =
                    shown + ": line " + std::to_string(diagnostic.line) + ": " + diagnostic.message;
                return false;
            }
            problem = shown + ": the rule file was refused";
            return false;
        }
        rules = parsed.rules;
    }

    if (tam.codes.empty() || tam.codes.size() > oamut::most_legacy_codes)
        return problem = shown + ": the compatibility codes cannot be stored", false;
    for (const std::string& code : tam.codes) {
        if (!code_ok(code))
            return problem = shown + ": compatibility code " + code + " cannot be stored", false;
    }

    std::vector<HeldArchive> held;
    for (const LegacyFile* archive : archives) {
        HeldArchive one;
        if (!open_archive(locate(archive->name).name, archive->bytes, one, problem))
            return problem = shown + ": " + problem, false;
        held.push_back(std::move(one));
    }
    std::vector<std::vector<uint8_t>> written_archives;
    if (!filter_archives(held, imported.warnings, written_archives, problem))
        return problem = shown + ": " + problem, false;

    bool description_cut = false;
    const std::string formatted = format_description(tam.description, description_cut);
    if (description_cut)
        imported.warnings.emplace_back("the description was cut to 2000 bytes");
    if (!formatted.empty() && has_control(formatted, true))
        return problem = shown + ": the description holds a control character", false;

    std::string legacy_name = tam.title;
    if (legacy_name.empty() || has_control(legacy_name, false))
        return problem = shown + ": the title holds a control character", false;
    if (legacy_name.size() > most_legacy_name_bytes) {
        legacy_name = cut_utf8(std::move(legacy_name), most_legacy_name_bytes);
        imported.warnings.emplace_back("the legacy name was cut to 128 bytes");
        if (legacy_name.empty())
            return problem = shown + ": the title holds a control character", false;
    }

    std::string display = trim_decorations(tam.title);
    display = cut_utf8(std::move(display), oamut::most_name_bytes);
    if (display.empty()) {
        display = cut_utf8(trim_decorations(stem), oamut::most_name_bytes);
        if (!display.empty())
            imported.warnings.emplace_back("the title has no name; the file name is used");
    }
    if (display.empty() || has_control(display, false))
        return problem = shown + ": the mutator has no name", false;

    const std::string credit = credit_of(formatted);
    if (credit.size() > most_author_name_bytes)
        return problem = shown + ": the credit cannot be stored", false;

    std::string base_id = options.id;
    if (base_id.empty()) {
        base_id = id_from_title(tam.title);
        if (base_id.empty())
            base_id = id_from_title(stem);
        if (base_id.empty())
            base_id = "mutator";
    }
    bool suffixed = false;
    const std::string id = unique_id(base_id, used, suffixed);
    if (id.empty())
        return problem = shown + ": the id cannot be made unique", false;
    if (suffixed) {
        imported.warnings.push_back("id " + base_id + " is shared; this mutator is " + id);
    }

    const bool wrote_rules = rule_bytes != nullptr;
    const bool wrote_archive = !written_archives.empty();
    if (!wrote_rules && !wrote_archive)
        imported.warnings.emplace_back("the mutator changes nothing");

    bool reached[9]{};
    if (wrote_rules) {
        if (rules.blocks.empty())
            reached[static_cast<std::size_t>(oamut::Layer::units)] = true;
        else {
            for (const soft::Block& block : rules.blocks)
                reach_target(block.target, reached);
        }
    }
    if (wrote_archive || !wrote_rules)
        reached[static_cast<std::size_t>(oamut::Layer::files)] = true;

    oamut::Manifest manifest;
    manifest.format = oamut::format_version;
    manifest.id = id;
    manifest.name = display;
    if (options.version.empty())
        manifest.version = *oamut::parse_version("1.0");
    else
        manifest.version = *oamut::parse_version(options.version);
    manifest.author.name = credit.empty() ? "unknown" : credit;
    if (!formatted.empty())
        manifest.description = formatted;
    if (options.tags.empty()) {
        manifest.tags.emplace_back("gameplay");
        imported.warnings.emplace_back("tags default to gameplay");
    } else {
        manifest.tags = options.tags;
    }

    bool star = false;
    bool mapped = false;
    for (const std::string& code : tam.codes) {
        if (code == "*") {
            star = true;
            mapped = true;
            continue;
        }
        if (same_ascii(code, "OTA")) {
            mapped = true;
            bool seen = false;
            for (const oamut::Condition& earlier : manifest.works_with)
                seen = seen || earlier.id == "ta-3.1c";
            if (!seen) {
                oamut::Condition condition;
                condition.id = "ta-3.1c";
                manifest.works_with.push_back(std::move(condition));
            }
            continue;
        }
        if (same_ascii(code, "TACC")) {
            mapped = true;
            imported.warnings.emplace_back(
                "TACC maps to ta-3.1c; Core Contingency's units are only there when it is on"
            );
            bool seen = false;
            for (const oamut::Condition& earlier : manifest.works_with)
                seen = seen || earlier.id == "ta-3.1c";
            if (!seen) {
                oamut::Condition condition;
                condition.id = "ta-3.1c";
                manifest.works_with.push_back(std::move(condition));
            }
            continue;
        }
        const std::string_view entry = mapped_entry(options, code);
        if (entry.empty()) {
            imported.warnings.push_back(code + " is not mapped to a mod");
            continue;
        }
        mapped = true;
        if (entry == "*") {
            star = true;
            continue;
        }
        bool seen = false;
        for (const oamut::Condition& earlier : manifest.works_with)
            seen = seen || earlier.id == entry;
        if (!seen) {
            oamut::Condition condition;
            condition.id = std::string{entry};
            manifest.works_with.push_back(std::move(condition));
        }
    }
    if (star)
        manifest.works_with = {oamut::Condition{"*", {}, {}}};
    else if (!mapped || manifest.works_with.empty()) {
        manifest.works_with = {oamut::Condition{"*", {}, {}}};
        imported.warnings.emplace_back(
            "no compatibility code maps to a mod; it works with every mod"
        );
    }
    manifest.requires_engine = options.engine;
    for (std::size_t layer = 0; layer < 9; ++layer) {
        if (reached[layer])
            manifest.layers.push_back(static_cast<oamut::Layer>(layer));
    }
    oamut::Legacy legacy;
    legacy.name = std::move(legacy_name);
    legacy.prefix = prefix;
    legacy.compatibility = tam.codes;
    legacy.files = std::move(legacy_names);
    manifest.legacy.push_back(std::move(legacy));

    const std::string yaml = oamut::write_manifest(manifest);
    std::vector<oamut::Problem> manifest_problems;
    oamut::Manifest read_back;
    if (!oamut::read_manifest(bytes_of(yaml), read_back, manifest_problems)) {
        problem = shown + ": " +
                  (manifest_problems.empty() ? "the manifest was refused"
                                             : oamut::describe(manifest_problems.front()));
        return false;
    }

    const bool files_layer = reached[static_cast<std::size_t>(oamut::Layer::files)];
    const std::string script = mutator_script(tam.title, id, files_layer, wrote_rules);
    oa::data::script_host::ScriptError script_error;
    if (!oa::data::script_host::compile_check(bytes_of(script), "mutator.lua", script_error)) {
        problem = shown + ": mutator.lua: " + script_error.message;
        return false;
    }

    imported.manifest = std::move(manifest);
    imported.files.emplace_back("oamut.yaml", bytes_of(yaml));
    imported.files.emplace_back("mutator.lua", bytes_of(script));
    if (wrote_rules)
        imported.files.emplace_back("rules/" + id + ".sm", *rule_bytes);
    if (written_archives.size() == 1) {
        imported.files.emplace_back("files/" + id + ".ccx", std::move(written_archives[0]));
    } else {
        for (std::size_t archive_index = 0; archive_index < written_archives.size();
             ++archive_index) {
            imported.files.emplace_back(
                "files/" + id + "-" + std::to_string(archive_index + 1) + ".ccx",
                std::move(written_archives[archive_index])
            );
        }
    }
    return true;
}

/// Bytes of a zip held in memory, for the streamed reader.
struct MemorySource {
    const uint8_t* data{}; ///< the archive; null when it is empty
    std::size_t size{};    ///< its size
};

/// Reads a slice of a memory zip.
///
/// @param context the MemorySource
/// @param offset where the slice starts
/// @param buffer where the bytes go
/// @return false when the slice runs past the archive
bool read_memory(void* context, uint64_t offset, std::span<uint8_t> buffer) {
    const auto* memory = static_cast<const MemorySource*>(context);
    if (memory == nullptr || offset > memory->size ||
        buffer.size() > memory->size - static_cast<std::size_t>(offset))
        return false;
    if (!buffer.empty())
        std::memcpy(buffer.data(), memory->data + static_cast<std::size_t>(offset), buffer.size());
    return true;
}

/// A zip failure, naming the entry when the reader has one.
///
/// @param error the reader's error
/// @return the line
std::string zip_problem(const zip::ZipError& error) {
    std::string text = zip::zip_status_message(error.status);
    if (!error.entry.empty()) {
        text += ": ";
        text += error.entry;
    }
    return text;
}

} // namespace

bool read_tam(std::span<const uint8_t> bytes, Tam& tam, std::string& problem) {
    if (bytes.size() > most_tam_bytes) {
        problem = "the .tam is larger than 64 KiB";
        return false;
    }
    for (const uint8_t byte : bytes) {
        if (byte == 0) {
            problem = "the .tam holds a NUL";
            return false;
        }
    }
    const std::string text = decode_code_page(bytes);
    std::vector<std::string> lines;
    std::string current;
    for (const char character : text) {
        if (character != '\n') {
            current.push_back(character);
            continue;
        }
        if (!current.empty() && current.back() == '\r')
            current.pop_back();
        lines.push_back(std::move(current));
        current.clear();
    }
    if (text.empty() || text.back() != '\n') {
        if (!current.empty() && current.back() == '\r')
            current.pop_back();
        lines.push_back(std::move(current));
    }
    if (lines.empty() || lines[0].empty()) {
        problem = "the .tam has no title";
        return false;
    }
    if (lines.size() < 2) {
        problem = "the .tam has no compatibility line";
        return false;
    }
    Tam read;
    read.title = lines[0];
    read.description = lines[1];
    std::size_t cursor = 2;
    while (cursor < lines.size() && trim_space(lines[cursor]).empty()) {
        read.warnings.emplace_back("skipped an empty line before the compatibility line");
        ++cursor;
    }
    if (cursor >= lines.size() || trim_space(lines[cursor]).empty()) {
        problem = "the .tam has no compatibility line";
        return false;
    }
    const std::string codes = std::string{trim_space(lines[cursor])};
    std::string token;
    for (std::size_t index = 0; index <= codes.size(); ++index) {
        const bool end = index == codes.size() || codes[index] == ' ' || codes[index] == '\t';
        if (!end) {
            token.push_back(codes[index]);
            continue;
        }
        if (!token.empty()) {
            read.codes.push_back(std::move(token));
            token.clear();
        }
    }
    ++cursor;
    if (cursor >= lines.size()) {
        problem = "the companion count is not 0 to 9";
        return false;
    }
    const std::string count = std::string{trim_space(lines[cursor])};
    if (count.size() != 1 || count[0] < '0' || count[0] > '9') {
        problem = "the companion count is not 0 to 9";
        return false;
    }
    read.companion_count = count[0] - '0';
    ++cursor;
    while (cursor < lines.size() && trim_space(lines[cursor]).empty())
        ++cursor;
    if (cursor < lines.size()) {
        read.archive_name = std::string{trim_space(lines[cursor])};
        ++cursor;
        while (cursor < lines.size()) {
            if (!trim_space(lines[cursor]).empty())
                read.warnings.emplace_back("ignored an extra line");
            ++cursor;
        }
    }
    bool cut = false;
    read.credit = credit_of(format_description(read.description, cut));
    tam = std::move(read);
    return true;
}

std::string id_from_title(std::string_view title) {
    std::string id;
    bool word = false;
    bool gap = false;
    std::size_t index = 0;
    while (index < title.size()) {
        char32_t character = 0;
        const bool formed = next_utf8(title, index, character);
        std::string piece;
        if (formed && append_word(piece, character)) {
            if (gap && word)
                id.push_back('-');
            id += piece;
            word = true;
            gap = false;
        } else if (word) {
            gap = true;
        }
    }
    if (id.size() > oamut::most_id_bytes) {
        std::size_t hyphen = std::string::npos;
        const std::size_t limit = oamut::most_id_bytes;
        for (std::size_t at = 0; at < limit && at < id.size(); ++at) {
            if (id[at] == '-')
                hyphen = at;
        }
        if (hyphen != std::string::npos && hyphen > 0)
            id.resize(hyphen);
        else
            id.resize(oamut::most_id_bytes);
    }
    while (!id.empty() && id.back() == '-')
        id.pop_back();
    return id;
}

std::string description_text(std::string_view description) {
    bool cut = false;
    return format_description(description, cut);
}

std::string credit_of(std::string_view description) {
    const auto break_at = description.rfind('\n');
    std::string_view line =
        break_at == std::string_view::npos ? description : description.substr(break_at + 1);
    line = trim_space(line);
    constexpr std::string_view phrases[] = {"created by", "made by", "by"};
    std::size_t name_at = 0;
    bool matched = false;
    for (const std::string_view phrase : phrases) {
        if (phrase_at(line, phrase, name_at)) {
            matched = true;
            break;
        }
    }
    if (!matched)
        return {};
    const std::string name{trim_space(line.substr(name_at))};
    if (name.empty() || name.size() > most_author_name_bytes || has_control(name, false))
        return {};
    return name;
}

ImportResult import_legacy(std::span<const LegacyFile> files, const ImportOptions& options) {
    ImportResult result;
    if (!options_ok(options, result.problems))
        return result;
    std::vector<char> claimed(files.size(), 0);
    std::set<std::string> used;
    for (std::size_t index = 0; index < files.size(); ++index) {
        if (!is_tam_name(files[index].name))
            continue;
        claimed[index] = 1;
        Imported imported;
        std::string problem;
        if (!import_one(files, index, options, used, claimed, imported, problem)) {
            result.problems.push_back(std::move(problem));
            continue;
        }
        result.mutators.push_back(std::move(imported));
    }
    for (std::size_t index = 0; index < files.size(); ++index) {
        if (claimed[index] == 0) {
            result.warnings.push_back(
                "left out " + files[index].name + "; it belongs to no mutator"
            );
        }
    }
    return result;
}

bool files_from_zip(
    std::span<const uint8_t> zip_bytes, std::vector<LegacyFile>& files, std::string& problem
) {
    MemorySource memory;
    memory.data = zip_bytes.data();
    memory.size = zip_bytes.size();
    zip::SourceHooks source;
    source.context = &memory;
    source.read_at = read_memory;
    // The directory reader refuses an entry above max_entry_bytes before this
    // function can leave that entry unread. The default cap lists a large
    // file that is not a .tam, a .sm or a .ccx.
    const zip::StreamLimits limits;
    zip::StreamDirectory directory;
    zip::ZipError error;
    if (!zip::read_stream_directory(source, zip_bytes.size(), limits, directory, error)) {
        problem = zip_problem(error);
        return false;
    }
    std::vector<char> load(directory.entries.size(), 0);
    uint64_t total = 0;
    const uint64_t cap = most_zip_read_bytes;
    for (std::size_t plan = 0; plan < directory.entries.size(); ++plan) {
        const zip::StreamEntry& entry = directory.entries[plan];
        if (entry.directory || (!entry.name.empty() && entry.name.back() == '/'))
            continue;
        if (entry.symbolic_link || entry.special_file) {
            problem = "refusing a link or special file: " + entry.name;
            return false;
        }
        if (!payload_name(entry.name))
            continue;
        if (is_tam_name(entry.name) && entry.bytes > most_tam_bytes) {
            problem = entry.name + ": the .tam is larger than 64 KiB";
            return false;
        }
        if (entry.bytes > most_archive_bytes) {
            problem = entry.name + ": the file is larger than 64 MiB";
            return false;
        }
        if (entry.bytes > cap || total > cap - entry.bytes) {
            problem = "the zip's files are larger than 512 MiB";
            return false;
        }
        total += entry.bytes;
        load[plan] = 1;
    }
    std::vector<LegacyFile> read;
    for (std::size_t held = 0; held < directory.entries.size(); ++held) {
        const zip::StreamEntry& entry = directory.entries[held];
        if (entry.directory || (!entry.name.empty() && entry.name.back() == '/'))
            continue;
        LegacyFile file;
        file.name = entry.name;
        if (load[held] == 0) {
            read.push_back(std::move(file));
            continue;
        }
        if (!zip::read_stream_entry(
                source, zip_bytes.size(), entry, most_archive_bytes, file.bytes, error
            )) {
            problem = zip_problem(error);
            if (problem.find(entry.name) == std::string::npos && !entry.name.empty()) {
                problem += ": ";
                problem += entry.name;
            }
            return false;
        }
        read.push_back(std::move(file));
    }
    files = std::move(read);
    return true;
}

} // namespace oa::data::mutator_import
