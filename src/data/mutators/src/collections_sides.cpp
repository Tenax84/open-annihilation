// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Sides and the other game-data tables on a hook's ctx.
//
// Sides are read from the language variant of sidedata when that file is
// present, because load_side_data reads the variant. Build lists are read
// from the plain gamedata/sidedata.tdf, because the build-list loader
// always passes an empty variant. When those paths differ, each document
// is served at the path its own reader opens.
#include "collections_sides.hpp"

#include "oa/data/defs/asset_files.hpp"
#include "oa/data/defs/files.hpp"
#include "oa/data/mutators/allow_list.hpp"
#include "oa/formats/tdf/edit.hpp"

#include "lua.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::data::mutators {
namespace rules = oa::data::mutation_rules;
namespace tdf = oa::formats::tdf;
namespace defs = oa::data::defs;

/// The sides and tables collections reach the journal through here.
struct SideCollectionsAccess {
    static rules::DocumentSet& documents(Collections& self) noexcept { return *self.documents_; }

    static bool active(const Collections& self) noexcept { return self.active_; }

    static const std::string& mutator(const Collections& self) noexcept { return self.mutator_; }

    static const WriteGrant& grant(const Collections& self) noexcept { return self.grant_; }

    static bool accept_write(Collections& self, rules::FieldWrite write, std::string& reason) {
        return self.accept_write(std::move(write), reason);
    }

    static bool accept_note(Collections& self, HookNote note, std::string& reason) {
        return self.accept_note(std::move(note), reason);
    }

    static void hold_error(Collections& self, std::string message) {
        self.hold_error(std::move(message));
    }

    static int raise(Collections& self, lua_State* state) { return self.raise_held(state); }
};

namespace {

constexpr uint32_t k_side_slots = 5;
constexpr uint32_t k_list_kept = 30;
constexpr uint32_t k_list_cap = 1024;
constexpr std::size_t k_name_limit = 29;
constexpr std::size_t k_prefix_limit = 3;
constexpr std::size_t k_commander_limit = 31;
constexpr std::size_t k_intgaf_limit = 29;
constexpr std::size_t k_font_limit = 255;
constexpr std::size_t k_text_limit = tdf::max_edited_value_bytes;
constexpr std::size_t k_no_doc = std::numeric_limits<std::size_t>::max();
constexpr const char* k_list_note =
    "the AI keeps only the profile's limits.build-list-entries count (30 by default)";

constexpr const char* k_sides_meta = "oa.mutators.sides";
constexpr const char* k_side_meta = "oa.mutators.side";
constexpr const char* k_tables_meta = "oa.mutators.tables";
constexpr const char* k_table_meta = "oa.mutators.table";
constexpr const char* k_section_meta = "oa.mutators.section";

enum class BoxKind : uint8_t { sides, side, tables, table, section };

/// Pointers a callback may use. None of them is owned here.
struct Box {
    Collections* collections{};
    const oa::AssetStore* store{};
    const defs::DataLayout* layout{};
    const std::string* variant{};
    BoxKind kind{BoxKind::sides};
    uint32_t side_slot{};
    std::size_t doc_index{k_no_doc};
    tdf::Block* block{};
    tdf::Block* parent{};
};

struct UnitRef {
    bool found{false};
    bool builder{false};
    std::string name;
};

struct FieldText {
    bool present{false};
    std::string key;
    std::string text;
};

char fold_ascii(char byte) {
    if (byte >= 'A' && byte <= 'Z')
        return static_cast<char>(byte - 'A' + 'a');
    return byte;
}

std::string fold_copy(std::string_view text) {
    std::string out;
    out.resize(text.size());
    for (std::size_t offset = 0; offset < text.size(); ++offset)
        out[offset] = fold_ascii(text[offset]);
    return out;
}

bool same_text(std::string_view earlier, std::string_view later) {
    if (earlier.size() != later.size())
        return false;
    for (std::size_t offset = 0; offset < earlier.size(); ++offset) {
        if (fold_ascii(earlier[offset]) != fold_ascii(later[offset]))
            return false;
    }
    return true;
}

bool same_path(std::string_view earlier, std::string_view later) {
    if (earlier.size() != later.size())
        return false;
    for (std::size_t offset = 0; offset < earlier.size(); ++offset) {
        char first_byte = earlier[offset] == '\\' ? '/' : fold_ascii(earlier[offset]);
        char second_byte = later[offset] == '\\' ? '/' : fold_ascii(later[offset]);
        if (first_byte != second_byte)
            return false;
    }
    return true;
}

std::string slash_copy(std::string path) {
    for (char& byte : path) {
        if (byte == '\\')
            byte = '/';
    }
    return path;
}

std::string_view trim_ends(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size()) {
        const char byte = text[begin];
        if (byte != ' ' && byte != '\t' && byte != '\n' && byte != '\r')
            break;
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin) {
        const char byte = text[end - 1];
        if (byte != ' ' && byte != '\t' && byte != '\n' && byte != '\r')
            break;
        --end;
    }
    return text.substr(begin, end - begin);
}

bool cutting_character(std::string_view text, std::string& reason) {
    for (const char byte : text) {
        if (byte == ';' || byte == '{' || byte == '}' || byte == '[' || byte == ']' ||
            byte == '\n' || byte == '\r' || byte == '\0') {
            reason = "a value cannot hold ';', braces, brackets or a line break";
            return true;
        }
    }
    for (std::size_t offset = 0; offset + 1 < text.size(); ++offset) {
        if (text[offset] == '/' && (text[offset + 1] == '/' || text[offset + 1] == '*')) {
            reason = "a value cannot hold a comment mark";
            return true;
        }
    }
    return false;
}

bool has_slash(std::string_view text) {
    return text.find('/') != std::string_view::npos || text.find('\\') != std::string_view::npos;
}

bool layer_ok(Collections& collections, bool allowed, const char* layer, std::string& reason) {
    if (!SideCollectionsAccess::active(collections)) {
        reason = "no mutator has begun";
        return false;
    }
    if (allowed)
        return true;
    reason = "this mutator does not declare the ";
    reason.append(layer);
    reason.append(" layer");
    return false;
}

bool commit_field(
    Collections& collections,
    std::string path,
    std::string record,
    std::string key,
    std::optional<std::string> old_text,
    std::optional<std::string> new_text,
    std::string& reason
) {
    rules::FieldWrite write;
    write.mutators.push_back(SideCollectionsAccess::mutator(collections));
    write.path = std::move(path);
    write.record = std::move(record);
    write.key = std::move(key);
    write.old_text = std::move(old_text);
    write.new_text = std::move(new_text);
    return SideCollectionsAccess::accept_write(collections, std::move(write), reason);
}

bool commit_note(
    Collections& collections,
    std::string path,
    std::string record,
    std::string message,
    std::string& reason
) {
    HookNote note;
    note.kind = HookNoteKind::check_warned;
    note.mutators.push_back(SideCollectionsAccess::mutator(collections));
    note.path = std::move(path);
    note.record = std::move(record);
    note.message = std::move(message);
    return SideCollectionsAccess::accept_note(collections, std::move(note), reason);
}

std::string directory_name(const defs::DataLayout& layout, defs::DataDirectory which) {
    return layout.directories[static_cast<std::size_t>(which)];
}

/// The side file load_side_data would open: the variant when it exists.
std::string side_file_path(const Box& box) {
    const defs::Files boundary = defs::asset_store_files(box.store);
    std::array<char, defs::path_capacity> buffer{};
    const char* word =
        box.variant != nullptr && !box.variant->empty() ? box.variant->c_str() : nullptr;
    defs::build_variant_path(
        &boundary,
        buffer.data(),
        buffer.size(),
        directory_name(*box.layout, defs::DataDirectory::gamedata).c_str(),
        "sidedata",
        "tdf",
        word
    );
    return slash_copy(buffer.data());
}

/// The file the build-list loader opens. It never asks for a variant.
std::string list_file_path(const Box& box) {
    return directory_name(*box.layout, defs::DataDirectory::gamedata) + "/sidedata.tdf";
}

