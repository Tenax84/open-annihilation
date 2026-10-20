# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# TA: Mutation files become mutator packages. The library reads bytes it is
# given and writes none of its own.
add_library(oa-data-mutator-import src/legacy.cpp)
add_library(oa::data::mutator_import ALIAS oa-data-mutator-import)
target_include_directories(oa-data-mutator-import PUBLIC include)
target_compile_features(oa-data-mutator-import PUBLIC cxx_std_20)
target_link_libraries(
  oa-data-mutator-import
  PUBLIC oa-formats-oamut
  PRIVATE oa-formats-soft-mutator oa-formats-zip oa-formats-hpi oa-data-mutators
    oa-data-script-host oa-options
)

if(BUILD_TESTING)
  # Made-up .tam texts, ids, credits and packages, including a zip whose
  # names are code page 437.
  add_executable(oa-data-mutator-import-test tests/import_test.cpp)
  target_link_libraries(
    oa-data-mutator-import-test
    PRIVATE oa-data-mutator-import oa-formats-hpi oa-data-script-host oa-formats-oamut
      ZLIB::ZLIB oa-test-support oa-options
  )
  add_test(NAME data-mutator-import COMMAND oa-data-mutator-import-test)
  set_tests_properties(data-mutator-import PROPERTIES TIMEOUT 30)
endif()
