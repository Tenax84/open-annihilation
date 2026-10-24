// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Fast approximate anti-aliasing (FXAA), run as the fragment shader of
// SDL's GPU renderer over a picture drawn texel for texel onto its
// destination. A pixel whose brightness differs little from its four
// neighbours' is kept as it is. On an edge, the shader finds which way the
// edge runs, follows it both ways to its ends, and reads the picture a
// fraction of a texel across the edge, more the nearer the pixel lies to
// the end where the edge steps; a pixel that stands out from all eight
// neighbours, as a lone pixel on a thin line does, is softened towards
// them too. The colour is times the vertex colour, the alpha the pixel's
// own. The texture and its sampler are the ones the draw binds; the
// sampler reads LINEAR, so that a read between texels mixes them.
Texture2D picture : register(t0, space2);
SamplerState picture_sampler : register(s0, space2);

// The pass, in fragment uniform slot 0.
cbuffer Pass : register(b0, space3) {
    // One texel across and down, (1 / width, 1 / height), in texture
    // coordinates.
    float2 texel;
    float2 unused;
};

struct Input {
    float4 colour : COLOR0;
    float2 uv : TEXCOORD0;
};

// The least share of the brightest of a pixel and its four neighbours
// their brightness must span for the pixel to lie on an edge.
static const float edge_share = 0.166;
// The least span of brightness that is an edge, however dark the pixels.
static const float least_edge = 0.0833;
// How far towards its neighbours a pixel that stands out from them is
// softened, 0 to 1.
static const float softening = 0.5;
// The texels each step of the search along an edge goes, one way.
static const int search_steps = 10;
static const float search_step_texels[search_steps] = {
    1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 4.0
};

float brightness(float3 colour) {
    return dot(colour, float3(0.299, 0.587, 0.114));
}

float4 read_at(float2 uv) {
    return picture.SampleLevel(picture_sampler, uv, 0.0);
}

float brightness_at(float2 uv) {
    return brightness(read_at(uv).rgb);
}

float4 main(Input input) : SV_Target {
    const float2 uv = input.uv;
    const float4 centre = read_at(uv);
    const float middle = brightness(centre.rgb);
    const float above = brightness_at(uv + float2(0.0, -texel.y));
    const float below = brightness_at(uv + float2(0.0, texel.y));
    const float left = brightness_at(uv + float2(-texel.x, 0.0));
    const float right = brightness_at(uv + float2(texel.x, 0.0));
    const float brightest = max(middle, max(max(above, below), max(left, right)));
    const float darkest = min(middle, min(min(above, below), min(left, right)));
    const float span = brightest - darkest;
    if (span < max(least_edge, brightest * edge_share))
        return centre * input.colour;

    const float above_left = brightness_at(uv + float2(-texel.x, -texel.y));
    const float above_right = brightness_at(uv + float2(texel.x, -texel.y));
    const float below_left = brightness_at(uv + float2(-texel.x, texel.y));
    const float below_right = brightness_at(uv + float2(texel.x, texel.y));
    const float above_below = above + below;
    const float left_right = left + right;
    const float left_corners = above_left + below_left;
    const float right_corners = above_right + below_right;
    const float top_corners = above_left + above_right;
    const float bottom_corners = below_left + below_right;

    // An edge across changes the brightness most from row to row.
    const float change_down = abs(left_corners - 2.0 * left) +
                              2.0 * abs(above_below - 2.0 * middle) +
                              abs(right_corners - 2.0 * right);
    const float change_across = abs(top_corners - 2.0 * above) +
                                2.0 * abs(left_right - 2.0 * middle) +
                                abs(bottom_corners - 2.0 * below);
    const bool runs_across = change_down >= change_across;

    // The side of the edge the brightness changes more towards.
    const float before = runs_across ? above : left;
    const float after = runs_across ? below : right;
    const float change_before = before - middle;
    const float change_after = after - middle;
    const bool steeper_before = abs(change_before) >= abs(change_after);
    const float end_change = 0.25 * max(abs(change_before), abs(change_after));
    float side_texels = runs_across ? texel.y : texel.x;
    float edge_middle;
    if (steeper_before) {
        side_texels = -side_texels;
        edge_middle = 0.5 * (before + middle);
    } else {
        edge_middle = 0.5 * (after + middle);
    }

    // Along the edge both ways, half a texel towards that side, until the
    // brightness there leaves the edge's middle by a quarter of its change.
    float2 on_edge = uv;
    if (runs_across)
        on_edge.y += 0.5 * side_texels;
    else
        on_edge.x += 0.5 * side_texels;
    const float2 along = runs_across ? float2(texel.x, 0.0) : float2(0.0, texel.y);
    float2 back_end = on_edge;
    float2 forward_end = on_edge;
    float back_change = 0.0;
    float forward_change = 0.0;
    bool back_found = false;
    bool forward_found = false;
    [loop] for (int step = 0; step < search_steps; ++step) {
        if (!back_found) {
            back_end -= along * search_step_texels[step];
            back_change = brightness_at(back_end) - edge_middle;
            back_found = abs(back_change) >= end_change;
        }
        if (!forward_found) {
            forward_end += along * search_step_texels[step];
            forward_change = brightness_at(forward_end) - edge_middle;
            forward_found = abs(forward_change) >= end_change;
        }
        if (back_found && forward_found)
            break;
    }
    const float back_distance = runs_across ? uv.x - back_end.x : uv.y - back_end.y;
    const float forward_distance = runs_across ? forward_end.x - uv.x : forward_end.y - uv.y;
    const bool nearer_back = back_distance < forward_distance;
    const float nearest = min(back_distance, forward_distance);
    const float edge_length = back_distance + forward_distance;
    // Only the end where the edge steps towards the pixel's own side moves it.
    const bool middle_darker = middle < edge_middle;
    const bool steps_here = ((nearer_back ? back_change : forward_change) < 0.0) != middle_darker;
    float shift = steps_here ? 0.5 - nearest / edge_length : 0.0;

    // A pixel that stands out from its eight neighbours, softened.
    const float neighbours = (2.0 * (above_below + left_right) + left_corners + right_corners) /
                             12.0;
    const float standing_out = saturate(abs(neighbours - middle) / span);
    const float eased = (3.0 - 2.0 * standing_out) * standing_out * standing_out;
    shift = max(shift, eased * eased * softening);

    float2 read = uv;
    if (runs_across)
        read.y += shift * side_texels;
    else
        read.x += shift * side_texels;
    return float4(read_at(read).rgb, centre.a) * input.colour;
}
