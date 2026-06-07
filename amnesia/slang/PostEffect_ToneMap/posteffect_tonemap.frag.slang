/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Output-encode pass. Runs after every HDR-domain effect in the chain: takes
// the linear-HDR pogo, applies `exposure`, then sRGB-encodes into the pogo,
// clamped to a normalized [0,1] — display-space effects that run after it
// (the image trail's persistent fp16 EMA accumulator) rely on the output
// being bounded and finite. The tail blit copies this straight to the
// UNORM swapchain, which performs NO hardware OETF — so the encode MUST happen
// here. This brings the scene in line with the GUI, which is authored to write
// display-encoded values directly to the same UNORM swapchain.
//
// ACES was previously removed because it crushed low-mids of a scene whose
// diffuse was capped at ≤1; with PBR lights pushing values >1 the filmic
// shoulder is wanted again. Tune `exposure` if mids read dim.
//
// Bindings (set 0):
//   binding 0 : SamplerState inputSampler
//   binding 1 : Texture2D<float4> sourceInput
// Push constant: exposure (scene-linear multiply); shadowLift unused (layout).

import Utils.Color.ColorHelpers;   // piecewise linearToSRGB()

[vk::binding(0, 0)]
SamplerState inputSampler;

[vk::binding(1, 0)]
Texture2D<float4> sourceInput;

// Stephen Hill's "ACES fitted" RRT+ODT(sRGB): treat the linear Rec.709 scene as
// ACEScg working space, apply the tone curve, map back to linear sRGB. Output is
// linear [0,1]; the sRGB OETF is applied separately (linearToSRGB) for the UNORM
// swapchain. Matrices are HLSL row-major, used as mul(matrix, vector).
static const float3x3 ACESInputMat = {
    { 0.59719f, 0.35458f, 0.04823f },
    { 0.07600f, 0.90834f, 0.01566f },
    { 0.02840f, 0.13383f, 0.83777f },
};
static const float3x3 ACESOutputMat = {
    {  1.60475f, -0.53108f, -0.07367f },
    { -0.10208f,  1.10813f, -0.00605f },
    { -0.00327f, -0.07276f,  1.07602f },
};

float3 RRTAndODTFit(float3 v)
{
    float3 a = v * (v + 0.0245786f) - 0.000090537f;
    float3 b = v * (0.983729f * v + 0.4329510f) + 0.238081f;
    return a / b;
}

float3 ACESFitted(float3 color)
{
    color = mul(ACESInputMat, color);
    color = RRTAndODTFit(color);
    color = mul(ACESOutputMat, color);
    return saturate(color);        // linear sRGB [0,1]
}

struct ToneMapPC
{
    float exposure;
    float shadowLift;
    float _pad0;
    float _pad1;
};

[vk::push_constant]
ToneMapPC pc;

struct PSIn
{
    [vk::location(0)] float2 v_uv;
};

[shader("fragment")]
float4 psMain(PSIn pin) : SV_Target
{
    float3 hdr = sourceInput.Sample(inputSampler, pin.v_uv).rgb * pc.exposure;
    return float4(saturate(ACESFitted(linearToSRGB(hdr))), 1.0f);   // display-encoded sRGB [0,1]
}