std::size_t document_index(
    Collections& collections,
    const oa::AssetStore& store,
    const std::string& path,
    std::string& error,
    bool required
) {
    rules::DocumentSet& documents = SideCollectionsAccess::documents(collections);
    for (std::size_t slot = 0; slot < documents.size(); ++slot) {
        rules::LoadedDocument& loaded = documents.at(slot);
        if (!same_path(loaded.path, path))
            continue;
        if (loaded.removed) {
            error = path + " was removed";
            return k_no_doc;
        }
        if (loaded.document.root() == nullptr) {
            error = path + ": the table could not be read";
            return k_no_doc;
        }
        return slot;
    }
    oa::AssetData data;
    try {
        data = store.read(path);
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception&) {
        if (required)
            error = path + " is missing";
        return k_no_doc;
    }
    const std::string text(data.bytes.begin(), data.bytes.end());
    std::string add_error;
    if (!documents.add(path, text, &add_error)) {
        for (std::size_t slot = 0; slot < documents.size(); ++slot) {
            if (same_path(documents.at(slot).path, path))
                return slot;
        }
        error = path;
        error.append(": ");
        error.append(add_error);
        return k_no_doc;
    }
    for (std::size_t slot = 0; slot < documents.size(); ++slot) {
        if (same_path(documents.at(slot).path, path))
            return slot;
    }
    error = path + ": the table could not be read";
    return k_no_doc;
}

tdf::Block* side_block(rules::LoadedDocument& loaded, uint32_t slot) {
    tdf::Block* root = tdf::editable_root(loaded.document.get());
    if (root == nullptr)
        return nullptr;
    const std::string name = std::string("SIDE") + std::to_string(slot);
    return tdf::editable_child(root, name);
}

uint32_t count_loaded_sides(rules::LoadedDocument& loaded) {
    uint32_t count = 0;
    while (count < k_side_slots && side_block(loaded, count) != nullptr)
        ++count;
    return count;
}

uint32_t count_sides(const Box& box, std::string& error) {
    const std::string path = side_file_path(box);
    if (path.empty())
        return 0;
    const std::size_t slot = document_index(*box.collections, *box.store, path, error, false);
    if (!error.empty() || slot == k_no_doc)
        return 0;
    return count_loaded_sides(SideCollectionsAccess::documents(*box.collections).at(slot));
}

std::optional<uint32_t> side_named(const Box& box, std::string_view sought, std::string& error) {
    const std::string path = side_file_path(box);
    if (path.empty())
        return std::nullopt;
    const std::size_t slot = document_index(*box.collections, *box.store, path, error, false);
    if (!error.empty() || slot == k_no_doc)
        return std::nullopt;
    rules::LoadedDocument& loaded = SideCollectionsAccess::documents(*box.collections).at(slot);
    const uint32_t count = count_loaded_sides(loaded);
    for (uint32_t index = 0; index < count; ++index) {
        tdf::Block* block = side_block(loaded, index);
        const char* name = block != nullptr ? tdf::find_value(block, "name") : nullptr;
        if (name != nullptr && same_text(name, sought))
            return index;
    }
    return std::nullopt;
}

bool load_side(
    const Box& box,
    uint32_t slot,
    std::size_t& doc,
    tdf::Block*& block,
    std::string& path,
    std::string& error
) {
    path = side_file_path(box);
    if (path.empty()) {
        error = "gamedata/sidedata.tdf is missing";
        return false;
    }
    doc = document_index(*box.collections, *box.store, path, error, true);
    if (doc == k_no_doc)
        return false;
    rules::LoadedDocument& loaded = SideCollectionsAccess::documents(*box.collections).at(doc);
    path = loaded.path;
    const uint32_t count = count_loaded_sides(loaded);
    if (slot >= count) {
        error = "a mutator cannot add a side";
        return false;
    }
    block = side_block(loaded, slot);
    if (block == nullptr) {
        error = "a mutator cannot add a side";
        return false;
    }
    return true;
}

std::string file_stem(std::string_view path, std::string_view suffix) {
    const auto slash = path.rfind('/');
    std::string_view file = slash == std::string_view::npos ? path : path.substr(slash + 1);
    if (file.size() >= suffix.size() && same_text(file.substr(file.size() - suffix.size()), suffix))
        file.remove_suffix(suffix.size());
    return std::string(file);
}

std::string stem_of(std::string_view path) {
    const auto slash = path.rfind('/');
    std::string_view file = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const auto dot = file.rfind('.');
    if (dot != std::string_view::npos)
        file = file.substr(0, dot);
    return std::string(file);
}

UnitRef
find_unit(Collections& collections, const defs::DataLayout& layout, std::string_view sought) {
    rules::DocumentSet& documents = SideCollectionsAccess::documents(collections);
    const std::string folder = directory_name(layout, defs::DataDirectory::units);
    const std::string suffix = std::string(".") + layout.unit_extension;
    const std::vector<std::size_t> listed = documents.documents_in(folder, suffix, false);
    UnitRef found;
    for (const std::size_t slot : listed) {
        rules::LoadedDocument& loaded = documents.at(slot);
        tdf::Block* root = tdf::editable_root(loaded.document.get());
        if (root == nullptr)
            continue;
        const uint32_t children = tdf::child_count(root);
        for (uint32_t child_index = 0; child_index < children; ++child_index) {
            tdf::Block* section = tdf::editable_child(root, child_index);
            if (section == nullptr || section->name == nullptr)
                continue;
            if (tdf::compare_nocase(section->name, "UNITINFO") != 0)
                continue;
            std::string unit_name;
            const char* written = tdf::find_value(section, "UnitName");
            if (written != nullptr && written[0] != '\0')
                unit_name = written;
            else
                unit_name = file_stem(loaded.path, suffix);
            if (!same_text(unit_name, sought))
                continue;
            found.found = true;
            found.builder = tdf::get_int(section, "builder", 0) != 0;
            found.name = std::move(unit_name);
            return found;
        }
    }
    return found;
}

bool readable(const oa::AssetStore& store, const std::string& path) {
    try {
        (void)store.read(path);
        return true;
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception&) {
        return false;
    }
}

bool art_exists(
    const Box& box, const char* directory, const std::string& name, const char* extension
) {
    const std::string plain = std::string(directory) + "/" + name + "." + extension;
    if (box.variant != nullptr && !box.variant->empty()) {
        const std::string varied =
            std::string(directory) + "-" + *box.variant + "/" + name + "." + extension;
        if (readable(*box.store, varied))
            return true;
    }
    return readable(*box.store, plain);
}

FieldText field_text(const tdf::Block* block, std::string_view key) {
    FieldText found;
    found.key.assign(key.begin(), key.end());
    if (block == nullptr)
        return found;
    const uint32_t count = tdf::property_count(block);
    for (uint32_t index = 0; index < count; ++index) {
        const char* stored = tdf::property_key_at(block, static_cast<int32_t>(index));
        if (stored == nullptr || !same_text(stored, key))
            continue;
        found.present = true;
        found.key = stored;
        const char* value = tdf::property_value_at(block, static_cast<int32_t>(index));
        found.text = value != nullptr ? value : "";
        break;
    }
    return found;
}

bool child_named(const tdf::Block* block, std::string_view key) {
    if (block == nullptr)
        return false;
    const uint32_t count = tdf::child_count(block);
    for (uint32_t index = 0; index < count; ++index) {
        const tdf::Block* child = tdf::child_at(block, index);
        if (child != nullptr && child->name != nullptr && same_text(child->name, key))
            return true;
    }
    return false;
}

bool put_text(
    tdf::Document* document,
    tdf::Block* block,
    const std::string& path,
    const std::string& record,
    const std::string& key,
    const std::string& value,
    Collections& collections,
    std::string& reason
) {
    const FieldText current = field_text(block, key);
    if (current.present && current.text == value)
        return true;
    const std::string& stored_key = current.present ? current.key : key;
    const tdf::EditStatus status = tdf::set_value(document, block, stored_key, value);
    if (status != tdf::EditStatus::ok) {
        reason = tdf::edit_status_message(status);
        return false;
    }
    std::optional<std::string> old_text;
    if (current.present)
        old_text = current.text;
    return commit_field(collections, path, record, stored_key, std::move(old_text), value, reason);
}

bool prefix_pages(
    const Box& box, std::string_view present, std::string_view updated, std::string& reason
) {
    if (present.empty() || same_text(present, updated))
        return true;
    const char* pages[] = {"GEN.GUI", "MAIN2.GUI", "DL.GUI"};
    const std::string folder = directory_name(*box.layout, defs::DataDirectory::guis);
    for (const char* page : pages) {
        const std::string had = folder + "/" + std::string(present) + page;
        const std::string next = folder + "/" + std::string(updated) + page;
        if (readable(*box.store, had) && !readable(*box.store, next)) {
            reason = "the prefix has no ";
            reason.append(next);
            reason.append(" page");
            return false;
        }
    }
    return true;
}

