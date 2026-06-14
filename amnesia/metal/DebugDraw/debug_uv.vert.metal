// Hand-written Metal port of amnesia/slang/DebugDraw/debug_uv.vert.slang
//
// Editor / debug overlay batcher — textured quads / billboards (entity icons).
// Pairs with debug_uv.frag.metal.
//
// Bindings:
//   set0 buffer(0); [[id]] = VK binding within the set:
//     pass (DebugPass UBO) 0
//
// Unified vertex stream layout (host: DebugDraw::DebugVertex, stride 36):
//   attribute 0 : float3 a_position (offset 0)
//   attribute 1 : float2 a_uv       (offset 12)
//   attribute 2 : float4 a_color    (offset 20)
#include <metal_stdlib>
using namespace metal;

struct DebugPass {
    float4x4 viewProjMat;
    float4x4 viewProj2DMat;
};

struct Set0 {
    constant DebugPass* pass [[id(0)]];
};

struct VSIn {
    float3 a_position [[attribute(0)]];
    float2 a_uv       [[attribute(1)]];
    float4 a_color    [[attribute(2)]];
};

struct VSOut {
    float4 posH [[position]];
    float2 v_uv;
    float4 v_color;
};

vertex VSOut vsMain(VSIn vin [[stage_in]],
                    device Set0& set0 [[buffer(0)]]) {
    VSOut o;
    o.v_uv    = vin.a_uv;
    o.v_color = vin.a_color;
    o.posH    = set0.pass->viewProjMat * float4(vin.a_position, 1.0f);
    return o;
}
