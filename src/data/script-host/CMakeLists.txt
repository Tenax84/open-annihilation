# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Lua 5.4.9 for the script host. The sources in third_party/lua are compiled
# unchanged, as C, in one translation unit, with the engine's floating-point
# options and with warnings off.

add_library(oa-data-script-host-lua STATIC src/lua_build.c)
add_library(oa::data::script_host_lua ALIAS oa-data-script-host-lua)
target_include_directories(oa-data-script-host-lua SYSTEM PUBLIC
  "${PROJECT_SOURCE_DIR}/third_party/lua")
if(MSVC)
  # /W0 turns every warning off. oa-options is left out: an option given after
  # the warning level it sets would not replace that level. /fp:strict is set
  # on this target, which is what oa-options would have carried, so each
  # floating-point operation rounds as written.
  target_compile_options(oa-data-script-host-lua PRIVATE /W0 /fp:strict)
else()
  target_link_libraries(oa-data-script-host-lua PRIVATE oa-options)
  target_compile_options(oa-data-script-host-lua PRIVATE -w)
endif()
# A C file otherwise uses the system C library's formatting. On Windows XP
# that library has no conversion for a long long and rounds decimal text
# differently from the formatting the engine's C++ files already use.
target_compile_definitions(oa-data-script-host-lua PRIVATE
  $<$<BOOL:${MINGW}>:__USE_MINGW_ANSI_STDIO=1>)
# pow, floor and the rest of the C maths calls. Visual Studio's C library
# provides them; every other target takes them from libm.
if(NOT MSVC)
  target_link_libraries(oa-data-script-host-lua PUBLIC m)
endif()