bool name_free(const Box& box, uint32_t self_slot, std::string_view updated, std::string& reason) {
    const std::string path = side_file_path(box);
    const std::size_t doc = document_index(*box.collections, *box.store, path, reason, true);
    if (doc == k_no_doc)
        return false;
    rules::LoadedDocument& loaded = SideCollectionsAccess::documents(*box.collections).at(doc);
    const uint32_t count = count_loaded_sides(loaded);
    for (uint32_t index = 0; index < count; ++index) {
        if (index == self_slot)
            continue;
        tdf::Block* block = side_block(loaded, index);
        const char* name = block != nullptr ? tdf::find_value(block, "name") : nullptr;
        if (name != nullptr && same_text(name, updated)) {
            reason = "a side already has that name";
            return false;
        }
    }
    reason.clear();
    return true;
}

bool checked_text(
    const Box& box,
    uint32_t self_slot,
    tdf::Block* block,
    std::string_view key,
    std::string_view raw,
    std::string& stored,
    std::string& reason
) {
    if (cutting_character(raw, reason))
        return false;
    const std::string_view trimmed = trim_ends(raw);
    stored.assign(trimmed.begin(), trimmed.end());
    if (same_text(key, "name")) {
        if (stored.empty() || stored.size() > k_name_limit) {
            reason = "a side name is 1 to 29 characters";
            return false;
        }
        return name_free(box, self_slot, stored, reason);
    }
    if (same_text(key, "nameprefix")) {
        if (has_slash(raw)) {
            reason = "a prefix cannot hold a slash";
            return false;
        }
        if (stored.empty() || stored.size() > k_prefix_limit) {
            reason = "a prefix is 1 to 3 letters or digits";
            return false;
        }
        for (const char byte : stored) {
            const bool digit = byte >= '0' && byte <= '9';
            const bool letter = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z');
            if (!digit && !letter) {
                reason = "a prefix is 1 to 3 letters or digits";
                return false;
            }
        }
        const FieldText present = field_text(block, "nameprefix");
        const std::string_view had =
            present.present ? std::string_view(present.text) : std::string_view();
        return prefix_pages(box, had, stored, reason);
    }
    if (same_text(key, "commander")) {
        if (stored.empty() || stored.size() > k_commander_limit) {
            reason = "a commander name is 1 to 31 characters";
            return false;
        }
        const UnitRef unit = find_unit(*box.collections, *box.layout, stored);
        if (!unit.found) {
            reason = "there is no unit ";
            reason.append(stored);
            return false;
        }
        if (cutting_character(unit.name, reason))
            return false;
        stored = unit.name;
        return true;
    }
    if (same_text(key, "intgaf")) {
        if (has_slash(raw)) {
            reason = "an intgaf name cannot hold a slash";
            return false;
        }
        if (stored.empty() || stored.size() > k_intgaf_limit) {
            reason = "an intgaf name is 1 to 29 characters";
            return false;
        }
        if (!art_exists(box, "anims", stored, "GAF")) {
            reason = "an intgaf file is missing: anims/";
            reason.append(stored);
            reason.append(".GAF");
            return false;
        }
        return true;
    }
    if (same_text(key, "font")) {
        if (has_slash(raw)) {
            reason = "a font name cannot hold a slash";
            return false;
        }
        if (stored.empty() || stored.size() > k_font_limit) {
            reason = "a font name is 1 to 255 characters";
            return false;
        }
        if (!art_exists(box, "fonts", stored, "FNT")) {
            reason = "a font file is missing: fonts/";
            reason.append(stored);
            reason.append(".FNT");
            return false;
        }
        return true;
    }
    if (stored.size() > k_text_limit) {
        reason = "a value is longer than the field can hold";
        return false;
    }
    return true;
}

bool parse_canbuild_key(std::string_view key, uint32_t& number) {
    const std::string folded = fold_copy(key);
    constexpr std::string_view prefix = "canbuild";
    if (folded.size() <= prefix.size() || folded.compare(0, prefix.size(), prefix) != 0)
        return false;
    uint32_t value = 0;
    for (std::size_t offset = prefix.size(); offset < folded.size(); ++offset) {
        const char digit = folded[offset];
        if (digit < '0' || digit > '9')
            return false;
        const auto piece = static_cast<uint32_t>(digit - '0');
        if (value > (std::numeric_limits<uint32_t>::max() - piece) / 10u)
            return false;
        value = value * 10u + piece;
    }
    number = value;
    return true;
}

std::vector<std::string> read_list(const tdf::Block* block) {
    std::vector<std::string> names;
    if (block == nullptr)
        return names;
    for (uint32_t number = 1; number <= k_list_cap; ++number) {
        const std::string key = std::string("canbuild") + std::to_string(number);
        const char* value = tdf::find_value(block, key.c_str());
        if (value == nullptr)
            break;
        names.emplace_back(value);
    }
    return names;
}

bool list_matches(const tdf::Block* block, const std::vector<std::string>& names) {
    if (block == nullptr)
        return names.empty();
    const std::vector<std::string> current = read_list(block);
    if (current.size() != names.size())
        return false;
    for (std::size_t slot = 0; slot < names.size(); ++slot) {
        if (current[slot] != names[slot])
            return false;
    }
    const uint32_t count = tdf::property_count(block);
    for (uint32_t key_index = 0; key_index < count; ++key_index) {
        const char* key = tdf::property_key_at(block, static_cast<int32_t>(key_index));
        if (key == nullptr)
            continue;
        uint32_t number = 0;
        if (!parse_canbuild_key(key, number))
            continue;
        const std::string canonical = std::string("canbuild") + std::to_string(number);
        if (number < 1 || static_cast<std::size_t>(number) > names.size() ||
            !same_text(key, canonical))
            return false;
    }
    return true;
}

bool write_list(
    tdf::Document* document,
    tdf::Block* unit_block,
    const std::vector<std::string>& names,
    const std::string& path,
    Collections& collections,
    std::string& reason
) {
    if (list_matches(unit_block, names))
        return true;
    const std::string record =
        std::string("CANBUILD.") + (unit_block->name != nullptr ? unit_block->name : "");
    for (std::size_t slot = 0; slot < names.size(); ++slot) {
        const std::string key = std::string("canbuild") + std::to_string(slot + 1);
        if (!put_text(document, unit_block, path, record, key, names[slot], collections, reason))
            return false;
    }
    std::vector<std::string> drop;
    const uint32_t count = tdf::property_count(unit_block);
    for (uint32_t key_index = 0; key_index < count; ++key_index) {
        const char* key = tdf::property_key_at(unit_block, static_cast<int32_t>(key_index));
        if (key == nullptr)
            continue;
        uint32_t number = 0;
        if (!parse_canbuild_key(key, number))
            continue;
        const std::string canonical = std::string("canbuild") + std::to_string(number);
        if (number < 1 || static_cast<std::size_t>(number) > names.size() ||
            !same_text(key, canonical))
            drop.emplace_back(key);
    }
    for (const std::string& key : drop) {
        const FieldText current = field_text(unit_block, key);
        if (!current.present)
            continue;
        const tdf::EditStatus status = tdf::remove_key(document, unit_block, current.key);
        if (status != tdf::EditStatus::ok) {
            reason = tdf::edit_status_message(status);
            return false;
        }
        if (!commit_field(
                collections, path, record, current.key, current.text, std::nullopt, reason
            ))
            return false;
    }
    if (names.size() > k_list_kept) {
        if (!commit_note(collections, path, record, k_list_note, reason))
            return false;
    }
    return true;
}

tdf::Block*
ensure_canbuild(tdf::Document* document, tdf::Block* root, bool create, std::string& reason) {
    tdf::Block* section = tdf::editable_child(root, "CANBUILD");
    if (section != nullptr || !create)
        return section;
    tdf::EditStatus status = tdf::EditStatus::ok;
    section = tdf::add_child(document, root, "CANBUILD", &status);
    if (section == nullptr)
        reason = tdf::edit_status_message(status);
    return section;
}

tdf::Block* ensure_builder(
    tdf::Document* document,
    tdf::Block* section,
    const std::string& name,
    bool create,
    std::string& reason
) {
    tdf::Block* block = tdf::editable_child(section, name);
    if (block != nullptr || !create)
        return block;
    tdf::EditStatus status = tdf::EditStatus::ok;
    block = tdf::add_child(document, section, name, &status);
    if (block == nullptr)
        reason = tdf::edit_status_message(status);
    return block;
}

bool unit_names(
    const Box& box,
    const std::vector<std::string>& raw_names,
    bool need_builder,
    std::string& builder,
    std::vector<std::string>& names,
    std::string& reason
) {
    const UnitRef unit = find_unit(*box.collections, *box.layout, builder);
    if (!unit.found) {
        reason = "there is no unit ";
        reason.append(builder);
        return false;
    }
    if (need_builder && !unit.builder) {
        reason = "the unit is not a builder";
        return false;
    }
    builder = unit.name;
    names.clear();
    names.reserve(raw_names.size());
    for (const std::string& raw_name : raw_names) {
        const UnitRef listed = find_unit(*box.collections, *box.layout, raw_name);
        if (!listed.found) {
            reason = "there is no unit ";
            reason.append(raw_name);
            return false;
        }
        if (cutting_character(listed.name, reason))
            return false;
        names.push_back(listed.name);
    }
    return true;
}

