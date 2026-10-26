// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The decimal of a finite binary value, spelled with integer arithmetic on
// its bits. A double's exact decimal has at most 767 significant digits, so
// a fixed base-10^9 buffer holds it. Every machine writes the same text.

#include "oa/formats/tdf/number.hpp"

#include <bit>
#include <cstdint>
#include <string>

namespace oa::formats::tdf {
namespace {

constexpr uint32_t limb_base = 1000000000u;
constexpr int limb_capacity = 96;
constexpr int digit_capacity = 800;
constexpr int significant_digits = 15;

/// A non-negative integer in base 10^9, least significant limb first.
struct Limbs {
    uint32_t limb[limb_capacity]{};
    int count = 0;
    bool overflow = false;

    /// Replaces the integer with an unsigned value.
    ///
    /// @param value value to store
    void set(uint64_t value) {
        count = 0;
        overflow = false;
        if (value == 0) {
            limb[0] = 0;
            count = 1;
            return;
        }
        while (value != 0) {
            limb[count++] = static_cast<uint32_t>(value % limb_base);
            value /= limb_base;
        }
    }

    /// Multiplies by a small factor.
    ///
    /// @param factor factor, 2 or 5
    void multiply(uint32_t factor) {
        uint64_t carry = 0;
        for (int index = 0; index < count; ++index) {
            const uint64_t product = uint64_t{limb[index]} * factor + carry;
            limb[index] = static_cast<uint32_t>(product % limb_base);
            carry = product / limb_base;
        }
        while (carry != 0) {
            if (count >= limb_capacity) {
                overflow = true;
                return;
            }
            limb[count++] = static_cast<uint32_t>(carry % limb_base);
            carry /= limb_base;
        }
    }
};

/// The significant digits of a decimal and where its point sits.
///
/// The value is the integer in `text` times ten to the power of `point - count`.
struct Digits {
    char text[digit_capacity]{};
    int count = 0;
    int point = 0;
};

/// Writes a limb integer's decimal digits, with no leading zeros.
///
/// @param magnitude integer to write
/// @param[out] digits digit buffer
/// @return the number of digits, or -1 when the buffer cannot hold them
[[nodiscard]] int write_digits(const Limbs& magnitude, char* digits) {
    int top = magnitude.count - 1;
    while (top > 0 && magnitude.limb[top] == 0)
        --top;
    uint32_t high = magnitude.limb[top];
    if (high == 0)
        return 0;
    char reversed[16];
    int reversed_count = 0;
    while (high != 0) {
        reversed[reversed_count++] = static_cast<char>('0' + high % 10u);
        high /= 10u;
    }
    int count = 0;
    while (reversed_count > 0) {
        if (count >= digit_capacity)
            return -1;
        digits[count++] = reversed[--reversed_count];
    }
    for (int index = top - 1; index >= 0; --index) {
        uint32_t part = magnitude.limb[index];
        char nine[9];
        for (int place = 8; place >= 0; --place) {
            nine[place] = static_cast<char>('0' + part % 10u);
            part /= 10u;
        }
        for (int place = 0; place < 9; ++place) {
            if (count >= digit_capacity)
                return -1;
            digits[count++] = nine[place];
        }
    }
    return count;
}

/// Rounds a digit string to 15 significant digits, half away from zero.
///
/// @param[in,out] digits digits to round; the point moves when 999... carries
void round_significant(Digits& digits) {
    if (digits.count <= significant_digits)
        return;
    const bool round_up = digits.text[significant_digits] >= '5';
    digits.count = significant_digits;
    if (!round_up)
        return;
    int index = significant_digits - 1;
    while (index >= 0 && digits.text[index] == '9') {
        digits.text[index] = '0';
        --index;
    }
    if (index >= 0) {
        digits.text[index] = static_cast<char>(digits.text[index] + 1);
        return;
    }
    digits.text[0] = '1';
    ++digits.point;
}

/// Spells digits with a point, dropping a bare point and trailing fractional zeros.
///
/// @param negative true to prefix a minus
/// @param digits digits to spell
/// @return the spelling; `0` when there are no digits
[[nodiscard]] std::string spell(bool negative, const Digits& digits) {
    if (digits.count <= 0)
        return "0";
    std::string text;
    if (negative)
        text.push_back('-');
    if (digits.point <= 0) {
        text += "0.";
        for (int zero = 0; zero < -digits.point; ++zero)
            text.push_back('0');
        int end = digits.count;
        while (end > 0 && digits.text[end - 1] == '0')
            --end;
        for (int index = 0; index < end; ++index)
            text.push_back(digits.text[index]);
        if (end == 0)
            return "0";
        return text;
    }
    if (digits.point >= digits.count) {
        for (int index = 0; index < digits.count; ++index)
            text.push_back(digits.text[index]);
        for (int zero = digits.count; zero < digits.point; ++zero)
            text.push_back('0');
        return text;
    }
    for (int index = 0; index < digits.point; ++index)
        text.push_back(digits.text[index]);
    int end = digits.count;
    while (end > digits.point && digits.text[end - 1] == '0')
        --end;
    if (end == digits.point)
        return text;
    text.push_back('.');
    for (int index = digits.point; index < end; ++index)
        text.push_back(digits.text[index]);
    return text;
}

} // namespace

std::optional<std::string> format_number(double value) {
    const auto bits = std::bit_cast<uint64_t>(value);
    const auto exponent_bits = static_cast<uint32_t>((bits >> 52) & 0x7ffu);
    const uint64_t fraction_bits = bits & ((uint64_t{1} << 52) - 1u);
    if (exponent_bits == 0x7ffu)
        return std::nullopt;
    if (exponent_bits == 0 && fraction_bits == 0)
        return std::string("0");

    const bool negative = (bits >> 63) != 0;
    uint64_t significand = 0;
    int power = 0;
    if (exponent_bits == 0) {
        significand = fraction_bits;
        power = -1074;
    } else {
        significand = fraction_bits | (uint64_t{1} << 52);
        power = static_cast<int>(exponent_bits) - 1023 - 52;
    }
    while ((significand & 1u) == 0u) {
        significand >>= 1u;
        ++power;
    }

    // value = significand * 2^power, with significand odd. It is a whole
    // number of 65536ths exactly when power is at least -16.
    Limbs magnitude;
    magnitude.set(significand);
    int scale = 0;
    if (power >= 0) {
        for (int step = 0; step < power; ++step)
            magnitude.multiply(2u);
    } else {
        scale = -power;
        for (int step = 0; step < scale; ++step)
            magnitude.multiply(5u);
    }
    if (magnitude.overflow)
        return std::nullopt;

    Digits digits;
    digits.count = write_digits(magnitude, digits.text);
    if (digits.count < 0)
        return std::nullopt;
    digits.point = digits.count - scale;
    if (power < -16)
        round_significant(digits);
    return spell(negative, digits);
}

} // namespace oa::formats::tdf
