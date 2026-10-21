// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/data/mutators/verdicts.hpp"

#include <cstddef>
#include <map>
#include <utility>

namespace oa::data::mutators {
namespace {

constexpr std::size_t kFieldCount = 8;
constexpr std::size_t kCountCount = 12;
constexpr std::size_t kEngineVersionMostBytes = 64;
constexpr char kForm[] = "1";

/// Returns whether every byte is UTF-8.
///
/// @param text the bytes
/// @return true when the text is well-formed UTF-8
bool utf8_ok(std::string_view text) noexcept {
    std::size_t index = 0;
    while (index < text.size()) {
        const auto lead = static_cast<unsigned char>(text[index]);
        std::size_t need = 1;
        uint32_t code = lead;
        if (lead <= 0x7f) {
            need = 1;
        } else if (lead >= 0xc2 && lead <= 0xdf) {
            need = 2;
            code = lead & 0x1fu;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            need = 3;
            code = lead & 0x0fu;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            need = 4;
            code = lead & 0x07u;
        } else {
            return false;
        }
        if (index + need > text.size())
            return false;
        for (std::size_t extra = 1; extra < need; ++extra) {
            const auto cont = static_cast<unsigned char>(text[index + extra]);
            if (cont < 0x80 || cont > 0xbf)
                return false;
            code = (code << 6) | (cont & 0x3fu);
        }
        if (need == 3 && code < 0x800)
            return false;
        if (need == 4 && (code < 0x10000 || code > 0x10ffff))
            return false;
        if (code >= 0xd800 && code <= 0xdfff)
            return false;
        index += need;
    }
    return true;
}

/// Appends one well-formed character, or skips one bad byte.
///
/// @param[in,out] out the text being built
/// @param text the source
/// @param[in,out] index the next byte; advanced past what was consumed
void append_character(std::string& out, std::string_view text, std::size_t& index) {
    const auto lead = static_cast<unsigned char>(text[index]);
    std::size_t need = 1;
    if (lead <= 0x7f) {
        need = 1;
    } else if (lead >= 0xc2 && lead <= 0xdf) {
        need = 2;
    } else if (lead >= 0xe0 && lead <= 0xef) {
        need = 3;
    } else if (lead >= 0xf0 && lead <= 0xf4) {
        need = 4;
    } else {
        ++index;
        return;
    }
    if (index + need > text.size() || !utf8_ok(text.substr(index, need))) {
        ++index;
        return;
    }
    const char byte = text[index];
    if (byte == '\n' || byte == '\r' || byte == '\t')
        out.push_back(' ');
    else
        out.append(text.substr(index, need));
    index += need;
}

/// Returns whether the engine version is printable ASCII without a space.
///
/// @param text the field
/// @return true when it has 1 to 64 bytes, each from 0x21 to 0x7e
bool engine_version_ok(std::string_view text) noexcept {
    if (text.empty() || text.size() > kEngineVersionMostBytes)
        return false;
    for (const char byte : text) {
        const auto value = static_cast<unsigned char>(byte);
        if (value < 0x21 || value > 0x7e)
            return false;
    }
    return true;
}

/// Returns whether the text is 64 lower-case hex digits.
///
/// @param text the field
/// @return true when every character is 0-9 or a-f and the length is 64
bool lower_hex_digest(std::string_view text) noexcept {
    if (text.size() != base::sha256::hex_size)
        return false;
    for (const char byte : text) {
        const bool digit = byte >= '0' && byte <= '9';
        const bool letter = byte >= 'a' && byte <= 'f';
        if (!digit && !letter)
            return false;
    }
    return true;
}

/// Reads one unsigned decimal with no leading zero.
///
/// @param text the field
/// @return the value, or nullopt when it is not a uint32 decimal
std::optional<uint32_t> parse_count(std::string_view text) noexcept {
    if (text.empty() || text.size() > 10)
        return std::nullopt;
    if (text.size() > 1 && text.front() == '0')
        return std::nullopt;
    uint64_t value = 0;
    for (const char byte : text) {
        if (byte < '0' || byte > '9')
            return std::nullopt;
        value = value * 10u + static_cast<uint64_t>(byte - '0');
        if (value > 0xffffffffu)
            return std::nullopt;
    }
    return static_cast<uint32_t>(value);
}

/// Splits a count field into the twelve counts.
///
/// @param text the field
/// @param[out] counts the counts, written only when all twelve parse
/// @return true when the field is twelve decimals
bool parse_counts(std::string_view text, ChangeCounts& counts) noexcept {
    uint32_t values[kCountCount] = {};
    std::size_t found = 0;
    std::size_t start = 0;
    for (std::size_t index = 0; index <= text.size(); ++index) {
        if (index != text.size() && text[index] != ',')
            continue;
        if (found == kCountCount)
            return false;
        const auto part = parse_count(text.substr(start, index - start));
        if (!part)
            return false;
        values[found] = *part;
        ++found;
        start = index + 1;
    }
    if (found != kCountCount)
        return false;
    counts.units_changed = values[0];
    counts.units_added = values[1];
    counts.units_removed = values[2];
    counts.weapons_changed = values[3];
    counts.weapons_added = values[4];
    counts.weapons_removed = values[5];
    counts.features_changed = values[6];
    counts.features_added = values[7];
    counts.features_removed = values[8];
    counts.menu_entries_changed = values[9];
    counts.files_added = values[10];
    counts.files_hidden = values[11];
    return true;
}

/// Returns the next field, or nullopt when a tab is missing.
///
/// @param line the rest of the line
/// @param last true for the last field, which keeps the remainder
/// @param[out] field the field
/// @return the unparsed tail, or nullopt when a required tab is absent
std::optional<std::string_view>
take_field(std::string_view line, bool last, std::string_view& field) noexcept {
    if (last) {
        field = line;
        return std::string_view{};
    }
    const auto tab = line.find('\t');
    if (tab == std::string_view::npos)
        return std::nullopt;
    field = line.substr(0, tab);
    return line.substr(tab + 1);
}

/// Reads one line into a remembered verdict.
///
/// @param line the line without its line feed
/// @return the verdict, or nullopt when the line breaks a rule
std::optional<RememberedVerdict> parse_line(std::string_view line) {
    if (line.size() > verdict_line_most_bytes)
        return std::nullopt;
    if (line.find('\r') != std::string_view::npos)
        return std::nullopt;
    if (!utf8_ok(line))
        return std::nullopt;

    std::string_view fields[kFieldCount];
    std::string_view rest = line;
    for (std::size_t index = 0; index < kFieldCount; ++index) {
        const bool last = index + 1 == kFieldCount;
        const auto next = take_field(rest, last, fields[index]);
        if (!next)
            return std::nullopt;
        rest = *next;
    }
    if (fields[0] != kForm)
        return std::nullopt;
    if (!lower_hex_digest(fields[1]) || !lower_hex_digest(fields[2]))
        return std::nullopt;
    if (!engine_version_ok(fields[3]))
        return std::nullopt;
    const auto verdict = parse_verdict_word(fields[4]);
    if (!verdict)
        return std::nullopt;
    ChangeCounts counts;
    if (!parse_counts(fields[5], counts))
        return std::nullopt;
    if (fields[7].find('\t') != std::string_view::npos)
        return std::nullopt;
    if ((*verdict == Verdict::compatible || *verdict == Verdict::no_effect) && !fields[7].empty())
        return std::nullopt;

    RememberedVerdict remembered;
    remembered.key.loadout = *base::sha256::parse_hex(fields[1]);
    remembered.key.mod = *base::sha256::parse_hex(fields[2]);
    remembered.key.engine_version = std::string{fields[3]};
    remembered.record.verdict = *verdict;
    remembered.record.counts = counts;
    remembered.record.mod_label = std::string{fields[6]};
    remembered.record.reason = std::string{fields[7]};
    return remembered;
}

/// Writes one decimal with no leading zero.
///
/// @param[in,out] out the line
/// @param value the count
void append_count(std::string& out, uint32_t value) {
    out += std::to_string(value);
}

/// Writes one verdict as a line, including its line feed.
///
/// @param verdict the verdict
/// @return the line, or empty when the engine version cannot be stored
std::string format_line(const RememberedVerdict& verdict) {
    if (!engine_version_ok(verdict.key.engine_version))
        return {};
    const std::string label = one_line_reason(verdict.record.mod_label);
    std::string reason;
    if (verdict.record.verdict != Verdict::compatible &&
        verdict.record.verdict != Verdict::no_effect)
        reason = one_line_reason(verdict.record.reason);

    std::string line;
    line.reserve(256);
    line += kForm;
    line.push_back('\t');
    const auto loadout = base::sha256::to_hex(verdict.key.loadout);
    line.append(loadout.data(), loadout.size());
    line.push_back('\t');
    const auto mod = base::sha256::to_hex(verdict.key.mod);
    line.append(mod.data(), mod.size());
    line.push_back('\t');
    line += verdict.key.engine_version;
    line.push_back('\t');
    line += verdict_word(verdict.record.verdict);
    line.push_back('\t');
    const ChangeCounts& counts = verdict.record.counts;
    append_count(line, counts.units_changed);
    const uint32_t rest[] = {
        counts.units_added,
        counts.units_removed,
        counts.weapons_changed,
        counts.weapons_added,
        counts.weapons_removed,
        counts.features_changed,
        counts.features_added,
        counts.features_removed,
        counts.menu_entries_changed,
        counts.files_added,
        counts.files_hidden,
    };
    for (const uint32_t value : rest) {
        line.push_back(',');
        append_count(line, value);
    }
    line.push_back('\t');
    line += label;
    line.push_back('\t');
    line += reason;
    line.push_back('\n');
    return line;
}

} // namespace

std::string_view verdict_word(Verdict verdict) noexcept {
    switch (verdict) {
    case Verdict::compatible:
        return "compatible";
    case Verdict::no_effect:
        return "no-effect";
    case Verdict::incompatible:
        return "incompatible";
    case Verdict::failed:
        return "failed";
    }
    return "failed";
}

std::optional<Verdict> parse_verdict_word(std::string_view word) noexcept {
    if (word == "compatible")
        return Verdict::compatible;
    if (word == "no-effect")
        return Verdict::no_effect;
    if (word == "incompatible")
        return Verdict::incompatible;
    if (word == "failed")
        return Verdict::failed;
    return std::nullopt;
}

bool any(const ChangeCounts& counts) noexcept {
    return counts.units_changed != 0 || counts.units_added != 0 || counts.units_removed != 0 ||
           counts.weapons_changed != 0 || counts.weapons_added != 0 ||
           counts.weapons_removed != 0 || counts.features_changed != 0 ||
           counts.features_added != 0 || counts.features_removed != 0 ||
           counts.menu_entries_changed != 0 || counts.files_added != 0 || counts.files_hidden != 0;
}

std::string one_line_reason(std::string_view text) {
    std::string built;
    built.reserve(
        text.size() < verdict_reason_most_bytes ? text.size() : verdict_reason_most_bytes
    );
    for (std::size_t index = 0; index < text.size();)
        append_character(built, text, index);

    std::string collapsed;
    collapsed.reserve(built.size());
    bool pending_space = false;
    bool any_word = false;
    for (const char byte : built) {
        if (byte == ' ') {
            if (any_word)
                pending_space = true;
            continue;
        }
        if (pending_space) {
            collapsed.push_back(' ');
            pending_space = false;
        }
        collapsed.push_back(byte);
        any_word = true;
    }

    if (collapsed.size() <= verdict_reason_most_bytes)
        return collapsed;
    std::size_t end = 0;
    while (end < collapsed.size()) {
        const auto lead = static_cast<unsigned char>(collapsed[end]);
        std::size_t need = 1;
        if (lead >= 0xc2 && lead <= 0xdf)
            need = 2;
        else if (lead >= 0xe0 && lead <= 0xef)
            need = 3;
        else if (lead >= 0xf0 && lead <= 0xf4)
            need = 4;
        if (end + need > verdict_reason_most_bytes)
            break;
        end += need;
    }
    collapsed.resize(end);
    while (!collapsed.empty() && collapsed.back() == ' ')
        collapsed.pop_back();
    return collapsed;
}

std::vector<RememberedVerdict> read_verdicts(std::span<const uint8_t> bytes) {
    std::vector<RememberedVerdict> verdicts;
    if (bytes.size() > verdicts_file_most_bytes)
        return verdicts;
    std::map<VerdictKey, std::size_t> place;
    std::size_t start = 0;
    while (start < bytes.size()) {
        std::size_t end = start;
        while (end < bytes.size() && bytes[end] != static_cast<uint8_t>('\n'))
            ++end;
        const auto length = end - start;
        if (length <= verdict_line_most_bytes && length > 0) {
            const auto* data = reinterpret_cast<const char*>(bytes.data() + start);
            if (auto parsed = parse_line(std::string_view{data, length})) {
                const auto found = place.find(parsed->key);
                if (found != place.end()) {
                    verdicts[found->second].record = std::move(parsed->record);
                } else {
                    place.emplace(parsed->key, verdicts.size());
                    verdicts.push_back(std::move(*parsed));
                }
            }
        }
        start = end < bytes.size() ? end + 1 : end;
    }
    if (verdicts.size() > verdicts_most_lines) {
        const auto drop = verdicts.size() - verdicts_most_lines;
        verdicts.erase(verdicts.begin(), verdicts.begin() + static_cast<std::ptrdiff_t>(drop));
    }
    return verdicts;
}

std::vector<uint8_t> write_verdicts(std::span<const RememberedVerdict> verdicts) {
    std::vector<uint8_t> bytes;
    const std::size_t begin =
        verdicts.size() > verdicts_most_lines ? verdicts.size() - verdicts_most_lines : 0;
    for (std::size_t index = begin; index < verdicts.size(); ++index) {
        const std::string line = format_line(verdicts[index]);
        bytes.insert(bytes.end(), line.begin(), line.end());
    }
    return bytes;
}

void remember_verdict(std::vector<RememberedVerdict>& verdicts, RememberedVerdict verdict) {
    for (std::size_t index = 0; index < verdicts.size(); ++index) {
        if (verdicts[index].key == verdict.key) {
            verdicts.erase(verdicts.begin() + static_cast<std::ptrdiff_t>(index));
            break;
        }
    }
    verdicts.push_back(std::move(verdict));
    if (verdicts.size() > verdicts_most_lines) {
        const auto extra = verdicts.size() - verdicts_most_lines;
        verdicts.erase(verdicts.begin(), verdicts.begin() + static_cast<std::ptrdiff_t>(extra));
    }
}

const VerdictRecord*
find_verdict(std::span<const RememberedVerdict> verdicts, const VerdictKey& key) noexcept {
    for (const RememberedVerdict& verdict : verdicts) {
        if (verdict.key == key)
            return &verdict.record;
    }
    return nullptr;
}

} // namespace oa::data::mutators