bool load_list_document(const Box& box, std::size_t& doc, std::string& path, std::string& error) {
    path = list_file_path(box);
    doc = document_index(*box.collections, *box.store, path, error, true);
    if (doc == k_no_doc)
        return false;
    path = SideCollectionsAccess::documents(*box.collections).at(doc).path;
    return true;
}

void push_names(lua_State* state, const std::vector<std::string>& names) {
    lua_createtable(state, static_cast<int>(names.size()), 0);
    for (std::size_t slot = 0; slot < names.size(); ++slot) {
        lua_pushlstring(state, names[slot].data(), names[slot].size());
        lua_rawseti(state, -2, static_cast<lua_Integer>(slot + 1));
    }
}

std::string canonical_table_path(const defs::DataLayout& layout, std::string_view requested) {
    std::string text(requested);
    for (char& byte : text) {
        if (byte == '\\')
            byte = '/';
    }
    while (!text.empty() && text.front() == '/')
        text.erase(text.begin());
    while (!text.empty() && text.back() == '/')
        text.pop_back();
    const std::string folder = directory_name(layout, defs::DataDirectory::gamedata);
    std::string rest = text;
    const std::string prefix = folder + "/";
    if (rest.size() >= prefix.size() && same_text(rest.substr(0, prefix.size()), prefix))
        rest = rest.substr(prefix.size());
    if (rest.find('/') != std::string::npos)
        return text;
    std::string file = rest;
    if (file.size() < 4 || !same_text(file.substr(file.size() - 4), ".tdf"))
        file.append(".tdf");
    return folder + "/" + file;
}

bool sidedata_path(std::string_view path) {
    const auto slash = path.rfind('/');
    std::string file(slash == std::string::npos ? path : path.substr(slash + 1));
    if (file.size() >= 4 && same_text(file.substr(file.size() - 4), ".tdf"))
        file.resize(file.size() - 4);
    return same_text(file, "sidedata");
}

bool contains_block(const tdf::Block* node, const tdf::Block* sought) {
    if (node == nullptr || sought == nullptr)
        return false;
    if (node == sought)
        return true;
    for (uint32_t index = 0; index < node->child_count; ++index) {
        if (contains_block(node->children[index], sought))
            return true;
    }
    return false;
}

tdf::Block* root_of(const Box& box) {
    rules::LoadedDocument& loaded =
        SideCollectionsAccess::documents(*box.collections).at(box.doc_index);
    return tdf::editable_root(loaded.document.get());
}

bool section_live(const Box& box, std::string& reason) {
    const rules::DocumentSet& documents = SideCollectionsAccess::documents(*box.collections);
    if (box.doc_index >= documents.size() || box.block == nullptr) {
        reason = "that section was removed";
        return false;
    }
    const rules::LoadedDocument& loaded = documents.at(box.doc_index);
    if (loaded.removed || !contains_block(loaded.document.root(), box.block)) {
        reason = "that section was removed";
        return false;
    }
    return true;
}

std::string parent_record(const Box& box, const tdf::Block* parent) {
    const tdf::Block* root = root_of(box);
    if (parent == nullptr || parent == root) {
        const std::string& path =
            SideCollectionsAccess::documents(*box.collections).at(box.doc_index).path;
        return stem_of(path);
    }
    return parent->name != nullptr ? parent->name : std::string();
}

void push_box(lua_State* state, const Box& source, const char* meta) {
    auto* created = static_cast<Box*>(lua_newuserdatauv(state, sizeof(Box), 0));
    *created = source;
    luaL_setmetatable(state, meta);
}

Box* box_at(lua_State* state, int index, BoxKind kind) {
    const char* meta = k_sides_meta;
    if (kind == BoxKind::side)
        meta = k_side_meta;
    else if (kind == BoxKind::tables)
        meta = k_tables_meta;
    else if (kind == BoxKind::table)
        meta = k_table_meta;
    else if (kind == BoxKind::section)
        meta = k_section_meta;
    auto* box = static_cast<Box*>(luaL_testudata(state, index, meta));
    if (box == nullptr || box->kind != kind)
        return nullptr;
    return box;
}

template <typename Fn>
int finish(lua_State* state, Collections& collections, Fn&& body) {
    int results = 0;
    try {
        results = body();
    } catch (const std::bad_alloc&) {
        SideCollectionsAccess::hold_error(collections, "out of memory");
        results = -1;
    } catch (const std::exception& ex) {
        const char* what = ex.what();
        SideCollectionsAccess::hold_error(
            collections, what != nullptr && what[0] != '\0' ? what : "the hook stopped"
        );
        results = -1;
    }
    if (results < 0)
        return SideCollectionsAccess::raise(collections, state);
    return results;
}

std::string lua_string(lua_State* state, int index, bool& ok) {
    ok = lua_type(state, index) == LUA_TSTRING;
    if (!ok)
        return {};
    std::size_t length = 0;
    const char* text = lua_tolstring(state, index, &length);
    if (text == nullptr) {
        ok = false;
        return {};
    }
    return std::string(text, length);
}

void push_side(lua_State* state, const Box& source, uint32_t slot) {
    Box copy = source;
    copy.kind = BoxKind::side;
    copy.side_slot = slot;
    copy.block = nullptr;
    copy.parent = nullptr;
    push_box(state, copy, k_side_meta);
}

int sides_next(lua_State* state) {
    Box* box = box_at(state, 1, BoxKind::sides);
    if (box == nullptr)
        return 0;
    uint32_t count = 0;
    bool failed = false;
    try {
        std::string error;
        count = count_sides(*box, error);
        if (!error.empty()) {
            SideCollectionsAccess::hold_error(*box->collections, std::move(error));
            failed = true;
        }
    } catch (const std::bad_alloc&) {
        SideCollectionsAccess::hold_error(*box->collections, "out of memory");
        failed = true;
    } catch (const std::exception& ex) {
        const char* what = ex.what();
        SideCollectionsAccess::hold_error(
            *box->collections, what != nullptr && what[0] != '\0' ? what : "the hook stopped"
        );
        failed = true;
    }
    if (failed)
        return SideCollectionsAccess::raise(*box->collections, state);
    lua_Integer slot = 0;
    if (lua_isinteger(state, 2))
        slot = lua_tointeger(state, 2);
    if (slot < 0 || static_cast<uint64_t>(slot) >= count)
        return 0;
    lua_pushinteger(state, slot + 1);
    push_side(state, *box, static_cast<uint32_t>(slot));
    return 2;
}

int sides_pairs(lua_State* state) {
    lua_pushcfunction(state, sides_next);
    lua_pushvalue(state, 1);
    lua_pushinteger(state, 0);
    return 3;
}

int sides_len(lua_State* state) {
    Box* box = box_at(state, 1, BoxKind::sides);
    if (box == nullptr) {
        lua_pushliteral(state, "expected the sides collection");
        return lua_error(state);
    }
    return finish(state, *box->collections, [&]() -> int {
        std::string error;
        const uint32_t count = count_sides(*box, error);
        if (!error.empty()) {
            SideCollectionsAccess::hold_error(*box->collections, std::move(error));
            return -1;
        }
        lua_pushinteger(state, static_cast<lua_Integer>(count));
        return 1;
    });
}

int read_canbuild(lua_State* state) {
    Box* box = box_at(state, 1, BoxKind::sides);
    if (box == nullptr) {
        lua_pushliteral(state, "expected the sides collection");
        return lua_error(state);
    }
    return finish(state, *box->collections, [&]() -> int {
        bool ok = false;
        const std::string builder = lua_string(state, 2, ok);
        if (!ok) {
            SideCollectionsAccess::hold_error(*box->collections, "a build list name is text");
            return -1;
        }
        std::string error;
        UnitRef unit = find_unit(*box->collections, *box->layout, builder);
        if (!unit.found) {
            error = "there is no unit ";
            error.append(builder);
            SideCollectionsAccess::hold_error(*box->collections, std::move(error));
            return -1;
        }
        std::size_t doc = k_no_doc;
        std::string path;
        if (!load_list_document(*box, doc, path, error)) {
            SideCollectionsAccess::hold_error(*box->collections, std::move(error));
            return -1;
        }
        rules::LoadedDocument& loaded = SideCollectionsAccess::documents(*box->collections).at(doc);
        tdf::Block* root = tdf::editable_root(loaded.document.get());
        tdf::Block* section = root != nullptr ? tdf::editable_child(root, "CANBUILD") : nullptr;
        tdf::Block* listed = section != nullptr ? tdf::editable_child(section, unit.name) : nullptr;
        const std::vector<std::string> names = read_list(listed);
        push_names(state, names);
        return 1;
    });
}

