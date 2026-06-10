#pragma once

// Global Scene wrapper for the engine's bindless set (set 0) + per-frame
// UBO (set 1). Surfel-port shaders that previously imported Falcor's
// `Scene.Scene` now `import Scene;` instead and call `gScene.foo(...)`.
//
// Design choices:
//   * Lights use a unified dispatcher (getLight returning ILight,
//     sampleLightByIndex calling .sample() through that). There are no
//     typed gScene.getPointLight() / etc. accessors — iterate
//     0..getLightCount().
//   * Materials remain per-type because each material variant has its own
//     SSBO with distinct field layout — runtime branching on `type`
//     wouldn't save anything.
//   * Vertex fetch delegates to BindlessTriangle.slang; barycentric
//     interpolation + object-to-world transform happen here.
//
// Known stubs / non-goals (see implementation plan):
//   * gScene.rtAccel — TLAS binding not added yet.
//   * gScene.envMap.eval — env map binding not declared yet.
//   * Per-light gobo + LUT attenuation: PointLight.sample / SpotLight.sample
//     in ILight.slang do analytic fallback only today; the
//     unified dispatcher inherits that.

#include "HostDefinitions.h"
#include "Constants.h"          // kLightGrid* / kLightsPerCellMax (lightLevelAt)

import SceneTypes;
import bindless;
__exported import ILight;
import PerFrame.resource;
import BindlessTriangle;
import Brdf;                    // evalDiffuseDemodulated (standard diffuse lobe)
import Random;
import Grid;                    // gridCellPos / gridFlattenCellIndex / gridCellValid (lightLevelAt)
import Utils.Color.ColorHelpers; // linearToSRGB (lightLevelAt's authored-space max channel)

// Concentric (low-distortion) unit-disk map of two [0,1) samples — used for
// soft-shadow light-disk sampling. Returns a point in the unit disk.
float2 concentricDiskSample(float2 u)
{
    float2 o = 2.0f * u - 1.0f;
    if (o.x == 0.0f && o.y == 0.0f) return float2(0.0f);
    float r, theta;
    const float kPiOver4 = 0.78539816f;
    const float kPiOver2 = 1.57079633f;
    if (abs(o.x) > abs(o.y)) { r = o.x; theta = kPiOver4 * (o.y / o.x); }
    else                     { r = o.y; theta = kPiOver2 - kPiOver4 * (o.x / o.y); }
    return r * float2(cos(theta), sin(theta));
}

// Falcor-compatible Ray struct (used by computeRayPinhole). Local to this
// module to avoid pulling in a Falcor `Scene` dependency.
public struct Ray
{
    public float3 origin;
    public float  tMin;
    public float3 dir;
    public float  tMax;
};

// Flat-index → ILight resolver. Slices the virtual index across the two
// typed light SSBOs (point, spot) in the same order documented by
// amnesia/glsl/surfel_light_nee.glsl. The interface return goes through
// Slang's existential boxing — fine for the cold paths (UI, debug);
// performance-critical NEE should call sampleLightByIndex below, which
// branches directly to the typed PointLight/SpotLight.sample().
public ILight getLight(uint ID)
{
    uint nPoint = gPerFrame.pointLightCount;
    if (ID < nPoint) return gPointLights[ID];
    ID -= nPoint;

    return gSpotLights[ID];
}

public struct SceneCamera
{
    public float3 getPosition()
    {
        return gPerFrame.posW;
    }

    public float4x4 getViewProj()
    {
        return mul(gPerFrame.projMat, gPerFrame.viewMat);
    }

    // No jitter on the engine UBO today; alias of getViewProj. When TAA /
    // temporal jitter lands, separate the two and update callers that
    // explicitly want the no-jitter matrix (visibility reprojection).
    public float4x4 getViewProjNoJitter()
    {
        return mul(gPerFrame.projMat, gPerFrame.viewMat);
    }

