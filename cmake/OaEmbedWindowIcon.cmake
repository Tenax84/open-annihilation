# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Writes the C++ source that holds the PNG file of one of the icons
# window_icon.hpp declares, run as a script:
#   cmake -DINPUT=<png> -DOUTPUT=<source> [-DFUNCTION=<name>] [-DHEADER=<header>]
#     -P OaEmbedWindowIcon.cmake
# The source defines oa::app::<FUNCTION>() over the file's bytes:
# window_icon_png() when FUNCTION is not given, settings_icon_png() for the
# settings icon and mods_icon_png() for the mods icon. HEADER is the header
# that declares the function, oa/app/window_icon.hpp when not given; the
# main menu's art is declared in oa/app/main_menu_art.hpp. OUTPUT is
# rewritten only when its text changes.
if(NOT INPUT OR NOT OUTPUT)
  message(FATAL_ERROR "OaEmbedWindowIcon.cmake needs -DINPUT=<png> -DOUTPUT=<source>")
endif()
if(NOT FUNCTION)
  set(FUNCTION window_icon_png)
endif()
if(NOT HEADER)
  set(HEADER oa/app/window_icon.hpp)
endif()
file(READ "${INPUT}" hex HEX)
string(LENGTH "${hex}" hex_digits)
if(hex_digits EQUAL 0)
  message(FATAL_ERROR "${INPUT} is empty")
endif()
# Sixteen bytes to a line; CMake's expressions have no counted repeat.
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1, " bytes "${hex}")
string(REPEAT "0x[0-9a-f][0-9a-f], " 16 line)
string(REGEX REPLACE "(${line})" "\\1\n    " bytes "${bytes}")
string(REGEX REPLACE "[ \n]+$" "" bytes "${bytes}")
string(REGEX REPLACE ", \n" ",\n" bytes "${bytes}")
get_filename_component(name "${INPUT}" NAME)
file(CONFIGURE OUTPUT "${OUTPUT}" @ONLY CONTENT [[
// Written by cmake/OaEmbedWindowIcon.cmake from @name@; edit the icon, not this file.
#include "@HEADER@"

namespace oa::app {
namespace {

// The bytes of @name@.
constexpr uint8_t icon_file[] = {
    @bytes@
};

} // namespace

std::span<const uint8_t> @FUNCTION@() {
    return icon_file;
}

} // namespace oa::app
]])