bool read_name_array(
    lua_State* state, int index, std::vector<std::string>& names, std::string& reason
) {
    if (lua_type(state, index) != LUA_TTABLE) {
        reason = "a build list is an array of unit names";
        return false;
    }
    const lua_Unsigned length = lua_rawlen(state, index);
    if (length > k_list_cap) {
        reason = "a build list keeps at most 1024 names";
        return false;
    }
    names.clear();
    names.reserve(static_cast<std::size_t>(length));
    for (lua_Unsigned slot = 1; slot <= length; ++slot) {
        lua_rawgeti(state, index, static_cast<lua_Integer>(slot));
        bool ok = false;
        std::string name = lua_string(state, -1, ok);
        lua_pop(state, 1);
        if (!ok) {
            reason = "a build list name is text";
            return false;
        }
        names.push_back(std::move(name));
    }
    return true;
}

int set_canbuild(lua_State* state) {
    Box* box = box_at(state, 1, BoxKind::sides);
    if (box == nullptr) {
        lua_pushliteral(state, "expected the sides collection");
        return lua_error(state);
    }
    return finish(state, *box->collections, [&]() -> int {
        std::string reason;
        if (!layer_ok(
                *box->collections,
                SideCollectionsAccess::grant(*box->collections).sides,
                "sides",
                reason
            )) {
            SideCollectionsAccess::hold_error(*box->collections, std::move(reason));
            return -1;
        }
        bool ok = false;
        std::string builder = lua_string(state, 2, ok);
        if (!ok) {
            SideCollectionsAccess::hold_error(*box->collections, "a build list name is text");
            return -1;
        }
        std::vector<std::string> raw_names;
        if (!read_name_array(state, 3, raw_names, reason)) {
            SideCollectionsAccess::hold_error(*box->collections, std::move(reason));
            return -1;
        }
        std::vector<std::string> names;
        if (!unit_names(*box, raw_names, true, builder, names, reason)) {
            SideCollectionsAccess::hold_error(*box->collections, std::move(reason));
            return -1;
        }
        std::size_t doc = k_no_doc;
        std::string path;
        if (!load_list_document(*box, doc, path, reason)) {
            SideCollectionsAccess::hold_error(*box->collections, std::move(reason));
            return -1;
        }
        rules::LoadedDocument& loaded = SideCollectionsAccess::documents(*box->collections).at(doc);
        tdf::Document* document = loaded.document.get();
        tdf::Block* root = tdf::editable_root(document);
        if (root == nullptr) {
            SideCollectionsAccess::hold_error(
                *box->collections, path + ": the table could not be read"
            );
            return -1;
        }
        const bool create = !names.empty();
        tdf::Block* section = ensure_canbuild(document, root, create, reason);
        if (create && section == nullptr) {
            SideCollectionsAccess::hold_error(*box->collections, std::move(reason));
            return -1;
        }
        tdf::Block* listed = section != nullptr
                                 ? ensure_builder(document, section, builder, create, reason)
                                 : nullptr;
        if (create && listed == nullptr) {
            SideCollectionsAccess::hold_error(*box->collections, std::move(reason));
            return -1;
        }
        if (listed != nullptr &&
            !write_list(document, listed, names, path, *box->collections, reason)) {
            SideCollectionsAccess::hold_error(*box->collections, std::move(reason));
            return -1;
        }
        return 0;
    });
}

int add_canbuild(lua_State* state) {
    Box* box = box_at(state, 1, BoxKind::sides);
    if (box == nullptr) {
        lua_pushliteral(state, "expected the sides collection");
        return lua_error(state);
    }
    return finish(state, *box->collections, [&]() -> int {
        std::string reason;
        if (!layer_ok(
                *box->collections,
                SideCollectionsAccess::grant(*box->collections).sides,
                "sides",
                reason
            )) {
            SideCollectionsAccess::hold_error(*box->collections, std::move(reason));
            return -1;
        }
        bool builder_ok = false;
        bool unit_ok = false;
        std::string builder = lua_string(state, 2, builder_ok);
        std::string unit = lua_string(state, 3, unit_ok);
        if (!builder_ok || !unit_ok) {
            SideCollectionsAccess::hold_error(*box->collections, "a build list name is text");
            return -1;
        }
        std::vector<std::string> raw_names{unit};
        std::vector<std::string> names;
        if (!unit_names(*box, raw_names, true, builder, names, reason)) {
            SideCollectionsAccess::hold_error(*box->collections, std::move(reason));
            return -1;
        }
        std::size_t doc = k_no_doc;
        std::string path;
        if (!load_list_document(*box, doc, path, reason)) {
            SideCollectionsAccess::hold_error(*box->collections, std::move(reason));
            return -1;
        }
        rules::LoadedDocument& loaded = SideCollectionsAccess::documents(*box->collections).at(doc);
        tdf::Document* document = loaded.document.get();
        tdf::Block* root = tdf::editable_root(document);
        if (root == nullptr) {
            SideCollectionsAccess::hold_error(
                *box->collections, path + ": the table could not be read"
            );
            return -1;
        }
        tdf::Block* section = ensure_canbuild(document, root, true, reason);
        if (section == nullptr) {
            SideCollectionsAccess::hold_error(*box->collections, std::move(reason));
            return -1;
        }
        tdf::Block* existing = tdf::editable_child(section, builder);
        std::vector<std::string> listed = read_list(existing);
        for (const std::string& already : listed) {
            if (same_text(already, names[0]))
                return 0;
        }
        listed.push_back(names[0]);
        if (listed.size() > k_list_cap) {
            SideCollectionsAccess::hold_error(
                *box->collections, "a build list keeps at most 1024 names"
            );
            return -1;
        }
        tdf::Block* block = ensure_builder(document, section, builder, true, reason);
        if (block == nullptr) {
            SideCollectionsAccess::hold_error(*box->collections, std::move(reason));
            return -1;
        }
        if (!write_list(document, block, listed, path, *box->collections, reason)) {
            SideCollectionsAccess::hold_error(*box->collections, std::move(reason));
            return -1;
        }
        return 0;
    });
}

int remove_canbuild(lua_State* state) {
    Box* box = box_at(state, 1, BoxKind::sides);
    if (box == nullptr) {
        lua_pushliteral(state, "expected the sides collection");
        return lua_error(state);
    }
    return finish(state, *box->collections, [&]() -> int {
        std::string reason;
        if (!layer_ok(
                *box->collections,
                SideCollectionsAccess::grant(*box->collections).sides,
                "sides",
                reason
            )) {
            SideCollectionsAccess::hold_error(*box->collections, std::move(reason));
            return -1;
        }
        const bool one_list = lua_type(state, 3) != LUA_TNONE && lua_type(state, 3) != LUA_TNIL;
        bool first_ok = false;
        bool second_ok = false;
        std::string first = lua_string(state, 2, first_ok);
        std::string second = one_list ? lua_string(state, 3, second_ok) : std::string();
        if (!first_ok || (one_list && !second_ok)) {
            SideCollectionsAccess::hold_error(*box->collections, "a build list name is text");
            return -1;
        }
        std::string builder = one_list ? first : std::string();
        std::string unit = one_list ? second : first;
        std::vector<std::string> raw_names{unit};
        std::vector<std::string> names;
        if (one_list) {
            if (!unit_names(*box, raw_names, true, builder, names, reason)) {
                SideCollectionsAccess::hold_error(*box->collections, std::move(reason));
                return -1;
            }
        } else {
            const UnitRef found = find_unit(*box->collections, *box->layout, unit);
            if (!found.found) {
                reason = "there is no unit ";
                reason.append(unit);
                SideCollectionsAccess::hold_error(*box->collections, std::move(reason));
                return -1;
            }
            names.push_back(found.name);
        }
        std::size_t doc = k_no_doc;
        std::string path;
        if (!load_list_document(*box, doc, path, reason)) {
            SideCollectionsAccess::hold_error(*box->collections, std::move(reason));
            return -1;
        }
        rules::LoadedDocument& loaded = SideCollectionsAccess::documents(*box->collections).at(doc);
        tdf::Document* document = loaded.document.get();
        tdf::Block* root = tdf::editable_root(document);
        tdf::Block* section = root != nullptr ? tdf::editable_child(root, "CANBUILD") : nullptr;
        if (section == nullptr)
            return 0;
        const uint32_t children = tdf::child_count(section);
        for (uint32_t child_index = 0; child_index < children; ++child_index) {
            tdf::Block* listed = tdf::editable_child(section, child_index);
            if (listed == nullptr || listed->name == nullptr)
                continue;
            if (one_list && !same_text(listed->name, builder))
                continue;
            std::vector<std::string> kept = read_list(listed);
            const std::size_t before = kept.size();
            kept.erase(
                std::remove_if(
                    kept.begin(),
                    kept.end(),
                    [&](const std::string& entry) { return same_text(entry, names[0]); }
                ),
                kept.end()
            );
            if (kept.size() == before)
                continue;
            if (!write_list(document, listed, kept, path, *box->collections, reason)) {
                SideCollectionsAccess::hold_error(*box->collections, std::move(reason));
                return -1;
            }
        }
        return 0;
    });
}

