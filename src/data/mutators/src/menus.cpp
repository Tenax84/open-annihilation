// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Build menus a mutator hook reads and writes.
//
// Every call reads the download files and the units from the documents as
// they stand when it runs, so a slot an earlier step or an earlier call
// emptied is free, and a unit a hook cloned can be placed. An edit changes
// only the sections and keys it names, in the live document, so another
// mutator's rule or hook writes stay. A slot is decided when the request is
// made.
//
// A Lua callback reads its arguments, resolves names through the units
// index (which charges), computes and edits into this object's working
// storage, charges the work it counted, and only then pushes its result or
// raises. No C++ object with a destructor is alive across a Lua call that
// can raise.
#include "oa/data/mutators/menus.hpp"

#include "oa/data/defs/unit_catalog.hpp"
#include "oa/data/limits.hpp"
#include "oa/data/script_host/script_host.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/formats/tdf/edit.hpp"
#include "oa/formats/tdf/write.hpp"

#include "lua.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace oa::data::mutators {
namespace rules = oa::data::mutation_rules;
namespace tdf = oa::formats::tdf;
namespace defs = oa::data::defs;
namespace host = oa::data::script_host;
namespace limits = oa::data::limits;

/// The build menus reach the collections' journal, units index and clones through here.
struct MenusAccess {
    static rules::DocumentSet& documents(Collections& self) noexcept { return *self.documents_; }

    static bool active(const Collections& self) noexcept { return self.active_; }

    static const std::string& mutator(const Collections& self) noexcept { return self.mutator_; }

    static bool accept_write(Collections& self, rules::FieldWrite write, std::string& reason) {
        return self.accept_write(std::move(write), reason);
    }

    static bool accept_note(Collections& self, HookNote note, std::string& reason) {
        return self.accept_note(std::move(note), reason);
    }

    static int raise(Collections& self, lua_State* state) { return self.raise_held(state); }

    static int lookup(Collections& self, lua_State* state, std::string_view sought) {
        return self.lookup_unit(state, sought);
    }

    static const std::string& looked_up(Collections& self) noexcept {
        return self.side_scratch().text;
    }

    static bool looked_up_builder(Collections& self) noexcept {
        return self.side_scratch().builder_unit;
    }

    static const std::string*
    indexed(Collections& self, std::string_view sought, bool* builder) noexcept {
        return self.indexed_unit(sought, builder);
    }

    /// The private name the calling mutator gave the unit `written`, or null.
    static const std::string* renamed(const Collections& self, std::string_view written) noexcept {
        return self.renamed_to(false, written);
    }

    /// How many renames the calling mutator has noted.
    static std::size_t renames(const Collections& self) noexcept { return self.rename_count(); }

    static std::size_t writes(const Collections& self) noexcept { return self.writes_.size(); }

    static std::vector<rules::FieldWrite>& field_writes(Collections& self) noexcept {
        return self.writes_;
    }

    static std::vector<HookNote>& notes(Collections& self) noexcept { return self.notes_; }

    static std::vector<RecordRemoval>& removals(Collections& self) noexcept {
        return self.removals_;
    }

    static std::span<const CloneRecord> clones(const Collections& self) noexcept {
        return self.clones_;
    }

    static ServedCopy* served(Collections& self, const std::string& folded_path) noexcept {
        const auto found = self.served_index_.find(folded_path);
        if (found == self.served_index_.end() || found->second >= self.served_.size())
            return nullptr;
        return &self.served_[found->second];
    }
};

namespace {

/// The loader reads this many sections of a download file.
constexpr std::size_t k_visible_sections = 5;

/// Player pages the build panel can reach. Page 0 is not a build page.
constexpr int k_max_page = 7;

/// Slots on a download page, counted from 1.
constexpr int k_max_slot = 6;

/// Authored pages probed for one builder. Pages past 7 cannot be reached.
constexpr int k_page_probe_cap = 32;

/// Bit of commonattribs that marks a unit button. The validity pass clears
/// grayedout on every button that still has it.
constexpr unsigned k_unit_button_bit = 0x04u;

/// Units for one call, before what it walks.
constexpr uint64_t k_call_charge = 8;

/// Units for one field read or written, as the unit collections charge it.
constexpr uint64_t k_field_charge = 8;

/// Units for one look at the asset store for a file.
constexpr uint64_t k_probe_charge = 64;

constexpr const char* k_meta_name = "oa.mutators.menus";
constexpr const char* k_page_itself = "the page itself";
constexpr const char* k_page_range = "page must be from 1 to 7";
constexpr const char* k_slot_range = "slot must be from 1 to 6";

/// A request the script cannot make, or a write that could not be made.
struct MenuRefusal : std::runtime_error {
    using std::runtime_error::runtime_error;
};

[[noreturn]] void refuse(std::string message) {
    throw MenuRefusal(message);
}

char fold_byte(char byte) noexcept {
    if (byte >= 'A' && byte <= 'Z')
        return static_cast<char>(byte - 'A' + 'a');
    return byte;
}

char fold_path_byte(char byte) noexcept {
    if (byte == '\\')
        return '/';
    return fold_byte(byte);
}

std::string fold_copy(std::string_view text) {
    std::string folded(text.size(), '\0');
    for (std::size_t index = 0; index < text.size(); ++index)
        folded[index] = fold_byte(text[index]);
    return folded;
}

std::string fold_path(std::string_view path) {
    std::string folded(path.size(), '\0');
    for (std::size_t index = 0; index < path.size(); ++index)
        folded[index] = fold_path_byte(path[index]);
    return folded;
}

bool same_text(std::string_view first, std::string_view second) noexcept {
    if (first.size() != second.size())
        return false;
    for (std::size_t index = 0; index < first.size(); ++index) {
        if (fold_byte(first[index]) != fold_byte(second[index]))
            return false;
    }
    return true;
}

bool same_path(std::string_view first, std::string_view second) noexcept {
    if (first.size() != second.size())
        return false;
    for (std::size_t index = 0; index < first.size(); ++index) {
        if (fold_path_byte(first[index]) != fold_path_byte(second[index]))
            return false;
    }
    return true;
}

/// True when `path` is `<folder>/<name>.<extension>`, with no deeper folder.
bool in_folder(std::string_view path, std::string_view folder, std::string_view extension) {
    if (path.size() <= folder.size() + 1 + extension.size() + 1)
        return false;
    for (std::size_t index = 0; index < folder.size(); ++index) {
        if (fold_path_byte(path[index]) != fold_path_byte(folder[index]))
            return false;
    }
    if (fold_path_byte(path[folder.size()]) != '/')
        return false;
    const std::string_view name = path.substr(folder.size() + 1);
    for (const char byte : name) {
        if (byte == '/' || byte == '\\')
            return false;
    }
    const std::size_t dot = name.size() - extension.size() - 1;
    return name[dot] == '.' && same_text(name.substr(dot + 1), extension);
}

std::string_view file_stem(std::string_view path) noexcept {
    const std::size_t slash = path.find_last_of("/\\");
    std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const std::size_t dot = name.rfind('.');
    if (dot != std::string_view::npos && dot != 0)
        name = name.substr(0, dot);
    return name;
}

std::string_view file_name(std::string_view path) noexcept {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

bool filled(const char* text) noexcept {
    return text != nullptr && text[0] != '\0';
}

std::string_view view_of(const char* text) noexcept {
    return text != nullptr ? std::string_view{text} : std::string_view{};
}

/// A section the loader leaves zeroed: it names no builder and marks nobody.
bool placeholder(const tdf::Block* section) noexcept {
    return section == nullptr || !filled(tdf::find_value(section, "UNITMENU"));
}

uint8_t low_byte(const char* text) noexcept {
    const int32_t parsed = tdf::parse_int(text != nullptr ? text : "0");
    return static_cast<uint8_t>(static_cast<uint32_t>(parsed) & 0xffu);
}

std::string cleared_attribs(const char* text) {
    const auto cleared =
        static_cast<unsigned>(static_cast<uint8_t>(low_byte(text) & ~k_unit_button_bit));
    return std::to_string(cleared);
}

void require_edit(tdf::EditStatus status) {
    if (status == tdf::EditStatus::ok)
        return;
    refuse(std::string{"a build menu could not be written: "} + tdf::edit_status_message(status));
}

/// One button of an authored page. Its identity is its place in the file.
struct Button {
    uint32_t gadget{};
    std::string name;
    bool unit_bit{};
    bool greyed{};
    bool hidden{};
    std::optional<std::string> grayed_text;
    std::optional<std::string> attribs_text;
    std::optional<std::string> active_text;
};

/// Where an authored page is read from, and where a write to it goes.
enum class PageSource : uint8_t {
    store,    ///< the layered store; the first write adds it to the documents
    document, ///< a document of the set
    served,   ///< bytes a clone serves; a write replaces those bytes
};

/// One authored page of a builder: `guis/<builder><number>.GUI`.
struct Page {
    std::string path;
    int number{};
    PageSource source{PageSource::store};
    std::size_t doc{};
    std::string text;
    std::vector<Button> buttons;
};

/// A builder's authored pages: page 0 when present, then 1, 2, … until one is missing.
struct BuilderPages {
    bool stale{};
    std::vector<Page> pages;
};

/// One visible download entry that names a builder. `unit` and `menu`
/// point into the document and are valid until that document is next edited.
struct EntryRef {
    uint32_t doc{};
    uint32_t section{};
    int page{};
    int slot{};
    const char* unit{};
    const char* menu{};
};

/// What the documents looked like when a view was built. Every change a
/// hook can make to a download file or a unit adds a document or a journal
/// entry, so an equal stamp means an unchanged view.
struct Stamp {
    std::size_t documents{std::numeric_limits<std::size_t>::max()};
    std::size_t writes{};
    std::size_t removals{};
    uint64_t turn{};

    bool operator==(const Stamp& other) const noexcept = default;
};

/// The sections of one download file as the menus last left them.
struct Snapshot {
    bool taken{};
    std::vector<const tdf::Block*> sections;
    std::string first_unit; ///< folded unit the first section marks, or empty
};

/// A download file the menus created for one unit's entries.
struct CreatedFile {
    std::string path;
    std::string unit; ///< folded
    uint32_t doc{};
};

/// What `{ page, slot }` or `{ page = "new" }` asked for.
struct Request {
    bool given{};
    bool fresh{};
    int page{};
    int slot{};
    uint8_t problem{}; ///< 0, or 1 for the page, 2 for the slot
};

static_assert(std::is_trivially_destructible_v<Request>);

struct Place {
    int page{};
    int slot{};
};

/// The slots one builder's pages and entries hold.
struct Grid {
    bool authored[k_max_page + 1]{};
    int holder[k_max_page + 1][k_max_slot + 1]{};
    int last{};
};

/// The userdata a script holds. It owns nothing.
struct LuaBox {
    MenusState* owner{};
};

static_assert(std::is_trivially_destructible_v<LuaBox>);

} // namespace

struct MenusState {
    Collections* collections{};
    const oa::AssetStore* store{};
    const defs::DataLayout* layout{};
    bool menus_layer{};

