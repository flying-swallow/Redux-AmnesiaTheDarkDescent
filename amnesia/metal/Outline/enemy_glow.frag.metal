// Hand-written Metal port of amnesia/slang/Outline/enemy_glow.frag.slang
//
// Enemy darkness-glow fragment for cLuxEffectRenderer — a soft view-space rim
// so an approaching enemy reads against the dark. The silhouette edge
// (|N.z| -> 0) lights up; the facing interior fades out. Additive, RGB-only
// (alpha 0). Pairs with outline_geom.vert (shared OutlinePass + OutlinePC,
// view-space normal forwarded as v_viewNormal).
//
// Bindings:
//   set0 buffer(0): empty in this stage (the vertex `pass` UBO is unused here
//     and stripped by slangc).
//   push constant (OutlinePC) -> [[buffer(1)]] (matches outline_geom.frag).
#include <metal_stdlib>
using namespace metal;

struct OutlinePC {
    float4x4 model;
    float4   color; // rgb = glow color, a = intensity
};

struct PSIn {
    float4 posH [[position]];
    float3 v_viewNormal;
};

fragment float4 psMain(PSIn pin [[stage_in]],
                       constant OutlinePC& pc [[buffer(1)]]) {
    float3 N = normalize(pin.v_viewNormal);
    float rim = pow(saturate(1.0f - abs(N.z)), 2.5f);
    return float4(pc.color.rgb * (rim * pc.color.a), 0.0f);
}