    // Last frame's no-jitter view-proj — paired with getViewProjNoJitter() and
    // UniformObject.prevModelMat to build motion vectors in the gbuffer.
    public float4x4 getPrevViewProj()
    {
        return mul(gPerFrame.prevProjMat, gPerFrame.prevViewMat);
    }

    // Non-normalized primary ray direction for a pinhole camera, matching
    // Falcor's Scene::Camera::computeNonNormalizedRayDirPinhole. The pixel
    // origin is top-left; `+ndc.x*cameraU + ndc.y*cameraV + cameraW` walks
    // out from the optical axis along the pre-scaled basis vectors filled
    // host-side in cHybridRenderer::Draw().
    public float3 computeNonNormalizedRayDirPinhole(uint2 pixel, uint2 frameDim, bool applyJitter)
    {
        float2 p = (float2(pixel) + float2(0.5f, 0.5f)) / float2(frameDim);
        if (applyJitter) p += float2(-gPerFrame.jitterX, gPerFrame.jitterY);
        float2 ndc = float2(2.0f, -2.0f) * p + float2(-1.0f, 1.0f);
        return ndc.x * gPerFrame.cameraU + ndc.y * gPerFrame.cameraV + gPerFrame.cameraW;
    }

    // World-space pinhole ray, Falcor parity. tMin/tMax scale by 1/cos(theta)
    // so the engine's view-space near/far planes map exactly onto ray length
    // — same trick Falcor uses to keep ray bounds tight at the frustum edge.
    public Ray computeRayPinhole(uint2 pixel, uint2 frameDim, bool applyJitter = true)
    {
        Ray r;
        r.origin = gPerFrame.posW;
        r.dir    = normalize(computeNonNormalizedRayDirPinhole(pixel, frameDim, applyJitter));

        float invCos = 1.0f / dot(normalize(gPerFrame.cameraW), r.dir);
        r.tMin = gPerFrame.zNear * invCos;
        r.tMax = (gPerFrame.zFar > 0.0f ? gPerFrame.zFar : 1e6f) * invCos;
        return r;
    }
};