    // Documents of each kind, by index, extended as the set grows.
    std::size_t listed{};
    std::vector<uint32_t> download_docs;
    std::vector<uint32_t> unit_docs;
    std::unordered_map<std::string, uint32_t> gui_docs;

    // Download files as the menus last left them, by document index.
    std::vector<Snapshot> snapshots;
    std::size_t removals_seen{};
    std::size_t turn_removals{};
    std::size_t clones_seen{};

    // Each mutator's turn is numbered, so a view built for an earlier one is rebuilt.
    uint64_t turn{};
    std::size_t indexed_documents{std::numeric_limits<std::size_t>::max()};
    uint64_t indexed_turn{};

    // Visible download entries by builder, and each file's first entry.
    Stamp view_stamp{};
    std::vector<EntryRef> rows;
    std::unordered_map<std::string, std::vector<uint32_t>> builder_rows;
    std::vector<uint32_t> first_rows;

    // Live builders, in document order.
    Stamp builders_stamp{};
    std::vector<std::string> builders;

    // Authored pages, by folded builder name. Cleared at each mutator.
    std::vector<BuilderPages> page_sets;
    std::unordered_map<std::string, std::size_t> page_lookup;

    // Files the menus created, and the mutator that placed each entry.
    std::vector<CreatedFile> created;
    std::vector<MenuFile> added;
    std::map<std::string, std::string> placers;

