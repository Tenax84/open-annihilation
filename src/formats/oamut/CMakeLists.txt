# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Mutator manifests (oamut.yaml): versions, the rules, and the text written back.
add_library(oa-formats-oamut src/version.cpp)
add_library(oa::formats::oamut ALIAS oa-formats-oamut)
target_include_directories(oa-formats-oamut PUBLIC include)
target_compile_features(oa-formats-oamut PUBLIC cxx_std_20)
target_link_libraries(oa-formats-oamut PUBLIC oa-formats-oamod PRIVATE oa-options)
