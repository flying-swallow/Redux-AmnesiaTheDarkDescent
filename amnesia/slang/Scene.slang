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

import SceneTypes;
import bindless;
__exported import ILight;
import PerFrame.resource;
import BindlessTriangle;
import Random;

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
    // ALPHA_REJECT default).
    public bool alphaTest(VertexData v, uint materialID, float lod)
    {
        if (materialID >= kSolidMaterialCapacity) return false;  // water range: no diffuse entry
        DiffuseMaterial m = gDiffuseMaterials[materialID];
        uint alphaIdx = m.tex[2];
        if (alphaIdx == kInvalidTextureIndex) return false;

        float a = gTextures2D[NonUniformResourceIndex(alphaIdx)]
                      .SampleLevel(gMaterialSampler, v.texC, lod).a;
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
               | RAY_FLAG_CULL_BACK_FACING_TRIANGLES> q;
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

                if (!alphaTest(v, materialID, 0.f))
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
    // gap shows; the surfel-irradiance atlas (gIrradianceMap on set 1) is
    // the next step but its weighting / cell-hash lookup pulls in the
    // SurfelEvaluation chain.
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
                if (!alphaTest(v, matID, 0.f))
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
                if (!alphaTest(v, matID, 0.f))
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
    // shadow-ray test, return albedo·Li·NdotL (no 1/π — base-game direct
    // convention). Shared by the direct composite (SurfelShade.evalAnalyticLight)
    // and the surfel NEE pass (SurfelRayTrace), which scales the result by its
    // 1/π·invPdf·indirect factors. `origin` is the (epsilon-offset) shadow-ray
    // origin the caller already computes.
    public float3 shadeAnalyticLight(float3 posW, float3 normalW, float3 albedo,
                                     uint lightIndex, float3 origin)
    {
        LightSample ls;
        if (!sampleLightByIndex(lightIndex, posW, ls)) return float3(0.f);

        float NdotL = dot(normalW, ls.dir);
        if (NdotL <= 0.f) return float3(0.f);

        if (!materials.traceShadowRay(origin, ls.dir, ls.distance)) return float3(0.f);

        return albedo * ls.Li * NdotL;
    }

    // Soft-shadow variant: for point or spot lights with a non-zero sourceRadius,
    // aim the shadow ray at a jittered point on the light disk (radius =
    // sourceRadius, facing the shading point) so one noisy sample per frame,
    // accumulated temporally, resolves to a penumbra. Energy (Li·NdotL) still uses
    // the light centre — only occlusion is softened. sourceRadius == 0 falls back
    // to the hard centre ray, matching shadeAnalyticLight.
    public float3 shadeAnalyticLightSoft(float3 posW, float3 normalW, float3 albedo,
                                         uint lightIndex, float3 origin, inout RNG rng)
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

        return albedo * ls.Li * NdotL;
    }

    // Unshadowed analytic contribution of one light: albedo·Li·NdotL with no
    // shadow ray (0 if back-facing / sample fails). Used as the RIS target weight
    // for stochastic light selection in the direct pass (pick a light ∝ this,
    // then trace one shadow ray for it).
    public float3 unshadowedAnalyticLight(float3 posW, float3 normalW,
                                          float3 albedo, uint lightIndex)
    {
        LightSample ls;
        if (!sampleLightByIndex(lightIndex, posW, ls)) return float3(0.f);
        float NdotL = dot(normalW, ls.dir);
        if (NdotL <= 0.f) return float3(0.f);
        return albedo * ls.Li * NdotL;
    }

    // --- Mesh / hit data ------------------------------------------------------
    // Reconstruct world-space VertexData for a triangle hit. Instance
    // transform comes from gSceneObjects[hit.instanceID]; vertex streams
    // come from the per-instance BDA handle arrays via BindlessTriangle.
    public VertexData getVertexData(TriangleHit hit)
    {
        BindlessTriangleVtx vtx;
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
