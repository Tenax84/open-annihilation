// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "mutator_report.hpp"

#include "oa/base/sha256.hpp"
#include "oa/core/unit_def.h"
#include "oa/data/defs/asset_files.hpp"
#include "oa/data/defs/unit_header.hpp"
#include "oa/formats/tdf.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <ostream>
#include <utility>

namespace oa::tool::mutator_report {
namespace {

namespace tdf = oa::formats::tdf;
namespace defs = oa::data::defs;

constexpr int8_t build_major = 3;
constexpr int8_t build_minor = 1;
constexpr std::string_view absent = "(absent)";

/// Writes a byte below 0x20, and 0x7F, as `\xHH`. Every other byte stays.
std::string escape_text(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    constexpr char hex[] = "0123456789ABCDEF";
    for (const unsigned char piece : text) {
        if (piece < 0x20 || piece == 0x7F) {
            out.append("\\x");
            out.push_back(hex[piece >> 4]);
            out.push_back(hex[piece & 0x0F]);
        } else {
            out.push_back(static_cast<char>(piece));
        }
    }
    return out;
}

/// Folds ASCII letters and turns backslashes into slashes.
std::string normalize_path(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const unsigned char piece : text) {
        char lowered = static_cast<char>(piece);
        if (lowered == '\\')
            lowered = '/';
        else if (lowered >= 'A' && lowered <= 'Z')
            lowered = static_cast<char>(lowered - 'A' + 'a');
        out.push_back(lowered);
    }
    return out;
}

/// Folds ASCII letters to lower case.
std::string lower_ascii(std::string_view text) {
    std::string out(text);
    for (char& piece : out) {
        if (piece >= 'A' && piece <= 'Z')
            piece = static_cast<char>(piece - 'A' + 'a');
    }
    return out;
}

/// Folds ASCII letters to upper case.
std::string upper_ascii(std::string_view text) {
    std::string out(text);
    for (char& piece : out) {
        if (piece >= 'a' && piece <= 'z')
            piece = static_cast<char>(piece - 'a' + 'A');
    }
    return out;
}

/// Reports whether two texts match, ASCII case ignored.
bool same_ascii(std::string_view left, std::string_view right) {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const char a = left[index] >= 'A' && left[index] <= 'Z'
                           ? static_cast<char>(left[index] - 'A' + 'a')
                           : left[index];
        const char b = right[index] >= 'A' && right[index] <= 'Z'
                           ? static_cast<char>(right[index] - 'A' + 'a')
                           : right[index];
        if (a != b)
            return false;
    }
    return true;
}

/// The part of a line before a tab, or the whole line.
std::string before_tab(std::string_view line) {
    const auto tab = line.find('\t');
    return std::string(line.substr(0, tab));
}

/// True for the text files the report parses.
bool text_extension(std::string_view path) {
    return path.ends_with(".fbi") || path.ends_with(".tdf") || path.ends_with(".gui") ||
           path.ends_with(".ota");
}

/// The file name of a path, as UTF-8.
std::string file_name_of(const std::filesystem::path& path) {
    const auto text = path.filename().u8string();
    return {text.begin(), text.end()};
}

/// A label that does not name a host path.
std::string source_label(const oa::Provider& provider) {
    const std::string& identity = provider.identity;
    constexpr std::string_view mutator_prefix = "mutator:";
    if (identity.starts_with(mutator_prefix)) {
        std::string owner = identity.substr(mutator_prefix.size());
        const auto serial = owner.find('#');
        if (serial != std::string::npos)
            owner.resize(serial);
        return "mutator " + owner;
    }
    if (provider.kind == oa::ProviderKind::loose || identity.starts_with("loose:"))
        return "loose";
    if (provider.kind == oa::ProviderKind::archive)
        return file_name_of(provider.path);
    if (provider.kind == oa::ProviderKind::pack_layer)
        return "pack";
    return "none";
}

/// SHA-256 of a byte string, lower case.
std::string sha256_of(const std::vector<uint8_t>& bytes) {
    const auto digest = oa::base::sha256::digest_of(bytes);
    const auto hex = oa::base::sha256::to_hex(digest);
    return {hex.begin(), hex.end()};
}

/// Splits `[unitinfo] workertime` into the section path and the key.
bool split_field(std::string_view key, std::string& section, std::string& name) {
    const auto mark = key.rfind("] ");
    if (mark == std::string_view::npos)
        return false;
    section.assign(key.substr(0, mark + 1));
    name.assign(key.substr(mark + 2));
    return !name.empty();
}