public struct SceneMaterials
{
    // Alpha-test for the diffuse opaque path: read tex[2] (alpha slot —
    // see SceneTypes.slang DiffuseMaterial comment), sample at lod with
    // gMaterialSampler, reject when a < 0.5 (matches the GLSL
    // ALPHA_REJECT default). Dedicated grayscale alpha maps (e.g.
    // chandelier_simple_alpha.tga, loaded as R8_UNORM) carry the mask in
    // .r — kMaterialFlagIsAlphaSingleChannel picks the channel.
    // Alpha test + CoverageAmount dissolve — ONE gate for every ray/raster
    // context (gbuffer FS, V-buffer anyhit, surfel scatter anyhit, shadow /
    // reflection RayQuery loops), so a fading object's primary visibility,
    // shadows, and direct lighting all dissolve consistently instead of a
    // half-dissolved mesh casting a full-strength ghost shadow.
    //
    // Dissolve math mirrors the legacy solid_z.frag.fsl (noise reshape per
    // material variant, threshold at the z-pass ALPHA_REJECT 0.5) with one
    // deviation: the noise is sampled in UV space (v.texC, wrap) instead of
    // legacy screen space — RayQuery callers have no pixel coordinate, and a
    // UV-anchored pattern sticks to the object instead of crawling with the
    // camera. `dissolveAmount` is UniformObject.dissolveAmount
    // (GetCoverageAmount); pass 1.0 semantics come from the object, not a
    // default, so every caller fetches it. NOTE: rays only reach this for
    // non-FORCE_OPAQUE instances — cHybridRenderer drops that flag while
    // coverage < 1 or the material carries dissolve variants.
    public bool alphaTest(VertexData v, uint materialID, float lod, float dissolveAmount)
    {
        if (materialID >= kSolidMaterialCapacity) return false;  // water range: no diffuse entry
        DiffuseMaterial m = gDiffuseMaterials[materialID];

        float a = 1.0f;     // plain solids dissolve from full alpha
        uint alphaIdx = m.tex[2];
        if (alphaIdx != kInvalidTextureIndex)
        {
            float4 s = gTextures2D[NonUniformResourceIndex(alphaIdx)]
                           .SampleLevel(gMaterialSampler, v.texC, lod);
            a = (m.materialConfig & kMaterialFlagIsAlphaSingleChannel) != 0u ? s.r : s.a;
        }
        else if (dissolveAmount >= 1.0f &&
                 (m.materialConfig & (kMaterialFlagEnableDissolveAlpha
                                    | kMaterialFlagUseDissolveFilter)) == 0u)
        {
            return false;   // plain solid at full coverage — cheap keep
        }

        bool hasDissolveAlpha  = (m.materialConfig & kMaterialFlagEnableDissolveAlpha) != 0u;
        bool hasDissolveFilter = (m.materialConfig & kMaterialFlagUseDissolveFilter)  != 0u;
        if (dissolveAmount < 1.0f || hasDissolveAlpha || hasDissolveFilter)
        {
            float d = gDissolveMap.SampleLevel(gMaterialSampler, v.texC, 0.f).x;
            if (hasDissolveAlpha)
            {
                // Combine the global noise with the material's DissolveAlpha
                // map (tex[6]): noise squeezed to [0.75,1], map subtracts up
                // to 0.25 — matches the legacy HasDissolveAlpha branch.
                d = d * 0.25f + 0.75f;
                uint dat = m.tex[6];
                float da = 1.0f;
                if (dat != kInvalidTextureIndex)
                    da = gTextures2D[NonUniformResourceIndex(dat)]
                             .SampleLevel(gMaterialSampler, v.texC, lod).r;
                d -= (0.25f - da * 0.25f);
            }
            else
            {
                d = d * 0.5f + 0.5f;
            }
            a = d - (1.0f - dissolveAmount * a) * 0.5f;
        }
        return a < 0.5f;
    }

    // True when the diffuse material at `materialID` carries a reflection
    // cubemap slot — used by SurfelVBuffer.rt to gate the optional
    // mirror-bounce on chrome / glassy surfaces. Matches the legacy engine
    // "has cubemap" check (DiffuseMaterial::cubeMapTextureIndex valid).
    public bool isReflective(uint materialID)
    {
        if (materialID >= kSolidMaterialCapacity) return false;  // water range: no diffuse entry
        DiffuseMaterial m = gDiffuseMaterials[materialID];
        return m.cubeMapTextureIndex != kInvalidTextureIndex;
    }

    // True when materialID falls in the translucent partition
    // [kTranslucentMaterialBase, kWaterMaterialBase) — indexes gTranslucentMaterials.
    public bool isTranslucent(uint materialID)
    {
        return materialID >= kTranslucentMaterialBase
            && materialID <  kWaterMaterialBase;
    }

    // True when the material was flagged HasRefraction host-side
    // (MaterialResource.cpp:CreateMaterailConfigFlags). Only translucent (glass)
    // materials carry the flag; reads it from the translucent table. Drives the
    // SurfelVBuffer.rt closeHit ray-bend so the V-buffer stores the refracted
    // background instead of the glass itself. (Water refraction is handled by
    // the earlier isWater branch.)
    public bool isRefractive(uint materialID)
    {
        if (!isTranslucent(materialID)) return false;
        TranslucentMaterial m = gTranslucentMaterials[materialID - kTranslucentMaterialBase];
        return (m.materialConfig & kMaterialFlagHasRefraction) != 0u;
    }

