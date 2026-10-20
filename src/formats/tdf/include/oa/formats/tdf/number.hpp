// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// One decimal spelling for every finite number a TDF value is written as.
#pragma once

#include <optional>
#include <string>

namespace oa::formats::tdf {

/// Formats a finite number as the one decimal spelling TDF writing uses.
///
/// Zero, of either sign, is `0`. A value that is a whole number of 65536ths,
/// which every integer and every 16.16 value is, is written as its exact
/// decimal: the whole part and at most 16 places, with no rounding. Any
/// other finite value is rounded to 15 significant digits, half away from
/// zero, and trailing zeros are dropped. The text is an optional minus,
/// digits and an optional point. Not-a-number and infinities have no text.
///
/// @param value the number
/// @return the spelling, or nullopt when value is not finite
[[nodiscard]] std::optional<std::string> format_number(double value);

} // namespace oa::formats::tdf