    // One call's arguments, results and bill.
    std::string builder;
    std::string unit;
    bool has_builder{};
    Request request{};
    Place out{};
    std::vector<std::string> names;
    std::vector<EntryRef> entries;
    std::vector<uint32_t> order;
    std::vector<uint32_t> targets;
    std::vector<std::pair<uint32_t, uint32_t>> found;
    std::vector<std::string> paths;
    std::string error;
    bool failed{};
    bool held{};
    uint64_t charge{};
};

namespace {

using Work = bool (*)(MenusState&);

rules::DocumentSet& documents_of(MenusState& m) noexcept {
    return MenusAccess::documents(*m.collections);
}

const std::string& folder_of(const MenusState& m, defs::DataDirectory which) noexcept {
    return m.layout->directories[static_cast<std::size_t>(which)];
}

const std::string& actor(const MenusState& m) noexcept {
    return MenusAccess::mutator(*m.collections);
}

void bill(MenusState& m, uint64_t units) noexcept {
    m.charge = units > std::numeric_limits<uint64_t>::max() - m.charge
                   ? std::numeric_limits<uint64_t>::max()
                   : m.charge + units;
}

void bill_visits(MenusState& m, uint64_t visits) noexcept {
    if (host::element_charge != 0 &&
        visits > std::numeric_limits<uint64_t>::max() / host::element_charge) {
        bill(m, std::numeric_limits<uint64_t>::max());
        return;
    }
    bill(m, visits * host::element_charge);
}

void bill_text(MenusState& m, std::size_t bytes) noexcept {
    bill(m, host::units_for_bytes(bytes));
}

void bill_name(MenusState& m, const char* text) noexcept {
    if (text != nullptr)
        bill_text(m, std::strlen(text));
}

void set_error(MenusState& m, std::string_view text) noexcept {
    m.failed = true;
    try {
        m.error.assign(text.data(), text.size());
    } catch (...) {
        m.error.clear();
    }
}

/// Runs one piece of C++ work. Any exception becomes the call's error.
bool guarded(MenusState& m, Work work) noexcept {
    bool ok = false;
    try {
        ok = work(m);
    } catch (const std::bad_alloc&) {
        set_error(m, "out of memory");
    } catch (const std::exception& ex) {
        const char* what = ex.what();
        set_error(m, what != nullptr && what[0] != '\0' ? what : "the hook stopped");
    } catch (...) {
        set_error(m, "the hook stopped");
    }
    // The menus' own removals are not a unit removal to repair.
    m.removals_seen = MenusAccess::removals(*m.collections).size();
    return ok && !m.failed;
}

/// Lists the documents added since the last call, by folder.
void refresh_lists(MenusState& m) {
    rules::DocumentSet& docs = documents_of(m);
    const std::size_t total = docs.size();
    if (m.listed < total) {
        const std::string& download = folder_of(m, defs::DataDirectory::download);
        const std::string& units = folder_of(m, defs::DataDirectory::units);
        const std::string& guis = folder_of(m, defs::DataDirectory::guis);
        for (std::size_t index = m.listed; index < total; ++index) {
            if (index > static_cast<std::size_t>(std::numeric_limits<uint32_t>::max()))
                refuse("too many documents");
            const auto narrow = static_cast<uint32_t>(index);
            const std::string& path = docs.at(index).path;
            bill_visits(m, 1);
            bill_text(m, path.size());
            if (in_folder(path, download, "tdf"))
                m.download_docs.push_back(narrow);
            else if (in_folder(path, units, m.layout->unit_extension))
                m.unit_docs.push_back(narrow);
            else if (in_folder(path, guis, "gui"))
                m.gui_docs.emplace(fold_path(path), narrow);
        }
        m.listed = total;
    }
    if (m.snapshots.size() < total)
        m.snapshots.resize(total);
}

/// Charges one search of the units index: the elements a binary search visits, and the name.
void bill_lookup(MenusState& m, std::size_t bytes) noexcept {
    bill_visits(m, static_cast<uint64_t>(std::bit_width(m.unit_docs.size())) + 1u);
    bill_text(m, bytes);
}

/// True when the units index holds `name`.
bool live(MenusState& m, std::string_view name) noexcept {
    bill_lookup(m, name.size());
    return MenusAccess::indexed(*m.collections, name, nullptr) != nullptr;
}

Stamp stamp_of(MenusState& m) noexcept {
    Stamp stamp;
    stamp.documents = documents_of(m).size();
    stamp.writes = MenusAccess::writes(*m.collections);
    stamp.removals = MenusAccess::removals(*m.collections).size();
    stamp.turn = m.turn;
    return stamp;
}

tdf::Block* root_of(MenusState& m, uint32_t doc) {
    rules::LoadedDocument& loaded = documents_of(m).at(doc);
    if (loaded.removed)
        return nullptr;
    return tdf::editable_root(loaded.document.get());
}

// ---------------------------------------------------------------------------
// The journal.

void journal_write(
    MenusState& m,
    std::string_view path,
    std::string_view record,
    std::string_view block,
    std::string_view key,
    std::optional<std::string> old_text,
    std::optional<std::string> new_text
) {
    rules::FieldWrite write;
    write.mutators.push_back(actor(m));
    write.path.assign(path);
    write.record.assign(record);
    write.block.assign(block);
    write.key.assign(key);
    write.old_text = std::move(old_text);
    write.new_text = std::move(new_text);
    std::string reason;
    if (!MenusAccess::accept_write(*m.collections, std::move(write), reason))
        refuse(std::move(reason));
    bill(m, k_field_charge);
}

void journal_note(
    MenusState& m,
    std::vector<std::string> mutators,
    std::string_view path,
    std::string_view record,
    std::string message
) {
    HookNote note;
    note.kind = HookNoteKind::menu_note;
    note.mutators = std::move(mutators);
    note.path.assign(path);
    note.record.assign(record);
    note.message = std::move(message);
    std::string reason;
    if (!MenusAccess::accept_note(*m.collections, std::move(note), reason))
        refuse(std::move(reason));
}

void journal_removal(
    MenusState& m, RemovalKind kind, std::string_view name, std::string_view path
) {
    RecordRemoval removal;
    removal.mutator = actor(m);
    removal.kind = kind;
    removal.name.assign(name);
    removal.path.assign(path);
    MenusAccess::removals(*m.collections).push_back(std::move(removal));
}

// ---------------------------------------------------------------------------
// Download sections.

/// Remembers the order of a download file's sections, and the unit its first section marks.
void take_snapshot(MenusState& m, uint32_t doc) {
    refresh_lists(m);
    Snapshot& snap = m.snapshots[doc];
    snap.taken = true;
    snap.sections.clear();
    snap.first_unit.clear();
    const tdf::Block* root = root_of(m, doc);
    const uint32_t count = tdf::child_count(root);
    bill_visits(m, count);
    for (uint32_t index = 0; index < count; ++index)
        snap.sections.push_back(tdf::child_at(root, index));
    if (count == 0)
        return;
    const tdf::Block* first = tdf::child_at(root, 0);
    const char* unit = tdf::find_value(first, "UNITNAME");
    const char* menu = tdf::find_value(first, "UNITMENU");
    if (filled(unit) && filled(menu) && live(m, menu))
        snap.first_unit = fold_copy(unit);
}

/// Moves the section at `from` to `to`, keeping the order of the others.
void move_section(MenusState& m, uint32_t doc, uint32_t from, uint32_t to) {
    tdf::Block* root = root_of(m, doc);
    if (root == nullptr || from >= root->child_count || to >= root->child_count)
        refuse("a build menu could not be written");
    if (from == to)
        return;
    if (from > to)
        std::rotate(root->children + to, root->children + from, root->children + from + 1);
    else
        std::rotate(root->children + from, root->children + from + 1, root->children + to + 1);
    root->edited = true;
}

/// The first of `base`, `base2`, `base3` … that no section of the file is named.
std::string fresh_section_name(MenusState& m, uint32_t doc, std::string_view base, bool numbered) {
    const tdf::Block* root = root_of(m, doc);
    const uint32_t count = tdf::child_count(root);
    for (int number = 1; number < 100000; ++number) {
        std::string name{base};
        if (numbered || number > 1)
            name += std::to_string(number);
        bool in_use = false;
        bill_visits(m, count);
        for (uint32_t index = 0; index < count && !in_use; ++index) {
            const tdf::Block* section = tdf::child_at(root, index);
            in_use =
                section != nullptr && section->name != nullptr && same_text(section->name, name);
        }
        if (!in_use)
            return name;
    }
    refuse("a build menu could not be written");
}

/// Adds a section named `name` and moves it to `at`. Returns the block.
tdf::Block* insert_section(MenusState& m, uint32_t doc, uint32_t at, std::string_view name) {
    rules::LoadedDocument& loaded = documents_of(m).at(doc);
    tdf::Block* root = tdf::editable_root(loaded.document.get());
    if (root == nullptr)
        refuse("a build menu could not be written");
    tdf::EditStatus status = tdf::EditStatus::ok;
    tdf::Block* made = tdf::add_child(loaded.document.get(), root, name, &status);
    require_edit(status);
    if (made == nullptr)
        refuse("a build menu could not be written");
    move_section(m, doc, root->child_count - 1, at);
    return made;
}

/// Writes one key of a section the menus made, and journals it as new.
void put_new_key(
    MenusState& m,
    uint32_t doc,
    tdf::Block* section,
    std::string_view record,
    std::string_view key,
    std::string_view value
) {
    rules::LoadedDocument& loaded = documents_of(m).at(doc);
    require_edit(tdf::set_value(loaded.document.get(), section, key, value));
    journal_write(m, loaded.path, record, "", key, std::nullopt, std::string{value});
}

/// Puts a placeholder section at `at`. The loader leaves a section whose
/// UNITMENU is empty zeroed: it names no builder, adds no button and marks
/// no unit downloadable. It holds a place among the five sections the loader
/// reads, or the first place, without changing what the file offers.
void write_placeholder(MenusState& m, uint32_t doc, uint32_t at) {
    const std::string name = fresh_section_name(m, doc, "PLACEHOLDER", false);
    tdf::Block* section = insert_section(m, doc, at, name);
    put_new_key(m, doc, section, name, "UNITMENU", "");
    put_new_key(m, doc, section, name, "MENU", "0");
    put_new_key(m, doc, section, name, "BUTTON", "0");
    put_new_key(m, doc, section, name, "UNITNAME", "");
}

/// Takes one section out of a download file and journals it.
void remove_section(MenusState& m, uint32_t doc, uint32_t index) {
    rules::LoadedDocument& loaded = documents_of(m).at(doc);
    tdf::Block* root = tdf::editable_root(loaded.document.get());
    const tdf::Block* section = tdf::child_at(root, index);
    if (section == nullptr)
        refuse("a build menu could not be written");
    const std::string name = section->name != nullptr ? section->name : "";
    require_edit(tdf::remove_child(loaded.document.get(), root, index));
    journal_removal(m, RemovalKind::download_entry, name, loaded.path);
}

/// True when no section of the file names a builder.
bool only_placeholders(MenusState& m, uint32_t doc) {
    const tdf::Block* root = root_of(m, doc);
    const uint32_t count = tdf::child_count(root);
    bill_visits(m, count);
    for (uint32_t index = 0; index < count; ++index) {
        if (!placeholder(tdf::child_at(root, index)))
            return false;
    }
    return true;
}

/// True when `loaded` is an empty document no pass has added or removed.
bool quiet_shell(const rules::LoadedDocument& loaded) noexcept {
    if (loaded.added || loaded.removed || tdf::document_edited(loaded.document.get()))
        return false;
    return tdf::child_count(loaded.document.root()) == 0;
}

/// True when this mutator created the download file and it is still listed as added.
bool created_by_actor(const MenusState& m, std::string_view path) noexcept {
    for (const MenuFile& file : m.added) {
        if (file.mutator == actor(m) && same_path(file.path, path))
            return true;
    }
    return false;
}

/// Drops the journal of a path, and the menus' record that they created it.
void forget_created(MenusState& m, std::string_view path) {
    std::vector<rules::FieldWrite>& writes = MenusAccess::field_writes(*m.collections);
    writes.erase(
        std::remove_if(
            writes.begin(),
            writes.end(),
            [&](const rules::FieldWrite& write) { return same_path(write.path, path); }
        ),
        writes.end()
    );
    std::vector<RecordRemoval>& removals = MenusAccess::removals(*m.collections);
    removals.erase(
        std::remove_if(
            removals.begin(),
            removals.end(),
            [&](const RecordRemoval& removal) { return same_path(removal.path, path); }
        ),
        removals.end()
    );
    std::vector<HookNote>& notes = MenusAccess::notes(*m.collections);
    notes.erase(
        std::remove_if(
            notes.begin(),
            notes.end(),
            [&](const HookNote& note) { return same_path(note.path, path); }
        ),
        notes.end()
    );
    m.created.erase(
        std::remove_if(
            m.created.begin(),
            m.created.end(),
            [&](const CreatedFile& file) { return same_path(file.path, path); }
        ),
        m.created.end()
    );
    m.added.erase(
        std::remove_if(
            m.added.begin(),
            m.added.end(),
            [&](const MenuFile& file) { return same_path(file.path, path); }
        ),
        m.added.end()
    );
}

/// Puts a created file back to the empty document it started as.
///
/// The journal then has no entry for it, and a later entry in this mutator
/// can use the same path. The store never held the path, so nothing is hidden.
void discard_created(MenusState& m, uint32_t doc) {
    rules::LoadedDocument& loaded = documents_of(m).at(doc);
    const std::string path = loaded.path;
    forget_created(m, path);
    if (!loaded.document.parse("\n"))
        refuse("a build menu could not be written");
    loaded.added = false;
    loaded.removed = false;
    take_snapshot(m, doc);
}

/// Hides a download file left with nothing to offer.
///
/// A file this mutator created and then emptied is dropped instead: the
/// journal does not add it and does not hide it.
void hide_document(MenusState& m, uint32_t doc) {
    rules::LoadedDocument& loaded = documents_of(m).at(doc);
    if (loaded.removed)
        return;
    if (created_by_actor(m, loaded.path)) {
        discard_created(m, doc);
        return;
    }
    loaded.removed = true;
    journal_removal(m, RemovalKind::document, file_name(loaded.path), loaded.path);
}

/// Lists every visible download section that names a builder, when the
/// documents changed since the last list.
void ensure_view(MenusState& m) {
    const Stamp now = stamp_of(m);
    bill_visits(m, 1);
    if (now == m.view_stamp)
        return;
    refresh_lists(m);
    m.rows.clear();
    m.builder_rows.clear();
    m.first_rows.clear();
    rules::DocumentSet& docs = documents_of(m);
    for (std::size_t slot = 0; slot < m.download_docs.size(); ++slot) {
        const uint32_t doc = m.download_docs[slot];
        bill_visits(m, 1);
        const rules::LoadedDocument& loaded = docs.at(doc);
        if (loaded.removed)
            continue;
        const tdf::Block* root = loaded.document.root();
        const uint32_t count = tdf::child_count(root);
        const auto shown = static_cast<uint32_t>(std::min<std::size_t>(count, k_visible_sections));
        for (uint32_t index = 0; index < shown; ++index) {
            const tdf::Block* section = tdf::child_at(root, index);
            const char* menu = tdf::find_value(section, "UNITMENU");
            bill_visits(m, 1);
            bill_name(m, menu);
            if (!filled(menu))
                continue;
            EntryRef row;
            row.doc = doc;
            row.section = index;
            // The page a player calls p is written MENU=p+1; BUTTON is the slot minus one.
            row.page = tdf::get_int(section, "MENU", 0) - 1;
            row.slot = tdf::get_int(section, "BUTTON", 0) + 1;
            row.unit = tdf::find_value(section, "UNITNAME");
            row.menu = menu;
            const auto at = static_cast<uint32_t>(m.rows.size());
            m.rows.push_back(row);
            m.builder_rows[fold_copy(menu)].push_back(at);
            if (index == 0)
                m.first_rows.push_back(at);
        }
    }
    m.view_stamp = now;
}

/// Every visible entry whose UNITMENU is `builder`, in file then section order.
void collect_entries(MenusState& m, std::string_view builder) {
    ensure_view(m);
    m.entries.clear();
    bill_lookup(m, builder.size());
    const auto found = m.builder_rows.find(fold_copy(builder));
    if (found == m.builder_rows.end())
        return;
    bill_visits(m, found->second.size());
    for (const uint32_t row : found->second)
        m.entries.push_back(m.rows[row]);
}

/// True when a download file's first section marks `unit` downloadable.
bool marked_now(MenusState& m, std::string_view unit) {
    ensure_view(m);
    bill_visits(m, m.first_rows.size());
    for (const uint32_t at : m.first_rows) {
        const EntryRef& row = m.rows[at];
        if (!filled(row.unit) || !same_text(row.unit, unit))
            continue;
        if (live(m, row.menu))
            return true;
    }
    return false;
}

bool path_taken(MenusState& m, const std::string& path) {
    rules::DocumentSet& docs = documents_of(m);
    bill_visits(m, m.download_docs.size());
    for (const uint32_t doc : m.download_docs) {
        const rules::LoadedDocument& loaded = docs.at(doc);
        if (!same_path(loaded.path, path))
            continue;
        // A file this mutator created and then emptied is free again.
        if (quiet_shell(loaded))
            continue;
        return true;
    }
    bill(m, k_probe_charge);
    return m.store->file_size(path) > 0;
}

/// `download/<unit>.tdf`, or `download/<unit>-<k>.tdf` with the first free k from 2.
std::string fresh_path(MenusState& m, std::string_view unit) {
    std::string base = folder_of(m, defs::DataDirectory::download);
    base += "/";
    base.append(unit.data(), unit.size());
    for (int suffix = 1; suffix < 10000; ++suffix) {
        std::string candidate = base;
        if (suffix > 1) {
            candidate += "-";
            candidate += std::to_string(suffix);
        }
        candidate += ".tdf";
        if (!path_taken(m, candidate))
            return candidate;
    }
    refuse("a build menu could not be written");
}

/// Records `doc` as a download file this mutator created.
void remember_created(MenusState& m, uint32_t doc, std::string_view unit) {
    rules::LoadedDocument& loaded = documents_of(m).at(doc);
    loaded.added = true;
    CreatedFile created;
    created.path = loaded.path;
    created.unit = fold_copy(unit);
    created.doc = doc;
    m.created.push_back(std::move(created));
    MenuFile added;
    added.mutator = actor(m);
    added.path = loaded.path;
    m.added.push_back(std::move(added));
}

/// Adds an empty download file for `unit` and records it as created.
///
/// A path left by a file this mutator created and then emptied is used again.
uint32_t create_file(MenusState& m, std::string_view unit) {
    const std::string path = fresh_path(m, unit);
    rules::DocumentSet& docs = documents_of(m);
    for (std::size_t index = 0; index < docs.size(); ++index) {
        rules::LoadedDocument& loaded = docs.at(index);
        if (!same_path(loaded.path, path) || !quiet_shell(loaded))
            continue;
        if (index > static_cast<std::size_t>(std::numeric_limits<uint32_t>::max()))
            refuse("too many documents");
        const auto doc = static_cast<uint32_t>(index);
        remember_created(m, doc, unit);
        return doc;
    }
    std::string error;
    if (!docs.add(path, "\n", &error))
        refuse(error.empty() ? std::string{"a build menu could not be written"} : error);
    const std::size_t index = docs.size() - 1;
    refresh_lists(m);
    const auto doc = static_cast<uint32_t>(index);
    remember_created(m, doc, unit);
    return doc;
}

/// The file a new entry for `unit` goes into.
///
/// A file the menus made for that unit is reused while it has fewer than
/// five sections. A new file starts with the unit's own entry when the unit
/// is already marked downloadable, and with a placeholder when it is not,
/// so no unit's mark changes.
uint32_t file_for(MenusState& m, std::string_view unit) {
    rules::DocumentSet& docs = documents_of(m);
    bill_visits(m, m.created.size());
    for (const CreatedFile& file : m.created) {
        if (!same_text(file.unit, unit))
            continue;
        const rules::LoadedDocument& loaded = docs.at(file.doc);
        if (loaded.removed)
            continue;
        if (tdf::child_count(loaded.document.root()) < k_visible_sections)
            return file.doc;
    }
    const bool marked = marked_now(m, unit);
    const uint32_t doc = create_file(m, unit);
    if (!marked)
        write_placeholder(m, doc, 0);
    return doc;
}

/// Appends one entry to a download file and journals its keys.
void append_entry(MenusState& m, uint32_t doc, Place place) {
    const std::string name = fresh_section_name(m, doc, "MENUENTRY", true);
    const uint32_t at = tdf::child_count(root_of(m, doc));
    tdf::Block* section = insert_section(m, doc, at, name);
    put_new_key(m, doc, section, name, "UNITMENU", m.builder);
    put_new_key(m, doc, section, name, "MENU", std::to_string(place.page + 1));
    put_new_key(m, doc, section, name, "BUTTON", std::to_string(place.slot - 1));
    put_new_key(m, doc, section, name, "UNITNAME", m.unit);
    take_snapshot(m, doc);
}

/// Gives `unit` back the downloadable mark it lost when one of its entries
/// was taken out, by putting one of its remaining entries first.
///
/// A file whose first section is a placeholder and that shows every section
/// loses the placeholder. Otherwise the entry moves to a new file of its own.
void promote(MenusState& m, std::string_view unit) {
    for (std::size_t slot = 0; slot < m.download_docs.size(); ++slot) {
        const uint32_t doc = m.download_docs[slot];
        bill_visits(m, 1);
        const tdf::Block* root = root_of(m, doc);
        const uint32_t count = tdf::child_count(root);
        if (count < 2 || count > k_visible_sections || !placeholder(tdf::child_at(root, 0)))
            continue;
        for (uint32_t index = 1; index < count; ++index) {
            const tdf::Block* section = tdf::child_at(root, index);
            bill_visits(m, 1);
            if (placeholder(section) ||
                !same_text(view_of(tdf::find_value(section, "UNITNAME")), unit) ||
                !live(m, view_of(tdf::find_value(section, "UNITMENU"))))
                continue;
            remove_section(m, doc, 0);
            move_section(m, doc, index - 1, 0);
            take_snapshot(m, doc);
            return;
        }
    }
    for (std::size_t slot = 0; slot < m.download_docs.size(); ++slot) {
        const uint32_t doc = m.download_docs[slot];
        bill_visits(m, 1);
        const tdf::Block* root = root_of(m, doc);
        const uint32_t count = tdf::child_count(root);
        const auto shown = static_cast<uint32_t>(std::min<std::size_t>(count, k_visible_sections));
        for (uint32_t index = 1; index < shown; ++index) {
            const tdf::Block* section = tdf::child_at(root, index);
            bill_visits(m, 1);
            if (placeholder(section) ||
                !same_text(view_of(tdf::find_value(section, "UNITNAME")), unit) ||
                !live(m, view_of(tdf::find_value(section, "UNITMENU"))))
                continue;
            // Copy the entry's name and keys, in file order, before it goes.
            const std::string name = section->name != nullptr ? section->name : "MENUENTRY1";
            std::vector<const tdf::Property*> keys;
            for (uint32_t key = 0; key < section->property_count; ++key)
                keys.push_back(&section->properties[key]);
            std::sort(
                keys.begin(), keys.end(), [](const tdf::Property* one, const tdf::Property* two) {
                    return one->order < two->order;
                }
            );
            std::vector<std::pair<std::string, std::string>> copied;
            for (const tdf::Property* property : keys) {
                if (property->key != nullptr)
                    copied.emplace_back(property->key, view_of(property->value));
            }
            const std::string spelled{unit};
            const bool deep = count > k_visible_sections;
            remove_section(m, doc, index);
            if (deep)
                write_placeholder(m, doc, index);
            if (only_placeholders(m, doc))
                hide_document(m, doc);
            take_snapshot(m, doc);
            const uint32_t fresh = create_file(m, spelled);
            tdf::Block* made = insert_section(m, fresh, 0, name);
            for (const auto& [key, value] : copied)
                put_new_key(m, fresh, made, name, key, value);
            take_snapshot(m, fresh);
            return;
        }
    }
}

/// When a placeholder holds a file's first place, the removal note must not
/// name a unit as the one the file marks.
void settle_first_mark(MenusState& m, uint32_t doc) {
    const tdf::Block* root = root_of(m, doc);
    if (root == nullptr || tdf::child_count(root) == 0 || !placeholder(tdf::child_at(root, 0)))
        return;
    const std::string& path = documents_of(m).at(doc).path;
    for (HookNote& note : MenusAccess::notes(*m.collections)) {
        if (note.kind != HookNoteKind::download_first_entry_changed || !same_path(note.path, path))
            continue;
        note.message = "a placeholder now holds the first place, so no unit gains a mark";
    }
}

/// Puts placeholders where the hooks' unit removals took visible entries
/// out of a download file, so no section past the fifth becomes visible and
/// no other unit takes the first place, then gives back a lost mark.
void repair(MenusState& m, uint32_t doc) {
    const Snapshot before = m.snapshots[doc];
    const tdf::Block* root = root_of(m, doc);
    if (root == nullptr)
        return;
    const auto present = [&](const tdf::Block* sought) {
        const uint32_t count = tdf::child_count(root);
        bill_visits(m, count);
        for (uint32_t index = 0; index < count; ++index) {
            if (tdf::child_at(root, index) == sought)
                return true;
        }
        return false;
    };
    const bool first_gone = !before.sections.empty() && !present(before.sections[0]);
    if (before.sections.size() > k_visible_sections) {
        for (uint32_t place = 0; place < k_visible_sections; ++place) {
            if (!present(before.sections[place]))
                write_placeholder(m, doc, place);
        }
    } else if (first_gone && tdf::child_count(root) > 0) {
        write_placeholder(m, doc, 0);
    }
    if (tdf::child_count(root_of(m, doc)) > 0 && only_placeholders(m, doc))
        hide_document(m, doc);
    take_snapshot(m, doc);
    if (first_gone && !before.first_unit.empty()) {
        bill_lookup(m, before.first_unit.size());
        const std::string* canonical =
            MenusAccess::indexed(*m.collections, before.first_unit, nullptr);
        if (canonical != nullptr) {
            const std::string spelled = *canonical;
            if (!marked_now(m, spelled))
                promote(m, spelled);
        }
    }
    settle_first_mark(m, doc);
}

/// Repairs the download files a unit removal touched since the menus last looked.
void settle_removals(MenusState& m) {
    std::vector<RecordRemoval>& removals = MenusAccess::removals(*m.collections);
    if (m.removals_seen >= removals.size()) {
        m.removals_seen = removals.size();
        return;
    }
    m.paths.clear();
    for (std::size_t index = m.removals_seen; index < removals.size(); ++index) {
        bill_visits(m, 1);
        if (removals[index].kind != RemovalKind::download_entry)
            continue;
        bool seen = false;
        for (const std::string& path : m.paths)
            seen = seen || same_path(path, removals[index].path);
        if (!seen)
            m.paths.push_back(removals[index].path);
    }
    m.removals_seen = removals.size();
    rules::DocumentSet& docs = documents_of(m);
    // A repair can add a file, so the lists are walked by position.
    for (std::size_t slot = 0; slot < m.paths.size(); ++slot) {
        std::size_t found = m.download_docs.size();
        for (std::size_t place = 0;
             place < m.download_docs.size() && found == m.download_docs.size();
             ++place) {
            bill_visits(m, 1);
            if (same_path(docs.at(m.download_docs[place]).path, m.paths[slot]))
                found = place;
        }
        if (found == m.download_docs.size())
            continue;
        const uint32_t doc = m.download_docs[found];
        if (!docs.at(doc).removed && m.snapshots[doc].taken)
            repair(m, doc);
    }
    m.removals_seen = MenusAccess::removals(*m.collections).size();
}

// ---------------------------------------------------------------------------
// Authored pages.

void read_buttons(const tdf::Block* root, Page& page) {
    const uint32_t count = tdf::child_count(root);
    for (uint32_t index = 0; index < count; ++index) {
        const tdf::Block* gadget = tdf::child_at(root, index);
        const tdf::Block* common = gadget != nullptr ? tdf::find_child(gadget, "COMMON") : nullptr;
        Button button;
        button.gadget = index;
        button.name.assign(view_of(common != nullptr ? tdf::find_value(common, "name") : nullptr));
        const char* attribs =
            common != nullptr ? tdf::find_value(common, "commonattribs") : nullptr;
        const char* grayed = gadget != nullptr ? tdf::find_value(gadget, "grayedout") : nullptr;
        const char* active = common != nullptr ? tdf::find_value(common, "active") : nullptr;
        if (attribs != nullptr)
            button.attribs_text = attribs;
        if (grayed != nullptr)
            button.grayed_text = grayed;
        if (active != nullptr)
            button.active_text = active;
        button.unit_bit = attribs != nullptr && (low_byte(attribs) & k_unit_button_bit) != 0;
        // A missing active is not drawn. grayedout sticks only when the unit bit is clear.
        button.hidden = active == nullptr || tdf::parse_int(active) == 0;
        button.greyed = grayed != nullptr && (tdf::parse_int(grayed) & 1) != 0 && !button.unit_bit;
        page.buttons.push_back(std::move(button));
    }
}

/// Reads one authored page: a document of the set, else the copy a clone
/// serves, else the store. False when the page is missing or empty.
bool load_page(MenusState& m, std::string path, int number, Page& page) {
    page.path = std::move(path);
    page.number = number;
    const std::string folded = fold_path(page.path);
    bill_text(m, page.path.size());
    rules::DocumentSet& docs = documents_of(m);
    const auto listed = m.gui_docs.find(folded);
    if (listed != m.gui_docs.end() && !docs.at(listed->second).removed) {
        const rules::LoadedDocument& loaded = docs.at(listed->second);
        const tdf::Block* root = loaded.document.root();
        if (tdf::child_count(root) == 0)
            return false;
        page.source = PageSource::document;
        page.doc = listed->second;
        bill_text(m, loaded.document.get()->text_length);
        bill_visits(m, tdf::child_count(root));
        read_buttons(root, page);
        return true;
    }
    std::string text;
    if (const ServedCopy* copy = MenusAccess::served(*m.collections, folded)) {
        if (copy->bytes == nullptr || copy->bytes->empty())
            return false;
        text.assign(copy->bytes->begin(), copy->bytes->end());
        page.source = PageSource::served;
    } else {
        bill(m, k_probe_charge);
        if (m.store->file_size(page.path) == 0)
            return false;
        try {
            const oa::AssetData data = m.store->read(page.path);
            text.assign(data.bytes.begin(), data.bytes.end());
        } catch (const std::bad_alloc&) {
            throw;
        } catch (const std::exception&) {
            return false;
        }
        if (text.empty())
            return false;
        page.source = PageSource::store;
    }
    bill_text(m, text.size());
    tdf::OwnedDocument parsed;
    // A page that does not parse still holds its slots. It has no buttons.
    if (parsed.parse(text)) {
        bill_visits(m, tdf::child_count(parsed.root()));
        read_buttons(parsed.root(), page);
    }
    if (page.source == PageSource::store)
        page.text = std::move(text);
    return true;
}

/// A builder's authored pages, read once per mutator. Returns an index into `page_sets`.
std::size_t pages_of(MenusState& m, std::string_view builder) {
    bill_lookup(m, builder.size());
    const std::string key = fold_copy(builder);
    const auto found = m.page_lookup.find(key);
    std::size_t slot = m.page_sets.size();
    if (found != m.page_lookup.end()) {
        if (!m.page_sets[found->second].stale)
            return found->second;
        slot = found->second;
    } else {
        m.page_sets.emplace_back();
        m.page_lookup.emplace(key, slot);
    }
    std::vector<Page> pages;
    const std::string& folder = folder_of(m, defs::DataDirectory::guis);
    for (int number = 0; number <= k_page_probe_cap; ++number) {
        std::string path = folder;
        path += "/";
        path.append(builder.data(), builder.size());
        path += std::to_string(number);
        path += ".GUI";
        Page page;
        if (!load_page(m, std::move(path), number, page)) {
            if (number == 0)
                continue;
            break;
        }
        pages.push_back(std::move(page));
    }
    m.page_sets[slot].pages = std::move(pages);
    m.page_sets[slot].stale = false;
    return slot;
}

/// Sets one key of a button, in the document a write to that page goes to.
void set_button_key(
    MenusState& m,
    Page& page,
    uint32_t gadget,
    bool in_common,
    const char* key,
    std::string_view value
) {
    bill(m, k_field_charge);
    if (page.source == PageSource::served) {
        ServedCopy* copy = MenusAccess::served(*m.collections, fold_path(page.path));
        if (copy == nullptr || copy->bytes == nullptr)
            refuse("a build menu could not be written");
        tdf::OwnedDocument parsed;
        if (!parsed.parse(
                std::string_view{
                    reinterpret_cast<const char*>(copy->bytes->data()), copy->bytes->size()
                }
            ))
            refuse("a build menu could not be written");
        tdf::Block* root = tdf::editable_root(parsed.get());
        tdf::Block* target = tdf::editable_child(root, gadget);
        if (target != nullptr && in_common)
            target = tdf::editable_child(target, "COMMON");
        if (target == nullptr)
            refuse("a build menu could not be written");
        require_edit(tdf::set_value(parsed.get(), target, key, value));
        const std::optional<std::string> text = tdf::write_text(parsed.get());
        if (!text.has_value())
            refuse("a build menu could not be written");
        bill_text(m, text->size());
        copy->bytes = std::make_shared<const std::vector<uint8_t>>(text->begin(), text->end());
        return;
    }
    rules::DocumentSet& docs = documents_of(m);
    if (page.source == PageSource::store) {
        std::string error;
        if (!docs.add(page.path, page.text, &error))
            refuse(error.empty() ? std::string{"a build menu could not be written"} : error);
        page.doc = docs.size() - 1;
        page.source = PageSource::document;
        page.text.clear();
        refresh_lists(m);
    }
    rules::LoadedDocument& loaded = docs.at(page.doc);
    tdf::Block* root = tdf::editable_root(loaded.document.get());
    tdf::Block* target = tdf::editable_child(root, gadget);
    if (target != nullptr && in_common)
        target = tdf::editable_child(target, "COMMON");
    if (target == nullptr)
        refuse("a build menu could not be written");
    require_edit(tdf::set_value(loaded.document.get(), target, key, value));
}

/// Greys a button for good: grayedout=1 at the gadget level and bit 0x04 of
/// commonattribs cleared, so the build page's validity pass leaves it greyed.
void grey_button(MenusState& m, Page& page, Button& button, std::string_view record) {
    const bool grayed_one = button.grayed_text.has_value() && *button.grayed_text == "1";
    if (!grayed_one) {
        set_button_key(m, page, button.gadget, false, "grayedout", "1");
        journal_write(m, page.path, record, "", "grayedout", button.grayed_text, std::string{"1"});
        button.grayed_text = "1";
    }
    if (button.unit_bit) {
        const std::string written = cleared_attribs(
            button.attribs_text.has_value() ? button.attribs_text->c_str() : nullptr
        );
        set_button_key(m, page, button.gadget, true, "commonattribs", written);
        journal_write(
            m, page.path, record, "COMMON", "commonattribs", button.attribs_text, written
        );
        button.attribs_text = written;
        button.unit_bit = false;
    }
    button.greyed = true;
}

/// Hides a button: active=0 in its [COMMON].
void hide_button(MenusState& m, Page& page, Button& button, std::string_view record) {
    button.hidden = true;
    if (!button.active_text.has_value() || *button.active_text == "0")
        return;
    set_button_key(m, page, button.gadget, true, "active", "0");
    journal_write(m, page.path, record, "COMMON", "active", button.active_text, std::string{"0"});
    button.active_text = "0";
}

/// Greys every button named `unit` on one builder's pages.
void grey_on(MenusState& m, std::size_t set, std::string_view unit) {
    for (std::size_t page = 0; page < m.page_sets[set].pages.size(); ++page) {
        for (std::size_t index = 0; index < m.page_sets[set].pages[page].buttons.size(); ++index) {
            Button& button = m.page_sets[set].pages[page].buttons[index];
            bill_visits(m, 1);
            bill_text(m, button.name.size());
            if (same_text(button.name, unit))
                grey_button(m, m.page_sets[set].pages[page], button, unit);
        }
    }
}

/// Lists the live builders, when the documents changed since the last list.
void ensure_builders(MenusState& m) {
    const Stamp now = stamp_of(m);
    bill_visits(m, 1);
    if (now == m.builders_stamp)
        return;
    refresh_lists(m);
    m.builders.clear();
    rules::DocumentSet& docs = documents_of(m);
    for (std::size_t slot = 0; slot < m.unit_docs.size(); ++slot) {
        bill_visits(m, 1);
        const rules::LoadedDocument& loaded = docs.at(m.unit_docs[slot]);
        if (loaded.removed)
            continue;
        const tdf::Block* root = loaded.document.root();
        const tdf::Block* info = nullptr;
        const uint32_t count = tdf::child_count(root);
        for (uint32_t index = 0; index < count && info == nullptr; ++index) {
            const tdf::Block* block = tdf::child_at(root, index);
            if (block != nullptr && block->name != nullptr &&
                tdf::compare_nocase(block->name, "UNITINFO") == 0)
                info = block;
        }
        bill_visits(m, count);
        if (info == nullptr || tdf::get_int(info, "builder", 0) == 0)
            continue;
        const char* named = tdf::find_value(info, "UnitName");
        m.builders.emplace_back(filled(named) ? std::string_view{named} : file_stem(loaded.path));
        bill_text(m, m.builders.back().size());
    }
    m.builders_stamp = now;
}

/// Greys every button named `unit` on every builder's pages.
void grey_everywhere(MenusState& m, std::string_view unit) {
    ensure_builders(m);
    // A grey below adds a journal entry, but the list is not rebuilt until the next call.
    for (std::size_t slot = 0; slot < m.builders.size(); ++slot)
        grey_on(m, pages_of(m, m.builders[slot]), unit);
}

// ---------------------------------------------------------------------------
// Slots.

Grid make_grid(const MenusState& m, std::size_t set, std::size_t skip) {
    Grid grid;
    for (auto& row : grid.holder) {
        for (int& cell : row)
            cell = -1;
    }
    for (const Page& page : m.page_sets[set].pages) {
        if (page.number >= 1 && page.number <= k_max_page)
            grid.authored[page.number] = true;
        grid.last = std::max(grid.last, page.number);
    }
    for (std::size_t index = 0; index < m.entries.size(); ++index) {
        if (index == skip)
            continue;
        const EntryRef& entry = m.entries[index];
        if (entry.page >= 1)
            grid.last = std::max(grid.last, entry.page);
        if (entry.page < 1 || entry.page > k_max_page || entry.slot < 1 || entry.slot > k_max_slot)
            continue;
        if (grid.holder[entry.page][entry.slot] < 0)
            grid.holder[entry.page][entry.slot] = static_cast<int>(index);
    }
    return grid;
}

bool taken(const Grid& grid, Place place) noexcept {
    return grid.authored[place.page] || grid.holder[place.page][place.slot] >= 0;
}

bool next_free(const Grid& grid, Place& place) noexcept {
    for (int page = 1; page <= k_max_page; ++page) {
        for (int slot = 1; slot <= k_max_slot; ++slot) {
            if (taken(grid, Place{page, slot}))
                continue;
            place = Place{page, slot};
            return true;
        }
    }
    return false;
}

std::string placer_key(std::string_view builder, std::string_view unit) {
    std::string key = fold_copy(builder);
    key += '\n';
    key += fold_copy(unit);
    return key;
}

/// Who holds a slot: the mutator that placed the entry there, or the page itself.
std::string occupant(const MenusState& m, const Grid& grid, Place place) {
    if (grid.authored[place.page])
        return k_page_itself;
    const int holder = grid.holder[place.page][place.slot];
    if (holder < 0)
        return k_page_itself;
    const auto found = m.placers.find(
        placer_key(m.builder, view_of(m.entries[static_cast<std::size_t>(holder)].unit))
    );
    if (found == m.placers.end())
        return k_page_itself;
    return found->second;
}

void note_collision(
    MenusState& m, Place from, Place to, const std::string& who, std::string_view path
) {
    std::vector<std::string> mutators;
    if (who != k_page_itself)
        mutators.push_back(who);
    if (who != actor(m))
        mutators.push_back(actor(m));
    std::string message = m.unit;
    message += " moved from page ";
    message += std::to_string(from.page);
    message += " slot ";
    message += std::to_string(from.slot);
    message += " to page ";
    message += std::to_string(to.page);
    message += " slot ";
    message += std::to_string(to.slot);
    message += ": taken by ";
    message += who;
    journal_note(m, std::move(mutators), path, m.unit, std::move(message));
}

void note_art(MenusState& m, std::string_view path) {
    std::string art = "anims/";
    art += m.unit;
    art += "_gadget.gaf";
    bill(m, k_probe_charge);
    if (m.store->file_size(art) > 0)
        return;
    std::string message = m.unit;
    message += "'s button has no picture on ";
    message += m.builder;
    message += ": no anims/";
    message += m.unit;
    message += "_gadget.gaf";
    journal_note(m, {actor(m)}, path, m.unit, std::move(message));
}

/// The builder's [CANBUILD] entries in sidedata that name a unit.
uint32_t canbuild_count(MenusState& m) {
    std::string path = folder_of(m, defs::DataDirectory::gamedata);
    path += "/sidedata.tdf";
    rules::DocumentSet& docs = documents_of(m);
    bill_visits(m, docs.size());
    const rules::LoadedDocument* loaded = docs.find(path);
    if (loaded == nullptr || loaded->removed)
        return 0;
    const tdf::Block* root = loaded->document.root();
    const tdf::Block* lists = root != nullptr ? tdf::find_child(root, "CANBUILD") : nullptr;
    const tdf::Block* block =
        lists != nullptr ? tdf::find_child(lists, m.builder.c_str()) : nullptr;
    if (block == nullptr)
        return 0;
    const uint32_t kept = limits::build_list_kept({});
    uint32_t count = 0;
    for (uint32_t number = 1; number <= kept; ++number) {
        const std::string key = "canbuild" + std::to_string(number);
        const char* value = tdf::find_value(block, key.c_str());
        bill_visits(m, 1);
        if (value == nullptr)
            break;
        if (live(m, value))
            ++count;
    }
    return count;
}

/// Notes an entry the AI's list cannot hold: the builder's [CANBUILD] names
/// and its entries in the files before the new one fill it.
void note_full_list(MenusState& m, uint32_t doc, std::string_view path) {
    const uint32_t limit = defs::download_build_list_entries({});
    uint32_t seen = canbuild_count(m);
    for (const EntryRef& entry : m.entries) {
        if (entry.doc > doc)
            break;
        if (filled(entry.unit) && live(m, entry.unit))
            ++seen;
    }
    if (seen < limit)
        return;
    std::string message = "the AI does not see ";
    message += m.unit;
    message += " on ";
    message += m.builder;
    message += ": ";
    message += m.builder;
    message += "'s list is full";
    journal_note(m, {actor(m)}, path, m.unit, std::move(message));
}

// ---------------------------------------------------------------------------
// The calls.

/// Brings the lists, the page cache and the download files up to date.
void begin_work(MenusState& m) {
    bill(m, k_call_charge);
    refresh_lists(m);
    const std::span<const CloneRecord> clones = MenusAccess::clones(*m.collections);
    for (std::size_t index = m.clones_seen; index < clones.size(); ++index) {
        if (clones[index].weapon)
            continue;
        const auto found = m.page_lookup.find(fold_copy(clones[index].name));
        if (found != m.page_lookup.end())
            m.page_sets[found->second].stale = true;
    }
    m.clones_seen = clones.size();
    settle_removals(m);
}

/// The place a builder already offers a unit at: a button of that name on
/// its pages, else one of its entries. False when it offers none.
bool offered(MenusState& m, std::size_t set, Place& place) {
    for (const Page& page : m.page_sets[set].pages) {
        for (const Button& button : page.buttons) {
            bill_visits(m, 1);
            bill_text(m, button.name.size());
            if (!same_text(button.name, m.unit))
                continue;
            place.page = page.number;
            place.slot = button.gadget >= 4 ? static_cast<int>(button.gadget) - 3 : 1;
            return true;
        }
    }
    for (const EntryRef& entry : m.entries) {
        bill_visits(m, 1);
        if (!same_text(view_of(entry.unit), m.unit))
            continue;
        place.page = entry.page;
        place.slot = entry.slot;
        return true;
    }
    return false;
}

bool work_add(MenusState& m) {
    begin_work(m);
    const std::size_t set = pages_of(m, m.builder);
    collect_entries(m, m.builder);
    if (offered(m, set, m.out))
        return true;
    const Grid grid = make_grid(m, set, std::numeric_limits<std::size_t>::max());
    Place target;
    Place asked;
    std::string who;
    bool moved = false;
    if (!m.request.given) {
        if (!next_free(grid, target))
            refuse(m.builder + " has no free build slot on pages 1 to 7");
    } else if (m.request.fresh) {
        target = Place{grid.last + 1, 1};
        if (target.page < 1 || target.page > k_max_page)
            refuse(m.builder + " has no free build slot on pages 1 to 7");
    } else {
        target = Place{m.request.page, m.request.slot};
        if (taken(grid, target)) {
            asked = target;
            who = occupant(m, grid, target);
            if (!next_free(grid, target))
                refuse(m.builder + " has no free build slot on pages 1 to 7");
            moved = true;
        }
    }
    const uint32_t doc = file_for(m, m.unit);
    const std::string path = documents_of(m).at(doc).path;
    if (moved)
        note_collision(m, asked, target, who, path);
    note_full_list(m, doc, path);
    append_entry(m, doc, target);
    note_art(m, path);
    m.placers.insert_or_assign(placer_key(m.builder, m.unit), actor(m));
    m.out = target;
    return true;
}

bool work_move(MenusState& m) {
    begin_work(m);
    const std::size_t set = pages_of(m, m.builder);
    for (const Page& page : m.page_sets[set].pages) {
        for (const Button& button : page.buttons) {
            bill_visits(m, 1);
            if (same_text(button.name, m.unit))
                refuse(
                    m.unit + "'s button is part of " + m.builder + "'s page " +
                    std::to_string(page.number) + "; grey or hide it and add an entry instead"
                );
        }
    }
    collect_entries(m, m.builder);
    std::size_t mine = m.entries.size();
    for (std::size_t index = 0; index < m.entries.size() && mine == m.entries.size(); ++index) {
        if (same_text(view_of(m.entries[index].unit), m.unit))
            mine = index;
    }
    if (mine == m.entries.size())
        refuse(m.builder + " does not build " + m.unit);
    const EntryRef entry = m.entries[mine];
    const Grid grid = make_grid(m, set, mine);
    Place target{m.request.page, m.request.slot};
    rules::LoadedDocument& loaded = documents_of(m).at(entry.doc);
    if (taken(grid, target)) {
        const Place asked = target;
        const std::string who = occupant(m, grid, target);
        if (!next_free(grid, target))
            refuse(m.builder + " has no free build slot on pages 1 to 7");
        note_collision(m, asked, target, who, loaded.path);
    }
    m.out = target;
    if (target.page == entry.page && target.slot == entry.slot)
        return true;
    tdf::Block* root = tdf::editable_root(loaded.document.get());
    tdf::Block* section = tdf::editable_child(root, entry.section);
    if (section == nullptr)
        refuse("a build menu could not be written");
    const std::string record = section->name != nullptr ? section->name : "";
    const std::pair<const char*, std::string> keys[] = {
        {"MENU", std::to_string(target.page + 1)}, {"BUTTON", std::to_string(target.slot - 1)}
    };
    for (const auto& [key, value] : keys) {
        const char* now = tdf::find_value(section, key);
        if (now != nullptr && value == now)
            continue;
        std::optional<std::string> old_text;
        if (now != nullptr)
            old_text = now;
        require_edit(tdf::set_value(loaded.document.get(), section, key, value));
        journal_write(m, loaded.path, record, "", key, std::move(old_text), value);
    }
    m.placers.insert_or_assign(placer_key(m.builder, m.unit), actor(m));
    return true;
}

/// Takes out the targeted visible sections of one file, last first.
///
/// In a file of more than five sections a placeholder takes each removed
/// entry's place, so no section past the fifth is read. When the first
/// section goes, another entry of the same unit in the file moves first;
/// failing that a placeholder takes the first place, so no other unit
/// becomes marked downloadable.
void drop_targets(MenusState& m, uint32_t doc) {
    const bool deep = tdf::child_count(root_of(m, doc)) > k_visible_sections;
    for (std::size_t index = m.targets.size(); index > 0; --index) {
        const uint32_t target = m.targets[index - 1];
        if (target != 0) {
            remove_section(m, doc, target);
            if (deep)
                write_placeholder(m, doc, target);
            continue;
        }
        const tdf::Block* root = root_of(m, doc);
        const uint32_t count = tdf::child_count(root);
        const auto shown = static_cast<uint32_t>(std::min<std::size_t>(count, k_visible_sections));
        uint32_t other = 0;
        for (uint32_t place = 1; place < shown && other == 0; ++place) {
            const tdf::Block* section = tdf::child_at(root, place);
            bill_visits(m, 1);
            if (!placeholder(section) &&
                same_text(view_of(tdf::find_value(section, "UNITNAME")), m.unit) &&
                live(m, view_of(tdf::find_value(section, "UNITMENU"))))
                other = place;
        }
        remove_section(m, doc, 0);
        if (other != 0) {
            move_section(m, doc, other - 1, 0);
            if (deep)
                write_placeholder(m, doc, other);
        } else if (tdf::child_count(root_of(m, doc)) > 0) {
            write_placeholder(m, doc, 0);
        }
    }
    if (only_placeholders(m, doc))
        hide_document(m, doc);
    take_snapshot(m, doc);
    settle_first_mark(m, doc);
}

bool work_remove(MenusState& m) {
    begin_work(m);
    const bool marked_before = marked_now(m, m.unit);
    // The targets are taken before any file changes, in file then section order.
    ensure_view(m);
    m.found.clear();
    bill_visits(m, m.rows.size());
    for (const EntryRef& row : m.rows) {
        if (!filled(row.unit) || !same_text(row.unit, m.unit))
            continue;
        if (m.has_builder && !same_text(row.menu, m.builder))
            continue;
        m.found.emplace_back(row.doc, row.section);
    }
    for (std::size_t first = 0; first < m.found.size();) {
        const uint32_t doc = m.found[first].first;
        m.targets.clear();
        std::size_t next = first;
        for (; next < m.found.size() && m.found[next].first == doc; ++next)
            m.targets.push_back(m.found[next].second);
        drop_targets(m, doc);
        first = next;
    }
    if (m.has_builder)
        grey_on(m, pages_of(m, m.builder), m.unit);
    else
        grey_everywhere(m, m.unit);
    if (marked_before && !marked_now(m, m.unit))
        promote(m, m.unit);
    return true;
}

bool work_style(MenusState& m, bool hide) {
    begin_work(m);
    const std::size_t set = pages_of(m, m.builder);
    bool on_page = false;
    for (std::size_t page = 0; page < m.page_sets[set].pages.size(); ++page) {
        for (std::size_t index = 0; index < m.page_sets[set].pages[page].buttons.size(); ++index) {
            Button& button = m.page_sets[set].pages[page].buttons[index];
            bill_visits(m, 1);
            if (!same_text(button.name, m.unit))
                continue;
            on_page = true;
            if (hide)
                hide_button(m, m.page_sets[set].pages[page], button, m.unit);
            else
                grey_button(m, m.page_sets[set].pages[page], button, m.unit);
        }
    }
    if (on_page)
        return true;
    collect_entries(m, m.builder);
    for (const EntryRef& entry : m.entries) {
        if (same_text(view_of(entry.unit), m.unit))
            refuse(m.unit + " is a download entry on " + m.builder + "; use menus:remove");
    }
    refuse(m.builder + " does not build " + m.unit);
}

bool work_grey(MenusState& m) {
    return work_style(m, false);
}

bool work_hide(MenusState& m) {
    return work_style(m, true);
}

/// Adds a name to the result once, as the units index spells it, when that unit is there.
void add_name(MenusState& m, std::string_view name) {
    bill_lookup(m, name.size());
    const std::string* canonical = MenusAccess::indexed(*m.collections, name, nullptr);
    if (canonical == nullptr)
        return;
    bill_visits(m, m.names.size());
    for (const std::string& kept : m.names) {
        if (same_text(kept, *canonical))
            return;
    }
    m.names.push_back(*canonical);
}

bool work_builds(MenusState& m) {
    begin_work(m);
    m.names.clear();
    const std::size_t set = pages_of(m, m.builder);
    for (const Page& page : m.page_sets[set].pages) {
        for (const Button& button : page.buttons) {
            bill_visits(m, 1);
            if (!button.unit_bit || button.hidden)
                continue;
            add_name(m, button.name);
        }
    }
    collect_entries(m, m.builder);
    m.order.clear();
    for (std::size_t index = 0; index < m.entries.size(); ++index)
        m.order.push_back(static_cast<uint32_t>(index));
    std::stable_sort(m.order.begin(), m.order.end(), [&m](uint32_t one, uint32_t two) {
        const EntryRef& first = m.entries[one];
        const EntryRef& second = m.entries[two];
        if (first.page != second.page)
            return first.page < second.page;
        return first.slot < second.slot;
    });
    bill_visits(m, m.order.size());
    for (const uint32_t index : m.order) {
        if (filled(m.entries[index].unit))
            add_name(m, m.entries[index].unit);
    }
    // The result table: one element and the bytes of each name.
    bill_visits(m, m.names.size());
    for (const std::string& name : m.names)
        bill_text(m, name.size());
    return true;
}

bool work_end(MenusState& m) {
    MenusAccess::lookup(*m.collections, nullptr, "");
    m.indexed_documents = documents_of(m).size();
    m.indexed_turn = m.turn;
    begin_work(m);
    const std::vector<RecordRemoval>& removals = MenusAccess::removals(*m.collections);
    m.names.clear();
    for (std::size_t index = m.turn_removals; index < removals.size(); ++index) {
        if (removals[index].kind == RemovalKind::unit)
            m.names.push_back(removals[index].name);
    }
    for (std::size_t index = 0; index < m.names.size(); ++index) {
        const std::string unit = m.names[index];
        grey_everywhere(m, unit);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Lua. Nothing below keeps a C++ object with a destructor alive across a Lua
// call that can raise.

void begin_call(MenusState& m) noexcept {
    m.failed = false;
    m.held = false;
    m.error.clear();
    m.charge = 0;
    m.has_builder = false;
    m.request = Request{};
    m.out = Place{};
}

/// Moves the counted work onto the script's allowance. May raise.
void settle(lua_State* state, MenusState& m) {
    const uint64_t units = m.charge;
    m.charge = 0;
    host::charge_work(state, units);
}

int raise(lua_State* state, MenusState& m) {
    if (m.held) {
        m.held = false;
        return MenusAccess::raise(*m.collections, state);
    }
    if (m.error.empty())
        lua_pushliteral(state, "the hook stopped");
    else
        lua_pushlstring(state, m.error.data(), m.error.size());
    return lua_error(state);
}

int raise_text(lua_State* state, const char* message) {
    lua_pushstring(state, message);
    return lua_error(state);
}

MenusState* menus_at(lua_State* state) {
    auto* box = static_cast<LuaBox*>(luaL_testudata(state, 1, k_meta_name));
    return box != nullptr ? box->owner : nullptr;
}

/// The text of a string argument, or null when the value is not a string.
const char* string_arg(lua_State* state, int index, std::size_t& length) {
    length = 0;
    if (lua_type(state, index) != LUA_TSTRING)
        return nullptr;
    return lua_tolstring(state, index, &length);
}

/// Reads `{ page, slot }` or `{ page = "new" }` by raw access.
void read_request(lua_State* state, int index, bool allow_fresh, Request& request) {
    request.given = true;
    if (!lua_istable(state, index)) {
        request.problem = 1;
        return;
    }
    const int table = lua_absindex(state, index);
    lua_pushliteral(state, "page");
    lua_rawget(state, table);
    if (lua_type(state, -1) == LUA_TSTRING) {
        std::size_t length = 0;
        const char* text = lua_tolstring(state, -1, &length);
        const bool fresh = text != nullptr && same_text(std::string_view{text, length}, "new");
        lua_pop(state, 1);
        if (!allow_fresh || !fresh) {
            request.problem = 1;
            return;
        }
        request.fresh = true;
        return;
    }
    if (lua_isinteger(state, -1) == 0) {
        lua_pop(state, 1);
        request.problem = 1;
        return;
    }
    const lua_Integer page = lua_tointeger(state, -1);
    lua_pop(state, 1);
    if (page < 1 || page > k_max_page) {
        request.problem = 1;
        return;
    }
    lua_pushliteral(state, "slot");
    lua_rawget(state, table);
    if (lua_isinteger(state, -1) == 0) {
        lua_pop(state, 1);
        request.problem = 2;
        return;
    }
    const lua_Integer slot = lua_tointeger(state, -1);
    lua_pop(state, 1);
    if (slot < 1 || slot > k_max_slot) {
        request.problem = 2;
        return;
    }
    request.page = static_cast<int>(page);
    request.slot = static_cast<int>(slot);
}

/// Why this call may not write, or null when it may.
const char* write_refusal(const MenusState& m) noexcept {
    if (!MenusAccess::active(*m.collections))
        return "no mutator has begun";
    if (!m.menus_layer)
        return "this mutator does not declare the menus layer";
    return nullptr;
}

/// Keeps the name the units index found, or the error that it found none.
bool take_name(
    MenusState& m, const char* text, std::size_t length, int found, bool builder
) noexcept {
    try {
        if (found == 0) {
            m.error = builder ? "unknown builder " : "unknown unit ";
            m.error.append(text, length);
            m.failed = true;
            return false;
        }
        const std::string& canonical = MenusAccess::looked_up(*m.collections);
        if (!builder) {
            m.unit = canonical;
            return true;
        }
        if (!MenusAccess::looked_up_builder(*m.collections)) {
            m.error = "unknown builder " + canonical;
            m.failed = true;
            return false;
        }
        m.builder = canonical;
        m.has_builder = true;
        return true;
    } catch (...) {
        set_error(m, "out of memory");
        return false;
    }
}

/// Finds a name in the units index as it stands, and keeps it or the error.
///
/// A unit or builder this mutator renamed is looked up by its private name,
/// as `lookup` finds it, and the walk of the rename list is charged. A
/// name that is not found is reported as the script wrote it.
bool take_indexed(MenusState& m, const char* text, std::size_t length, bool builder) noexcept {
    const std::string_view written{text, length};
    bill_visits(m, MenusAccess::renames(*m.collections));
    const std::string* renamed = MenusAccess::renamed(*m.collections, written);
    const std::string_view sought = renamed != nullptr ? std::string_view{*renamed} : written;
    bill_lookup(m, sought.size());
    bool is_builder = false;
    const std::string* canonical =
        MenusAccess::indexed(*m.collections, sought, builder ? &is_builder : nullptr);
    try {
        if (canonical == nullptr) {
            m.error = builder ? "unknown builder " : "unknown unit ";
            m.error.append(written);
            m.failed = true;
            return false;
        }
        if (!builder) {
            m.unit = *canonical;
            return true;
        }
        if (!is_builder) {
            m.error = "unknown builder " + *canonical;
            m.failed = true;
            return false;
        }
        m.builder = *canonical;
        m.has_builder = true;
        return true;
    } catch (...) {
        set_error(m, "out of memory");
        return false;
    }
}

/// Resolves a builder or a unit through the units index.
///
/// The index is brought up to date, which charges and may raise, only when
/// the document set grew or a new mutator's turn began since the last time.
/// Otherwise it is searched as it stands: a removal updates it in place.
/// Either way a name the calling mutator renamed in Add mode means its
/// private record. Names read from the documents are never mapped.
bool resolve(lua_State* state, MenusState& m, const char* text, std::size_t length, bool builder) {
    const std::size_t documents = documents_of(m).size();
    if (m.indexed_documents == documents && m.indexed_turn == m.turn)
        return take_indexed(m, text, length, builder);
    const int found = MenusAccess::lookup(*m.collections, state, std::string_view{text, length});
    if (found < 0) {
        m.held = true;
        return false;
    }
    m.indexed_documents = documents;
    m.indexed_turn = m.turn;
    return take_name(m, text, length, found, builder);
}

/// Runs the work, charges it, then raises or reports success.
bool finish(lua_State* state, MenusState& m, Work work) {
    const bool ok = guarded(m, work);
    settle(state, m);
    return ok;
}

void push_place(lua_State* state, Place place) {
    lua_createtable(state, 0, 2);
    lua_pushinteger(state, place.page);
    lua_setfield(state, -2, "page");
    lua_pushinteger(state, place.slot);
    lua_setfield(state, -2, "slot");
}

} // namespace

extern "C" {

static int menus_add(lua_State* state);
static int menus_move(lua_State* state);
static int menus_drop(lua_State* state);
static int menus_grey(lua_State* state);
static int menus_hide(lua_State* state);
static int menus_builds(lua_State* state);
static int menus_index(lua_State* state);
static int menus_newindex(lua_State* state);

static int menus_add(lua_State* state) {
    MenusState* m = menus_at(state);
    if (m == nullptr)
        return raise_text(state, "expected a menus collection");
    begin_call(*m);
    const int top = lua_gettop(state);
    std::size_t builder_length = 0;
    std::size_t unit_length = 0;
    const char* builder = string_arg(state, 2, builder_length);
    const char* unit = string_arg(state, 3, unit_length);
    if ((top != 3 && top != 4) || builder == nullptr || unit == nullptr)
        return raise_text(state, "menus:add expects a builder and a unit");
    if (top == 4)
        read_request(state, 4, true, m->request);
    if (m->request.problem != 0)
        return raise_text(state, m->request.problem == 1 ? k_page_range : k_slot_range);
    if (const char* refusal = write_refusal(*m))
        return raise_text(state, refusal);
    if (!resolve(state, *m, builder, builder_length, true) ||
        !resolve(state, *m, unit, unit_length, false))
        return raise(state, *m);
    if (!finish(state, *m, work_add))
        return raise(state, *m);
    push_place(state, m->out);
    return 1;
}

static int menus_move(lua_State* state) {
    MenusState* m = menus_at(state);
    if (m == nullptr)
        return raise_text(state, "expected a menus collection");
    begin_call(*m);
    std::size_t builder_length = 0;
    std::size_t unit_length = 0;
    const char* builder = string_arg(state, 2, builder_length);
    const char* unit = string_arg(state, 3, unit_length);
    if (lua_gettop(state) != 4 || builder == nullptr || unit == nullptr)
        return raise_text(state, "menus:move expects a builder, a unit and a place");
    read_request(state, 4, false, m->request);
    if (m->request.problem != 0)
        return raise_text(state, m->request.problem == 1 ? k_page_range : k_slot_range);
    if (const char* refusal = write_refusal(*m))
        return raise_text(state, refusal);
    if (!resolve(state, *m, builder, builder_length, true) ||
        !resolve(state, *m, unit, unit_length, false))
        return raise(state, *m);
    if (!finish(state, *m, work_move))
        return raise(state, *m);
    push_place(state, m->out);
    return 1;
}

static int menus_drop(lua_State* state) {
    MenusState* m = menus_at(state);
    if (m == nullptr)
        return raise_text(state, "expected a menus collection");
    begin_call(*m);
    const int top = lua_gettop(state);
    std::size_t first_length = 0;
    std::size_t second_length = 0;
    const char* first = string_arg(state, 2, first_length);
    const char* second = top == 3 ? string_arg(state, 3, second_length) : nullptr;
    if ((top != 2 && top != 3) || first == nullptr || (top == 3 && second == nullptr))
        return raise_text(state, "menus:remove expects a unit");
    if (const char* refusal = write_refusal(*m))
        return raise_text(state, refusal);
    if (top == 3) {
        if (!resolve(state, *m, first, first_length, true) ||
            !resolve(state, *m, second, second_length, false))
            return raise(state, *m);
    } else if (!resolve(state, *m, first, first_length, false)) {
        return raise(state, *m);
    }
    if (!finish(state, *m, work_remove))
        return raise(state, *m);
    return 0;
}

static int menus_grey(lua_State* state) {
    MenusState* m = menus_at(state);
    if (m == nullptr)
        return raise_text(state, "expected a menus collection");
    begin_call(*m);
    std::size_t builder_length = 0;
    std::size_t unit_length = 0;
    const char* builder = string_arg(state, 2, builder_length);
    const char* unit = string_arg(state, 3, unit_length);
    if (lua_gettop(state) != 3 || builder == nullptr || unit == nullptr)
        return raise_text(state, "menus:grey expects a builder and a unit");
    if (const char* refusal = write_refusal(*m))
        return raise_text(state, refusal);
    if (!resolve(state, *m, builder, builder_length, true) ||
        !resolve(state, *m, unit, unit_length, false))
        return raise(state, *m);
    if (!finish(state, *m, work_grey))
        return raise(state, *m);
    return 0;
}

static int menus_hide(lua_State* state) {
    MenusState* m = menus_at(state);
    if (m == nullptr)
        return raise_text(state, "expected a menus collection");
    begin_call(*m);
    std::size_t builder_length = 0;
    std::size_t unit_length = 0;
    const char* builder = string_arg(state, 2, builder_length);
    const char* unit = string_arg(state, 3, unit_length);
    if (lua_gettop(state) != 3 || builder == nullptr || unit == nullptr)
        return raise_text(state, "menus:hide expects a builder and a unit");
    if (const char* refusal = write_refusal(*m))
        return raise_text(state, refusal);
    if (!resolve(state, *m, builder, builder_length, true) ||
        !resolve(state, *m, unit, unit_length, false))
        return raise(state, *m);
    if (!finish(state, *m, work_hide))
        return raise(state, *m);
    return 0;
}

static int menus_builds(lua_State* state) {
    MenusState* m = menus_at(state);
    if (m == nullptr)
        return raise_text(state, "expected a menus collection");
    begin_call(*m);
    std::size_t builder_length = 0;
    const char* builder = string_arg(state, 2, builder_length);
    if (lua_gettop(state) != 2 || builder == nullptr)
        return raise_text(state, "menus:builds expects a builder");
    if (!resolve(state, *m, builder, builder_length, true))
        return raise(state, *m);
    if (!finish(state, *m, work_builds))
        return raise(state, *m);
    const std::size_t count = m->names.size();
    if (count > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return raise_text(state, "too many names");
    lua_createtable(state, static_cast<int>(count), 0);
    for (std::size_t index = 0; index < count; ++index) {
        lua_pushlstring(state, m->names[index].data(), m->names[index].size());
        lua_rawseti(state, -2, static_cast<lua_Integer>(index + 1));
    }
    return 1;
}

static int menus_index(lua_State* state) {
    if (menus_at(state) == nullptr)
        return raise_text(state, "expected a menus collection");
    std::size_t length = 0;
    const char* name = string_arg(state, 2, length);
    if (name == nullptr)
        return raise_text(state, "menus has no method");
    const std::string_view key{name, length};
    lua_CFunction method = nullptr;
    if (key == "add")
        method = menus_add;
    else if (key == "move")
        method = menus_move;
    else if (key == "remove")
        method = menus_drop;
    else if (key == "grey")
        method = menus_grey;
    else if (key == "hide")
        method = menus_hide;
    else if (key == "builds")
        method = menus_builds;
    if (method != nullptr) {
        lua_pushcfunction(state, method);
        return 1;
    }
    lua_pushliteral(state, "menus has no ");
    lua_pushvalue(state, 2);
    lua_concat(state, 2);
    return lua_error(state);
}

static int menus_newindex(lua_State* state) {
    return raise_text(state, "a menus collection cannot be assigned");
}

} // extern "C"

Menus::Menus(Collections& collections, const oa::AssetStore& store, const defs::DataLayout* layout)
    : state_(std::make_unique<MenusState>()) {
    state_->collections = &collections;
    state_->store = &store;
    state_->layout = layout != nullptr ? layout : &defs::data_layout();
}

Menus::~Menus() = default;

void Menus::begin_mutator(bool menus_layer) {
    MenusState& m = *state_;
    m.menus_layer = menus_layer;
    ++m.turn;
    m.page_sets.clear();
    m.page_lookup.clear();
    MenusAccess::lookup(*m.collections, nullptr, "");
    m.indexed_documents = documents_of(m).size();
    m.indexed_turn = m.turn;
    refresh_lists(m);
    for (std::size_t slot = 0; slot < m.download_docs.size(); ++slot)
        take_snapshot(m, m.download_docs[slot]);
    m.turn_removals = MenusAccess::removals(*m.collections).size();
    m.removals_seen = m.turn_removals;
    m.clones_seen = MenusAccess::clones(*m.collections).size();
    m.charge = 0;
}

bool Menus::end_mutator(std::string& error) {
    MenusState& m = *state_;
    // A menus object the script kept writes nothing in a later phase.
    m.menus_layer = false;
    begin_call(m);
    const bool ok = guarded(m, work_end);
    // The repair is the engine's own step: no script is charged for it.
    m.charge = 0;
    if (ok)
        return true;
    try {
        error = m.error.empty() ? std::string{"a build menu could not be written"} : m.error;
    } catch (...) {
    }
    return false;
}

void Menus::push(lua_State* state) {
    if (luaL_newmetatable(state, k_meta_name) != 0) {
        lua_pushcfunction(state, menus_index);
        lua_setfield(state, -2, "__index");
        lua_pushcfunction(state, menus_newindex);
        lua_setfield(state, -2, "__newindex");
        lua_pushboolean(state, 0);
        lua_setfield(state, -2, "__metatable");
    }
    lua_pop(state, 1);
    auto* box = static_cast<LuaBox*>(lua_newuserdatauv(state, sizeof(LuaBox), 0));
    box->owner = state_.get();
    luaL_setmetatable(state, k_meta_name);
}

void Menus::set_context(lua_State* state, int ctx_index) {
    const int ctx = lua_absindex(state, ctx_index);
    push(state);
    lua_setfield(state, ctx, "menus");
}

std::span<const MenuFile> Menus::added_files() const {
    return state_->added;
}

} // namespace oa::data::mutators