int sides_index(lua_State* state) {
    Box* box = box_at(state, 1, BoxKind::sides);
    if (box == nullptr) {
        lua_pushliteral(state, "expected the sides collection");
        return lua_error(state);
    }
    return finish(state, *box->collections, [&]() -> int {
        if (lua_isinteger(state, 2)) {
            const lua_Integer slot = lua_tointeger(state, 2);
            std::string error;
            const uint32_t count = count_sides(*box, error);
            if (!error.empty()) {
                SideCollectionsAccess::hold_error(*box->collections, std::move(error));
                return -1;
            }
            if (slot < 1 || static_cast<uint64_t>(slot) > count) {
                lua_pushnil(state);
                return 1;
            }
            push_side(state, *box, static_cast<uint32_t>(slot - 1));
            return 1;
        }
        bool ok = false;
        const std::string key = lua_string(state, 2, ok);
        if (!ok) {
            lua_pushnil(state);
            return 1;
        }
        if (same_text(key, "each")) {
            lua_pushcfunction(state, [](lua_State* call) -> int {
                Box* owner = box_at(call, 1, BoxKind::sides);
                if (owner == nullptr) {
                    lua_pushliteral(call, "expected the sides collection");
                    return lua_error(call);
                }
                if (!lua_isfunction(call, 2)) {
                    SideCollectionsAccess::hold_error(
                        *owner->collections, "each expects a function"
                    );
                    return SideCollectionsAccess::raise(*owner->collections, call);
                }
                uint32_t count = 0;
                bool failed = false;
                try {
                    std::string error;
                    count = count_sides(*owner, error);
                    if (!error.empty()) {
                        SideCollectionsAccess::hold_error(*owner->collections, std::move(error));
                        failed = true;
                    }
                } catch (const std::bad_alloc&) {
                    SideCollectionsAccess::hold_error(*owner->collections, "out of memory");
                    failed = true;
                } catch (const std::exception& ex) {
                    const char* what = ex.what();
                    SideCollectionsAccess::hold_error(
                        *owner->collections,
                        what != nullptr && what[0] != '\0' ? what : "the hook stopped"
                    );
                    failed = true;
                }
                if (failed)
                    return SideCollectionsAccess::raise(*owner->collections, call);
                for (uint32_t index = 0; index < count; ++index) {
                    lua_pushvalue(call, 2);
                    push_side(call, *owner, index);
                    lua_call(call, 1, 0);
                }
                return 0;
            });
            return 1;
        }
        if (same_text(key, "canbuild")) {
            lua_pushcfunction(state, read_canbuild);
            return 1;
        }
        if (same_text(key, "set_canbuild")) {
            lua_pushcfunction(state, set_canbuild);
            return 1;
        }
        if (same_text(key, "add_canbuild")) {
            lua_pushcfunction(state, add_canbuild);
            return 1;
        }
        if (same_text(key, "remove_canbuild")) {
            lua_pushcfunction(state, remove_canbuild);
            return 1;
        }
        std::string error;
        const std::optional<uint32_t> slot = side_named(*box, key, error);
        if (!error.empty()) {
            SideCollectionsAccess::hold_error(*box->collections, std::move(error));
            return -1;
        }
        if (!slot.has_value()) {
            lua_pushnil(state);
            return 1;
        }
        push_side(state, *box, *slot);
        return 1;
    });
}

int sides_newindex(lua_State* state) {
    Box* box = box_at(state, 1, BoxKind::sides);
    if (box == nullptr) {
        lua_pushliteral(state, "expected the sides collection");
        return lua_error(state);
    }
    return finish(state, *box->collections, [&]() -> int {
        const bool clearing = lua_type(state, 3) == LUA_TNIL;
        bool known = false;
        if (lua_isinteger(state, 2)) {
            const lua_Integer slot = lua_tointeger(state, 2);
            std::string error;
            const uint32_t count = count_sides(*box, error);
            if (!error.empty()) {
                SideCollectionsAccess::hold_error(*box->collections, std::move(error));
                return -1;
            }
            known = slot >= 1 && static_cast<uint64_t>(slot) <= count;
        } else {
            bool ok = false;
            const std::string key = lua_string(state, 2, ok);
            if (ok && (same_text(key, "each") || same_text(key, "canbuild") ||
                       same_text(key, "set_canbuild") || same_text(key, "add_canbuild") ||
                       same_text(key, "remove_canbuild"))) {
                SideCollectionsAccess::hold_error(*box->collections, "that name is a method");
                return -1;
            }
            if (ok) {
                std::string error;
                known = side_named(*box, key, error).has_value();
                if (!error.empty()) {
                    SideCollectionsAccess::hold_error(*box->collections, std::move(error));
                    return -1;
                }
            }
        }
        SideCollectionsAccess::hold_error(
            *box->collections,
            known && clearing ? "a mutator cannot remove a side" : "a mutator cannot add a side"
        );
        return -1;
    });
}

int side_index(lua_State* state) {
    Box* box = box_at(state, 1, BoxKind::side);
    if (box == nullptr) {
        lua_pushliteral(state, "expected a side");
        return lua_error(state);
    }
    return finish(state, *box->collections, [&]() -> int {
        bool ok = false;
        const std::string key = lua_string(state, 2, ok);
        if (!ok) {
            lua_pushnil(state);
            return 1;
        }
        std::string error;
        std::size_t doc = k_no_doc;
        tdf::Block* block = nullptr;
        std::string path;
        if (!load_side(*box, box->side_slot, doc, block, path, error)) {
            SideCollectionsAccess::hold_error(*box->collections, std::move(error));
            return -1;
        }
        if (child_named(block, key)) {
            lua_pushnil(state);
            return 1;
        }
        const FieldText current = field_text(block, key);
        if (!current.present) {
            lua_pushnil(state);
            return 1;
        }
        lua_pushlstring(state, current.text.data(), current.text.size());
        return 1;
    });
}

int side_newindex(lua_State* state) {
    Box* box = box_at(state, 1, BoxKind::side);
    if (box == nullptr) {
        lua_pushliteral(state, "expected a side");
        return lua_error(state);
    }
    return finish(state, *box->collections, [&]() -> int {
        bool ok = false;
        const std::string key = lua_string(state, 2, ok);
        if (!ok) {
            SideCollectionsAccess::hold_error(*box->collections, "a side field name is text");
            return -1;
        }
        if (lua_type(state, 3) != LUA_TSTRING) {
            SideCollectionsAccess::hold_error(*box->collections, "a side value is text");
            return -1;
        }
        std::string reason;
        if (!layer_ok(
                *box->collections,
                SideCollectionsAccess::grant(*box->collections).sides,
                "sides",
                reason
            )) {
            SideCollectionsAccess::hold_error(*box->collections, std::move(reason));
            return -1;
        }
        const std::string raw = lua_string(state, 3, ok);
        std::size_t doc = k_no_doc;
        tdf::Block* block = nullptr;
        std::string path;
        if (!load_side(*box, box->side_slot, doc, block, path, reason)) {
            SideCollectionsAccess::hold_error(*box->collections, std::move(reason));
            return -1;
        }
        if (child_named(block, key)) {
            SideCollectionsAccess::hold_error(*box->collections, "a side's panels are not changed");
            return -1;
        }
        std::string stored;
        if (!checked_text(*box, box->side_slot, block, key, raw, stored, reason)) {
            SideCollectionsAccess::hold_error(*box->collections, std::move(reason));
            return -1;
        }
        rules::LoadedDocument& loaded = SideCollectionsAccess::documents(*box->collections).at(doc);
        const std::string record = block->name != nullptr ? block->name : std::string();
        if (!put_text(
                loaded.document.get(), block, path, record, key, stored, *box->collections, reason
            )) {
            SideCollectionsAccess::hold_error(*box->collections, std::move(reason));
            return -1;
        }
        return 0;
    });
}

