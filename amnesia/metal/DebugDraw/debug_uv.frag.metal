// Hand-written Metal port of amnesia/slang/DebugDraw/debug_uv.frag.slang
//
// Editor / debug overlay batcher — textured quad / billboard fragment stage:
// diffuse sample modulated by the per-vertex tint. Alpha-blended by the
// pipeline. Pairs with debug_uv.vert.metal.
//
// Bindings:
//   set0 buffer(0); [[id]] = VK binding within the set:
//     pass (DebugPass UBO) 0  (unreferenced by this stage)
//     diffuseSampler       1
//     diffuseMap           2
#include <metal_stdlib>
using namespace metal;

struct DebugPass {
    float4x4 viewProjMat;
    float4x4 viewProj2DMat;
};

struct Set0 {
    constant DebugPass*            pass           [[id(0)]];
    sampler                        diffuseSampler [[id(1)]];
    texture2d<float, access::sample> diffuseMap   [[id(2)]];
};

struct PSIn {
    float4 posH [[position]];
    float2 v_uv;
    float4 v_color;
};

fragment float4 psMain(PSIn pin [[stage_in]],
                       device Set0& set0 [[buffer(0)]]) {
    return pin.v_color * set0.diffuseMap.sample(set0.diffuseSampler, pin.v_uv);
}
