/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Slang port of HPL2's bloom_add_posteffect.frag.fsl. Composites the blurred
// bright-pass back into the source image, weighting the blur by an intensity
// derived from luminance (rgbToIntensity dot product).
//
// Bindings (set 0):
//   binding 0 : SamplerState inputSampler
//   binding 1 : Texture2D<float4> sourceInput
//   binding 2 : Texture2D<float4> blurInput

[vk::binding(0, 0)]
SamplerState inputSampler;

[vk::binding(1, 0)]
Texture2D<float4> sourceInput;

[vk::binding(2, 0)]
Texture2D<float4> blurInput;

struct BloomAddPC
{
    float3 rgbToIntensity;
    float  _pad;
};

[vk::push_constant]
BloomAddPC pc;

struct PSIn
{
    [vk::location(0)] float2 v_uv;
};

[shader("fragment")]
float4 psMain(PSIn pin) : SV_Target
{
    float4 color     = sourceInput.Sample(inputSampler, pin.v_uv);
    float4 blurColor = blurInput.Sample(inputSampler, pin.v_uv);
    // Bright-pass weight, clamped to [0,1]. The original HPL2 formula was
    // blurColor^2 * dot(blurColor, rgbToIntensity) — bounded only because that
    // renderer bloomed in LDR ([0,1], so blur^3 <= blur). Here bloom runs in the
    // HDR linear pogo *before* the tonemap, so blurColor can be >> 1 and the
    // cubic term explodes (a bright white particle adds ~L^3 per channel, blowing
    // out the frame and bleeding into the darks). saturate() bounds the added
    // bloom to at most the blurred bright-pass itself, which ACES then rolls off.
    float intensity = saturate(dot(blurColor.xyz, pc.rgbToIntensity));
    return float4(color.xyz + blurColor.xyz * intensity, color.a);
}