/// The dump's name for a child section: `[name]` for the first child of
/// that name, ASCII case ignored, and `[name@N]` for the N-th, from 2.
/// 3.1c's build pages repeat [GADGET2] to [GADGET6], and the game reads
/// every gadget in file order.
std::string child_label(const char* name, std::map<std::string, int>& seen) {
    const std::string lowered = lower_ascii(name);
    const int count = ++seen[lowered];
    if (count == 1)
        return "[" + lowered + "]";
    return "[" + lowered + "@" + std::to_string(count) + "]";
}

/// Copies one block's fields, then its children.
void collect_fields(const tdf::Block* block, const std::string& prefix, DumpedFile& file) {
    if (block == nullptr)
        return;
    const uint32_t properties = tdf::property_count(block);
    for (uint32_t index = 0; index < properties; ++index) {
        const char* const key = tdf::property_key_at(block, static_cast<int32_t>(index));
        if (key == nullptr)
            continue;
        const char* const value = tdf::find_value(block, key);
        if (value == nullptr)
            continue;
        file.fields.insert_or_assign(prefix + " " + lower_ascii(key), std::string(value));
    }
    const uint32_t children = tdf::child_count(block);
    std::map<std::string, int> seen;
    for (uint32_t index = 0; index < children; ++index) {
        const tdf::Block* const child = tdf::child_at(block, index);
        if (child == nullptr || child->name == nullptr)
            continue;
        collect_fields(child, prefix + child_label(child->name, seen), file);
    }
}

/// Parses text into fields. The document is freed before return.
void parse_fields(std::string_view text, DumpedFile& file) {
    tdf::Document document;
    tdf::document_init(&document);
    const bool parsed =
        text.size() <= tdf::max_input_bytes &&
        tdf::parse_text(&document, text.data(), static_cast<uint32_t>(text.size()), false, nullptr);
    if (parsed && document.root != nullptr) {
        file.parsed = true;
        const uint32_t children = tdf::child_count(document.root);
        std::map<std::string, int> seen;
        for (uint32_t index = 0; index < children; ++index) {
            const tdf::Block* const child = tdf::child_at(document.root, index);
            if (child == nullptr || child->name == nullptr)
                continue;
            collect_fields(child, child_label(child->name, seen), file);
        }
    }
    tdf::document_free(&document);
}

/// The unit name an `units/<name>.fbi` path names, or empty.
std::string unit_name_of(std::string_view path) {
    constexpr std::string_view folder = "units/";
    constexpr std::string_view extension = ".fbi";
    if (!path.starts_with(folder) || !path.ends_with(extension))
        return {};
    if (path.find('/', folder.size()) != std::string_view::npos)
        return {};
    return upper_ascii(path.substr(folder.size(), path.size() - folder.size() - extension.size()));
}

/// Looks up one parsed field.
const std::string* field_of(const DumpedFile& file, std::string_view key) {
    const auto found = file.fields.find(std::string(key));
    if (found == file.fields.end())
        return nullptr;
    return &found->second;
}

/// Classifies one FBI from `load_unit_header`. Hidden files are not read.
void classify_unit(const oa::data::defs::Files& files, Dump& dump, const std::string& path) {
    const std::string name = unit_name_of(path);
    if (name.empty())
        return;
    if (dump.hidden.contains(path)) {
        dump.dropped_units.insert_or_assign(name, "hidden");
        return;
    }
    if (!dump.files.contains(path))
        return;
    UnitDef unit{};
    defs::WeaponTdfSet weapons{};
    defs::weapon_tdf_set_init(&weapons);
    defs::UnitHeaderSources sources{};
    sources.language = "";
    sources.weapons = &weapons;
    sources.build_major = build_major;
    sources.build_minor = build_minor;
    bool refused = false;
    const bool loaded = defs::load_unit_header(&files, path.c_str(), unit, sources, &refused);
    defs::weapon_tdf_set_free(&weapons);
    if (!loaded) {
        dump.dropped_units.insert_or_assign(name, "version");
        return;
    }
    if (refused) {
        dump.dropped_units.insert_or_assign(name, "copyright");
        return;
    }
    if ((unit.flags & OA_UNIT_DEF_FLAG_AVAILABLE) == 0)
        dump.dropped_units.insert_or_assign(name, "version");
}