    // True when materialID falls in the water partition of the continuous
    // material-id space (ids >= kWaterMaterialBase index gWaterMaterials).
    // Drives the SurfelVBuffer.rt wave-animated water branch, which re-traces
    // the refraction ray as a kind-0 bounce so the GI composite shades the
    // refracted underwater background. Checked before the generic glass
    // (isRefractive) branch.
    public bool isWater(uint materialID)
    {
        return materialID >= kWaterMaterialBase;
    }

    // Inline non-opaque shadow query. The BLAS is built with
    // VK_GEOMETRY_OPAQUE_BIT cleared (VertexBuffer_RI.cpp:589 sets
    // geom.flags = 0), so the driver reports every triangle hit as a
    // *candidate* — the shader must explicitly commit. A plain
    // `Proceed() + CommittedStatus()` (no commit call) silently reports
    // every shadow ray as unoccluded, which is why direct-light shadows
    // were missing from the GI composite before this helper landed.
    //
    // Loops while candidates remain, runs the diffuse alpha test on each
    // non-opaque triangle, and commits when alpha passes. Returns true
    // when [origin, origin + dir*tMax] is unoccluded — matches the GLSL
    // twin in amnesia/glsl/traceray_rq.glsl::AnyHit.
    public bool traceShadowRay(float3 origin, float3 dir, float tMax)
    {
        RayDesc ray;
        ray.Origin    = origin;
        ray.Direction = dir;
        ray.TMin      = 0.f;
        ray.TMax      = tMax;

        RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH
               | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES
               | RAY_FLAG_CULL_FRONT_FACING_TRIANGLES> q;
        q.TraceRayInline(gRtAccel, 0, kRayMaskOpaque, ray);

        while (q.Proceed())
        {
            if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
            {
                TriangleHit hit;
                hit.instanceID     = q.CandidateInstanceID();
                hit.primitiveIndex = q.CandidatePrimitiveIndex();
                hit.barycentrics   = q.CandidateTriangleBarycentrics();

                uint materialID = gScene.getMaterialID(hit.instanceID);
                VertexData v    = gScene.getVertexData(hit);

                if (!alphaTest(v, materialID, 0.f,
                               gSceneObjects[hit.instanceID].dissolveAmount))
                    q.CommitNonOpaqueTriangleHit();
            }
        }
        return q.CommittedStatus() == COMMITTED_NOTHING;
    }

    // Inline ray-traced reflection lookup. Walks the TLAS in `dir` starting
    // at `origin` (already biased off the surface by the caller), commits
    // the first non-opaque alpha-passing hit, and returns the hit's diffuse
    // albedo as the reflection contribution. Misses return float3(0) and
    // set `hit` = false so the caller can fall back to a cube-map sample.
    //
    // This is the Phase 4 starting cut for ray-traced reflections — it
    // gives correct mirrored geometry (right object in the right pixel),
    // but the reflection colour is just the hit material's albedo rather
    // than fully GI-shaded. Promote to a full evaluation when the visual
    // gap shows; the surfel radiance gather (the SurfelEvaluation chain)
    // is the next step but its weighting / cell-hash lookup pulls in the
    // full evaluation path.
    public float3 traceReflection(float3 origin, float3 dir, float tMax,
                                  out bool hit)
    {
        RayDesc ray;
        ray.Origin    = origin;
        ray.Direction = dir;
        ray.TMin      = 0.f;
        ray.TMax      = tMax;

        RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES
               | RAY_FLAG_CULL_BACK_FACING_TRIANGLES> q;
        q.TraceRayInline(gRtAccel, 0, kRayMaskOpaque, ray);

        while (q.Proceed())
        {
            if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
            {
                TriangleHit candidate;
                candidate.instanceID     = q.CandidateInstanceID();
                candidate.primitiveIndex = q.CandidatePrimitiveIndex();
                candidate.barycentrics   = q.CandidateTriangleBarycentrics();

                uint matID  = gScene.getMaterialID(candidate.instanceID);
                VertexData v = gScene.getVertexData(candidate);
                if (!alphaTest(v, matID, 0.f,
                               gSceneObjects[candidate.instanceID].dissolveAmount))
                    q.CommitNonOpaqueTriangleHit();
            }
        }

        if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
        {
            hit = false;
            return float3(0.f);
        }

        hit = true;

        TriangleHit triangleHit;
        triangleHit.instanceID     = q.CommittedInstanceID();
        triangleHit.primitiveIndex = q.CommittedPrimitiveIndex();
        triangleHit.barycentrics   = q.CommittedTriangleBarycentrics();

        VertexData v        = gScene.getVertexData(triangleHit);
        uint materialID     = gScene.getMaterialID(triangleHit.instanceID);
        DiffuseMaterial m   = gDiffuseMaterials[materialID];
        uint diffuseTex     = m.tex[0];
        if (diffuseTex == kInvalidTextureIndex)
            return float3(0.5f);  // fallback grey
        return gTextures2D[NonUniformResourceIndex(diffuseTex)]
                  .SampleLevel(gMaterialSampler, v.texC, 0.f).rgb;
    }

