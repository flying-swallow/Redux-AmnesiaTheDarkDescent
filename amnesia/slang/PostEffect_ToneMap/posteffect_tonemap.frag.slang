/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Exposure-only output pass (no filmic curve). Runs as a post-effect after
// bloom: applies `exposure` and clamps the linear-HDR pogo into [0,1]. Output
// stays LINEAR — the scRGB-linear swapchain + display apply the sRGB OETF, so no
// gamma encode here.
//
// The ACES filmic tonemap was removed deliberately: the base game has no
// tonemapper, and ACES crushed the low-mids of a scene whose diffuse is capped
// at ≤1, making everything read dim. With the curve gone, the linearize→sRGB-
// encode round-trip reproduces base-game brightness for fully-lit surfaces.
// If specular/box highlights clip harshly, reintroduce a soft shoulder that
// only affects values >1.0 (so sub-1.0 brightness stays base-game-identical).
//
// Bindings (set 0):
//   binding 0 : SamplerState inputSampler
//   binding 1 : Texture2D<float4> sourceInput
// Push constant: exposure (output multiply); shadowLift unused (kept for layout).

[vk::binding(0, 0)]
SamplerState inputSampler;

[vk::binding(1, 0)]
Texture2D<float4> sourceInput;

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
    return float4(saturate(hdr), 1.0f);
}
