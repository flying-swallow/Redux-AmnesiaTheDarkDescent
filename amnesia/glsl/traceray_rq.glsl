//-------------------------------------------------------------------------------------------------
// Ray-query helpers shared by the visibility composite (shadow rays) and the
// surfel ray-trace pass (bounce + NEE occlusion).
//
// Alpha-reject parity with the rasterizer: gbuffer.frag:28-30 discards
// fragments where the diffuse texture's alpha < ALPHA_REJECT (foliage,
// fences, chain-link, decals). The BLAS used to be built with
// VK_GEOMETRY_OPAQUE_BIT, which let the driver auto-commit candidate hits
// and bypass any shader-side filter; that's now cleared host-side in
// VertexBuffer_RI.cpp. So this header drops gl_RayFlagsOpaqueEXT and runs
// the alpha test on each candidate triangle: look up the material's diffuse
// slot, interpolate UVs from the bindless vertex stream, sample alpha at
// LOD 0 (ray queries have no derivatives), and only confirm the hit when
// alpha >= ALPHA_REJECT. Materials with no diffuse texture, or instances
// with no UV stream, fall back to "always passes" — matches the gbuffer
// behaviour where a missing diffuse never discards.
//
// Reads: topLevelAS (set 2 binding 0) — declared in layouts.glsl for the
// surfel pass, declared inline in visibility_shade.frag for that pass.
// `ClosestHit` writes its caller-supplied `inout PtPayload prd`; `AnyHit`
// has no side effects beyond its return value.

#ifndef TRACERAY_RQ_GLSL
#define TRACERAY_RQ_GLSL 1

// Local BDA aliases for UV0 + index buffers — kept distinct from VBT*
// (bindless_triangle.glsl) and RT* (shaderUtils_surfel_cell.glsl) so this
// header coexists with both when surfel_raytrace.comp pulls all three in.
layout(buffer_reference, scalar) readonly buffer TRQUv0Buf   { vec3 v[]; };
layout(buffer_reference, scalar) readonly buffer TRQIndexBuf { uint v[]; };

// True iff the current candidate triangle in `q` should be treated as a
// real hit. Mirrors the gbuffer's discard exactly: no diffuse → passes,
// otherwise alpha test against ALPHA_REJECT. Cheap when there's no diffuse;
// one textureLod when there is.
bool RQCandidatePassesAlphaTest(rayQueryEXT q)
{
    uint instId = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(q, false));
    UniformObject   obj = sceneObjects[instId];
    DiffuseMaterial mat = opaqueMaterial[MATERIAL_ID(obj)];

    uint diffSlot = DiffuseMaterial_DiffuseTexture_ID(mat);
    if (diffSlot == INVALID_TEXTURE_INDEX) {
        return true;
    }

    uint64_t uvAddr = opaqueUv0Handles.data[instId];
    if (uvAddr == 0ul) {
        return true;
    }

    uint primId = uint(rayQueryGetIntersectionPrimitiveIndexEXT(q, false));
    vec2 bary2  = rayQueryGetIntersectionBarycentricsEXT(q, false);
    vec3 bary   = vec3(1.0 - bary2.x - bary2.y, bary2.x, bary2.y);

    uint64_t idxAddr = opaqueIndexHandles.data[instId];
    uint base = primId * 3u;
    uvec3 tri = uvec3(base, base + 1u, base + 2u);
    if (idxAddr != 0ul) {
        TRQIndexBuf ib = TRQIndexBuf(idxAddr);
        tri = uvec3(ib.v[base + 0u],
                    ib.v[base + 1u],
                    ib.v[base + 2u]);
    }

    TRQUv0Buf ub = TRQUv0Buf(uvAddr);
    vec2 uv = bary.x * ub.v[tri.x].xy
            + bary.y * ub.v[tri.y].xy
            + bary.z * ub.v[tri.z].xy;

    float a = textureLod(
        sampler2D(textures_2d[nonuniformEXT(diffSlot)], materialSampler),
        uv, 0.0).a;
    return a >= ALPHA_REJECT;
}

