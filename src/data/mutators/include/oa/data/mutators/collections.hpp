// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The units, weapons and features a mutator hook reads and writes.
//
// A hook sees the pipeline's shared documents, one record at a time, and
// every change is kept in the journal below. The object must outlive every
// Lua state it is pushed into, and the document set must outlive the object:
// a script holds handles, not copies, and each access looks the record up
// again.
#pragma once

#include "oa/data/defs/layout.hpp"
#include "oa/data/mutation_rules/apply.hpp"
#include "oa/data/mutation_rules/documents.hpp"
#include "oa/formats/soft_mutator.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct lua_State;

namespace oa {
class AssetStore;
}

namespace oa::data::mutators {

/// Reaches the records from Lua. The bindings are defined with the
/// implementation; a caller does not use this type.
struct CollectionsAccess;

/// Working storage for one collections object. Defined with the implementation.
struct CollectionsState;

/// The most field writes one mutator may journal on one collections object.
///
/// A further write fails that mutator with
/// "this mutator has changed too many fields" and is not stored.
inline constexpr uint32_t mutator_write_limit = 65536;

/// The most notes one mutator may add on one collections object.
///
/// A further note fails that mutator with "this mutator has made too many notes".
inline constexpr uint32_t mutator_note_limit = 65536;

/// Which of these collections a mutator may change.
///
/// A flag is set when the mutator's manifest declares that layer. Reading is
/// allowed either way.
struct WriteGrant {
    bool units{};    ///< unit records and the download entries a removal drops
    bool weapons{};  ///< weapon records, including a weapon's damage block
    bool features{}; ///< feature records
    bool sides{};    ///< side data and the build lists
    bool tables{};   ///< other game-data tables
};

/// What a removal took out of the document set.
enum class RemovalKind : uint8_t {
    unit,           ///< a unit document
    weapon,         ///< one weapon section
    feature,        ///< one feature section
    download_entry, ///< one download section that named a removed unit
    document,       ///< a document left with no section
};

/// One record or download entry a hook removed.
struct RecordRemoval {
    std::string mutator; ///< mutator whose hook removed it
    RemovalKind kind{};  ///< which kind of removal
    std::string name;    ///< record or section name
    std::string path;    ///< document path
};

/// Why a hook left a note beside its writes.
enum class HookNoteKind : uint8_t {
    check_warned,                 ///< a typed write was outside the comfortable range and was kept
    hook_overwrote,               ///< a later hook replaced an earlier hook's value
    download_first_entry_changed, ///< removing the first download entry left a different unit first
    not_a_number,                 ///< a scale skipped a value that is not a number
    id_kept,   ///< a write of id on a weapon the game already numbered was left undone
    menu_note, ///< a moved button, a missing picture or a full AI list
};

/// One unit or weapon a hook cloned.
///
/// `sentence` is the journal line: "added unit NEW (copy of SRC)" or
/// "added weapon NEW (copy of SRC) at id N". `weapon_id` is that id, and 0
/// for a unit.
struct CloneRecord {
    std::string mutator;  ///< mutator whose hook cloned it
    bool weapon{};        ///< true for a weapon section, false for a unit
    std::string name;     ///< the new record's name, as the script wrote it
    std::string source;   ///< the record it was copied from, as the script wrote it
    int weapon_id{};      ///< free weapon id, from 1 to 255; 0 for a unit
    std::string path;     ///< document path the copy was written to
    std::string sentence; ///< the journal line for this clone
};

/// One binary file a clone serves under the new name.
///
/// `sentence` is "added path <new>, copy of <source>". The bytes are the
/// source file as the store held it when the clone ran.
struct ServedCopy {
    std::string mutator;                               ///< mutator whose clone served it
    std::string path;                                  ///< path the layer serves
    std::string source_path;                           ///< path the bytes were read from
    std::string sentence;                              ///< the journal line for this copy
    std::shared_ptr<const std::vector<uint8_t>> bytes; ///< the served bytes
};

/// One note a hook produced, for the change journal.
struct HookNote {
    HookNoteKind kind{};               ///< which note
    std::vector<std::string> mutators; ///< mutators involved, earlier ones first
    std::string path;                  ///< document path
    std::string record;                ///< record name
    std::string key;                   ///< field name; empty when the note is not about one field
    std::string message;               ///< what happened, in plain words
};

/// Publishes `range`, `is`, `has`, `absent` and `not_` in a state.
///
/// The state is one the script host opened. The forms build the needs
/// `need_from_lua` reads, and their metatables cannot be read or replaced.
///
/// @param state state to publish into
void install_selection_forms(lua_State* state);

/// Reads one need from a Lua value.
///
/// A number `n` is `range(n, n)`. `true` is `range(1, 1)` and `false` is
/// `not_(range(1, 1))`. A string is `is` of that string. `absent()` is
/// `not_(has(""))`. `not_` of a `not_` is refused. Anything else is refused
/// with a message that names `field` when `field` is not empty.
///
/// @param state state that holds the value
/// @param index stack index of the value
/// @param field field the need applies to; empty when the caller has none yet
/// @param[out] need the need; unchanged when the call returns false
/// @param[out] error the reason; may be null
/// @return true when `need` was filled
[[nodiscard]] bool need_from_lua(
    lua_State* state,
    int index,
    std::string_view field,
    oa::formats::soft_mutator::Need& need,
    std::string* error
);

/// The units, weapons and features of one document set, as Lua collections.
///
/// The object must outlive every state `push_units`, `push_weapons`,
/// `push_features` or `set_context_collections` was called with. The document
/// set must outlive this object. A handle remembers the section it was given.
/// The name is kept for messages. Removing that section, or its document,
/// makes the handle fail. Removing a different section, or renaming the unit,
/// leaves the handle on its own section.
class Collections {
  public:

    /// Binds the collections to a document set.
    ///
    /// @param documents documents the hooks read and write; must outlive this object
    /// @param layout layout that names the folders; null uses the layout in force at each call
    Collections(
        oa::data::mutation_rules::DocumentSet& documents,
        const oa::data::defs::DataLayout* layout = nullptr
    );
    Collections(const Collections&) = delete;
    Collections& operator=(const Collections&) = delete;
    Collections(Collections&&) = delete;
    Collections& operator=(Collections&&) = delete;
    /// Drops the collections. Every state they were pushed into is already closed.
    ~Collections();

    /// Starts one mutator's turn.
    ///
    /// Writes and removals until `end_mutator` are journaled to `mutator` and
    /// refused unless `grant` allows that collection.
    ///
    /// @param mutator mutator name, as the player list spells it
    /// @param grant layers this mutator may write
    void begin_mutator(std::string mutator, WriteGrant grant);

    /// Ends the mutator's turn. Later writes are refused until the next begin.
    void end_mutator();

    /// Names and paths the side and table bindings compute before they push.
    ///
    /// The object outlives the Lua callback. A callback fills it, returns,
    /// and only then pushes, so a Lua error cannot skip a destructor.
    struct SideScratch {
        std::vector<std::string> names;
        std::vector<std::string> raw_names;
        std::string text;
        std::string path;
        std::string record;
        std::string key;
        std::string builder;
        std::string side_path;
        std::string list_path;
        std::string side_error;
        std::string list_error;
        uint64_t visits = 0;
        uint64_t text_units = 0;
        uint64_t fields = 0;
        uint64_t comparisons = 0;
        bool scan = false;
        bool found = false;
        bool builder_unit = false;
        bool present = false;
        bool side_ready = false;
        bool list_ready = false;
        bool side_found = false;
        bool list_found = false;
        uint32_t count = 0;
        uint32_t slot = 0;
        std::size_t side_doc = 0;
        std::size_t list_doc = 0;
    };

    /// Pushes the units collection.
    ///
    /// @param state state to push into
    void push_units(lua_State* state);

    /// Pushes the weapons collection.
    ///
    /// @param state state to push into
    void push_weapons(lua_State* state);

    /// Pushes the features collection.
    ///
    /// @param state state to push into
    void push_features(lua_State* state);

    /// Sets `units`, `weapons` and `features` on a context table.
    ///
    /// @param state state that holds the table
    /// @param ctx_index stack index of the context table
    void set_context_collections(lua_State* state, int ctx_index);

    /// Returns the field writes so far, in the order the hooks made them.
    ///
    /// The span is invalidated by the next write.
    ///
    /// @return the writes
    [[nodiscard]] std::span<const oa::data::mutation_rules::FieldWrite> writes() const;

    /// Returns the removals so far, in the order the hooks made them.
    ///
    /// The span is invalidated by the next removal.
    ///
    /// @return the removals
    [[nodiscard]] std::span<const RecordRemoval> removals() const;

    /// Returns the notes so far, in the order the hooks made them.
    ///
    /// The span is invalidated by the next note.
    ///
    /// @return the notes
    [[nodiscard]] std::span<const HookNote> notes() const;

    /// Remembers the store a clone reads while a hook runs.
    ///
    /// The store must outlive the hooks. Null clears it.
    ///
    /// @param store the layered store, or null
    void bind_store(oa::AssetStore* store) { store_ = store; }

