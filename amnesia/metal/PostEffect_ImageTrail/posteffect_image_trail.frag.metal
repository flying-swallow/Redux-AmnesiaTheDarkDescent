// Hand-written Metal port of amnesia/slang/PostEffect_ImageTrail/posteffect_image_trail.frag.slang
//
// Outputs source color with an alpha derived from the per-frame fade, so the
// host can blend (SRC_ALPHA, ONE_MINUS_SRC_ALPHA) into a persistent accumulator.
//
// Bindings:
//   set0 buffer(0); [[id]] = VK binding within the set:
//     inputSampler  0
//     sourceInput   1
//   push constant (ImageTrailPC) -> [[buffer(1)]]
#include <metal_stdlib>
using namespace metal;

struct Set0 {
    sampler                          inputSampler [[id(0)]];
    texture2d<float, access::sample> sourceInput  [[id(1)]];
};

struct ImageTrailPC {
    float alpha;
};

struct PSIn {
    float4 posH [[position]];
    float2 v_uv;
};

fragment float4 psMain(PSIn pin [[stage_in]],
                       device Set0& set0 [[buffer(0)]],
                       constant ImageTrailPC& pc [[buffer(1)]]) {
    float3 color = set0.sourceInput.sample(set0.inputSampler, pin.v_uv).rgb;
    return float4(color, pc.alpha);
}
