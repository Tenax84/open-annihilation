// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// units:clone and weapons:clone. The collections register the method; the
// pipeline journals what a clone added and serves the copied bytes.
#pragma once

#include <string>

struct lua_State;

namespace oa::data::mutators {

class Collections;

/// Copies one unit or one weapon for the collection method `clone`.
///
/// `weapons` selects the weapons collection. A refusal returns -1 and sets
/// `error` to the Lua error, including the mutator's request. Success returns
/// 0 and leaves the new record in the document set. The caller marks the
/// collection index dirty only after success.
///
/// @param state sandbox state; argument 1 is the collection, then source, new
///        name and fields
/// @param collections collections the hook is running on
/// @param weapons true for weapons, false for units
/// @param[out] error the Lua error when the call returns -1
/// @return 0 on success, -1 on refusal
int clone_collection(lua_State* state, Collections& collections, bool weapons, std::string& error);

} // namespace oa::data::mutators
