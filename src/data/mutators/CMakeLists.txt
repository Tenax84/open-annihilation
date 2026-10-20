# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

add_library(oa-data-mutators src/allow_list.cpp)
add_library(oa::data::mutators ALIAS oa-data-mutators)
target_include_directories(oa-data-mutators PUBLIC include)
target_compile_features(oa-data-mutators PUBLIC cxx_std_20)
target_link_libraries(oa-data-mutators PUBLIC oa-data-defs PRIVATE oa-formats-hpi oa-options)
if(BUILD_TESTING)
  add_executable(oa-data-mutators-allow-list-test tests/allow_list_test.cpp)
  target_link_libraries(oa-data-mutators-allow-list-test PRIVATE oa-data-mutators oa-test-support)
  add_test(NAME data-mutators-allow-list COMMAND oa-data-mutators-allow-list-test)
endif()
