# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# The mutator picker's model and its screen: which mutators are on, in which
# order, with which options and clash mode, and the kit window that draws
# them. It reads no file, clock, preference or network.
add_library(oa-ui-mutator-picker src/draw.cpp src/input.cpp src/layout.cpp src/model.cpp
  src/prompts.cpp src/text.cpp)
add_library(oa::ui::mutator_picker ALIAS oa-ui-mutator-picker)
target_include_directories(oa-ui-mutator-picker PUBLIC include)
target_compile_features(oa-ui-mutator-picker PUBLIC cxx_std_20)
target_link_libraries(oa-ui-mutator-picker PUBLIC oa-ui-library oa-ui-kit
  PRIVATE oa-formats-oamod oa-formats-oamut oa-options)

if(BUILD_TESTING)
  # Opening a set, prerequisites and their order, locked rows, refreshing,
  # conflicts, every option type and a number's values, the rows, the
  # footer, the hint, the turn-off question, and random walks.
  add_executable(oa-ui-mutator-picker-model-test tests/model_test.cpp)
  target_link_libraries(oa-ui-mutator-picker-model-test PRIVATE oa-ui-mutator-picker
    oa-formats-oamut oa-test-support oa-options)
  add_test(NAME ui-mutator-picker-model COMMAND oa-ui-mutator-picker-model-test)
  set_tests_properties(ui-mutator-picker-model PROPERTIES TIMEOUT 60)

  # The four questions: one mutator and several, a download before and after
  # it stops, the texts fitting at every class, and a lookup that translates.
  add_executable(oa-ui-mutator-picker-prompts-test tests/prompts_test.cpp)
  target_link_libraries(oa-ui-mutator-picker-prompts-test PRIVATE oa-ui-mutator-picker oa-ui-kit
    oa-test-support)
  add_test(NAME ui-mutator-picker-prompts COMMAND oa-ui-mutator-picker-prompts-test)
  set_tests_properties(ui-mutator-picker-prompts PROPERTIES TIMEOUT 30)

  # The pointer, a finger, the keys, typed text, the wheel and the controller.
  add_executable(oa-ui-mutator-picker-input-test tests/input_test.cpp)
  target_link_libraries(oa-ui-mutator-picker-input-test PRIVATE oa-ui-mutator-picker oa-ui-kit
    oa-test-support oa-options)
  add_test(NAME ui-mutator-picker-input COMMAND oa-ui-mutator-picker-input-test)
  set_tests_properties(ui-mutator-picker-input PROPERTIES TIMEOUT 60)

  # The screen at Compact, Regular and Large and at three windows: parts
  # inside the window, no two controls meeting, every row's grip and switch,
  # the options, Tab's order, the names, and the texts fitting.
  add_executable(oa-ui-mutator-picker-layout-test tests/layout_test.cpp)
  target_link_libraries(oa-ui-mutator-picker-layout-test PRIVATE oa-ui-mutator-picker oa-ui-kit
    oa-test-support oa-options)
  add_test(NAME ui-mutator-picker-layout COMMAND oa-ui-mutator-picker-layout-test)
  set_tests_properties(ui-mutator-picker-layout PROPERTIES TIMEOUT 60)

  # Each picture drawn twice in the installed game's fonts, byte for byte the
  # same and nothing outside the window; with --frames DIR it writes them.
  add_executable(oa-ui-mutator-picker-draw-test tests/draw_test.cpp)
  target_link_libraries(oa-ui-mutator-picker-draw-test PRIVATE oa-ui-mutator-picker oa-ui-kit
    oa-data-languages oa-formats-png oa-formats-hpi oa-platform-text-font oa-present
    oa-test-game-data oa-test-support oa-options)
  oa_copy_text_fonts(oa-ui-mutator-picker-draw-test
    "$<TARGET_FILE_DIR:oa-ui-mutator-picker-draw-test>")
  oa_add_game_data_test(ui-mutator-picker-draw-data oa-ui-mutator-picker-draw-test --data
    "${PROJECT_SOURCE_DIR}/languages/interface.tdf")
  set_tests_properties(ui-mutator-picker-draw-data PROPERTIES TIMEOUT 60)
endif()
