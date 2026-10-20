// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/data/script_host/deterministic_math.hpp"

#include "oa/base/game_math/extended.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>

namespace oa::data::script_host {
namespace {

using oa::base::game_math::Extended;
using oa::base::game_math::Precision;

// ln 2, 1/ln 2 and 1/ln 10, each rounded once to 64 bits. ln 2 is also split
// into 30 leading bits and the rest, so a small integer times the leading
// part is exact and the reduction x - n ln 2 loses no bits.
constexpr Extended ln2{0xb17217f7d1cf79acULL, -1, false};
constexpr Extended ln2_hi{0xb17217f400000000ULL, -1, false};
constexpr Extended ln2_lo{0xf473de6af278ece6ULL, -31, false};
constexpr Extended inv_ln2{0xb8aa3b295c17f0bcULL, 0, false};
constexpr Extended inv_ln10{0xde5bd8a937287195ULL, -2, false};

// frexp mantissa below this moves into [1/sqrt(2), sqrt(2)], where the
// logarithm series converges quickly. The threshold is the double of 1/sqrt(2).
constexpr double sqrt_half = 0x1.6a09e667f3bcdp-1;

constexpr double pi = 0x1.921fb54442d18p+1;
constexpr double half_pi = 0x1.921fb54442d18p+0;
constexpr double quarter_pi = 0x1.921fb54442d18p-1;
constexpr double three_quarter_pi = 0x1.2d97c7f3321d2p+1;

// exp(710) overflows a double and exp(-746) underflows to zero, both with
// room to spare past the real edges, so the series never sees those inputs.
constexpr double exponential_overflow = 710.0;
constexpr double exponential_underflow = -746.0;

constexpr int exponential_terms = 20;
constexpr int logarithm_terms = 20;

// A product whose exponents sum past this cannot be a finite double, and the
// sum would not fit in the 32-bit exponent field.
constexpr int64_t exponent_limit = 8192;

enum class Magnitude : int8_t { finite, overflow, underflow };

struct Scaled {
    Extended value{};
    Magnitude magnitude{Magnitude::finite};
};

// Two 64-bit parts. The value is high + low, with low much smaller than high.
struct Pair {
    Extended high{};
    Extended low{};
};

uint64_t bits_of(double value) noexcept {
    return std::bit_cast<uint64_t>(value);
}

uint64_t magnitude_bits(double value) noexcept {
    return bits_of(value) & 0x7fffffffffffffffULL;
}

bool is_nan(double value) noexcept {
    return magnitude_bits(value) > 0x7ff0000000000000ULL;
}

bool is_infinite(double value) noexcept {
    return magnitude_bits(value) == 0x7ff0000000000000ULL;
}

bool is_finite(double value) noexcept {
    return magnitude_bits(value) < 0x7ff0000000000000ULL;
}

bool is_zero(double value) noexcept {
    return magnitude_bits(value) == 0;
}

bool is_negative(double value) noexcept {
    return (bits_of(value) >> 63) != 0;
}

double quiet_nan() noexcept {
    return std::bit_cast<double>(uint64_t{0x7ff8000000000001ULL});
}

double infinity(bool negative) noexcept {
    const uint64_t sign = negative ? uint64_t{1} << 63 : 0;
    return std::bit_cast<double>(sign | 0x7ff0000000000000ULL);
}

double signed_zero(bool negative) noexcept {
    return std::bit_cast<double>(negative ? uint64_t{1} << 63 : 0);
}

double absolute_value(double value) noexcept {
    return std::bit_cast<double>(magnitude_bits(value));
}

bool angle_in_range(double radians) noexcept {
    return is_finite(radians) && magnitude_bits(radians) < magnitude_bits(largest_angle);
}

Extended add64(Extended left, Extended right) noexcept {
    return oa::base::game_math::add(left, right, Precision::bits_64);
}

Extended sub64(Extended left, Extended right) noexcept {
    return oa::base::game_math::subtract(left, right, Precision::bits_64);
}

Extended mul64(Extended left, Extended right) noexcept {
    return oa::base::game_math::multiply(left, right, Precision::bits_64);
}

Extended div64(Extended left, Extended right) noexcept {
    return oa::base::game_math::divide(left, right, Precision::bits_64);
}

Extended one() noexcept {
    return oa::base::game_math::to_extended(1.0);
}

Extended zero() noexcept {
    return oa::base::game_math::to_extended(0.0);
}

// Nearest integer, ties to even. The argument of the exponential reduction
// is only a few thousand, so the result fits in a small integer.
int32_t round_even(Extended value) noexcept {
    if (value.significand == 0 || value.exponent < -1)
        return 0;
    if (value.exponent >= 62)
        return value.negative ? -1000000 : 1000000;
    uint64_t whole = 0;
    if (value.exponent == -1) {
        const uint64_t rest = value.significand & ((uint64_t{1} << 63) - 1);
        whole = rest != 0 ? 1 : 0;
    } else {
        const int shift = 63 - value.exponent;
        whole = value.significand >> static_cast<unsigned>(shift);
        const uint64_t half = uint64_t{1} << static_cast<unsigned>(shift - 1);
        const uint64_t mask = (uint64_t{1} << static_cast<unsigned>(shift)) - 1;
        const uint64_t remainder = value.significand & mask;
        if (remainder > half || (remainder == half && (whole & 1) != 0))
            ++whole;
    }
    if (whole > 1000000)
        whole = 1000000;
    const auto magnitude = static_cast<int32_t>(whole);
    return value.negative ? -magnitude : magnitude;
}

Pair split32(Extended value) noexcept {
    if (value.significand == 0)
        return {value, Extended{0, 0, value.negative}};
    Extended high = value;
    high.significand &= ~uint64_t{0xffffffffULL};
    return {high, sub64(value, high)};
}

Pair two_sum(Extended left, Extended right) noexcept {
    const Extended sum = add64(left, right);
    const Extended right_prime = sub64(sum, right);
    const Extended left_prime = sub64(sum, right_prime);
    const Extended left_error = sub64(left, right_prime);
    const Extended right_error = sub64(right, left_prime);
    return {sum, add64(left_error, right_error)};
}

Pair two_prod(Extended left, Extended right) noexcept {
    const Extended product = mul64(left, right);
    const Pair left_parts = split32(left);
    const Pair right_parts = split32(right);
    const Extended high_high = mul64(left_parts.high, right_parts.high);
    const Extended low_high = mul64(left_parts.low, right_parts.high);
    const Extended high_low = mul64(left_parts.high, right_parts.low);
    const Extended low_low = mul64(left_parts.low, right_parts.low);
    const Extended error =
        sub64(low_low, sub64(sub64(sub64(product, high_high), low_high), high_low));
    return {product, error};
}

Pair dd_add(Pair left, Pair right) noexcept {
    const Pair sum = two_sum(left.high, right.high);
    const Extended extra = add64(sum.low, add64(left.low, right.low));
    return two_sum(sum.high, extra);
}

Pair dd_mul(Pair left, Pair right) noexcept {
    const Pair product = two_prod(left.high, right.high);
    const Extended cross = add64(mul64(left.high, right.low), mul64(left.low, right.high));
    return two_sum(product.high, add64(product.low, cross));
}

Pair dd_div(Extended numerator, Extended denominator) noexcept {
    const Extended quotient = div64(numerator, denominator);
    const Pair product = two_prod(quotient, denominator);
    const Extended difference = sub64(sub64(numerator, product.high), product.low);
    return two_sum(quotient, div64(difference, denominator));
}

Pair scale2(Pair value, int32_t steps) noexcept {
    if (value.high.significand != 0)
        value.high.exponent += steps;
    if (value.low.significand != 0)
        value.low.exponent += steps;
    return value;
}

Extended exp_extended(Extended argument) noexcept {
    const int32_t steps = round_even(mul64(argument, inv_ln2));
    const Extended count = oa::base::game_math::to_extended(steps);
    Extended reduced = sub64(argument, mul64(count, ln2_hi));
    reduced = sub64(reduced, mul64(count, ln2_lo));
    std::array<Extended, exponential_terms + 1> coefficient{};
    coefficient[0] = one();
    Extended inverse = one();
    for (int term = 1; term <= exponential_terms; ++term) {
        inverse = div64(inverse, oa::base::game_math::to_extended(term));
        coefficient[static_cast<std::size_t>(term)] = inverse;
    }
    Extended polynomial = coefficient[exponential_terms];
    for (int term = exponential_terms - 1; term >= 0; --term) {
        polynomial = add64(mul64(polynomial, reduced), coefficient[static_cast<std::size_t>(term)]);
    }
    if (polynomial.significand != 0)
        polynomial.exponent += steps;
    return polynomial;
}

// Natural log of a finite positive double, kept at 64 bits.
Extended log_extended(double value) noexcept {
    int binade = 0;
    double mantissa = std::frexp(value, &binade);
    if (mantissa < sqrt_half) {
        mantissa *= 2.0;
        --binade;
    }
    const Extended wide = oa::base::game_math::to_extended(mantissa);
    const Extended unit = one();
    const Extended ratio = div64(sub64(wide, unit), add64(wide, unit));
    const Extended square = mul64(ratio, ratio);
    const int last = 2 * (logarithm_terms - 1) + 1;
    Extended polynomial = div64(unit, oa::base::game_math::to_extended(last));
    for (int term = logarithm_terms - 2; term >= 0; --term) {
        const int denominator = 2 * term + 1;
        const Extended coefficient = div64(unit, oa::base::game_math::to_extended(denominator));
        polynomial = add64(mul64(polynomial, square), coefficient);
    }
    Extended logarithm = mul64(ratio, polynomial);
    if (logarithm.significand != 0)
        logarithm.exponent += 1;
    const Extended count = oa::base::game_math::to_extended(binade);
    const Extended high = mul64(count, ln2_hi);
    const Extended low = add64(mul64(count, ln2_lo), logarithm);
    return add64(high, low);
}

// Natural log as two 64-bit parts, for a power that is not a small integer.
Pair log_pair(double value) noexcept {
    int binade = 0;
    double mantissa = std::frexp(value, &binade);
    if (mantissa < sqrt_half) {
        mantissa *= 2.0;
        --binade;
    }
    const Extended wide = oa::base::game_math::to_extended(mantissa);
    const Extended unit = one();
    const Pair ratio = dd_div(sub64(wide, unit), add64(wide, unit));
    const Pair square = dd_mul(ratio, ratio);
    const int last = 2 * (logarithm_terms - 1) + 1;
    Pair polynomial{div64(unit, oa::base::game_math::to_extended(last)), zero()};
    for (int term = logarithm_terms - 2; term >= 0; --term) {
        const int denominator = 2 * term + 1;
        const Pair coefficient{div64(unit, oa::base::game_math::to_extended(denominator)), zero()};
        polynomial = dd_add(coefficient, dd_mul(square, polynomial));
    }
    const Pair doubled = scale2(ratio, 1);
    const Pair logarithm = dd_mul(doubled, polynomial);
    const Pair count{oa::base::game_math::to_extended(binade), zero()};
    const Pair scaled = dd_mul(count, Pair{ln2_hi, ln2_lo});
    return dd_add(scaled, logarithm);
}

double exp_pair(Pair argument) noexcept {
    const double approx = oa::base::game_math::to_double(argument.high);
    if (approx > exponential_overflow)
        return infinity(false);
    if (approx < exponential_underflow)
        return 0.0;
    const int32_t steps = round_even(mul64(argument.high, inv_ln2));
    const Extended count = oa::base::game_math::to_extended(steps);
    const Pair reduction{
        oa::base::game_math::negate(mul64(count, ln2_hi)),
        oa::base::game_math::negate(mul64(count, ln2_lo))
    };
    const Pair reduced = dd_add(argument, reduction);
    std::array<Extended, exponential_terms + 1> coefficient{};
    coefficient[0] = one();
    Extended inverse = one();
    for (int term = 1; term <= exponential_terms; ++term) {
        inverse = div64(inverse, oa::base::game_math::to_extended(term));
        coefficient[static_cast<std::size_t>(term)] = inverse;
    }
    Pair polynomial{coefficient[exponential_terms], zero()};
    for (int term = exponential_terms - 1; term >= 0; --term) {
        const Pair weight{coefficient[static_cast<std::size_t>(term)], zero()};
        polynomial = dd_add(weight, dd_mul(reduced, polynomial));
    }
    const Pair scaled = scale2(polynomial, steps);
    const Pair sum = two_sum(scaled.high, scaled.low);
    return oa::base::game_math::to_double(add64(sum.high, sum.low));
}

Scaled multiply_scaled(Scaled left, Scaled right) noexcept {
    if (left.magnitude == Magnitude::underflow || right.magnitude == Magnitude::underflow)
        return {{}, Magnitude::underflow};
    if (left.magnitude == Magnitude::overflow || right.magnitude == Magnitude::overflow)
        return {{}, Magnitude::overflow};
    if (left.value.significand == 0 || right.value.significand == 0)
        return {};
    const int64_t span = int64_t{left.value.exponent} + int64_t{right.value.exponent};
    if (span > exponent_limit)
        return {{}, Magnitude::overflow};
    if (span < -exponent_limit)
        return {{}, Magnitude::underflow};
    return {mul64(left.value, right.value), Magnitude::finite};
}

double finish_signed(Scaled value, bool reciprocal, bool negative) noexcept {
    if (reciprocal) {
        if (value.magnitude == Magnitude::overflow || value.value.significand == 0)
            return infinity(negative);
        if (value.magnitude == Magnitude::underflow)
            return signed_zero(negative);
        if (value.value.exponent < -2048)
            return infinity(negative);
        if (value.value.exponent > 2048)
            return signed_zero(negative);
        const int64_t span = int64_t{one().exponent} - int64_t{value.value.exponent};
        if (span > exponent_limit)
            return infinity(negative);
        if (span < -exponent_limit)
            return signed_zero(negative);
        value.value = div64(one(), value.value);
    } else if (value.magnitude == Magnitude::overflow) {
        return infinity(negative);
    } else if (value.magnitude == Magnitude::underflow) {
        return signed_zero(negative);
    }
    double result = oa::base::game_math::to_double(value.value);
    if (negative)
        result = -result;
    return result;
}

double integer_power(double base, int64_t exponent) noexcept {
    const bool negative = base < 0.0 && (exponent & 1) != 0;
    const bool reciprocal = exponent < 0;
    uint64_t remaining =
        exponent < 0 ? static_cast<uint64_t>(-exponent) : static_cast<uint64_t>(exponent);
    Scaled factor{oa::base::game_math::to_extended(absolute_value(base)), Magnitude::finite};
    Scaled result{one(), Magnitude::finite};
    while (remaining > 0) {
        if ((remaining & 1) != 0)
            result = multiply_scaled(result, factor);
        remaining >>= 1;
        if (remaining > 0)
            factor = multiply_scaled(factor, factor);
    }
    return finish_signed(result, reciprocal, negative);
}

double huge_integer_power(double base, double exponent) noexcept {
    // Every integer at or above 2^63 in magnitude is even.
    const bool reciprocal = exponent < 0.0;
    const double magnitude = absolute_value(base);
    if (magnitude > 1.0)
        return reciprocal ? 0.0 : infinity(false);
    if (magnitude == 1.0)
        return 1.0;
    return reciprocal ? infinity(false) : 0.0;
}

bool integer_exponent(double exponent, int64_t& slot) noexcept {
    if (!is_finite(exponent) || exponent != std::floor(exponent))
        return false;
    constexpr double limit = 9223372036854775808.0;
    if (exponent >= limit || exponent <= -limit)
        return false;
    slot = static_cast<int64_t>(exponent);
    return true;
}

bool odd_integer(double exponent) noexcept {
    int64_t slot = 0;
    return integer_exponent(exponent, slot) && (slot & 1) != 0;
}

constexpr std::array<double, 23> powers_of_ten = [] {
    std::array<double, 23> powers{};
    powers[0] = 1.0;
    for (int index = 1; index <= 22; ++index)
        powers[static_cast<std::size_t>(index)] =
            powers[static_cast<std::size_t>(index - 1)] * 10.0;
    return powers;
}();

double signed_pi(bool negative) noexcept {
    return negative ? -pi : pi;
}

double signed_half_pi(bool negative) noexcept {
    return negative ? -half_pi : half_pi;
}

double signed_quarter_pi(bool negative) noexcept {
    return negative ? -quarter_pi : quarter_pi;
}

double signed_three_quarter_pi(bool negative) noexcept {
    return negative ? -three_quarter_pi : three_quarter_pi;
}

double inverse_with_root(double value, bool arc_sine) noexcept {
    const Extended wide = oa::base::game_math::to_extended(value);
    const Extended unit = one();
    const Extended product = mul64(sub64(unit, wide), add64(unit, wide));
    const bool negative_root = product.negative && product.significand != 0;
    const Extended root =
        negative_root ? zero() : oa::base::game_math::square_root(product, Precision::bits_64);
    const double adjacent = oa::base::game_math::to_double(root);
    // asin(x) = atan2(x, sqrt(1-x^2)); acos(x) = atan2(sqrt(1-x^2), x).
    Extended angle = arc_sine ? oa::base::game_math::arctangent(value, adjacent)
                              : oa::base::game_math::arctangent(adjacent, value);
    const Extended delta = sub64(root, oa::base::game_math::to_extended(adjacent));
    if (delta.significand == 0)
        return oa::base::game_math::to_double(angle);
    const Extended y_wide = oa::base::game_math::to_extended(adjacent);
    const Extended denominator = add64(mul64(wide, wide), mul64(y_wide, y_wide));
    if (denominator.significand == 0)
        return oa::base::game_math::to_double(angle);
    // The root passed to arctangent was rounded to a double. Put that
    // rounding back before the result itself is rounded.
    const Extended slope = arc_sine ? oa::base::game_math::negate(wide) : wide;
    const Extended correction = div64(mul64(slope, delta), denominator);
    return oa::base::game_math::to_double(add64(angle, correction));
}

double power_from_log(double magnitude, double exponent) noexcept {
    const Pair logarithm = log_pair(magnitude);
    const Pair product =
        dd_mul(Pair{oa::base::game_math::to_extended(exponent), zero()}, logarithm);
    return exp_pair(product);
}

} // namespace

double sine(double radians) noexcept {
    if (is_nan(radians))
        return radians;
    if (!angle_in_range(radians))
        return quiet_nan();
    return oa::base::game_math::to_double(oa::base::game_math::sine(radians));
}

double cosine(double radians) noexcept {
    if (is_nan(radians))
        return radians;
    if (!angle_in_range(radians))
        return quiet_nan();
    return oa::base::game_math::to_double(oa::base::game_math::cosine(radians));
}

double tangent(double radians) noexcept {
    if (is_nan(radians))
        return radians;
    if (!angle_in_range(radians))
        return quiet_nan();
    const oa::base::game_math::SineCosine pair = oa::base::game_math::sine_cosine(radians);
    if (pair.cosine.significand == 0)
        return quiet_nan();
    return oa::base::game_math::to_double(div64(pair.sine, pair.cosine));
}

double arcsine(double value) noexcept {
    if (is_nan(value))
        return value;
    if (!is_finite(value) || value > 1.0 || value < -1.0)
        return quiet_nan();
    return inverse_with_root(value, true);
}

double arccosine(double value) noexcept {
    if (is_nan(value))
        return value;
    if (!is_finite(value) || value > 1.0 || value < -1.0)
        return quiet_nan();
    return inverse_with_root(value, false);
}

double arctangent(double y, double x) noexcept {
    if (is_nan(y))
        return y;
    if (is_nan(x))
        return x;
    const bool y_negative = is_negative(y);
    const bool x_negative = is_negative(x);
    if (is_infinite(y) && is_infinite(x))
        return x_negative ? signed_three_quarter_pi(y_negative) : signed_quarter_pi(y_negative);
    if (is_infinite(y))
        return signed_half_pi(y_negative);
    if (is_infinite(x))
        return x_negative ? signed_pi(y_negative) : signed_zero(y_negative);
    return oa::base::game_math::to_double(oa::base::game_math::arctangent(y, x));
}

double exponential(double value) noexcept {
    if (is_nan(value))
        return value;
    if (is_infinite(value))
        return value > 0.0 ? infinity(false) : 0.0;
    if (value > exponential_overflow)
        return infinity(false);
    if (value < exponential_underflow)
        return 0.0;
    return oa::base::game_math::to_double(exp_extended(oa::base::game_math::to_extended(value)));
}

double natural_log(double value) noexcept {
    if (is_nan(value))
        return value;
    if (is_zero(value))
        return -infinity(false);
    if (value < 0.0)
        return quiet_nan();
    if (is_infinite(value))
        return infinity(false);
    return oa::base::game_math::to_double(log_extended(value));
}

double log2(double value) noexcept {
    if (is_nan(value))
        return value;
    if (is_zero(value))
        return -infinity(false);
    if (value < 0.0)
        return quiet_nan();
    if (is_infinite(value))
        return infinity(false);
    int binade = 0;
    const double mantissa = std::frexp(value, &binade);
    if (mantissa == 0.5)
        return static_cast<double>(binade - 1);
    double moved = mantissa;
    int exponent = binade;
    if (moved < sqrt_half) {
        moved *= 2.0;
        --exponent;
    }
    // log2(m * 2^e) = ln(m) / ln 2 + e. ln(m) is already the full logarithm.
    const Extended ratio = div64(log_extended(moved), ln2);
    return oa::base::game_math::to_double(add64(ratio, oa::base::game_math::to_extended(exponent)));
}

double log10(double value) noexcept {
    if (is_nan(value))
        return value;
    if (is_zero(value))
        return -infinity(false);
    if (value < 0.0)
        return quiet_nan();
    if (is_infinite(value))
        return infinity(false);
    for (int index = 0; index <= 22; ++index) {
        if (value == powers_of_ten[static_cast<std::size_t>(index)])
            return static_cast<double>(index);
    }
    return oa::base::game_math::to_double(mul64(log_extended(value), inv_ln10));
}

double power(double base, double exponent) noexcept {
    if (is_zero(exponent))
        return 1.0;
    if (base == 1.0)
        return 1.0;
    if (is_nan(base))
        return base;
    if (is_nan(exponent))
        return exponent;
    if (is_zero(base)) {
        const bool odd = odd_integer(exponent);
        if (exponent < 0.0)
            return odd ? infinity(is_negative(base)) : infinity(false);
        return odd ? signed_zero(is_negative(base)) : 0.0;
    }
    if (is_infinite(exponent)) {
        if (base == -1.0)
            return 1.0;
        const double magnitude = absolute_value(base);
        if (exponent < 0.0)
            return magnitude < 1.0 ? infinity(false) : 0.0;
        return magnitude < 1.0 ? 0.0 : infinity(false);
    }
    if (is_infinite(base)) {
        const bool odd = odd_integer(exponent);
        if (exponent < 0.0)
            return odd && is_negative(base) ? signed_zero(true) : 0.0;
        return odd && is_negative(base) ? infinity(true) : infinity(false);
    }
    if (exponent == 2.0)
        return base * base;
    if (exponent == 0.5)
        return base < 0.0 ? quiet_nan() : std::sqrt(base);
    int64_t whole = 0;
    if (integer_exponent(exponent, whole)) {
        const int64_t magnitude = whole < 0 ? -whole : whole;
        // Past this magnitude each extra squaring doubles the error, and the
        // result leaves the eighth of a unit the goldens rely on.
        if (magnitude <= repeated_squaring_limit)
            return integer_power(base, whole);
        const double raised = power_from_log(absolute_value(base), exponent);
        if (is_negative(base) && (whole & 1) != 0)
            return is_zero(raised) ? signed_zero(true) : -raised;
        return raised;
    }
    if (exponent == std::floor(exponent))
        return huge_integer_power(base, exponent);
    if (base < 0.0)
        return quiet_nan();
    return power_from_log(base, exponent);
}

} // namespace oa::data::script_host

extern "C" double oa_script_host_power(double base, double exponent) {
    return oa::data::script_host::power(base, exponent);
}
