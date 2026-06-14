// Hand-written Metal port of amnesia/slang/PostEffect_Bloom/posteffect_bloom_add.frag.slang
//
// Composites the blurred bright-pass back into the source, weighting the blur by
// a saturate()'d luminance-derived intensity (HDR-safe vs the legacy cubic).
//
// Bindings:
//   set0 buffer(0); [[id]] = VK binding within the set:
//     inputSampler  0
//     sourceInput   1
//     blurInput     2
//   push constant (BloomAddPC) -> [[buffer(1)]]
#include <metal_stdlib>
using namespace metal;

struct Set0 {
    sampler                          inputSampler [[id(0)]];
    texture2d<float, access::sample> sourceInput  [[id(1)]];
    texture2d<float, access::sample> blurInput    [[id(2)]];
};

struct BloomAddPC {
    float3 rgbToIntensity;
    float  _pad;
};

struct PSIn {
    float4 posH [[position]];
    float2 v_uv;
};

fragment float4 psMain(PSIn pin [[stage_in]],
                       device Set0& set0 [[buffer(0)]],
                       constant BloomAddPC& pc [[buffer(1)]]) {
    float4 color     = set0.sourceInput.sample(set0.inputSampler, pin.v_uv);
    float4 blurColor = set0.blurInput.sample(set0.inputSampler, pin.v_uv);
    float intensity = saturate(dot(blurColor.xyz, pc.rgbToIntensity));
    return float4(color.xyz + blurColor.xyz * intensity, color.a);
}
