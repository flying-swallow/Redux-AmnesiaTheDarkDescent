/// Copyright © 2009-2020 Frictional Games
/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Slang port of amnesia/glsl/inventory_post.frag.
//
// Inventory background post-effect. Remaps RGB through a desaturate-
// towards-green matrix, scales intensity down, then gamma-shapes the
// result to deepen the contrast. Originally ported from
// HPL2/resource/dds_inventory_posteffect.frag.fsl in the reference fork.
//
// Bindings match the GLSL twin:
//   set 0 binding 0 : SamplerState        inputSampler
//   set 0 binding 1 : Texture2D<float4>   sourceInput

[vk::binding(0, 0)]
SamplerState inputSampler;

[vk::binding(1, 0)]
Texture2D<float4> sourceInput;

struct PSIn
{
    [vk::location(0)] float2 v_uv;
};

struct PSOut
{
    [vk::location(0)] float4 out_color;
};

[shader("fragment")]
PSOut psMain(PSIn pin)
{
    float3 color = sourceInput.Sample(inputSampler, pin.v_uv).rgb;

    const float3 colorToR  = float3(0.39f,  0.769f, 0.189f);
    const float3 colorToG  = float3(0.349f, 0.686f, 0.168f);
    const float3 colorToB  = float3(0.272f, 0.534f, 0.131f);
    const float  intensity = 0.35f;

    float3 mixed = float3(dot(color, colorToR),
                          dot(color, colorToG),
                          dot(color, colorToB)) * intensity;

    PSOut o;
    o.out_color = float4(pow(mixed, float3(1.5f)), 1.0f);
    return o;
}
