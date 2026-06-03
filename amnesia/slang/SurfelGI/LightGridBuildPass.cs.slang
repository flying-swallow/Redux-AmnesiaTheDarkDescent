// LightGridBuildPass — builds the coarse, camera-centered world-space light
// grid used by the surfel ray-trace NEE and the MainCompositePass direct cull.
//
//   binLights : ONE THREAD PER CELL. Each cell walks the unified light list
//               (point [0,nPoint), spot [nPoint, nPoint+nSpot) — same index space
//               as Scene.sampleLightByIndex) and appends the lights whose
//               CPU-precomputed reach-sphere overlaps the cell, capped at
//               kLightsPerCellMax. The cell owns its count + list slots, so there
//               are no atomics, no inter-thread contention, and the per-cell count
//               is written unconditionally (no host zero-clear needed).
//
// Per-cell gather (vs the old per-light scatter): work is uniform across threads
// — a single large light can't stall a whole wave — and the cap drops the
// deterministic tail (first N lights by index) instead of a nondeterministic
// atomic-order subset. The light reach is precomputed on the host
// (pl/sl.radius), and the sphere/cell test compares squared distance, so the
// inner loop has no sqrt. Point lights are omnidirectional so the reach sphere is
// exact; spotlights additionally get a cone-vs-cell angular cull
// (coneIntersectsGridCell) so a narrow cone bins only into the cells it can
// actually reach instead of its whole bounding sphere. Binning stays conservative
// (downstream SpotLight.sample does the exact cone test); box lights aren't binned
// (NEE handles them separately).
//
// Dispatched each frame after the surfel cell scatter and before the surfel
// ray trace (Stage E) in cHybridRenderer::Draw.

#include "../Constants.h"

import bindless;
import SceneTypes;
import PerFrame.resource;
import Grid;                    // gridUnflattenCellIndex / sphereIntersectsGridCell

[numthreads(64, 1, 1)]
void binLights(uint3 dispatchThreadId: SV_DispatchThreadID)
{
    uint flat = dispatchThreadId.x;
    if (flat >= kLightGridCellCount)
        return;

    int3   cellPos = gridUnflattenCellIndex(flat, kLightGridDim);
    float3 camPos  = gPerFrame.posW;
    uint   nPoint  = gPerFrame.pointLightCount;
    uint   total   = nPoint + gPerFrame.spotLightCount;

    uint count = 0u;
    for (uint i = 0u; i < total; ++i)
    {
        // Unified index → point/spot. radius is the host-precomputed bin reach
        // (0 ⇒ the light was too dim to matter anywhere; skip).
        bool   isSpot = (i >= nPoint);
        float3 center;
        float  radius;
        float3 spotAxis = float3(0.f, 0.f, 1.f);
        float  spotCos  = -1.f;
        if (!isSpot)
        {
            PointLight pl = gPointLights[i];
            center = pl.position;  radius = pl.radius;
        }
        else
        {
            SpotLight sl = gSpotLights[i - nPoint];
            center = sl.position;  radius = sl.radius;
            spotAxis = sl.direction;  spotCos = sl.cosOuterAngle;
        }
        if (radius <= 0.f)
            continue;

        // Radial reach cap (tight cell-AABB sphere test) — applies to all lights.
        if (!sphereIntersectsGridCell(center, radius, cellPos, camPos, kLightGridDim, kLightGridUnit))
            continue;

        // Spotlights: reject cells outside the cone's angular wedge. Point lights
        // are omnidirectional, so the sphere above is already exact for them.
        if (isSpot &&
            !coneIntersectsGridCell(center, spotAxis, spotCos, cellPos, camPos, kLightGridDim, kLightGridUnit))
            continue;

        if (count < kLightsPerCellMax)
            gLightGridList[flat * kLightsPerCellMax + count] = i;   // deterministic: first N by index
        ++count;
    }

    gLightGridCount[flat] = min(count, kLightsPerCellMax);          // every cell written → no host clear
}
