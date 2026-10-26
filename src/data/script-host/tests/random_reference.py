# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Golden draws for math.random. Run with python3 -I. The stdout is the table
# in deterministic_math_test.cpp. Each group is the first ten calls of that
# kind after setseed, and the generator is seeded again before the next group.

MASK = (1 << 64) - 1


def rotate_left(bits, places):
    bits &= MASK
    return ((bits << places) | (bits >> (64 - places))) & MASK


def next_word(word):
    state0, state1, state2, state3 = word
    mixed2 = (state2 ^ state0) & MASK
    mixed3 = (state3 ^ state1) & MASK
    drawn = (rotate_left((state1 * 5) & MASK, 7) * 9) & MASK
    return drawn, [
        (state0 ^ mixed3) & MASK,
        (state1 ^ mixed2) & MASK,
        (mixed2 ^ ((state1 << 17) & MASK)) & MASK,
        rotate_left(mixed3, 45),
    ]


def set_seed(first, second):
    word = [first & MASK, 0xFF, second & MASK, 0]
    for _step in range(16):
        _drawn, word = next_word(word)
    return word


def project(drawn, limit, word):
    drawn &= MASK
    limit &= MASK
    if (limit & (limit + 1)) == 0:
        return drawn & limit, word
    mask = limit
    mask |= mask >> 1
    mask |= mask >> 2
    mask |= mask >> 4
    mask |= mask >> 8
    mask |= mask >> 16
    mask |= mask >> 32
    mask &= MASK
    while True:
        drawn &= mask
        if drawn <= limit:
            return drawn, word
        drawn, word = next_word(word)


def as_integer(bits):
    bits &= MASK
    if bits >= 2**63:
        return bits - 2**64
    return bits


def ten_bounded(first, second):
    word = set_seed(first, second)
    values = []
    for _step in range(10):
        drawn, word = next_word(word)
        projected, word = project(drawn, 99, word)
        values.append(projected + 1)
    return values


def ten_unit(first, second):
    word = set_seed(first, second)
    values = []
    for _step in range(10):
        drawn, word = next_word(word)
        top = (drawn & MASK) >> 11
        values.append(top * (2**-53))
    return values


def ten_full(first, second):
    word = set_seed(first, second)
    values = []
    for _step in range(10):
        drawn, word = next_word(word)
        values.append(as_integer(drawn))
    return values


def hex_bits(value):
    return f"0x{struct_bits(value):016x}ULL"


def struct_bits(value):
    import struct

    return struct.unpack(">Q", struct.pack(">d", value))[0]


def main():
    print("// H03-RANDOM-GOLDEN-BEGIN")
    for first, second in ((0, 0), (1, 0), (42, 0), (12345, 678)):
        bounded = ", ".join(str(value) for value in ten_bounded(first, second))
        unit = ", ".join(hex_bits(value) for value in ten_unit(first, second))
        full = ", ".join(f"{value}LL" for value in ten_full(first, second))
        print(f"    {{{first}ULL, {second}ULL, {{{bounded}}}, {{{unit}}}, {{{full}}}}},")
    print("// H03-RANDOM-GOLDEN-END")


if __name__ == "__main__":
    main()
