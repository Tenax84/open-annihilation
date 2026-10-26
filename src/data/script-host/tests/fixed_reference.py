# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Golden counts for the script host's fixed values. Run with python3 -I.
# The stdout is the table in fixed_test.cpp, from the BEGIN line through the
# END line. Counts are Python integers. A number or a field text is Python's
# float() of the same grammar the loader reads, times 65536, truncated toward
# zero. Nothing here calls the engine.

import math
import sys

SCALE = 65536
COUNT_MIN = -(1 << 47)
COUNT_MAX = (1 << 47) - 1
INT64_MIN = -1 << 63
INT64_MAX = (1 << 63) - 1
WHOLE_MIN = -2147483648
WHOLE_MAX = 2147483647
RANGE_LIMIT = 9223372036854775808.0  # 2^63, the truncating conversion's edge

rows = []
seen = set()


def add(op, left, right, result):
    row = (op, str(left), str(right), "error" if result is None else str(result))
    if row in seen:
        return
    for part in row:
        if any(ch in part for ch in "\"\\\n\r{};"):
            print(f"unsafe golden text {part!r}", file=sys.stderr)
            sys.exit(1)
    seen.add(row)
    rows.append(row)


def trunc_div(numerator, divisor):
    quotient, remainder = divmod(numerator, divisor)
    if remainder != 0 and (numerator < 0) != (divisor < 0):
        quotient += 1
    return quotient


def truncate_to_int64(value):
    if not (value >= -RANGE_LIMIT and value < RANGE_LIMIT):
        return INT64_MIN
    return int(value)


def from_number(value):
    if not math.isfinite(value):
        return None
    count = truncate_to_int64(value * float(SCALE))
    if count < COUNT_MIN or count > COUNT_MAX:
        return None
    return count


def parse_loader(text):
    """The loader's number grammar, then Python's float()."""
    index = 0
    while index < len(text) and (text[index] == " " or "\t" <= text[index] <= "\r"):
        index += 1
    buffer = []

    def put(char):
        if len(buffer) + 1 < 512:
            buffer.append(char)

    if index < len(text) and text[index] in "+-":
        put(text[index])
        index += 1
    digits = False
    while index < len(text) and text[index].isdigit():
        put(text[index])
        index += 1
        digits = True
    if index < len(text) and text[index] == ".":
        put(".")
        index += 1
        while index < len(text) and text[index].isdigit():
            put(text[index])
            index += 1
            digits = True
    if not digits:
        return 0.0
    if index < len(text) and text[index] in "eEdD":
        exponent = index + 1
        if exponent < len(text) and text[exponent] in "+-":
            exponent += 1
        if exponent < len(text) and text[exponent].isdigit():
            put("e")
            index += 1
            if index < len(text) and text[index] in "+-":
                put(text[index])
                index += 1
            while index < len(text) and text[index].isdigit():
                put(text[index])
                index += 1
    return float("".join(buffer))


def from_text(text):
    return from_number(parse_loader(text))


def fits_int64(value):
    return INT64_MIN <= value <= INT64_MAX


def in_range(value):
    return COUNT_MIN <= value <= COUNT_MAX


def add_counts(left, right):
    total = left + right
    if not fits_int64(total) or not in_range(total):
        return None
    return total


def negate(count):
    if count == INT64_MIN or count == COUNT_MIN:
        return None
    flipped = -count
    if not in_range(flipped):
        return None
    return flipped


def subtract(left, right):
    if right == INT64_MIN:
        return None
    return add_counts(left, -right)


def multiply(left, right):
    product = left * right
    if not fits_int64(product):
        return None
    shifted = product >> 16
    if not in_range(shifted):
        return None
    return shifted


def divide(left, right):
    if right == 0 or not in_range(left):
        return None
    if left == COUNT_MIN and right == -1:
        return None
    quotient = trunc_div(left * SCALE, right)
    if not in_range(quotient):
        return None
    return quotient


def whole_floor(count):
    return count >> 16


def whole_ceil(count):
    toward_floor = count >> 16
    if (count & 0xFFFF) == 0:
        return toward_floor
    return toward_floor + 1


def whole_round(count):
    whole = trunc_div(count, SCALE)
    frac = count - whole * SCALE
    if frac >= 32768:
        return whole + 1
    if frac <= -32768:
        return whole - 1
    return whole


def spell(count):
    if count == 0:
        return "0"
    sign = "-" if count < 0 else ""
    magnitude = abs(count)
    whole = magnitude // SCALE
    frac = magnitude % SCALE
    if frac == 0:
        return sign + str(whole)
    digits = f"{frac * 5**16:016d}".rstrip("0")
    return f"{sign}{whole}.{digits}"


def number_token(value):
    if math.isnan(value):
        return "nan"
    if math.isinf(value):
        return "-inf" if math.copysign(1.0, value) < 0 else "inf"
    return float(value).hex()


