// Hand-written Metal port of amnesia/slang/PostEffect/posteffect_fullscreen.vert.slang
//
// Shared fullscreen-triangle vertex stage for every post-effect frag pass
// (Bloom, ColorConvTex, ImageTrail, RadialBlur, the blit, and the surfel-GI
// composite). No vertex input — UV and clip-space position are derived from
// the vertex id; the host issues a 3-vertex draw. Vulkan-native orientation:
// v_uv = (0,0) is the top-left of the framebuffer.
//
// Bindings: none (vertex-id only).
#include <metal_stdlib>
using namespace metal;

struct VSOut {
    float4 posH [[position]];
    float2 v_uv;
};

vertex VSOut vsMain(uint vertexId [[vertex_id]]) {
    VSOut o;
    o.v_uv = float2(float((vertexId << 1) & 2), float(vertexId & 2));
    o.posH = float4(o.v_uv * 2.0f - 1.0f, 0.0f, 1.0f);
    return o;
}
