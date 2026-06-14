// Hand-written Metal port of amnesia/slang/Outline/outline_geom.vert.slang
//
// Shared geometry vertex stage for cLuxEffectRenderer's object passes (hover
// outline silhouette/rim + enemy darkness rim). Forwards the view-space normal
// for the rim term. Pairs with outline_geom.frag / enemy_glow.frag.
//
// Bindings:
//   set0 buffer(0); [[id]] = VK binding within the set:
//     pass (OutlinePass UBO) 0
//   push constant (OutlinePC) -> [[buffer(1)]] (pass UBO occupies buffer0)
//
// Vertex stream layout (fixed-function streams, matches the translucent pass):
//   attribute 0 : float3 a_position (binding 0, stride 16, .xyz)
//   attribute 1 : float3 a_normal   (binding 1, stride 12)
#include <metal_stdlib>
using namespace metal;

struct OutlinePass {
    float4x4 viewProj;
    float4x4 view;
};

struct OutlinePC {
    float4x4 model;
    float4   color; // rgb = glow color, a = intensity
};

struct Set0 {
    constant OutlinePass* pass [[id(0)]];
};

struct VSIn {
    float3 a_position [[attribute(0)]];
    float3 a_normal   [[attribute(1)]];
};

struct VSOut {
    float4 posH [[position]];
    float3 v_viewNormal;
};

vertex VSOut vsMain(VSIn vin [[stage_in]],
                    device Set0& set0 [[buffer(0)]],
                    constant OutlinePC& pc [[buffer(1)]]) {
    VSOut o;
    float4 worldPos = pc.model * float4(vin.a_position, 1.0f);
    o.posH = set0.pass->viewProj * worldPos;
    // Ignore non-uniform scale (good enough for a rim term).
    float3 worldN = float3x3(pc.model[0].xyz, pc.model[1].xyz, pc.model[2].xyz) * vin.a_normal;
    o.v_viewNormal = float3x3(set0.pass->view[0].xyz, set0.pass->view[1].xyz, set0.pass->view[2].xyz) * worldN;
    return o;
}
