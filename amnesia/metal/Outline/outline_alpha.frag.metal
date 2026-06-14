// Hand-written Metal port of amnesia/slang/Outline/outline_alpha.frag.slang
//
// Alpha-tested outline fragment. Samples the material's Alpha (cutout) texture
// and discards below the 0.5 gate so the hover outline follows the visible
// silhouette of cutout geometry. Surviving fragments emit the flat glow color
// (RGB additive, alpha 0). Pairs with outline_alpha.vert.
//
// Bindings:
//   set0 buffer(0); [[id]] = VK binding within the set:
//     alphaSampler 1   alphaMap 2
//   push constant (OutlineAlphaPC) -> [[buffer(1)]]
//     (set0 argument buffer occupies buffer0; see flag in port notes — the
//      host's slangc-derived pcMtlFrag=0 assumes the loose-binding MSL, but the
//      argument-buffer port keeps the PC off the set0 slot.)
#include <metal_stdlib>
using namespace metal;

struct OutlineAlphaPC {
    float4x4 model;
    float4   color;  // rgb = glow color, a = intensity
    float4   params; // x = alpha texture is single-channel (sample .r vs .a)
};

struct Set0 {
    sampler                          alphaSampler [[id(1)]];
    texture2d<float, access::sample> alphaMap     [[id(2)]];
};

struct PSIn {
    float4 posH [[position]];
    float2 v_uv;
};

fragment float4 psMain(PSIn pin [[stage_in]],
                       device Set0& set0 [[buffer(0)]],
                       constant OutlineAlphaPC& pc [[buffer(1)]]) {
    float4 s = set0.alphaMap.sample(set0.alphaSampler, pin.v_uv);
    float a = (pc.params.x > 0.5f) ? s.r : s.a;
    if (a < 0.5f)
        discard_fragment();
    return float4(pc.color.rgb * pc.color.a, 0.0f);
}