/// True when a unit name is one the dump keeps in the catalog.
bool unit_available(const Dump& dump, std::string_view name) {
    if (dump.dropped_units.contains(std::string(name)))
        return false;
    const std::string path = "units/" + lower_ascii(name) + ".fbi";
    return dump.files.contains(path);
}

/// The catalog name a download value names, or empty.
std::string matched_unit(const Dump& dump, std::string_view written) {
    for (const auto& [path, file] : dump.files) {
        (void)file;
        const std::string name = unit_name_of(path);
        if (!name.empty() && same_ascii(name, written) && unit_available(dump, name))
            return name;
    }
    return {};
}

/// Reads download menus from the files the dump already parsed.
void collect_menus(Dump& dump) {
    for (const auto& [path, file] : dump.files) {
        if (!path.starts_with("download/") || !path.ends_with(".tdf") || !file.parsed)
            continue;
        std::set<std::string> sections;
        for (const auto& [key, value] : file.fields) {
            (void)value;
            std::string section;
            std::string name;
            if (!split_field(key, section, name))
                continue;
            if (section.find("][") != std::string::npos)
                continue;
            sections.insert(section);
        }
        for (const std::string& section : sections) {
            const std::string* const builder = field_of(file, section + " unitmenu");
            const std::string* const unit = field_of(file, section + " unitname");
            if (builder == nullptr || unit == nullptr || builder->empty() || unit->empty())
                continue;
            const std::string builder_name = matched_unit(dump, *builder);
            const std::string unit_name = matched_unit(dump, *unit);
            if (builder_name.empty() || unit_name.empty())
                continue;
            MenuEntry entry;
            entry.builder = builder_name;
            const std::string* const page = field_of(file, section + " menu");
            const std::string* const button = field_of(file, section + " button");
            entry.page = page != nullptr ? *page : "0";
            entry.button = button != nullptr ? *button : "0";
            entry.unit = unit_name;
            dump.menus.insert(std::move(entry));
        }
    }
}

/// Names the units whose FBI is in the catalog.
std::set<std::string> available_units(const Dump& dump) {
    std::set<std::string> names;
    for (const auto& [path, file] : dump.files) {
        (void)file;
        const std::string name = unit_name_of(path);
        if (!name.empty() && unit_available(dump, name))
            names.insert(name);
    }
    return names;
}

/// Appends a journal suffix when the attribution names the write.
///
/// A field with no entry of its own uses the file's owner.
void finish_line(
    std::string& line, const Attribution& attribution, std::string_view path, std::string_view field
) {
    const std::vector<std::string>* owners = attribution.ids(path, field);
    if ((owners == nullptr || owners->empty()) && !field.empty())
        owners = attribution.ids(path, {});
    if (owners == nullptr || owners->empty())
        return;
    line.push_back('\t');
    line.append("by ");
    for (std::size_t index = 0; index < owners->size(); ++index) {
        if (index != 0)
            line.push_back(',');
        line.append((*owners)[index]);
    }
}

/// One field line. The missing side is `(absent)`.
std::string field_line(
    const std::string& path,
    std::string_view key,
    const std::string* before,
    const std::string* after,
    const Attribution& attribution
) {
    std::string section;
    std::string name;
    if (!split_field(key, section, name))
        return {};
    std::string line =
        "field " + escape_text(path) + " " + escape_text(section) + " " + escape_text(name) + ": ";
    line.append(escape_text(before != nullptr ? *before : std::string(absent)));
    line.append(" -> ");
    line.append(escape_text(after != nullptr ? *after : std::string(absent)));
    finish_line(line, attribution, path, key);
    return line;
}

