// Hand-written Metal port of amnesia/slang/PostEffect_ColorConvTex/posteffect_color_conv.frag.slang
//
// Per-channel 1D LUT color remap with an alpha-fade blend between the LUT result
// and the untouched source.
//
// Bindings:
//   set0 buffer(0); [[id]] = VK binding within the set:
//     inputSampler  0
//     sourceInput   1   (Texture2D)
//     colorConv     2   (Texture1D — 1D LUT, sampled per channel)
//   push constant (ColorConvPC) -> [[buffer(1)]]
#include <metal_stdlib>
using namespace metal;

struct Set0 {
    sampler                          inputSampler [[id(0)]];
    texture2d<float, access::sample> sourceInput  [[id(1)]];
    texture1d<float, access::sample> colorConv    [[id(2)]];
};

struct ColorConvPC {
    float alphaFade;
};

struct PSIn {
    float4 posH [[position]];
    float2 v_uv;
};

fragment float4 psMain(PSIn pin [[stage_in]],
                       device Set0& set0 [[buffer(0)]],
                       constant ColorConvPC& pc [[buffer(1)]]) {
    float3 diffuseColor = set0.sourceInput.sample(set0.inputSampler, pin.v_uv).rgb;
    float3 outputColor  = float3(
        set0.colorConv.sample(set0.inputSampler, diffuseColor.x).x,
        set0.colorConv.sample(set0.inputSampler, diffuseColor.y).y,
        set0.colorConv.sample(set0.inputSampler, diffuseColor.z).z);
    return float4(outputColor * pc.alphaFade
                  + diffuseColor * (1.0f - pc.alphaFade),
                  1.0f);
}
