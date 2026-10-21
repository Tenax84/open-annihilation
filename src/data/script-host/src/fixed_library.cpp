// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Fixed-point values for scripts. A value counts 65536ths in a signed 64-bit
// integer. Addition, subtraction, multiplication and division are integer
// arithmetic, so every build agrees, including 32-bit Windows.
//
// Three conversions touch a floating-point value. A Lua number or a parsed
// field is multiplied by 65536 and truncated toward zero. Text is read with
// parse_double, the same function the unit loader uses. Text and tonumber
// divide the count by 65536, which is exact for every count in range.

#include "internal.hpp"

#include "oa/data/script_host/fixed.hpp"

#include "oa/base/game_math.hpp"
#include "oa/formats/tdf.hpp"
#include "oa/formats/tdf/number.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <optional>
#include <string>

namespace oa::data::script_host {
namespace {

char fixed_meta_key = 0;

static_assert(sizeof(lua_Integer) >= sizeof(int64_t));

constexpr int64_t whole_min = int64_t{-2147483647} - 1;
constexpr int64_t whole_max = 2147483647LL;
constexpr char range_text[] = "(-2147483648 to 2147483647.99998)";

/// True when the exponent is not the all-ones pattern of a NaN or an infinity.
bool double_is_finite(double value) noexcept {
    static_assert(sizeof(uint64_t) == sizeof(double));
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return ((bits >> 52) & 0x7ffu) != 0x7ffu;
}

/// The count as a double. Exact for every count in the fixed range.
double count_as_double(int64_t count) noexcept {
    return static_cast<double>(count) / 65536.0;
}

void copy_capped(const char* text, char* out, std::size_t size) {
    if (size == 0)
        return;
    if (text == nullptr) {
        out[0] = '\0';
        return;
    }
    const std::size_t length = std::strlen(text);
    const std::size_t keep = length + 1 <= size ? length : size - 1;
    if (keep > 0)
        std::memcpy(out, text, keep);
    out[keep] = '\0';
}

/// Lua's own spelling of a number: an integer's digits, or tostring of a float.
void copy_number_text(lua_State* state, int index, char* out, std::size_t size) {
    if (size == 0)
        return;
    if (lua_isinteger(state, index) != 0) {
        const lua_Integer whole = lua_tointeger(state, index);
        std::snprintf(out, size, "%lld", static_cast<long long>(whole));
        return;
    }
    const char* text = luaL_tolstring(state, index, nullptr);
    copy_capped(text, out, size);
    lua_pop(state, 1);
}

int raise_outside(lua_State* state, const char* shown) {
    return luaL_error(
        state, "fixed: %s is outside the range of fixed numbers %s", shown, range_text
    );
}

int raise_operation(lua_State* state, const char* symbol, int64_t left_count, int64_t right_count) {
    char left_text[fixed_text_bytes];
    char right_text[fixed_text_bytes];
    if (!fixed_text(left_count, left_text, sizeof left_text))
        copy_capped("?", left_text, sizeof left_text);
    if (!fixed_text(right_count, right_text, sizeof right_text))
        copy_capped("?", right_text, sizeof right_text);
    return luaL_error(
        state,
        "fixed: %s %s %s is outside the range of fixed numbers %s",
        left_text,
        symbol,
        right_text,
        range_text
    );
}

/// Stores a Lua number as fixed(v) does. Raises when it does not fit.
///
/// @return false after a raising call, so the caller returns as well
[[nodiscard]] bool read_lua_number(lua_State* state, int index, int64_t* count) {
    if (lua_isinteger(state, index) != 0) {
        const lua_Integer whole = lua_tointeger(state, index);
        if (whole < whole_min || whole > whole_max) {
            char spelling[32];
            std::snprintf(spelling, sizeof spelling, "%lld", static_cast<long long>(whole));
            raise_outside(state, spelling);
            return false;
        }
        *count = static_cast<int64_t>(whole) * fixed_scale;
        return true;
    }
    const double value = lua_tonumber(state, index);
    if (!double_is_finite(value)) {
        char spelling[64];
        copy_number_text(state, index, spelling, sizeof spelling);
        luaL_error(state, "fixed: %s is not a finite number", spelling);
        return false;
    }
    int64_t converted = 0;
    bool fitted = false;
    {
        const std::optional<int64_t> got = fixed_from_number(value);
        fitted = got.has_value();
        if (fitted)
            converted = *got;
    }
    if (!fitted) {
        char spelling[64];
        copy_number_text(state, index, spelling, sizeof spelling);
        raise_outside(state, spelling);
        return false;
    }
    *count = converted;
    return true;
}

/// Stores field text as fixed.parse does. Raises when it does not fit.
///
/// @return false after a raising call, so the caller returns as well
[[nodiscard]] bool read_text_value(lua_State* state, int index, int64_t* count) {
    std::size_t length = 0;
    const char* text = lua_tolstring(state, index, &length);
    // Charge the scan before it runs, including a call that then raises.
    // One unit per 32 bytes, the same as tonumber of a text.
    charge(state, units_for_bytes(length));
    const char* body = text != nullptr ? text : "";
    char shown[160];
    const std::size_t keep = length < sizeof shown ? length : sizeof shown - 1;
    if (keep > 0)
        std::memcpy(shown, body, keep);
    shown[keep] = '\0';
    int64_t converted = 0;
    bool fitted = false;
    bool finite = true;
    {
        const double parsed = oa::formats::tdf::parse_double(body);
        finite = double_is_finite(parsed);
        const std::optional<int64_t> got = fixed_from_number(parsed);
        fitted = got.has_value();
        if (fitted)
            converted = *got;
    }
    if (!fitted) {
        if (!finite)
            luaL_error(state, "fixed: %s is not a finite number", shown);
        else
            raise_outside(state, shown);
        return false;
    }
    *count = converted;
    return true;
}

/// Converts one `fixed(v)` argument. Raises when it does not fit.
///
/// @return false after a raising call, so the caller returns as well
[[nodiscard]] bool convert_argument(lua_State* state, int index, int64_t* count) {
    if (to_fixed(state, index, count))
        return true;
    const int type = lua_type(state, index);
    if (type == LUA_TNUMBER)
        return read_lua_number(state, index, count);
    if (type == LUA_TSTRING)
        return read_text_value(state, index, count);
    const char* type_name = luaL_typename(state, index);
    luaL_error(
        state,
        "fixed: expected a fixed value, a number or a text, got %s",
        type_name != nullptr ? type_name : "value"
    );
    return false;
}

/// Reads one operator operand. A string is refused and is not parsed.
///
/// @return false after a raising call, so the caller returns as well
[[nodiscard]] bool read_operand(lua_State* state, int index, const char* verb, int64_t* count) {
    if (to_fixed(state, index, count))
        return true;
    if (lua_type(state, index) == LUA_TSTRING) {
        luaL_error(state, "fixed: cannot %s a string; use fixed(text)", verb);
        return false;
    }
    if (lua_type(state, index) != LUA_TNUMBER) {
        const char* type_name = luaL_typename(state, index);
        luaL_error(
            state,
            "fixed: expected a fixed value, a number or a text, got %s",
            type_name != nullptr ? type_name : "value"
        );
        return false;
    }
    return read_lua_number(state, index, count);
}

int push_converted(lua_State* state, int index) {
    int64_t count = 0;
    if (to_fixed(state, index, &count)) {
        lua_pushvalue(state, index);
        return 1;
    }
    if (!convert_argument(state, index, &count))
        return 0;
    push_fixed(state, count);
    return 1;
}

enum class Arithmetic : uint8_t { add, subtract, multiply, divide };

int meta_arithmetic(lua_State* state, Arithmetic which) {
    const char* verb = "add";
    const char* symbol = "+";
    if (which == Arithmetic::subtract) {
        verb = "subtract";
        symbol = "-";
    } else if (which == Arithmetic::multiply) {
        verb = "multiply";
        symbol = "*";
    } else if (which == Arithmetic::divide) {
        verb = "divide";
        symbol = "/";
    }
    int64_t left_count = 0;
    int64_t right_count = 0;
    if (!read_operand(state, 1, verb, &left_count))
        return 0;
    if (!read_operand(state, 2, verb, &right_count))
        return 0;
    if (which == Arithmetic::divide && right_count == 0)
        return luaL_error(state, "fixed: division by zero");
    int64_t result = 0;
    bool fitted = false;
    {
        std::optional<int64_t> got;
        if (which == Arithmetic::add)
            got = fixed_add(left_count, right_count);
        else if (which == Arithmetic::subtract)
            got = fixed_subtract(left_count, right_count);
        else if (which == Arithmetic::multiply)
            got = fixed_multiply(left_count, right_count);
        else
            got = fixed_divide(left_count, right_count);
        fitted = got.has_value();
        if (fitted)
            result = *got;
    }
    if (!fitted)
        return raise_operation(state, symbol, left_count, right_count);
    push_fixed(state, result);
    return 1;
}

int meta_order(lua_State* state, bool or_equal) {
    int64_t left_count = 0;
    int64_t right_count = 0;
    if (!read_operand(state, 1, "compare", &left_count))
        return 0;
    if (!read_operand(state, 2, "compare", &right_count))
        return 0;
    const bool holds = or_equal ? left_count <= right_count : left_count < right_count;
    lua_pushboolean(state, holds ? 1 : 0);
    return 1;
}

/// Pushes the text of one concatenation operand.
///
/// A fixed value becomes its own text. A string or a number is left to
/// `luaL_tolstring`. Anything else raises Lua's concatenation error.
///
/// @return false after a raising call, so the caller returns as well
[[nodiscard]] bool push_concat_piece(lua_State* state, int index) {
    int64_t count = 0;
    if (to_fixed(state, index, &count)) {
        char text[fixed_text_bytes];
        if (!fixed_text(count, text, sizeof text)) {
            luaL_error(state, "fixed: out of memory");
            return false;
        }
        lua_pushstring(state, text);
        return true;
    }
    const int type = lua_type(state, index);
    if (type == LUA_TSTRING || type == LUA_TNUMBER) {
        luaL_tolstring(state, index, nullptr);
        return true;
    }
    const char* type_name = luaL_typename(state, index);
    luaL_error(
        state, "attempt to concatenate a %s value", type_name != nullptr ? type_name : "value"
    );
    return false;
}

/// True when index 1 is a fixed value. Raises, then returns false, otherwise.
[[nodiscard]] bool require_fixed(lua_State* state, int64_t* count) {
    if (to_fixed(state, 1, count))
        return true;
    const char* type_name = luaL_typename(state, 1);
    luaL_error(
        state,
        "fixed: expected a fixed value, a number or a text, got %s",
        type_name != nullptr ? type_name : "value"
    );
    return false;
}

extern "C" {

static int meta_add(lua_State* state) {
    return meta_arithmetic(state, Arithmetic::add);
}

static int meta_sub(lua_State* state) {
    return meta_arithmetic(state, Arithmetic::subtract);
}

static int meta_mul(lua_State* state) {
    return meta_arithmetic(state, Arithmetic::multiply);
}

static int meta_div(lua_State* state) {
    return meta_arithmetic(state, Arithmetic::divide);
}

static int meta_unm(lua_State* state) {
    int64_t count = 0;
    if (!read_operand(state, 1, "subtract", &count))
        return 0;
    int64_t negated = 0;
    bool fitted = false;
    {
        const std::optional<int64_t> got = fixed_negate(count);
        fitted = got.has_value();
        if (fitted)
            negated = *got;
    }
    if (!fitted) {
        char text[fixed_text_bytes];
        if (!fixed_text(count, text, sizeof text))
            copy_capped("?", text, sizeof text);
        return luaL_error(
            state, "fixed: -%s is outside the range of fixed numbers %s", text, range_text
        );
    }
    push_fixed(state, negated);
    return 1;
}

static int meta_eq(lua_State* state) {
    int64_t left_count = 0;
    int64_t right_count = 0;
    if (!to_fixed(state, 1, &left_count) || !to_fixed(state, 2, &right_count)) {
        lua_pushboolean(state, 0);
        return 1;
    }
    lua_pushboolean(state, left_count == right_count ? 1 : 0);
    return 1;
}

static int meta_lt(lua_State* state) {
    return meta_order(state, false);
}

static int meta_le(lua_State* state) {
    return meta_order(state, true);
}

static int meta_concat(lua_State* state) {
    const int first = lua_absindex(state, 1);
    const int second = lua_absindex(state, 2);
    if (!push_concat_piece(state, first))
        return 0;
    if (!push_concat_piece(state, second))
        return 0;
    lua_concat(state, 2);
    return 1;
}

static int method_raw(lua_State* state) {
    int64_t count = 0;
    if (!require_fixed(state, &count))
        return 0;
    lua_pushinteger(state, static_cast<lua_Integer>(count));
    return 1;
}

static int method_text(lua_State* state) {
    int64_t count = 0;
    if (!require_fixed(state, &count))
        return 0;
    char text[fixed_text_bytes];
    if (!fixed_text(count, text, sizeof text))
        return luaL_error(state, "fixed: out of memory");
    lua_pushstring(state, text);
    return 1;
}

static int method_tonumber(lua_State* state) {
    int64_t count = 0;
    if (!require_fixed(state, &count))
        return 0;
    lua_pushnumber(state, count_as_double(count));
    return 1;
}

static int method_floor(lua_State* state) {
    int64_t count = 0;
    if (!require_fixed(state, &count))
        return 0;
    lua_pushinteger(state, static_cast<lua_Integer>(fixed_whole_floor(count)));
    return 1;
}

static int method_ceil(lua_State* state) {
    int64_t count = 0;
    if (!require_fixed(state, &count))
        return 0;
    lua_pushinteger(state, static_cast<lua_Integer>(fixed_whole_ceil(count)));
    return 1;
}

static int method_round(lua_State* state) {
    int64_t count = 0;
    if (!require_fixed(state, &count))
        return 0;
    lua_pushinteger(state, static_cast<lua_Integer>(fixed_whole_round(count)));
    return 1;
}

static int lib_call(lua_State* state) {
    if (lua_gettop(state) < 2)
        return luaL_error(state, "fixed: expected a fixed value, a number or a text, got no value");
    return push_converted(state, 2);
}

static int lib_parse(lua_State* state) {
    if (lua_type(state, 1) != LUA_TSTRING) {
        const char* type_name = luaL_typename(state, 1);
        return luaL_error(
            state, "fixed: expected a text, got %s", type_name != nullptr ? type_name : "value"
        );
    }
    int64_t count = 0;
    if (!read_text_value(state, 1, &count))
        return 0;
    push_fixed(state, count);
    return 1;
}

static int lib_raw(lua_State* state) {
    if (lua_type(state, 1) != LUA_TNUMBER) {
        const char* type_name = luaL_typename(state, 1);
        return luaL_error(
            state, "fixed: expected a number, got %s", type_name != nullptr ? type_name : "value"
        );
    }
    // lua_tointegerx accepts an integer, or a float that is an exact integer.
    // lua_numbertointeger only range-checks and truncates, so 1.5 would become 1.
    int is_exact = 0;
    const lua_Integer raw_count = lua_tointegerx(state, 1, &is_exact);
    if (is_exact != 0) {
        if (raw_count < fixed_count_min || raw_count > fixed_count_max) {
            char spelling[32];
            std::snprintf(spelling, sizeof spelling, "%lld", static_cast<long long>(raw_count));
            return raise_outside(state, spelling);
        }
        push_fixed(state, static_cast<int64_t>(raw_count));
        return 1;
    }
    const double value = lua_tonumber(state, 1);
    char spelling[64];
    copy_number_text(state, 1, spelling, sizeof spelling);
    if (!double_is_finite(value))
        return luaL_error(state, "fixed: %s is not a finite number", spelling);
    // Both ends of the count range are exact doubles. 1e300 is finite and far
    // outside them, so it names the range; 1.5 sits inside and is not a count.
    // The comparison classifies the number. It does not produce the count.
    if (value < static_cast<double>(fixed_count_min) ||
        value > static_cast<double>(fixed_count_max))
        return raise_outside(state, spelling);
    return luaL_error(state, "fixed: %s is not an integer count", spelling);
}

static int extreme(lua_State* state, bool want_min) {
    const int argc = lua_gettop(state);
    if (argc < 1)
        return luaL_error(state, "fixed: expected a fixed value, a number or a text, got no value");
    int64_t best = 0;
    if (!convert_argument(state, 1, &best))
        return 0;
    int best_slot = 1;
    for (int slot = 2; slot <= argc; ++slot) {
        // One comparison per value after the first, as math.min and math.max
        // charge, before the conversion so a long list cannot run for free.
        charge(state, comparison_charge(state, best_slot, slot));
        int64_t current = 0;
        if (!convert_argument(state, slot, &current))
            return 0;
        if (want_min) {
            if (current < best) {
                best = current;
                best_slot = slot;
            }
        } else if (current > best) {
            best = current;
            best_slot = slot;
        }
    }
    push_fixed(state, best);
    return 1;
}

static int lib_min(lua_State* state) {
    return extreme(state, true);
}

static int lib_max(lua_State* state) {
    return extreme(state, false);
}

} // extern "C"

} // namespace

std::optional<int64_t> fixed_from_number(double value) noexcept {
    if (!double_is_finite(value))
        return std::nullopt;
    const double scaled = value * static_cast<double>(fixed_scale);
    const int64_t count = oa::base::game_math::truncate_to_int64(scaled);
    if (count < fixed_count_min || count > fixed_count_max)
        return std::nullopt;
    return count;
}

std::optional<int64_t> fixed_from_text(const char* text) noexcept {
    if (text == nullptr)
        return std::nullopt;
    return fixed_from_number(oa::formats::tdf::parse_double(text));
}

std::optional<int64_t> fixed_add(int64_t a, int64_t b) noexcept {
    const int64_t limit_max = std::numeric_limits<int64_t>::max();
    const int64_t limit_min = std::numeric_limits<int64_t>::min();
    if ((b > 0 && a > limit_max - b) || (b < 0 && a < limit_min - b))
        return std::nullopt;
    const int64_t sum = a + b;
    if (sum < fixed_count_min || sum > fixed_count_max)
        return std::nullopt;
    return sum;
}

std::optional<int64_t> fixed_subtract(int64_t a, int64_t b) noexcept {
    if (b == std::numeric_limits<int64_t>::min())
        return std::nullopt;
    return fixed_add(a, -b);
}

std::optional<int64_t> fixed_negate(int64_t count) noexcept {
    if (count == std::numeric_limits<int64_t>::min() || count == fixed_count_min)
        return std::nullopt;
    const int64_t negated = -count;
    if (negated < fixed_count_min || negated > fixed_count_max)
        return std::nullopt;
    return negated;
}

std::optional<int64_t> fixed_multiply(int64_t a, int64_t b) noexcept {
    // Two counts multiply to at most 94 bits. The high and low halves come
    // from 32-bit limbs. A product that is not a signed 64-bit integer has
    // no fixed result. The arithmetic shift rounds a negative product toward
    // minus infinity, as scale_vector_fixed does.
    const uint64_t left_bits = static_cast<uint64_t>(a);
    const uint64_t right_bits = static_cast<uint64_t>(b);
    const uint64_t left_low = left_bits & 0xffffffffu;
    const uint64_t left_high = left_bits >> 32;
    const uint64_t right_low = right_bits & 0xffffffffu;
    const uint64_t right_high = right_bits >> 32;
    const uint64_t low_low = left_low * right_low;
    const uint64_t low_high = left_low * right_high;
    const uint64_t high_low = left_high * right_low;
    const uint64_t high_high = left_high * right_high;
    const uint64_t mid = (low_low >> 32) + (low_high & 0xffffffffu) + (high_low & 0xffffffffu);
    const uint64_t lo = (low_low & 0xffffffffu) | ((mid & 0xffffffffu) << 32);
    uint64_t hi = high_high + (low_high >> 32) + (high_low >> 32) + (mid >> 32);
    if (a < 0)
        hi -= right_bits;
    if (b < 0)
        hi -= left_bits;
    const uint64_t sign = (lo >> 63) == 0 ? uint64_t{0} : ~uint64_t{0};
    if (hi != sign)
        return std::nullopt;
    const int64_t shifted = static_cast<int64_t>(lo) >> 16;
    if (shifted < fixed_count_min || shifted > fixed_count_max)
        return std::nullopt;
    return shifted;
}

std::optional<int64_t> fixed_divide(int64_t a, int64_t b) noexcept {
    if (b == 0)
        return std::nullopt;
    if (a < fixed_count_min || a > fixed_count_max)
        return std::nullopt;
    // Every count in range, times 65536, fits in a signed 64-bit integer.
    // The minimum count times 65536 is the most negative of those integers.
    // Dividing that by -1 is not a defined signed division, and the quotient
    // would sit outside the fixed range.
    if (a == fixed_count_min && b == -1)
        return std::nullopt;
    const int64_t scaled = a * fixed_scale;
    const int64_t quot = scaled / b;
    if (quot < fixed_count_min || quot > fixed_count_max)
        return std::nullopt;
    return quot;
}

int64_t fixed_whole_floor(int64_t count) noexcept {
    return count >> 16;
}

int64_t fixed_whole_ceil(int64_t count) noexcept {
    const int64_t toward_floor = count >> 16;
    if ((count & 0xffff) == 0)
        return toward_floor;
    return toward_floor + 1;
}

int64_t fixed_whole_round(int64_t count) noexcept {
    const int64_t whole = count / fixed_scale;
    const int64_t frac = count % fixed_scale;
    if (frac >= 32768)
        return whole + 1;
    if (frac <= -32768)
        return whole - 1;
    return whole;
}

bool fixed_text(int64_t count, char* out, std::size_t size) noexcept {
    if (out == nullptr || size == 0)
        return false;
    try {
        const std::optional<std::string> text =
            oa::formats::tdf::format_number(count_as_double(count));
        if (!text.has_value() || text->size() + 1 > size)
            return false;
        std::memcpy(out, text->data(), text->size());
        out[text->size()] = '\0';
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}

bool to_fixed(lua_State* state, int index, int64_t* count) noexcept {
    if (state == nullptr || count == nullptr)
        return false;
    if (lua_type(state, index) != LUA_TUSERDATA)
        return false;
    if (lua_checkstack(state, 3) == 0)
        return false;
    if (lua_getmetatable(state, index) == 0)
        return false;
    lua_pushlightuserdata(state, &fixed_meta_key);
    lua_rawget(state, LUA_REGISTRYINDEX);
    const int match = lua_rawequal(state, -1, -2);
    lua_pop(state, 2);
    if (match == 0)
        return false;
    const void* memory = lua_touserdata(state, index);
    if (memory == nullptr || lua_rawlen(state, index) < sizeof(int64_t))
        return false;
    std::memcpy(count, memory, sizeof(int64_t));
    return true;
}

void push_fixed(lua_State* state, int64_t count) {
    if (lua_checkstack(state, 3) == 0) {
        luaL_error(state, "fixed: the fixed-point library is not open");
        return;
    }
    lua_pushlightuserdata(state, &fixed_meta_key);
    if (lua_rawget(state, LUA_REGISTRYINDEX) != LUA_TTABLE) {
        lua_pop(state, 1);
        luaL_error(state, "fixed: the fixed-point library is not open");
        return;
    }
    void* memory = lua_newuserdatauv(state, sizeof(int64_t), 0);
    std::memcpy(memory, &count, sizeof(count));
    lua_insert(state, -2);
    lua_setmetatable(state, -2);
}

void open_fixed_library(lua_State* state) {
    lua_newtable(state);
    lua_pushcfunction(state, method_raw);
    lua_setfield(state, -2, "raw");
    lua_pushcfunction(state, method_text);
    lua_setfield(state, -2, "text");
    lua_pushcfunction(state, method_tonumber);
    lua_setfield(state, -2, "tonumber");
    lua_pushcfunction(state, method_floor);
    lua_setfield(state, -2, "floor");
    lua_pushcfunction(state, method_ceil);
    lua_setfield(state, -2, "ceil");
    lua_pushcfunction(state, method_round);
    lua_setfield(state, -2, "round");

    lua_newtable(state);
    lua_pushvalue(state, -2);
    lua_setfield(state, -2, "__index");
    lua_pushcfunction(state, meta_add);
    lua_setfield(state, -2, "__add");
    lua_pushcfunction(state, meta_sub);
    lua_setfield(state, -2, "__sub");
    lua_pushcfunction(state, meta_mul);
    lua_setfield(state, -2, "__mul");
    lua_pushcfunction(state, meta_div);
    lua_setfield(state, -2, "__div");
    lua_pushcfunction(state, meta_unm);
    lua_setfield(state, -2, "__unm");
    lua_pushcfunction(state, meta_eq);
    lua_setfield(state, -2, "__eq");
    lua_pushcfunction(state, meta_lt);
    lua_setfield(state, -2, "__lt");
    lua_pushcfunction(state, meta_le);
    lua_setfield(state, -2, "__le");
    lua_pushcfunction(state, method_text);
    lua_setfield(state, -2, "__tostring");
    lua_pushcfunction(state, meta_concat);
    lua_setfield(state, -2, "__concat");
    lua_pushboolean(state, 0);
    lua_setfield(state, -2, "__metatable");

    lua_pushlightuserdata(state, &fixed_meta_key);
    lua_pushvalue(state, -2);
    lua_rawset(state, LUA_REGISTRYINDEX);
    lua_pop(state, 1);
    lua_pop(state, 1);

    lua_newtable(state);
    lua_pushcfunction(state, lib_parse);
    lua_setfield(state, -2, "parse");
    lua_pushcfunction(state, lib_raw);
    lua_setfield(state, -2, "raw");
    lua_pushcfunction(state, lib_min);
    lua_setfield(state, -2, "min");
    lua_pushcfunction(state, lib_max);
    lua_setfield(state, -2, "max");

    lua_newtable(state);
    lua_pushcfunction(state, lib_call);
    lua_setfield(state, -2, "__call");
    lua_pushboolean(state, 0);
    lua_setfield(state, -2, "__metatable");
    lua_setmetatable(state, -2);
    lua_setglobal(state, "fixed");
}

} // namespace oa::data::script_host
