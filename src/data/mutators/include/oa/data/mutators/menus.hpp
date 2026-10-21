// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Build menus a mutator hook reads and writes: download entries and the
// buttons on a builder's authored pages. Every call reads the documents as
// they stand when it runs and changes only the entries and buttons it names.
// The object must outlive every Lua state it is pushed into. The collections
// and the asset store must outlive the object.
#pragma once

#include "oa/data/defs/layout.hpp"
#include "oa/data/mutators/collections.hpp"

#include <memory>
#include <span>
#include <string>

struct lua_State;

namespace oa {
class AssetStore;
}

namespace oa::data::mutators {

/// Working storage of one menus object. Defined with the implementation.
struct MenusState;

/// One download file the menus created for a unit's new entries.
struct MenuFile {
    std::string mutator; ///< mutator whose request created it
    std::string path;    ///< document path, '/' separated
};

/// The build menus of one document set, as a Lua collection.
///
/// A slot is decided when the request is made, in list order, from the
/// documents as they stand then. Edits go straight into the documents the
/// collections share and are journaled there, so a later mutator, and a
/// later call of the same mutator, sees them.
class Menus {
  public:

    /// Binds the menus to the collections' documents and journal.
    ///
    /// @param collections collections whose documents, units and journal the menus use
    /// @param store layered store the authored pages and button art are read from
    /// @param layout layout that names the folders; null uses the layout in force
    Menus(
        Collections& collections,
        const oa::AssetStore& store,
        const oa::data::defs::DataLayout* layout = nullptr
    );
    Menus(const Menus&) = delete;
    Menus& operator=(const Menus&) = delete;
    Menus(Menus&&) = delete;
    Menus& operator=(Menus&&) = delete;
    /// Drops the menus. Every state they were pushed into is already closed.
    ~Menus();

    /// Starts one mutator's turn, after `Collections::begin_mutator`.
    ///
    /// A write is refused unless `menus_layer` is set. Reading what a builder
    /// offers is allowed either way. The order of every download file is
    /// noted here, so a unit the hooks remove can be taken out of the menus
    /// without moving another entry.
    ///
    /// @param menus_layer true when the manifest declares the menus layer
    void begin_mutator(bool menus_layer);

    /// Ends the turn, before `Collections::end_mutator`.
    ///
    /// Puts a placeholder where a removed unit's download entry was, when
    /// that keeps a section past the fifth unread or keeps another unit from
    /// being marked downloadable, and greys the removed unit's buttons on
    /// every builder's authored pages. The writes are journaled to the
    /// mutator. A script's writes are refused from here until the next
    /// begin, even through a menus object it kept from this turn.
    ///
    /// @param[out] error the reason, when the call returns false
    /// @return false when a menu could not be written or the journal is full
    [[nodiscard]] bool end_mutator(std::string& error);

    /// Pushes the menus collection.
    ///
    /// @param state state to push into
    void push(lua_State* state);

    /// Sets `menus` on a context table.
    ///
    /// @param state state that holds the table
    /// @param ctx_index stack index of the context table
    void set_context(lua_State* state, int ctx_index);

    /// Download files the menus created, in the order they were created.
    ///
    /// The span is invalidated by the next created file.
    ///
    /// @return the files
    [[nodiscard]] std::span<const MenuFile> added_files() const;

  private:

    std::unique_ptr<MenusState> state_;
};

} // namespace oa::data::mutators