    // Like traceReflection but returns the committed TriangleHit (not just its
    // albedo) so the caller can fully lit-shade it — used by the raster water
    // pass for sharp lit reflections. Returns false (outHit zeroed) on miss.
    public bool traceReflectionHit(float3 origin, float3 dir, float tMax, out TriangleHit outHit)
    {
        outHit.instanceID = 0u; outHit.primitiveIndex = 0u; outHit.barycentrics = float2(0.f);

        RayDesc ray;
        ray.Origin = origin; ray.Direction = dir; ray.TMin = 0.f; ray.TMax = tMax;

        RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES
               | RAY_FLAG_CULL_BACK_FACING_TRIANGLES> q;
        q.TraceRayInline(gRtAccel, 0, kRayMaskOpaque, ray);

        while (q.Proceed())
        {
            if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
            {
                TriangleHit c;
                c.instanceID     = q.CandidateInstanceID();
                c.primitiveIndex = q.CandidatePrimitiveIndex();
                c.barycentrics   = q.CandidateTriangleBarycentrics();
                uint matID  = gScene.getMaterialID(c.instanceID);
                VertexData v = gScene.getVertexData(c);
                if (!alphaTest(v, matID, 0.f,
                               gSceneObjects[c.instanceID].dissolveAmount))
                    q.CommitNonOpaqueTriangleHit();
            }
        }

        if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
            return false;

        outHit.instanceID     = q.CommittedInstanceID();
        outHit.primitiveIndex = q.CommittedPrimitiveIndex();
        outHit.barycentrics   = q.CommittedTriangleBarycentrics();
        return true;
    }
};

public struct Scene
{
    public SceneCamera    camera;
    public SceneMaterials materials;

    // --- Objects / materials --------------------------------------------------
    public UniformObject getObject(uint slot)
    {
        return gSceneObjects[slot];
    }

    // Material accessors over the continuous, range-partitioned material-id
    // space (see Constants.h). These are the single place that maps a
    // partitioned materialID to a buffer entry — entry shaders must go through
    // them rather than indexing gDiffuseMaterials / gWaterMaterials directly, so
    // a repartition only touches Scene.slang + Constants.h.

    // Solid + decal materials occupy [0, kSolidMaterialCapacity), so the
    // partitioned id is the slot directly. Valid only for solid-range ids.
    public DiffuseMaterial getDiffuseMaterial(uint materialID)
    {
        return gDiffuseMaterials[materialID];
    }

    // Translucent materials occupy [kTranslucentMaterialBase, kWaterMaterialBase);
    // de-bias into the translucent table. Valid only for translucent-range ids
    // (isTranslucent(materialID) == true).
    public TranslucentMaterial getTranslucentMaterial(uint materialID)
    {
        return gTranslucentMaterials[materialID - kTranslucentMaterialBase];
    }

