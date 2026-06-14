// Hand-written Metal port of amnesia/slang/SimplePass/Simple.vert.slang
//
// Simple unlit textured forward renderer vertex stage (cRendererSimple).
// Standalone — no bindless set; one frame-scratch UBO ("pass") per draw.
//
// Bindings:
//   set0 buffer(0); [[id]] = VK binding within the set:
//     pass (SimplePass UBO) 0
//
// Vertex stream layout (host: RendererSimple.cpp):
//   attribute 0 : float3 a_position (engine position stream float4 stride 16; .xyz)
//   attribute 1 : float4 a_color    (engine color stream stride 16)
//   attribute 2 : float2 a_uv       (engine texcoord stream float3 stride 12; .xy)
#include <metal_stdlib>
using namespace metal;

struct SimplePass {
    float4x4 mvp;
    float4x4 uvMtx;
    float4   color;
    float    alphaReject;
};

struct Set0 {
    constant SimplePass* pass [[id(0)]];
};

struct VSIn {
    float3 a_position [[attribute(0)]];
    float4 a_color    [[attribute(1)]];
    float2 a_uv       [[attribute(2)]];
};

struct VSOut {
    float4 posH [[position]];
    float2 v_uv;
    float4 v_color;
};

vertex VSOut vsMain(VSIn vin [[stage_in]],
                    device Set0& set0 [[buffer(0)]]) {
    VSOut o;
    o.v_uv    = (set0.pass->uvMtx * float4(vin.a_uv, 0.0f, 1.0f)).xy;
    o.v_color = vin.a_color;
    o.posH    = set0.pass->mvp * float4(vin.a_position, 1.0f);
    return o;
}
