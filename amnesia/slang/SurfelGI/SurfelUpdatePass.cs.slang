// SurfelUpdatePass — three entry points covering the surfel lifecycle:
//   collectCellInfo            : promote dirty surfels to valid, free dead,
//                                count cell occupancy, allocate rays.
//   accumulateCellInfo         : prefix-sum cell offsets into the flat
//                                cell-to-surfel table.
//   updateCellToSurfelBuffer   : scatter valid surfel indices into the cell
//                                buffer at the computed offsets.
//
// Ported from SurfelGI/SurfelUpdatePass.cs.slang. Buffers come from the
// engine bindless set (set 0) via Bindless.resource; camera + per-frame
// state come from Scene (gScene.*).

#include "../Constants.h"

import Bindless.resource;
import SceneTypes;
import PerFrame.resource;
import Bindless.interface;
import Scene;
import SurfelGI.SurfelTypes;
import SurfelGI.SurfelUtils;

// SurfelUpdate has no runtime-tunable parameters. The values that used to
// live in SurfelUpdateCB (the lock-surfel debug toggle, per-surfel ray
// budget bounds, and the variance sensitivity that biases the ray-count
// lerp) are sourced from `Constants.h`.

// Per-cell surfel count + ray allocation. One thread per dirty surfel.
[numthreads(32, 1, 1)]
void collectCellInfo(uint3 dispatchThreadId: SV_DispatchThreadID)
{
    uint dirtySurfelCount = gSurfelCounter[kSurfelCounterDirty];
    if (dispatchThreadId.x >= dirtySurfelCount)
        return;

    uint surfelIndex = gSurfelDirtyIndexBuffer[dispatchThreadId.x];
    Surfel surfel = gSurfelBuffer[surfelIndex];

    float surfelRadius = surfel.radius;
    SurfelRecycleInfo surfelRecycleInfo = gSurfelRecycleInfoBuffer[surfelIndex];
    bool isSleeping = surfelRecycleInfo.status & 0x0001;
    bool lastSeen   = surfelRecycleInfo.status & 0x0002;

    surfelRecycleInfo.life  = max(asint(surfelRecycleInfo.life) - 1, 0);
    surfelRecycleInfo.frame = clamp(asint(surfelRecycleInfo.frame) + 1, 0, 65535);

    if (lastSeen)
    {
        surfelRecycleInfo.life = kMaxLife;
        if (isSleeping)
            isSleeping = false;
    }

    const bool hasEnoughRefCount = gSurfelRefCounter[surfelIndex] > kRefCountThreshold;
    if (isSleeping && hasEnoughRefCount)
        surfelRecycleInfo.life = kSleepingMaxLife;

    float3 cameraPos = gScene.camera.getPosition();
    uint2  resolution = uint2(gPerFrame.viewportSize);
    float  fovy = gPerFrame.cameraFov;

    // DEBUG: split the alive gate so we can see which arm is failing when
    // DbgAlive collapses to 0. Counted on every dispatched thread.
    if (surfelRadius > 0)              InterlockedAdd(gSurfelCounter[kSurfelCounterDbgRadiusPos], 1u);
    if (surfelRecycleInfo.life > 0)    InterlockedAdd(gSurfelCounter[kSurfelCounterDbgLifePos],   1u);

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
    // the recycle (free-list) else-branch below return the slot. instanceID
    // always came from a real hit, so indexing gBindlessSlotGeneration is in
    // bounds.
    HitInfo hi;
    hi.packed = gSurfelGeometryBuffer[surfelIndex];
    TriangleHit hit = hi.getTriangleHit();
    bool slotStale =
        gSurfelSlotGeneration[surfelIndex] != gBindlessSlotGeneration[hit.instanceID];

    if (surfelRadius > 0 && surfelRecycleInfo.life > 0 && !slotStale)
    {
        // DEBUG: dirty surfels that reach the cell-binding branch.
        InterlockedAdd(gSurfelCounter[kSurfelCounterDbgAlive], 1u);

        uint validSurfelCount;
        InterlockedAdd(gSurfelCounter[kSurfelCounterValid], 1u, validSurfelCount);
        gSurfelValidIndexBuffer[validSurfelCount] = surfelIndex;

        VertexData data = gScene.getVertexData(hit);

        if (kDefaultLockSurfel == 0u)
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

        // Stage 1: publish the compact cull record the generation pass reads in
        // place of the full surfel. position/normal/radius are final here in
        // both normal and lock-surfel modes; normal is pre-normalized so the
        // per-pixel loop drops its rsqrt. One write per valid surfel per frame.
        SurfelBounds bounds;
        bounds.position = surfel.position;
        bounds.radius   = surfel.radius;
        bounds.normal   = normalize(surfel.normal);
        gSurfelBoundsBuffer[surfelIndex] = bounds;

        int3 cellPos = getCellPos(surfel.position, cameraPos, kCellUnit);
        for (uint i = 0; i < 125; ++i)
        {
            int3 neighborPos = cellPos + neighborOffset[i];

            // DEBUG: split the validity vs intersection checks so we can
            // distinguish "cell off the grid" from "surfel didn't reach
            // this neighbor". isSurfelIntersectCell normally short-circuits
            // on isCellValid and we'd never know how many neighbors fell
            // outside ±kCellDimension/2.
            if (!isCellValid(neighborPos))
            {
                InterlockedAdd(gSurfelCounter[kSurfelCounterDbgCellInvalid], 1u);
                continue;
            }
            if (isSurfelIntersectCell(surfel, neighborPos, cameraPos, kCellUnit))
            {
                uint flattenIndex = getFlattenCellIndex(neighborPos);
                InterlockedAdd(gCellInfoBuffer[flattenIndex].surfelCount, 1u);
                InterlockedAdd(gSurfelCounter[kSurfelCounterDbgCellHits], 1u);
            }
        }

        if (kDefaultLockSurfel == 0u)
        {
            uint lower = isSleeping ? (kDefaultMinRayCount / 4u) : (kDefaultMaxRayCount / 4u);
            uint upper = isSleeping ?  kDefaultMinRayCount       :  kDefaultMaxRayCount;

            uint rayOffset = 0;
            uint rayRequestCount = clamp(
                lerp(lower, upper, length(surfel.msmeData.variance) * kDefaultVarianceSensitivity),
                lower, upper);
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

            surfelRecycleInfo.status = isSleeping ? 0x0001 : 0x0000;
            gSurfelRecycleInfoBuffer[surfelIndex] = surfelRecycleInfo;
            gSurfelBuffer[surfelIndex] = surfel;
            gSurfelRefCounter[surfelIndex] = 0u;
        }
    }
    else if (kDefaultLockSurfel == 0u)
    {
        uint freeSurfelCount;
        InterlockedAdd(gSurfelCounter[kSurfelCounterFree], 1u, freeSurfelCount);
        gSurfelFreeIndexBuffer[freeSurfelCount] = surfelIndex;
    }
}

