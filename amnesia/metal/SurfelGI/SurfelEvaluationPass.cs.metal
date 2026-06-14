// Hand-written Metal port of amnesia/slang/SurfelGI/SurfelEvaluationPass.cs.slang
//
// Per-pixel surfel-GI evaluation (numthreads 16,16,1). For each pixel of
// gPackedHitInfo that hit geometry, fetch the vertex, look up the surfel cell,
// gather covering surfels and write indirect lighting (or a diagnostic overlay)
// into gOutput.
//
// Self-contained; scalar struct layouts (packed_float3). Bindless geometry is
// fetched through buffer-device-address handles (ulong -> device*) — the C++
// side stores MTL gpuAddress in gOpaque*Handles and keeps the geometry resident.
//
// Bindings:
//   set0 (bindless arg buffer) buffer(0); [[id]] = kBinding* (Constants.h):
//     gOpaquePositionHandles 3  gOpaqueTangentHandles 4  gOpaqueNormalHandles 5
//     gOpaqueUv0Handles 6  gOpaqueIndexHandles 8  gSurfelBuffer 11
//     gSurfelRecycle 16  gCellInfo 18  gCellToSurfel 19  gSceneObjects 20
//     gSurfelRefCounter 27  gSurfelDepthSampler 35
//   gPerFrame buffer(1)
//   gPackedHitInfo texture(0)[uint,read]  gSurfelDepth texture(1)[sample]
//   gOutput texture(2)[read_write]
#include <metal_stdlib>
using namespace metal;

constant float kCellUnit      = 0.5f;   // world units per cell (used by lookupSurfelCell)
constant float kDefaultVarianceSensitivity = 40.0f;
constant uint  kDefaultBlendingDelay       = 30u;
constant uint  kDefaultOverlayMode         = 0u;
constant float kDefaultPlacementThreshold  = 2.0f;
constant float kDefaultRemovalThreshold    = 4.0f;

// --- Scalar struct layouts (packed_float3) ----------------------------------
// --- Shared struct layouts + utilities: mirrored from the Slang modules
// (flattened into this file at stage time by cmake/flatten_metal_includes.py).
#include "SceneTypes.metalh"            // SceneConstants, MatStorage, UniformObject, HitInfo, TriangleHit, VertexData, lights
#include "SurfelGI/SurfelTypes.metalh"  // Surfel, CellInfo, SurfelRayResult, SurfelRecycleInfo, SurfelBounds (+ MSMEData)
#include "SurfelGI/SurfelUtils.metalh"  // cell math, octEncode, get_tangentspace, getSurfelDepthUV, lerp/stepColor (+ kCellDimension, depth-atlas constants)

// Set-0 bindless argument buffer (subset this kernel reads).
struct Set0 {
    device ulong*             gOpaquePositionHandles [[id(3)]];
    device ulong*             gOpaqueTangentHandles  [[id(4)]];
    device ulong*             gOpaqueNormalHandles   [[id(5)]];
    device ulong*             gOpaqueUv0Handles      [[id(6)]];
    device ulong*             gOpaqueIndexHandles    [[id(8)]];
    device Surfel*            gSurfelBuffer          [[id(11)]];
    device SurfelRecycleInfo* gSurfelRecycleInfoBuffer [[id(16)]];
    device CellInfo*          gCellInfoBuffer        [[id(18)]];
    device uint*              gCellToSurfelBuffer    [[id(19)]];
    device UniformObject*     gSceneObjects          [[id(20)]];
    device uint*              gSurfelRefCounter      [[id(27)]];
    sampler                   gSurfelDepthSampler    [[id(35)]];
};
// Set-1 / Set-2 program argument buffers; [[id]] = VK binding within the set.
struct Set1 {
    constant SceneConstants* gPerFrame [[id(0)]];
    texture2d<uint,  access::read>   gPackedHitInfo [[id(1)]];
    texture2d<float, access::sample> gSurfelDepth   [[id(2)]];
};
struct Set2 { texture2d<float, access::read_write> gOutput [[id(0)]]; };

// Utility headers that reference `device Set0&` must come AFTER struct Set0.
#include "SurfelGI/BindlessTriangle.metalh"  // hitIsValid, getTriangleHit, loadMat, fetchBindlessTriangle, getVertexData
#include "SurfelGI/SurfelGather.metalh"      // SurfelHit, testSurfelCoverage, surfelKernelWeight, surfelDepthWeight, lookupSurfelCell

