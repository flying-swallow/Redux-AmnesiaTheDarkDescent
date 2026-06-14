// Hand-written Metal port of amnesia/slang/InventoryPost/inventory_post.frag.slang
//
// Inventory background post-effect. Remaps RGB through a desaturate-towards-green
// matrix, scales intensity down, then gamma-shapes the result to deepen contrast.
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
    float3 color = set0.sourceInput.sample(set0.inputSampler, pin.v_uv).rgb;

    const float3 colorToR  = float3(0.39f,  0.769f, 0.189f);
    const float3 colorToG  = float3(0.349f, 0.686f, 0.168f);
    const float3 colorToB  = float3(0.272f, 0.534f, 0.131f);
    const float  intensity = 0.35f;

    float3 mixed = float3(dot(color, colorToR),
                          dot(color, colorToG),
                          dot(color, colorToB)) * intensity;

    return float4(pow(mixed, float3(1.5f)), 1.0f);
}
