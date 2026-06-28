// SurfelUpdatePass — three entry points covering the surfel lifecycle:
//   collectCellInfo            : promote dirty surfels to valid, free dead,
//                                count cell occupancy, allocate rays.
//   accumulateCellInfo         : prefix-sum cell offsets into the flat
//                                cell-to-surfel table.
//   updateCellToSurfelBuffer   : scatter valid surfel indices into the cell
//                                buffer at the computed offsets.
//
// Ported from SurfelGI/SurfelUpdatePass.cs.slang. Buffers come from the
// engine bindless set (set 0) via bindless; camera + per-frame
// state come from Scene (free functions over set 0 + gPerFrame).

#include "../Constants.h"

import bindless;
import SceneTypes;
import PerFrame.resource;
import HitInfo;
import Scene;
import SurfelGI.SurfelTypes;
import SurfelGI.SurfelUtils;
import SurfelGI.SurfelLifecycle;    // releaseSurfel

// SurfelUpdate has no runtime-tunable parameters. The values that used to
// live in SurfelUpdateCB (the lock-surfel debug toggle, per-surfel ray
// budget bounds, and the variance sensitivity that biases the ray-count
// lerp) are sourced from `Constants.h`.

// --- collectCellInfo stages -------------------------------------------------

// Lifecycle tick: age life/frame, wake + refresh on lastSeen, extend a
// well-referenced sleeping surfel's life. Pure — returns the updated info;
// isSleeping is cleared in-place when the surfel wakes.
SurfelRecycleInfo tickSurfelLifecycle(SurfelRecycleInfo info, bool hasEnoughRefCount,
                                      inout bool isSleeping)
{
    bool lastSeen = (uint(info.status) & 0x0002u) != 0u;

    info.life  = uint16_t(max(int(info.life) - 1, 0));
    info.frame = uint16_t(clamp(int(info.frame) + 1, 0, 65535));

    if (lastSeen)
    {
        info.life = kMaxLife;
        if (isSleeping)
            isSleeping = false;
    }

    if (isSleeping && hasEnoughRefCount)
        info.life = kSleepingMaxLife;

    return info;
}

// Cached primary-ray triangle hit + stale-anchor guard. A surfel's bindless
// anchor slot goes stale whenever the slot's geometry changes out from under
// it: the backing vertex buffer is freed, the slot is REUSED by a different
// object (common on map load, when the new map floods the bindless pool and
// evicts the old map's slots), or the mesh is rebuilt in place. In every
// case getVertexData would pull this surfel's cached primitiveIndex against
// the wrong / freed geometry -> out-of-bounds vertex/index BDA read -> GPUVM
// fault. The host bumps the slot's reuse generation on each of those events
// (destroy / reuse / rebuild); if it no longer matches what this surfel
// captured at spawn, the anchor is stale, so skip the alive branch and let
// the caller's recycle (free-list) path return the slot.
//
// The instanceID bound check is load-bearing: gSurfelGeometryBuffer is
// populated lazily by spawn writers (SurfelGenerationPass / SurfelRayTrace)
// and a freshly-popped free slot can carry a previous tenant's packed hit
// until the next spawn rewrites it. If that residue decodes to an
// instanceID >= kObjectSlotCapacity, gBindlessSlotGeneration[hit.instanceID]
// is an unmapped SSBO read -> GPUVM fault before slotStale can be
// evaluated. Treat OOB instanceIDs as stale so the slot recycles cleanly.
bool isAnchorStale(uint surfelIndex, TriangleHit hit)
{
    return hit.instanceID >= kObjectSlotCapacity ||
           gSurfelSlotGeneration[surfelIndex] != gBindlessSlotGeneration[hit.instanceID];
}

// Reproject the surfel onto its cached triangle anchor and recompute the
// screen-area-derived radius (sleeping surfels get 16x the target area and a
// half-cell radius floor).
void reprojectSurfel(inout Surfel surfel, VertexData data, bool isSleeping,
                     float3 cameraPos, uint2 resolution, float fovy)
{
    surfel.position = data.posW;
    surfel.normal   = data.normalW;
    surfel.radius   = calcSurfelRadius(
        distance(cameraPos, surfel.position),
        fovy,
        resolution,
        kSurfelTargetArea * (isSleeping ? 16.f : 1.f),
        kCellUnit
    );
    if (isSleeping)
        surfel.radius = max(surfel.radius, kCellUnit * 0.5f);
}