    // Water materials occupy [kWaterMaterialBase, +kWaterMaterialCapacity);
    // de-bias the partitioned id into the compact water table. Valid only for
    // water-range ids (isWater(materialID) == true).
    public WaterMaterial getWaterMaterial(uint materialID)
    {
        return gWaterMaterials[materialID - kWaterMaterialBase];
    }

    public DecalMaterial getDecalMaterial(uint slot)
    {
        return gDecalMaterials[slot];
    }

    // Convenience accessors used by GBuffer / V-buffer entry points.
    public uint getMaterialID(uint objectSlot)
    {
        return gSceneObjects[objectSlot].materialID;
    }

    public float4x4 getWorldMatrix(uint objectSlot)
    {
        return gSceneObjects[objectSlot].modelMat;
    }

    // --- Lights ---------------------------------------------------------------
    public uint getLightCount()
    {
        return gPerFrame.pointLightCount
             + gPerFrame.spotLightCount;
    }

    // Performance path for surfel NEE: branches directly on the slice
    // index and calls the typed PointLight/SpotLight.sample() without going
    // through an interface existential.
    public bool sampleLightByIndex(uint virtualIndex, float3 shadingPosW, out LightSample ls)
    {
        ls.Li       = float3(0.0f);
        ls.pdf      = 1.0f;
        ls.dir      = float3(0.0f, 1.0f, 0.0f);
        ls.distance = 0.0f;

        uint nPoint = gPerFrame.pointLightCount;
        if (virtualIndex < nPoint)
        {
            return gPointLights[virtualIndex].sample(shadingPosW, ls);
        }
        virtualIndex -= nPoint;

        uint nSpot = gPerFrame.spotLightCount;
        if (virtualIndex < nSpot)
        {
            return gSpotLights[virtualIndex].sample(shadingPosW, ls);
        }
        return false;
    }

    // Shade one analytic light at a surface point: sample it, NdotL-cull,
    // shadow-ray test, return Li·NdotL·f_d/albedo where f_d is the standard
    // diffuse lobe (kDiffuseBrdf, carries its 1/π — the intensity knobs are
    // π-compensated; no albedo — demodulated, the caller applies albedo where
    // outgoing radiance is needed). Shared by the direct composite
    // (SurfelShade.evalAnalyticLight, V = toward camera) and the surfel NEE
    // pass (SurfelRayTrace, V = back along the scatter ray), which scales the
    // result by its albedo·invPdf·indirect factors. `origin` is the
    // (epsilon-offset) shadow-ray origin the caller already computes; `rough`
    // is the perceptual diffuse-lobe roughness (decodeMaterialRoughness).
    public float3 shadeAnalyticLight(float3 posW, float3 normalW, float3 V,
                                     float rough, uint lightIndex, float3 origin)
    {
        LightSample ls;
        if (!sampleLightByIndex(lightIndex, posW, ls)) return float3(0.f);

        float NdotL = dot(normalW, ls.dir);
        if (NdotL <= 0.f) return float3(0.f);

        if (!materials.traceShadowRay(origin, ls.dir, ls.distance)) return float3(0.f);

        return ls.Li * NdotL * evalDiffuseDemodulated(normalW, V, ls.dir, rough);
    }

