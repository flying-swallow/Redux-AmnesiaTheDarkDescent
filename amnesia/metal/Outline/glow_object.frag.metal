// Hand-written Metal port of amnesia/slang/Outline/glow_object.frag.slang
//
// Pickup-flash + enemy-darkness-glow fragment. The glow FILLS the object's
// visible surface (front faces, cull BACK), using the object's own diffuse
// colour tinted and added into the linear-HDR scene (alpha 0, RGB additive).
// Flash sets the alpha-test flag to discard cutout texels; enemy leaves it off.
// Pairs with glow_object.vert.
//
// Bindings:
//   set0 buffer(0); [[id]] = VK binding within the set:
//     diffuseSampler 1   diffuseMap 2
//   push constant (OutlineAlphaPC) -> [[buffer(1)]]
//     (set0 argument buffer occupies buffer0; see flag in port notes — the
//      host's slangc-derived pcMtlFrag=0 assumes the loose-binding MSL.)
#include <metal_stdlib>
using namespace metal;

struct OutlineAlphaPC {
    float4x4 model;
    float4   color;  // rgb = linear tint, a = intensity (mfAlpha * pulse)
    float4   params; // x = alpha-test flag, y = diffuse single-channel flag
};

struct Set0 {
    sampler                          diffuseSampler [[id(1)]];
    texture2d<float, access::sample> diffuseMap     [[id(2)]];
};

struct PSIn {
    float4 posH [[position]];
    float2 v_uv;
};

fragment float4 psMain(PSIn pin [[stage_in]],
                       device Set0& set0 [[buffer(0)]],
                       constant OutlineAlphaPC& pc [[buffer(1)]]) {
    float4 tex = set0.diffuseMap.sample(set0.diffuseSampler, pin.v_uv);
    if (pc.params.x > 0.5f) {
        float a = (pc.params.y > 0.5f) ? tex.r : tex.a;
        if (a < 0.5f)
            discard_fragment();
    }
    return float4(tex.rgb * pc.color.rgb * pc.color.a, 0.0f);
}
