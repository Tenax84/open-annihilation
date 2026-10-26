# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Installed mutators: each folder's oamut.yaml and, when it reads, its
# origin record. It opens those two files and never a script.
add_library(oa-data-mutator-library src/library.cpp)
add_library(oa::data::mutator_library ALIAS oa-data-mutator-library)
target_include_directories(oa-data-mutator-library PUBLIC include)
target_compile_features(oa-data-mutator-library PUBLIC cxx_std_20)
target_link_libraries(oa-data-mutator-library PUBLIC oa-formats-oamut PRIVATE oa-options)

if(BUILD_TESTING)
  # A scratch Mutators folder listed from its manifests: good mutators,
  # broken and missing manifests, the installer's own folders, a link, two
  # folders with one id, origin records and a missing folder.
  add_executable(oa-data-mutator-library-test tests/library_test.cpp)
  target_link_libraries(oa-data-mutator-library-test PRIVATE oa-data-mutator-library
    oa-test-support oa-options)
  add_test(NAME data-mutator-library COMMAND oa-data-mutator-library-test)
  set_tests_properties(data-mutator-library PROPERTIES TIMEOUT 30)
endif()
