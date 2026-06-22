#pragma once

// Slang mirror of amnesia/glsl/bindless_triangle.glsl::FetchBindlessTriangle.
// Each call resolves the per-instance VkDeviceAddress for every vertex
// stream we care about, indexes the (optional) index buffer, then loads
// position / normal / tangent / uv for the three triangle vertices.
//
// Buffer-device-address access uses Slang's vk::buffer_reference attribute
// — same memory model as the GLSL `layout(buffer_reference, scalar) buffer`
// declarations on the GLSL side.

#include "HostDefinitions.h"

import SceneTypes;
import bindless;

// BDA accesses use direct primitive pointer casts (`float4*`, `float3*`,
// `uint*`). The wrapper-struct-with-runtime-array pattern from earlier
// Slang releases silently miscompiles in slangc 2026+: `((Wrapper*)addr).v[i]`
// collapses to `Ptr<PhysicalStorageBuffer, uint>` at the SPIR-V level and
// the load yields zero. `[vk::buffer_reference]` is also unrecognized in
// 2026+. Primitive pointer types translate correctly per the Slang docs.
//
// Stride assumptions (must match the host upload format):
//   position : tightly packed vec4 (16 B)  — float4*
//   tangent  : tightly packed vec4 (16 B)  — float4*
//   normal   : tightly packed vec3 (12 B)  — float3*
//   uv0      : tightly packed vec3 (12 B)  — float3*
//   index    : uint (4 B)                  — uint*

public struct BindlessTriangleVtx
{
    public float3 posL[3];      // local-space positions
    public float3 normalL[3];   // local-space normals
    // xyz: local-space tangent. w: bitangent handedness (+/-1) — see the
    // GLSL twin for the rationale (composite derives bitangent via
    // cross(tangent, normal) * w to preserve export handedness).
    public float4 tangentL[3];
    public float2 uv[3];        // UV0 per vertex
};

// primId is per-instance, zero-based (matches gl_PrimitiveID at the GBuffer
// fragment site). Falls back to zero / +Z normal / Gram-Schmidt tangent /
// zero UV on missing streams, mirroring the GLSL behavior so the V-buffer
// reconstructor stays branch-identical across the two paths.
public void fetchBindlessTriangle(uint instanceId, uint primId, out BindlessTriangleVtx vtx)
{
    // Stream BDAs now live in the per-object UniformObject (folded in from the
    // former gOpaque*Handles buffers — same per-slot index).
    UniformObject obj  = gSceneObjects[instanceId];
    uint64_t indexAddr = obj.indexHandle;
    uint64_t posAddr   = obj.posHandle;
    uint64_t nrmAddr   = obj.normalHandle;
    uint64_t tanAddr   = obj.tangentHandle;
    uint64_t uvAddr    = obj.uv0Handle;

    uint idxBase = primId * 3u;
    uint3 tri = uint3(idxBase, idxBase + 1u, idxBase + 2u);
    if (indexAddr != 0ull)
    {
        uint *ib = (uint*)indexAddr;
        tri = uint3(ib[idxBase + 0u],
                    ib[idxBase + 1u],
                    ib[idxBase + 2u]);
    }

    if (posAddr != 0ull)
    {
        float4 *pb = (float4*)posAddr;
        vtx.posL[0] = pb[tri.x].xyz;
        vtx.posL[1] = pb[tri.y].xyz;
        vtx.posL[2] = pb[tri.z].xyz;
    }
    else
    {
        vtx.posL[0] = float3(0.0f);
        vtx.posL[1] = float3(0.0f);
        vtx.posL[2] = float3(0.0f);
    }

    if (nrmAddr != 0ull)
    {
        float3 *nb = (float3*)nrmAddr;
        vtx.normalL[0] = nb[tri.x];
        vtx.normalL[1] = nb[tri.y];
        vtx.normalL[2] = nb[tri.z];
    }
    else
    {
        vtx.normalL[0] = float3(0.0f, 0.0f, 1.0f);
        vtx.normalL[1] = float3(0.0f, 0.0f, 1.0f);
        vtx.normalL[2] = float3(0.0f, 0.0f, 1.0f);
    }

    if (tanAddr != 0ull)
    {
        float4 *tb = (float4*)tanAddr;
        vtx.tangentL[0] = tb[tri.x];
        vtx.tangentL[1] = tb[tri.y];
        vtx.tangentL[2] = tb[tri.z];
    }
    else
    {
        // No tangent stream — synthesize one orthogonal to N using a helper
        // axis that isn't collinear (constant fallbacks collapse on +/-X
        // walls; see the GLSL twin's note for the symptom).
        [unroll]
        for (uint k = 0u; k < 3u; ++k)
        {
            float3 N = vtx.normalL[k];
            float3 helper = (abs(N.x) < 0.9f) ? float3(1.0f, 0.0f, 0.0f)
                                              : float3(0.0f, 1.0f, 0.0f);
            float3 T = normalize(cross(helper, N));
            vtx.tangentL[k] = float4(T, 1.0f);
        }
    }

    if (uvAddr != 0ull)
    {
        float3 *ub = (float3*)uvAddr;
        vtx.uv[0] = ub[tri.x].xy;
        vtx.uv[1] = ub[tri.y].xy;
        vtx.uv[2] = ub[tri.z].xy;
    }
    else
    {
        vtx.uv[0] = float2(0.0f);
        vtx.uv[1] = float2(0.0f);
        vtx.uv[2] = float2(0.0f);
    }
}
