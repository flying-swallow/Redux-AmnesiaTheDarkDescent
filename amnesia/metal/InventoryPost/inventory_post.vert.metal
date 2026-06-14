// Hand-written Metal port of amnesia/slang/InventoryPost/inventory_post.vert.slang
//
// Fullscreen-triangle vertex stage. No vertex input — UV and clip-space position
// derived from the vertex id; host issues a 3-vertex draw. Y is flipped in clip
// space (Vulkan-down) so a (0,0)->(1,1) UV sweep covers the screen top-to-bottom.
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
    o.posH = float4(o.v_uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f),
                    0.0f, 1.0f);
    return o;
}
