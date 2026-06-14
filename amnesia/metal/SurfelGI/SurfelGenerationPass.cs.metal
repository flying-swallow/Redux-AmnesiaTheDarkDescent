// Hand-written Metal port of amnesia/slang/SurfelGI/SurfelGenerationPass.cs.slang
//
// Per-pixel surfel placement + indirect-lighting composite (numthreads 16,16,1).
// Reads the V-buffer, gathers covering surfels, accumulates smoothed indirect
// lighting into gOutput, and runs a tile-wide election to probabilistically
// spawn / remove a surfel based on coverage thresholds.
//
// Self-contained; scalar struct layouts (packed types). Set-0 globals come
// through one Set0 argument buffer (buffer(0), [[id]] = kBinding*). The
// no-early-return structure is preserved exactly (divergent threadgroup
// barriers hang some GPUs).
//
// Bindings: set0 buffer(0); gPerFrame buffer(1); gPackedHitInfo texture(0)[uint,
// read]; gSurfelDepth texture(1)[sample]; gOutput texture(2)[read_write].
#include <metal_stdlib>
using namespace metal;

constant float kCellUnit      = 0.5f;
constant float kSurfelTargetArea = 50000.0f;
constant uint  kPerCellSurfelLimit = 1024u;
constant float kDefaultVarianceSensitivity = 40.0f;
constant uint  kDefaultBlendingDelay = 30u;
constant uint  kDefaultOverlayMode = 0u;
constant float kDefaultPlacementThreshold = 2.0f;
constant float kDefaultRemovalThreshold   = 4.0f;
constant float kDefaultChanceMultiply = 0.3f;
constant uint  kDefaultChancePower    = 1u;
constant uint  kSurfelCounterValid = 0u, kSurfelCounterFree = 2u;

// --- Shared struct layouts + utilities: mirrored from the Slang modules
// (flattened into this file at stage time by cmake/flatten_metal_includes.py).
#include "SceneTypes.metalh"            // SceneConstants, MatStorage, UniformObject, HitInfo, TriangleHit, VertexData, lights
#include "SurfelGI/SurfelTypes.metalh"  // Surfel, CellInfo, SurfelRayResult, SurfelRecycleInfo, SurfelBounds (+ MSMEData)
#include "Random.metalh"                // RNG + rng_* (verbatim from Random.slang lowering)
#include "SurfelGI/SurfelUtils.metalh"  // cell math, octEncode, get_tangentspace, getSurfelDepthUV, calcSurfelRadius, lerp/stepColor (+ M_PI_, kCellDimension, depth-atlas constants)

struct Set0 {
    device ulong*          gOpaquePositionHandles [[id(3)]];
    device ulong*          gOpaqueTangentHandles  [[id(4)]];
    device ulong*          gOpaqueNormalHandles   [[id(5)]];
    device ulong*          gOpaqueUv0Handles      [[id(6)]];
    device ulong*          gOpaqueIndexHandles    [[id(8)]];
    device uint*           gSurfelCounter         [[id(10)]];
    device Surfel*         gSurfelBuffer          [[id(11)]];
    device uint4*          gSurfelGeometryBuffer  [[id(12)]];
    device uint*           gSurfelValidIndexBuffer [[id(13)]];
    device uint*           gSurfelFreeIndexBuffer [[id(15)]];
    device SurfelRecycleInfo* gSurfelRecycleInfoBuffer [[id(16)]];
    device CellInfo*       gCellInfoBuffer        [[id(18)]];
    device uint*           gCellToSurfelBuffer    [[id(19)]];
    device UniformObject*  gSceneObjects          [[id(20)]];
    device uint*           gSurfelRefCounter      [[id(27)]];
    sampler                gSurfelDepthSampler    [[id(35)]];
    device SurfelBounds*   gSurfelBoundsBuffer    [[id(37)]];
    device uint*           gBindlessSlotGeneration [[id(38)]];
    device uint*           gSurfelSlotGeneration  [[id(39)]];
};
// Set-1 / Set-2 program argument buffers; [[id]] = VK binding within the set.
struct Set1 {
    constant SceneConstants* gPerFrame [[id(0)]];
    texture2d<uint,  access::read>   gPackedHitInfo [[id(1)]];
    texture2d<float, access::sample> gSurfelDepth   [[id(2)]];
};
struct Set2 { texture2d<float, access::read_write> gOutput [[id(0)]]; };

static uint  f32tof16(float x) { return uint(as_type<ushort>(half(x))); }
static float f16tof32(uint x) { return float(as_type<half>(ushort(x & 0xFFFFu))); }

// Utility headers that reference `device Set0&` must come AFTER struct Set0.
#include "SurfelGI/BindlessTriangle.metalh"  // TriVtx, loadMat, getVertexData, getTriangleHit
#include "SurfelGI/SurfelGather.metalh"      // SurfelHit, testSurfelCoverage, surfelKernelWeight, surfelDepthWeight
#include "SurfelGI/SurfelLifecycle.metalh"   // makeMSMEData, makeSurfel, tryAllocateSurfelSlot, publishNewSurfel (+ kTotalSurfelLimit, kObjectSlotCapacity, kMaxLife)