/// File and field lines for one path.
void append_path_lines(
    std::vector<std::string>& lines,
    const std::string& path,
    const Dump& before,
    const Dump& after,
    const Attribution& attribution
) {
    const auto before_file = before.files.find(path);
    const auto after_file = after.files.find(path);
    const bool had = before_file != before.files.end();
    const bool has = after_file != after.files.end();
    if (had && !has) {
        std::string line = "file hidden " + escape_text(path);
        finish_line(line, attribution, path, {});
        lines.push_back(std::move(line));
        return;
    }
    if (!had && has) {
        if (after_file->second.parsed) {
            std::string line = "file added " + escape_text(path);
            finish_line(line, attribution, path, {});
            lines.push_back(std::move(line));
            for (const auto& [key, value] : after_file->second.fields) {
                std::string field = field_line(path, key, nullptr, &value, attribution);
                if (!field.empty())
                    lines.push_back(std::move(field));
            }
            return;
        }
        std::string line = "file added " + escape_text(path) + " " + after_file->second.sha256;
        finish_line(line, attribution, path, {});
        lines.push_back(std::move(line));
        return;
    }
    if (!had || !has)
        return;
    const bool both_parsed = before_file->second.parsed && after_file->second.parsed;
    if (both_parsed) {
        std::set<std::string> keys;
        for (const auto& [key, value] : before_file->second.fields) {
            (void)value;
            keys.insert(key);
        }
        for (const auto& [key, value] : after_file->second.fields) {
            (void)value;
            keys.insert(key);
        }
        for (const std::string& key : keys) {
            const auto old_field = before_file->second.fields.find(key);
            const auto new_field = after_file->second.fields.find(key);
            const bool old_present = old_field != before_file->second.fields.end();
            const bool new_present = new_field != after_file->second.fields.end();
            if (old_present && new_present && old_field->second == new_field->second)
                continue;
            std::string field = field_line(
                path,
                key,
                old_present ? &old_field->second : nullptr,
                new_present ? &new_field->second : nullptr,
                attribution
            );
            if (!field.empty())
                lines.push_back(std::move(field));
        }
        return;
    }
    if (before_file->second.sha256 != after_file->second.sha256) {
        std::string line = "file replaced " + escape_text(path) + " " + before_file->second.sha256 +
                           " " + after_file->second.sha256;
        finish_line(line, attribution, path, {});
        lines.push_back(std::move(line));
    }
}

/// The unit a `unit dropped` line names.
std::string dropped_name(std::string_view line) {
    constexpr std::string_view prefix = "unit dropped ";
    if (!line.starts_with(prefix))
        return {};
    const std::string_view rest = line.substr(prefix.size());
    const auto space = rest.find(' ');
    return std::string(rest.substr(0, space));
}

/// True for a download file or field line.
bool download_line(std::string_view line) {
    constexpr std::string_view kinds[] = {
        "file added ", "file hidden ", "file replaced ", "field "
    };
    for (const std::string_view kind : kinds) {
        if (!line.starts_with(kind))
            continue;
        return line.substr(kind.size()).starts_with("download/");
    }
    return false;
}

/// True when `text` is 64 hexadecimal digits.
bool hash_token(std::string_view text) {
    if (text.size() != 64)
        return false;
    for (const unsigned char piece : text) {
        const bool digit = piece >= '0' && piece <= '9';
        const bool lower = piece >= 'a' && piece <= 'f';
        if (!digit && !lower)
            return false;
    }
    return true;
}

/// The path a file or field line names.
///
/// A field path ends at the first ` [`. A hidden file, and a text file that
/// was added, take the rest of the line. A hashed file line ends at the
/// last hash token.
std::string line_path(std::string_view line) {
    constexpr std::string_view field = "field ";
    if (line.starts_with(field)) {
        const std::string_view rest = line.substr(field.size());
        const auto bracket = rest.find(" [");
        return std::string(rest.substr(0, bracket));
    }
    constexpr std::string_view kinds[] = {"file added ", "file hidden ", "file replaced "};
    for (const std::string_view kind : kinds) {
        if (!line.starts_with(kind))
            continue;
        std::string_view rest = line.substr(kind.size());
        if (kind != "file hidden ") {
            while (true) {
                const auto space = rest.rfind(' ');
                if (space == std::string_view::npos || !hash_token(rest.substr(space + 1)))
                    break;
                rest = rest.substr(0, space);
            }
        }
        return std::string(rest);
    }
    return {};
}

/// The FBI path a dropped unit has in one after-dump.
std::string fbi_path(const Dump& dump, const std::string& name) {
    const std::string exact = "units/" + lower_ascii(name) + ".fbi";
    if (dump.files.contains(exact) || dump.hidden.contains(exact))
        return exact;
    for (const auto& [path, file] : dump.files) {
        (void)file;
        if (unit_name_of(path) == name)
            return path;
    }
    for (const std::string& path : dump.hidden) {
        if (unit_name_of(path) == name)
            return path;
    }
    return exact;
}

