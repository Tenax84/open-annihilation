// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Lua 5.4's math library. The generator is xoshiro256**, seeded from the
// sandbox instead of the clock. sin, cos, tan, asin, acos, atan, exp, log
// and the power operator use the engine's arithmetic.

#include "internal.hpp"

#include "oa/data/script_host/deterministic_math.hpp"

#include <bit>
#include <cmath>
#include <cstdint>

namespace oa::data::script_host {
namespace {

constexpr double pi = 3.141592653589793238462643383279502884;

struct RandomState {
    uint64_t word[4]{};
};

uint64_t rotate_left(uint64_t bits, int places) {
    return (bits << places) | (bits >> (64 - places));
}

uint64_t next_word(uint64_t word[4]) {
    const uint64_t state0 = word[0];
    const uint64_t state1 = word[1];
    const uint64_t state2 = word[2] ^ state0;
    const uint64_t state3 = word[3] ^ state1;
    const uint64_t drawn = rotate_left(state1 * 5, 7) * 9;
    word[0] = state0 ^ state3;
    word[1] = state1 ^ state2;
    word[2] = state2 ^ (state1 << 17);
    word[3] = rotate_left(state3, 45);
    return drawn;
}

double unit_interval(uint64_t bits) {
    const uint64_t top = bits >> 11;
    return static_cast<double>(top) * 0x1p-53;
}

lua_Unsigned project(lua_Unsigned drawn, lua_Unsigned limit, uint64_t word[4]) {
    if ((limit & (limit + 1)) == 0)
        return drawn & limit;
    lua_Unsigned mask = limit;
    mask |= mask >> 1;
    mask |= mask >> 2;
    mask |= mask >> 4;
    mask |= mask >> 8;
    mask |= mask >> 16;
    mask |= mask >> 32;
    while ((drawn &= mask) > limit)
        drawn = static_cast<lua_Unsigned>(next_word(word));
    return drawn;
}

void set_seed(lua_State* state, uint64_t word[4], uint64_t first, uint64_t second) {
    word[0] = first;
    word[1] = 0xff;
    word[2] = second;
    word[3] = 0;
    for (int step = 0; step < 16; ++step)
        next_word(word);
    lua_pushinteger(state, static_cast<lua_Integer>(first));
    lua_pushinteger(state, static_cast<lua_Integer>(second));
}

void push_number_or_integer(lua_State* state, double value) {
    lua_Integer whole = 0;
    if (lua_numbertointeger(value, &whole))
        lua_pushinteger(state, whole);
    else
        lua_pushnumber(state, value);
}

bool angle_rejected(lua_State* state, double radians, const char* name) {
    const uint64_t bits = std::bit_cast<uint64_t>(radians);
    const uint64_t magnitude = bits & 0x7fffffffffffffffULL;
    if (magnitude > 0x7ff0000000000000ULL) {
        lua_pushnumber(state, radians);
        return true;
    }
    if (magnitude >= 0x41e0000000000000ULL) {
        luaL_error(state, "%s: the angle must be below 2147483648 in magnitude", name);
        return true;
    }
    return false;
}

extern "C" {
static int math_abs(lua_State* state) {
    if (lua_isinteger(state, 1)) {
        lua_Integer number = lua_tointeger(state, 1);
        if (number < 0)
            number = static_cast<lua_Integer>(0u - static_cast<lua_Unsigned>(number));
        lua_pushinteger(state, number);
    } else {
        lua_pushnumber(state, std::fabs(luaL_checknumber(state, 1)));
    }
    return 1;
}

static int math_sin(lua_State* state) {
    const double radians = luaL_checknumber(state, 1);
    if (angle_rejected(state, radians, "sin"))
        return 1;
    lua_pushnumber(state, sine(radians));
    return 1;
}

static int math_cos(lua_State* state) {
    const double radians = luaL_checknumber(state, 1);
    if (angle_rejected(state, radians, "cos"))
        return 1;
    lua_pushnumber(state, cosine(radians));
    return 1;
}

static int math_tan(lua_State* state) {
    const double radians = luaL_checknumber(state, 1);
    if (angle_rejected(state, radians, "tan"))
        return 1;
    lua_pushnumber(state, tangent(radians));
    return 1;
}

static int math_asin(lua_State* state) {
    lua_pushnumber(state, arcsine(luaL_checknumber(state, 1)));
    return 1;
}

static int math_acos(lua_State* state) {
    lua_pushnumber(state, arccosine(luaL_checknumber(state, 1)));
    return 1;
}

static int math_atan(lua_State* state) {
    const double y = luaL_checknumber(state, 1);
    const double x = luaL_optnumber(state, 2, 1);
    lua_pushnumber(state, arctangent(y, x));
    return 1;
}

static int math_tointeger(lua_State* state) {
    int valid = 0;
    const lua_Integer number = lua_tointegerx(state, 1, &valid);
    if (valid)
        lua_pushinteger(state, number);
    else {
        luaL_checkany(state, 1);
        luaL_pushfail(state);
    }
    return 1;
}

static int math_floor(lua_State* state) {
    if (lua_isinteger(state, 1))
        lua_settop(state, 1);
    else
        push_number_or_integer(state, std::floor(luaL_checknumber(state, 1)));
    return 1;
}

static int math_ceil(lua_State* state) {
    if (lua_isinteger(state, 1))
        lua_settop(state, 1);
    else
        push_number_or_integer(state, std::ceil(luaL_checknumber(state, 1)));
    return 1;
}

static int math_fmod(lua_State* state) {
    if (lua_isinteger(state, 1) && lua_isinteger(state, 2)) {
        const lua_Integer divisor = lua_tointeger(state, 2);
        if (static_cast<lua_Unsigned>(divisor) + 1u <= 1u) {
            luaL_argcheck(state, divisor != 0, 2, "zero");
            lua_pushinteger(state, 0);
        } else {
            lua_pushinteger(state, lua_tointeger(state, 1) % divisor);
        }
    } else {
        lua_pushnumber(state, std::fmod(luaL_checknumber(state, 1), luaL_checknumber(state, 2)));
    }
    return 1;
}

static int math_modf(lua_State* state) {
    if (lua_isinteger(state, 1)) {
        lua_settop(state, 1);
        lua_pushnumber(state, 0);
    } else {
        const double number = luaL_checknumber(state, 1);
        const double whole = number < 0 ? std::ceil(number) : std::floor(number);
        push_number_or_integer(state, whole);
        lua_pushnumber(state, number == whole ? 0.0 : number - whole);
    }
    return 2;
}

static int math_sqrt(lua_State* state) {
    lua_pushnumber(state, std::sqrt(luaL_checknumber(state, 1)));
    return 1;
}

static int math_ult(lua_State* state) {
    const lua_Integer left = luaL_checkinteger(state, 1);
    const lua_Integer right = luaL_checkinteger(state, 2);
    lua_pushboolean(state, static_cast<lua_Unsigned>(left) < static_cast<lua_Unsigned>(right));
    return 1;
}

static int math_log(lua_State* state) {
    const double value = luaL_checknumber(state, 1);
    double result = 0;
    if (lua_isnoneornil(state, 2)) {
        result = natural_log(value);
    } else {
        const double base = luaL_checknumber(state, 2);
        if (base == 2.0)
            result = log2(value);
        else if (base == 10.0)
            result = log10(value);
        else
            result = natural_log(value) / natural_log(base);
    }
    lua_pushnumber(state, result);
    return 1;
}

static int math_exp(lua_State* state) {
    lua_pushnumber(state, exponential(luaL_checknumber(state, 1)));
    return 1;
}

static int math_deg(lua_State* state) {
    lua_pushnumber(state, luaL_checknumber(state, 1) * (180.0 / pi));
    return 1;
}

static int math_rad(lua_State* state) {
    lua_pushnumber(state, luaL_checknumber(state, 1) * (pi / 180.0));
    return 1;
}

static int math_min(lua_State* state) {
    const int count = lua_gettop(state);
    int chosen = 1;
    luaL_argcheck(state, count >= 1, 1, "value expected");
    for (int index = 2; index <= count; ++index) {
        charge(state, comparison_charge(state, index, chosen));
        if (lua_compare(state, index, chosen, LUA_OPLT))
            chosen = index;
    }
    lua_pushvalue(state, chosen);
    return 1;
}

static int math_max(lua_State* state) {
    const int count = lua_gettop(state);
    int chosen = 1;
    luaL_argcheck(state, count >= 1, 1, "value expected");
    for (int index = 2; index <= count; ++index) {
        charge(state, comparison_charge(state, chosen, index));
        if (lua_compare(state, chosen, index, LUA_OPLT))
            chosen = index;
    }
    lua_pushvalue(state, chosen);
    return 1;
}

static int math_type(lua_State* state) {
    if (lua_type(state, 1) == LUA_TNUMBER)
        lua_pushstring(state, lua_isinteger(state, 1) ? "integer" : "float");
    else {
        luaL_checkany(state, 1);
        luaL_pushfail(state);
    }
    return 1;
}

static int math_random(lua_State* state) {
    auto* random_state = static_cast<RandomState*>(lua_touserdata(state, lua_upvalueindex(1)));
    const uint64_t drawn = next_word(random_state->word);
    switch (lua_gettop(state)) {
    case 0:
        lua_pushnumber(state, unit_interval(drawn));
        return 1;
    case 1: {
        const lua_Integer upper = luaL_checkinteger(state, 1);
        if (upper == 0) {
            lua_pushinteger(state, static_cast<lua_Integer>(drawn));
            return 1;
        }
        luaL_argcheck(state, 1 <= upper, 1, "interval is empty");
        const lua_Unsigned span = static_cast<lua_Unsigned>(upper) - 1u;
        const lua_Unsigned value =
            project(static_cast<lua_Unsigned>(drawn), span, random_state->word);
        lua_pushinteger(state, static_cast<lua_Integer>(value + 1u));
        return 1;
    }
    case 2: {
        const lua_Integer lower = luaL_checkinteger(state, 1);
        const lua_Integer upper = luaL_checkinteger(state, 2);
        luaL_argcheck(state, lower <= upper, 1, "interval is empty");
        const lua_Unsigned span =
            static_cast<lua_Unsigned>(upper) - static_cast<lua_Unsigned>(lower);
        const lua_Unsigned value =
            project(static_cast<lua_Unsigned>(drawn), span, random_state->word);
        lua_pushinteger(state, static_cast<lua_Integer>(value + static_cast<lua_Unsigned>(lower)));
        return 1;
    }
    default:
        return luaL_error(state, "wrong number of arguments");
    }
}

static int math_randomseed(lua_State* state) {
    auto* random_state = static_cast<RandomState*>(lua_touserdata(state, lua_upvalueindex(1)));
    if (lua_isnone(state, 1)) {
        set_seed(state, random_state->word, control_from(state)->random_seed, 0);
    } else {
        const lua_Integer first = luaL_checkinteger(state, 1);
        const lua_Integer second = luaL_optinteger(state, 2, 0);
        set_seed(
            state, random_state->word, static_cast<uint64_t>(first), static_cast<uint64_t>(second)
        );
    }
    return 2;
}
} // extern "C"

const luaL_Reg math_functions[] = {
    {"abs", math_abs},     {"acos", math_acos}, {"asin", math_asin},
    {"atan", math_atan},   {"ceil", math_ceil}, {"cos", math_cos},
    {"deg", math_deg},     {"exp", math_exp},   {"tointeger", math_tointeger},
    {"floor", math_floor}, {"fmod", math_fmod}, {"ult", math_ult},
    {"log", math_log},     {"max", math_max},   {"min", math_min},
    {"modf", math_modf},   {"rad", math_rad},   {"sin", math_sin},
    {"sqrt", math_sqrt},   {"tan", math_tan},   {"type", math_type},
    {nullptr, nullptr}
};

const luaL_Reg random_functions[] = {
    {"random", math_random}, {"randomseed", math_randomseed}, {nullptr, nullptr}
};

extern "C" {
static int open_math(lua_State* state) {
    luaL_newlib(state, math_functions);
    auto* random_state =
        static_cast<RandomState*>(lua_newuserdatauv(state, sizeof(RandomState), 0));
    set_seed(state, random_state->word, control_from(state)->random_seed, 0);
    lua_pop(state, 2);
    luaL_setfuncs(state, random_functions, 1);
    lua_pushnumber(state, pi);
    lua_setfield(state, -2, "pi");
    lua_pushnumber(state, static_cast<lua_Number>(HUGE_VAL));
    lua_setfield(state, -2, "huge");
    lua_pushinteger(state, LUA_MAXINTEGER);
    lua_setfield(state, -2, "maxinteger");
    lua_pushinteger(state, LUA_MININTEGER);
    lua_setfield(state, -2, "mininteger");
    return 1;
}
} // extern "C"

} // namespace

void open_math_library(lua_State* state) {
    luaL_requiref(state, "math", open_math, 1);
    lua_pop(state, 1);
}

} // namespace oa::data::script_host
