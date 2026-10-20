// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Lua 5.4.9 as one C translation unit, in the order onelua.c compiles it.
// The coroutine, debug, input and output, maths, operating-system and
// package libraries are left out, and so is linit.c, which would open
// every library. Only the lua_*, luaL_* and luaopen_* functions stay
// external.

#define LUA_CORE
#define LUA_LIB
#define ltable_c
#define lvm_c

#include "lua_config.h"

#undef LUAI_FUNC
#define LUAI_FUNC static
#undef LUAI_DDEC
#define LUAI_DDEC(def) /* empty */
#undef LUAI_DDEF
#define LUAI_DDEF static

#include "lzio.c"
#include "lctype.c"
#include "lopcodes.c"
#include "lmem.c"
#include "lundump.c"
#include "ldump.c"
#include "lstate.c"
#include "lgc.c"
#include "llex.c"
#include "lcode.c"
#include "lparser.c"
#include "ldebug.c"
#include "lfunc.c"
#include "lobject.c"
#include "ltm.c"
#include "lstring.c"
#include "ltable.c"
#include "ldo.c"
#include "lvm.c"
#include "lapi.c"
#include "lauxlib.c"
#include "lbaselib.c"
#include "lstrlib.c"
#include "ltablib.c"
#include "lutf8lib.c"
