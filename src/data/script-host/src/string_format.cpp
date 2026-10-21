// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// string.format, following lstrlib.c of Lua 5.4.9.
// Copyright (C) 1994-2026 Lua.org, PUC-Rio.
// The licence is the MIT licence; the text is licenses/lua-LICENSE.txt.
// The charges and two refusals are the parts this file adds. Each call
// counts one unit per byte of its format, argument_unit per value it
// converts and one unit per byte of each text it copies with %s or %q.
// Past the allowance the call raises the same budget message as an endless
// loop. %p is refused, because it would show where a value sits, and %a,
// %e, %f and %g of a NaN or an infinity are refused, because C libraries
// spell those differently. Hexadecimal floats are written by the
// interpreter's own code, as the interpreter itself does on every platform.
// string.pack and its family are not copied: the sandbox removes them.
//
// The functions are C calls that Lua leaves by a long jump. No object with
// a destructor is alive across any of those calls.

#include "internal.hpp"

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

/// Raises when `value` is a NaN or an infinity, which C libraries write
/// differently for `%a`, `%e`, `%f` and `%g`.
///
/// @param state the sandbox state
/// @param conversion the conversion, named in the message
/// @param value the number
void refuse_special_number(lua_State* state, char conversion, lua_Number value) {
    if (std::isnan(value) || std::isinf(value)) {
        luaL_error(
            state,
            "string.format: cannot write nan or an infinity with %%%c; test the number first, "
            "or use tostring",
            conversion
        );
    }
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
            refuse_special_number(state, conversion, value);
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
            refuse_special_number(state, conversion, value);
            add_length_modifier(form, LUA_NUMBER_FRMLEN);
            count = std::snprintf(
                item, static_cast<std::size_t>(item_size), form, static_cast<LUAI_UACNUMBER>(value)
            );
            break;
        }
        case 'p':
            // Where a value sits differs between machines and between runs.
            return luaL_error(state, "string.format: %%p would show a memory address");
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
} // extern "C"

} // namespace

void install_format(lua_State* state) {
    lua_pushcfunction(state, host_string_format);
    lua_setfield(state, -2, "format");
}

} // namespace oa::data::script_host
