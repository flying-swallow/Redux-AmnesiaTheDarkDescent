/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Shared fullscreen-triangle vertex shader for every post-effect frag pass
// (Bloom, ColorConvTex, ImageTrail, RadialBlur, the blit, and the surfel-GI
// composite). No vertex input — UV and clip-space position are derived from
// SV_VertexID; the host issues vkCmdDraw(3, 1, 0, 0).
//
// Vulkan-native orientation: v_uv = (0,0) is at the top-left of the
// framebuffer, matching how textures are stored (row 0 at the top). This
// matters because post-effects ping-pong through a pogo buffer that the
// surfel-GI pass writes via SV_Position (also top-down): keeping the UV
// origin top-left means a downstream sampler reads pogo at the same row
// the upstream fragment wrote.

struct VSOut
{
    [vk::location(0)] float2 v_uv;
    float4 posH : SV_Position;
};

[shader("vertex")]
VSOut vsMain(uint vertexId : SV_VertexID)
{
    VSOut o;
    o.v_uv = float2(float((vertexId << 1) & 2), float(vertexId & 2));
    o.posH = float4(o.v_uv * 2.0f - 1.0f, 0.0f, 1.0f);
    return o;
}