    // Soft-shadow variant: for point or spot lights with a non-zero sourceRadius,
    // aim the shadow ray at a jittered point on the light disk (radius =
    // sourceRadius, facing the shading point) so one noisy sample per frame,
    // accumulated temporally, resolves to a penumbra. Energy still uses the
    // light centre — only occlusion is softened. sourceRadius == 0 falls back
    // to the hard centre ray, matching shadeAnalyticLight. Demodulated like
    // shadeAnalyticLight: Li·NdotL·f_d/albedo, no albedo.
    public float3 shadeAnalyticLightSoft(float3 posW, float3 normalW, float3 V,
                                         float rough, uint lightIndex,
                                         float3 origin, inout RNG rng)
    {
        LightSample ls;
        if (!sampleLightByIndex(lightIndex, posW, ls)) return float3(0.f);

        float NdotL = dot(normalW, ls.dir);
        if (NdotL <= 0.f) return float3(0.f);

        float3 sDir = ls.dir;
        float  sMax = ls.distance;
        // Pick the source radius + light centre from whichever array this virtual
        // index lands in: [0, nPoint) are point lights, [nPoint, nPoint+nSpot) spots.
        float  r    = 0.f;
        float3 lpos = float3(0.f);
        uint nPoint = gPerFrame.pointLightCount;
        if (lightIndex < nPoint)
        {
            r    = gPointLights[lightIndex].sourceRadius;
            lpos = gPointLights[lightIndex].position;
        }
        else
        {
            uint si = lightIndex - nPoint;
            if (si < gPerFrame.spotLightCount)
            {
                r    = gSpotLights[si].sourceRadius;
                lpos = gSpotLights[si].position;
            }
        }
        if (r > 0.f)
        {
            // Tangent basis perpendicular to the light direction.
            float3 t = normalize(abs(ls.dir.y) < 0.99f
                                 ? cross(float3(0.f, 1.f, 0.f), ls.dir)
                                 : cross(float3(1.f, 0.f, 0.f), ls.dir));
            float3 b = cross(ls.dir, t);
            float2 d = concentricDiskSample(rng.next_float2()) * r;
            float3 target = lpos + d.x * t + d.y * b;
            float3 toT = target - origin;
            sMax = length(toT);
            sDir = toT / max(sMax, 1.0e-6f);
        }

        if (!materials.traceShadowRay(origin, sDir, sMax)) return float3(0.f);

        return ls.Li * NdotL * evalDiffuseDemodulated(normalW, V, ls.dir, rough);
    }

    // Unshadowed analytic contribution of one light: Li·NdotL·f_d/albedo with
    // no shadow ray (0 if back-facing / sample fails). Used as the RIS target
    // weight for stochastic light selection in the direct pass (pick a light
    // ∝ this, then trace one shadow ray for it) — including the lobe keeps the
    // target proportional to the true integrand.
    public float3 unshadowedAnalyticLight(float3 posW, float3 normalW, float3 V,
                                          float rough, uint lightIndex)
    {
        LightSample ls;
        if (!sampleLightByIndex(lightIndex, posW, ls)) return float3(0.f);
        float NdotL = dot(normalW, ls.dir);
        if (NdotL <= 0.f) return float3(0.f);
        return ls.Li * NdotL * evalDiffuseDemodulated(normalW, V, ls.dir, rough);
    }

