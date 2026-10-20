// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Provisional: this header changes whenever OA's screens need it to, until the kit is declared stable (src/ui/kit/README.md).

// A catalogue tag's icon. The kit maps an id to an icon and never a label:
// the caller already holds the translated word. looks.hpp and chrome.hpp
// both include this header, so a nav entry can name an icon without a cycle.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace oa::ui::kit {

/// One icon of the catalogue's tag list. The order is the list's order.
enum class TagIcon : uint8_t {
    statistics, ///< the id statistics
    economy,    ///< the id economy
    commanders, ///< the id commanders
    new_units,  ///< the id new-units
    disable,    ///< the id disable
    gameplay,   ///< the id gameplay
    ai,         ///< the id ai
    maps,       ///< the id maps
};

/// Every tag icon, in the order TagIcon declares them.
inline constexpr std::array all_tag_icons{
    TagIcon::statistics,
    TagIcon::economy,
    TagIcon::commanders,
    TagIcon::new_units,
    TagIcon::disable,
    TagIcon::gameplay,
    TagIcon::ai,
    TagIcon::maps,
};

/// The catalogue id of each tag icon, in TagIcon's order, matched exactly.
inline constexpr std::array<std::string_view, all_tag_icons.size()> tag_ids{
    "statistics",
    "economy",
    "commanders",
    "new-units",
    "disable",
    "gameplay",
    "ai",
    "maps",
};

/// Returns the icon of a catalogue tag id.
///
/// The id matches exactly as the catalogue writes it, lower-case and
/// kebab-case. Any other id, including a different case, gives none.
///
/// @param id the catalogue id
/// @return the icon, or none
[[nodiscard]] inline std::optional<TagIcon> tag_icon_of(std::string_view id) noexcept {
    for (std::size_t index = 0; index < tag_ids.size(); ++index)
        if (tag_ids[index] == id)
            return all_tag_icons[index];
    return std::nullopt;
}

/// Returns a tag icon's catalogue id.
///
/// @param icon the icon
/// @return its id; empty when the value is not one of the eight
[[nodiscard]] inline std::string_view tag_id(TagIcon icon) noexcept {
    const auto index = static_cast<std::size_t>(icon);
    if (index >= tag_ids.size())
        return {};
    return tag_ids[index];
}

} // namespace oa::ui::kit
