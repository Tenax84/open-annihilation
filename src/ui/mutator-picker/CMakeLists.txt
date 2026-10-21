# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# The mutator picker's model: which mutators are on, in which order, with
# which options and clash mode, and the words its rows, footer and question
# show. It draws nothing and reads no file, clock, preference or network.
add_library(oa-ui-mutator-picker src/model.cpp src/text.cpp)
add_library(oa::ui::mutator_picker ALIAS oa-ui-mutator-picker)
target_include_directories(oa-ui-mutator-picker PUBLIC include)
target_compile_features(oa-ui-mutator-picker PUBLIC cxx_std_20)
target_link_libraries(oa-ui-mutator-picker PUBLIC oa-ui-library oa-ui-kit
  PRIVATE oa-formats-oamod oa-formats-oamut oa-options)

if(BUILD_TESTING)
  # Opening a set, prerequisites and their order, locked rows, refreshing,
  # conflicts, every option type, the rows, the footer, the hint, the
  # turn-off question, and random walks.
  add_executable(oa-ui-mutator-picker-model-test tests/model_test.cpp)
  target_link_libraries(oa-ui-mutator-picker-model-test PRIVATE oa-ui-mutator-picker
    oa-formats-oamut oa-test-support oa-options)
  add_test(NAME ui-mutator-picker-model COMMAND oa-ui-mutator-picker-model-test)
  set_tests_properties(ui-mutator-picker-model PROPERTIES TIMEOUT 60)
endif()
