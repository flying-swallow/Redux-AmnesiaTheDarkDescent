// Hand-written Metal port of amnesia/slang/Outline/outline_alpha.vert.slang
//
// Alpha-tested variant of the outline geometry vertex stage: forwards the
// texcoord so the hover outline follows the visible (cutout) silhouette.
// Pairs with outline_alpha.frag.
//
// Bindings:
//   set0 buffer(0); [[id]] = VK binding within the set:
//     pass (OutlinePass UBO) 0
//   push constant (OutlineAlphaPC) -> [[buffer(1)]] (pass UBO occupies buffer0)
//
// Vertex stream layout:
//   attribute 0 : float3 a_position (binding 0)
//   attribute 1 : float2 a_uv       (binding 1)
#include <metal_stdlib>
using namespace metal;

struct OutlinePass {
    float4x4 viewProj;
    float4x4 view;
};

struct OutlineAlphaPC {
    float4x4 model;
    float4   color;  // rgb = glow color, a = intensity
    float4   params; // x = alpha texture is single-channel (sample .r vs .a)
};

struct Set0 {
    constant OutlinePass* pass [[id(0)]];
};

struct VSIn {
    float3 a_position [[attribute(0)]];
    float2 a_uv       [[attribute(1)]];
};

struct VSOut {
    float4 posH [[position]];
    float2 v_uv;
};

vertex VSOut vsMain(VSIn vin [[stage_in]],
                    device Set0& set0 [[buffer(0)]],
                    constant OutlineAlphaPC& pc [[buffer(1)]]) {
    VSOut o;
    float4 worldPos = pc.model * float4(vin.a_position, 1.0f);
    o.posH = set0.pass->viewProj * worldPos;
    o.v_uv = vin.a_uv;
    return o;
}
