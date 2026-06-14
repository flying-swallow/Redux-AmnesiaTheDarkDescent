// Hand-written Metal port of amnesia/slang/Outline/glow_object.vert.slang
//
// Pickup-flash + enemy-darkness-glow vertex stage for cLuxEffectRenderer. The
// glow fills the object's visible surface (no rim), so this stage only forwards
// the texcoord. A tiny clip-space depth bias pulls the glow toward the camera
// so front faces win the LEQUAL test. Pairs with glow_object.frag.
//
// Bindings:
//   set0 buffer(0); [[id]] = VK binding within the set:
//     pass (OutlinePass UBO) 0
//   push constant (OutlineAlphaPC) -> [[buffer(1)]] (pass UBO occupies buffer0)
//
// Vertex stream layout:
//   attribute 0 : float3 a_position (binding 0, stride 16, .xyz)
//   attribute 1 : float2 a_uv       (binding 1, Texture0 stream, .xy)
#include <metal_stdlib>
using namespace metal;

struct OutlinePass {
    float4x4 viewProj;
    float4x4 view;
};

struct OutlineAlphaPC {
    float4x4 model;
    float4   color;  // rgb = linear tint, a = intensity (mfAlpha * pulse)
    float4   params; // x = alpha-test flag, y = diffuse single-channel flag
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
    o.posH.z -= 0.0001f; // depth bias toward camera (matches dds_flash/enemy)
    o.v_uv = vin.a_uv;
    return o;
}
