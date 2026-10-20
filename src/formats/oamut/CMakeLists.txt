# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Mutator manifests (oamut.yaml): versions, the rules, and the text written back.
add_library(oa-formats-oamut src/version.cpp src/manifest.cpp src/write.cpp)
add_library(oa::formats::oamut ALIAS oa-formats-oamut)
target_include_directories(oa-formats-oamut PUBLIC include)
target_compile_features(oa-formats-oamut PUBLIC cxx_std_20)
target_link_libraries(oa-formats-oamut PUBLIC oa-formats-oamod PRIVATE oa-options)

if(BUILD_TESTING)
  add_executable(oa-formats-oamut-test tests/manifest_test.cpp)
  target_link_libraries(oa-formats-oamut-test PRIVATE oa-formats-oamut oa-test-support oa-options)
  target_compile_definitions(oa-formats-oamut-test PRIVATE
    "OA_OAMUT_CASES=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/version_cases.txt\"")
  add_test(NAME formats-oamut COMMAND oa-formats-oamut-test)
  set_tests_properties(formats-oamut PROPERTIES TIMEOUT 30)
endif()
