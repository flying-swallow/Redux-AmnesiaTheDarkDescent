/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Slang port of HPL2's color_conv_posteffect.frag.fsl. Per-channel 1D LUT
// color remap with an alpha-fade blend between the LUT result and the
// untouched source.
//
// Bindings (set 0):
//   binding 0 : SamplerState inputSampler
//   binding 1 : Texture2D<float4> sourceInput
//   binding 2 : Texture1D<float4> colorConv

[vk::binding(0, 0)]
SamplerState inputSampler;

[vk::binding(1, 0)]
Texture2D<float4> sourceInput;

[vk::binding(2, 0)]
Texture1D<float4> colorConv;

struct ColorConvPC
{
    float alphaFade;
};

[vk::push_constant]
ColorConvPC pc;

struct PSIn
{
    [vk::location(0)] float2 v_uv;
};

[shader("fragment")]
float4 psMain(PSIn pin) : SV_Target
{
    float3 diffuseColor = sourceInput.Sample(inputSampler, pin.v_uv).rgb;
    float3 outputColor  = float3(
        colorConv.Sample(inputSampler, diffuseColor.x).x,
        colorConv.Sample(inputSampler, diffuseColor.y).y,
        colorConv.Sample(inputSampler, diffuseColor.z).z);
    return float4(outputColor * pc.alphaFade
                  + diffuseColor * (1.0f - pc.alphaFade),
                  1.0f);
}
