# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Writes the C++ source that holds one of the fragment shaders
# oa/app/card/shaders.hpp declares, run as a script:
#   cmake -DFUNCTION=<name> -DOUTPUT=<source> -DSAMPLERS=<n> -DUNIFORM_BUFFERS=<n>
#     [-DSPIRV=<file> -DDXIL=<file> -DMSL=<file>] -P OaEmbedShader.cmake
# The source defines oa::app::card::<FUNCTION>() over the bytes of the
# three compiled forms; a form whose file is not given is left empty, as a
# build without the shader compiler leaves all three. OUTPUT is rewritten
# only when its text changes.
if(NOT FUNCTION OR NOT OUTPUT)
  message(FATAL_ERROR "OaEmbedShader.cmake needs -DFUNCTION=<name> -DOUTPUT=<source>")
endif()
if(NOT SAMPLERS)
  set(SAMPLERS 0)
endif()
if(NOT UNIFORM_BUFFERS)
  set(UNIFORM_BUFFERS 0)
endif()
# The bytes of one form as an array of `kind`, or an empty array.
function(oa_shader_bytes kind file)
  if(NOT file)
    set(${kind}_array "constexpr std::span<const uint8_t> ${kind}_code{};" PARENT_SCOPE)
    return()
  endif()
  file(READ "${file}" hex HEX)
  string(LENGTH "${hex}" hex_digits)
  if(hex_digits EQUAL 0)
    message(FATAL_ERROR "${file} is empty")
  endif()
  # Sixteen bytes to a line; CMake's expressions have no counted repeat.
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1, " bytes "${hex}")
  string(REPEAT "0x[0-9a-f][0-9a-f], " 16 line)
  string(REGEX REPLACE "(${line})" "\\1\n    " bytes "${bytes}")
  string(REGEX REPLACE "[ \n]+$" "" bytes "${bytes}")
  string(REGEX REPLACE ", \n" ",\n" bytes "${bytes}")
  set(${kind}_array "constexpr uint8_t ${kind}_code[] = {\n    ${bytes}\n};" PARENT_SCOPE)
endfunction()
oa_shader_bytes(spirv "${SPIRV}")
oa_shader_bytes(dxil "${DXIL}")
oa_shader_bytes(msl "${MSL}")
file(CONFIGURE OUTPUT "${OUTPUT}" @ONLY CONTENT [[
// Written by cmake/OaEmbedShader.cmake; edit the shader under src/app/card/shaders, not this file.
#include "oa/app/card/shaders.hpp"

namespace oa::app::card {
namespace {

@spirv_array@

@dxil_array@

@msl_array@

} // namespace

ShaderCode @FUNCTION@() noexcept {
    return ShaderCode{spirv_code, dxil_code, msl_code, @SAMPLERS@, @UNIFORM_BUFFERS@};
}

} // namespace oa::app::card
]])
