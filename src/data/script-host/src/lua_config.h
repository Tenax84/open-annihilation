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
