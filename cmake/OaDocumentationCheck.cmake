# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# oa-doc-check: Clang reads the /// blocks of the engine's public headers.
#
# Each header under an include/ directory of src/ (network play's Runtime
# members, a part of class Runtime, through runtime.hpp), and each header of
# src/app that oa-extension-sdk publishes to an extension, is compiled on its own in
# a generated source that includes it, with the usage requirements of every
# library in the tree. Clang's documentation warnings are errors there: a
# @param that names no parameter, @return on a function returning void, a
# command it does not know (@quirk is declared as one) or a malformed block.
# The target exists only when the C++ compiler is Clang or AppleClang. A
# plain build leaves it out; the test doc-check builds it, so ctest runs the
# check with the other tests, and CI builds it on macOS.
include_guard(GLOBAL)

# Collects every library target defined in directory and the directories below it.
function(oa_collect_library_targets directory out_var)
  set(targets "")
  get_property(directory_targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
  foreach(target IN LISTS directory_targets)
    get_target_property(type ${target} TYPE)
    if(type MATCHES "^(STATIC|SHARED|OBJECT|INTERFACE)_LIBRARY$")
      list(APPEND targets ${target})
    endif()
  endforeach()
  get_property(subdirectories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
  foreach(subdirectory IN LISTS subdirectories)
    oa_collect_library_targets("${subdirectory}" subdirectory_targets)
    list(APPEND targets ${subdirectory_targets})
  endforeach()
  set(${out_var} ${targets} PARENT_SCOPE)
endfunction()

# Registers doc-check, the test that builds oa-doc-check in this build tree
# and fails when that build fails. Its first run compiles every header's
# source, about a minute at eight jobs; once the check has passed, a later
# run compiles again only the sources whose headers changed. The timeout
# leaves room for a loaded machine. The build may also build a library the
# headers need, so no other test runs beside it. Where the compiler is not
# Clang there is no target, and the test skips with ctest's skip return
# code, 77.
function(oa_add_documentation_check_test)
  if(NOT BUILD_TESTING)
    return()
  endif()
  if(TARGET oa-doc-check)
    add_test(NAME doc-check COMMAND "${CMAKE_COMMAND}" --build "${PROJECT_BINARY_DIR}"
      --config "$<CONFIG>" --target oa-doc-check)
    set_tests_properties(doc-check PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE)
    return()
  endif()
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME doc-check COMMAND ${Python3_EXECUTABLE} -c "raise SystemExit(77)")
    set_tests_properties(doc-check PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
  endif()
endfunction()

# Defines oa-doc-check from the headers and library targets of the whole
# tree, and its test (oa_add_documentation_check_test); call it after every
# target exists.
function(oa_add_documentation_check)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    oa_add_documentation_check_test()
    return()
  endif()
  oa_collect_library_targets("${PROJECT_SOURCE_DIR}" libraries)
  file(GLOB_RECURSE headers CONFIGURE_DEPENDS RELATIVE "${PROJECT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/src/*.h" "${PROJECT_SOURCE_DIR}/src/*.hpp")
  list(FILTER headers INCLUDE REGEX "/include/")
  list(FILTER headers EXCLUDE REGEX "(^|/)tests/")
  if(TARGET oa-extension-sdk)
    file(GLOB app_headers CONFIGURE_DEPENDS RELATIVE "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src/app/*.hpp")
    list(APPEND headers ${app_headers})
  endif()
  list(SORT headers)
  set(sources "")
  foreach(relative IN LISTS headers)
    set(header "${PROJECT_SOURCE_DIR}/${relative}")
    set(source "${PROJECT_BINARY_DIR}/doc-check/${relative}.cpp")
    file(CONFIGURE OUTPUT "${source}" CONTENT "#include \"${header}\"\n" @ONLY)
    list(APPEND sources "${source}")
  endforeach()
  add_library(oa-doc-check OBJECT EXCLUDE_FROM_ALL ${sources})
  add_library(oa::doc::check ALIAS oa-doc-check)
  target_link_libraries(oa-doc-check PRIVATE ${libraries})
  # SDL's package marks its headers as the project's own, not the system's;
  # its documentation is not the engine's to check.
  if(TARGET SDL3::Headers)
    get_target_property(sdl_include_directories SDL3::Headers INTERFACE_INCLUDE_DIRECTORIES)
    target_include_directories(oa-doc-check SYSTEM PRIVATE ${sdl_include_directories})
  endif()
  target_compile_options(oa-doc-check PRIVATE
    -Wdocumentation -Wdocumentation-unknown-command -fcomment-block-commands=quirk
    -Werror=documentation -Werror=documentation-unknown-command)
  oa_add_documentation_check_test()
endfunction()