[[kernel]] void csMain(uint3 dispatchThreadId [[thread_position_in_grid]],
                       uint  groupIndex    [[thread_index_in_threadgroup]],
                       uint3 groupThreadID [[thread_position_in_threadgroup]],
                       uint3 groupdId      [[threadgroup_position_in_grid]],
                       device Set0& set0 [[buffer(0)]],
                       device Set1& set1 [[buffer(1)]],
                       device Set2& set2 [[buffer(2)]]) {
    constant SceneConstants* gPerFrame = set1.gPerFrame;
    texture2d<uint,  access::read>   gPackedHitInfo = set1.gPackedHitInfo;
    texture2d<float, access::sample> gSurfelDepth   = set1.gSurfelDepth;
    texture2d<float, access::read_write> gOutput    = set2.gOutput;
    threadgroup atomic_uint groupShareMinCoverage;
    threadgroup atomic_uint groupShareMaxContribution;
    if (groupIndex == 0u) {
        atomic_store_explicit(&groupShareMinCoverage, ~0u, memory_order_relaxed);
        atomic_store_explicit(&groupShareMaxContribution, 0u, memory_order_relaxed);
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    // No early return: off-screen / invalid threads stay resident so every
    // thread reaches the post-loop barrier (divergent barriers hang some GPUs).
    uint2 resolution = uint2(gPerFrame->viewportSize);
    bool  active = (dispatchThreadId.x < resolution.x && dispatchThreadId.y < resolution.y);

    uint2 pixelPos   = dispatchThreadId.xy;
    float3 cameraPos = gPerFrame->posW;
    float  fovy      = gPerFrame->cameraFov;

    RNG randomState;
    rng_init(randomState, pixelPos, gPerFrame->totalFrames);

    HitInfo  hitInfo; hitInfo.packed = uint4(0u);
    VertexData v = {};
    float      depth = 0.0f;
    CellInfo   cellInfo = {0u, 0u};

    if (active) {
        hitInfo.packed = gPackedHitInfo.read(pixelPos);
        if (!any(hitInfo.packed != uint4(0u))) {
            active = false;
        } else {
            TriangleHit triangleHit = getTriangleHit(hitInfo.packed);
            v = getVertexData(triangleHit, set0);
            float kPlacementDepthRange = max(1e-3f, float(kCellDimension / 2u) * kCellUnit);
            depth = saturate(distance(cameraPos, v.posW) / kPlacementDepthRange);
            int3 cellPos = getCellPos(v.posW, cameraPos, kCellUnit);
            if (!isCellValid(cellPos)) active = false;
            else cellInfo = set0.gCellInfoBuffer[getFlattenCellIndex(cellPos)];
        }
    }

    float coverage = 0.0f, varianceEx = 0.0f, rayCountEx = 0.0f, maxVariance = 0.0f, maxContribution = 0.0f;
    uint  refCount = 0u, life = 0u;
    uint  maxContributionSurfelIndex = rng_next_uint(randomState, cellInfo.surfelCount);
    float4 indirectLighting = float4(0.0f);

    if (active) {
        for (uint i = 0u; i < cellInfo.surfelCount; ++i) {
            uint surfelIndex = set0.gCellToSurfelBuffer[cellInfo.cellToSurfelBufferOffset + i];
            SurfelBounds sb = set0.gSurfelBoundsBuffer[surfelIndex];

            SurfelHit hit;
            if (testSurfelCoverage(v.posW, v.normalW, float3(sb.position), float3(sb.normal), sb.radius, hit)) {
                SurfelRecycleInfo recycle = set0.gSurfelRecycleInfoBuffer[surfelIndex];
                bool isSleeping = (recycle.status & 0x0001u) != 0u;
                bool lastSeen   = (recycle.status & 0x0002u) != 0u;

                if (!isSleeping) {
                    Surfel surfel = set0.gSurfelBuffer[surfelIndex];
                    float contribution = surfelKernelWeight(hit.dotN, hit.dist, sb.radius);
                    coverage += contribution;
                    contribution *= surfelDepthWeight(surfelIndex, hit.dirUnit, float3(sb.normal), hit.dist, gSurfelDepth, set0.gSurfelDepthSampler);

                    indirectLighting += float4(float3(surfel.radiance), 1.0f) * contribution
                                        * smoothstep(0.0f, float(kDefaultBlendingDelay), float(recycle.frame));
                    float varLen = length(float3(surfel.msmeData.variance));
                    varianceEx += varLen * contribution;
                    rayCountEx += float(surfel.rayCount) * contribution;
                    refCount = max(refCount, set0.gSurfelRefCounter[surfelIndex]);
                    life = max(life, uint(recycle.life));
                    if (maxContribution < contribution) { maxContribution = contribution; maxContributionSurfelIndex = i; }
                    maxVariance = max(maxVariance, varLen);
                }
                if (!lastSeen)
                    set0.gSurfelRecycleInfoBuffer[surfelIndex].status |= ushort(0x0002);
            }
        }

        if (indirectLighting.w > 0.0f) {
            indirectLighting.xyz /= indirectLighting.w;
            indirectLighting.w = saturate(indirectLighting.w);
            varianceEx /= indirectLighting.w;
            rayCountEx /= indirectLighting.w;
            float4 outColor = indirectLighting;
            if      (kDefaultOverlayMode == 1u) outColor = float4(stepColor(maxVariance * kDefaultVarianceSensitivity, 0.8f, 0.5f), 1.0f);
            else if (kDefaultOverlayMode == 2u) outColor = float4(stepColor(rayCountEx, 48.0f, 32.0f), 1.0f);
            else if (kDefaultOverlayMode == 3u) outColor = float4(lerpColor(float(refCount) / 256.0f), 1.0f);
            else if (kDefaultOverlayMode == 4u) outColor = float4(step(float(life), 0.0f), step(1.0f, float(life)), 0.0f, 1.0f);
            else if (kDefaultOverlayMode == 5u) outColor = float4(lerpColor(smoothstep(kDefaultPlacementThreshold, kDefaultRemovalThreshold, coverage)), 1.0f);
            gOutput.write(outColor, pixelPos);
        }

        uint covData = 0u;
        covData |= ((f32tof16(coverage) & 0x0000FFFFu) << 16);
        covData |= ((rng_next_uint(randomState, 255u) & 0x000000FFu) << 8);
        covData |= ((groupThreadID.x & 0x0000000Fu) << 4);
        covData |= ((groupThreadID.y & 0x0000000Fu) << 0);
        atomic_fetch_min_explicit(&groupShareMinCoverage, covData, memory_order_relaxed);

        uint contribData = 0u;
        contribData |= ((f32tof16(maxContribution) & 0x0000FFFFu) << 16);
        contribData |= ((maxContributionSurfelIndex & 0x0000FFFFu) << 0);
        atomic_fetch_max_explicit(&groupShareMaxContribution, contribData, memory_order_relaxed);
    }

    threadgroup_barrier(mem_flags::mem_threadgroup);

    uint  electedData = atomic_load_explicit(&groupShareMinCoverage, memory_order_relaxed);
    float electedCoverage = f16tof32((electedData & 0xFFFF0000u) >> 16);
    uint  x = (electedData & 0x000000F0u) >> 4;
    uint  y = (electedData & 0x0000000Fu) >> 0;

    if (cellInfo.surfelCount < kPerCellSurfelLimit) {
        if (groupThreadID.x == x && groupThreadID.y == y) {
            if (electedCoverage <= kDefaultPlacementThreshold && hitInfo.packed.x < kObjectSlotCapacity) {
                float chance = pow(depth, float(kDefaultChancePower));
                if (rng_next_float(randomState) < chance * kDefaultChanceMultiply) {
                    uint newIndex, validSlot;
                    if (tryAllocateSurfelSlot(newIndex, validSlot, set0)) {
                        float varRadius = calcSurfelRadius(distance(cameraPos, v.posW), fovy, resolution, kSurfelTargetArea, kCellUnit);
                        Surfel newSurfel = makeSurfel(v.posW, v.normalW, varRadius);
                        newSurfel.radiance = indirectLighting.xyz;
                        newSurfel.msmeData.mean = indirectLighting.xyz;
                        newSurfel.msmeData.shortMean = indirectLighting.xyz;
                        SurfelRecycleInfo recycle = SurfelRecycleInfo{ ushort(kMaxLife), ushort(0), ushort(0) };
                        publishNewSurfel(newIndex, validSlot, newSurfel, recycle, hitInfo.packed, hitInfo.packed.x, 0u, set0);
                    }
                }
            }
        }
    }

    if (cellInfo.surfelCount > 0u) {
        if (groupThreadID.x == x && groupThreadID.y == y) {
            if (electedCoverage > kDefaultRemovalThreshold) {
                float chance = pow(depth, float(kDefaultChancePower));
                if (rng_next_float(randomState) < chance * kDefaultChanceMultiply) {
                    uint  electedContribData = atomic_load_explicit(&groupShareMaxContribution, memory_order_relaxed);
                    uint  electedSurfelListIndex = (electedContribData & 0x0000FFFFu) >> 0;
                    uint  toDestroySurfelIndex = set0.gCellToSurfelBuffer[cellInfo.cellToSurfelBufferOffset + electedSurfelListIndex];
                    Surfel toDestroySurfel = set0.gSurfelBuffer[toDestroySurfelIndex];
                    toDestroySurfel.radius = 0.0f;
                    set0.gSurfelBuffer[toDestroySurfelIndex] = toDestroySurfel;
                    set0.gSurfelBoundsBuffer[toDestroySurfelIndex].radius = 0.0f;
                }
            }
        }
    }
}
