#pragma once

#include "HostDefinitions.h"

HOST_NAMESPACE_BEGIN

SLANG_PUBLIC inline float lightGridAbs(float value)
{
    return value < 0.0f ? -value : value;
}

// Maximum inward-plane distance over an axis-aligned cubic cell. Keep cells
// touching a plane, including roundoff from translated projection matrices.
// Scalar arithmetic is shared with headless C++ geometry regression tests.
SLANG_PUBLIC inline bool lightGridPlaneIntersectsBox(float4 plane, float3 center, float halfSize)
{
    float nx = lightGridAbs(plane.x);
    float ny = lightGridAbs(plane.y);
    float nz = lightGridAbs(plane.z);
    float support = halfSize * (nx + ny + nz);
    float upper = plane.x * center.x + plane.y * center.y + plane.z * center.z
                + plane.w + support;
    float tolerance = 1.0e-5f * (nx * (lightGridAbs(center.x) + 1.0f)
                              + ny * (lightGridAbs(center.y) + 1.0f)
                              + nz * (lightGridAbs(center.z) + 1.0f)
                              + lightGridAbs(plane.w) + support);
    return upper >= -tolerance;
}

// Rows of the world-to-texture projection used by SpotLight.sample. Its
// support is w > 0, 0 <= x <= w, 0 <= y <= w. Near/far clip planes are NOT
// tested by shading; radial reach is handled separately by the grid builder.
SLANG_PUBLIC inline bool lightGridProjectionIntersectsBox(
    float4 rowX, float4 rowY, float4 rowW, float3 center, float halfSize)
{
    float4 right = float4(rowW.x - rowX.x, rowW.y - rowX.y,
                          rowW.z - rowX.z, rowW.w - rowX.w);
    float4 top = float4(rowW.x - rowY.x, rowW.y - rowY.y,
                        rowW.z - rowY.z, rowW.w - rowY.w);
    return lightGridPlaneIntersectsBox(rowW, center, halfSize)
        && lightGridPlaneIntersectsBox(rowX, center, halfSize)
        && lightGridPlaneIntersectsBox(right, center, halfSize)
        && lightGridPlaneIntersectsBox(rowY, center, halfSize)
        && lightGridPlaneIntersectsBox(top, center, halfSize);
}

HOST_NAMESPACE_END
