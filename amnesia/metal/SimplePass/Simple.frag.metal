// Hand-written Metal port of amnesia/slang/SimplePass/Simple.frag.slang
//
// Simple unlit textured forward renderer fragment stage: diffuse sample
// modulated by the per-vertex tint and per-draw color, with optional cutout
// alpha test (alphaReject > 0).
//
// Bindings:
//   set0 buffer(0); [[id]] = VK binding within the set:
//     pass (SimplePass UBO) 0
//     diffuseSampler        1
//     diffuseMap            2
#include <metal_stdlib>
using namespace metal;

struct SimplePass {
    float4x4 mvp;
    float4x4 uvMtx;
    float4   color;
    float    alphaReject;
};

struct Set0 {
    constant SimplePass*           pass           [[id(0)]];
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
    float4 c = pin.v_color * set0.pass->color *
               set0.diffuseMap.sample(set0.diffuseSampler, pin.v_uv);
    if (c.a < set0.pass->alphaReject) {
        discard_fragment();
    }
    return c;
}
