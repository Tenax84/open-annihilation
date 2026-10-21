// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Build menus a mutator hook reads and writes.
//
// Slots are decided as each request is made, so a later request cannot move
// an earlier one. Download files and authored pages are written once, from
// resolve, after every mutator's hooks have committed. A unit stays
// downloadable exactly when it was when this object was built, except a unit
// a hook removed and a unit whose every download entry was removed.
#include "oa/data/mutators/menus.hpp"

#include "oa/data/defs/unit_catalog.hpp"
#include "oa/data/limits.hpp"
#include "oa/data/script_host/script_host.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/formats/tdf/edit.hpp"
#include "oa/formats/tdf/write.hpp"

#include "lua.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::data::mutators {
namespace rules = oa::data::mutation_rules;
namespace tdf = oa::formats::tdf;
namespace defs = oa::data::defs;
namespace host = oa::data::script_host;
namespace limits = oa::data::limits;

namespace {

/// The loader reads this many sections of a download file. One more is refused.
constexpr std::size_t k_visible_sections = 5;

/// Player pages the build panel can show. Page 0 is the command page.
constexpr int k_max_page = 7;

/// Slots on a download page, counted from 1.
constexpr int k_max_slot = 6;

/// Authored pages probed before "new" gives up. Pages past this still refuse.
constexpr int k_page_probe_cap = 32;

/// How many repair steps one resolve may take before the layout is refused.
constexpr int k_repair_steps = 10000;

/// Bit of commonattribs that marks a unit button. The validity pass clears
/// grayedout on every button that still has it.
constexpr unsigned k_unit_button_bit = 0x04u;

const char* k_meta_name = "oa.mutators.menus";

/// A menus request the script cannot make.
struct MenuError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(std::string message) {
    throw MenuError(std::move(message));
}

char fold_byte(char byte) {
    if (byte >= 'A' && byte <= 'Z')
        return static_cast<char>(byte - 'A' + 'a');
    return byte;
}

std::string fold_text(std::string_view text) {
    std::string folded;
    folded.resize(text.size());
    for (std::size_t index = 0; index < text.size(); ++index)
        folded[index] = fold_byte(text[index]);
    return folded;
}

bool same_text(std::string_view first, std::string_view second) {
    if (first.size() != second.size())
        return false;
    for (std::size_t index = 0; index < first.size(); ++index) {
        if (fold_byte(first[index]) != fold_byte(second[index]))
            return false;
    }
    return true;
}

bool same_path(std::string_view first, std::string_view second) {
    if (first.size() != second.size())
        return false;
    for (std::size_t index = 0; index < first.size(); ++index) {
        char one = first[index] == '\\' ? '/' : first[index];
        char other = second[index] == '\\' ? '/' : second[index];
        if (fold_byte(one) != fold_byte(other))
            return false;
    }
    return true;
}

std::string directory_of(const defs::DataLayout& layout, defs::DataDirectory which) {
    return layout.directories[static_cast<std::size_t>(which)];
}

/// One field of a download section, in the order the file writes it.
struct KeySnap {
    std::string key;
    std::string value;
};

void put_key(std::vector<KeySnap>& keys, std::string name, std::string value) {
    for (KeySnap& snap : keys) {
        if (!same_text(snap.key, name))
            continue;
        snap.value = std::move(value);
        return;
    }
    keys.push_back(KeySnap{std::move(name), std::move(value)});
}

const std::string* find_snap(const std::vector<KeySnap>& keys, std::string_view name) {
    for (const KeySnap& snap : keys) {
        if (same_text(snap.key, name))
            return &snap.value;
    }
    return nullptr;
}

/// One button on an authored page. Its identity is its place in the file.
struct Button {
    uint32_t gadget_index{};
    std::string name;
    std::string folded;
    bool unit_bit{false};
    bool was_unit_button{false};
    bool greyed{false};
    bool hidden{false};
    std::string grayed_text;
    bool has_grayed{false};
    std::string attribs_text;
    bool has_attribs{false};
    std::string active_text;
    bool has_active{false};
    bool dirty{false};
    std::optional<std::string> new_grayed;
    std::optional<std::string> new_attribs;
    std::optional<std::string> new_active;
};

/// One authored page. Page 0 is probed and does not take build slots.
struct Page {
    std::string path;
    int page{0};
    bool authored{false};
    bool parsed{false};
    bool dirty{false};
    std::vector<Button> buttons;
};

/// One download section. Sections past the fifth are kept and are not shown.
struct Entry {
    std::string section;
    std::string unit_menu;
    std::string unit_name;
    std::string menu_text;
    std::string button_text;
    int menu{0};
    int button{0};
    bool placeholder{false};
    bool alive{true};
    bool from_file{false};
    bool externally_removed{false};
    std::string placer;
    std::vector<KeySnap> keys;
};

/// One download file, in the order the store listed it. Created files follow.
struct DownloadFile {
    std::string path;
    std::vector<Entry> sections;
    bool created{false};
    bool dirty{false};
    bool hide{false};
    std::string owner_unit;
    std::string actor;
    std::size_t doc_index{std::numeric_limits<std::size_t>::max()};
};

struct UnitInfo {
    std::string canonical;
    std::string folded;
    std::size_t doc_index{};
    bool builder{false};
    std::vector<Page> pages;
};

struct Place {
    int page{1};
    int slot{1};
};

struct Request {
    bool fresh_page{false};
    int page{1};
    int slot{1};
};

struct Tally {
    std::string name;
    uint32_t writes{0};
    uint32_t notes{0};
};

std::optional<std::string> read_store(const oa::AssetStore& store, std::string_view path) {
    try {
        const oa::AssetData data = store.read(path);
        return std::string(data.bytes.begin(), data.bytes.end());
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

bool art_present(const oa::AssetStore& store, std::string_view unit) {
    std::string path = "anims/";
    path.append(unit.data(), unit.size());
    path += "_gadget.gaf";
    // An empty file is still a picture the button can name. A missing file is not.
    return read_store(store, path).has_value();
}

uint8_t low_byte(std::string_view text) {
    const int32_t parsed = tdf::parse_int(std::string{text}.c_str());
    return static_cast<uint8_t>(static_cast<uint32_t>(parsed) & 0xffu);
}

std::string cleared_attribs(std::string_view text) {
    const uint8_t low = low_byte(text);
    const auto cleared = static_cast<unsigned>(static_cast<uint8_t>(low & ~k_unit_button_bit));
    return std::to_string(cleared);
}

Entry read_entry(const tdf::Block* block) {
    Entry entry;
    entry.section = block != nullptr && block->name != nullptr ? block->name : "";
    entry.from_file = true;
    entry.alive = true;
    if (block == nullptr)
        return entry;
    std::vector<uint32_t> order;
    order.reserve(block->property_count);
    for (uint32_t index = 0; index < block->property_count; ++index)
        order.push_back(index);
    std::sort(order.begin(), order.end(), [&](uint32_t first, uint32_t second) {
        return block->properties[first].order < block->properties[second].order;
    });
    for (const uint32_t index : order) {
        const tdf::Property& property = block->properties[index];
        if (property.key == nullptr)
            continue;
        KeySnap snap;
        snap.key = property.key;
        snap.value = property.value != nullptr ? property.value : "";
        entry.keys.push_back(std::move(snap));
    }
    const auto text_of = [&](const char* key) {
        const char* value = tdf::find_value(block, key);
        return value != nullptr ? std::string{value} : std::string{};
    };
    entry.unit_menu = text_of("UNITMENU");
    entry.unit_name = text_of("UNITNAME");
    entry.menu_text = text_of("MENU");
    entry.button_text = text_of("BUTTON");
    entry.menu = tdf::parse_int(entry.menu_text.c_str());
    entry.button = tdf::parse_int(entry.button_text.c_str());
    // An empty UNITMENU is zeroed by the loader: it names no builder and marks nothing.
    entry.placeholder = entry.unit_menu.empty();
    return entry;
}

void sync_entry(Entry& entry) {
    put_key(entry.keys, "UNITMENU", entry.unit_menu);
    put_key(entry.keys, "MENU", entry.menu_text);
    put_key(entry.keys, "BUTTON", entry.button_text);
    put_key(entry.keys, "UNITNAME", entry.unit_name);
}

Entry make_placeholder() {
    Entry entry;
    entry.section = "PLACEHOLDER";
    entry.menu_text = "0";
    entry.button_text = "0";
    entry.placeholder = true;
    entry.alive = true;
    entry.from_file = false;
    // UNITMENU is empty on purpose. load_download_entry leaves the section zeroed
    // when UNITMENU is missing or names no unit, so this marks nobody.
    put_key(entry.keys, "UNITMENU", "");
    put_key(entry.keys, "MENU", "0");
    put_key(entry.keys, "BUTTON", "0");
    put_key(entry.keys, "UNITNAME", "");
    return entry;
}

Entry make_entry(std::string builder, std::string unit, int page, int slot, std::string placer) {
    Entry entry;
    entry.section = "ENTRY";
    entry.unit_menu = std::move(builder);
    entry.unit_name = std::move(unit);
    // The page a player calls p is written MENU=p+1. BUTTON is the slot minus one.
    entry.menu = page + 1;
    entry.button = slot - 1;
    entry.menu_text = std::to_string(entry.menu);
    entry.button_text = std::to_string(entry.button);
    entry.placeholder = false;
    entry.alive = true;
    entry.from_file = false;
    entry.placer = std::move(placer);
    put_key(entry.keys, "UNITMENU", entry.unit_menu);
    put_key(entry.keys, "MENU", entry.menu_text);
    put_key(entry.keys, "BUTTON", entry.button_text);
    put_key(entry.keys, "UNITNAME", entry.unit_name);
    return entry;
}

std::size_t visible_count(const DownloadFile& file) {
    return file.sections.size() < k_visible_sections ? file.sections.size() : k_visible_sections;
}

bool section_matches(const tdf::Block* block, const Entry& entry) {
    if (block == nullptr || block->name == nullptr)
        return false;
    if (!same_text(block->name, entry.section))
        return false;
    if (tdf::property_count(block) != entry.keys.size())
        return false;
    for (const KeySnap& snap : entry.keys) {
        const char* value = tdf::find_value(block, snap.key.c_str());
        if (value == nullptr || snap.value != value)
            return false;
    }
    return true;
}

void require_edit(tdf::EditStatus status) {
    if (status == tdf::EditStatus::ok)
        return;
    fail(std::string{"a build menu could not be written: "} + tdf::edit_status_message(status));
}

} // namespace

struct Menus::LuaBox {
    Menus* owner{};
};

struct Menus::State {
    rules::DocumentSet* documents{};
    const oa::AssetStore* store{};
    const defs::DataLayout* layout{};
    std::vector<UnitInfo> units;
    std::vector<DownloadFile> files;
    std::vector<std::string> marked;
    std::vector<rules::FieldWrite> field_writes;
    std::vector<HookNote> hook_notes;
    std::vector<RecordRemoval> record_removals;
    std::vector<Tally> tallies;
    std::string mutator;
    std::string last_mutator;
    bool menus_layer{false};
    bool active{false};
    std::string failed;
    std::string applying;

    State(
        rules::DocumentSet& document_set,
        const oa::AssetStore& asset_store,
        const defs::DataLayout* used
    )
        : documents(&document_set), store(&asset_store),
          layout(used != nullptr ? used : &defs::data_layout()) {
        load_units();
        load_downloads();
        capture_marks();
    }

    void begin(std::string name, bool layer) {
        mutator = std::move(name);
        last_mutator = mutator;
        menus_layer = layer;
        active = true;
    }

    void end(std::span<const RecordRemoval> removals_this_turn) {
        for (const RecordRemoval& removal : removals_this_turn) {
            if (removal.kind == RemovalKind::download_entry)
                note_external_removal(removal);
        }
        for (const RecordRemoval& removal : removals_this_turn) {
            if (removal.kind != RemovalKind::unit)
                continue;
            drop_named(removal.name, true);
            grey_named(removal.name);
        }
        active = false;
    }

    [[nodiscard]] bool write_back(std::string& error) {
        try {
            repair();
            write_downloads();
            write_pages();
            return true;
        } catch (const std::bad_alloc&) {
            error = "out of memory";
            failed = applying.empty() ? last_mutator : applying;
            return false;
        } catch (const std::exception& ex) {
            error = ex.what() != nullptr && ex.what()[0] != '\0'
                        ? ex.what()
                        : "a build menu could not be written";
            failed = applying.empty() ? last_mutator : applying;
            return false;
        }
    }

    int add_lua(lua_State* state) {
        const int top = lua_gettop(state);
        if (top != 3 && top != 4)
            fail("menus:add expects a builder and a unit");
        const std::string builder_text =
            require_lua_string(state, 2, "menus:add expects a builder and a unit");
        const std::string unit_text =
            require_lua_string(state, 3, "menus:add expects a builder and a unit");
        std::optional<Request> request;
        if (top == 4)
            request = read_request(state, 4, true);
        const Place landed = add_entry(builder_text, unit_text, request);
        push_place(state, landed);
        return 1;
    }

    int move_lua(lua_State* state) {
        if (lua_gettop(state) != 4)
            fail("menus:move expects a builder, a unit and a place");
        const std::string builder_text =
            require_lua_string(state, 2, "menus:move expects a builder, a unit and a place");
        const std::string unit_text =
            require_lua_string(state, 3, "menus:move expects a builder, a unit and a place");
        const Request request = read_request(state, 4, false);
        const Place landed = move_entry(builder_text, unit_text, request);
        push_place(state, landed);
        return 1;
    }

    int remove_lua(lua_State* state) {
        const int top = lua_gettop(state);
        if (top == 2) {
            const std::string unit_text =
                require_lua_string(state, 2, "menus:remove expects a unit");
            remove_entry(std::nullopt, unit_text);
            return 0;
        }
        if (top == 3) {
            const std::string builder_text =
                require_lua_string(state, 2, "menus:remove expects a unit");
            const std::string unit_text =
                require_lua_string(state, 3, "menus:remove expects a unit");
            remove_entry(builder_text, unit_text);
            return 0;
        }
        fail("menus:remove expects a unit");
    }

    int grey_lua(lua_State* state) {
        if (lua_gettop(state) != 3)
            fail("menus:grey expects a builder and a unit");
        const std::string builder_text =
            require_lua_string(state, 2, "menus:grey expects a builder and a unit");
        const std::string unit_text =
            require_lua_string(state, 3, "menus:grey expects a builder and a unit");
        restyle(builder_text, unit_text, false);
        return 0;
    }

    int hide_lua(lua_State* state) {
        if (lua_gettop(state) != 3)
            fail("menus:hide expects a builder and a unit");
        const std::string builder_text =
            require_lua_string(state, 2, "menus:hide expects a builder and a unit");
        const std::string unit_text =
            require_lua_string(state, 3, "menus:hide expects a builder and a unit");
        restyle(builder_text, unit_text, true);
        return 0;
    }

    int builds_lua(lua_State* state) {
        if (lua_gettop(state) != 2)
            fail("menus:builds expects a builder");
        const std::string builder_text =
            require_lua_string(state, 2, "menus:builds expects a builder");
        const std::vector<std::string> names = builds_of(builder_text);
        lua_createtable(state, static_cast<int>(names.size()), 0);
        for (std::size_t index = 0; index < names.size(); ++index) {
            lua_pushlstring(state, names[index].data(), names[index].size());
            lua_rawseti(state, -2, static_cast<lua_Integer>(index + 1));
        }
        return 1;
    }

  private:

    [[nodiscard]] const std::string& actor_name() const { return active ? mutator : last_mutator; }

    [[nodiscard]] bool unit_removed(const UnitInfo& unit) const {
        return documents->at(unit.doc_index).removed;
    }

    [[nodiscard]] UnitInfo* find_unit(std::string_view name) {
        const std::string folded = fold_text(name);
        for (UnitInfo& unit : units) {
            if (unit.folded == folded)
                return &unit;
        }
        return nullptr;
    }

    [[nodiscard]] const UnitInfo* find_unit(std::string_view name) const {
        const std::string folded = fold_text(name);
        for (const UnitInfo& unit : units) {
            if (unit.folded == folded)
                return &unit;
        }
        return nullptr;
    }

    [[nodiscard]] bool was_marked(const UnitInfo& unit) const {
        const std::string folded = unit.folded;
        for (const std::string& name : marked) {
            if (name == folded)
                return true;
        }
        return false;
    }

    [[nodiscard]] bool known_menu(const Entry& entry) const {
        const UnitInfo* menu_unit = find_unit(entry.unit_menu);
        return menu_unit != nullptr && !unit_removed(*menu_unit);
    }

    [[nodiscard]] bool marks_unit(const Entry& entry, const UnitInfo& unit) const {
        return entry.alive && !entry.placeholder && same_text(entry.unit_name, unit.canonical) &&
               known_menu(entry);
    }

    [[nodiscard]] bool marked_ok_name(std::string_view name) const {
        const UnitInfo* unit = find_unit(name);
        if (unit == nullptr || !was_marked(*unit) || unit_removed(*unit))
            return false;
        for (const DownloadFile& file : files) {
            const std::size_t shown = visible_count(file);
            for (std::size_t index = 0; index < shown; ++index) {
                if (marks_unit(file.sections[index], *unit))
                    return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool is_marking_first(const UnitInfo& unit) const {
        for (const DownloadFile& file : files) {
            if (file.sections.empty())
                continue;
            if (marks_unit(file.sections[0], unit))
                return true;
        }
        return false;
    }

    void require_write() const {
        if (!active)
            fail("no mutator has begun");
        if (!menus_layer)
            fail("this mutator does not declare the menus layer");
    }

    UnitInfo& require_builder(std::string_view text) {
        UnitInfo* unit = find_unit(text);
        if (unit == nullptr)
            fail("unknown builder " + std::string{text});
        if (unit_removed(*unit))
            fail(unit->canonical + " was removed");
        if (!unit->builder)
            fail("unknown builder " + unit->canonical);
        return *unit;
    }

    UnitInfo& require_live_unit(std::string_view text) {
        UnitInfo* unit = find_unit(text);
        if (unit == nullptr)
            fail("unknown unit " + std::string{text});
        if (unit_removed(*unit))
            fail(unit->canonical + " was removed");
        return *unit;
    }

    Tally& tally_for(const std::string& name) {
        for (Tally& tally : tallies) {
            if (tally.name == name)
                return tally;
        }
        tallies.push_back(Tally{name, 0, 0});
        return tallies.back();
    }

    void remember_write(
        std::string path,
        std::string record,
        std::string block,
        std::string key,
        std::optional<std::string> old_text,
        std::optional<std::string> new_text
    ) {
        const std::string& who = actor_name();
        Tally& tally = tally_for(who);
        if (tally.writes >= mutator_write_limit)
            fail("this mutator has changed too many fields");
        ++tally.writes;
        rules::FieldWrite write;
        if (!who.empty())
            write.mutators.push_back(who);
        write.path = std::move(path);
        write.record = std::move(record);
        write.block = std::move(block);
        write.key = std::move(key);
        write.old_text = std::move(old_text);
        write.new_text = std::move(new_text);
        field_writes.push_back(std::move(write));
    }

    void remember_note(
        std::vector<std::string> who, std::string path, std::string record, std::string message
    ) {
        const std::string& actor = actor_name();
        Tally& tally = tally_for(actor);
        if (tally.notes >= mutator_note_limit)
            fail("this mutator has made too many notes");
        ++tally.notes;
        HookNote note;
        note.kind = HookNoteKind::menu_note;
        note.mutators = std::move(who);
        note.path = std::move(path);
        note.record = std::move(record);
        note.message = std::move(message);
        hook_notes.push_back(std::move(note));
    }

    void remember_removal(const DownloadFile& file, const Entry& entry) {
        if (!entry.from_file || entry.externally_removed)
            return;
        for (const RecordRemoval& removal : record_removals) {
            if (removal.kind == RemovalKind::download_entry && same_path(removal.path, file.path) &&
                same_text(removal.name, entry.section))
                return;
        }
        RecordRemoval removal;
        removal.mutator = actor_name();
        removal.kind = RemovalKind::download_entry;
        removal.name = entry.section;
        removal.path = file.path;
        record_removals.push_back(std::move(removal));
    }

    void journal_new_section(const DownloadFile& file, const Entry& entry) {
        const char* const names[] = {"UNITMENU", "MENU", "BUTTON", "UNITNAME"};
        for (const char* name : names) {
            const std::string* value = find_snap(entry.keys, name);
            remember_write(
                file.path,
                entry.section,
                "",
                name,
                std::nullopt,
                value != nullptr ? std::optional<std::string>{*value}
                                 : std::optional<std::string>{""}
            );
        }
    }

    void load_units() {
        const std::string folder = directory_of(*layout, defs::DataDirectory::units);
        std::string suffix = ".";
        suffix += fold_text(layout->unit_extension);
        const std::vector<std::size_t> indices = documents->documents_in(folder, suffix, false);
        for (const std::size_t doc_index : indices) {
            const rules::LoadedDocument& loaded = documents->at(doc_index);
            const tdf::Block* root = loaded.document.root();
            const tdf::Block* info = root != nullptr ? tdf::find_child(root, "UNITINFO") : nullptr;
            UnitInfo unit;
            unit.doc_index = doc_index;
            const char* named = info != nullptr ? tdf::find_value(info, "UnitName") : nullptr;
            if (named != nullptr && named[0] != '\0') {
                unit.canonical = named;
            } else {
                const auto slash = loaded.path.rfind('/');
                const std::string_view file_name =
                    slash == std::string::npos ? std::string_view{loaded.path}
                                               : std::string_view{loaded.path}.substr(slash + 1);
                const auto dot = file_name.rfind('.');
                unit.canonical = std::string{
                    dot == std::string_view::npos ? file_name : file_name.substr(0, dot)
                };
            }
            unit.folded = fold_text(unit.canonical);
            unit.builder = info != nullptr && tdf::get_int(info, "builder", 0) != 0;
            if (unit.builder)
                probe_pages(unit);
            units.push_back(std::move(unit));
        }
    }

    void probe_pages(UnitInfo& unit) {
        const std::string folder = directory_of(*layout, defs::DataDirectory::guis);
        for (int page_index = 0; page_index <= k_page_probe_cap; ++page_index) {
            std::string path = folder;
            path += "/";
            path += unit.canonical;
            path += std::to_string(page_index);
            path += ".GUI";
            const std::optional<std::string> text = read_store(*store, path);
            const bool missing = !text.has_value() || text->empty();
            if (page_index == 0) {
                if (!missing)
                    unit.pages.push_back(load_page(path, page_index, *text));
                continue;
            }
            if (missing)
                break;
            unit.pages.push_back(load_page(path, page_index, *text));
        }
    }

    Page load_page(const std::string& path, int page_index, const std::string& text) {
        Page page;
        page.path = path;
        page.page = page_index;
        page.authored = true;
        tdf::OwnedDocument parsed;
        if (!parsed.parse(text)) {
            // A page that does not parse still occupies its slots. It has no buttons.
            page.parsed = false;
            return page;
        }
        page.parsed = true;
        const tdf::Block* root = parsed.root();
        const uint32_t count = tdf::child_count(root);
        for (uint32_t gadget_index = 0; gadget_index < count; ++gadget_index) {
            const tdf::Block* gadget = tdf::child_at(root, gadget_index);
            Button button;
            button.gadget_index = gadget_index;
            const tdf::Block* common =
                gadget != nullptr ? tdf::find_child(gadget, "COMMON") : nullptr;
            const char* name = common != nullptr ? tdf::find_value(common, "name") : nullptr;
            if (name != nullptr)
                button.name = name;
            button.folded = fold_text(button.name);
            const char* attribs =
                common != nullptr ? tdf::find_value(common, "commonattribs") : nullptr;
            if (attribs != nullptr) {
                button.has_attribs = true;
                button.attribs_text = attribs;
                button.unit_bit = (low_byte(button.attribs_text) & k_unit_button_bit) != 0;
            }
            button.was_unit_button = button.unit_bit;
            const char* grayed = gadget != nullptr ? tdf::find_value(gadget, "grayedout") : nullptr;
            if (grayed != nullptr) {
                button.has_grayed = true;
                button.grayed_text = grayed;
            }
            const char* active_text =
                common != nullptr ? tdf::find_value(common, "active") : nullptr;
            if (active_text != nullptr) {
                button.has_active = true;
                button.active_text = active_text;
            }
            // A missing active is not drawn. grayedout sticks only when the unit bit is clear.
            button.hidden = !button.has_active || tdf::parse_int(button.active_text.c_str()) == 0;
            const bool gray_bit =
                button.has_grayed && (tdf::parse_int(button.grayed_text.c_str()) & 1) != 0;
            button.greyed = gray_bit && !button.unit_bit;
            page.buttons.push_back(std::move(button));
        }
        return page;
    }

    void load_downloads() {
        const std::string folder = directory_of(*layout, defs::DataDirectory::download);
        const std::vector<std::size_t> indices = documents->documents_in(folder, ".tdf", false);
        for (const std::size_t doc_index : indices) {
            const rules::LoadedDocument& loaded = documents->at(doc_index);
            DownloadFile file;
            file.path = loaded.path;
            file.doc_index = doc_index;
            const tdf::Block* root = loaded.document.root();
            const uint32_t count = tdf::child_count(root);
            for (uint32_t index = 0; index < count; ++index)
                file.sections.push_back(read_entry(tdf::child_at(root, index)));
            files.push_back(std::move(file));
        }
    }

    void capture_marks() {
        for (const DownloadFile& file : files) {
            if (file.sections.empty())
                continue;
            const Entry& first = file.sections[0];
            if (!marks_candidate(first))
                continue;
            const UnitInfo* unit = find_unit(first.unit_name);
            if (unit == nullptr)
                continue;
            if (!was_marked(*unit))
                marked.push_back(unit->folded);
        }
    }

    [[nodiscard]] bool marks_candidate(const Entry& entry) const {
        if (!entry.alive || entry.placeholder)
            return false;
        if (find_unit(entry.unit_menu) == nullptr)
            return false;
        return find_unit(entry.unit_name) != nullptr;
    }

    [[nodiscard]] bool path_taken(std::string_view path) const {
        if (documents->find(path) != nullptr)
            return true;
        for (const DownloadFile& file : files) {
            if (same_path(file.path, path))
                return true;
        }
        return false;
    }

    std::string fresh_path(const UnitInfo& unit) {
        const std::string folder = directory_of(*layout, defs::DataDirectory::download);
        std::string first = folder;
        first += "/";
        first += unit.canonical;
        first += ".tdf";
        if (!path_taken(first))
            return first;
        for (int suffix = 2; suffix < 10000; ++suffix) {
            std::string candidate = folder;
            candidate += "/";
            candidate += unit.canonical;
            candidate += "-";
            candidate += std::to_string(suffix);
            candidate += ".tdf";
            if (!path_taken(candidate))
                return candidate;
        }
        fail("a build menu could not be written");
    }

    std::size_t ensure_file(const UnitInfo& unit) {
        for (std::size_t index = 0; index < files.size(); ++index) {
            DownloadFile& file = files[index];
            if (!file.created || !same_text(file.owner_unit, unit.canonical))
                continue;
            if (file.sections.size() < k_visible_sections)
                return index;
        }
        const std::size_t created = files.size();
        DownloadFile file;
        file.path = fresh_path(unit);
        file.created = true;
        file.dirty = true;
        file.owner_unit = unit.canonical;
        file.actor = actor_name();
        if (!was_marked(unit)) {
            file.sections.push_back(make_placeholder());
        }
        files.push_back(std::move(file));
        if (!files[created].sections.empty())
            journal_new_section(files[created], files[created].sections[0]);
        return created;
    }

    [[nodiscard]] bool page_authored(const UnitInfo& builder, int page) const {
        if (page < 1 || page > k_max_page)
            return false;
        for (const Page& candidate : builder.pages) {
            if (candidate.authored && candidate.page == page)
                return true;
        }
        return false;
    }

    [[nodiscard]] bool
    entry_occupies(const Entry& entry, const UnitInfo& builder, int page, int slot) const {
        if (!entry.alive || entry.placeholder)
            return false;
        if (!same_text(entry.unit_menu, builder.canonical))
            return false;
        if (entry.menu < 2)
            return false;
        const int entry_page = entry.menu - 1;
        const int entry_slot = entry.button + 1;
        return entry_page == page && entry_slot == slot && page >= 1 && page <= k_max_page &&
               slot >= 1 && slot <= k_max_slot;
    }

    [[nodiscard]] bool slot_taken(
        const UnitInfo& builder, int page, int slot, std::size_t skip_file, std::size_t skip_index
    ) const {
        if (page_authored(builder, page))
            return true;
        for (std::size_t file_index = 0; file_index < files.size(); ++file_index) {
            const DownloadFile& file = files[file_index];
            const std::size_t shown = visible_count(file);
            for (std::size_t entry_index = 0; entry_index < shown; ++entry_index) {
                if (file_index == skip_file && entry_index == skip_index)
                    continue;
                if (entry_occupies(file.sections[entry_index], builder, page, slot))
                    return true;
            }
        }
        return false;
    }

    [[nodiscard]] std::string occupant(
        const UnitInfo& builder, int page, int slot, std::size_t skip_file, std::size_t skip_index
    ) const {
        if (page_authored(builder, page))
            return "the page itself";
        for (std::size_t file_index = 0; file_index < files.size(); ++file_index) {
            const DownloadFile& file = files[file_index];
            const std::size_t shown = visible_count(file);
            for (std::size_t entry_index = 0; entry_index < shown; ++entry_index) {
                if (file_index == skip_file && entry_index == skip_index)
                    continue;
                const Entry& entry = file.sections[entry_index];
                if (!entry_occupies(entry, builder, page, slot))
                    continue;
                if (entry.placer.empty())
                    return "the page itself";
                return entry.placer;
            }
        }
        return "the page itself";
    }

    [[nodiscard]] bool next_free(
        const UnitInfo& builder, std::size_t skip_file, std::size_t skip_index, int& page, int& slot
    ) const {
        for (int page_index = 1; page_index <= k_max_page; ++page_index) {
            for (int slot_index = 1; slot_index <= k_max_slot; ++slot_index) {
                if (slot_taken(builder, page_index, slot_index, skip_file, skip_index))
                    continue;
                page = page_index;
                slot = slot_index;
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] int last_page_of(const UnitInfo& builder) const {
        int last = 0;
        for (const Page& page : builder.pages) {
            if (page.authored && page.page >= 1 && page.page > last)
                last = page.page;
        }
        for (const DownloadFile& file : files) {
            const std::size_t shown = visible_count(file);
            for (std::size_t index = 0; index < shown; ++index) {
                const Entry& entry = file.sections[index];
                if (!entry.alive || entry.placeholder)
                    continue;
                if (!same_text(entry.unit_menu, builder.canonical) || entry.menu < 2)
                    continue;
                const int page = entry.menu - 1;
                if (page > last)
                    last = page;
            }
        }
        return last;
    }

    [[nodiscard]] std::optional<Place>
    offered_place(const UnitInfo& builder, const UnitInfo& unit) const {
        for (const Page& page : builder.pages) {
            for (const Button& button : page.buttons) {
                if (!same_text(button.name, unit.canonical))
                    continue;
                Place place;
                place.page = page.page;
                place.slot =
                    button.gadget_index >= 4 ? static_cast<int>(button.gadget_index) - 3 : 1;
                return place;
            }
        }
        for (const DownloadFile& file : files) {
            const std::size_t shown = visible_count(file);
            for (std::size_t index = 0; index < shown; ++index) {
                const Entry& entry = file.sections[index];
                if (!entry.alive || entry.placeholder)
                    continue;
                if (!same_text(entry.unit_menu, builder.canonical) ||
                    !same_text(entry.unit_name, unit.canonical))
                    continue;
                Place place;
                place.page = entry.menu - 1;
                place.slot = entry.button + 1;
                return place;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] bool find_entry(
        const UnitInfo& builder,
        const UnitInfo& unit,
        std::size_t& file_index,
        std::size_t& entry_index
    ) const {
        std::size_t fallback_file = std::numeric_limits<std::size_t>::max();
        std::size_t fallback_entry = 0;
        for (std::size_t file_slot = 0; file_slot < files.size(); ++file_slot) {
            const DownloadFile& file = files[file_slot];
            for (std::size_t section_index = 0; section_index < file.sections.size();
                 ++section_index) {
                const Entry& entry = file.sections[section_index];
                if (!entry.alive || entry.placeholder)
                    continue;
                if (!same_text(entry.unit_menu, builder.canonical) ||
                    !same_text(entry.unit_name, unit.canonical))
                    continue;
                if (section_index < visible_count(file)) {
                    file_index = file_slot;
                    entry_index = section_index;
                    return true;
                }
                if (fallback_file == std::numeric_limits<std::size_t>::max()) {
                    fallback_file = file_slot;
                    fallback_entry = section_index;
                }
            }
        }
        if (fallback_file == std::numeric_limits<std::size_t>::max())
            return false;
        file_index = fallback_file;
        entry_index = fallback_entry;
        return true;
    }

    [[nodiscard]] int first_button_page(const UnitInfo& builder, const UnitInfo& unit) const {
        for (const Page& page : builder.pages) {
            for (const Button& button : page.buttons) {
                if (same_text(button.name, unit.canonical))
                    return page.page;
            }
        }
        return -1;
    }

    void note_collision(
        const UnitInfo& unit,
        int from_page,
        int from_slot,
        int to_page,
        int to_slot,
        const std::string& who,
        const std::string& path
    ) {
        if (from_page == to_page && from_slot == to_slot)
            return;
        std::vector<std::string> mutators;
        if (who == "the page itself") {
            mutators.push_back(actor_name());
        } else {
            mutators.push_back(who);
            if (who != actor_name())
                mutators.push_back(actor_name());
        }
        std::string message = unit.canonical;
        message += " moved from page ";
        message += std::to_string(from_page);
        message += " slot ";
        message += std::to_string(from_slot);
        message += " to page ";
        message += std::to_string(to_page);
        message += " slot ";
        message += std::to_string(to_slot);
        message += ": taken by ";
        message += who;
        remember_note(std::move(mutators), path, unit.canonical, std::move(message));
    }

    void note_art(const UnitInfo& unit, const UnitInfo& builder, const std::string& path) {
        if (art_present(*store, unit.canonical))
            return;
        std::string message = unit.canonical;
        message += "'s button has no picture on ";
        message += builder.canonical;
        message += ": no anims/";
        message += unit.canonical;
        message += "_gadget.gaf";
        remember_note({actor_name()}, path, unit.canonical, std::move(message));
    }

    [[nodiscard]] uint32_t canbuild_count(const UnitInfo& builder) const {
        const std::string folder = directory_of(*layout, defs::DataDirectory::gamedata);
        const std::vector<std::size_t> indices =
            documents->documents_in(folder, "sidedata.tdf", false);
        if (indices.empty())
            return 0;
        const tdf::Block* root = documents->at(indices[0]).document.root();
        const tdf::Block* canbuild = root != nullptr ? tdf::find_child(root, "CANBUILD") : nullptr;
        const tdf::Block* block =
            canbuild != nullptr ? tdf::find_child(canbuild, builder.canonical.c_str()) : nullptr;
        if (block == nullptr)
            return 0;
        const uint32_t kept = limits::build_list_kept({});
        uint32_t count = 0;
        for (uint32_t number = 1; number <= kept; ++number) {
            const std::string key = "canbuild" + std::to_string(number);
            const char* value = tdf::find_value(block, key.c_str());
            if (value == nullptr)
                break;
            const UnitInfo* named = find_unit(value);
            if (named == nullptr || unit_removed(*named))
                continue;
            ++count;
        }
        return count;
    }

    [[nodiscard]] uint32_t
    downloads_before(const UnitInfo& builder, std::size_t dest_file, std::size_t dest_index) const {
        uint32_t count = 0;
        for (std::size_t file_index = 0; file_index < files.size(); ++file_index) {
            const DownloadFile& file = files[file_index];
            const std::size_t shown = visible_count(file);
            const std::size_t stop = file_index == dest_file ? std::min(dest_index, shown) : shown;
            for (std::size_t entry_index = 0; entry_index < stop; ++entry_index) {
                const Entry& entry = file.sections[entry_index];
                if (!entry.alive || entry.placeholder)
                    continue;
                if (!same_text(entry.unit_menu, builder.canonical))
                    continue;
                const UnitInfo* named = find_unit(entry.unit_name);
                if (named == nullptr || unit_removed(*named))
                    continue;
                ++count;
            }
            if (file_index == dest_file)
                break;
        }
        return count;
    }

    void note_ai_if_full(
        const UnitInfo& unit,
        const UnitInfo& builder,
        std::size_t dest_file,
        std::size_t dest_index,
        const std::string& path
    ) {
        const uint32_t limit = defs::download_build_list_entries({});
        const uint32_t seen =
            canbuild_count(builder) + downloads_before(builder, dest_file, dest_index);
        if (seen < limit)
            return;
        std::string message = "the AI does not see ";
        message += unit.canonical;
        message += " on ";
        message += builder.canonical;
        message += ": ";
        message += builder.canonical;
        message += "'s list is full";
        remember_note({actor_name()}, path, unit.canonical, std::move(message));
    }

    void place_new(const UnitInfo& builder, const UnitInfo& unit, int page, int slot) {
        const std::size_t file_index = ensure_file(unit);
        DownloadFile& file = files[file_index];
        const std::size_t dest_index = file.sections.size();
        note_ai_if_full(unit, builder, file_index, dest_index, file.path);
        Entry entry = make_entry(builder.canonical, unit.canonical, page, slot, actor_name());
        file.actor = actor_name();
        file.dirty = true;
        file.sections.push_back(std::move(entry));
        journal_new_section(file, file.sections.back());
        note_art(unit, builder, file.path);
    }

    Place add_entry(
        const std::string& builder_text,
        const std::string& unit_text,
        const std::optional<Request>& request
    ) {
        require_write();
        UnitInfo& builder = require_builder(builder_text);
        UnitInfo& unit = require_live_unit(unit_text);
        if (const std::optional<Place> current = offered_place(builder, unit))
            return *current;
        const std::size_t none = std::numeric_limits<std::size_t>::max();
        Place landed;
        int from_page = 0;
        int from_slot = 0;
        if (!request.has_value()) {
            if (!next_free(builder, none, none, landed.page, landed.slot))
                fail(builder.canonical + " has no free build slot on pages 1 to 7");
        } else if (request->fresh_page) {
            const int page = last_page_of(builder) + 1;
            if (page < 1 || page > k_max_page)
                fail(builder.canonical + " has no free build slot on pages 1 to 7");
            landed.page = page;
            landed.slot = 1;
        } else {
            from_page = request->page;
            from_slot = request->slot;
            landed.page = from_page;
            landed.slot = from_slot;
            if (slot_taken(builder, landed.page, landed.slot, none, none)) {
                const std::string who = occupant(builder, from_page, from_slot, none, none);
                if (!next_free(builder, none, none, landed.page, landed.slot))
                    fail(builder.canonical + " has no free build slot on pages 1 to 7");
                const std::size_t file_index = ensure_file(unit);
                note_collision(
                    unit,
                    from_page,
                    from_slot,
                    landed.page,
                    landed.slot,
                    who,
                    files[file_index].path
                );
                place_new_at(builder, unit, landed, file_index);
                return landed;
            }
        }
        place_new(builder, unit, landed.page, landed.slot);
        return landed;
    }

    void place_new_at(
        const UnitInfo& builder, const UnitInfo& unit, Place landed, std::size_t file_index
    ) {
        DownloadFile& file = files[file_index];
        const std::size_t dest_index = file.sections.size();
        note_ai_if_full(unit, builder, file_index, dest_index, file.path);
        Entry entry =
            make_entry(builder.canonical, unit.canonical, landed.page, landed.slot, actor_name());
        file.actor = actor_name();
        file.dirty = true;
        file.sections.push_back(std::move(entry));
        journal_new_section(file, file.sections.back());
        note_art(unit, builder, file.path);
    }

    void retarget(DownloadFile& file, Entry& entry, int page, int slot) {
        const int menu = page + 1;
        const int button = slot - 1;
        const std::string menu_text = std::to_string(menu);
        const std::string button_text = std::to_string(button);
        if (entry.menu_text != menu_text) {
            remember_write(
                file.path,
                entry.section,
                "",
                "MENU",
                entry.from_file || find_snap(entry.keys, "MENU") != nullptr
                    ? std::optional<std::string>{entry.menu_text}
                    : std::nullopt,
                menu_text
            );
        }
        if (entry.button_text != button_text) {
            remember_write(
                file.path,
                entry.section,
                "",
                "BUTTON",
                entry.from_file || find_snap(entry.keys, "BUTTON") != nullptr
                    ? std::optional<std::string>{entry.button_text}
                    : std::nullopt,
                button_text
            );
        }
        entry.menu = menu;
        entry.button = button;
        entry.menu_text = menu_text;
        entry.button_text = button_text;
        entry.placer = actor_name();
        sync_entry(entry);
        file.dirty = true;
        file.actor = actor_name();
    }

    Place move_entry(
        const std::string& builder_text, const std::string& unit_text, const Request& request
    ) {
        require_write();
        if (request.fresh_page)
            fail("page must be from 1 to 7");
        UnitInfo& builder = require_builder(builder_text);
        UnitInfo& unit = require_live_unit(unit_text);
        const int on_page = first_button_page(builder, unit);
        if (on_page >= 0)
            fail(
                unit.canonical + "'s button is part of " + builder.canonical + "'s page " +
                std::to_string(on_page) + "; grey or hide it and add an entry instead"
            );
        std::size_t file_index = 0;
        std::size_t entry_index = 0;
        if (!find_entry(builder, unit, file_index, entry_index))
            fail(builder.canonical + " does not build " + unit.canonical);
        Entry& entry = files[file_index].sections[entry_index];
        const int old_page = entry.menu - 1;
        const int old_slot = entry.button + 1;
        int page = request.page;
        int slot = request.slot;
        if (slot_taken(builder, page, slot, file_index, entry_index)) {
            const std::string who =
                occupant(builder, request.page, request.slot, file_index, entry_index);
            if (!next_free(builder, file_index, entry_index, page, slot))
                fail(builder.canonical + " has no free build slot on pages 1 to 7");
            note_collision(
                unit, request.page, request.slot, page, slot, who, files[file_index].path
            );
        }
        if (page != old_page || slot != old_slot)
            retarget(files[file_index], files[file_index].sections[entry_index], page, slot);
        return Place{page, slot};
    }

    void kill_section(std::size_t file_index, std::size_t entry_index, bool journal_it) {
        DownloadFile& file = files[file_index];
        Entry& entry = file.sections[entry_index];
        if (!entry.alive)
            return;
        entry.alive = false;
        if (journal_it)
            remember_removal(file, entry);
        file.dirty = true;
        if (file.actor.empty())
            file.actor = actor_name();
    }

    void drop_named(std::string_view unit_text, bool from_removal) {
        for (std::size_t file_index = 0; file_index < files.size(); ++file_index) {
            DownloadFile& file = files[file_index];
            for (std::size_t entry_index = 0; entry_index < file.sections.size(); ++entry_index) {
                Entry& entry = file.sections[entry_index];
                if (!entry.alive)
                    continue;
                const bool names =
                    same_text(entry.unit_name, unit_text) || same_text(entry.unit_menu, unit_text);
                if (!names)
                    continue;
                const bool journal_it = !from_removal || !entry.externally_removed;
                kill_section(file_index, entry_index, journal_it && !entry.externally_removed);
            }
        }
    }

    void note_external_removal(const RecordRemoval& removal) {
        for (DownloadFile& file : files) {
            if (!same_path(file.path, removal.path))
                continue;
            for (Entry& entry : file.sections) {
                if (!entry.alive || !same_text(entry.section, removal.name))
                    continue;
                entry.alive = false;
                entry.externally_removed = true;
                return;
            }
        }
    }

    void grey_button(Page& page, Button& button, std::string_view record_name) {
        std::string current_attribs = button.new_attribs.value_or(button.attribs_text);
        const bool have_attribs = button.new_attribs.has_value() || button.has_attribs;
        const bool bit = have_attribs && (low_byte(current_attribs) & k_unit_button_bit) != 0;
        std::string current_grayed = button.new_grayed.value_or(button.grayed_text);
        const bool have_grayed = button.new_grayed.has_value() || button.has_grayed;
        const bool already = !bit && have_grayed && current_grayed == "1";
        button.was_unit_button = true;
        button.greyed = true;
        if (already)
            return;
        if (!(have_grayed && current_grayed == "1")) {
            std::optional<std::string> old_text;
            if (have_grayed)
                old_text = current_grayed;
            remember_write(page.path, std::string{record_name}, "", "grayedout", old_text, "1");
            button.new_grayed = "1";
            button.dirty = true;
            page.dirty = true;
        }
        if (bit) {
            const std::string written = cleared_attribs(current_attribs);
            if (current_attribs != written) {
                remember_write(
                    page.path,
                    std::string{record_name},
                    "COMMON",
                    "commonattribs",
                    have_attribs ? std::optional<std::string>{current_attribs} : std::nullopt,
                    written
                );
                button.new_attribs = written;
                button.dirty = true;
                page.dirty = true;
            }
        }
    }

    void hide_button(Page& page, Button& button, std::string_view record_name) {
        std::string current = button.new_active.value_or(button.active_text);
        const bool have = button.new_active.has_value() || button.has_active;
        button.hidden = true;
        if (!have || current == "0")
            return;
        remember_write(page.path, std::string{record_name}, "COMMON", "active", current, "0");
        button.new_active = "0";
        button.dirty = true;
        page.dirty = true;
    }

    void grey_named(std::string_view unit_text) {
        for (UnitInfo& builder : units) {
            if (!builder.builder)
                continue;
            for (Page& page : builder.pages) {
                for (Button& button : page.buttons) {
                    if (!same_text(button.name, unit_text))
                        continue;
                    const UnitInfo* named = find_unit(button.name);
                    grey_button(page, button, named != nullptr ? named->canonical : button.name);
                }
            }
        }
    }

    void
    remove_entry(const std::optional<std::string>& builder_text, const std::string& unit_text) {
        require_write();
        UnitInfo* builder = nullptr;
        if (builder_text.has_value())
            builder = &require_builder(*builder_text);
        UnitInfo& unit = require_live_unit(unit_text);
        for (std::size_t file_index = 0; file_index < files.size(); ++file_index) {
            DownloadFile& file = files[file_index];
            for (std::size_t entry_index = 0; entry_index < file.sections.size(); ++entry_index) {
                Entry& entry = file.sections[entry_index];
                if (!entry.alive || entry.placeholder)
                    continue;
                if (!same_text(entry.unit_name, unit.canonical))
                    continue;
                if (builder != nullptr && !same_text(entry.unit_menu, builder->canonical))
                    continue;
                kill_section(file_index, entry_index, true);
            }
        }
        for (UnitInfo& candidate : units) {
            if (!candidate.builder)
                continue;
            if (builder != nullptr && &candidate != builder)
                continue;
            for (Page& page : candidate.pages) {
                for (Button& button : page.buttons) {
                    if (!same_text(button.name, unit.canonical))
                        continue;
                    grey_button(page, button, unit.canonical);
                }
            }
        }
    }

    [[nodiscard]] bool offers_download(const UnitInfo& builder, const UnitInfo& unit) const {
        for (const DownloadFile& file : files) {
            const std::size_t shown = visible_count(file);
            for (std::size_t index = 0; index < shown; ++index) {
                const Entry& entry = file.sections[index];
                if (!entry.alive || entry.placeholder)
                    continue;
                if (same_text(entry.unit_menu, builder.canonical) &&
                    same_text(entry.unit_name, unit.canonical))
                    return true;
            }
        }
        return false;
    }

    void restyle(const std::string& builder_text, const std::string& unit_text, bool hide) {
        require_write();
        UnitInfo& builder = require_builder(builder_text);
        UnitInfo& unit = require_live_unit(unit_text);
        bool on_page = false;
        for (Page& page : builder.pages) {
            for (Button& button : page.buttons) {
                if (!same_text(button.name, unit.canonical))
                    continue;
                on_page = true;
                if (hide)
                    hide_button(page, button, unit.canonical);
                else
                    grey_button(page, button, unit.canonical);
            }
        }
        if (on_page)
            return;
        if (offers_download(builder, unit))
            fail(
                unit.canonical + " is a download entry on " + builder.canonical +
                "; use menus:remove"
            );
        fail(builder.canonical + " does not build " + unit.canonical);
    }

    [[nodiscard]] std::vector<std::string> builds_of(const std::string& builder_text) {
        UnitInfo& builder = require_builder(builder_text);
        std::vector<std::string> names;
        const auto push_name = [&](const UnitInfo& unit) {
            for (const std::string& kept : names) {
                if (same_text(kept, unit.canonical))
                    return;
            }
            names.push_back(unit.canonical);
        };
        for (const Page& page : builder.pages) {
            for (const Button& button : page.buttons) {
                if (button.greyed || button.hidden)
                    continue;
                if (!button.unit_bit && !button.was_unit_button)
                    continue;
                const UnitInfo* unit = find_unit(button.name);
                if (unit == nullptr || unit_removed(*unit))
                    continue;
                push_name(*unit);
            }
        }

        struct Ranked {
            int page{0};
            int slot{0};
            std::size_t file_index{0};
            std::size_t entry_index{0};
            const UnitInfo* unit{};
        };

        std::vector<Ranked> ranked;
        for (std::size_t file_index = 0; file_index < files.size(); ++file_index) {
            const DownloadFile& file = files[file_index];
            const std::size_t shown = visible_count(file);
            for (std::size_t entry_index = 0; entry_index < shown; ++entry_index) {
                const Entry& entry = file.sections[entry_index];
                if (!entry.alive || entry.placeholder)
                    continue;
                if (!same_text(entry.unit_menu, builder.canonical))
                    continue;
                const UnitInfo* unit = find_unit(entry.unit_name);
                if (unit == nullptr || unit_removed(*unit))
                    continue;
                Ranked row;
                row.page = entry.menu - 1;
                row.slot = entry.button + 1;
                row.file_index = file_index;
                row.entry_index = entry_index;
                row.unit = unit;
                ranked.push_back(row);
            }
        }
        std::stable_sort(
            ranked.begin(), ranked.end(), [](const Ranked& first, const Ranked& second) {
                if (first.page != second.page)
                    return first.page < second.page;
                if (first.slot != second.slot)
                    return first.slot < second.slot;
                if (first.file_index != second.file_index)
                    return first.file_index < second.file_index;
                return first.entry_index < second.entry_index;
            }
        );
        for (const Ranked& row : ranked)
            push_name(*row.unit);
        return names;
    }

    static std::string require_lua_string(lua_State* state, int index, const char* complaint) {
        if (lua_type(state, index) != LUA_TSTRING)
            fail(complaint);
        std::size_t length = 0;
        const char* text = lua_tolstring(state, index, &length);
        return text != nullptr ? std::string{text, length} : std::string{};
    }

    static void push_place(lua_State* state, Place place) {
        lua_createtable(state, 0, 2);
        lua_pushinteger(state, place.page);
        lua_setfield(state, -2, "page");
        lua_pushinteger(state, place.slot);
        lua_setfield(state, -2, "slot");
    }

    static Request read_request(lua_State* state, int index, bool allow_fresh) {
        if (!lua_istable(state, index))
            fail("page must be from 1 to 7");
        const int table = lua_absindex(state, index);
        lua_pushstring(state, "page");
        lua_rawget(state, table);
        Request request;
        if (lua_type(state, -1) == LUA_TSTRING) {
            std::size_t length = 0;
            const char* text = lua_tolstring(state, -1, &length);
            const std::string folded =
                fold_text(std::string_view{text != nullptr ? text : "", length});
            lua_pop(state, 1);
            if (!allow_fresh || folded != "new")
                fail("page must be from 1 to 7");
            request.fresh_page = true;
            request.page = 1;
            request.slot = 1;
            return request;
        }
        if (lua_isinteger(state, -1) == 0) {
            lua_pop(state, 1);
            fail("page must be from 1 to 7");
        }
        const lua_Integer page_number = lua_tointeger(state, -1);
        lua_pop(state, 1);
        if (page_number < 1 || page_number > k_max_page)
            fail("page must be from 1 to 7");
        lua_pushstring(state, "slot");
        lua_rawget(state, table);
        if (lua_isinteger(state, -1) == 0) {
            lua_pop(state, 1);
            fail("slot must be from 1 to 6");
        }
        const lua_Integer slot_number = lua_tointeger(state, -1);
        lua_pop(state, 1);
        if (slot_number < 1 || slot_number > k_max_slot)
            fail("slot must be from 1 to 6");
        request.page = static_cast<int>(page_number);
        request.slot = static_cast<int>(slot_number);
        return request;
    }

    void compact() {
        for (DownloadFile& file : files) {
            std::vector<Entry> kept;
            kept.reserve(file.sections.size());
            for (Entry& entry : file.sections) {
                if (entry.alive)
                    kept.push_back(std::move(entry));
            }
            if (kept.size() != file.sections.size())
                file.sections = std::move(kept);
            if (file.sections.empty() && !file.created)
                file.hide = true;
        }
    }

    [[nodiscard]] bool file_marks_other(std::size_t file_index, std::string_view unit_name) const {
        if (files[file_index].sections.empty())
            return false;
        const Entry& first = files[file_index].sections[0];
        if (!marks_candidate(first) || !known_menu(first))
            return false;
        if (same_text(first.unit_name, unit_name))
            return false;
        return marked_ok_name(first.unit_name);
    }

    std::size_t park_section(Entry entry) {
        const UnitInfo* unit = find_unit(entry.unit_name);
        if (unit == nullptr)
            fail("a build menu could not be laid out");
        std::size_t dest = ensure_file(*unit);
        if (files[dest].sections.size() >= k_visible_sections)
            dest = force_new_file(*unit);
        const bool marked_unit = was_marked(*unit) && !unit_removed(*unit);
        DownloadFile& file = files[dest];
        entry.from_file = false;
        entry.alive = true;
        entry.externally_removed = false;
        if (marked_unit) {
            if (!file.sections.empty() && file.sections[0].placeholder) {
                remember_removal(file, file.sections[0]);
                file.sections.erase(file.sections.begin());
            }
            file.sections.insert(file.sections.begin(), std::move(entry));
        } else if (file.sections.empty() || !file.sections[0].placeholder) {
            file.sections.insert(file.sections.begin(), make_placeholder());
            journal_new_section(file, file.sections[0]);
            file.sections.insert(file.sections.begin() + 1, std::move(entry));
        } else {
            file.sections.insert(file.sections.begin() + 1, std::move(entry));
        }
        journal_new_section(file, marked_unit ? file.sections[0] : file.sections[1]);
        file.dirty = true;
        file.hide = false;
        file.actor = actor_name().empty() ? last_mutator : actor_name();
        return dest;
    }

    std::size_t force_new_file(const UnitInfo& unit) {
        const std::size_t created = files.size();
        DownloadFile file;
        file.path = fresh_path(unit);
        file.created = true;
        file.dirty = true;
        file.owner_unit = unit.canonical;
        file.actor = actor_name().empty() ? last_mutator : actor_name();
        files.push_back(std::move(file));
        return created;
    }

    bool promote_one() {
        for (const UnitInfo& unit : units) {
            if (!was_marked(unit) || unit_removed(unit) || is_marking_first(unit))
                continue;
            std::size_t file_index = std::numeric_limits<std::size_t>::max();
            std::size_t entry_index = 0;
            for (std::size_t scan_file = 0;
                 scan_file < files.size() && file_index == std::numeric_limits<std::size_t>::max();
                 ++scan_file) {
                const std::size_t shown = visible_count(files[scan_file]);
                for (std::size_t scan_entry = 0; scan_entry < shown; ++scan_entry) {
                    if (!marks_unit(files[scan_file].sections[scan_entry], unit))
                        continue;
                    file_index = scan_file;
                    entry_index = scan_entry;
                    break;
                }
            }
            if (file_index == std::numeric_limits<std::size_t>::max())
                continue;
            if (entry_index == 0)
                continue;
            DownloadFile& file = files[file_index];
            const bool steal =
                file.sections[0].placeholder || !file_marks_other(file_index, unit.canonical);
            if (!steal) {
                Entry moved = std::move(file.sections[entry_index]);
                remember_removal(file, moved);
                file.sections.erase(
                    file.sections.begin() + static_cast<std::ptrdiff_t>(entry_index)
                );
                file.dirty = true;
                moved.from_file = false;
                park_section(std::move(moved));
                return true;
            }
            if (file.sections[0].placeholder) {
                remember_removal(file, file.sections[0]);
                Entry moved = std::move(file.sections[entry_index]);
                file.sections.erase(
                    file.sections.begin() + static_cast<std::ptrdiff_t>(entry_index)
                );
                file.sections.erase(file.sections.begin());
                file.sections.insert(file.sections.begin(), std::move(moved));
            } else {
                Entry moved = std::move(file.sections[entry_index]);
                file.sections.erase(
                    file.sections.begin() + static_cast<std::ptrdiff_t>(entry_index)
                );
                file.sections.insert(file.sections.begin(), std::move(moved));
            }
            file.dirty = true;
            file.hide = false;
            if (file.actor.empty())
                file.actor = last_mutator;
            return true;
        }
        return false;
    }

    bool suppress_one() {
        for (std::size_t file_index = 0; file_index < files.size(); ++file_index) {
            DownloadFile& file = files[file_index];
            if (file.sections.empty())
                continue;
            const Entry& first = file.sections[0];
            if (first.placeholder || !marks_candidate(first) || !known_menu(first))
                continue;
            if (marked_ok_name(first.unit_name))
                continue;
            if (file.sections.size() >= k_visible_sections) {
                // The section at index 4 would fall out of the loader's five once a
                // placeholder is inserted in front. Park it first, then insert.
                const std::size_t park_index = k_visible_sections - 1;
                Entry parked = std::move(file.sections[park_index]);
                remember_removal(file, parked);
                file.sections.erase(
                    file.sections.begin() + static_cast<std::ptrdiff_t>(park_index)
                );
                file.dirty = true;
                parked.from_file = false;
                park_section(std::move(parked));
            }
            DownloadFile& edited = files[file_index];
            edited.sections.insert(edited.sections.begin(), make_placeholder());
            journal_new_section(edited, edited.sections[0]);
            edited.dirty = true;
            edited.hide = false;
            if (edited.actor.empty())
                edited.actor = last_mutator;
            return true;
        }
        return false;
    }

    void repair() {
        compact();
        for (int step = 0; step < k_repair_steps; ++step) {
            if (promote_one())
                continue;
            if (suppress_one())
                continue;
            compact();
            return;
        }
        fail("a build menu could not be laid out");
    }

    void write_one_download(DownloadFile& file) {
        applying = file.actor.empty() ? last_mutator : file.actor;
        if (file.created && file.sections.empty())
            return;
        for (Entry& entry : file.sections)
            sync_entry(entry);
        rules::LoadedDocument* loaded = documents->find(file.path);
        if (file.sections.empty()) {
            if (loaded != nullptr && !loaded->removed)
                loaded->removed = true;
            return;
        }
        if (loaded != nullptr && loaded->removed && !file.sections.empty())
            loaded->removed = false;
        if (loaded == nullptr) {
            tdf::OwnedDocument fresh;
            if (!fresh.parse("[ENTRY]\r\n{\r\n}\r\n"))
                fail("a build menu could not be written");
            fill_document(fresh.get(), file);
            const std::optional<std::string> text = tdf::write_text(fresh.get());
            if (!text.has_value())
                fail("a build menu could not be written");
            std::string error;
            if (!documents->add(file.path, *text, &error))
                fail(error.empty() ? "a build menu could not be written" : error);
            rules::LoadedDocument* added = documents->find(file.path);
            if (added == nullptr)
                fail("a build menu could not be written");
            if (file.created)
                added->added = true;
            file.doc_index = documents->size() - 1;
            return;
        }
        if (download_matches(*loaded, file))
            return;
        fill_or_patch(*loaded, file);
    }

    [[nodiscard]] bool
    download_matches(const rules::LoadedDocument& loaded, const DownloadFile& file) const {
        const tdf::Block* root = loaded.document.root();
        if (tdf::child_count(root) != file.sections.size())
            return false;
        for (std::size_t index = 0; index < file.sections.size(); ++index) {
            if (!section_matches(
                    tdf::child_at(root, static_cast<uint32_t>(index)), file.sections[index]
                ))
                return false;
        }
        return true;
    }

    void fill_document(tdf::Document* document, const DownloadFile& file) {
        tdf::Block* root = tdf::editable_root(document);
        if (root == nullptr)
            fail("a build menu could not be written");
        while (tdf::child_count(root) > 0)
            require_edit(tdf::remove_child(document, root, tdf::child_count(root) - 1));
        for (const Entry& entry : file.sections) {
            tdf::EditStatus status = tdf::EditStatus::ok;
            tdf::Block* child = tdf::add_child(document, root, entry.section, &status);
            require_edit(status);
            if (child == nullptr)
                fail("a build menu could not be written");
            for (const KeySnap& snap : entry.keys)
                require_edit(tdf::set_value(document, child, snap.key, snap.value));
        }
    }

    void fill_or_patch(rules::LoadedDocument& loaded, const DownloadFile& file) {
        tdf::Document* document = loaded.document.get();
        tdf::Block* root = tdf::editable_root(document);
        if (root == nullptr)
            fail("a build menu could not be written");
        const bool same_shape = tdf::child_count(root) == file.sections.size();
        bool names_match = same_shape;
        if (same_shape) {
            for (std::size_t index = 0; index < file.sections.size(); ++index) {
                const tdf::Block* child = tdf::child_at(root, static_cast<uint32_t>(index));
                if (child == nullptr || child->name == nullptr ||
                    !same_text(child->name, file.sections[index].section))
                    names_match = false;
            }
        }
        if (!names_match) {
            fill_document(document, file);
            return;
        }
        for (std::size_t index = 0; index < file.sections.size(); ++index) {
            tdf::Block* child = tdf::editable_child(root, static_cast<uint32_t>(index));
            if (child == nullptr)
                fail("a build menu could not be written");
            for (const KeySnap& snap : file.sections[index].keys) {
                const char* value = tdf::find_value(child, snap.key.c_str());
                if (value != nullptr && snap.value == value)
                    continue;
                require_edit(tdf::set_value(document, child, snap.key, snap.value));
            }
        }
    }

    void write_downloads() {
        const std::size_t count = files.size();
        for (std::size_t index = 0; index < count; ++index) {
            DownloadFile& file = files[index];
            if (!file.dirty && !file.created && !file.hide)
                continue;
            write_one_download(file);
        }
    }

    void write_pages() {
        for (UnitInfo& unit : units) {
            for (Page& page : unit.pages) {
                if (!page.dirty)
                    continue;
                bool needed = false;
                for (const Button& button : page.buttons) {
                    if (button.dirty)
                        needed = true;
                }
                if (!needed) {
                    page.dirty = false;
                    continue;
                }
                applying = last_mutator;
                rules::LoadedDocument* loaded = documents->find(page.path);
                if (loaded == nullptr) {
                    const std::optional<std::string> text = read_store(*store, page.path);
                    if (!text.has_value())
                        fail("a build menu could not be written");
                    std::string error;
                    if (!documents->add(page.path, *text, &error))
                        fail(error.empty() ? "a build menu could not be written" : error);
                    loaded = documents->find(page.path);
                    if (loaded == nullptr)
                        fail("a build menu could not be written");
                }
                patch_page(*loaded, page);
            }
        }
    }

    void patch_page(rules::LoadedDocument& loaded, const Page& page) {
        tdf::Document* document = loaded.document.get();
        tdf::Block* root = tdf::editable_root(document);
        if (root == nullptr)
            fail("a build menu could not be written");
        for (const Button& button : page.buttons) {
            if (!button.dirty)
                continue;
            tdf::Block* gadget = tdf::editable_child(root, button.gadget_index);
            if (gadget == nullptr)
                fail("a build menu could not be written");
            tdf::Block* common = tdf::editable_child(gadget, "COMMON");
            if (button.new_grayed.has_value()) {
                const char* value = tdf::find_value(gadget, "grayedout");
                if (value == nullptr || *button.new_grayed != value)
                    require_edit(tdf::set_value(document, gadget, "grayedout", *button.new_grayed));
            }
            if (button.new_attribs.has_value()) {
                if (common == nullptr)
                    fail("a build menu could not be written");
                const char* value = tdf::find_value(common, "commonattribs");
                if (value == nullptr || *button.new_attribs != value)
                    require_edit(
                        tdf::set_value(document, common, "commonattribs", *button.new_attribs)
                    );
            }
            if (button.new_active.has_value()) {
                if (common == nullptr)
                    fail("a build menu could not be written");
                const char* value = tdf::find_value(common, "active");
                if (value == nullptr || *button.new_active != value)
                    require_edit(tdf::set_value(document, common, "active", *button.new_active));
            }
        }
    }
};

Menus* Menus::from_lua(lua_State* state, int index) {
    void* memory = luaL_testudata(state, index, k_meta_name);
    if (memory == nullptr) {
        luaL_error(state, "expected a menus collection");
        return nullptr;
    }
    auto* box = static_cast<LuaBox*>(memory);
    if (box->owner == nullptr) {
        luaL_error(state, "expected a menus collection");
        return nullptr;
    }
    return box->owner;
}

Menus::Menus(
    rules::DocumentSet& documents, const oa::AssetStore& store, const defs::DataLayout* layout
)
    : state_(std::make_unique<State>(documents, store, layout)) {
}

Menus::~Menus() = default;

void Menus::begin_mutator(std::string mutator, bool menus_layer) {
    state_->begin(std::move(mutator), menus_layer);
}

void Menus::end_mutator(std::span<const RecordRemoval> removals_this_turn) {
    state_->end(removals_this_turn);
}

void Menus::ensure_meta(lua_State* state) {
    if (luaL_newmetatable(state, k_meta_name) == 0) {
        lua_pop(state, 1);
        return;
    }
    lua_pushcfunction(state, lua_index);
    lua_setfield(state, -2, "__index");
    lua_pushcfunction(state, lua_newindex);
    lua_setfield(state, -2, "__newindex");
    lua_pushboolean(state, 0);
    lua_setfield(state, -2, "__metatable");
    lua_pop(state, 1);
}

void Menus::push(lua_State* state) {
    ensure_meta(state);
    auto* box = static_cast<LuaBox*>(lua_newuserdatauv(state, sizeof(LuaBox), 0));
    box->owner = this;
    luaL_setmetatable(state, k_meta_name);
}

void Menus::set_context(lua_State* state, int ctx_index) {
    const int ctx = lua_absindex(state, ctx_index);
    push(state);
    lua_setfield(state, ctx, "menus");
}

bool Menus::resolve(std::string& error) {
    return state_->write_back(error);
}

const std::string& Menus::failed_mutator() const {
    return state_->failed;
}

std::span<const rules::FieldWrite> Menus::writes() const {
    return state_->field_writes;
}

std::span<const HookNote> Menus::notes() const {
    return state_->hook_notes;
}

std::span<const RecordRemoval> Menus::removals() const {
    return state_->record_removals;
}

int Menus::lua_index(lua_State* state) {
    from_lua(state, 1);
    if (lua_type(state, 2) != LUA_TSTRING)
        return luaL_error(state, "menus has no method");
    std::size_t length = 0;
    const char* text = lua_tolstring(state, 2, &length);
    const std::string_view name{text != nullptr ? text : "", length};
    if (name == "add") {
        lua_pushcfunction(state, lua_add);
        return 1;
    }
    if (name == "move") {
        lua_pushcfunction(state, lua_move);
        return 1;
    }
    if (name == "remove") {
        lua_pushcfunction(state, lua_drop);
        return 1;
    }
    if (name == "grey") {
        lua_pushcfunction(state, lua_grey);
        return 1;
    }
    if (name == "hide") {
        lua_pushcfunction(state, lua_hide);
        return 1;
    }
    if (name == "builds") {
        lua_pushcfunction(state, lua_builds);
        return 1;
    }
    std::string message = "menus has no ";
    message.append(name.data(), name.size());
    return luaL_error(state, "%s", message.c_str());
}

int Menus::lua_newindex(lua_State* state) {
    from_lua(state, 1);
    return luaL_error(state, "a menus collection cannot be assigned");
}

int Menus::lua_add(lua_State* state) {
    host::charge_work(state, host::comparison_unit * static_cast<uint64_t>(32));
    try {
        Menus* menus = from_lua(state, 1);
        if (menus == nullptr)
            return luaL_error(state, "expected a menus collection");
        return menus->state_->add_lua(state);
    } catch (const MenuError& error) {
        lua_pushstring(state, error.what());
    } catch (const std::bad_alloc&) {
        lua_pushstring(state, "out of memory");
    } catch (const std::exception& error) {
        lua_pushstring(state, error.what());
    }
    return lua_error(state);
}

int Menus::lua_move(lua_State* state) {
    host::charge_work(state, host::comparison_unit * static_cast<uint64_t>(32));
    try {
        Menus* menus = from_lua(state, 1);
        if (menus == nullptr)
            return luaL_error(state, "expected a menus collection");
        return menus->state_->move_lua(state);
    } catch (const MenuError& error) {
        lua_pushstring(state, error.what());
    } catch (const std::bad_alloc&) {
        lua_pushstring(state, "out of memory");
    } catch (const std::exception& error) {
        lua_pushstring(state, error.what());
    }
    return lua_error(state);
}

int Menus::lua_drop(lua_State* state) {
    host::charge_work(state, host::comparison_unit * static_cast<uint64_t>(32));
    try {
        Menus* menus = from_lua(state, 1);
        if (menus == nullptr)
            return luaL_error(state, "expected a menus collection");
        return menus->state_->remove_lua(state);
    } catch (const MenuError& error) {
        lua_pushstring(state, error.what());
    } catch (const std::bad_alloc&) {
        lua_pushstring(state, "out of memory");
    } catch (const std::exception& error) {
        lua_pushstring(state, error.what());
    }
    return lua_error(state);
}

int Menus::lua_grey(lua_State* state) {
    host::charge_work(state, host::comparison_unit * static_cast<uint64_t>(32));
    try {
        Menus* menus = from_lua(state, 1);
        if (menus == nullptr)
            return luaL_error(state, "expected a menus collection");
        return menus->state_->grey_lua(state);
    } catch (const MenuError& error) {
        lua_pushstring(state, error.what());
    } catch (const std::bad_alloc&) {
        lua_pushstring(state, "out of memory");
    } catch (const std::exception& error) {
        lua_pushstring(state, error.what());
    }
    return lua_error(state);
}

int Menus::lua_hide(lua_State* state) {
    host::charge_work(state, host::comparison_unit * static_cast<uint64_t>(32));
    try {
        Menus* menus = from_lua(state, 1);
        if (menus == nullptr)
            return luaL_error(state, "expected a menus collection");
        return menus->state_->hide_lua(state);
    } catch (const MenuError& error) {
        lua_pushstring(state, error.what());
    } catch (const std::bad_alloc&) {
        lua_pushstring(state, "out of memory");
    } catch (const std::exception& error) {
        lua_pushstring(state, error.what());
    }
    return lua_error(state);
}

int Menus::lua_builds(lua_State* state) {
    host::charge_work(state, host::comparison_unit * static_cast<uint64_t>(32));
    try {
        Menus* menus = from_lua(state, 1);
        if (menus == nullptr)
            return luaL_error(state, "expected a menus collection");
        return menus->state_->builds_lua(state);
    } catch (const MenuError& error) {
        lua_pushstring(state, error.what());
    } catch (const std::bad_alloc&) {
        lua_pushstring(state, "out of memory");
    } catch (const std::exception& error) {
        lua_pushstring(state, error.what());
    }
    return lua_error(state);
}

} // namespace oa::data::mutators