def main():
    pin_one = from_number(float("1.6"))
    pin_product = multiply(104857, 98304)
    pin_text = spell(157285)
    pin_wide = multiply(600000 * SCALE, 3 * SCALE)
    if (
        pin_one != 104857
        or pin_product != 157285
        or pin_text != "2.3999786376953125"
        or pin_wide != 117964800000
        or spell(104857) != "1.5999908447265625"
        or spell(COUNT_MAX) != "2147483647.9999847412109375"
        or spell(COUNT_MIN) != "-2147483648"
        or from_text("1.6") != 104857
    ):
        print(
            f"pins failed {pin_one} {pin_product} {pin_text} {pin_wide}",
            file=sys.stderr,
        )
        sys.exit(1)

    bases = [
        0,
        1,
        -1,
        2,
        -2,
        3,
        -3,
        32767,
        -32767,
        32768,
        -32768,
        32769,
        -32769,
        65535,
        -65535,
        65536,
        -65536,
        65537,
        -65537,
        98304,
        -98304,
        104857,
        -104857,
        (1 << 20) + 3,
        -((1 << 20) + 3),
        (1 << 31) - 1,
        -((1 << 31) - 1),
        1 << 31,
        -(1 << 31),
        1 << 32,
        -(1 << 32),
        (1 << 40) + 5,
        -((1 << 40) + 5),
        COUNT_MAX,
        COUNT_MIN,
        3 * SCALE,
        -3 * SCALE,
        100000 * SCALE,
    ]
    partners = [1, -1, 3, -3, 65536, -65536, 65537, -65537]
    for left in bases:
        for right in partners:
            add("mul", left, right, multiply(left, right))
            add("div", left, right, divide(left, right))

    add("mul", COUNT_MAX, COUNT_MAX, multiply(COUNT_MAX, COUNT_MAX))
    add("mul", COUNT_MIN, SCALE, multiply(COUNT_MIN, SCALE))
    add("mul", COUNT_MIN, SCALE + 1, multiply(COUNT_MIN, SCALE + 1))
    add("mul", COUNT_MIN, -SCALE, multiply(COUNT_MIN, -SCALE))
    add("mul", (1 << 31) - 1, 1 << 32, multiply((1 << 31) - 1, 1 << 32))
    add("mul", 1 << 31, 1 << 32, multiply(1 << 31, 1 << 32))
    add("mul", -1, 1, multiply(-1, 1))
    add("mul", 1, 1, multiply(1, 1))
    add("mul", 3, -1, multiply(3, -1))
    add("mul", 600000 * SCALE, 3 * SCALE, pin_wide)
    add("mul", 104857, 98304, multiply(104857, 98304))
    add("div", SCALE, 2 * SCALE, divide(SCALE, 2 * SCALE))
    add("div", -SCALE, 2 * SCALE, divide(-SCALE, 2 * SCALE))
    add("div", SCALE, -2 * SCALE, divide(SCALE, -2 * SCALE))
    add("div", -SCALE, -2 * SCALE, divide(-SCALE, -2 * SCALE))
    add("div", 1, 65537, divide(1, 65537))
    add("div", -1, 65537, divide(-1, 65537))
    add("div", 1, -65537, divide(1, -65537))
    add("div", -1, -65537, divide(-1, -65537))
    add("div", -3, 2, divide(-3, 2))
    add("div", COUNT_MAX, 1, divide(COUNT_MAX, 1))
    add("div", COUNT_MAX, SCALE, divide(COUNT_MAX, SCALE))
    add("div", COUNT_MAX, -SCALE, divide(COUNT_MAX, -SCALE))
    add("div", COUNT_MIN, SCALE, divide(COUNT_MIN, SCALE))
    add("div", COUNT_MIN, -SCALE, divide(COUNT_MIN, -SCALE))
    add("div", COUNT_MIN, -1, divide(COUNT_MIN, -1))
    add("div", WHOLE_MAX * SCALE, SCALE // 2, divide(WHOLE_MAX * SCALE, SCALE // 2))

    sums = [
        (0, 0),
        (1, -1),
        (65536, 32768),
        (104857, 98304),
        (-32768, -1),
        (COUNT_MAX, 0),
        (COUNT_MAX, 1),
        (COUNT_MAX, -1),
        (COUNT_MIN, 0),
        (COUNT_MIN, -1),
        (COUNT_MIN, 1),
        (COUNT_MAX, COUNT_MIN),
        (COUNT_MIN, COUNT_MAX),
        (1 << 46, 1 << 46),
        (-(1 << 46), -(1 << 46)),
        (WHOLE_MAX * SCALE, SCALE),
        (WHOLE_MIN * SCALE, -SCALE),
        (3 * SCALE, -SCALE),
        (-3 * SCALE, SCALE // 2),
    ]
    for left, right in sums:
        add("add", left, right, add_counts(left, right))
        add("sub", left, right, subtract(left, right))
    add("sub", 0, COUNT_MIN, subtract(0, COUNT_MIN))
    add("sub", 0, COUNT_MAX, subtract(0, COUNT_MAX))
    add("sub", COUNT_MIN, 1, subtract(COUNT_MIN, 1))
    add("sub", COUNT_MAX, -1, subtract(COUNT_MAX, -1))

    for count in (
        0,
        1,
        -1,
        2,
        -2,
        32767,
        -32767,
        32768,
        -32768,
        65535,
        65536,
        -65536,
        98304,
        -98304,
        COUNT_MAX,
        COUNT_MIN,
        WHOLE_MAX,
        WHOLE_MIN * SCALE,
        3 * SCALE,
        -3 * SCALE,
    ):
        add("neg", count, "", negate(count))

    quarters = []
    for whole in (0, 1, -1, 2, -2, 3, -3, WHOLE_MAX, WHOLE_MIN):
        for frac in (0, 1, 16384, 32767, 32768, 32769, 65535):
            count = whole * SCALE + (frac if whole >= 0 else -frac)
            if count == whole * SCALE and frac != 0 and whole < 0:
                count = whole * SCALE - frac
            if in_range(count):
                quarters.append(count)
    quarters.extend(
        [
            1,
            -1,
            32767,
            -32767,
            32768,
            -32768,
            32769,
            -32769,
            2 * SCALE + 32768,
            -(2 * SCALE + 32768),
            COUNT_MAX,
            COUNT_MIN,
            104857,
            157285,
        ]
    )
    for count in quarters:
        if not in_range(count):
            continue
        add("floor", count, "", whole_floor(count))
        add("ceil", count, "", whole_ceil(count))
        add("round", count, "", whole_round(count))

    texts = [
        "0",
        "1",
        "2",
        "-2",
        "+2",
        "1.5",
        "1.6",
        "0.5",
        "0.25",
        "0.75",
        "2.5",
        "-2.5",
        "-0.5",
        "  1.6",
        "   12",
        "+1.5",
        "-0",
        "1e2",
        "1E2",
        "1d2",
        "1D2",
        "1e-1",
        "2.5e+1",
        "1.25e0",
        "10e-1",
        "1.5xyz",
        "42abc",
        "abc",
        ".",
        "+",
        "-",
        "e10",
        "E3",
        "0.0000152587890625",
        "0.0000152587890624",
        "0.0000152587890626",
        "32767",
        "-32767",
        "32768",
        "-32768",
        "2147483647",
        "-2147483648",
        "2147483648",
        "-2147483648.0001",
        "1e20",
        "1e999",
        "-1e999",
        "3.14159",
        "0008",
        "1.",
        ".5",
        "+.5",
        "  -4.5e1tail",
        "1.5999908447265625",
        "2.3999786376953125",
    ]
    for text in texts:
        add("parse", text, "", from_text(text))

    numbers = [
        float("1.6"),
        float("0.5"),
        float("-0.5"),
        float("2.5"),
        float("-2.5"),
        float("1e20"),
        float("inf"),
        float("-inf"),
        float("nan"),
        math.nextafter(1.0 / 65536.0, 0.0),
        math.nextafter(1.0 / 65536.0, 1.0),
        1.0 / 65536.0,
        2147483647.0,
        -2147483648.0,
        2147483648.0,
        0.1,
        1.0,
        -0.0,
        32768.0,
        -32768.0,
        65536.0,
        3.25,
        -3.25,
    ]
    for value in numbers:
        add("number", number_token(value), "", from_number(value))

    spelled = [
        0,
        1,
        -1,
        3,
        -3,
        32767,
        32768,
        -32768,
        65535,
        65536,
        -65536,
        98304,
        104857,
        157285,
        196608,
        49152,
        163840,
        -163840,
        209714,
        1310720,
        117964800000,
        100000,
        COUNT_MAX,
        COUNT_MIN,
        WHOLE_MAX * SCALE,
        WHOLE_MIN * SCALE,
        2 * SCALE + 32768,
        -(2 * SCALE + 32768),
        SCALE + 1,
        -(SCALE + 1),
        65535 * SCALE + 1,
        5 * SCALE + 16384,
        -(5 * SCALE + 49152),
    ]
    for count in spelled:
        add("spell", count, "", spell(count))

    if len(rows) < 300:
        print(f"only {len(rows)} rows", file=sys.stderr)
        sys.exit(1)
    print(f"{len(rows)} rows", file=sys.stderr)
    print("// H07-FIXED-GOLDEN-BEGIN")
    for op, left, right, result in rows:
        print(f'    {{"{op}", "{left}", "{right}", "{result}"}},')
    print("// H07-FIXED-GOLDEN-END")


if __name__ == "__main__":
    main()
