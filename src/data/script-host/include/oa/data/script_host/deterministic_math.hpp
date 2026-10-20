// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Sine, cosine, exponential, logarithm and power for scripts. Each result is
// the same bits on every platform. The angle, root and logarithm reductions
// use the engine's 64-bit arithmetic; square root, and the four basic
// operations on ordinary doubles, stay with IEEE 754.
#pragma once

namespace oa::data::script_host {

/// Largest angle, in radians, accepted by `sine`, `cosine` and `tangent`.
///
/// The Lua functions raise at and beyond this magnitude. The functions here
/// return a quiet NaN there, and for a non-finite angle, so they never pass
/// an out-of-range angle to the engine.
inline constexpr double largest_angle = 2147483648.0;

/// Largest magnitude of an integer exponent raised by repeated squaring.
///
/// Squaring at 64 bits stays inside an eighth of a unit through this
/// magnitude. A larger integer exponent is `exp(y · log x)` on two 64-bit
/// words, and a negative base keeps the sign an odd integer calls for.
inline constexpr int repeated_squaring_limit = 256;

/// Returns the sine of an angle.
///
/// A NaN returns that NaN. An infinity, or a magnitude at or above
/// `largest_angle`, returns a quiet NaN. The sine of a zero is that zero.
///
/// @param radians angle in radians
/// @return the sine
[[nodiscard]] double sine(double radians) noexcept;

/// Returns the cosine of an angle.
///
/// A NaN returns that NaN. An infinity, or a magnitude at or above
/// `largest_angle`, returns a quiet NaN.
///
/// @param radians angle in radians
/// @return the cosine
[[nodiscard]] double cosine(double radians) noexcept;

/// Returns the tangent of an angle.
///
/// A NaN returns that NaN. An infinity, a magnitude at or above
/// `largest_angle`, or an angle whose cosine is zero, returns a quiet NaN.
/// The tangent of a zero is that zero.
///
/// @param radians angle in radians
/// @return the tangent
[[nodiscard]] double tangent(double radians) noexcept;

/// Returns the arc sine of a number.
///
/// A NaN returns that NaN. A value outside [-1, 1], including an infinity,
/// returns a quiet NaN. The arc sine of a zero is that zero.
///
/// @param value sine of the angle
/// @return the angle in radians, in [-pi/2, pi/2]
[[nodiscard]] double arcsine(double value) noexcept;

/// Returns the arc cosine of a number.
///
/// A NaN returns that NaN. A value outside [-1, 1], including an infinity,
/// returns a quiet NaN.
///
/// @param value cosine of the angle
/// @return the angle in radians, in [0, pi]
[[nodiscard]] double arccosine(double value) noexcept;

/// Returns the angle of the point (x, y) from the positive x axis.
///
/// Follows C99 Annex F for zeros and infinities: a zero y gives a zero of
/// y's sign when x is positive or +0, and pi of y's sign when x is negative
/// or -0; a zero x with a non-zero finite y gives pi/2 of y's sign; infinite
/// y with a finite x gives pi/2 of y's sign; infinite x with a finite y gives
/// a zero of y's sign when x is +infinity and pi of y's sign when x is
/// -infinity; the four infinity pairs give pi/4, 3pi/4 and their negatives.
/// A NaN argument returns that NaN, or the other argument when that one is a
/// NaN too.
///
/// @param y ordinate; may be infinite or NaN
/// @param x abscissa; may be infinite or NaN
/// @return the angle in radians, in [-pi, pi]
[[nodiscard]] double arctangent(double y, double x) noexcept;

/// Returns e raised to a power.
///
/// A NaN returns that NaN. +infinity returns +infinity and -infinity returns
/// +0. A finite value above the overflow edge returns +infinity, and one
/// below the underflow edge returns +0.
///
/// @param value exponent
/// @return e^value
[[nodiscard]] double exponential(double value) noexcept;

/// Returns the natural logarithm.
///
/// A NaN returns that NaN. A negative number returns a quiet NaN. A zero,
/// of either sign, returns -infinity. +infinity returns +infinity.
///
/// @param value argument; a negative value is out of range
/// @return ln(value)
[[nodiscard]] double natural_log(double value) noexcept;

/// Returns the base-2 logarithm.
///
/// A power of two, including a subnormal one, returns its exponent exactly.
/// Every other special value matches `natural_log`.
///
/// @param value argument; a negative value is out of range
/// @return log2(value)
[[nodiscard]] double log2(double value) noexcept;

/// Returns the base-10 logarithm.
///
/// 10^k for k from 0 to 22 returns k exactly. Every other special value
/// matches `natural_log`.
///
/// @param value argument; a negative value is out of range
/// @return log10(value)
[[nodiscard]] double log10(double value) noexcept;

/// Returns a base raised to an exponent, following C99 Annex F.
///
/// An exponent of +0 or -0 returns +1 for every base, including a NaN.
/// A base of +1 returns +1 for every exponent, including a NaN. A negative
/// finite base with a finite non-integer exponent returns a quiet NaN.
/// A zero base with a negative exponent returns an infinity, signed only
/// when the exponent is an odd integer. A zero base with a positive exponent
/// returns a zero, signed only when the exponent is an odd integer.
/// An infinite exponent returns +infinity or +0 from the magnitude of the
/// base, and +1 when the base is -1. An infinite base follows the odd-integer
/// sign rows of Annex F.
///
/// @param base base of the power
/// @param exponent exponent of the power
/// @return base^exponent
[[nodiscard]] double power(double base, double exponent) noexcept;

} // namespace oa::data::script_host