//-----------------------------------------------------------------------
// Shoot a ray and fill the caller's `prd` with closest-hit info, or set
// hitT = INFINITY on a miss. Gated on `TRACERAY_RQ_HAS_PRD` for type
// visibility: PtPayload is declared in globals.glsl, which fragment-shader
// includers that only need AnyHit do not pull in. Callers that want
// ClosestHit must `#include "globals.glsl"` and `#define TRACERAY_RQ_HAS_PRD`
// before including this header.
void ClosestHit(Ray r, inout PtPayload prd)
{
  prd.hitT = INFINITY;

  rayQueryEXT rayQuery;
  rayQueryInitializeEXT(rayQuery,
                        topLevelAS,
                        gl_RayFlagsNoneEXT,
                        0xFF,
                        r.origin,
                        0.0,
                        r.direction,
                        INFINITY);

  // Non-opaque traversal: rayQueryProceed reports candidate triangle hits
  // and the alpha test decides whether to confirm. The driver tracks the
  // current closest committed intersection across calls to ConfirmIntersection.
  while (rayQueryProceedEXT(rayQuery))
  {
    if (rayQueryGetIntersectionTypeEXT(rayQuery, false)
        == gl_RayQueryCandidateIntersectionTriangleEXT)
    {
      if (RQCandidatePassesAlphaTest(rayQuery))
      {
        rayQueryConfirmIntersectionEXT(rayQuery);
      }
    }
  }

  if (rayQueryGetIntersectionTypeEXT(rayQuery, true) !=
      gl_RayQueryCommittedIntersectionNoneEXT)
  {
    prd.hitT                = rayQueryGetIntersectionTEXT(rayQuery, true);
    prd.primitiveID         = rayQueryGetIntersectionPrimitiveIndexEXT(rayQuery, true);
    prd.instanceID          = rayQueryGetIntersectionInstanceIdEXT(rayQuery, true);
    prd.instanceCustomIndex = rayQueryGetIntersectionInstanceCustomIndexEXT(rayQuery, true);
    prd.baryCoord           = rayQueryGetIntersectionBarycentricsEXT(rayQuery, true);
    prd.objectToWorld       = rayQueryGetIntersectionObjectToWorldEXT(rayQuery, true);
    prd.worldToObject       = rayQueryGetIntersectionWorldToObjectEXT(rayQuery, true);
  }
}

//-----------------------------------------------------------------------
// Shadow / visibility query — returns true if anything (that passes the
// alpha test) blocks the ray within [0, maxDist]. TLAS is passed in so
// this helper can be shared by passes with different binding layouts
// (e.g. visibility_shade.frag, which declares its own topLevelAS at set 2).
bool AnyHit(accelerationStructureEXT tlas, Ray r, float maxDist)
{
  rayQueryEXT rayQuery;
  rayQueryInitializeEXT(rayQuery,
                        tlas,
                        gl_RayFlagsTerminateOnFirstHitEXT |
                            gl_RayFlagsSkipClosestHitShaderEXT,
                        0xFF,
                        r.origin,
                        0.0,
                        r.direction,
                        maxDist);

  while (rayQueryProceedEXT(rayQuery))
  {
    if (rayQueryGetIntersectionTypeEXT(rayQuery, false)
        == gl_RayQueryCandidateIntersectionTriangleEXT)
    {
      if (RQCandidatePassesAlphaTest(rayQuery))
      {
        rayQueryConfirmIntersectionEXT(rayQuery);
        // TerminateOnFirstHit ends traversal on the next iteration.
      }
    }
  }

  return (rayQueryGetIntersectionTypeEXT(rayQuery, true) !=
          gl_RayQueryCommittedIntersectionNoneEXT);
}

#endif  // TRACERAY_RQ_GLSL
