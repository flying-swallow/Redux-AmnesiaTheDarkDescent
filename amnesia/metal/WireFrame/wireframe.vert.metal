// Hand-written Metal port of amnesia/slang/WireFrame/wireframe.vert.slang
//
// Wireframe debug renderer vertex stage (cRendererWireFrame). Standalone — no
// bindless set; one frame-scratch UBO ("pass") per draw.
//
// Bindings:
//   set0 buffer(0); [[id]] = VK binding within the set:
//     pass (WireFramePass UBO) 0
//
// Vertex stream layout (host: RendererWireFrame.cpp, engine position stream
// is float4 stride 16; only .xyz is read):
//   attribute 0 : float3 a_position
#include <metal_stdlib>
using namespace metal;

struct WireFramePass {
    float4x4 mvp;
    float4   color;
};

struct Set0 {
    constant WireFramePass* pass [[id(0)]];
};

struct VSIn {
    float3 a_position [[attribute(0)]];
};

struct VSOut {
    float4 posH [[position]];
};

vertex VSOut vsMain(VSIn vin [[stage_in]],
                    device Set0& set0 [[buffer(0)]]) {
    VSOut o;
    o.posH = set0.pass->mvp * float4(vin.a_position, 1.0f);
    return o;
}
