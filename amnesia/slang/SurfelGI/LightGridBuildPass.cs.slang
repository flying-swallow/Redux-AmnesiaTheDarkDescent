// LightGridBuildPass — builds the coarse, camera-centered world-space light
// grid used by the surfel ray-trace NEE for light importance sampling.
//
//   binLights : bin each point/spot light into every grid cell its radius-sphere
//               overlaps, appending the UNIFIED light index (the same index
//               space as Scene.sampleLightByIndex: point [0,nPoint), spot
//               [nPoint, nPoint+nSpot)) to that cell's list (capped at
//               kLightsPerCellMax). The per-cell count buffer is zeroed by a
//               vkCmdFillBuffer on the host before this runs.
//
// Binning is conservative (a light lands in every cell it could reach):
// over-inclusion is harmless (the NEE's sample() rejects out-of-range lights,
// still unbiased given pdf = 1/listLen), while under-inclusion would silently
// drop a light → bias. Box lights are skipped (NEE returns false for them).
//
// Dispatched each frame after the surfel cell scatter and before the surfel
// ray trace (Stage E) in cHybridRenderer::Draw.

#include "../Constants.h"

import Bindless.resource;
import SceneTypes;
import PerFrame.resource;
import Grid;                    // gridCellPos / gridFlattenCellIndex / sphereIntersectsGridCell

[numthreads(64, 1, 1)]
void binLights(uint3 dispatchThreadId: SV_DispatchThreadID)
{
    uint nPoint = gPerFrame.pointLightCount;
    uint nSpot  = gPerFrame.spotLightCount;
    uint total  = nPoint + nSpot;

    uint i = dispatchThreadId.x;
    if (i >= total)
        return;

    // Unified virtual index (matches Scene.sampleLightByIndex) + bounding sphere.
    // Both light types bin by the same intensity-derived reach (the spot's
    // angular cone ⊂ its radius-sphere, so the sphere bound is conservative).
    float3 center;
    float  intensity;
    if (i < nPoint)
    {
        PointLight pl = gPointLights[i];
        center = pl.position;  intensity = pl.intensity;
    }
    else
    {
        SpotLight sl = gSpotLights[i - nPoint];
        center = sl.position;  intensity = sl.intensity;
    }

    // Bin reach. The point/spot falloff (interface.slang) is a plain
    // inverse-square with NO window, so the reach cutoff lives only here.
    // reach² = intensity / cutoff (inlined from the former LightFalloff.lightReachSq).
    float radius = sqrt(intensity / kPointLightCutoff);
    if (radius <= 0.f)
        return;

    float3 camPos = gPerFrame.posW;

    // Cell-space AABB of the light's sphere (camera-relative). round() matches
    // gridCellPos so the box brackets every cell the sphere can touch.
    int3 lo = gridCellPos(center - radius.xxx, camPos, kLightGridUnit);
    int3 hi = gridCellPos(center + radius.xxx, camPos, kLightGridUnit);

    int half = int(kLightGridDim / 2u);
    lo = clamp(lo, int3(-half + 1), int3(half - 1));
    hi = clamp(hi, int3(-half + 1), int3(half - 1));

    for (int z = lo.z; z <= hi.z; ++z)
    for (int y = lo.y; y <= hi.y; ++y)
    for (int x = lo.x; x <= hi.x; ++x)
    {
        int3 cell = int3(x, y, z);
        if (!sphereIntersectsGridCell(center, radius, cell, camPos, kLightGridDim, kLightGridUnit))
            continue;

        uint flat = gridFlattenCellIndex(cell, kLightGridDim);
        uint slot;
        InterlockedAdd(gLightGridCount[flat], 1u, slot);
        if (slot < kLightsPerCellMax)
            gLightGridList[flat * kLightsPerCellMax + slot] = i;
    }
}
