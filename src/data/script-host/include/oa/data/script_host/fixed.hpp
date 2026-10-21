// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Fixed-point counts for scripts. One count is a number of 65536ths, the
// scale of the engine's 16.16 values, held in a signed 64-bit integer.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

struct lua_State;

namespace oa::data::script_host {

/// How many 65536ths make one whole unit.
///
/// This is the scale of `oa_fixed`. Multiplying by it is exact in a double.
inline constexpr int64_t fixed_scale = 65536;

/// Smallest count a fixed value may hold, −2^47.
inline constexpr int64_t fixed_count_min = -(int64_t{1} << 47);

/// Largest count a fixed value may hold, 2^47 − 1.
inline constexpr int64_t fixed_count_max = (int64_t{1} << 47) - 1;

/// Bytes enough for the text of any count in range, including the NUL.
inline constexpr std::size_t fixed_text_bytes = 48;

/// Reads text the way the unit loader reads a 16.16 field, without keeping
/// only the low 32 bits.
///
/// Leading spaces, one sign, digits, one point and an `e`, `E`, `d` or `D`
/// exponent are read by `parse_double`. The value times 65536 is truncated
/// toward zero. Text with no digits reads as zero. A result outside
/// [fixed_count_min, fixed_count_max], and a non-finite reading, is no value.
///
/// @param text NUL-terminated field text
/// @return the count, or no value outside the range
[[nodiscard]] std::optional<int64_t> fixed_from_text(const char* text) noexcept;

/// Converts a number by multiplying by 65536 and truncating toward zero.
///
/// @param value the number
/// @return the count, or no value when value is not finite or the count is
///         outside [fixed_count_min, fixed_count_max]
[[nodiscard]] std::optional<int64_t> fixed_from_number(double value) noexcept;

/// Adds two counts.
///
/// @param a first count
/// @param b second count
/// @return the sum, or no value when it is outside the fixed range
[[nodiscard]] std::optional<int64_t> fixed_add(int64_t a, int64_t b) noexcept;

/// Subtracts the second count from the first.
///
/// @param a count to subtract from
/// @param b count to subtract
/// @return the difference, or no value when it is outside the fixed range
[[nodiscard]] std::optional<int64_t> fixed_subtract(int64_t a, int64_t b) noexcept;

/// Negates a count.
///
/// Negating `fixed_count_min` does not fit: the largest count is one less
/// than its magnitude.
///
/// @param count count to negate
/// @return the negated count, or no value when it is outside the fixed range
[[nodiscard]] std::optional<int64_t> fixed_negate(int64_t count) noexcept;

/// Multiplies two counts and shifts the product right by 16.
///
/// A negative product rounds toward minus infinity, as `scale_vector_fixed`
/// does. The product is built from 32-bit limbs. A product that does not fit
/// in a signed 64-bit integer has no result.
///
/// @param a first count
/// @param b second count
/// @return the shifted product, or no value when it is outside the fixed range
[[nodiscard]] std::optional<int64_t> fixed_multiply(int64_t a, int64_t b) noexcept;

/// Divides two counts as `(a << 16) / b`, truncated toward zero.
///
/// @param a dividend count
/// @param b divisor count
/// @return the quotient, or no value for a zero divisor or a quotient outside
///         the fixed range
[[nodiscard]] std::optional<int64_t> fixed_divide(int64_t a, int64_t b) noexcept;

/// Returns the whole part at or below the count, toward minus infinity.
///
/// This is the count shifted right by 16. The result is a whole number, not
/// a count.
///
/// @param count count to shift
/// @return the whole part
[[nodiscard]] int64_t fixed_whole_floor(int64_t count) noexcept;

/// Returns the whole part at or above the count.
///
/// An exact whole number stays that number. Any other count moves one whole
/// toward positive infinity from `fixed_whole_floor`. The result is a whole
/// number, not a count.
///
/// @param count count to raise
/// @return the whole part
[[nodiscard]] int64_t fixed_whole_ceil(int64_t count) noexcept;

/// Rounds a count to a whole number, half away from zero.
///
/// A fraction of 32768 65536ths, or more, moves one whole away from zero.
/// The result is a whole number, not a count.
///
/// @param count count to round
/// @return the whole number
[[nodiscard]] int64_t fixed_whole_round(int64_t count) noexcept;

/// Writes `format_number` of `count / 65536` into `out`.
///
/// A count in range is a whole number of 65536ths, so the text is the exact
/// decimal. Zero is `0`.
///
/// @param count count to write
/// @param out buffer that receives the text, NUL terminated
/// @param size bytes available at out
/// @return false when out is missing, size is zero, or the text does not fit
[[nodiscard]] bool fixed_text(int64_t count, char* out, std::size_t size) noexcept;

/// Reads a fixed value at `index` without raising.
///
/// A value that is not a fixed userdata leaves `count` unchanged and returns
/// false. The library does not have to be open: a foreign userdata is false.
///
/// @param state sandbox state
/// @param index stack index
/// @param count receives the count when the value is fixed
/// @return true when index holds a fixed value
[[nodiscard]] bool to_fixed(lua_State* state, int index, int64_t* count) noexcept;

/// Pushes a fixed value holding `count`.
///
/// Allocates a userdata, so this may raise. `count` is stored as given.
///
/// @param state sandbox state
/// @param count count to store
void push_fixed(lua_State* state, int64_t count);

} // namespace oa::data::script_host
