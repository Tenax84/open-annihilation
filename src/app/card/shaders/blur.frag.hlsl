// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// One pass of the glow's blur, run as the fragment shader of SDL's GPU
// renderer: each pixel the weighted mean of nine texels in a line across
// or down the picture, times the vertex colour. Two passes, one across and
// one down, blur the picture both ways. The texture and its sampler are the
// ones the draw binds; the sampler reads LINEAR, so that each of the five
// reads takes two texels at once.
Texture2D picture : register(t0, space2);
SamplerState picture_sampler : register(s0, space2);

// The pass, in fragment uniform slot 0.
cbuffer Pass : register(b0, space3) {
    // One texel across (1 / width, 0) or down (0, 1 / height), in texture
    // coordinates.
    float2 texel_step;
    float2 unused;
};

struct Input {
    float4 colour : COLOR0;
    float2 uv : TEXCOORD0;
};

float4 main(Input input) : SV_Target {
    float2 near_offset = texel_step * 1.3846153846;
    float2 far_offset = texel_step * 3.2307692308;
    float4 sum = picture.Sample(picture_sampler, input.uv) * 0.2270270270;
    sum += picture.Sample(picture_sampler, input.uv + near_offset) * 0.3162162162;
    sum += picture.Sample(picture_sampler, input.uv - near_offset) * 0.3162162162;
    sum += picture.Sample(picture_sampler, input.uv + far_offset) * 0.0702702703;
    sum += picture.Sample(picture_sampler, input.uv - far_offset) * 0.0702702703;
    return sum * input.colour;
}
