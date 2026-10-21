// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// How a mutator treats a unit or weapon whose name is already in the game.
// The player's choice wins, then the manifest, then the preference. Add keeps
// the existing record: an identical one is shared, and a different one takes
// a private name. Replace puts the new record in its place.
#pragma once

#include "oa/formats/oamut/manifest.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace oa::formats::tdf {
struct Block;
}

namespace oa::data::mutators {

/// The most characters in a unit or weapon name.
inline constexpr std::size_t record_name_limit = 31;

/// Preference key for the clash mode used when the player and the manifest
/// leave it unset. The text is `add` or `replace`. `Runtime::apply_match_mutators`
/// reads it. Any other text, and a missing key, are `replace`.
inline constexpr std::string_view clash_default_preference = "mutators.clash-default";

/// The clash mode a missing or unrecognised preference reads as.
inline constexpr oa::formats::oamut::ClashMode clash_setting_default =
    oa::formats::oamut::ClashMode::replace;

/// One name a mutator gave a record so the existing record could stay.
///
/// `from` is the name the mutator wrote. `to` is the private name now on the
/// record. `weapon` is true for a weapon section and false for a unit.
struct ClashRename {
    /// True when `from` and `to` name a weapon section.
    bool weapon{};
    /// The name the mutator wrote.
    std::string from;
    /// The private name the record was stored under.
    std::string to;
};

/// The clash mode in force for one mutator.
///
/// The player's choice wins when it is set. Otherwise the manifest's
/// `clashes` wins when it is set. Otherwise `setting` is the mode.
///
/// @param player the player's choice, or nothing when the player left it
/// @param manifest the manifest's `clashes`, or nothing when it names none
/// @param setting the mode the preference resolved to
/// @return the mode that applies
[[nodiscard]] oa::formats::oamut::ClashMode effective_clash(
    std::optional<oa::formats::oamut::ClashMode> player,
    std::optional<oa::formats::oamut::ClashMode> manifest,
    oa::formats::oamut::ClashMode setting
) noexcept;

/// Reads the clash preference.
///
/// `add` and `replace` are the only texts that count, compared byte for
/// byte. Anything else, including an empty text, is `clash_setting_default`.
///
/// @param text the stored preference
/// @return the mode
[[nodiscard]] oa::formats::oamut::ClashMode clash_from_setting(std::string_view text) noexcept;

/// Reports whether a name can be a unit or weapon name.
///
/// A name is one to `record_name_limit` characters and holds no `/`, `\`,
/// `:` or `.`. Comparison of two names ignores ASCII case. The clone
/// alphabet is stricter and is not this rule.
///
/// @param name the name
/// @return true when the name is allowed
[[nodiscard]] bool record_name_ok(std::string_view name) noexcept;

/// The first private name for a record whose name is already taken.
///
/// The name is `name`, `_`, and the first integer from 2 for which `taken`
/// returns false. When the result would pass `record_name_limit`, the stem
/// is shortened from the end so the suffix still fits. A candidate that
/// `record_name_ok` refuses is skipped. The search stops after 100000.
///
/// @param name the name the mutator wrote
/// @param taken true when that candidate is already a unit, a weapon, or a
///        name assigned earlier in this mutator
/// @return the private name, or nothing when none is free
[[nodiscard]] std::optional<std::string>
private_record_name(std::string_view name, const std::function<bool(std::string_view)>& taken);

/// Maps one field value while comparing the incoming record.
///
/// The key is the field name as stored. The value is the incoming text.
/// The returned text is what the comparison uses for that field.
using RecordFieldMap = std::function<std::string(std::string_view key, std::string_view value)>;

/// Reports whether two records are the same record.
///
/// Keys and child-section names match without ASCII case. Values match
/// byte for byte, so a display string that differs only in case is a
/// different record. Order does not matter. Duplicate keys are compared
/// as a multiset. Children of the same name match in file order, and
/// their contents are compared the same way. When `weapon` is set, the
/// top-level `ID` is ignored: the engine owns weapon ids. A nested `id`
/// is compared. When `map_right` is set, it is applied to the right
/// record's values, including nested children, before the byte compare.
/// The left record is not mapped.
///
/// @param left one record
/// @param right the other record
/// @param weapon true when both blocks are weapon sections
/// @param map_right optional map of the right record's values; null maps nothing
/// @return true when neither record would change the other
[[nodiscard]] bool records_identical(
    const oa::formats::tdf::Block* left,
    const oa::formats::tdf::Block* right,
    bool weapon,
    const RecordFieldMap* map_right = nullptr
);

/// Reports whether a field names a weapon.
///
/// The keys are `weapon1`, `weapon2`, `weapon3`, `explodeas` and
/// `selfdestructas`, compared without ASCII case.
///
/// @param key the field name
/// @return true when the field's value is a weapon name
[[nodiscard]] bool weapon_reference_key(std::string_view key) noexcept;

/// Reports whether a field is a side or AI unit list.
///
/// The keys are `commander`, `canbuild`, and `canbuild` followed only by
/// digits, compared without ASCII case.
///
/// @param key the field name
/// @return true when the field's value is a unit name in such a list
[[nodiscard]] bool unit_list_key(std::string_view key) noexcept;

} // namespace oa::data::mutators