// Publish the compact cull record the generation pass reads in place of the
// full surfel. position/normal/radius are final here; normal is
// pre-normalized so the per-pixel loop drops its rsqrt. One write per valid
// surfel per frame.
void publishSurfelBounds(uint surfelIndex, Surfel surfel)
{
    SurfelBounds bounds;
    bounds.position = surfel.position;
    bounds.radius   = surfel.radius;
    bounds.normal   = normalize(surfel.normal);
    gSurfelBoundsBuffer[surfelIndex] = bounds;
}

// Count this surfel into every intersecting cell of its 5x5x5 neighborhood
// (the prefix-sum pass turns the counts into gCellToSurfelBuffer offsets).
void countIntersectingCells(Surfel surfel, float3 cameraPos)
{
    int3 cellPos = getCellPos(surfel.position, cameraPos, kCellUnit);
    for (uint i = 0; i < kSurfelNeighborCellCount; ++i)
    {
        int3 neighborPos = surfelNeighborCellPos(cellPos, i);
        if (isSurfelIntersectCell(surfel, neighborPos, cameraPos, kCellUnit))
        {
            uint flattenIndex = getFlattenCellIndex(neighborPos);
            InterlockedAdd(gCellInfoBuffer[flattenIndex].surfelCount, 1u);
        }
    }
}

// Variance-driven ray budget: lerp lower..upper by MSME variance, reserve a
// slice of gSurfelRayResultBuffer and seed it. Over-budget surfels keep their
// previous offset/count (the reservation counter still advances — matches the
// reference).
void allocateSurfelRays(uint surfelIndex, inout Surfel surfel, bool isSleeping)
{
    uint lower = isSleeping ? (kDefaultMinRayCount / 4u) : (kDefaultMaxRayCount / 4u);
    uint upper = isSleeping ?  kDefaultMinRayCount       :  kDefaultMaxRayCount;

    uint rayOffset = 0;
    uint rayRequestCount = uint(clamp(
        lerp(float(lower), float(upper), length(surfel.msmeData.variance) * kDefaultVarianceSensitivity),
        float(lower), float(upper)));
    InterlockedAdd(gSurfelCounter[kSurfelCounterRequestedRay], rayRequestCount, rayOffset);

    if (rayOffset < kRayBudget)
    {
        surfel.rayOffset = rayOffset;
        surfel.rayCount  = rayRequestCount;

        SurfelRayResult initSurfelRayResult;
        initSurfelRayResult.surfelIndex = surfelIndex;
        for (uint rayIndex = 0; rayIndex < rayRequestCount; ++rayIndex)
            gSurfelRayResultBuffer[rayOffset + rayIndex] = initSurfelRayResult;
    }
}

// Per-cell surfel count + ray allocation. One thread per dirty surfel:
// tick lifecycle → alive? (radius, life, anchor) → reproject + publish cull
// record + bin into cells + allocate rays → write state back; dead surfels
// return to the free list.
[numthreads(64, 1, 1)]
void collectCellInfo(uint3 dispatchThreadId: SV_DispatchThreadID)
{
    uint dirtySurfelCount = gSurfelCounter[kSurfelCounterDirty];
    if (dispatchThreadId.x >= dirtySurfelCount)
        return;

    uint surfelIndex = gSurfelDirtyIndexBuffer[dispatchThreadId.x];
    Surfel surfel = gSurfelBuffer[surfelIndex];

    SurfelRecycleInfo surfelRecycleInfo = gSurfelRecycleInfoBuffer[surfelIndex];
    bool isSleeping = (uint(surfelRecycleInfo.status) & 0x0001u) != 0u;
    const bool hasEnoughRefCount = gSurfelRefCounter[surfelIndex] > kRefCountThreshold;
    surfelRecycleInfo = tickSurfelLifecycle(surfelRecycleInfo, hasEnoughRefCount, isSleeping);

    HitInfo hi;
    hi.packed = gSurfelGeometryBuffer[surfelIndex];
    TriangleHit hit = hi.getTriangleHit();

    bool alive = surfel.radius > 0 && surfelRecycleInfo.life > 0 &&
                 !isAnchorStale(surfelIndex, hit);
    if (!alive)
    {
        releaseSurfel(surfelIndex);
        return;
    }

    uint validSurfelCount;
    InterlockedAdd(gSurfelCounter[kSurfelCounterValid], 1u, validSurfelCount);
    gSurfelValidIndexBuffer[validSurfelCount] = surfelIndex;

    float3 cameraPos  = gPerFrame.posW;
    uint2  resolution = uint2(gPerFrame.viewportSize);
    float  fovy       = gPerFrame.cameraFov;

    reprojectSurfel(surfel, getVertexData(hit), isSleeping,
                    cameraPos, resolution, fovy);
    publishSurfelBounds(surfelIndex, surfel);
    countIntersectingCells(surfel, cameraPos);
    allocateSurfelRays(surfelIndex, surfel, isSleeping);

    surfelRecycleInfo.status = uint16_t(isSleeping ? 0x0001u : 0x0000u);   // clears lastSeen
    gSurfelRecycleInfoBuffer[surfelIndex] = surfelRecycleInfo;
    gSurfelBuffer[surfelIndex] = surfel;
    gSurfelRefCounter[surfelIndex] = 0u;
}

