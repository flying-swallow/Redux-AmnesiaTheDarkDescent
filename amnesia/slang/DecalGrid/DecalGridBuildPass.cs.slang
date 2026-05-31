// DecalGridBuildPass — bins clustered OOB decals into the coarse, camera-centered
// world-space grid that albedo_resolve uses to look up only the decals near a
// shaded pixel (instead of looping all of gDecals[]).
//
//   binDecals : append each decal index to every grid cell its bounding sphere
//               overlaps, capped at kDecalsPerCellMax. The per-cell count buffer
//               is zeroed by a vkCmdFillBuffer on the host before this runs.
//
// The decal grid is its own coarse, camera-centered world grid (kDecalGridDim /
// kDecalGridUnit), independent of the light grid but sharing the Grid.slang
// helpers (gridCellPos / gridFlattenCellIndex / sphereIntersectsGridCell) so
// binning here and the lookup in SurfelGIRenderPass can't diverge. Binning is
// conservative (over-inclusion is harmless — the box test in the lookup rejects
// misses; under-inclusion would drop a decal). Dispatched each frame after the
// decal array upload and before the albedo resolve in cHybridRenderer::Draw.

#include "../Constants.h"

import Bindless.resource;
import SceneTypes;
import PerFrame.resource;
import Grid;                    // gridCellPos / gridFlattenCellIndex / sphereIntersectsGridCell

[numthreads(64, 1, 1)]
void binDecals(uint3 dispatchThreadId: SV_DispatchThreadID)
{
    uint i = dispatchThreadId.x;
    if (i >= gPerFrame.decalCount)
        return;

    GpuDecal d = gDecals[i];
    if (d.radius <= 0.f)
        return;

    float3 camPos = gPerFrame.posW;

    // Cell-space AABB of the decal's bounding sphere (camera-relative).
    int3 lo = gridCellPos(d.center - d.radius.xxx, camPos, kDecalGridUnit);
    int3 hi = gridCellPos(d.center + d.radius.xxx, camPos, kDecalGridUnit);

    int half = int(kDecalGridDim / 2u);
    lo = clamp(lo, int3(-half + 1), int3(half - 1));
    hi = clamp(hi, int3(-half + 1), int3(half - 1));

    for (int z = lo.z; z <= hi.z; ++z)
    for (int y = lo.y; y <= hi.y; ++y)
    for (int x = lo.x; x <= hi.x; ++x)
    {
        int3 cell = int3(x, y, z);
        if (!sphereIntersectsGridCell(d.center, d.radius, cell, camPos, kDecalGridDim, kDecalGridUnit))
            continue;

        uint flat = gridFlattenCellIndex(cell, kDecalGridDim);
        uint slot;
        InterlockedAdd(gDecalGridCount[flat], 1u, slot);
        if (slot < kDecalsPerCellMax)
            gDecalGridList[flat * kDecalsPerCellMax + slot] = i;
    }
}
