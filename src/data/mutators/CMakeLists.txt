# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

add_library(
  oa-data-mutators
  src/allow_list.cpp
  src/collections.cpp
  src/files_layer.cpp
  src/journal.cpp
  src/pipeline.cpp
  src/rules_lua.cpp
)
add_library(oa::data::mutators ALIAS oa-data-mutators)
target_include_directories(oa-data-mutators PUBLIC include)
target_compile_features(oa-data-mutators PUBLIC cxx_std_20)
target_link_libraries(
  oa-data-mutators
  PUBLIC oa-data-defs oa-data-mutation-rules oa-data-script-host oa-formats-hpi
         oa-formats-oamut
  PRIVATE oa-base-sha256 oa-data-defs-assets oa-data-field-schema oa-formats-tdf
          oa-options
)
if(BUILD_TESTING)
  add_executable(oa-data-mutators-allow-list-test tests/allow_list_test.cpp)
  target_link_libraries(oa-data-mutators-allow-list-test PRIVATE oa-data-mutators oa-test-support)
  add_test(NAME data-mutators-allow-list COMMAND oa-data-mutators-allow-list-test)

  add_executable(oa-data-mutators-collections-test tests/collections_test.cpp)
  target_link_libraries(
    oa-data-mutators-collections-test PRIVATE oa-data-mutators oa-data-script-host oa-test-support
  )
  target_compile_definitions(
    oa-data-mutators-collections-test
    PRIVATE "OA_MUTATORS_COLLECTIONS_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/collections\""
  )
  add_test(NAME data-mutators-collections COMMAND oa-data-mutators-collections-test)

  add_executable(oa-data-mutators-files-layer-test tests/files_layer_test.cpp)
  target_link_libraries(
    oa-data-mutators-files-layer-test
    PRIVATE oa-data-mutators oa-formats-hpi oa-test-support oa-options
  )
  add_test(NAME data-mutators-files-layer COMMAND oa-data-mutators-files-layer-test)

  add_executable(oa-data-mutators-rules-lua-test tests/rules_lua_test.cpp)
  target_link_libraries(
    oa-data-mutators-rules-lua-test PRIVATE oa-data-mutators oa-data-script-host oa-test-support
  )
  target_compile_definitions(
    oa-data-mutators-rules-lua-test
    PRIVATE "OA_MUTATORS_RULES_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/rules_lua\""
  )
  add_test(NAME data-mutators-rules-lua COMMAND oa-data-mutators-rules-lua-test)
endif()