/// True when a menu line names the unit as its builder or its unit.
bool menu_names(std::string_view line, const std::string& name) {
    constexpr std::string_view added = "menu added ";
    constexpr std::string_view removed = "menu removed ";
    if (!line.starts_with(added) && !line.starts_with(removed))
        return false;
    std::string builder;
    std::string unit;
    std::size_t word = 0;
    std::size_t begin = 0;
    while (begin <= line.size()) {
        const auto end = line.find(' ', begin);
        const std::string_view token = line.substr(begin, end - begin);
        if (word == 2)
            builder.assign(token);
        if (end == std::string_view::npos) {
            unit.assign(token);
            break;
        }
        ++word;
        begin = end + 1;
    }
    return builder == name || unit == name;
}

/// Section path and key of a field line.
bool field_parts(std::string_view line, std::string& path, std::string& section, std::string& key) {
    constexpr std::string_view prefix = "field ";
    if (!line.starts_with(prefix))
        return false;
    const std::string_view body = line.substr(prefix.size());
    const auto path_end = body.find(" [");
    if (path_end == std::string_view::npos)
        return false;
    path.assign(body.substr(0, path_end));
    const std::string_view after = body.substr(path_end + 1);
    const auto colon = after.find(':');
    if (colon == std::string_view::npos || colon == 0)
        return false;
    const std::string_view heading = after.substr(0, colon);
    const auto key_at = heading.rfind(' ');
    if (key_at == std::string_view::npos)
        return false;
    section.assign(heading.substr(0, key_at));
    key.assign(heading.substr(key_at + 1));
    return true;
}

/// The first `[gadget]` of a section path.
std::string gadget_of(std::string_view section) {
    if (section.empty() || section.front() != '[')
        return {};
    const auto end = section.find(']');
    if (end == std::string_view::npos)
        return {};
    return std::string(section.substr(0, end + 1));
}

/// True when `section` is `gadget` or a child of it, and not a longer gadget.
bool under_gadget(std::string_view section, std::string_view gadget) {
    if (!section.starts_with(gadget))
        return false;
    return section.size() == gadget.size() || section[gadget.size()] == '[';
}

/// True when a GUI field belongs to a gadget named for the unit.
bool gui_button(std::string_view line, const Dump& dump, const std::string& name) {
    std::string path;
    std::string section;
    std::string key;
    if (!field_parts(line, path, section, key) || !path.starts_with("guis/"))
        return false;
    const std::string gadget = gadget_of(section);
    if (gadget.empty() || !under_gadget(section, gadget))
        return false;
    const auto file = dump.files.find(path);
    if (file == dump.files.end())
        return false;
    const std::string* const common = field_of(file->second, gadget + "[common] name");
    const std::string* const plain = field_of(file->second, gadget + " name");
    const bool named = (common != nullptr && same_ascii(*common, name)) ||
                       (plain != nullptr && same_ascii(*plain, name));
    return named;
}

/// `*` any run, `?` one character, matched against the whole text.
bool pattern_matches(std::string_view pattern, std::string_view text) {
    std::vector<char> previous(text.size() + 1, 0);
    std::vector<char> current(text.size() + 1, 0);
    previous[0] = 1;
    for (const char piece : pattern) {
        std::fill(current.begin(), current.end(), 0);
        if (piece == '*') {
            current[0] = previous[0];
            for (std::size_t index = 1; index <= text.size(); ++index)
                current[index] = static_cast<char>(current[index - 1] != 0 || previous[index] != 0);
        } else {
            for (std::size_t index = 1; index <= text.size(); ++index) {
                const bool one = piece == '?' || piece == text[index - 1];
                current[index] = static_cast<char>(one && previous[index - 1] != 0);
            }
        }
        previous.swap(current);
    }
    return previous[text.size()] != 0;
}

/// Paths the two dumps mention.
std::set<std::string> every_path(const Dump& before, const Dump& after) {
    std::set<std::string> paths;
    for (const auto& [path, file] : before.files) {
        (void)file;
        paths.insert(path);
    }
    for (const auto& [path, file] : after.files) {
        (void)file;
        paths.insert(path);
    }
    return paths;
}

/// The hidden-path set a lookup observer fills.
struct HiddenNote {
    std::set<std::string>* hidden{}; ///< paths the mutator layer hid
};

