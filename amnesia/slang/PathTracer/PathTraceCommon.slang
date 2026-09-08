#pragma once

// Shared path continuation and hit shading, extracted from
// PathTracePass.rt.slang so a second, reduced-resolution producer can reuse the
// same estimator instead of forking it. Nothing here is stage-specific: the
// entry-point attributes, the stage intrinsics that build a TriangleHit, and
// the output descriptors all stay in the calling .rt.slang.
//
// The two knobs a non-opaque caller needs are parameters rather than constants:
// the trace's ray flags (the opaque tracer forces opaque intersections; a
// reflection producer wants alpha-tested visibility) and the path-vertex budget
// Russian roulette measures against. `enableAnalyticLights` exists so a caller
// carrying its own direct-lighting toggle can drop the light-grid NEE sample
// without a second copy of this function.

#include "../HostDefinitions.h"
#include "../Constants.h"

import SceneTypes;
import Scene;
import RayCone;
import Material;             // load*Material / materialAlphaTest / traceShadowRay / traceReflectionHit
import bindless;             // gSceneObjects, gRtAccel, lights, grid
import Brdf;                 // SurfaceBRDF / prepareSurface / evalBrdf / sampleBrdf
import HitInfo;
import ILight;
import PerFrame.resource;
import Grid;                 // gridCellPos / gridFlattenCellIndex / gridCellValid (light grid lookup)
import Random;
import DiffuseShading;       // shared DiffuseShading + prepareDiffuseShading

// Plane-offset epsilon for the shadow / scatter ray origin.
public static const float kRayEps = 1.0e-3f;

// ScatterPayload.status bit: the path is finished and must not be traced again.
public static const uint kPathTraceStatusTerminated = 0x0002u;

// Flags the opaque path tracer traces with. Kept here so the value stays with
// the trace helper, but passed explicitly by the caller — a reflection producer
// needs alpha-tested (non-FORCE_OPAQUE) visibility instead.
public static const uint kPathTraceOpaqueRayFlags = RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES
                                                 | RAY_FLAG_FORCE_OPAQUE
                                                 | RAY_FLAG_CULL_BACK_FACING_TRIANGLES;

// [status]
// kPathTraceStatusTerminated : terminated
public struct ScatterPayload
{
    public float3 radiance;
    public float3 thp;
    public float3 origin;
    public float3 direction;
    public float firstRayLength;
    public float coneWidth;
    public float coneSpreadAngle;
    public uint currStep;
    public uint status;
    public uint rngHi;     // RNG state carried in the payload so sampling is consistent across recursion steps
    public uint rngLo;

    public __init(uint2 rngState)
    {
        this.radiance = float3(0, 0, 0);
        this.thp = float3(1, 1, 1);
        this.origin = float3(0, 0, 0);
        this.direction = float3(0, 0, 0);
        this.firstRayLength = 0.f;
        this.coneWidth = 0.f;
        this.coneSpreadAngle = 0.f;
        this.currStep = 1u;
        this.status = 0x0000;
        this.rngHi = rngState.x;
        this.rngLo = rngState.y;
    }
}

// Shadow rays use inline RayQuery (no shadow-specific miss/anyhit entry
// points needed). RIProgram's RT pipeline layout is single-miss +
// single-hit-group, so keeping shadow off the SBT lets the whole
// pipeline build with one ray type.

public void traceScatterRay(inout ScatterPayload scatterPayload, uint rayFlags)
{
    RayDesc ray;
    ray.Origin    = scatterPayload.origin;
    ray.TMin      = 0.f;
    ray.TMax      = 1e30f;
    ray.Direction = scatterPayload.direction;

    TraceRay(
        gRtAccel,
        rayFlags,
        kRayMaskOpaque,
        0, /* hitIdx */
        1, /* rayTypeCount (scatter only — shadow uses inline RayQuery) */
        0, /* missIdx */
        ray,
        scatterPayload
    );
}

