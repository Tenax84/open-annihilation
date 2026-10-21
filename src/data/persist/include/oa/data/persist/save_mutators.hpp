// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Mutators account of a savegame: the loadout text of a game played with
// mutators, kept as one blob. The text is stored as given and is not read
// here. A save written without calling for the account has no such account,
// so its bytes are unchanged.
#pragma once

#include "oa/data/persist/hapibank.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace oa::data::persist {

/// Account and blob names of the Mutators account.
namespace mutators_key {
/// The account a save uses for the loadout text.
inline constexpr std::string_view account = "Mutators";
/// The blob that holds the loadout text.
inline constexpr std::string_view loadout = "Loadout";
} // namespace mutators_key

/// Most bytes a loadout blob may hold. A longer text is refused on write and
/// reads as none.
inline constexpr std::size_t save_mutators_most_bytes = 64 * 1024;

/// Writes the Mutators account: one blob named Loadout holding the loadout text.
///
/// An empty text, or one longer than save_mutators_most_bytes, is refused.
/// The bytes are stored as given.
///
/// @param[in,out] bank the save being written; the account is left open
/// @param loadout_text the loadout, as text
/// @return false when the text is empty or over the limit, or the blob cannot be written
bool save_write_mutators(Bank* bank, std::string_view loadout_text);

/// Reads the Loadout blob of the Mutators account.
///
/// A save with no such account or blob, or a blob over save_mutators_most_bytes,
/// reads as none. A blob's size is checked before its bytes are copied.
///
/// @param[in,out] bank a save read whole; the account is left open when present
/// @return the loadout text, or nullopt when the save has none
[[nodiscard]] std::optional<std::string> save_read_mutators(Bank* bank);

} // namespace oa::data::persist
