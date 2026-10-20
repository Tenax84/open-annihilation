# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Golden values for the script host's maths. Run with python3 -I. The stdout
# is the table in deterministic_math_test.cpp, from the BEGIN line through
# the END line. Inputs whose exact result lies within an eighth of a unit in
# the last place of a rounding boundary are left out.

import math
import struct
import sys
from decimal import Decimal, ROUND_FLOOR, ROUND_HALF_EVEN, getcontext, localcontext

# The division that builds pi/2 has to see this precision. The default
# context keeps 28 digits, and that error is the whole result of sin near pi.
PREC = 80
getcontext().prec = PREC
getcontext().rounding = ROUND_HALF_EVEN
PI = Decimal(
    "3.1415926535897932384626433832795028841971693993751058209749445923078164062862089986280348253421170679"
)
HALF_PI = PI / 2


def bits_of(value):
    return struct.unpack(">Q", struct.pack(">d", value))[0]


def hex_bits(value):
    return f"0x{bits_of(value):016x}ULL"


def spacing(rounded, below):
    """Width of the binade `rounded` lies in, or of the binade just below it.

    A value that rounds up to a power of two sits in the binade below that
    power, whose numbers are half as far apart.
    """
    bits = bits_of(abs(rounded))
    field = (bits >> 52) & 0x7FF
    if field == 0:
        return Decimal(2) ** -1074
    unit = Decimal(2) ** (int(field) - 1023 - 52)
    if below and (bits & ((1 << 52) - 1)) == 0:
        if field == 1:
            return Decimal(2) ** -1074
        return unit / 2
    return unit


def keep(exact):
    """Bits of the correctly rounded double, or None when the rounding is close."""
    if not exact.is_finite():
        return None
    try:
        rounded = float(exact)
    except (OverflowError, ValueError):
        return None
    if math.isnan(rounded) or math.isinf(rounded):
        return None
    if rounded == 0.0 and exact != 0:
        return None
    magnitude = abs(exact)
    if magnitude == 0:
        return bits_of(math.copysign(0.0, rounded))
    unit = spacing(rounded, magnitude < abs(Decimal(rounded)))
    quotient = magnitude / unit
    fraction = quotient - quotient.to_integral_value(rounding=ROUND_FLOOR)
    if abs(fraction - Decimal("0.5")) < Decimal("0.125"):
        return None
    return bits_of(rounded)


def unambiguous_underflow(exact):
    """Bits of zero when `exact` is unambiguously closer to zero than to any subnormal."""
    if not exact.is_finite() or exact == 0:
        return None
    margin = (Decimal(2) ** -1074) * Decimal("0.125")
    midpoint = Decimal(2) ** -1075
    if exact.copy_abs() + margin >= midpoint:
        return None
    return bits_of(math.copysign(0.0, -1.0 if exact < 0 else 1.0))


def sin_cos(value):
    reduced = value / HALF_PI
    steps = reduced.to_integral_value(rounding=ROUND_HALF_EVEN)
    remainder = value - steps * HALF_PI
    square = remainder * remainder
    sine_term = remainder
    sine = remainder
    cosine_term = Decimal(1)
    cosine = Decimal(1)
    index = 1
    while True:
        cosine_term *= -square / ((2 * index - 1) * (2 * index))
        cosine += cosine_term
        sine_term *= -square / ((2 * index) * (2 * index + 1))
        sine += sine_term
        index += 1
        if abs(sine_term) <= abs(sine) * Decimal("1e-30") and abs(cosine_term) <= abs(cosine) * Decimal(
            "1e-30"
        ):
            break
    quadrant = int(steps) % 4
    if quadrant < 0:
        quadrant += 4
    if quadrant == 0:
        return sine, cosine
    if quadrant == 1:
        return cosine, -sine
    if quadrant == 2:
        return -sine, -cosine
    return -cosine, sine


def atan_positive(value):
    complement = value > 1
    if complement:
        value = 1 / value
    doublings = 0
    while value > Decimal("0.1"):
        value = value / (1 + (1 + value * value).sqrt())
        doublings += 1
    total = Decimal(0)
    term = value
    square = value * value
    index = 1
    # A fixed cutoff would turn every tiny argument into zero.
    while index < 800:
        total += term / index
        term *= -square
        index += 2
        if total == 0 or abs(term) <= abs(total) * Decimal("1e-30"):
            break
    total *= Decimal(2) ** doublings
    if complement:
        total = HALF_PI - total
    return total


def atan2_exact(y, x):
    y = Decimal(y)
    x = Decimal(x)
    if x == 0 and y == 0:
        return Decimal(0)
    if x > 0:
        angle = atan_positive(abs(y / x))
    elif x < 0:
        angle = PI - atan_positive(abs(y / x))
    else:
        angle = HALF_PI
    if y < 0:
        angle = -angle
    return angle


def asin_exact(value):
    value = Decimal(value)
    if value == 0:
        return Decimal(0)
    if abs(value) == 1:
        return HALF_PI.copy_sign(value)
    root = (1 - value * value).sqrt()
    return atan2_exact(value, root)


