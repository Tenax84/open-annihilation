# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Legacy rule files: text in, rule records and diagnostics out. The reader
# opens nothing; applying the records belongs to the mutator pipeline.
add_library(oa-formats-soft-mutator src/parse.cpp src/records.cpp)
add_library(oa::formats::soft_mutator ALIAS oa-formats-soft-mutator)
target_include_directories(oa-formats-soft-mutator PUBLIC include)
target_link_libraries(oa-formats-soft-mutator PRIVATE oa-formats-tdf oa-options)
target_compile_features(oa-formats-soft-mutator PUBLIC cxx_std_20)
