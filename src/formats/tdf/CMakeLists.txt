# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

cmake_minimum_required(VERSION 3.24)
project(oa_formats_tdf LANGUAGES CXX)

include(CTest)

add_library(oa-formats-tdf src/tdf.cpp src/edit.cpp src/write.cpp src/number.cpp)
add_library(oa::formats::tdf ALIAS oa-formats-tdf)
# In the engine these targets supply the two header-only helpers tdf.cpp
# calls. A standalone configure has neither target; the helpers are then
# found from the source tree beside this module.
if(TARGET oa-base-text)
  target_link_libraries(oa-formats-tdf PRIVATE oa-base-text oa-base-game-math)
else()
  set(oa_tdf_here "${CMAKE_CURRENT_SOURCE_DIR}")
  cmake_path(GET oa_tdf_here PARENT_PATH oa_tdf_formats)
  cmake_path(GET oa_tdf_formats PARENT_PATH oa_tdf_src)
  cmake_path(APPEND oa_tdf_src "base" "text" "include" OUTPUT_VARIABLE oa_tdf_text_include)
  cmake_path(APPEND oa_tdf_src "base" "game-math" "include" OUTPUT_VARIABLE oa_tdf_math_include)
  target_include_directories(oa-formats-tdf PRIVATE "${oa_tdf_text_include}" "${oa_tdf_math_include}")
endif()
target_include_directories(oa-formats-tdf PUBLIC include)
target_compile_features(oa-formats-tdf PUBLIC cxx_std_20)
if(TARGET oa-options)
  target_link_libraries(oa-formats-tdf PUBLIC oa-options)
endif()

if(BUILD_TESTING)
  add_executable(oa-tdf-test tests/tdf_test.cpp)
  target_link_libraries(oa-tdf-test PRIVATE oa-formats-tdf)
  add_test(NAME tdf-parser COMMAND oa-tdf-test)
  add_executable(oa-tdf-writer-test tests/writer_test.cpp)
  target_link_libraries(oa-tdf-writer-test PRIVATE oa-formats-tdf)
  add_test(NAME tdf-writer COMMAND oa-tdf-writer-test)
endif()
