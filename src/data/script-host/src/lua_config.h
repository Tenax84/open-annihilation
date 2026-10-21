// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// lua_build.c alone includes this header. Code that calls Lua includes
// lua.hpp and never sees these settings: none of them changes a type or
// a layout.

// One constant seed for every string hash. 0x4F414C55 spells the letters OALU.
#define luai_makeseed(L) ((void)(L), 0x4F414C55u) // stops the clock or a place seeding hashes
#define l_randomizePivot() (~0u)                  // stops table.sort drawing a pivot from the clock
#define lua_getlocaledecpoint() ('.')             // stops numbers taking the locale's decimal point

// pow differs across libm and the old Windows runtime, so the operator calls
// the engine. Squaring stays a multiply: that result is exact in IEEE 754.
/// Returns `base` raised to `exponent` with the engine's arithmetic.
double oa_script_host_power(double base, double exponent);
#define luai_numpow(L, a, b) ((void)(L), (b) == 2 ? (a) * (a) : oa_script_host_power((a), (b)))

#include "lprefix.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "luaconf.h"

#undef lua_strx2number // stops hexadecimal numbers being read by the C library
#undef lua_number2strx // stops hexadecimal numbers being written by the C library

#ifdef __cplusplus
extern "C" {
#endif
/// Writes a float as script text: a NaN of either sign is "nan", the
/// infinities are "inf" and "-inf", and anything else is "%.14g". Like
/// snprintf, it cuts the text to `size` bytes and returns its whole length.
int oa_script_host_number_text(char* buffer, size_t size, double value);
#ifdef __cplusplus
}
#endif
#undef lua_number2str // stops a NaN or an infinity taking the C library's spelling
#define lua_number2str(s, sz, n) oa_script_host_number_text((s), (sz), (n))

#undef lua_pointer2str // stops a table, a function or userdata showing where it sits
#define lua_pointer2str(buff, sz, p) ((void)(p), l_sprintf((buff), (sz), "%s", "hidden"))
