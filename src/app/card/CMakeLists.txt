# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# The card command list (card.hpp): what one frame asks the graphics card
# to draw, as plain data with no SDL, with its check and the quad builder.
add_library(oa-app-card src/frame.cpp)
add_library(oa::app::card ALIAS oa-app-card)
target_include_directories(oa-app-card PUBLIC include)
target_compile_features(oa-app-card PUBLIC cxx_std_20)
target_link_libraries(oa-app-card PRIVATE oa-options)
# The card's fragment shaders (card/shaders.hpp), compiled from HLSL by
# SDL_shadercross into SPIR-V, DXIL and Metal source and embedded in the
# game. OA_SHADERCROSS names the compiler, found under
# local/deps/shadercross-install when not given; without it each shader is
# embedded empty and the card's effects stay off.
find_program(OA_SHADERCROSS shadercross
  HINTS "${PROJECT_SOURCE_DIR}/local/deps/shadercross-install/bin"
  DOC "SDL_shadercross, which compiles the card's fragment shaders")
set(oa_card_shader_sources)
# Each shader: its name, the textures it reads and its uniform slots.
foreach(oa_card_shader IN ITEMS "blur;1;1")
  list(GET oa_card_shader 0 oa_card_shader_name)
  list(GET oa_card_shader 1 oa_card_shader_samplers)
  list(GET oa_card_shader 2 oa_card_shader_uniform_buffers)
  set(oa_card_shader_hlsl "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${oa_card_shader_name}.frag.hlsl")
  set(oa_card_shader_dir "${CMAKE_CURRENT_BINARY_DIR}/shaders")
  set(oa_card_shader_source "${oa_card_shader_dir}/${oa_card_shader_name}_shader.cpp")
  set(oa_card_shader_forms)
  set(oa_card_shader_compile)
  if(OA_SHADERCROSS)
    foreach(oa_card_shader_form IN ITEMS SPIRV DXIL MSL)
      set(oa_card_shader_file "${oa_card_shader_dir}/${oa_card_shader_name}.frag.${oa_card_shader_form}")
      list(APPEND oa_card_shader_compile
        COMMAND "${OA_SHADERCROSS}" "${oa_card_shader_hlsl}" -s HLSL -d ${oa_card_shader_form}
          -t fragment -o "${oa_card_shader_file}")
      list(APPEND oa_card_shader_forms "-D${oa_card_shader_form}=${oa_card_shader_file}")
    endforeach()
  endif()
  add_custom_command(OUTPUT "${oa_card_shader_source}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${oa_card_shader_dir}"
    ${oa_card_shader_compile}
    COMMAND ${CMAKE_COMMAND} -DFUNCTION=${oa_card_shader_name}_shader
      "-DOUTPUT=${oa_card_shader_source}" -DSAMPLERS=${oa_card_shader_samplers}
      -DUNIFORM_BUFFERS=${oa_card_shader_uniform_buffers} ${oa_card_shader_forms}
      -P "${PROJECT_SOURCE_DIR}/cmake/OaEmbedShader.cmake"
    DEPENDS "${oa_card_shader_hlsl}" "${PROJECT_SOURCE_DIR}/cmake/OaEmbedShader.cmake"
    COMMENT "Compiling the card's ${oa_card_shader_name} shader"
    VERBATIM)
  list(APPEND oa_card_shader_sources "${oa_card_shader_source}")
endforeach()
if(NOT OA_SHADERCROSS)
  message(STATUS "SDL_shadercross not found: the card's effects are built without their shaders")
endif()
add_library(oa-app-card-shaders ${oa_card_shader_sources})
add_library(oa::app::card_shaders ALIAS oa-app-card-shaders)
target_include_directories(oa-app-card-shaders PUBLIC include)
target_compile_features(oa-app-card-shaders PUBLIC cxx_std_20)
target_link_libraries(oa-app-card-shaders PRIVATE oa-options)
if(OA_BUILD_PLATFORM)
  # The executor (card/executor.hpp): texture pages and render targets made,
  # updated and destroyed on SDL's renderer, and a frame run on it with
  # SDL_RenderGeometryRaw and the renderer's blend modes, one call for each
  # run of batches that share their state, and on SDL's GPU renderer the
  # blur through the embedded shader. Only the Full tier reaches it.
  add_library(oa-app-card-executor src/executor.cpp)
  add_library(oa::app::card_executor ALIAS oa-app-card-executor)
  target_link_libraries(oa-app-card-executor PUBLIC oa-app-card SDL3::SDL3
    PRIVATE oa-app-card-shaders oa-options)
  if(BUILD_TESTING)
    # Frames run on SDL's software renderer over a surface and read back
    # against a reference rasteriser on the processor: textured and
    # untextured quads and triangles under every blend mode, premultiplied
    # alpha among them, a scissor, vertex colours, a draw's own sampling
    # mode, a page's levels and a part of one updated, render targets at
    # supersampling factors 2 and 4 resolved with a downscale, a transparent
    # target resolved by premultiplied alpha, a target written again
    # between two resolves, edges between a target's pixels reduced to
    # their coverage, and the two-level reduction of a target that keeps
    # its half against the renderer's own stretch and blend; malformed
    # frames and frames naming a destroyed page or a target without its
    # half refused with nothing drawn; pages and targets beyond the
    # renderer's texture limit refused with an error naming both; batches
    # that share their state merged into one call; the renderer's state put
    # back; and the processor cost of 5,000 and 20,000 quads a frame,
    # printed.
    add_executable(oa-app-card-frame-test tests/card_frame_test.cpp)
    target_link_libraries(oa-app-card-frame-test PRIVATE oa-app-card-executor SDL3::SDL3
      oa-test-support oa-options)
    add_test(NAME app-card-frame COMMAND oa-app-card-frame-test)
    set_tests_properties(app-card-frame PROPERTIES TIMEOUT 180
      ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy")
  endif()
endif()
