// Hand-written Metal port of amnesia/slang/PostEffect/posteffect_blit.frag.slang
//
// Trivial 1-tap blit. Tail pass that copies the last pogo attachment into the
// swapchain image after the post-effect chain.
//
// Bindings:
//   set0 buffer(0); [[id]] = VK binding within the set:
//     inputSampler  0
//     sourceInput   1
#include <metal_stdlib>
using namespace metal;

struct Set0 {
    sampler                          inputSampler [[id(0)]];
    texture2d<float, access::sample> sourceInput  [[id(1)]];
};

struct PSIn {
    float4 posH [[position]];
    float2 v_uv;
};

fragment float4 psMain(PSIn pin [[stage_in]],
                       device Set0& set0 [[buffer(0)]]) {
    return float4(set0.sourceInput.sample(set0.inputSampler, pin.v_uv).rgb, 1.0f);
}