[[kernel]] void csMain(uint3 dispatchThreadId [[thread_position_in_grid]],
                       device Set0& set0 [[buffer(0)]],
                       device Set1& set1 [[buffer(1)]],
                       device Set2& set2 [[buffer(2)]]) {
    constant SceneConstants* gPerFrame = set1.gPerFrame;
    texture2d<uint,  access::read>   gPackedHitInfo = set1.gPackedHitInfo;
    texture2d<float, access::sample> gSurfelDepth   = set1.gSurfelDepth;
    texture2d<float, access::read_write> gOutput    = set2.gOutput;
    uint2 pixelPos = dispatchThreadId.xy;

    HitInfo hitInfo;
    hitInfo.packed = gPackedHitInfo.read(pixelPos);
    if (!hitIsValid(hitInfo))
        return;

    TriangleHit triangleHit = getTriangleHit(hitInfo);
    VertexData v = getVertexData(triangleHit, set0);

    uint flattenIndex;
    CellInfo cellInfo;
    if (!lookupSurfelCell(v.posW, gPerFrame->posW, flattenIndex, cellInfo, set0.gCellInfoBuffer))
        return;

    float4 indirectLighting = float4(0.0f);
    float coverage = 0.0f, varianceEx = 0.0f, rayCountEx = 0.0f, maxVariance = 0.0f;
    uint refCount = 0u, life = 0u;

    for (uint i = 0u; i < cellInfo.surfelCount; ++i) {
        uint surfelIndex = set0.gCellToSurfelBuffer[cellInfo.cellToSurfelBufferOffset + i];
        Surfel surfel = set0.gSurfelBuffer[surfelIndex];

        float3 normal = normalize(float3(surfel.normal));
        SurfelHit hit;
        if (!testSurfelCoverage(v.posW, v.normalW, float3(surfel.position), normal, surfel.radius, hit))
            continue;

        float contribution = surfelKernelWeight(hit.dotN, hit.dist, surfel.radius);
        coverage += contribution;
        contribution *= surfelDepthWeight(surfelIndex, hit.dirUnit, normal, hit.dist, gSurfelDepth, set0.gSurfelDepthSampler);

        SurfelRecycleInfo recycleInfo = set0.gSurfelRecycleInfoBuffer[surfelIndex];
        float blendDelayT = smoothstep(0.0f, float(kDefaultBlendingDelay), float(recycleInfo.frame));
        indirectLighting += float4(float3(surfel.radiance), 1.0f) * contribution * blendDelayT;

        float vlen = length(float3(surfel.msmeData.variance));
        varianceEx += vlen * contribution;
        rayCountEx += float(surfel.rayCount) * contribution;
        refCount = max(refCount, set0.gSurfelRefCounter[surfelIndex]);
        life     = max(life, uint(recycleInfo.life));
        maxVariance = max(maxVariance, vlen);
    }

    if (indirectLighting.w > 0.0f) {
        indirectLighting.xyz /= indirectLighting.w;
        indirectLighting.w = saturate(indirectLighting.w);
        varianceEx /= indirectLighting.w;
        rayCountEx /= indirectLighting.w;

        float4 outColor = indirectLighting;
        if (kDefaultOverlayMode == 1u)
            outColor = float4(stepColor(maxVariance * kDefaultVarianceSensitivity, 0.8f, 0.5f), 1.0f);
        else if (kDefaultOverlayMode == 2u)
            outColor = float4(stepColor(rayCountEx, 48.0f, 32.0f), 1.0f);
        else if (kDefaultOverlayMode == 3u)
            outColor = float4(lerpColor(float(refCount) / 256.0f), 1.0f);
        else if (kDefaultOverlayMode == 4u)
            outColor = float4(step(float(life), 0.0f), step(1.0f, float(life)), 0.0f, 1.0f);
        else if (kDefaultOverlayMode == 5u)
            outColor = float4(lerpColor(smoothstep(kDefaultPlacementThreshold, kDefaultRemovalThreshold, coverage)), 1.0f);
        gOutput.write(outColor, pixelPos);
    }
}
