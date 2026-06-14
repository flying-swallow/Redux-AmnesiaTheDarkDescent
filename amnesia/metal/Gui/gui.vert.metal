// Hand-written Metal port of amnesia/slang/Gui/gui.vert.slang
//
// GUI quad vertex stage (cGuiSet::Render). Standalone — no bindless set; the
// host binds one frame-scratch UBO ("pass") per draw.
//
// Bindings:
//   set0 buffer(0); [[id]] = VK binding within the set:
//     pass (GuiPass UBO) 0
//
// Vertex stream layout (host: PositionTexColor in GuiSet.cpp, stride 36):
//   attribute 0 : float3 a_position
//   attribute 1 : float2 a_texcoord
//   attribute 2 : float4 a_color
#include <metal_stdlib>
using namespace metal;

struct GuiPass {
    float4x4 mvp;
    float4   clipPlane[4];
    int      textureConfig;
};

struct Set0 {
    constant GuiPass* pass [[id(0)]];
};

struct VSIn {
    float3 a_position [[attribute(0)]];
    float2 a_texcoord [[attribute(1)]];
    float4 a_color    [[attribute(2)]];
};

struct VSOut {
    float4 posH [[position]];
    float2 v_texcoord;
    float4 v_color;
    // Screen-space (pre-MVP) position — clip planes are computed in screen
    // space (cGuiSet::Render), so the frag clip test uses this not posH.
    float4 v_position;
};

vertex VSOut vsMain(VSIn vin [[stage_in]],
                    device Set0& set0 [[buffer(0)]]) {
    VSOut o;
    o.v_color    = vin.a_color;
    o.v_texcoord = vin.a_texcoord;
    o.v_position = float4(vin.a_position, 1.0f);
    o.posH       = set0.pass->mvp * float4(vin.a_position, 1.0f);
    return o;
}
