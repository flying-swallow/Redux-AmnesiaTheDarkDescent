// LightGridBuildPass — builds the coarse, camera-centered world-space light
// grid used by the surfel ray-trace NEE and the MainCompositePass direct cull.
//
//   binLights : ONE THREAD PER CELL. Each cell walks every light type over its
//               stable SLOT CAPACITY (gPerFrame.{point,spot,area}LightCount are
//               host high-water slot counts, sparse — zeroed holes have radius 0
//               and are skipped) and appends the lights whose CPU-precomputed
//               reach-sphere overlaps the cell, capped at kLightsPerCellMax. The
//               grid stores the packed light id (packLightId(type, slot) — the
//               same value getLight() decodes), so a reservoir's persisted index
//               stays valid across frames. The cell owns its count + list slots,
//               so there are no atomics, no inter-thread contention, and the
//               per-cell count is written unconditionally (no host zero-clear).
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
// (downstream SpotLight.sample does the exact cone test).
//
// Dispatched each frame after the surfel cell scatter and before the surfel
// ray trace (Stage E) in cHybridRenderer::Draw.

#include "../Constants.h"

import bindless;
import SceneTypes;
import Scene;                   // packLightId / kLightType*
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
    uint   nPoint  = gPerFrame.pointLightCount;   // per-type slot capacities (sparse, zeroed holes)
    uint   nSpot   = gPerFrame.spotLightCount;
    uint   nArea   = gPerFrame.areaLightCount;

    uint count = 0u;

    // Point lights — omnidirectional, reach sphere is exact.
    for (uint p = 0u; p < nPoint; ++p)
    {
        PointLight pl = gPointLights[p];
        if (pl.radius <= 0.f)   // 0 ⇒ invisible / dim-to-skip / unused slot hole
            continue;
        if (!sphereIntersectsGridCell(pl.position, pl.radius, cellPos, camPos, kLightGridDim, kLightGridUnit))
            continue;
        if (count < kLightsPerCellMax)
            gLightGridList[flat * kLightsPerCellMax + count] = packLightId(kLightTypePoint, p);
        ++count;
    }

    // Spot lights — reach sphere + angular cone wedge cull.
    for (uint s = 0u; s < nSpot; ++s)
    {
        SpotLight sl = gSpotLights[s];
        if (sl.radius <= 0.f)
            continue;
        if (!sphereIntersectsGridCell(sl.position, sl.radius, cellPos, camPos, kLightGridDim, kLightGridUnit))
            continue;
        if (!coneIntersectsGridCell(sl.position, sl.direction, sl.cosOuterAngle, cellPos, camPos, kLightGridDim, kLightGridUnit))
            continue;
        if (count < kLightsPerCellMax)
            gLightGridList[flat * kLightsPerCellMax + count] = packLightId(kLightTypeSpot, s);
        ++count;
    }

    // Area lights — planar emitters; the reach sphere is a conservative bound
    // (the exact rect + barn-door cull stays in RectLight.sample).
    for (uint a = 0u; a < nArea; ++a)
    {
        RectLight al = gAreaLights[a];
        if (al.radius <= 0.f)
            continue;
        if (!sphereIntersectsGridCell(al.position, al.radius, cellPos, camPos, kLightGridDim, kLightGridUnit))
            continue;
        if (count < kLightsPerCellMax)
            gLightGridList[flat * kLightsPerCellMax + count] = packLightId(kLightTypeArea, a);
        ++count;
    }

    gLightGridCount[flat] = min(count, kLightsPerCellMax);          // every cell written → no host clear
}
