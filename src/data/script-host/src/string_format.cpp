// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// string.format, string.pack, string.packsize and string.unpack, following
// lstrlib.c of Lua 5.4.9.
// Copyright (C) 1994-2026 Lua.org, PUC-Rio.
// The licence is the MIT licence; the text is licenses/lua-LICENSE.txt.
// The charges are the part this file adds. Each call counts one unit per
// byte of its format. string.format also counts argument_unit per value it
// converts and one unit per byte of each text it copies with %s or %q.
// string.pack counts one unit per byte it writes, padding included.
// string.unpack counts one unit per bytes_per_charge a z option scans. Past
// the allowance the call raises the same budget message as an endless loop.
// Hexadecimal floats are written by the interpreter's own code, as the
// interpreter itself does on every platform.
//
// The functions are C calls that Lua leaves by a long jump. No object with
// a destructor is alive across any of those calls.

#include "internal.hpp"

#include <bit>
#include <cctype>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace oa::data::script_host {
namespace {

constexpr char format_escape = '%';

/// Longest text one `%f` item can make: a precision of 99, the sign, the
/// point and the NUL, and the digits of the largest number, with a margin.
constexpr int max_item_fixed = 110 + DBL_MAX_10_EXP;

/// Longest text any other item can make.
constexpr int max_item = 120;

/// Longest conversion specification, such as `%-099.99d`, with room for a
/// length modifier and the NUL.
constexpr std::size_t max_format = 32;

constexpr const char* flags_float = "-+#0 ";
constexpr const char* flags_hex = "-#0";
constexpr const char* flags_integer = "-+0 ";
constexpr const char* flags_unsigned = "-0";
constexpr const char* flags_text = "-";

/// The flags, width and precision characters a specification may hold.
constexpr const char* specification_characters = "-+#0 123456789.";

/// Bits the first hexadecimal digit of a float holds, so that the digits
/// after it are whole nibbles.
constexpr int first_digit_bits = (DBL_MANT_DIG - 1) % 4 + 1;

/// The byte `value` holds, as the character functions take it.
unsigned char byte_of(char value) {
    return static_cast<unsigned char>(value);
}

/// Writes the integer part of `value` as one hexadecimal digit.
///
/// @param buffer the text being written
/// @param at where the digit goes
/// @param value a number from 0 to 16
/// @return the fraction left
lua_Number add_digit(char* buffer, int at, lua_Number value) {
    const lua_Number whole = std::floor(value);
    const int digit = static_cast<int>(whole);
    buffer[at] = static_cast<char>(digit < 10 ? digit + '0' : digit - 10 + 'a');
    return value - whole;
}

/// Writes `value` as a hexadecimal float, `0x1.8p+1` for 3.
///
/// @param buffer where the text goes
/// @param size bytes available at `buffer`
/// @param value the number
/// @return the length of the text
int hex_float_digits(char* buffer, int size, lua_Number value) {
    if (std::isnan(value) || std::isinf(value))
        return std::snprintf(buffer, static_cast<std::size_t>(size), LUA_NUMBER_FMT, value);
    if (value == 0)
        return std::snprintf(buffer, static_cast<std::size_t>(size), LUA_NUMBER_FMT "x0p+0", value);
    int exponent = 0;
    lua_Number fraction = std::frexp(value, &exponent);
    int count = 0;
    if (fraction < 0) {
        buffer[count++] = '-';
        fraction = -fraction;
    }
    buffer[count++] = '0';
    buffer[count++] = 'x';
    fraction = add_digit(buffer, count++, fraction * (1 << first_digit_bits));
    exponent -= first_digit_bits;
    if (fraction > 0) {
        buffer[count++] = '.';
        do {
            fraction = add_digit(buffer, count++, fraction * 16);
        } while (fraction > 0);
    }
    count +=
        std::snprintf(buffer + count, static_cast<std::size_t>(size - count), "p%+d", exponent);
    return count;
}

/// Writes `value` for `%a` or `%A`, refusing any flag, width or precision.
///
/// @param state the sandbox state
/// @param buffer where the text goes
/// @param size bytes available at `buffer`
/// @param form the specification, `%a` or `%A` when it has no modifiers
/// @param value the number
/// @return the length of the text
int hex_float(lua_State* state, char* buffer, int size, const char* form, lua_Number value) {
    const int count = hex_float_digits(buffer, size, value);
    if (form[1] == 'A') {
        for (int index = 0; index < count; ++index)
            buffer[index] = static_cast<char>(std::toupper(byte_of(buffer[index])));
    } else if (form[1] != 'a') {
        return luaL_error(state, "modifiers for format '%%a'/'%%A' not implemented");
    }
    return count;
}

/// Appends `text` as a Lua string literal in double quotes.
///
/// @param buffer the result being built
/// @param text the bytes, followed by a NUL as every Lua string is
/// @param length how many bytes `text` holds
void add_quoted(luaL_Buffer* buffer, const char* text, std::size_t length) {
    luaL_addchar(buffer, '"');
    while (length-- != 0) {
        if (*text == '"' || *text == '\\' || *text == '\n') {
            luaL_addchar(buffer, '\\');
            luaL_addchar(buffer, *text);
        } else if (std::iscntrl(byte_of(*text))) {
            char escaped[10];
            if (!std::isdigit(byte_of(*(text + 1))))
                std::snprintf(escaped, sizeof escaped, "\\%d", static_cast<int>(byte_of(*text)));
            else
                std::snprintf(escaped, sizeof escaped, "\\%03d", static_cast<int>(byte_of(*text)));
            luaL_addstring(buffer, escaped);
        } else {
            luaL_addchar(buffer, *text);
        }
        ++text;
    }
    luaL_addchar(buffer, '"');
}

/// Writes a float so that Lua reads back the same number: hexadecimal, or
/// `1e9999`, `-1e9999` or `(0/0)` for the values no numeral can hold.
///
/// @param state the sandbox state
/// @param buffer where the text goes, max_item bytes
/// @param value the number
/// @return the length of the text
int quote_float(lua_State* state, char* buffer, lua_Number value) {
    const char* fixed = nullptr;
    if (value == static_cast<lua_Number>(HUGE_VAL))
        fixed = "1e9999";
    else if (value == -static_cast<lua_Number>(HUGE_VAL))
        fixed = "-1e9999";
    else if (std::isnan(value))
        fixed = "(0/0)";
    else
        return hex_float(state, buffer, max_item, "%" LUA_NUMBER_FRMLEN "a", value);
    return std::snprintf(buffer, static_cast<std::size_t>(max_item), "%s", fixed);
}

/// Appends the value at `argument` for `%q`.
///
/// @param state the sandbox state
/// @param buffer the result being built
/// @param argument stack index of the value
void add_literal(lua_State* state, luaL_Buffer* buffer, int argument) {
    switch (lua_type(state, argument)) {
    case LUA_TSTRING: {
        std::size_t length = 0;
        const char* text = lua_tolstring(state, argument, &length);
        add_quoted(buffer, text, length);
        break;
    }
    case LUA_TNUMBER: {
        char* item = luaL_prepbuffsize(buffer, max_item);
        int count = 0;
        if (!lua_isinteger(state, argument)) {
            count = quote_float(state, item, lua_tonumber(state, argument));
        } else {
            const lua_Integer number = lua_tointeger(state, argument);
            const char* form =
                number == LUA_MININTEGER ? "0x%" LUA_INTEGER_FRMLEN "x" : LUA_INTEGER_FMT;
            count = std::snprintf(
                item, static_cast<std::size_t>(max_item), form, static_cast<LUAI_UACINT>(number)
            );
        }
        luaL_addsize(buffer, static_cast<std::size_t>(count));
        break;
    }
    case LUA_TNIL:
    case LUA_TBOOLEAN:
        luaL_tolstring(state, argument, nullptr);
        luaL_addvalue(buffer);
        break;
    default:
        luaL_argerror(state, argument, "value has no literal form");
    }
}

/// Skips at most two decimal digits.
///
/// @param text where the digits may start
/// @return the first byte after them
const char* skip_two_digits(const char* text) {
    if (std::isdigit(byte_of(*text))) {
        ++text;
        if (std::isdigit(byte_of(*text)))
            ++text;
    }
    return text;
}

/// Raises unless `form` is a specification with only the given flags, a
/// width and, when `precision` is true, a precision.
///
/// @param state the sandbox state
/// @param form the specification, from `%` to the conversion
/// @param flags the flags it may hold
/// @param precision whether it may hold a precision
void check_format(lua_State* state, const char* form, const char* flags, bool precision) {
    const char* specification = form + 1;
    specification += std::strspn(specification, flags);
    if (*specification != '0') {
        specification = skip_two_digits(specification);
        if (*specification == '.' && precision) {
            ++specification;
            specification = skip_two_digits(specification);
        }
    }
    if (!std::isalpha(byte_of(*specification)))
        luaL_error(state, "invalid conversion specification: '%s'", form);
}

/// Copies the specification starting at `format` into `form`, after a `%`.
///
/// @param state the sandbox state
/// @param format the first byte after the `%`
/// @param[out] form max_format bytes for the specification
/// @return the specification's last byte, its conversion
const char* read_format(lua_State* state, const char* format, char* form) {
    std::size_t length = std::strspn(format, specification_characters);
    ++length;
    if (length >= max_format - 10)
        luaL_error(state, "invalid format (too long)");
    *(form++) = '%';
    std::memcpy(form, format, length);
    *(form + length) = '\0';
    return format + length - 1;
}

/// Puts a length modifier in front of the conversion of `form`.
///
/// @param[in,out] form the specification, with room for the modifier
/// @param modifier the length modifier, such as `ll`
void add_length_modifier(char* form, const char* modifier) {
    const std::size_t length = std::strlen(form);
    const std::size_t modifier_length = std::strlen(modifier);
    const char conversion = form[length - 1];
    std::memcpy(form + length - 1, modifier, modifier_length);
    form[length + modifier_length - 1] = conversion;
    form[length + modifier_length] = '\0';
}

// string.pack and its family.

/// Byte the format's padding and alignment write.
constexpr char pack_padding = 0x00;

/// Most bytes one packed integer may take.
constexpr int max_integer_size = 16;

/// Bits in one byte.
constexpr int byte_bits = CHAR_BIT;

/// One byte of ones.
constexpr int byte_mask = (1 << byte_bits) - 1;

/// Bytes in a Lua integer.
constexpr int integer_size = static_cast<int>(sizeof(lua_Integer));

/// Largest size a format may name, which fits both an int and a size_t.
constexpr std::size_t max_pack_size = sizeof(std::size_t) < sizeof(int)
                                          ? static_cast<std::size_t>(-1)
                                          : static_cast<std::size_t>(INT_MAX);

constexpr bool native_little = std::endian::native == std::endian::little;

/// The strictest alignment the platform needs, which `!` without a number
/// asks for.
union MaxAlignValue {
    LUAI_MAXALIGN;
};

struct MaxAlignProbe {
    char lead{};
    MaxAlignValue value;
};

constexpr int native_max_align = static_cast<int>(offsetof(MaxAlignProbe, value));

/// What one format option reads or writes.
enum class PackOption : uint8_t {
    signed_integer,
    unsigned_integer,
    float_value,
    lua_number,
    double_value,
    fixed_text,
    counted_text,
    zero_text,
    padding,
    align_padding,
    none,
};

/// The state of one format as it is read.
struct PackHeader {
    lua_State* state{};
    bool little{};
    int max_align{};
};

/// Whether `value` is a decimal digit, whatever the locale.
bool is_digit(int value) {
    return '0' <= value && value <= '9';
}

/// Reads a decimal number from the format, or returns `fallback` when there
/// is none.
///
/// @param[in,out] format the format, moved past the digits
/// @param fallback the value when no digit follows
/// @return the number
int read_number(const char** format, int fallback) {
    if (!is_digit(**format))
        return fallback;
    int number = 0;
    do {
        number = number * 10 + (*((*format)++) - '0');
    } while (is_digit(**format) && number <= (static_cast<int>(max_pack_size) - 9) / 10);
    return number;
}

/// Reads an integer size and raises when it is outside 1 to max_integer_size.
///
/// @param header the format being read
/// @param[in,out] format the format, moved past the digits
/// @param fallback the size when no digit follows
/// @return the size
int read_size(PackHeader* header, const char** format, int fallback) {
    const int size = read_number(format, fallback);
    if (size > max_integer_size || size <= 0)
        return luaL_error(
            header->state, "integral size (%d) out of limits [1,%d]", size, max_integer_size
        );
    return size;
}

/// Starts reading a format: the platform's byte order, no alignment.
void start_header(lua_State* state, PackHeader* header) {
    header->state = state;
    header->little = native_little;
    header->max_align = 1;
}

/// Reads the next option and its size.
///
/// @param header the format being read
/// @param[in,out] format the format, moved past the option
/// @param[out] size the option's size in bytes
/// @return the option
PackOption read_option(PackHeader* header, const char** format, int* size) {
    const int option = *((*format)++);
    *size = 0;
    switch (option) {
    case 'b':
        *size = static_cast<int>(sizeof(char));
        return PackOption::signed_integer;
    case 'B':
        *size = static_cast<int>(sizeof(char));
        return PackOption::unsigned_integer;
    case 'h':
        *size = static_cast<int>(sizeof(short));
        return PackOption::signed_integer;
    case 'H':
        *size = static_cast<int>(sizeof(short));
        return PackOption::unsigned_integer;
    case 'l':
        *size = static_cast<int>(sizeof(long));
        return PackOption::signed_integer;
    case 'L':
        *size = static_cast<int>(sizeof(long));
        return PackOption::unsigned_integer;
    case 'j':
        *size = static_cast<int>(sizeof(lua_Integer));
        return PackOption::signed_integer;
    case 'J':
        *size = static_cast<int>(sizeof(lua_Integer));
        return PackOption::unsigned_integer;
    case 'T':
        *size = static_cast<int>(sizeof(std::size_t));
        return PackOption::unsigned_integer;
    case 'f':
        *size = static_cast<int>(sizeof(float));
        return PackOption::float_value;
    case 'n':
        *size = static_cast<int>(sizeof(lua_Number));
        return PackOption::lua_number;
    case 'd':
        *size = static_cast<int>(sizeof(double));
        return PackOption::double_value;
    case 'i':
        *size = read_size(header, format, static_cast<int>(sizeof(int)));
        return PackOption::signed_integer;
    case 'I':
        *size = read_size(header, format, static_cast<int>(sizeof(int)));
        return PackOption::unsigned_integer;
    case 's':
        *size = read_size(header, format, static_cast<int>(sizeof(std::size_t)));
        return PackOption::counted_text;
    case 'c':
        *size = read_number(format, -1);
        if (*size == -1)
            luaL_error(header->state, "missing size for format option 'c'");
        return PackOption::fixed_text;
    case 'z':
        return PackOption::zero_text;
    case 'x':
        *size = 1;
        return PackOption::padding;
    case 'X':
        return PackOption::align_padding;
    case ' ':
        break;
    case '<':
        header->little = true;
        break;
    case '>':
        header->little = false;
        break;
    case '=':
        header->little = native_little;
        break;
    case '!':
        header->max_align = read_size(header, format, native_max_align);
        break;
    default:
        luaL_error(header->state, "invalid format option '%c'", option);
    }
    return PackOption::none;
}

/// Reads the next option, its size and the padding that aligns it.
///
/// `X` takes its alignment from the option after it. Alignment is limited by
/// the format's maximum, and a fixed-length text needs none.
///
/// @param header the format being read
/// @param total_size bytes the format has covered so far
/// @param[in,out] format the format, moved past the option
/// @param[out] size the option's size in bytes
/// @param[out] align_bytes padding bytes before the option
/// @return the option
PackOption read_details(
    PackHeader* header, std::size_t total_size, const char** format, int* size, int* align_bytes
) {
    const PackOption option = read_option(header, format, size);
    int align = *size;
    if (option == PackOption::align_padding) {
        if (**format == '\0' || read_option(header, format, &align) == PackOption::fixed_text ||
            align == 0)
            luaL_argerror(header->state, 1, "invalid next option for option 'X'");
    }
    if (align <= 1 || option == PackOption::fixed_text) {
        *align_bytes = 0;
    } else {
        if (align > header->max_align)
            align = header->max_align;
        if ((align & (align - 1)) != 0)
            luaL_argerror(header->state, 1, "format asks for alignment not power of 2");
        *align_bytes =
            (align - static_cast<int>(total_size & static_cast<std::size_t>(align - 1))) &
            (align - 1);
    }
    return option;
}

/// Appends `size` bytes of `value`, sign-extending past a Lua integer when
/// `negative` is true.
void pack_integer(luaL_Buffer* buffer, lua_Unsigned value, bool little, int size, bool negative) {
    char* bytes = luaL_prepbuffsize(buffer, static_cast<std::size_t>(size));
    bytes[little ? 0 : size - 1] = static_cast<char>(value & byte_mask);
    for (int index = 1; index < size; ++index) {
        value >>= byte_bits;
        bytes[little ? index : size - 1 - index] = static_cast<char>(value & byte_mask);
    }
    if (negative && size > integer_size) {
        for (int index = integer_size; index < size; ++index)
            bytes[little ? index : size - 1 - index] = static_cast<char>(byte_mask);
    }
    luaL_addsize(buffer, static_cast<std::size_t>(size));
}

/// Copies `size` bytes, reversing them when `little` is not the platform's order.
void copy_with_order(char* destination, const char* source, int size, bool little) {
    if (little == native_little) {
        std::memcpy(destination, source, static_cast<std::size_t>(size));
        return;
    }
    destination += size - 1;
    while (size-- != 0)
        *(destination--) = *(source++);
}

/// Reads a `size`-byte integer, sign-extending it when `is_signed` is true.
/// Raises when the bytes past a Lua integer do not fit in one.
lua_Integer
unpack_integer(lua_State* state, const char* text, bool little, int size, bool is_signed) {
    lua_Unsigned result = 0;
    const int limit = size <= integer_size ? size : integer_size;
    for (int index = limit - 1; index >= 0; --index) {
        result <<= byte_bits;
        result |= static_cast<lua_Unsigned>(byte_of(text[little ? index : size - 1 - index]));
    }
    if (size < integer_size) {
        if (is_signed) {
            const lua_Unsigned mask = static_cast<lua_Unsigned>(1) << (size * byte_bits - 1);
            result = ((result ^ mask) - mask);
        }
    } else if (size > integer_size) {
        const int mask = (!is_signed || static_cast<lua_Integer>(result) >= 0) ? 0 : byte_mask;
        for (int index = limit; index < size; ++index) {
            if (byte_of(text[little ? index : size - 1 - index]) != mask)
                luaL_error(state, "%d-byte integer does not fit into Lua Integer", size);
        }
    }
    return static_cast<lua_Integer>(result);
}

/// Turns a position that may count from the end into one from the start.
std::size_t relative_position(lua_Integer position, std::size_t length) {
    if (position > 0)
        return static_cast<std::size_t>(position);
    if (position == 0)
        return 1;
    if (position < -static_cast<lua_Integer>(length))
        return 1;
    return length + static_cast<std::size_t>(position) + 1;
}

/// Charges one unit per byte of the format, and at least one.
const char* charged_format(lua_State* state) {
    std::size_t length = 0;
    const char* format = luaL_checklstring(state, 1, &length);
    charge(state, length == 0 ? 1 : static_cast<uint64_t>(length));
    return format;
}

extern "C" {
// string.format.
static int host_string_format(lua_State* state) {
    const int top = lua_gettop(state);
    int argument = 1;
    std::size_t format_length = 0;
    const char* format = luaL_checklstring(state, argument, &format_length);
    const char* format_end = format + format_length;
    charge(state, format_length == 0 ? 1 : static_cast<uint64_t>(format_length));
    luaL_Buffer buffer;
    luaL_buffinit(state, &buffer);
    while (format < format_end) {
        if (*format != format_escape) {
            luaL_addchar(&buffer, *format++);
            continue;
        }
        if (*++format == format_escape) {
            luaL_addchar(&buffer, *format++);
            continue;
        }
        char form[max_format];
        int item_size = max_item;
        char* item = luaL_prepbuffsize(&buffer, static_cast<std::size_t>(item_size));
        int count = 0;
        if (++argument > top)
            return luaL_argerror(state, argument, "no value");
        charge(state, argument_unit);
        format = read_format(state, format, form);
        const char conversion = *format++;
        switch (conversion) {
        case 'c': {
            check_format(state, form, flags_text, false);
            const auto value = static_cast<int>(luaL_checkinteger(state, argument));
            count = std::snprintf(item, static_cast<std::size_t>(item_size), form, value);
            break;
        }
        case 'd':
        case 'i':
        case 'u':
        case 'o':
        case 'x':
        case 'X': {
            const char* flags = flags_hex;
            if (conversion == 'd' || conversion == 'i')
                flags = flags_integer;
            else if (conversion == 'u')
                flags = flags_unsigned;
            const lua_Integer value = luaL_checkinteger(state, argument);
            check_format(state, form, flags, true);
            add_length_modifier(form, LUA_INTEGER_FRMLEN);
            count = std::snprintf(
                item, static_cast<std::size_t>(item_size), form, static_cast<LUAI_UACINT>(value)
            );
            break;
        }
        case 'a':
        case 'A': {
            check_format(state, form, flags_float, true);
            add_length_modifier(form, LUA_NUMBER_FRMLEN);
            const lua_Number value = luaL_checknumber(state, argument);
            count = hex_float(state, item, item_size, form, value);
            break;
        }
        case 'f':
        case 'e':
        case 'E':
        case 'g':
        case 'G': {
            if (conversion == 'f') {
                item_size = max_item_fixed;
                item = luaL_prepbuffsize(&buffer, static_cast<std::size_t>(item_size));
            }
            const lua_Number value = luaL_checknumber(state, argument);
            check_format(state, form, flags_float, true);
            add_length_modifier(form, LUA_NUMBER_FRMLEN);
            count = std::snprintf(
                item, static_cast<std::size_t>(item_size), form, static_cast<LUAI_UACNUMBER>(value)
            );
            break;
        }
        case 'p': {
            const void* pointer = lua_topointer(state, argument);
            check_format(state, form, flags_text, false);
            if (pointer == nullptr) {
                pointer = "(null)";
                form[std::strlen(form) - 1] = 's';
            }
            count = std::snprintf(item, static_cast<std::size_t>(item_size), form, pointer);
            break;
        }
        case 'q': {
            if (form[2] != '\0')
                return luaL_error(state, "specifier '%%q' cannot have modifiers");
            if (lua_type(state, argument) == LUA_TSTRING) {
                std::size_t length = 0;
                lua_tolstring(state, argument, &length);
                charge(state, static_cast<uint64_t>(length));
            }
            add_literal(state, &buffer, argument);
            break;
        }
        case 's': {
            std::size_t length = 0;
            const char* text = luaL_tolstring(state, argument, &length);
            charge(state, static_cast<uint64_t>(length));
            if (form[2] == '\0') {
                luaL_addvalue(&buffer);
            } else {
                luaL_argcheck(
                    state, length == std::strlen(text), argument, "string contains zeros"
                );
                check_format(state, form, flags_text, true);
                if (std::strchr(form, '.') == nullptr && length >= 100) {
                    luaL_addvalue(&buffer);
                } else {
                    count = std::snprintf(item, static_cast<std::size_t>(item_size), form, text);
                    lua_pop(state, 1);
                }
            }
            break;
        }
        default:
            return luaL_error(state, "invalid conversion '%s' to 'format'", form);
        }
        luaL_addsize(&buffer, static_cast<std::size_t>(count));
    }
    luaL_pushresult(&buffer);
    return 1;
}

// string.pack.
static int host_string_pack(lua_State* state) {
    const char* format = charged_format(state);
    int argument = 1;
    std::size_t total_size = 0;
    PackHeader header;
    start_header(state, &header);
    lua_pushnil(state);
    luaL_Buffer buffer;
    luaL_buffinit(state, &buffer);
    while (*format != '\0') {
        int size = 0;
        int align_bytes = 0;
        const PackOption option = read_details(&header, total_size, &format, &size, &align_bytes);
        // Every byte this option writes, its padding and a fixed text's fill included.
        charge(state, static_cast<uint64_t>(align_bytes) + static_cast<uint64_t>(size));
        total_size += static_cast<std::size_t>(align_bytes) + static_cast<std::size_t>(size);
        while (align_bytes-- > 0)
            luaL_addchar(&buffer, pack_padding);
        ++argument;
        switch (option) {
        case PackOption::signed_integer: {
            const lua_Integer value = luaL_checkinteger(state, argument);
            if (size < integer_size) {
                const lua_Integer limit = static_cast<lua_Integer>(1) << ((size * byte_bits) - 1);
                luaL_argcheck(
                    state, -limit <= value && value < limit, argument, "integer overflow"
                );
            }
            pack_integer(&buffer, static_cast<lua_Unsigned>(value), header.little, size, value < 0);
            break;
        }
        case PackOption::unsigned_integer: {
            const lua_Integer value = luaL_checkinteger(state, argument);
            if (size < integer_size)
                luaL_argcheck(
                    state,
                    static_cast<lua_Unsigned>(value) <
                        (static_cast<lua_Unsigned>(1) << (size * byte_bits)),
                    argument,
                    "unsigned overflow"
                );
            pack_integer(&buffer, static_cast<lua_Unsigned>(value), header.little, size, false);
            break;
        }
        case PackOption::float_value: {
            const auto value = static_cast<float>(luaL_checknumber(state, argument));
            char* bytes = luaL_prepbuffsize(&buffer, sizeof value);
            copy_with_order(
                bytes,
                reinterpret_cast<const char*>(&value),
                static_cast<int>(sizeof value),
                header.little
            );
            luaL_addsize(&buffer, static_cast<std::size_t>(size));
            break;
        }
        case PackOption::lua_number: {
            const lua_Number value = luaL_checknumber(state, argument);
            char* bytes = luaL_prepbuffsize(&buffer, sizeof value);
            copy_with_order(
                bytes,
                reinterpret_cast<const char*>(&value),
                static_cast<int>(sizeof value),
                header.little
            );
            luaL_addsize(&buffer, static_cast<std::size_t>(size));
            break;
        }
        case PackOption::double_value: {
            const auto value = static_cast<double>(luaL_checknumber(state, argument));
            char* bytes = luaL_prepbuffsize(&buffer, sizeof value);
            copy_with_order(
                bytes,
                reinterpret_cast<const char*>(&value),
                static_cast<int>(sizeof value),
                header.little
            );
            luaL_addsize(&buffer, static_cast<std::size_t>(size));
            break;
        }
        case PackOption::fixed_text: {
            std::size_t length = 0;
            const char* text = luaL_checklstring(state, argument, &length);
            luaL_argcheck(
                state,
                length <= static_cast<std::size_t>(size),
                argument,
                "string longer than given size"
            );
            luaL_addlstring(&buffer, text, length);
            while (length++ < static_cast<std::size_t>(size))
                luaL_addchar(&buffer, pack_padding);
            break;
        }
        case PackOption::counted_text: {
            std::size_t length = 0;
            const char* text = luaL_checklstring(state, argument, &length);
            luaL_argcheck(
                state,
                size >= static_cast<int>(sizeof(std::size_t)) ||
                    length < (static_cast<std::size_t>(1) << (size * byte_bits)),
                argument,
                "string length does not fit in given size"
            );
            charge(state, static_cast<uint64_t>(length));
            pack_integer(&buffer, static_cast<lua_Unsigned>(length), header.little, size, false);
            luaL_addlstring(&buffer, text, length);
            total_size += length;
            break;
        }
        case PackOption::zero_text: {
            std::size_t length = 0;
            const char* text = luaL_checklstring(state, argument, &length);
            luaL_argcheck(state, std::strlen(text) == length, argument, "string contains zeros");
            charge(state, static_cast<uint64_t>(length) + 1);
            luaL_addlstring(&buffer, text, length);
            luaL_addchar(&buffer, '\0');
            total_size += length + 1;
            break;
        }
        case PackOption::padding:
            luaL_addchar(&buffer, pack_padding);
            --argument;
            break;
        case PackOption::align_padding:
        case PackOption::none:
            --argument;
            break;
        }
    }
    luaL_pushresult(&buffer);
    return 1;
}

// string.packsize.
static int host_string_packsize(lua_State* state) {
    const char* format = charged_format(state);
    std::size_t total_size = 0;
    PackHeader header;
    start_header(state, &header);
    while (*format != '\0') {
        int size = 0;
        int align_bytes = 0;
        const PackOption option = read_details(&header, total_size, &format, &size, &align_bytes);
        luaL_argcheck(
            state,
            option != PackOption::counted_text && option != PackOption::zero_text,
            1,
            "variable-length format"
        );
        size += align_bytes;
        luaL_argcheck(
            state,
            total_size <= max_pack_size - static_cast<std::size_t>(size),
            1,
            "format result too large"
        );
        total_size += static_cast<std::size_t>(size);
    }
    lua_pushinteger(state, static_cast<lua_Integer>(total_size));
    return 1;
}

// string.unpack.
static int host_string_unpack(lua_State* state) {
    const char* format = charged_format(state);
    std::size_t data_length = 0;
    const char* data = luaL_checklstring(state, 2, &data_length);
    std::size_t position = relative_position(luaL_optinteger(state, 3, 1), data_length) - 1;
    int results = 0;
    luaL_argcheck(state, position <= data_length, 3, "initial position out of string");
    PackHeader header;
    start_header(state, &header);
    while (*format != '\0') {
        int size = 0;
        int align_bytes = 0;
        const PackOption option = read_details(&header, position, &format, &size, &align_bytes);
        luaL_argcheck(
            state,
            static_cast<std::size_t>(align_bytes) + static_cast<std::size_t>(size) <=
                data_length - position,
            2,
            "data string too short"
        );
        position += static_cast<std::size_t>(align_bytes);
        luaL_checkstack(state, 2, "too many results");
        ++results;
        switch (option) {
        case PackOption::signed_integer:
        case PackOption::unsigned_integer: {
            const lua_Integer value = unpack_integer(
                state, data + position, header.little, size, option == PackOption::signed_integer
            );
            lua_pushinteger(state, value);
            break;
        }
        case PackOption::float_value: {
            float value = 0;
            copy_with_order(
                reinterpret_cast<char*>(&value),
                data + position,
                static_cast<int>(sizeof value),
                header.little
            );
            lua_pushnumber(state, static_cast<lua_Number>(value));
            break;
        }
        case PackOption::lua_number: {
            lua_Number value = 0;
            copy_with_order(
                reinterpret_cast<char*>(&value),
                data + position,
                static_cast<int>(sizeof value),
                header.little
            );
            lua_pushnumber(state, value);
            break;
        }
        case PackOption::double_value: {
            double value = 0;
            copy_with_order(
                reinterpret_cast<char*>(&value),
                data + position,
                static_cast<int>(sizeof value),
                header.little
            );
            lua_pushnumber(state, static_cast<lua_Number>(value));
            break;
        }
        case PackOption::fixed_text:
            lua_pushlstring(state, data + position, static_cast<std::size_t>(size));
            break;
        case PackOption::counted_text: {
            const auto length = static_cast<std::size_t>(
                unpack_integer(state, data + position, header.little, size, false)
            );
            luaL_argcheck(
                state,
                length <= data_length - position - static_cast<std::size_t>(size),
                2,
                "data string too short"
            );
            lua_pushlstring(state, data + position + size, length);
            position += length;
            break;
        }
        case PackOption::zero_text: {
            const std::size_t length = std::strlen(data + position);
            charge(state, units_for_bytes(length + 1));
            luaL_argcheck(
                state, position + length < data_length, 2, "unfinished string for format 'z'"
            );
            lua_pushlstring(state, data + position, length);
            position += length + 1;
            break;
        }
        case PackOption::align_padding:
        case PackOption::padding:
        case PackOption::none:
            --results;
            break;
        }
        position += static_cast<std::size_t>(size);
    }
    lua_pushinteger(state, static_cast<lua_Integer>(position) + 1);
    return results + 1;
}
} // extern "C"

} // namespace

void install_format_and_pack(lua_State* state) {
    lua_pushcfunction(state, host_string_format);
    lua_setfield(state, -2, "format");
    lua_pushcfunction(state, host_string_pack);
    lua_setfield(state, -2, "pack");
    lua_pushcfunction(state, host_string_packsize);
    lua_setfield(state, -2, "packsize");
    lua_pushcfunction(state, host_string_unpack);
    lua_setfield(state, -2, "unpack");
}

} // namespace oa::data::script_host
