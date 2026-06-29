#pragma once

// Material — free functions over the engine's flat material table (gMaterials,
// set 0): typed blob loads, the shared alpha/dissolve gate, and the inline
// RayQuery shadow / reflection traces. Material data structs themselves stay in
// SceneTypes.slang (shared with the C++ host); type/flag queries are plain field
// reads off the loaded struct, so callers test them inline rather than through a
// wrapper.

#include "HostDefinitions.h"
#include "Constants.h"

import SceneTypes;       // MaterialDataBlob / MaterialHeader / concrete structs, VertexData, TriangleHit
import bindless;         // gMaterials, gSceneObjects, gMaterialSampler, gDissolveMap, gRtAccel, sampleBindless2DLevel
import Scene;            // getVertexData (vertex fetch for the RayQuery traces)
import PerFrame.resource; // gPerFrame.forceShadows

// Typed material reads. gMaterials[materialID] is a fixed-size MaterialDataBlob;
// reinterpret<T> turns it into the concrete struct (the host memcpys the struct
// into the blob, scalar layout keeps offsets identical). reinterpret handles the
// inner tex[8] array correctly, unlike a BDA struct-pointer cast (slangc 2026+
// zeros those — see BindlessTriangle.slang). `type` and `materialConfig` sit at
// offset 0/1 for every variant, so loadMaterialHeader resolves the discriminator
// and shared flags without committing to a concrete type.
public MaterialHeader loadMaterialHeader(uint materialID)
{
    return reinterpret<MaterialHeader, MaterialDataBlob>(gMaterials[materialID]);
}
public DiffuseMaterial loadDiffuseMaterial(uint materialID)
{
    return reinterpret<DiffuseMaterial, MaterialDataBlob>(gMaterials[materialID]);
}
public TranslucentMaterial loadTranslucentMaterial(uint materialID)
{
    return reinterpret<TranslucentMaterial, MaterialDataBlob>(gMaterials[materialID]);
}
public WaterMaterial loadWaterMaterial(uint materialID)
{
    return reinterpret<WaterMaterial, MaterialDataBlob>(gMaterials[materialID]);
}

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
public bool materialAlphaTest(VertexData v, uint materialID, float lod, float dissolveAmount)
{
    // One blob load; `type` is at offset 0 for every material struct, so
    // reinterpreting as DiffuseMaterial reads the discriminator regardless.
    DiffuseMaterial m = loadDiffuseMaterial(materialID);
    if (m.type != MATERIAL_TYPE_DIFFUSE) return false;  // only diffuse/decal alpha-test

    float a = 1.0f;     // plain solids dissolve from full alpha
    uint alphaIdx = m.tex[2];
    if (alphaIdx != kInvalidTextureIndex)
    {
        float4 s = sampleBindless2DLevel(alphaIdx, v.texC, lod);
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
                da = sampleBindless2DLevel(dat, v.texC, lod).r;
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
    // Force-shadows restores the old behavior by tracing against all opaque
    // instances. Normal mode culls on the shadow-caster bit.
    uint shadowMask = gPerFrame.forceShadows != 0u ? kRayMaskOpaque : kRayMaskShadow;
    q.TraceRayInline(gRtAccel, 0, shadowMask, ray);

    while (q.Proceed())
    {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
        {
            TriangleHit hit;
            hit.instanceID     = q.CandidateInstanceID();
            hit.primitiveIndex = q.CandidatePrimitiveIndex();
            hit.barycentrics   = q.CandidateTriangleBarycentrics();

            uint materialID = gSceneObjects[hit.instanceID].materialID;
            VertexData v    = getVertexData(hit);

            if (!materialAlphaTest(v, materialID, 0.f,
                                   gSceneObjects[hit.instanceID].dissolveAmount))
                q.CommitNonOpaqueTriangleHit();
        }
    }
    return q.CommittedStatus() == COMMITTED_NOTHING;
}

// Inline ray-traced reflection lookup returning the committed TriangleHit (not
// just its albedo) so the caller can fully lit-shade it — used by the raster
// water pass for sharp lit reflections. Walks the TLAS in `dir` from `origin`
// (already biased off the surface by the caller), commits the first non-opaque
// alpha-passing hit. Returns false (outHit zeroed) on miss.
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
            uint matID  = gSceneObjects[c.instanceID].materialID;
            VertexData v = getVertexData(c);
            if (!materialAlphaTest(v, matID, 0.f,
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