    // Scalar "surrounding light" heuristic for AffectedByLightLevel
    // translucents — base-game afLightLevel semantics (the legacy CPU loop in
    // MaterialType_BasicTranslucent::SetupObjectSpecificData): sum
    // maxChannel(authored color) · saturate(1 − d/authoredRadius) over the
    // lights binned to the grid cell at posW, saturated at 1. Normal-free and
    // shadow-free: billboards have no normal and the legacy heuristic never
    // had occlusion (or a spot-cone term — spots fall off radially like
    // points, matching the original).
    //   * `intensity` IS the authored engine radius (uploaded verbatim for
    //     both light types — Falcor LightData semantics, no scale knobs), so
    //     it is used directly. The struct's `radius` field is the grid-bin
    //     reach, a different quantity.
    //   * GPU light colors are linear (sRGB-decoded at upload) while the
    //     legacy heuristic read the authored sRGB values, so the max channel
    //     is re-encoded to match the CPU translucent-mesh path's dimming
    //     curve.
    //   * Out-of-grid points return 1.0 (don't darken) — distant geometry
    //     beyond the camera-centered grid shouldn't fade to black.
    public float lightLevelAt(float3 posW)
    {
        int3 cell = gridCellPos(posW, camera.getPosition(), kLightGridUnit);
        if (getLightCount() == 0u || !gridCellValid(cell, kLightGridDim))
            return 1.0f;

        uint flat = gridFlattenCellIndex(cell, kLightGridDim);
        uint n    = min(gLightGridCount[flat], kLightsPerCellMax);
        uint nPoint = gPerFrame.pointLightCount;

        float sum = 0.0f;
        for (uint k = 0u; k < n; ++k)
        {
            uint li = gLightGridList[flat * kLightsPerCellMax + k];
            float3 lpos;
            float3 lcolor;
            float  lintensity;
            if (li < nPoint)
            {
                lpos       = gPointLights[li].position;
                lcolor     = gPointLights[li].color;
                lintensity = gPointLights[li].intensity;
            }
            else
            {
                uint si = li - nPoint;
                if (si >= gPerFrame.spotLightCount) continue;
                lpos       = gSpotLights[si].position;
                lcolor     = gSpotLights[si].color;
                lintensity = gSpotLights[si].intensity;
            }

            float authoredRadius = lintensity;
            if (authoredRadius <= 0.0f) continue;

            float maxC = linearToSRGB(max(lcolor.x, max(lcolor.y, lcolor.z)));
            float dist = length(posW - lpos);
            sum += maxC * saturate(1.0f - dist / authoredRadius);
            if (sum >= 1.0f) return 1.0f;
        }
        return sum;
    }

    // --- Mesh / hit data ------------------------------------------------------
    // Reconstruct world-space VertexData for a triangle hit. Instance
    // transform comes from gSceneObjects[hit.instanceID]; vertex streams
    // come from the per-instance BDA handle arrays via BindlessTriangle.
    public VertexData getVertexData(TriangleHit hit)
    {
        BindlessTriangleVtx vtx;
        return getVertexDataAndTri(hit, vtx);
    }

    // Same as getVertexData, but also returns the raw per-vertex triangle data
    // (positions / normals / tangents / UVs). Parallax mapping needs the
    // per-vertex UVs to invert a UV offset back into a barycentric delta — see
    // SurfelVBuffer.rt.slang. Fetches the bindless triangle once so callers that
    // want both don't pay the BDA handle loads twice.
    public VertexData getVertexDataAndTri(TriangleHit hit, out BindlessTriangleVtx vtx)
    {
        fetchBindlessTriangle(hit.instanceID, hit.primitiveIndex, vtx);

        float b1 = hit.barycentrics.x;
        float b2 = hit.barycentrics.y;
        float b0 = 1.0f - b1 - b2;

        float3 posL    = vtx.posL[0]    * b0 + vtx.posL[1]    * b1 + vtx.posL[2]    * b2;
        float3 normalL = vtx.normalL[0] * b0 + vtx.normalL[1] * b1 + vtx.normalL[2] * b2;
        float3 tanL    = vtx.tangentL[0].xyz * b0
                       + vtx.tangentL[1].xyz * b1
                       + vtx.tangentL[2].xyz * b2;
        float2 uv      = vtx.uv[0]      * b0 + vtx.uv[1]      * b1 + vtx.uv[2]      * b2;

        UniformObject obj = gSceneObjects[hit.instanceID];
        float4x4 modelMat = obj.modelMat;
        // Upper-3x3 carries rotation + uniform scale; for normals we'd
        // ideally want the inverse-transpose, but the engine ships
        // uniform-scaled meshes, so the upper-3x3 of modelMat is
        // equivalent up to a normalize().
        float3x3 nrmMat = float3x3(modelMat[0].xyz,
                                   modelMat[1].xyz,
                                   modelMat[2].xyz);

        VertexData v;
        v.posW     = mul(modelMat, float4(posL, 1.0f)).xyz;
        v.normalW  = normalize(mul(nrmMat, normalL));
        v.tangentW = normalize(mul(nrmMat, tanL));
        v.texC     = uv;
        return v;
    }
};

public static Scene gScene;
