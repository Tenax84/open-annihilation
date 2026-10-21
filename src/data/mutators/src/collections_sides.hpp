// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Sides and the other game-data tables a hook reads and writes.
//
// The pipeline calls set_context_sides when it builds a hook's ctx.
// Sides come from the language variant of sidedata when that file is
// present, because that is the file the side loader reads. Build lists
// come from the plain gamedata/sidedata.tdf, because the build-list
// loader always reads that file. When the two paths are the same there
// is one document; when they differ each write is served at the path
// its reader opens.
#pragma once

#include "oa/data/defs/layout.hpp"
#include "oa/data/mutators/collections.hpp"
#include "oa/formats/hpi.hpp"

#include <string>

struct lua_State;

namespace oa::data::mutators {

/// Puts `sides` and `tables` on a context table.
///
/// `variant` empty selects the base side file. The store, the layout and
/// the variant text must outlive the hook.
///
/// @param state the hook's state
/// @param ctx_index stack index of the context table
/// @param collections the shared documents and journal
/// @param store the layered store the files are read from
/// @param layout the mod's directory names
/// @param variant the game-data variant word; empty for the base files
void set_context_sides(
    lua_State* state,
    int ctx_index,
    Collections& collections,
    const oa::AssetStore& store,
    const oa::data::defs::DataLayout& layout,
    const std::string& variant
);

} // namespace oa::data::mutators
