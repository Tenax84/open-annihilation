// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The maps hook: one chosen map's OTA, rewritten for a single match.
// A mutator's `maps` entry is a function or a list of `where():change()`
// blocks. The rewritten text is served from the mutator layer only when a
// mutator changed it. Start positions are never shown or written.
#pragma once

#include "oa/data/defs/layout.hpp"
#include "oa/data/mutators/journal.hpp"
#include "oa/data/mutators/loadout.hpp"
#include "oa/data/script_host/script_host.hpp"
#include "oa/formats/hpi.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct lua_State;

namespace oa::data::mutators {

/// The chosen map the maps phase reads. Absent on a dry run with no map.
struct ChosenMap {
    /// The map key the match selected, including a pack's `\@id` when it has one.
    std::string key;
    /// Store path of the OTA, `maps/<key>.ota`.
    std::string ota_path;
    /// The pack id after the last `\@`, or empty for a base map.
    std::string pack;
    /// The OTA bytes the store serves for this match, before any rewrite.
    std::shared_ptr<const std::vector<uint8_t>> ota;
};

/// What `ctx` shows a maps function. The pointers stay valid for the call.
struct MapScriptContext {
    /// The mod's id.
    std::string_view mod_id;
    /// The mod's version text.
    std::string_view mod_version;
    /// The mod's game id.
    std::string_view mod_game;
    /// The engine version.
    std::string_view engine_version;
    /// The mutator's options. Null is none.
    const std::vector<LoadoutOption>* options{};
};

/// What one mutator's maps turn did. A script error leaves `entries` empty
/// and `changed` false: the caller keeps the bytes from before the turn.
struct MapsTurn {
    /// False when the turn refused the map or the script stopped.
    bool ok{true};
    /// True when the failure is the script's, including a pattern it wrote.
    bool from_script{false};
    /// The script host's reason, when `from_script` is set.
    oa::data::script_host::ErrorKind kind{oa::data::script_host::ErrorKind::none};
    /// What happened, when the turn did not apply.
    std::string message;
    /// Journal rows for the writes that remain.
    std::vector<JournalEntry> entries;
    /// A later write over an earlier mutator's map field.
    std::vector<Conflict> conflicts;
    /// True when the served bytes differ from the bytes the turn was given.
    bool changed{false};
    /// The OTA to serve after this turn. Empty when nothing changed.
    std::shared_ptr<const std::vector<uint8_t>> bytes;
};

/// The refusal a units rule, or any need outside the map table form, gets
/// for `glob()` or `pattern()`.
inline constexpr std::string_view map_match_refusal = "glob() and pattern() match maps only";

/// Publishes `glob` and `pattern` in a state.
///
/// `glob` matches `*` and `?` only, ASCII case ignored, with `/` ordinary
/// and no character classes. A pattern longer than 256 bytes is refused.
/// `pattern` is a Lua pattern, matched later with the sandbox's `string.find`.
/// Both build a value only the map table form accepts.
///
/// @param state state to publish into
void install_map_match_forms(lua_State* state);

/// Reports whether the value is a `glob()` or `pattern()` value.
///
/// @param state state that holds the value
/// @param index stack index
/// @return true when the value is one of those two forms
[[nodiscard]] bool map_match_value(lua_State* state, int index);

/// Checks a maps list the plan phase already found to be a plain table.
///
/// An empty list is a maps entry that changes nothing. A hole, a key that is
/// not a list index, or an item that is not a finished `where():change()`
/// refuses the plan. A change key that is not a header or schema setting is
/// refused here, and so is a read-only header or schema key. The text is
/// empty when the list may run.
///
/// @param state state that holds the list
/// @param index stack index of the list
/// @return the refusal, or an empty string
[[nodiscard]] std::string maps_list_problem(lua_State* state, int index);

/// Runs one mutator's maps entry against the current OTA bytes.
///
/// `function_entry` selects the function stored at `ref`; otherwise `ref`
/// is the list of `where():change()` blocks. No map, and an empty plan,
/// never call this. A script that writes and then errors returns a failed
/// turn and no bytes. An unreadable OTA fails with an engine message before
/// the script runs.
///
/// @param sandbox the mutator's sandbox, still holding `ref`
/// @param function_entry true when the entry is a function
/// @param ref registry reference of the function or the list
/// @param chosen the map being played
/// @param current the OTA bytes at the start of this turn
/// @param store the layered store, for unit, feature and profile checks
/// @param layout directory names the checks build paths from
/// @param context what `ctx` shows
/// @param mutator this mutator's index in the input list
/// @param earlier journal rows from maps turns already kept this pass
/// @return the turn's bytes, journal and failure
[[nodiscard]] MapsTurn apply_maps(
    oa::data::script_host::Sandbox& sandbox,
    bool function_entry,
    int ref,
    const ChosenMap& chosen,
    const std::vector<uint8_t>& current,
    oa::AssetStore& store,
    const oa::data::defs::DataLayout& layout,
    const MapScriptContext& context,
    uint16_t mutator,
    const std::vector<JournalEntry>& earlier
);

} // namespace oa::data::mutators
