# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Lua 5.4.9 for the script host. The sources in third_party/lua are compiled
# unchanged, as C, in one translation unit, with the engine's floating-point
# options and with warnings off.

# The ^ operator calls this library, so it has to follow the Lua archive on a
# link line. H05 adds its number text here as well. libm supplies sqrt and
# frexp; a Linux link drops an earlier libm once those references appear.
add_library(oa-data-script-host-numbers STATIC src/deterministic_math.cpp)
add_library(oa::data::script_host_numbers ALIAS oa-data-script-host-numbers)
target_include_directories(oa-data-script-host-numbers PUBLIC include)
target_compile_features(oa-data-script-host-numbers PUBLIC cxx_std_20)
target_link_libraries(oa-data-script-host-numbers PRIVATE oa-base-game-math oa-options)
if(NOT MSVC)
  target_link_libraries(oa-data-script-host-numbers PRIVATE m)
endif()

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
# floor, frexp and the other C maths calls the interpreter still makes.
# Visual Studio's C library provides them; every other target takes them
# from libm. The ^ operator is not one of them.
if(NOT MSVC)
  target_link_libraries(oa-data-script-host-lua PUBLIC m)
endif()
target_link_libraries(oa-data-script-host-lua PUBLIC oa-data-script-host-numbers)

add_library(oa-data-script-host
  src/sandbox.cpp
  src/libraries.cpp
  src/require.cpp)
add_library(oa::data::script_host ALIAS oa-data-script-host)
target_include_directories(oa-data-script-host PUBLIC include)
target_compile_features(oa-data-script-host PUBLIC cxx_std_20)
target_link_libraries(oa-data-script-host
  PUBLIC oa-data-script-host-lua
  PRIVATE oa-options)

if(BUILD_TESTING)
  # A text chunk, the open libraries and their numbers, one table order from
  # three states, and a refused precompiled chunk.
  add_executable(oa-data-script-host-lua-test tests/lua_build_test.cpp)
  target_link_libraries(oa-data-script-host-lua-test PRIVATE
    oa-data-script-host-lua oa-test-support oa-options)
  add_test(NAME data-script-host-lua COMMAND oa-data-script-host-lua-test)
  set_tests_properties(data-script-host-lua PROPERTIES TIMEOUT 30)

  # Libraries, require, errors, profiles and a phase that does not run a chunk.
  add_executable(oa-data-script-host-sandbox-test tests/sandbox_test.cpp)
  target_link_libraries(oa-data-script-host-sandbox-test PRIVATE
    oa-data-script-host oa-test-support oa-options)
  add_test(NAME data-script-host-sandbox COMMAND oa-data-script-host-sandbox-test)
  set_tests_properties(data-script-host-sandbox PROPERTIES TIMEOUT 60)

  # Instruction and memory allowances, including one run at the default budget.
  add_executable(oa-data-script-host-budgets-test tests/budgets_test.cpp)
  target_link_libraries(oa-data-script-host-budgets-test PRIVATE
    oa-data-script-host oa-test-support oa-options)
  add_test(NAME data-script-host-budgets COMMAND oa-data-script-host-budgets-test)
  set_tests_properties(data-script-host-budgets PROPERTIES TIMEOUT 120)
endif()
