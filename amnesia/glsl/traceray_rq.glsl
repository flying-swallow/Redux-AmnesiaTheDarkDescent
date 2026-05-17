//-------------------------------------------------------------------------------------------------
// Ray-query helpers for surfel_raytrace.comp.
//
// The original version (kept under git history) consulted GLTF materials for
// alpha-tested transparency during traversal. The bindless-migrated renderer
// doesn't carry GLTF materials, so traversal here is opaque-only: every BLAS
// hit is committed as-is. This matches how the bindless geometry is built —
// every triangle in the TLAS is fully opaque.
//
// Reads: topLevelAS (set 2 binding 0) — declared in layouts.glsl.
// ClosestHit writes its caller-supplied `inout PtPayload prd`; AnyHit has
// no side effects beyond its return value.

#ifndef TRACERAY_RQ_GLSL
#define TRACERAY_RQ_GLSL 1

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
                        gl_RayFlagsOpaqueEXT,
                        0xFF,
                        r.origin,
                        0.0,
                        r.direction,
                        INFINITY);

  // Opaque-only traversal: rayQueryProceed will commit triangle hits itself
  // when the AS was built with VK_GEOMETRY_OPAQUE_BIT. We drain the loop to
  // be safe across drivers.
  while (rayQueryProceedEXT(rayQuery)) {}

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
// Shadow / visibility query — returns true if anything blocks the ray
// within [0, maxDist]. TLAS is passed in so this helper can be shared by
// passes with different binding layouts (e.g. visibility_shade.frag).
bool AnyHit(accelerationStructureEXT tlas, Ray r, float maxDist)
{
  rayQueryEXT rayQuery;
  rayQueryInitializeEXT(rayQuery,
                        tlas,
                        gl_RayFlagsOpaqueEXT |
                            gl_RayFlagsTerminateOnFirstHitEXT |
                            gl_RayFlagsSkipClosestHitShaderEXT,
                        0xFF,
                        r.origin,
                        0.0,
                        r.direction,
                        maxDist);

  while (rayQueryProceedEXT(rayQuery)) {}

  return (rayQueryGetIntersectionTypeEXT(rayQuery, true) !=
          gl_RayQueryCommittedIntersectionNoneEXT);
}

#endif  // TRACERAY_RQ_GLSL