// Convert per-cell counts to prefix-sum offsets into gCellToSurfelBuffer.
//
// Dispatch shape: host issues (63, 63, 63) workgroups (ceil(250/4) per
// axis), so each thread maps 1:1 to a cell in the 250³ uniform grid.
// numthreads(4,4,4) = 64 threads/group keeps the same group size the
// host's `(kCellDimension + 3u) / 4u` math expects. Bounds check trims
// the 252³ thread cube to the 250³ valid range; the flat index ordering
// (z*D*D + y*D + x) matches getFlattenCellIndex in SurfelUtils.slang.
[numthreads(4, 4, 4)]
void accumulateCellInfo(uint3 dispatchThreadId: SV_DispatchThreadID)
{
    if (any(dispatchThreadId >= uint3(kCellDimension)))
        return;

    uint flattenIndex =
        dispatchThreadId.z * (kCellDimension * kCellDimension) +
        dispatchThreadId.y * kCellDimension +
        dispatchThreadId.x;
    gSurfelReservationBuffer[flattenIndex] = 0u;

    if (gCellInfoBuffer[flattenIndex].surfelCount == 0)
        return;

    InterlockedAdd(
        gSurfelCounter[kSurfelCounterCell],
        gCellInfoBuffer[flattenIndex].surfelCount,
        gCellInfoBuffer[flattenIndex].cellToSurfelBufferOffset);

    gCellInfoBuffer[flattenIndex].surfelCount = 0;
}

// Scatter valid surfel indices into cell-to-surfel table at computed offsets.
[numthreads(32, 1, 1)]
void updateCellToSurfelBuffer(uint3 dispatchThreadId: SV_DispatchThreadID)
{
    uint validSurfelCount = gSurfelCounter[kSurfelCounterValid];
    if (dispatchThreadId.x >= validSurfelCount)
        return;

    uint surfelIndex = gSurfelValidIndexBuffer[dispatchThreadId.x];
    Surfel surfel = gSurfelBuffer[surfelIndex];

    float3 cameraPos = gScene.camera.getPosition();
    int3 cellPos = getCellPos(surfel.position, cameraPos, kCellUnit);
    for (uint i = 0; i < 125; ++i)
    {
        int3 neighborPos = cellPos + neighborOffset[i];
        if (isSurfelIntersectCell(surfel, neighborPos, cameraPos, kCellUnit))
        {
            uint flattenIndex = getFlattenCellIndex(neighborPos);

            uint prevCount;
            InterlockedAdd(gCellInfoBuffer[flattenIndex].surfelCount, 1u, prevCount);
            gCellToSurfelBuffer[gCellInfoBuffer[flattenIndex].cellToSurfelBufferOffset + prevCount] = surfelIndex;
        }
    }
}