def emit(name, rows):
    if len(rows) < 40:
        print(f"{name} kept {len(rows)}", file=sys.stderr)
        sys.exit(1)
    for left, right, expected in rows[:48]:
        print(f'    {{"{name}", {hex_bits(left)}, {hex_bits(right)}, {hex_bits(expected)}}},')


def unique(values):
    seen = {}
    for value in values:
        if not math.isfinite(value):
            continue
        seen.setdefault(bits_of(value), value)
    return list(seen.values())


def weyl(low, high, count):
    span = high - low
    for index in range(count):
        fraction = (index * 0.6180339887498949) % 1.0
        yield low + span * fraction


def main():
    with localcontext() as ctx:
        ctx.prec = PREC
        ctx.rounding = ROUND_HALF_EVEN
        angles = unique(
            [
                0.0,
                1.0,
                -1.0,
                0.5,
                -0.5,
                0.25,
                -0.25,
                2.0,
                -2.0,
                3.0,
                -3.0,
                math.pi,
                -math.pi,
                math.pi / 2,
                -math.pi / 2,
                math.pi / 4,
                math.pi / 6,
                1e-8,
                -1e-8,
                1e-20,
                -1e-20,
                float.fromhex("0x1p-1022"),
                float.fromhex("0x1p-1074"),
                float.fromhex("-0x1p-1074"),
                10.0,
                -10.0,
                100.0,
                -30.25,
                1000.0,
                1e6,
                -1e6,
                1e8,
                -1e7,
                2147483647.0,
                -2147483647.0,
                *weyl(-1000.0, 1000.0, 40),
                *weyl(-1e-4, 1e-4, 20),
                *weyl(-1e8, 1e8, 30),
            ]
        )
        sine_rows = []
        cosine_rows = []
        tangent_rows = []
        for angle in angles:
            sine, cosine = sin_cos(Decimal(angle))
            sine_bits = keep(sine)
            cosine_bits = keep(cosine)
            if sine_bits is not None:
                sine_rows.append((angle, 0.0, struct.unpack(">d", struct.pack(">Q", sine_bits))[0]))
            if cosine_bits is not None:
                cosine_rows.append((angle, 0.0, struct.unpack(">d", struct.pack(">Q", cosine_bits))[0]))
            if cosine != 0:
                tangent_bits = keep(sine / cosine)
                if tangent_bits is not None:
                    tangent_rows.append(
                        (angle, 0.0, struct.unpack(">d", struct.pack(">Q", tangent_bits))[0])
                    )

        domain = unique(
            [
                0.0,
                0.5,
                -0.5,
                1.0,
                -1.0,
                0.1,
                -0.1,
                0.25,
                -0.25,
                0.9,
                -0.9,
                0.999,
                -0.999,
                1e-8,
                -1e-8,
                1e-20,
                float.fromhex("0x1p-1022"),
                *weyl(-1.0, 1.0, 80),
            ]
        )
        arcsine_rows = []
        arccosine_rows = []
        for value in domain:
            if abs(value) > 1.0:
                continue
            arc = asin_exact(value)
            arc_bits = keep(arc)
            if arc_bits is not None:
                arcsine_rows.append((value, 0.0, struct.unpack(">d", struct.pack(">Q", arc_bits))[0]))
            cosine_bits = keep(HALF_PI - arc)
            if cosine_bits is not None:
                arccosine_rows.append(
                    (value, 0.0, struct.unpack(">d", struct.pack(">Q", cosine_bits))[0])
                )

        points = []
        for y in (0.0, 0.5, -0.5, 1.0, -1.0, 2.0, -3.0, 1e-6, 1e6, -1e8, 0.1):
            for x in (0.5, -0.5, 1.0, -1.0, 2.0, -4.0, 1e-8, 1e3, -1e6, 0.25):
                points.append((y, x))
        for index in range(30):
            points.append((((index * 0.37) % 1.0) * 4 - 2, ((index * 0.19) % 1.0) * 6 - 3))
        arctangent_rows = []
        seen_points = set()
        for y, x in points:
            key = (bits_of(y), bits_of(x))
            if key in seen_points or not math.isfinite(y) or not math.isfinite(x):
                continue
            seen_points.add(key)
            bits = keep(atan2_exact(y, x))
            if bits is not None:
                arctangent_rows.append((y, x, struct.unpack(">d", struct.pack(">Q", bits))[0]))

        exponents = unique(
            [
                0.0,
                1.0,
                -1.0,
                0.5,
                -0.5,
                2.0,
                -2.0,
                10.0,
                -10.0,
                20.0,
                -20.0,
                0.1,
                -0.1,
                1e-8,
                -1e-8,
                1e-20,
                -1e-20,
                math.log(2),
                -math.log(2),
                100.0,
                -100.0,
                400.0,
                -400.0,
                700.0,
                -700.0,
                709.0,
                -740.0,
                -744.0,
                float.fromhex("0x1p-1022"),
                float.fromhex("-0x1p-50"),
                *weyl(-20.0, 20.0, 30),
                *weyl(-700.0, 700.0, 40),
            ]
        )
        exponential_rows = []
        for value in exponents:
            bits = keep(Decimal(value).exp())
            if bits is not None:
                exponential_rows.append((value, 0.0, struct.unpack(">d", struct.pack(">Q", bits))[0]))

        positives = unique(
            [
                1.0,
                2.0,
                0.5,
                10.0,
                math.e,
                0.1,
                100.0,
                1e-6,
                1e6,
                1e-20,
                1e20,
                1.0000001,
                0.999,
                3.5,
                math.pi,
                float.fromhex("0x1p-1022"),
                float.fromhex("0x1p-1074"),
                float.fromhex("0x1p+1023"),
                float.fromhex("0x1.8p-1022"),
                1e-300,
                1e300,
                *weyl(1e-8, 1e8, 40),
                *[10.0**k for k in range(0, 23)],
                *[2.0**k for k in (-10, -1, 0, 1, 10, 50, 100)],
            ]
        )
        natural_rows = []
        log2_rows = []
        log10_rows = []
        ln2 = Decimal(2).ln()
        ln10 = Decimal(10).ln()
        for value in positives:
            if value <= 0.0:
                continue
            logarithm = Decimal(value).ln()
            natural_bits = keep(logarithm)
            log2_bits = keep(logarithm / ln2)
            log10_bits = keep(logarithm / ln10)
            if natural_bits is not None:
                natural_rows.append((value, 0.0, struct.unpack(">d", struct.pack(">Q", natural_bits))[0]))
            if log2_bits is not None:
                log2_rows.append((value, 0.0, struct.unpack(">d", struct.pack(">Q", log2_bits))[0]))
            if log10_bits is not None:
                log10_rows.append((value, 0.0, struct.unpack(">d", struct.pack(">Q", log10_bits))[0]))

        bases = [0.5, 1.5, 2.0, 3.0, 10.0, 0.1, 1.1, 0.9, 100.0, 1e-3, 1e3, 1.0000001, 9.9, 1.7]
        powers = [0.5, 1.5, 2.5, 3.25, -0.5, -1.5, 10.5, -3.5, 0.1, -0.1, 20.2, -8.5, 7.0, -2.0]
        # Integer exponents past the squaring limit. The result has to be a
        # normal number, apart from 0.999^(2^31-1), which underflows and is
        # unambiguously zero.
        large_powers = [
            (1.0001, 100000.0),
            (1.0 + 2.0**-52, float(2**53 + 2)),
            (0.999, float(2**31 - 1)),
            (1.0000001, float(2**20)),
            (0.999999, -float(2**20)),
            (1.0 + 2.0**-30, float(2**30)),
            (1.0 + 2.0**-30, -float(2**30)),
            (1.0 + 2.0**-40, float(2**40)),
            (1.0 - 2.0**-40, -float(2**40)),
            (1.0 + 2.0**-48, float(2**52)),
            (1.0 - 2.0**-48, -float(2**52)),
            (-1.0001, 100001.0),
            (1.0001, -100000.0),
        ]
        power_rows = []
        seen_power = set()
        for base, exponent in large_powers:
            exact = Decimal(base) ** Decimal(exponent)
            bits = keep(exact)
            if bits is None:
                bits = unambiguous_underflow(exact)
            if bits is None:
                print(f"power dropped {base} ^ {exponent}", file=sys.stderr)
                continue
            seen_power.add((bits_of(base), bits_of(exponent)))
            power_rows.append((base, exponent, struct.unpack(">d", struct.pack(">Q", bits))[0]))
        for base in bases:
            for exponent in powers:
                key = (bits_of(base), bits_of(exponent))
                if key in seen_power:
                    continue
                seen_power.add(key)
                bits = keep(Decimal(base) ** Decimal(exponent))
                if bits is not None:
                    power_rows.append((base, exponent, struct.unpack(">d", struct.pack(">Q", bits))[0]))
        for base, exponent in (
            (1.1, 200.5),
            (1.0000001, 1000.0),
            (0.999, 500.0),
            (2.5, 40.5),
            (8.0, 10.0),
            (9.0, 0.5),
            (4.0, -0.5),
            (1.25, 8.0),
        ):
            bits = keep(Decimal(base) ** Decimal(exponent))
            if bits is not None:
                power_rows.append((base, exponent, struct.unpack(">d", struct.pack(">Q", bits))[0]))

        print("// H03-MATHS-GOLDEN-BEGIN")
        emit("sine", sine_rows)
        emit("cosine", cosine_rows)
        emit("tangent", tangent_rows)
        emit("arcsine", arcsine_rows)
        emit("arccosine", arccosine_rows)
        emit("arctangent", arctangent_rows)
        emit("exponential", exponential_rows)
        emit("natural_log", natural_rows)
        emit("log2", log2_rows)
        emit("log10", log10_rows)
        emit("power", power_rows)
        print("// H03-MATHS-GOLDEN-END")


if __name__ == "__main__":
    main()
