// Grid.slang — camera-centered uniform world-grid helpers. Free functions
// parameterized by gridDim (cells per axis) and gridUnit (world units per cell),
// so the coarse light grid and decal grid can share the math without sharing a
// layout. Each grid passes its own kGridDim / kGridUnit; the flatten index is
// valid only against a buffer allocated for that grid's gridDim³ cells.
//
// Camera-centered: cell (0,0,0) brackets the camera; valid cells span
// [-gridDim/2 + 1, gridDim/2 - 1] per axis.

int3 gridCellPos(float3 posW, float3 cameraPosW, float gridUnit)
{
    return (int3)round((posW - cameraPosW) / gridUnit);
}

uint gridFlattenCellIndex(int3 cellPos, uint gridDim)
{
    uint3 u = cellPos + uint3(gridDim / 2);
    return (u.z * gridDim * gridDim) + (u.y * gridDim) + u.x;
}

// Inverse of gridFlattenCellIndex — map a flat cell index back to its signed
// cell position (camera-relative). Used by the per-cell light-grid gather.
int3 gridUnflattenCellIndex(uint flat, uint gridDim)
{
    uint3 u = uint3(flat % gridDim,
                    (flat / gridDim) % gridDim,
                    flat / (gridDim * gridDim));
    return int3(u) - int(gridDim / 2);
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

    // Squared compare — sphere/box overlap never needs the actual distance, and
    // this runs per cell in the grid builds, so avoid the sqrt that distance() emits.
    float3 d = close - center;
    return dot(d, d) < radius * radius;
}

// Sphere-vs-cone angular cull (Bart Wronski, "Cull that cone", 2017). Returns
// false only when the cell's bounding sphere is wholly outside the cone's
// angular wedge or wholly behind the apex — i.e. the spot cannot light the cell.
// The radial range is bounded separately by sphereIntersectsGridCell, so this
// adds only the angle/back-plane cull. apex = spot position, axis = unit forward,
// cosHalfAngle = cosOuterAngle = cos(FOV/2). Uses the cell BOUNDING SPHERE (which
// encloses the cell AABB) so it is conservative — never rejects a cell the cone
// reaches.
bool coneIntersectsGridCell(float3 apex, float3 axis, float cosHalfAngle,
                            int3 cellPos, float3 cameraPosW, uint gridDim, float gridUnit)
{
    if (!gridCellValid(cellPos, gridDim))
        return false;

    float3 cellCenter = cellPos * gridUnit + cameraPosW;
    float  cellR      = gridUnit * 0.8660254f;      // sqrt(3)/2 * side = bounding-sphere radius

    float  sinHalf    = sqrt(saturate(1.0f - cosHalfAngle * cosHalfAngle));
    float3 v          = cellCenter - apex;
    float  vDotA      = dot(v, axis);                            // axial distance
    float  perp       = sqrt(max(dot(v, v) - vDotA * vDotA, 0.0f)); // perp dist to axis
    float  distToSurf = cosHalfAngle * perp - vDotA * sinHalf;   // signed dist to cone surface

    bool   angleCull  = distToSurf > cellR;         // wholly outside the angular wedge
    bool   backCull   = vDotA < -cellR;             // wholly behind the apex
    return !(angleCull || backCull);
}