// Convert per-cell counts to prefix-sum offsets into gCellToSurfelBuffer.
//
// Dispatch shape: host issues (63, 63, 63) workgroups (ceil(250/4) per
// axis), so each thread maps 1:1 to a cell in the 250³ uniform grid.
// numthreads(4,4,4) = 64 threads/group keeps the same group size the
// host's `(kCellDimension + 3u) / 4u` math expects and exactly fills one
// RDNA/RDNA2 wave (wave width = 64). Bounds check trims the 252³ thread
// cube to the 250³ valid range; the flat index ordering
// (z*D*D + y*D + x) matches getFlattenCellIndex in SurfelUtils.slang.
//
// Wave-level atomic batching: instead of one InterlockedAdd per non-empty
// cell (potentially millions of serialized atomics on a single counter),
// each wave gathers its threads' contributions with WavePrefixSum, then
// one thread per wave does a single InterlockedAdd for the wave's total.
// This bounds global atomic contention at O(workgroups) = ~250K regardless
// of surfel density.
[numthreads(4, 4, 4)]
void accumulateCellInfo(uint3 dispatchThreadId: SV_DispatchThreadID)
{
    bool validCell = !any(dispatchThreadId >= uint3(kCellDimension));

    uint flattenIndex = 0u;
    if (validCell)
    {
        flattenIndex =
            dispatchThreadId.z * (kCellDimension * kCellDimension) +
            dispatchThreadId.y * kCellDimension +
            dispatchThreadId.x;
        gSurfelReservationBuffer[flattenIndex] = 0u;
    }

    uint count = (validCell && gCellInfoBuffer[flattenIndex].surfelCount > 0u)
                 ? gCellInfoBuffer[flattenIndex].surfelCount : 0u;

    uint waveLocalOffset = WavePrefixSum(count);
    uint waveTotal       = WaveActiveSum(count);

    uint waveBase = 0u;
    if (WaveIsFirstLane() && waveTotal > 0u)
        InterlockedAdd(gSurfelCounter[kSurfelCounterCell], waveTotal, waveBase);
    waveBase = WaveReadLaneFirst(waveBase);

    if (validCell && count > 0u)
    {
        gCellInfoBuffer[flattenIndex].cellToSurfelBufferOffset = waveBase + waveLocalOffset;
        gCellInfoBuffer[flattenIndex].surfelCount = 0u;
    }
}

// Scatter valid surfel indices into cell-to-surfel table at computed offsets.
// Reads the compact 32-byte SurfelBounds (position + radius) rather than the
// full 128-byte Surfel — both fields needed for getCellPos / intersection test
// are already there, reducing memory bandwidth 4× vs. the naive path.
[numthreads(64, 1, 1)]
void updateCellToSurfelBuffer(uint3 dispatchThreadId: SV_DispatchThreadID)
{
    uint validSurfelCount = gSurfelCounter[kSurfelCounterValid];
    if (dispatchThreadId.x >= validSurfelCount)
        return;

    uint surfelIndex = gSurfelValidIndexBuffer[dispatchThreadId.x];
    SurfelBounds bounds = gSurfelBoundsBuffer[surfelIndex];

    float3 cameraPos = gPerFrame.posW;
    int3 cellPos = getCellPos(bounds.position, cameraPos, kCellUnit);
    for (uint i = 0; i < kSurfelNeighborCellCount; ++i)
    {
        int3 neighborPos = surfelNeighborCellPos(cellPos, i);
        if (isSurfelIntersectCell(bounds.position, bounds.radius, neighborPos, cameraPos, kCellUnit))
        {
            uint flattenIndex = getFlattenCellIndex(neighborPos);

            uint prevCount;
            InterlockedAdd(gCellInfoBuffer[flattenIndex].surfelCount, 1u, prevCount);
            gCellToSurfelBuffer[gCellInfoBuffer[flattenIndex].cellToSurfelBufferOffset + prevCount] = surfelIndex;
        }
    }
}