/// Records a path the mutator layer hid.
void note_hidden(void* context, std::string_view path, std::string_view owner, bool hidden) {
    (void)owner;
    if (!hidden || context == nullptr)
        return;
    auto* const note = static_cast<HiddenNote*>(context);
    if (note->hidden == nullptr)
        return;
    note->hidden->insert(normalize_path(path));
}

} // namespace

void Attribution::add(std::string_view path, std::string_view field, std::string_view id) {
    std::string key = normalize_path(path);
    key.push_back('\n');
    key.append(lower_ascii(field));
    std::vector<std::string>& owners = by_[std::move(key)];
    const std::string name(id);
    if (std::find(owners.begin(), owners.end(), name) == owners.end())
        owners.push_back(name);
}

const std::vector<std::string>*
Attribution::ids(std::string_view path, std::string_view field) const {
    std::string key = normalize_path(path);
    key.push_back('\n');
    key.append(lower_ascii(field));
    const auto found = by_.find(key);
    if (found == by_.end())
        return nullptr;
    return &found->second;
}

Dump dump_store(oa::AssetStore& store, const Dump* unchanged) {
    Dump dump;
    // The observer is cleared before this returns, which also clears one the
    // caller had installed. Nothing keeps the note.
    HiddenNote note{&dump.hidden};
    oa::LookupObserver observer{};
    observer.context = &note;
    observer.mutator_layer_used = note_hidden;
    store.observe_lookups(observer);
    std::vector<std::string> listed;
    try {
        listed = store.list_effective_recursive("", "");
    } catch (...) {
        store.observe_lookups({});
        throw;
    }
    store.observe_lookups({});

    for (const std::string& listed_path : listed) {
        const std::string path = normalize_path(listed_path);
        const oa::Provider provider = store.provider(path);
        const bool layered = provider.kind == oa::ProviderKind::mutator_layer;
        if (unchanged != nullptr && !layered && !dump.hidden.contains(path)) {
            const auto kept = unchanged->files.find(path);
            if (kept != unchanged->files.end()) {
                dump.files.insert_or_assign(path, kept->second);
                continue;
            }
        }
        const std::vector<uint8_t> bytes = store.read(path).bytes;
        DumpedFile file;
        file.source = source_label(provider);
        file.sha256 = sha256_of(bytes);
        if (text_extension(path) && bytes.size() <= tdf::max_input_bytes) {
            const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            parse_fields(text, file);
        }
        dump.files.insert_or_assign(path, std::move(file));
    }

    const defs::Files files = defs::asset_store_files(&store);
    std::vector<std::string> unit_paths;
    for (const auto& [path, file] : dump.files) {
        (void)file;
        if (!unit_name_of(path).empty())
            unit_paths.push_back(path);
    }
    for (const std::string& path : dump.hidden) {
        if (!unit_name_of(path).empty())
            unit_paths.push_back(path);
    }
    for (const std::string& path : unit_paths)
        classify_unit(files, dump, path);
    collect_menus(dump);
    return dump;
}

std::vector<std::string>
report_lines(const Dump& before, const Dump& after, const Attribution& attribution) {
    std::vector<std::string> lines;
    const std::set<std::string> before_units = available_units(before);
    const std::set<std::string> after_units = available_units(after);
    for (const std::string& name : before_units) {
        if (after_units.contains(name))
            continue;
        const auto reason = after.dropped_units.find(name);
        const std::string why = reason != after.dropped_units.end() ? reason->second : "hidden";
        std::string line = "unit dropped " + escape_text(name) + " " + why;
        finish_line(line, attribution, "units/" + lower_ascii(name) + ".fbi", {});
        lines.push_back(std::move(line));
    }
    for (const std::string& name : after_units) {
        if (before_units.contains(name))
            continue;
        std::string line = "unit added " + escape_text(name);
        finish_line(line, attribution, "units/" + lower_ascii(name) + ".fbi", {});
        lines.push_back(std::move(line));
    }
    for (const std::string& path : every_path(before, after))
        append_path_lines(lines, path, before, after, attribution);
    for (const MenuEntry& entry : before.menus) {
        if (after.menus.contains(entry))
            continue;
        std::string line = "menu removed " + escape_text(entry.builder) + " " +
                           escape_text(entry.page) + " " + escape_text(entry.button) + " " +
                           escape_text(entry.unit);
        finish_line(line, attribution, "menu", entry.unit);
        lines.push_back(std::move(line));
    }
    for (const MenuEntry& entry : after.menus) {
        if (before.menus.contains(entry))
            continue;
        std::string line = "menu added " + escape_text(entry.builder) + " " +
                           escape_text(entry.page) + " " + escape_text(entry.button) + " " +
                           escape_text(entry.unit);
        finish_line(line, attribution, "menu", entry.unit);
        lines.push_back(std::move(line));
    }
    std::sort(lines.begin(), lines.end());
    return lines;
}

