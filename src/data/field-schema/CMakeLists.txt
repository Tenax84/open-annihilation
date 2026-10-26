# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Typed ranges for the FBI, weapon and movement-class keys the loaders read.
add_library(oa-data-field-schema src/fields.cpp src/check.cpp)
add_library(oa::data::field_schema ALIAS oa-data-field-schema)
target_include_directories(oa-data-field-schema PUBLIC include)
target_compile_features(oa-data-field-schema PUBLIC cxx_std_20)
target_link_libraries(oa-data-field-schema PUBLIC oa-formats-tdf PRIVATE oa-options)

if(BUILD_TESTING)
  add_executable(oa-data-field-schema-test tests/field_schema_test.cpp)
  target_link_libraries(oa-data-field-schema-test PRIVATE oa-data-field-schema oa-test-support
    oa-formats-hpi oa-test-game-data)
  add_test(NAME data-field-schema COMMAND oa-data-field-schema-test)
  oa_add_game_data_test(data-field-schema-data oa-data-field-schema-test --data)
  set_tests_properties(data-field-schema-data PROPERTIES TIMEOUT 120)

  add_executable(oa-data-field-schema-defaults-test tests/defaults_test.cpp)
  target_link_libraries(oa-data-field-schema-defaults-test PRIVATE oa-data-field-schema oa-data-defs
    oa-data-mission-types oa-base-text oa-base-game-math oa-test-support)
  add_test(NAME data-field-schema-defaults COMMAND oa-data-field-schema-defaults-test)
  set_tests_properties(data-field-schema-defaults PROPERTIES TIMEOUT 30)
endif()
