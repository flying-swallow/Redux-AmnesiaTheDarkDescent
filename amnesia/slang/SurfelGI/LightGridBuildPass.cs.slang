// LightGridBuildPass — builds the coarse, camera-centered world-space light
// grid used by the surfel ray-trace NEE and the SurfelGIRenderPass direct cull.
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
// inner loop has no sqrt. Binning stays conservative; box lights aren't binned
// (NEE handles them separately).
//
// Dispatched each frame after the surfel cell scatter and before the surfel
// ray trace (Stage E) in cHybridRenderer::Draw.

#include "../Constants.h"

import Bindless.resource;
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
        float3 center;
        float  radius;
        if (i < nPoint)
        {
            PointLight pl = gPointLights[i];
            center = pl.position;  radius = pl.radius;
        }
        else
        {
            SpotLight sl = gSpotLights[i - nPoint];
            center = sl.position;  radius = sl.radius;
        }
        if (radius <= 0.f)
            continue;

        if (!sphereIntersectsGridCell(center, radius, cellPos, camPos, kLightGridDim, kLightGridUnit))
            continue;

        if (count < kLightsPerCellMax)
            gLightGridList[flat * kLightsPerCellMax + count] = i;   // deterministic: first N by index
        ++count;
    }

    gLightGridCount[flat] = min(count, kLightsPerCellMax);          // every cell written → no host clear
}
