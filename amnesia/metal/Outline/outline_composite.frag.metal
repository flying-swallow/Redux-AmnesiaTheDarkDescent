// Hand-written Metal port of amnesia/slang/Outline/outline_composite.frag.slang
//
// Fullscreen composite for cLuxEffectRenderer's hover outline: samples the
// blurred glow scratch target and emits it for an RGB-only additive blend into
// the pogo read half. Alpha 0 keeps the scene alpha intact. Pairs with the
// shared posteffect_fullscreen.vert (provides float4 [[position]] + v_uv).
//
// Bindings:
//   set0 buffer(0); [[id]] = VK binding within the set:
//     inputSampler 0   blurInput 1
//   (no push constant)
#include <metal_stdlib>
using namespace metal;

struct Set0 {
    sampler                          inputSampler [[id(0)]];
    texture2d<float, access::sample> blurInput    [[id(1)]];
};

struct PSIn {
    float4 posH [[position]];
    float2 v_uv;
};

fragment float4 psMain(PSIn pin [[stage_in]],
                       device Set0& set0 [[buffer(0)]]) {
    float3 c = set0.blurInput.sample(set0.inputSampler, pin.v_uv).rgb;
    return float4(c, 0.0f);
}
