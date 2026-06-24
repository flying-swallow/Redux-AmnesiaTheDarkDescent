#pragma once
// Shared surfel-GI surface shading helpers.
//
// Extracted from MainCompositePass.cs.slang so both the final composite
// pass and the reflection-resolve pass shade a resolved TriangleHit the same
// way: albedo + emission prep, NEE-over-all-lights direct lighting (standard
// diffuse lobe, kDiffuseBrdf — demodulated, π-compensated knobs), and a
// world-space surfel-cache indirect gather. Keeping one copy avoids the two
// passes drifting on the lighting convention.

#include "../HostDefinitions.h"
#include "../Constants.h"

import SceneTypes;
import Scene;
import bindless;
import Brdf;                    // decodeMaterialRoughness (diffuse-lobe roughness)
import ILight;
import PerFrame.resource;
import SurfelGI.SurfelTypes;
import SurfelGI.SurfelGather;   // cell lookup + coverage/kernel/depth weights
import Grid;                    // gridCellPos / gridFlattenCellIndex / gridCellValid (light grid cull)
import Random;                  // RNG for soft-shadow disk sampling
import Reservoir;               // ReSTIR DI light-selection reservoir
// DiffuseShading + prepareDiffuseShading now live in the shared module; re-export
// so this module's consumers (composite, water, both direct passes) still see them.
__exported import DiffuseShading;

static const float kShadeRayEps = 1.0e-3f;

// The per-light shade (sample → NdotL-cull → ray-traced shadow →
// Li·NdotL·f_d/albedo with the standard diffuse lobe) is shadeAnalyticLight in
// the shared DiffuseShading module (re-exported above), also used by
// the surfel NEE pass. Demodulated: returns albedo-free radiance — the caller
// applies albedo where outgoing radiance is needed. V here is toward the camera
// (these wrappers shade camera-visible points: composite, water, direct pass).
float3 evalAnalyticLight(DiffuseShading sd)
{
    float3 Lr = float3(0.f);
    float3 origin = sd.posW + sd.normalW * kShadeRayEps;
    float3 V = normalize(gPerFrame.posW - sd.posW);

    // Walk the point/spot lights binned to this point's cell in the coarse world
    // light grid (built each frame by LightGridBuildPass). Conservative binning
    // means every light whose reach-sphere touches the cell is present, so the
    // deterministic sum is exact apart from the kLightsPerCellMax cap.
    CellLights cl = getCellLights(sd.posW);
    for (uint k = 0u; k < cl.count; ++k)
        Lr += shadeAnalyticLight(sd.posW, sd.normalW, V, sd.rough,
                                        cellLightAt(cl, k), origin);

    return Lr;
}

static const float3 kReservoirLum = float3(0.2126f, 0.7152f, 0.0722f);

// --- ReSTIR DI helpers ----------------------------------------------------
// The direct pass is split into a reservoir build + temporal reuse (the
// reworked DirectLightingPass) and a spatial reuse + resolve
// (DirectSpatialReusePass). These three helpers are the shared shading core.

// Target function (luminance of the unshadowed demodulated contribution) of one
// light at a shading point — the RIS / reuse weight. Re-evaluated at the
// receiving pixel whenever a neighbour's or previous frame's sample is reused.
float directTargetPdf(DiffuseShading sd, uint lightIndex)
{
    float3 V = normalize(gPerFrame.posW - sd.posW);
    return dot(unshadowedAnalyticLight(sd.posW, sd.normalW, V, sd.rough,
                                              lightIndex),
               kReservoirLum);
}

// Build a light-selection reservoir: streaming RIS over the cell's lights with
// the unshadowed-luminance target. No shadow ray — visibility is resolved later
// with a single ray. Empty cells return an empty reservoir.
Reservoir buildDirectReservoir(DiffuseShading sd, inout RNG rng)
{
    Reservoir r; r.init();

    CellLights cl = getCellLights(sd.posW);
    float3 V  = normalize(gPerFrame.posW - sd.posW);
    for (uint k = 0u; k < cl.count; ++k)
    {
        uint li = cellLightAt(cl, k);
        float pHat = dot(unshadowedAnalyticLight(sd.posW, sd.normalW, V,
                                                        sd.rough, li),
                         kReservoirLum);
        r.streamCandidate(li, pHat, rng);
    }
    r.finalizeBuild();
    return r;
}

// Resolve a reservoir to demodulated irradiance: one soft (blue-noise disk)
// shadow ray for the selected light, weighted by the UCW.
float3 resolveDirectReservoir(DiffuseShading sd, Reservoir r, inout RNG rng)
{
    if (r.isEmpty())
        return float3(0.f);
    float3 V = normalize(gPerFrame.posW - sd.posW);
    float3 origin = sd.posW + sd.normalW * kShadeRayEps;
    float3 shaded = shadeAnalyticLightSoft(sd.posW, sd.normalW, V, sd.rough,
                                                  r.y, origin, rng);
    return shaded * r.W;
}

// World-space surfel-cache indirect gather. Mirrors the per-pixel gather in
// SurfelEvaluationPass.cs.slang, but takes an explicit (posW, normalW) so it
// works for any shaded point — including a reflection / refraction hit that is
// not the primary camera pixel. Returns averaged incoming radiance (the cosθ
// factor is baked into the surfel cache), or float3(0) when no surfel covers
// the point. Multiply by albedo at the call site (f = albedo/π, pdf = cosθ/π
// → reduces to albedo) to get outgoing radiance.
float3 gatherSurfelIndirect(float3 posW, float3 normalW)
{
    uint flattenIndex;
    CellInfo cellInfo;
    if (!lookupSurfelCell(posW, gPerFrame.posW, flattenIndex, cellInfo))
        return float3(0.f);

    float4 indirect = float4(0.f);
    for (uint i = 0; i < cellInfo.surfelCount; ++i)
    {
        uint surfelIndex = gCellToSurfelBuffer[cellInfo.cellToSurfelBufferOffset + i];
        Surfel surfel = gSurfelBuffer[surfelIndex];

        float3 sNormal = normalize(surfel.normal);
        SurfelHit hit;
        if (!testSurfelCoverage(posW, normalW, surfel.position, sNormal,
                                surfel.radius, hit))
            continue;

        float contribution = surfelKernelWeight(hit.dotN, hit.dist, surfel.radius);
        contribution *= surfelDepthWeight(surfelIndex, hit.dirUnit, sNormal, hit.dist);

        SurfelRecycleInfo recycleInfo = gSurfelRecycleInfoBuffer[surfelIndex];
        float blendDelayT = smoothstep(0.f, float(kDefaultBlendingDelay), float(recycleInfo.frame));
        indirect += float4(surfel.radiance, 1.f) * contribution * blendDelayT;
    }

    if (indirect.w > 0.f)
        return indirect.xyz / indirect.w;
    return float3(0.f);
}