public void pathTraceHandleHit(TriangleHit triangleHit, inout ScatterPayload scatterPayload,
                               uint maxPathVertices, bool enableAnalyticLights)
{
    float triLod;
    VertexData v = getVertexDataAndLodConstant(triangleHit, triLod);
    // This is a secondary traced hit, not a camera pixel or a V-buffer lookup;
    // its position and every subsequent ray origin are world-space geometry, so
    // camera projection jitter has no term to apply.
    // Capture the geometric normal before prepareDiffuseShading overwrites
    // sd.normalW with the normal-mapped normal: cone projection is a geometry
    // term, not a shading-normal term.
    float3 geometricNormalW = v.normalW;
    float hitT = distance(scatterPayload.origin, v.posW);
    if (scatterPayload.currStep == 1u)
        scatterPayload.firstRayLength = hitT;

    RayCone cone;
    cone.width       = scatterPayload.coneWidth;
    cone.spreadAngle = scatterPayload.coneSpreadAngle;
    // Accumulate the width with the incoming spread and use that spread for
    // this hit's LOD and surface footprint. The spread is widened for the
    // next bounce only after its lobe is chosen, so this bounce's material
    // lookup is not blurred by the surface that produced it.
    cone = rayConePropagate(cone, hitT);
    // The payload direction is still the incoming ray here; use it for the
    // cone-to-surface projection before it is overwritten for the next bounce.
    float texLodBase = rayConeTexLodBase(cone, triLod,
                                         geometricNormalW,
                                         scatterPayload.direction);
    float surfaceFootprintW = rayConeSurfaceFootprint(cone, geometricNormalW,
                                                      scatterPayload.direction);
    // Carry the propagated footprint into the next ray's payload.
    scatterPayload.coneWidth       = cone.width;
    scatterPayload.coneSpreadAngle = cone.spreadAngle;

    uint materialID = gSceneObjects[triangleHit.instanceID].materialID;
    float illuminationAmount = gSceneObjects[triangleHit.instanceID].illuminationAmount;
    DiffuseShading sd = prepareDiffuseShading(v, materialID, illuminationAmount,
                                              texLodBase, surfaceFootprintW,
                                              /*decodeRoughness=*/true,
                                              /*applyNormalMap=*/true);
    // Full PBR surface for the BSDF (albedo / F0 / GGX alpha). prepareDiffuseShading
    // still supplies posW, the normal-mapped normalW (the BSDF shades with the
    // bumped normal, same as the direct pass) and emission.
    SurfaceBRDF s = prepareSurface(loadDiffuseMaterial(materialID), sd.normalW, v.texC,
                                   texLodBase);

    // Emission — emissive (illumination-map) surfaces add radiance directly into
    // the path, so they bounce into the indirect term.
    scatterPayload.radiance += scatterPayload.thp * sd.emission;

    // Pull the RNG out of the payload, run NEE + bounce sample, push back.
    RNG rng;
    rng.s = uint2(scatterPayload.rngHi, scatterPayload.rngLo);

    // Direct lighting (one-sample NEE). scatterPayload.direction still holds
    // the incoming ray direction here (overwritten below for the next bounce),
    // so the outgoing/"view" direction ("V") at the hit is its negation; the
    // BSDF is view-dependent.
    //
    // Importance-sample lights from the world-space light grid: pick uniformly
    // among only the lights whose radius reaches this shading point's cell
    // (pdf = 1/count). Empty / out-of-grid cell → no contribution.
    if (enableAnalyticLights)
    {
        CellLights cl = getCellLights(sd.posW);
        if (cl.count != 0u)
        {
            // Category (b): the outgoing view direction at a secondary hit is
            // the negated incoming traced-ray direction, not a pixel-derived
            // camera vector, so it must remain free of camera jitter.
            float3 V        = -scatterPayload.direction;
            uint   pick     = rng.next_uint(cl.count);
            uint   lightIdx = cellLightAt(cl, pick);
            float  invPdf   = float(cl.count);

            // Shared shadow-ray gate (shadeAnalyticLightRadiance: sample →
            // NdotL-cull → shadow ray → raw Li·NdotL, undemodulated and BRDF-free),
            // weighted here by the full GGX BSDF. Albedo is inside evalBrdf — this
            // is BOUNCE albedo; the primary hit's factor stays out of the diffuse
            // estimator (see file header).
            float3 origin = sd.posW + sd.normalW * kRayEps;
            float3 Ldir, LiNdotL;
            if (shadeAnalyticLightRadiance(sd.posW, sd.normalW,
                                           sd.surfaceFootprintW, lightIdx,
                                           origin, Ldir, LiNdotL))
            {
                float3 perLight = evalBrdf(s, V, Ldir) * LiNdotL;
                scatterPayload.radiance += scatterPayload.thp * (perLight * invPdf);
            }
        }
    }

    // Sample the next bounce from the full BSDF (diffuse or GGX lobe, picked
    // stochastically). `weight` is already f·N.L/pdf with the lobe-selection
    // probability folded in — no cosine / albedo / 1/pSpec on top. V is the
    // incoming ray negated, read BEFORE scatterPayload.direction is overwritten.
    // Category (b): the bounce sampler sees the same traced-ray-derived view
    // direction, entirely in world space; it is not reconstructed from a
    // camera pixel and receives no projection jitter.
    float3 Vb = -scatterPayload.direction;
    float3 bounceL;
    float3 bounceWeight;
    bool   bounceSpecular;
    if (!sampleBrdf(s, Vb, rng, bounceL, bounceWeight, bounceSpecular))
    {
        scatterPayload.status |= kPathTraceStatusTerminated;
        scatterPayload.rngHi = rng.s.x;
        scatterPayload.rngLo = rng.s.y;
        return;
    }

    scatterPayload.origin    = sd.posW + sd.normalW * kRayEps;
    scatterPayload.direction = bounceL;
    scatterPayload.thp      *= bounceWeight;
    // `bounceSpecular` makes a near-mirror bounce barely widen while a diffuse
    // bounce widens to the hemisphere bound.
    scatterPayload.coneSpreadAngle =
        rayConeWiden(cone, rayConeSurfaceSpreadAngle(s.rough, bounceSpecular)).spreadAngle;

    // Russian roulette on luminance(thp). There is nothing to finalize into, so
    // the path just keeps going until the caller's bounce budget
    // (maxPathVertices — kPathTraceMaxBounces for the opaque tracer, enforced by
    // its rayGen loop) or RR kills it.
    uint rayStep = maxPathVertices;
    if (scatterPayload.currStep < rayStep)
    {
        float rrValue = dot(scatterPayload.thp, float3(0.2126f, 0.7152f, 0.0722f));
        float prob    = max(0.f, 1.f - rrValue);
        if (rng.next_float() >= prob)
        {
            scatterPayload.thp /= max(1e-12f, 1.f - prob);
            if (any(scatterPayload.thp > 0.f))
                scatterPayload.currStep++;
            else
                scatterPayload.status |= kPathTraceStatusTerminated;

            scatterPayload.rngHi = rng.s.x;
            scatterPayload.rngLo = rng.s.y;
            return;
        }
    }

    scatterPayload.currStep++;

    scatterPayload.rngHi = rng.s.x;
    scatterPayload.rngLo = rng.s.y;
}
