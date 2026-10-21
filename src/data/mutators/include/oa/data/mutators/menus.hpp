// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Build menus a mutator hook reads and writes: download entries and the
// buttons on a builder's authored pages. The object must outlive every Lua
// state it is pushed into. The document set and the asset store must outlive
// the object.
#pragma once

#include "oa/data/defs/layout.hpp"
#include "oa/data/mutators/collections.hpp"
#include "oa/data/mutation_rules/apply.hpp"
#include "oa/data/mutation_rules/documents.hpp"

#include <memory>
#include <span>
#include <string>
#include <string_view>

struct lua_State;

namespace oa {
class AssetStore;
}

namespace oa::data::mutators {

/// The build menus of one document set, as a Lua collection.
///
/// Slots are resolved as each request is made, in list order, so a later
/// request cannot move an earlier one. The documents are written once, from
/// `resolve`, after every mutator's hooks have committed.
class Menus {
  public:

    /// Reads builders, authored pages and download files.
    ///
    /// Downloadable marks are taken from the files as they stand here, before
    /// any hook runs. `layout` null uses the layout in force.
    ///
    /// @param documents documents the hooks read; download files are already parsed
    /// @param store layered store the pages and button art are read from
    /// @param layout layout that names the folders; null uses the layout in force
    Menus(
        oa::data::mutation_rules::DocumentSet& documents,
        const oa::AssetStore& store,
        const oa::data::defs::DataLayout* layout = nullptr
    );
    Menus(const Menus&) = delete;
    Menus& operator=(const Menus&) = delete;
    Menus(Menus&&) = delete;
    Menus& operator=(Menus&&) = delete;
    /// Drops the menus. Every state they were pushed into is already closed.
    ~Menus();

    /// Starts one mutator's turn.
    ///
    /// Writes until `end_mutator` are journaled to `mutator`. A write is
    /// refused unless `menus_layer` is set. Reading what a builder offers is
    /// allowed either way.
    ///
    /// @param mutator mutator name, as the player list spells it
    /// @param menus_layer true when the manifest declares the menus layer
    void begin_mutator(std::string mutator, bool menus_layer);

    /// Greys units and builders this mutator removed, then ends the turn.
    ///
    /// Download entries D03 already dropped are not journaled again. Page
    /// buttons greyed here are.
    ///
    /// @param removals_this_turn removals the collections recorded for this mutator
    void end_mutator(std::span<const RecordRemoval> removals_this_turn);

    /// Pushes the menus collection.
    ///
    /// @param state state to push into
    void push(lua_State* state);

    /// Sets `menus` on a context table.
    ///
    /// @param state state that holds the table
    /// @param ctx_index stack index of the context table
    void set_context(lua_State* state, int ctx_index);

    /// Writes every menu document that changed.
    ///
    /// A unit stays downloadable exactly when it was before `Menus` was built,
    /// except a unit a hook removed and a unit whose every download entry was
    /// removed. Unchanged files are left untouched.
    ///
    /// @param[out] error the reason, when the call returns false
    /// @return false when a document could not be written
    [[nodiscard]] bool resolve(std::string& error);

    /// The mutator `resolve` failed for, or empty when the failure names none.
    ///
    /// @return the mutator id
    [[nodiscard]] const std::string& failed_mutator() const;

    /// Field writes so far, in the order the hooks made them.
    ///
    /// The span is invalidated by the next write.
    ///
    /// @return the writes
    [[nodiscard]] std::span<const oa::data::mutation_rules::FieldWrite> writes() const;

    /// Notes so far, in the order the hooks made them.
    ///
    /// The span is invalidated by the next note.
    ///
    /// @return the notes
    [[nodiscard]] std::span<const HookNote> notes() const;

    /// Removals so far, in the order the hooks made them.
    ///
    /// The span is invalidated by the next removal. Download entries a unit
    /// removal already journaled on the collections are not listed again.
    ///
    /// @return the removals
    [[nodiscard]] std::span<const RecordRemoval> removals() const;

  private:

    struct State;
    struct LuaBox;
    static Menus* from_lua(lua_State* state, int index);
    static int lua_index(lua_State* state);
    static int lua_newindex(lua_State* state);
    static int lua_add(lua_State* state);
    static int lua_move(lua_State* state);
    static int lua_drop(lua_State* state);
    static int lua_grey(lua_State* state);
    static int lua_hide(lua_State* state);
    static int lua_builds(lua_State* state);
    static void ensure_meta(lua_State* state);

    std::unique_ptr<State> state_;
};

} // namespace oa::data::mutators
