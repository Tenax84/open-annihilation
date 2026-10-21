// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// units:clone and weapons:clone. The collections register the method; the
// pipeline journals what a clone added and serves the copied bytes.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

struct lua_State;

namespace oa::formats::tdf {
struct Block;
}

namespace oa::data::mutators {

class Collections;

/// One record the collection index already holds.
struct CloneHit {
    std::size_t doc_index = 0;
    oa::formats::tdf::Block* section = nullptr;
    std::string name;
    std::string path;
};

/// The first record of `name` in the collection index.
///
/// Uses the same lookup as a script reading `units.name`: the index is built
/// once and charged, and a missing name returns false. May raise when the
/// work budget is spent.
///
/// @param state sandbox state the charge is counted against
/// @param collections collections the hook is running on
/// @param weapons true for weapons, false for units
/// @param name record name, compared without ASCII case
/// @param[out] hit the record; unchanged when the call returns false
/// @return true when a record of that name is indexed
[[nodiscard]] bool clone_find(
    lua_State* state, Collections& collections, bool weapons, std::string_view name, CloneHit& hit
);

/// True when `asked` is the clone's unit name.
///
/// The allow list strips every trailing digit of `anims/<name><page>.gaf`,
/// so a clone named PLAIN2 is asked as PLAIN. `asked` matches `unit` itself,
/// or `unit` with those extra digits removed.
///
/// @param asked the unit name `check_mutable_path` extracted
/// @param unit the new unit name
/// @return true when the art belongs to that clone
[[nodiscard]] bool clone_names_unit(std::string_view asked, std::string_view unit);

/// True when `path` is `<gui_folder>/<unit><digits>.gui`.
///
/// The folder, the unit and the extension compare without ASCII case. A
/// backslash is a slash. The digits are required, so the unit file itself
/// is not a build page.
///
/// @param path path a clone would serve
/// @param gui_folder the layout's gui directory
/// @param unit the new unit name
/// @return true when the path is that unit's build page
[[nodiscard]] bool
clone_build_page(std::string_view path, std::string_view gui_folder, std::string_view unit);

/// Copies one unit or one weapon for the collection method `clone`.
///
/// `weapons` selects the weapons collection. A refusal returns -1 and sets
/// `error` to the Lua error, including the mutator's request. Success returns
/// 0 and leaves the new record in the document set. The caller indexes that
/// record only after success. The call charges the name lookups, the bytes it
/// is about to write and, for a weapon, each section the id search visits.
///
/// @param state sandbox state; argument 1 is the collection, then source, new
///        name and fields
/// @param collections collections the hook is running on
/// @param weapons true for weapons, false for units
/// @param[out] error the Lua error when the call returns -1
/// @return 0 on success, -1 on refusal
int clone_collection(lua_State* state, Collections& collections, bool weapons, std::string& error);

} // namespace oa::data::mutators
