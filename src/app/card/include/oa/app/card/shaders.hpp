// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The fragment shaders the card's effects run on SDL's GPU renderer
// (src/app/card/shaders), compiled when the game is built, each in the
// three forms SDL's GPU devices take. A build made without the shader
// compiler holds none, and the effects stay off.
#pragma once

#include <cstdint>
#include <span>

namespace oa::app::card {

/// One fragment shader in the three forms SDL's GPU devices take, each
/// empty in a build made without the shader compiler.
struct ShaderCode {
    std::span<const uint8_t> spirv; ///< SPIR-V, for Vulkan; entry point "main"
    std::span<const uint8_t> dxil;  ///< DXIL, for Direct3D 12; entry point "main"
    std::span<const uint8_t> msl;   ///< Metal source text; entry point "main0"
    uint32_t sampler_count{};        ///< the textures it reads, each with its sampler
    uint32_t uniform_buffer_count{}; ///< the fragment uniform slots it reads
};

/// The glow's blur (blur.frag.hlsl): each pixel the weighted mean of nine
/// texels in a line, times the vertex colour. It reads the drawn texture
/// with a LINEAR sampler, and in uniform slot 0 two pairs of floats: one
/// texel across or down in texture coordinates, then two unused.
ShaderCode blur_shader() noexcept;

/// The edge smoothing (fxaa.frag.hlsl), fast approximate anti-aliasing: a
/// pixel on an edge between bright and dark read a fraction of a texel
/// across the edge, by how near it lies to the edge's step, times the
/// vertex colour; a pixel off any edge kept. It reads the drawn texture,
/// drawn texel for texel, with a LINEAR sampler, and in uniform slot 0 two
/// pairs of floats: one texel across and down in texture coordinates, then
/// two unused.
ShaderCode fxaa_shader() noexcept;

} // namespace oa::app::card
