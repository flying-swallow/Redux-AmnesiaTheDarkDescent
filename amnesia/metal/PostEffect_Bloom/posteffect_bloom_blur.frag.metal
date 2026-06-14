// Hand-written Metal port of amnesia/slang/PostEffect_Bloom/posteffect_bloom_blur.frag.slang
//
// 5-tap separable blur. Host runs it twice per iteration with blurDir = (size,0)
// then (0,size) for horizontal + vertical passes.
//
// Bindings:
//   set0 buffer(0); [[id]] = VK binding within the set:
//     inputSampler  0
//     sourceInput   1
//   push constant (BloomBlurPC) -> [[buffer(1)]]
#include <metal_stdlib>
using namespace metal;

struct Set0 {
    sampler                          inputSampler [[id(0)]];
    texture2d<float, access::sample> sourceInput  [[id(1)]];
};

struct BloomBlurPC {
    float2 blurDir;
    float2 _pad;
};

struct PSIn {
    float4 posH [[position]];
    float2 v_uv;
};

fragment float4 psMain(PSIn pin [[stage_in]],
                       device Set0& set0 [[buffer(0)]],
                       constant BloomBlurPC& pc [[buffer(1)]]) {
    uint w = set0.sourceInput.get_width();
    uint h = set0.sourceInput.get_height();
    float2 texel = 1.0f / float2(float(w), float(h));

    const float vMul[5]    = { 0.25f, 0.30f, 0.50f, 0.30f, 0.25f };
    const float fOffset[5] = { -2.5f, -0.75f, 0.0f, 0.75f, 2.5f  };
    const float mulSum = 1.6f; // sum of vMul[]

    float3 amount = float3(0.0f);
    for (int i = 0; i < 5; ++i) {
        float2 offset = (float2(fOffset[i], fOffset[i]) * texel) * pc.blurDir;
        amount += set0.sourceInput.sample(set0.inputSampler, pin.v_uv + offset).rgb
                  * vMul[i];
    }
    amount /= mulSum;
    return float4(amount, 1.0f);
}
