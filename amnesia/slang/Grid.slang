// Grid.slang — camera-centered uniform world-grid helpers. Free functions
// parameterized by gridDim (cells per axis) and gridUnit (world units per cell),
// so the coarse light grid and decal grid can share the math without sharing a
// layout. Each grid passes its own kGridDim / kGridUnit; the flatten index is
// valid only against a buffer allocated for that grid's gridDim³ cells.
//
// Camera-centered: cell (0,0,0) brackets the camera; valid cells span
// [-gridDim/2 + 1, gridDim/2 - 1] per axis (same convention as the surfel grid).

int3 gridCellPos(float3 posW, float3 cameraPosW, float gridUnit)
{
    return (int3)round((posW - cameraPosW) / gridUnit);
}

uint gridFlattenCellIndex(int3 cellPos, uint gridDim)
{
    uint3 u = cellPos + uint3(gridDim / 2);
    return (u.z * gridDim * gridDim) + (u.y * gridDim) + u.x;
}

bool gridCellValid(int3 cellPos, uint gridDim)
{
    return abs(cellPos.x) < int(gridDim / 2)
        && abs(cellPos.y) < int(gridDim / 2)
        && abs(cellPos.z) < int(gridDim / 2);
}

bool sphereIntersectsGridCell(float3 center, float radius, int3 cellPos,
                              float3 cameraPosW, uint gridDim, float gridUnit)
{
    if (!gridCellValid(cellPos, gridDim))
        return false;

    float3 half3   = float3(gridUnit, gridUnit, gridUnit) / 2.0f;
    float3 minPosW = cellPos * gridUnit - half3 + cameraPosW;
    float3 maxPosW = cellPos * gridUnit + half3 + cameraPosW;
    float3 close   = min(max(center, minPosW), maxPosW);

    return distance(close, center) < radius;
}
