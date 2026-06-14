// Hand-written Metal port of amnesia/slang/SurfelGI/LightGridBuildPass.cs.slang
//
// binLights: ONE THREAD PER CELL of the coarse, camera-centered world-space
// light grid. Each thread walks the unified light list (point [0,nPoint), spot
// [nPoint, nPoint+nSpot)) and appends the lights whose host-precomputed reach
// sphere overlaps its cell, capped at kLightsPerCellMax. The cell owns its count
// + list slots, so there are no atomics and the count is written
// unconditionally (no host zero-clear).
//
// Self-contained (the runtime compiles this text via newLibrary with no include
// paths). The set-0 bindless globals are read through a Metal argument buffer
// (struct Set0) bound at buffer(0) by bindExternalSet; its [[id(N)]] indices
// match the Vulkan kBinding* numbers (amnesia/slang/Constants.h) that the
// RIBindlessDescriptorSet argument encoder fills. Program-managed gPerFrame is a
// direct binding at buffer(1) (bindDescriptors).
//   set0 (bindless arg buffer) buffer(0)   gPerFrame buffer(1)
//   set0.gPointLights id(22)  set0.gSpotLights id(29)
//   set0.gLightGridList id(41)  set0.gLightGridCount id(40)

#include <metal_stdlib>
using namespace metal;

// --- Constants (amnesia/slang/Constants.h, resolved) ------------------------
constant uint  kLightGridDim       = 128u;
constant uint  kLightGridCellCount = 2097152u;          // 128^3
constant float kLightGridUnit      = 0.9765625f;        // world units per cell
constant uint  kLightsPerCellMax   = 128u;

// --- Shared struct/util layouts: mirrored from the Slang modules (flattened
// into this file at stage time by cmake/flatten_metal_includes.py).
#include "SceneTypes.metalh"   // SceneConstants, PointLight, SpotLight, MatStorage
#include "Grid.metalh"         // gridUnflattenCellIndex / *IntersectsGridCell / gridCellValid


// Set-0 bindless argument buffer (subset this kernel reads). [[id]] = kBinding*.
struct Set0 {
    device PointLight* gPointLights    [[id(22)]];   // kBindingPointLights
    device SpotLight*  gSpotLights     [[id(29)]];   // kBindingSpotLights
    device uint*       gLightGridCount [[id(40)]];   // kBindingLightGridCount
    device uint*       gLightGridList  [[id(41)]];   // kBindingLightGridList
};
// Set-1 program argument buffer (buffer(1)); [[id]] = VK binding within set 1.
struct Set1 { constant SceneConstants* gPerFrame [[id(0)]]; };

[[kernel]] void binLights(uint3 dispatchThreadId [[thread_position_in_grid]],
                          device Set0& set0 [[buffer(0)]],
                          device Set1& set1 [[buffer(1)]]) {
    constant SceneConstants* gPerFrame = set1.gPerFrame;
    uint flat = dispatchThreadId.x;
    if (flat >= kLightGridCellCount)
        return;

    int3   cellPos = gridUnflattenCellIndex(flat, kLightGridDim);
    float3 camPos  = gPerFrame->posW;
    uint   nPoint  = gPerFrame->pointLightCount;
    uint   total   = nPoint + gPerFrame->spotLightCount;

    uint count = 0u;
    for (uint i = 0u; i < total; ++i) {
        // Unified index -> point/spot. radius is the host-precomputed bin reach
        // (0 => light too dim to matter anywhere; skip).
        bool   isSpot = (i >= nPoint);
        float3 center;
        float  radius;
        float3 spotAxis = float3(0.0f, 0.0f, 1.0f);
        float  spotCos  = -1.0f;
        if (!isSpot) {
            device PointLight& pl = set0.gPointLights[i];
            center = pl.position;  radius = pl.radius;
        } else {
            device SpotLight& sl = set0.gSpotLights[i - nPoint];
            center = sl.position;  radius = sl.radius;
            spotAxis = sl.direction;  spotCos = sl.cosOuterAngle;
        }
        if (radius <= 0.0f)
            continue;

        if (!sphereIntersectsGridCell(center, radius, cellPos, camPos, kLightGridDim, kLightGridUnit))
            continue;

        // Spotlights: reject cells outside the cone's angular wedge. Point
        // lights are omnidirectional, so the sphere above is already exact.
        if (isSpot &&
            !coneIntersectsGridCell(center, spotAxis, spotCos, cellPos, camPos, kLightGridDim, kLightGridUnit))
            continue;

        if (count < kLightsPerCellMax)
            set0.gLightGridList[flat * kLightsPerCellMax + count] = i;  // deterministic: first N by index
        ++count;
    }

    set0.gLightGridCount[flat] = min(count, kLightsPerCellMax);         // every cell written -> no host clear
}