void push_section(
    lua_State* state, const Box& source, std::size_t doc, tdf::Block* parent, tdf::Block* block
) {
    Box copy = source;
    copy.kind = BoxKind::section;
    copy.doc_index = doc;
    copy.parent = parent;
    copy.block = block;
    push_box(state, copy, k_section_meta);
}

int push_child_names(lua_State* state, const tdf::Block* parent) {
    const uint32_t count = parent != nullptr ? tdf::child_count(parent) : 0;
    lua_createtable(state, static_cast<int>(count), 0);
    for (uint32_t index = 0; index < count; ++index) {
        const tdf::Block* child = tdf::child_at(parent, index);
        const char* name = child != nullptr && child->name != nullptr ? child->name : "";
        lua_pushstring(state, name);
        lua_rawseti(state, -2, static_cast<lua_Integer>(index + 1));
    }
    return 1;
}

int section_keys(lua_State* state) {
    Box* box = box_at(state, 1, BoxKind::section);
    if (box == nullptr) {
        lua_pushliteral(state, "expected a section");
        return lua_error(state);
    }
    return finish(state, *box->collections, [&]() -> int {
        std::string reason;
        if (!section_live(*box, reason)) {
            SideCollectionsAccess::hold_error(*box->collections, std::move(reason));
            return -1;
        }
        const tdf::Block* block = box->block;
        const uint32_t count = tdf::property_count(block);
        std::vector<uint32_t> places(count);
        for (uint32_t place = 0; place < count; ++place)
            places[place] = place;
        std::stable_sort(
            places.begin(), places.end(), [&](uint32_t first_place, uint32_t second_place) {
                return block->properties[first_place].order < block->properties[second_place].order;
            }
        );
        lua_createtable(state, static_cast<int>(count), 0);
        for (uint32_t index = 0; index < count; ++index) {
            const char* key = block->properties[places[index]].key;
            lua_pushstring(state, key != nullptr ? key : "");
            lua_rawseti(state, -2, static_cast<lua_Integer>(index + 1));
        }
        return 1;
    });
}

int add_section(lua_State* state) {
    Box* box = box_at(state, 1, BoxKind::section);
    const bool on_section = box != nullptr;
    if (!on_section)
        box = box_at(state, 1, BoxKind::table);
    if (box == nullptr) {
        lua_pushliteral(state, "expected a table");
        return lua_error(state);
    }
    return finish(state, *box->collections, [&]() -> int {
        std::string reason;
        if (!layer_ok(
                *box->collections,
                SideCollectionsAccess::grant(*box->collections).tables,
                "tables",
                reason
            )) {
            SideCollectionsAccess::hold_error(*box->collections, std::move(reason));
            return -1;
        }
        if (on_section && !section_live(*box, reason)) {
            SideCollectionsAccess::hold_error(*box->collections, std::move(reason));
            return -1;
        }
        bool ok = false;
        const std::string raw = lua_string(state, 2, ok);
        if (!ok) {
            SideCollectionsAccess::hold_error(*box->collections, "a section name is text");
            return -1;
        }
        if (cutting_character(raw, reason)) {
            SideCollectionsAccess::hold_error(*box->collections, std::move(reason));
            return -1;
        }
        const std::string_view trimmed = trim_ends(raw);
        if (trimmed.empty()) {
            SideCollectionsAccess::hold_error(*box->collections, "a section name is empty");
            return -1;
        }
        const std::string name(trimmed);
        rules::LoadedDocument& loaded =
            SideCollectionsAccess::documents(*box->collections).at(box->doc_index);
        tdf::Document* document = loaded.document.get();
        tdf::Block* parent = on_section ? box->block : tdf::editable_root(document);
        if (parent == nullptr) {
            SideCollectionsAccess::hold_error(*box->collections, "that section was removed");
            return -1;
        }
        tdf::EditStatus status = tdf::EditStatus::ok;
        tdf::Block* created = tdf::add_child(document, parent, name, &status);
        if (created == nullptr) {
            SideCollectionsAccess::hold_error(*box->collections, tdf::edit_status_message(status));
            return -1;
        }
        const std::string record = parent_record(*box, parent);
        const std::string stored = created->name != nullptr ? created->name : name;
        if (!commit_field(
                *box->collections, loaded.path, record, stored, std::nullopt, stored, reason
            )) {
            SideCollectionsAccess::hold_error(*box->collections, std::move(reason));
            return -1;
        }
        return 0;
    });
}

int remove_section(lua_State* state) {
    Box* box = box_at(state, 1, BoxKind::section);
    const bool on_section = box != nullptr;
    if (!on_section)
        box = box_at(state, 1, BoxKind::table);
    if (box == nullptr) {
        lua_pushliteral(state, "expected a table");
        return lua_error(state);
    }
    return finish(state, *box->collections, [&]() -> int {
        std::string reason;
        if (!layer_ok(
                *box->collections,
                SideCollectionsAccess::grant(*box->collections).tables,
                "tables",
                reason
            )) {
            SideCollectionsAccess::hold_error(*box->collections, std::move(reason));
            return -1;
        }
        if (on_section && !section_live(*box, reason)) {
            SideCollectionsAccess::hold_error(*box->collections, std::move(reason));
            return -1;
        }
        bool ok = false;
        const std::string name = lua_string(state, 2, ok);
        if (!ok) {
            SideCollectionsAccess::hold_error(*box->collections, "a section name is text");
            return -1;
        }
        rules::LoadedDocument& loaded =
            SideCollectionsAccess::documents(*box->collections).at(box->doc_index);
        tdf::Document* document = loaded.document.get();
        tdf::Block* parent = on_section ? box->block : tdf::editable_root(document);
        if (parent == nullptr) {
            SideCollectionsAccess::hold_error(*box->collections, "that section was removed");
            return -1;
        }
        tdf::Block* child = tdf::editable_child(parent, name);
        if (child == nullptr)
            return 0;
        uint32_t index = 0;
        const uint32_t count = tdf::child_count(parent);
        while (index < count && tdf::editable_child(parent, index) != child)
            ++index;
        if (index >= count) {
            SideCollectionsAccess::hold_error(*box->collections, "that section was removed");
            return -1;
        }
        const std::string record = parent_record(*box, parent);
        const std::string stored = child->name != nullptr ? child->name : name;
        const tdf::EditStatus status = tdf::remove_child(document, parent, index);
        if (status != tdf::EditStatus::ok) {
            SideCollectionsAccess::hold_error(*box->collections, tdf::edit_status_message(status));
            return -1;
        }
        if (!commit_field(
                *box->collections, loaded.path, record, stored, stored, std::nullopt, reason
            )) {
            SideCollectionsAccess::hold_error(*box->collections, std::move(reason));
            return -1;
        }
        return 0;
    });
}

int section_sections(lua_State* state) {
    Box* box = box_at(state, 1, BoxKind::section);
    if (box == nullptr) {
        lua_pushliteral(state, "expected a section");
        return lua_error(state);
    }
    return finish(state, *box->collections, [&]() -> int {
        std::string reason;
        if (!section_live(*box, reason)) {
            SideCollectionsAccess::hold_error(*box->collections, std::move(reason));
            return -1;
        }
        return push_child_names(state, box->block);
    });
}

int section_index(lua_State* state) {
    Box* box = box_at(state, 1, BoxKind::section);
    if (box == nullptr) {
        lua_pushliteral(state, "expected a section");
        return lua_error(state);
    }
    return finish(state, *box->collections, [&]() -> int {
        bool ok = false;
        const std::string key = lua_string(state, 2, ok);
        if (!ok) {
            lua_pushnil(state);
            return 1;
        }
        std::string reason;
        if (!section_live(*box, reason)) {
            SideCollectionsAccess::hold_error(*box->collections, std::move(reason));
            return -1;
        }
        if (same_text(key, "keys")) {
            lua_pushcfunction(state, section_keys);
            return 1;
        }
        if (same_text(key, "sections")) {
            lua_pushcfunction(state, section_sections);
            return 1;
        }
        if (same_text(key, "add_section")) {
            lua_pushcfunction(state, add_section);
            return 1;
        }
        if (same_text(key, "remove_section")) {
            lua_pushcfunction(state, remove_section);
            return 1;
        }
        tdf::Block* child = tdf::editable_child(box->block, key);
        if (child != nullptr) {
            push_section(state, *box, box->doc_index, box->block, child);
            return 1;
        }
        const FieldText current = field_text(box->block, key);
        if (!current.present) {
            lua_pushnil(state);
            return 1;
        }
        lua_pushlstring(state, current.text.data(), current.text.size());
        return 1;
    });
}

