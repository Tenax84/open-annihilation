# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Parsed documents for one pipeline run. One mutator's rules are a step
# on those documents.
add_library(oa-data-mutation-rules src/documents.cpp src/needs.cpp src/apply.cpp)
add_library(oa::data::mutation_rules ALIAS oa-data-mutation-rules)
target_include_directories(oa-data-mutation-rules PUBLIC include)
target_compile_features(oa-data-mutation-rules PUBLIC cxx_std_20)
target_link_libraries(oa-data-mutation-rules
  PUBLIC oa-formats-soft-mutator oa-formats-tdf oa-data-defs
  PRIVATE oa-data-field-schema oa-options)

if(BUILD_TESTING)
  add_executable(oa-data-mutation-rules-test tests/mutation_rules_test.cpp)
  target_link_libraries(oa-data-mutation-rules-test PRIVATE oa-data-mutation-rules oa-test-support)
  add_test(NAME data-mutation-rules COMMAND oa-data-mutation-rules-test)

  add_executable(oa-data-mutation-rules-apply-test tests/apply_test.cpp)
  target_link_libraries(
    oa-data-mutation-rules-apply-test PRIVATE oa-data-mutation-rules oa-test-support
  )
  add_test(NAME data-mutation-rules-apply COMMAND oa-data-mutation-rules-apply-test)
endif()
