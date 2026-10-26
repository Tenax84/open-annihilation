// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// units:clone and weapons:clone. The collections register the method; the
// pipeline journals what a clone added and serves the copied bytes.
#pragma once

#include <cstddef>
#include <cstdint>
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
/// Uses the same lookup as a script reading `units.name`, but charges
/// nothing and never raises: the caller brings the index up to date and
/// charges it first. Adds the lookup's units to `units`, saturating: one
/// element per document for the growth check, the record access and the
/// name's bytes. A missing name returns false. Throws `std::bad_alloc` when the index
/// cannot grow.
///
/// @param collections collections the hook is running on
/// @param weapons true for weapons, false for units
/// @param name record name, compared without ASCII case
/// @param[out] hit the record; unchanged when the call returns false
/// @param[in,out] units the clone's bill, which the lookup adds to
/// @return true when a record of that name is indexed
[[nodiscard]] bool clone_find(
    Collections& collections, bool weapons, std::string_view name, CloneHit& hit, uint64_t& units
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
/// record only after success. The call never raises: it adds to `units` the
/// name lookups, the bytes it reads or writes and, for a weapon, each section
/// the id search visits, refused or not, and the caller charges that bill
/// once its own C++ objects are gone. Throws `std::bad_alloc` when memory
/// runs out.
///
/// @param state sandbox state; argument 1 is the collection, then source, new
///        name and fields
/// @param collections collections the hook is running on
/// @param weapons true for weapons, false for units
/// @param[out] error the Lua error when the call returns -1
/// @param[in,out] units the clone's bill in instruction units, saturating
/// @return 0 on success, -1 on refusal
int clone_collection(
    lua_State* state, Collections& collections, bool weapons, std::string& error, uint64_t& units
);

} // namespace oa::data::mutators