int section_newindex(lua_State* state) {
    Box* box = box_at(state, 1, BoxKind::section);
    if (box == nullptr) {
        lua_pushliteral(state, "expected a section");
        return lua_error(state);
    }
    return finish(state, *box->collections, [&]() -> int {
        bool ok = false;
        const std::string key = lua_string(state, 2, ok);
        if (!ok) {
            SideCollectionsAccess::hold_error(*box->collections, "a table key is text");
            return -1;
        }
        std::string reason;
        if (!layer_ok(
                *box->collections,
                SideCollectionsAccess::grant(*box->collections).tables,
                "tables",
                reason
            )) {
            SideCollectionsAccess::hold_error(*box->collections, std::move(reason));
            return -1;
        }
        if (!section_live(*box, reason)) {
            SideCollectionsAccess::hold_error(*box->collections, std::move(reason));
            return -1;
        }
        if (same_text(key, "keys") || same_text(key, "sections") || same_text(key, "add_section") ||
            same_text(key, "remove_section")) {
            SideCollectionsAccess::hold_error(*box->collections, "that name is a method");
            return -1;
        }
        if (child_named(box->block, key)) {
            SideCollectionsAccess::hold_error(*box->collections, "a section is not assigned");
            return -1;
        }
        if (lua_type(state, 3) == LUA_TNIL) {
            SideCollectionsAccess::hold_error(*box->collections, "nil does not remove a table key");
            return -1;
        }
        if (lua_type(state, 3) != LUA_TSTRING) {
            SideCollectionsAccess::hold_error(*box->collections, "a table value is text");
            return -1;
        }
        const std::string raw = lua_string(state, 3, ok);
        if (cutting_character(raw, reason)) {
            SideCollectionsAccess::hold_error(*box->collections, std::move(reason));
            return -1;
        }
        const std::string_view trimmed = trim_ends(raw);
        if (trimmed.size() > k_text_limit) {
            SideCollectionsAccess::hold_error(
                *box->collections, "a value is longer than the field can hold"
            );
            return -1;
        }
        const std::string stored(trimmed);
        rules::LoadedDocument& loaded =
            SideCollectionsAccess::documents(*box->collections).at(box->doc_index);
        const std::string record = box->block->name != nullptr ? box->block->name : std::string();
        if (!put_text(
                loaded.document.get(),
                box->block,
                loaded.path,
                record,
                key,
                stored,
                *box->collections,
                reason
            )) {
            SideCollectionsAccess::hold_error(*box->collections, std::move(reason));
            return -1;
        }
        return 0;
    });
}

int table_sections(lua_State* state) {
    Box* box = box_at(state, 1, BoxKind::table);
    if (box == nullptr) {
        lua_pushliteral(state, "expected a table");
        return lua_error(state);
    }
    return finish(state, *box->collections, [&]() -> int {
        if (box->doc_index >= SideCollectionsAccess::documents(*box->collections).size()) {
            SideCollectionsAccess::hold_error(*box->collections, "that section was removed");
            return -1;
        }
        return push_child_names(state, root_of(*box));
    });
}

int table_index(lua_State* state) {
    Box* box = box_at(state, 1, BoxKind::table);
    if (box == nullptr) {
        lua_pushliteral(state, "expected a table");
        return lua_error(state);
    }
    return finish(state, *box->collections, [&]() -> int {
        bool ok = false;
        const std::string key = lua_string(state, 2, ok);
        if (!ok) {
            lua_pushnil(state);
            return 1;
        }
        if (same_text(key, "sections")) {
            lua_pushcfunction(state, table_sections);
            return 1;
        }
        if (same_text(key, "add_section")) {
            lua_pushcfunction(state, add_section);
            return 1;
        }
        if (same_text(key, "remove_section")) {
            lua_pushcfunction(state, remove_section);
            return 1;
        }
        tdf::Block* root = root_of(*box);
        tdf::Block* child = root != nullptr ? tdf::editable_child(root, key) : nullptr;
        if (child == nullptr) {
            lua_pushnil(state);
            return 1;
        }
        push_section(state, *box, box->doc_index, root, child);
        return 1;
    });
}

int table_newindex(lua_State* state) {
    Box* box = box_at(state, 1, BoxKind::table);
    if (box == nullptr) {
        lua_pushliteral(state, "expected a table");
        return lua_error(state);
    }
    return finish(state, *box->collections, [&]() -> int {
        bool ok = false;
        const std::string key = lua_string(state, 2, ok);
        if (ok && (same_text(key, "sections") || same_text(key, "add_section") ||
                   same_text(key, "remove_section"))) {
            SideCollectionsAccess::hold_error(*box->collections, "that name is a method");
            return -1;
        }
        SideCollectionsAccess::hold_error(*box->collections, "a section is not assigned");
        return -1;
    });
}

int open_table(lua_State* state, const Box& box, const std::string& requested, std::string& error) {
    const std::string path = canonical_table_path(*box.layout, requested);
    if (sidedata_path(path)) {
        error = "use ctx.sides";
        return -1;
    }
    PathContext context;
    context.layout = box.layout;
    const PathVerdict verdict = check_mutable_path(path, context);
    if (!verdict.allowed()) {
        error = verdict.message;
        return -1;
    }
    const std::size_t doc = document_index(*box.collections, *box.store, path, error, false);
    if (!error.empty())
        return -1;
    if (doc == k_no_doc) {
        lua_pushnil(state);
        return 1;
    }
    Box copy = box;
    copy.kind = BoxKind::table;
    copy.doc_index = doc;
    copy.block = nullptr;
    copy.parent = nullptr;
    push_box(state, copy, k_table_meta);
    return 1;
}

int tables_index(lua_State* state) {
    Box* box = box_at(state, 1, BoxKind::tables);
    if (box == nullptr) {
        lua_pushliteral(state, "expected the tables collection");
        return lua_error(state);
    }
    return finish(state, *box->collections, [&]() -> int {
        bool ok = false;
        const std::string name = lua_string(state, 2, ok);
        if (!ok) {
            lua_pushnil(state);
            return 1;
        }
        std::string error;
        const int opened = open_table(state, *box, name, error);
        if (opened < 0) {
            SideCollectionsAccess::hold_error(*box->collections, std::move(error));
            return -1;
        }
        return opened;
    });
}

int tables_newindex(lua_State* state) {
    Box* box = box_at(state, 1, BoxKind::tables);
    if (box == nullptr) {
        lua_pushliteral(state, "expected the tables collection");
        return lua_error(state);
    }
    SideCollectionsAccess::hold_error(*box->collections, "a table is opened by name, not assigned");
    return SideCollectionsAccess::raise(*box->collections, state);
}

void ensure_meta(
    lua_State* state,
    const char* name,
    lua_CFunction index,
    lua_CFunction newindex,
    lua_CFunction length,
    lua_CFunction pairs
) {
    if (luaL_newmetatable(state, name) == 0) {
        lua_pop(state, 1);
        return;
    }
    lua_pushcfunction(state, index);
    lua_setfield(state, -2, "__index");
    if (newindex != nullptr) {
        lua_pushcfunction(state, newindex);
        lua_setfield(state, -2, "__newindex");
    }
    if (length != nullptr) {
        lua_pushcfunction(state, length);
        lua_setfield(state, -2, "__len");
    }
    if (pairs != nullptr) {
        lua_pushcfunction(state, pairs);
        lua_setfield(state, -2, "__pairs");
    }
    lua_pushboolean(state, 0);
    lua_setfield(state, -2, "__metatable");
    lua_pop(state, 1);
}

} // namespace

void set_context_sides(
    lua_State* state,
    int ctx_index,
    Collections& collections,
    const oa::AssetStore& store,
    const defs::DataLayout& layout,
    const std::string& variant
) {
    ensure_meta(state, k_sides_meta, sides_index, sides_newindex, sides_len, sides_pairs);
    ensure_meta(state, k_side_meta, side_index, side_newindex, nullptr, nullptr);
    ensure_meta(state, k_tables_meta, tables_index, tables_newindex, nullptr, nullptr);
    ensure_meta(state, k_table_meta, table_index, table_newindex, nullptr, nullptr);
    ensure_meta(state, k_section_meta, section_index, section_newindex, nullptr, nullptr);
    const int ctx = lua_absindex(state, ctx_index);
    Box box;
    box.collections = &collections;
    box.store = &store;
    box.layout = &layout;
    box.variant = &variant;
    box.kind = BoxKind::sides;
    push_box(state, box, k_sides_meta);
    lua_setfield(state, ctx, "sides");
    box.kind = BoxKind::tables;
    push_box(state, box, k_tables_meta);
    lua_setfield(state, ctx, "tables");
}

} // namespace oa::data::mutators