Comparison compare(
    const std::vector<std::string>& left,
    const std::vector<std::string>& right,
    const Dump& left_after,
    const Dump& right_after,
    const AllowList& allowed
) {
    std::set<std::string> left_names;
    std::set<std::string> right_names;
    for (const std::string& line : left) {
        const std::string name = dropped_name(before_tab(line));
        if (!name.empty())
            left_names.insert(name);
    }
    for (const std::string& line : right) {
        const std::string name = dropped_name(before_tab(line));
        if (!name.empty())
            right_names.insert(name);
    }
    std::set<std::string> both;
    for (const std::string& name : left_names) {
        if (right_names.contains(name))
            both.insert(name);
    }

    const auto fold = [&](const std::vector<std::string>& lines, const Dump& after) {
        std::set<std::string> folded;
        for (const std::string& raw : lines) {
            std::string line = before_tab(raw);
            if (download_line(line))
                continue;
            const std::string name = dropped_name(line);
            if (!name.empty() && both.contains(name))
                line = "unit dropped " + name;
            bool drop = false;
            for (const std::string& dropped : both) {
                const std::string path = fbi_path(after, dropped);
                if (!path.empty() && line_path(line) == path) {
                    drop = true;
                    break;
                }
                if (menu_names(line, dropped) || gui_button(line, after, dropped)) {
                    drop = true;
                    break;
                }
            }
            if (!drop)
                folded.insert(std::move(line));
        }
        return folded;
    };

    const std::set<std::string> left_folded = fold(left, left_after);
    const std::set<std::string> right_folded = fold(right, right_after);
    Comparison result;
    std::vector<char> used(allowed.patterns.size(), 0);
    const auto keep = [&](const std::string& line, bool left_side) {
        const std::string shown = (left_side ? "left only " : "right only ") + line;
        bool matched = false;
        for (std::size_t index = 0; index < allowed.patterns.size(); ++index) {
            if (!pattern_matches(allowed.patterns[index], shown))
                continue;
            used[index] = 1;
            matched = true;
        }
        if (matched)
            result.allowed.push_back(shown);
        else if (left_side)
            result.left_only.push_back(shown);
        else
            result.right_only.push_back(shown);
    };
    for (const std::string& line : left_folded) {
        if (!right_folded.contains(line))
            keep(line, true);
    }
    for (const std::string& line : right_folded) {
        if (!left_folded.contains(line))
            keep(line, false);
    }
    std::sort(result.left_only.begin(), result.left_only.end());
    std::sort(result.right_only.begin(), result.right_only.end());
    std::sort(result.allowed.begin(), result.allowed.end());
    for (std::size_t index = 0; index < allowed.patterns.size(); ++index) {
        if (used[index] == 0)
            result.unused_patterns.push_back(allowed.patterns[index]);
    }
    return result;
}

std::optional<AllowList> parse_allowed(std::string_view text, std::string* error) {
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.remove_prefix(3);
    }
    AllowList list;
    bool reason = false;
    std::size_t line_number = 1;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        auto end = text.find('\n', begin);
        if (end == std::string_view::npos)
            end = text.size();
        std::string_view line = text.substr(begin, end - begin);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        if (line.empty()) {
            reason = false;
        } else if (line.front() == '#') {
            reason = true;
        } else if (!reason) {
            if (error != nullptr)
                *error = std::to_string(line_number) + ": a pattern needs a reason above it";
            return std::nullopt;
        } else {
            list.patterns.emplace_back(line);
        }
        if (end == text.size())
            break;
        begin = end + 1;
        ++line_number;
    }
    return list;
}

void write_dump(std::ostream& out, const Dump& dump) {
    for (const auto& [path, file] : dump.files) {
        out << escape_text(path) << '\t' << escape_text(file.source) << '\t' << file.sha256 << '\n';
        if (!file.parsed)
            continue;
        for (const auto& [key, value] : file.fields)
            out << '\t' << escape_text(key) << " = " << escape_text(value) << '\n';
    }
}

} // namespace oa::tool::mutator_report
