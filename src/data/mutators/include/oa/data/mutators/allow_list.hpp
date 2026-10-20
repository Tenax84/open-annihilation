// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Which paths a mutator may change for one match. The game reads some files
// again at every match start, and a mutator may replace those. Everything
// else is refused, so a mutator cannot leave the game half-changed until
// the next launch.
#pragma once

#include "oa/data/defs/layout.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace oa::data::mutators {

/// Why a path is allowed or refused.
enum class PathRule : uint8_t {
    allowed,        ///< the file is read again for every match
    invalid_path,   ///< not a relative path the game can store
    menus_only,     ///< build pages and screens change through the menus hook
    maps_only,      ///< maps change through a mutator's maps hook
    read_once,      ///< read once when the game starts
    not_confirmed,  ///< not yet shown to be read again for every match
    interface_art,  ///< interface art, other than a unit's build picture or download button
    unknown_folder, ///< a folder the allow-list does not name
};

/// The result of judging one path or one mask.
struct PathVerdict {
    /// The rule that decided the path.
    PathRule rule{};
    /// The path as given, a colon, and the reason in English.
    std::string message;

    /// Returns whether the path may change for one match.
    ///
    /// @return true when the rule is allowed
    [[nodiscard]] bool allowed() const noexcept { return rule == PathRule::allowed; }
};

/// The mod facts a check needs. The checks read nothing else.
struct PathContext {
    /// The mod's directory names. Null means the layout in use.
    const oa::data::defs::DataLayout* layout{};
    /// True when `name` is a unit this mod has. The letters are as the path
    /// spells them; the test decides whether case matters. A unit's build
    /// picture and its download-button art are allowed only when this holds.
    /// Empty means no unit is known.
    std::function<bool(std::string_view)> is_unit_name;
};

/// Says whether one relative path may be added for a match.
///
/// A backslash is read as '/'. Comparison ignores ASCII case. An empty, absolute,
/// drive-qualified, NUL-holding, traversing or over-long path is refused.
///
/// @param path the path, as a mutator wrote it
/// @param context the layout and the unit-name test
/// @return the rule and a message that starts with `path`
[[nodiscard]] PathVerdict check_mutable_path(std::string_view path, const PathContext& context);

/// Says whether one mask may hide paths for a match.
///
/// A pattern `mutator_mask_problem` rejects is an invalid path. A pattern
/// with no '*' or '?' is judged as a path. Any other pattern is allowed
/// only when its first folder is one the files layer may replace as a whole.
///
/// @param pattern the mask, as a mutator wrote it
/// @param context the layout and the unit-name test
/// @return the rule and a message that starts with `pattern`
[[nodiscard]] PathVerdict check_mutable_mask(std::string_view pattern, const PathContext& context);

/// Returns the stable lower-case name of a rule.
///
/// The names are for logs and tests: allowed, invalid-path, menus-only,
/// maps-only, read-once, not-confirmed, interface-art and unknown-folder.
///
/// @param rule the rule
/// @return the name, or "unknown" when `rule` is not an enumerator
[[nodiscard]] std::string_view rule_name(PathRule rule) noexcept;

} // namespace oa::data::mutators
