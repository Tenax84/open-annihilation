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
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oa::formats::tdf {
struct Document;
struct Block;
} // namespace oa::formats::tdf

struct lua_State;

namespace oa::data::mutators {

class Collections;

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
    /// True when the served bytes differ from the bytes the turn was given.
    bool changed{false};
    /// The OTA to serve after this turn. Empty when nothing changed.
    std::shared_ptr<const std::vector<uint8_t>> bytes;
    /// True when the script called `ctx.incompatible`. The bytes are not kept.
    bool declared{false};
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
/// Every read and write of the view is charged against the turn's
/// instruction allowance, at the collections' rates: one field access, the
/// key's bytes and the value's, each schema a `:set`, `:scale` or list item
/// writes, and each element a walk or a `where` visits. A refused write is
/// an error the script can catch. One mutator may journal at most
/// `mutator_write_limit` writes, counted with the writes its other hooks
/// made; a further write fails the mutator, even when the script catches
/// that error. The collections' turn must already name this mutator, so
/// the tally is the one those hooks used. No exception leaves the call.
///
/// @param sandbox the mutator's sandbox, still holding `ref`
/// @param function_entry true when the entry is a function
/// @param ref registry reference of the function or the list
/// @param chosen the map being played
/// @param current the OTA bytes at the start of this turn
/// @param store the layered store, for the profile checks
/// @param collections the pass's units and features as the mutators above
///     left them; a placed name is checked here
/// @param layout directory names the checks build paths from
/// @param context what `ctx` shows
/// @param mutator this mutator's index in the input list
/// @return the turn's bytes, journal and failure
[[nodiscard]] MapsTurn apply_maps(
    oa::data::script_host::Sandbox& sandbox,
    bool function_entry,
    int ref,
    const ChosenMap& chosen,
    const std::vector<uint8_t>& current,
    oa::AssetStore& store,
    Collections& collections,
    const oa::data::defs::DataLayout& layout,
    const MapScriptContext& context,
    uint16_t mutator
);

/// The refusal D07 would give for this header text, or empty when it would keep it.
///
/// Whole-number header keys (`minwindspeed`, `maxwindspeed`, `gravity`,
/// `waterdamage`) accept a fractional result when it is finite and from 0 to
/// 32,767, because a rule's product is written as computed. A bit is still
/// 0 or 1, a real is still finite and from 0 to 1,000, and a frozen header
/// key cannot change. A null text is a removal, which the header refuses.
/// An unknown key is not a header or schema setting.
///
/// @param key header key, any case
/// @param text the new text, or null when the field is removed
/// @return the refusal, or an empty string
[[nodiscard]] std::string
map_header_refusal(std::string_view key, const std::optional<std::string>& text);

/// The refusal D07 gives for a schema `SurfaceMetal` outside 0 to 255.
///
/// The text must be a finite whole number in that range. A null text, a
/// fraction and any other text are refused with the schema sentence.
///
/// @param text the new text, or null when the field is removed
/// @return the refusal, or an empty string
[[nodiscard]] std::string map_schema_metal_refusal(const std::optional<std::string>& text);

/// Writes one key with D07's editor. The block keeps an existing spelling.
///
/// @param document document whose arena stores the text
/// @param block block to change
/// @param spelling field name to use when the key is new
/// @param text field value
/// @return true when the editor accepted the write
[[nodiscard]] bool write_map_field(
    oa::formats::tdf::Document* document,
    oa::formats::tdf::Block* block,
    const char* spelling,
    std::string_view text
);

/// How many `[Schema N]` blocks the header holds, stopping at the first gap.
///
/// @param document parsed OTA, or null
/// @return the count, from 0
[[nodiscard]] int map_schema_count(const oa::formats::tdf::Document* document);

/// The `[Schema N]` block, to edit. Null when that number is absent.
///
/// @param document parsed OTA, or null
/// @param number schema number, from 0
/// @return the block, or null
[[nodiscard]] oa::formats::tdf::Block*
map_schema_block(oa::formats::tdf::Document* document, int number);

/// The `Schema N` spelling D07 looks up, or empty when `number` is negative.
///
/// @param number schema number, from 0
/// @return the label
[[nodiscard]] std::string map_schema_label(int number);

/// The notes for map fields that more than one mutator wrote.
///
/// One `same_field_hooked` note for each pair of an earlier and a later
/// mutator that set the same record, block and field, naming the earlier
/// one first. The rows are read once, indexed by that field.
///
/// @param rows the maps journal rows the pass kept, in list order
/// @return the notes, in the order the later writes were made
[[nodiscard]] std::vector<Conflict> map_conflicts(const std::vector<JournalEntry>& rows);

} // namespace oa::data::mutators
