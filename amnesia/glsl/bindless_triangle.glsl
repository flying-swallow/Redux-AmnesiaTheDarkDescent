#ifndef BINDLESS_TRIANGLE_GLSL
#define BINDLESS_TRIANGLE_GLSL

// Per-instance triangle vertex fetch for vis-buffer reconstruction (the
// fragment shader that takes the gbuffer's packed (objectID, primID) and
// computes Hawkins-style perspective-correct barycentric + texture
// derivatives from the 3 source vertices).
//
// Mirrors the RT-hit path in shaderUtils_surfel_cell.glsl:140-176 but lives
// outside the LAYOUTS_GLSL guard so vis-buffer shaders (which don't include
// the ray-tracing layout) can use it. Uses distinct buffer_reference type
// names (VBT*) so the two headers can coexist if a future shader needs both.

#include "forward_shared.h"
#include "bindless.resource.glsl"

layout(buffer_reference, scalar) readonly buffer VBTPositionBuf { vec4 v[]; };
layout(buffer_reference, scalar) readonly buffer VBTTangentBuf  { vec4 v[]; };
layout(buffer_reference, scalar) readonly buffer VBTNormalBuf   { vec3 v[]; };
layout(buffer_reference, scalar) readonly buffer VBTUv0Buf      { vec3 v[]; };
layout(buffer_reference, scalar) readonly buffer VBTIndexBuf    { uint v[]; };

struct BindlessTriangleVtx
{
    vec3 posL[3];     // local-space positions
    vec3 normalL[3];  // local-space normals
    // xyz: local-space tangent. w: bitangent handedness (+/-1) — host upload
    // is HPL2's eVertexBufferElement_Texture1Tangent (4-float). Composite
    // derives bitangent as cross(tangent, normal) * w to match the gbuffer.vert
    // bitangent order (cross(a_tangent, a_normal)) while preserving the export
    // handedness.
    vec4 tangentL[3];
    vec2 uv[3];       // UV0 per vertex
};

// `primId` is per-instance, zero-based — matches gl_PrimitiveID at the
// gbuffer fragment site (packed by pack_visibility in common.glsl).
void FetchBindlessTriangle(uint instanceId, uint primId,
                           out BindlessTriangleVtx vtx)
{
    uint64_t indexAddr = opaqueIndexHandles.data[instanceId];
    uint64_t posAddr   = opaquePositionHandles.data[instanceId];
    uint64_t nrmAddr   = opaqueNormalHandles.data[instanceId];
    uint64_t tanAddr   = opaqueTangentHandles.data[instanceId];
    uint64_t uvAddr    = opaqueUv0Handles.data[instanceId];

    uint idxBase = primId * 3u;
    uvec3 tri = uvec3(idxBase, idxBase + 1u, idxBase + 2u);
    if (indexAddr != 0ul) {
        VBTIndexBuf ib = VBTIndexBuf(indexAddr);
        tri = uvec3(ib.v[idxBase + 0u],
                    ib.v[idxBase + 1u],
                    ib.v[idxBase + 2u]);
    }

    if (posAddr != 0ul) {
        VBTPositionBuf pb = VBTPositionBuf(posAddr);
        vtx.posL[0] = pb.v[tri.x].xyz;
        vtx.posL[1] = pb.v[tri.y].xyz;
        vtx.posL[2] = pb.v[tri.z].xyz;
    } else {
        vtx.posL[0] = vec3(0.0);
        vtx.posL[1] = vec3(0.0);
        vtx.posL[2] = vec3(0.0);
    }

    if (nrmAddr != 0ul) {
        VBTNormalBuf nb = VBTNormalBuf(nrmAddr);
        vtx.normalL[0] = nb.v[tri.x];
        vtx.normalL[1] = nb.v[tri.y];
        vtx.normalL[2] = nb.v[tri.z];
    } else {
        vtx.normalL[0] = vec3(0.0, 0.0, 1.0);
        vtx.normalL[1] = vec3(0.0, 0.0, 1.0);
        vtx.normalL[2] = vec3(0.0, 0.0, 1.0);
    }

    if (tanAddr != 0ul) {
        VBTTangentBuf tb = VBTTangentBuf(tanAddr);
        vtx.tangentL[0] = tb.v[tri.x];
        vtx.tangentL[1] = tb.v[tri.y];
        vtx.tangentL[2] = tb.v[tri.z];
    } else {
        // Mesh has no tangent stream (loader didn't pass
        // eVertexCompileFlag_CreateTangents — see MeshLoaderMSH.cpp:284). The
        // composite still needs SOME tangent for parallax + normal mapping.
        // Pick a helper axis NOT collinear with N and cross to land in the
        // tangent plane. A constant fallback like (1,0,0) would be collinear
        // with N on +/-X-facing walls and collapse to zero under Gram-Schmidt,
        // producing NaN T/B — exactly the very-bad-warping symptom on vertical
        // surfaces. This direction won't align with the heightmap's intended
        // +U axis but at least keeps the basis well-defined.
        for (uint k = 0u; k < 3u; ++k) {
            vec3 N = vtx.normalL[k];
            vec3 helper = (abs(N.x) < 0.9) ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
            vec3 T = normalize(cross(helper, N));
            vtx.tangentL[k] = vec4(T, 1.0);
        }
    }

    if (uvAddr != 0ul) {
        VBTUv0Buf ub = VBTUv0Buf(uvAddr);
        vtx.uv[0] = ub.v[tri.x].xy;
        vtx.uv[1] = ub.v[tri.y].xy;
        vtx.uv[2] = ub.v[tri.z].xy;
    } else {
        vtx.uv[0] = vec2(0.0);
        vtx.uv[1] = vec2(0.0);
        vtx.uv[2] = vec2(0.0);
    }
}

#endif // BINDLESS_TRIANGLE_GLSL
