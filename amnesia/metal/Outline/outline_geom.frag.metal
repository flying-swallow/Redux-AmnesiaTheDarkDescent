// Hand-written Metal port of amnesia/slang/Outline/outline_geom.frag.slang
//
// Flat glow fragment for cLuxEffectRenderer (silhouette mark / rim color /
// pickup flash). Emits the flat glow color; alpha 0 so the RGB-only additive
// blend leaves scene alpha intact. Pairs with outline_geom.vert.
//
// Bindings:
//   set0 buffer(0): empty in this stage (the vertex `pass` UBO is unused here
//     and stripped by slangc).
//   push constant (OutlinePC) -> [[buffer(1)]] (matches the slangc per-stage
//     slot assigned for outline_geom; see LuxEffectRenderer kGeom).
#include <metal_stdlib>
using namespace metal;

struct OutlinePC {
    float4x4 model;
    float4   color; // rgb = glow color, a = intensity
};

fragment float4 psMain(constant OutlinePC& pc [[buffer(1)]]) {
    return float4(pc.color.rgb * pc.color.a, 0.0f);
}