    /// Remembers weapon sections whose ids the game already assigned.
    ///
    /// A later hook that writes `id` on one of these weapons does not change
    /// it, and the note names the mutator. A weapon a mutator brings has a
    /// new name and is not in this list, so a write of `id` on it is kept
    /// and the numbering pass reads it as that weapon's request.
    ///
    /// @param names section names, compared without ASCII case
    void keep_weapon_ids(std::span<const std::string> names);

    /// Forgets a document the index has listed, so a later read sees it again.
    ///
    /// A hook that removed the document leaves the index holding that path.
    /// Bringing the file back clears that memory before the text is parsed
    /// again. A handle that still names a section of it then fails.
    ///
    /// @param path document path, '/' or '\\' separated
    void release_document(std::string_view path);

    /// Returns the clones made so far, in the order the hooks made them.
    ///
    /// The span is invalidated by the next clone.
    ///
    /// @return the clones
    [[nodiscard]] std::span<const CloneRecord> clones() const { return clones_; }

    /// Returns the binary copies served so far, in the order they were read.
    ///
    /// The span is invalidated by the next copy.
    ///
    /// @return the copies
    [[nodiscard]] std::span<const ServedCopy> served_copies() const { return served_; }

  private:

    friend struct CollectionsAccess;
    friend struct CloneAccess;
    /// The sides and tables collections journal through the same writes.
    friend struct SideCollectionsAccess;
    /// The build menus journal through the same writes and read the units index.
    friend struct MenusAccess;

    /// Keeps one field write, and an overwrite note when an earlier hook wrote it.
    ///
    /// @param write the write, naming this mutator
    /// @param[out] reason the limit it crossed; unchanged on success
    /// @return false when the mutator has journaled too much
    bool accept_write(oa::data::mutation_rules::FieldWrite write, std::string& reason);

    /// Keeps one hook note.
    ///
    /// @param note the note
    /// @param[out] reason the limit it crossed; unchanged on success
    /// @return false when the mutator has journaled too much
    bool accept_note(HookNote note, std::string& reason);

    /// Holds the next Lua error until `raise_held`.
    ///
    /// @param message the error
    void hold_error(std::string message);

    /// Raises the held error. Call it with no local objects still to destroy.
    ///
    /// @param state the state to raise in
    /// @return does not return
    int raise_held(lua_State* state);

    /// The side bindings' working storage. It lives as long as the collections.
    ///
    /// @return the storage
    SideScratch& side_scratch() noexcept;

    /// Resolves one unit through the units index.
    ///
    /// Charges the index check and the matched section. Copies the canonical
    /// name into `side_scratch().text` and the builder flag into
    /// `side_scratch().builder_unit`. Call it with no local object still to
    /// destroy: the charge can raise.
    ///
    /// @param state the state to charge, or null to build the index quietly
    /// @param sought the unit name the script wrote
    /// @return 1 when the unit is there, 0 when it is not, -1 on a held error
    int lookup_unit(lua_State* state, std::string_view sought);

    /// Finds a unit in the units index as the last `lookup_unit` left it.
    ///
    /// Neither charges nor looks for new documents, so it answers for the
    /// documents as they stood at that lookup. A caller charges the search.
    ///
    /// @param sought the unit name, compared without ASCII case
    /// @param[out] builder set true when the unit's FBI marks it a builder; may be null
    /// @return the unit's own spelling, valid until the next lookup; null when there is none
    [[nodiscard]] const std::string* indexed_unit(std::string_view sought, bool* builder) noexcept;

    oa::data::mutation_rules::DocumentSet* documents_;
    const oa::data::defs::DataLayout* layout_;
    bool active_{};
    std::string mutator_;
    WriteGrant grant_{};
    std::vector<oa::data::mutation_rules::FieldWrite> writes_;
    std::vector<RecordRemoval> removals_;
    std::vector<HookNote> notes_;
    std::unique_ptr<CollectionsState> state_;
    oa::AssetStore* store_{};         ///< store a clone reads; not owned
    std::vector<CloneRecord> clones_; ///< clones made during the hooks
    std::vector<ServedCopy> served_;  ///< binary copies those clones serve
    /// Folded weapon section names whose ids stay as the game assigned them.
    std::vector<std::string> kept_weapon_ids_;
    /// Folded served path to its index in `served_`. A page lookup does not scan the list.
    std::unordered_map<std::string, std::size_t> served_index_;
    SideScratch side_scratch_{};
};

} // namespace oa::data::mutators
