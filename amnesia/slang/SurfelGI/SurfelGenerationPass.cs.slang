// SurfelGenerationPass — per-pixel surfel placement decision and indirect-
// lighting composite. Reads the V-buffer at gPackedHitInfo, finds covering
// surfels in the local cell, accumulates their radiance into a smoothed
// indirect-lighting output, and probabilistically spawns / removes surfels
// based on coverage thresholds.
//
// Ported from SurfelGI/SurfelGenerationPass.cs.slang.

#include "../Constants.h"

import Bindless.resource;
import SceneTypes;
import PerFrame.resource;
import Bindless.interface;
import Scene;
import Random;
import HashUtils;
import SurfelGI.SurfelTypes;
import SurfelGI.SurfelUtils;

// CB fields (placement / removal / blending / variance / chance) now come
// from Constants.h as `kDefault*`, matching the SurfelEvaluationPass /
// SurfelIntegratePass / SurfelRayTrace convention. Only the per-pixel
// indirect-lighting output stays pass-private at set 2.

[vk::binding(0, 2)]
RWTexture2D<float4> gOutput;

groupshared uint groupShareMinCoverage;
groupshared uint groupShareMaxContribution;

// Stage 2 — cooperative cell sweep. The tile processes one distinct grid cell
// per outer iteration: all 256 threads stream that cell's SurfelBounds list
// through LDS once, then the threads whose pixel maps to it consume the cached
// copy. This collapses the ~256x redundant global re-reads of a shared cell
// list into a single read. kLdsSurfelCap caps the per-chunk residency; longer
// cell lists (up to kPerCellSurfelLimit) are streamed in successive chunks.
static const uint kLdsSurfelCap = 256u;
static const uint kCellSentinel = 0xFFFFFFFFu;
groupshared uint         gLeaderCell;                  // cell being swept this round (or sentinel)
groupshared uint         gLeaderOffset;                // its cellToSurfelBufferOffset
groupshared uint         gLeaderCount;                 // its surfelCount
groupshared SurfelBounds gLdsBounds[kLdsSurfelCap];    // cached cull records
groupshared uint         gLdsSurfelIdx[kLdsSurfelCap]; // their surfel indices

[numthreads(16, 16, 1)]
void csMain(
    uint3 dispatchThreadId: SV_DispatchThreadID,
    uint  groupIndex      : SV_GroupIndex,
    uint3 groupThreadID   : SV_GroupThreadID,
    uint3 groupdId        : SV_GroupID)
{
    if (groupIndex == 0)
    {
        groupShareMinCoverage     = ~0u;
        groupShareMaxContribution = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    uint2 resolution = uint2(gPerFrame.viewportSize);
    uint2 pixelPos   = dispatchThreadId.xy;

    // Stage 2: no early return. Threads with no valid hit / off-grid cell are
    // flagged inactive but stay resident so every thread reaches the group
    // barriers and helps the cooperative LDS loads. (The original returned
    // here, which also left the post-loop GroupMemoryBarrier in divergent
    // control flow — flattening it removes that latent hazard too.)
    bool   active    = (dispatchThreadId.x < resolution.x && dispatchThreadId.y < resolution.y);
    float3 cameraPos = gScene.camera.getPosition();
    float  fovy      = gPerFrame.cameraFov;

    RNG randomState;
    randomState.init(pixelPos, gPerFrame.totalFrames);

    HitInfo hitInfo;
    hitInfo.packed = uint4(0u, 0u, 0u, 0u);
    VertexData v        = {};
    float      depth    = 0.f;
    CellInfo   cellInfo = { 0u, 0u };
    uint       myFlatten = kCellSentinel;

    if (active)
    {
        hitInfo.packed = gPackedHitInfo[pixelPos];
        if (!hitInfo.isValid())
        {
            active = false;
        }
        else
        {
            TriangleHit triangleHit = hitInfo.getTriangleHit();
            v = gScene.getVertexData(triangleHit);
            float4 curPosH = mul(gScene.camera.getViewProjNoJitter(), float4(v.posW, 1.f));
            depth = curPosH.z / curPosH.w;

            int3 cellPos = getCellPos(v.posW, cameraPos, kCellUnit);
            if (!isCellValid(cellPos))
                active = false;
            else
            {
                myFlatten = getFlattenCellIndex(cellPos);
                cellInfo  = gCellInfoBuffer[myFlatten];
            }
        }
    }

    // Per-pixel accumulators (meaningful only for active threads). RNG draw
    // order matches the original: this seed first, then next_uint(255) for the
    // election, then the spawn/removal next_float() — so placement decisions
    // stay bit-identical.
    float  coverage    = 0.f;
    float  varianceEx  = 0.f;
    float  rayCountEx  = 0.f;
    uint   refCount    = 0u;
    uint   life        = 0u;
    float  maxVariance = 0.f;
    float  maxContribution = 0.f;
    uint   maxContributionSurfelIndex = randomState.next_uint(cellInfo.surfelCount);
    float4 indirectLighting = float4(0.f);

    // Sweep the distinct cells present in the tile, one per outer iteration.
    bool done = !active;
    while (true)
    {
        if (groupIndex == 0)
            gLeaderCell = kCellSentinel;
        GroupMemoryBarrierWithGroupSync();

        // Elect one cell present among the not-yet-processed threads (a race;
        // some not-done thread's cell wins). Every thread in that cell will
        // match below and finish this round, so the not-done set strictly
        // shrinks — the loop runs once per distinct cell in the tile.
        if (!done)
            gLeaderCell = myFlatten;
        GroupMemoryBarrierWithGroupSync();

        uint leader = gLeaderCell;
        if (leader == kCellSentinel)
            break;                       // uniform across the group: all threads exit together

        if (!done && myFlatten == leader)
        {
            gLeaderOffset = cellInfo.cellToSurfelBufferOffset;
            gLeaderCount  = cellInfo.surfelCount;
        }
        GroupMemoryBarrierWithGroupSync();

        uint offset = gLeaderOffset;
        uint count  = gLeaderCount;   // uniform -> chunk loop is barrier-uniform

        for (uint base = 0u; base < count; base += kLdsSurfelCap)
        {
            uint chunk = min(kLdsSurfelCap, count - base);

            // Cooperative load: each thread pulls one (index, bounds) pair.
            for (uint k = groupIndex; k < chunk; k += 256u)
            {
                uint sIdx        = gCellToSurfelBuffer[offset + base + k];
                gLdsSurfelIdx[k] = sIdx;
                gLdsBounds[k]    = gSurfelBoundsBuffer[sIdx];
            }
            GroupMemoryBarrierWithGroupSync();

            // Consume from LDS — only threads whose pixel is in the leader cell.
            // Iterating k (hence i) ascending keeps the maxContribution
            // tie-break identical to the original linear loop.
            if (!done && myFlatten == leader)
            {
                for (uint k = 0u; k < chunk; ++k)
                {
                    uint         i           = base + k;   // cell-list position
                    uint         surfelIndex = gLdsSurfelIdx[k];
                    SurfelBounds sb          = gLdsBounds[k];

                    float3 bias  = v.posW - sb.position;
                    float  dist2 = dot(bias, bias);
                    if (dist2 < sb.radius * sb.radius)
                    {
                        float dotN = dot(v.normalW, sb.normal);
                        if (dotN > 0)
                        {
                            const SurfelRecycleInfo surfelRecycleInfo = gSurfelRecycleInfoBuffer[surfelIndex];
                            bool isSleeping = surfelRecycleInfo.status & 0x0001;
                            bool lastSeen   = surfelRecycleInfo.status & 0x0002;

                            if (!isSleeping)
                            {
                                // Survivor: only now pull the full surfel for the
                                // data the accumulation needs (radiance / variance
                                // / rayCount).
                                Surfel surfel = gSurfelBuffer[surfelIndex];

                                float dist = sqrt(dist2);
                                float contribution = 1.f;
                                contribution *= saturate(dotN);
                                contribution *= saturate(1 - dist / sb.radius);
                                contribution  = smoothstep(0, 1, contribution);

                                coverage += contribution;

                                #ifdef USE_SURFEL_DEPTH
                                {
                                    float2 uv = getSurfelDepthUV(surfelIndex, bias / dist, sb.normal);
                                    float2 surfelDepth = gSurfelDepth.SampleLevel(gSurfelDepthSampler, uv, 0u);
                                    float mean    = surfelDepth.x;
                                    float sqrMean = surfelDepth.y;
                                    if (dist > mean)
                                    {
                                        float variance = sqrMean - pow(mean, 2);
                                        contribution *= variance / (variance + pow(dist - mean, 2));
                                    }
                                }
                                #endif

                                indirectLighting += float4(surfel.radiance, 1.f) * contribution
                                                  * smoothstep(0, kDefaultBlendingDelay, surfelRecycleInfo.frame);

                                float varLen = length(surfel.msmeData.variance);
                                varianceEx += varLen * contribution;
                                rayCountEx += surfel.rayCount * contribution;

                                refCount = max(refCount, gSurfelRefCounter[surfelIndex]);
                                life     = max(life, surfelRecycleInfo.life);

                                if (maxContribution < contribution)
                                {
                                    maxContribution = contribution;
                                    maxContributionSurfelIndex = i;
                                }
                                maxVariance = max(maxVariance, varLen);
                            }

                            if (!lastSeen)
                                gSurfelRecycleInfoBuffer[surfelIndex].status |= 0x0002;
                        }
                    }
                }
            }
            GroupMemoryBarrierWithGroupSync();   // before reusing LDS for the next chunk
        }

        if (!done && myFlatten == leader)
            done = true;
    }

    // Per-pixel output + tile-wide election — active threads only (the original
    // reached this only for valid pixels).
    if (active)
    {
        if (indirectLighting.w > 0)
        {
            indirectLighting.xyz /= indirectLighting.w;
            indirectLighting.w    = saturate(indirectLighting.w);
            varianceEx           /= indirectLighting.w;
            rayCountEx           /= indirectLighting.w;

            if      (kDefaultOverlayMode == 0) gOutput[pixelPos] = indirectLighting;
            else if (kDefaultOverlayMode == 1) gOutput[pixelPos] = float4(stepColor(maxVariance * kDefaultVarianceSensitivity, 0.8f, 0.5f), 1);
            else if (kDefaultOverlayMode == 2) gOutput[pixelPos] = float4(stepColor(rayCountEx, 48.f, 32.f), 1);
            else if (kDefaultOverlayMode == 3) gOutput[pixelPos] = float4(lerpColor(refCount / 256.f), 1);
            else if (kDefaultOverlayMode == 4) gOutput[pixelPos] = float4(step(life, 0u), step(1u, life), 0, 1);
            else if (kDefaultOverlayMode == 5) gOutput[pixelPos] = float4(lerpColor(smoothstep(kDefaultPlacementThreshold, kDefaultRemovalThreshold, coverage)), 1);
        }

        uint covData = 0;
        covData |= ((f32tof16(coverage) & 0x0000FFFF) << 16);
        covData |= ((randomState.next_uint(255) & 0x000000FF) << 8);
        covData |= ((groupThreadID.x & 0x0000000F) << 4);
        covData |= ((groupThreadID.y & 0x0000000F) << 0);
        InterlockedMin(groupShareMinCoverage, covData);

        uint contribData = 0;
        contribData |= ((f32tof16(maxContribution) & 0x0000FFFF) << 16);
        contribData |= ((maxContributionSurfelIndex & 0x0000FFFF) << 0);
        InterlockedMax(groupShareMaxContribution, contribData);
    }
    GroupMemoryBarrierWithGroupSync();

    uint  electedData     = groupShareMinCoverage;
    float electedCoverage = f16tof32((electedData & 0xFFFF0000) >> 16);
    uint  x = (electedData & 0x000000F0) >> 4;
    uint  y = (electedData & 0x0000000F) >> 0;

    // DEBUG (Stage 0): sample per-cell surfel list length to size the
    // cooperative-LDS budget. One global atomic per tile from the elected
    // (min-coverage) thread — that thread is guaranteed valid (it ran the
    // InterlockedMin above) and reaches here without early-returning.
    if (groupThreadID.x == x && groupThreadID.y == y)
        InterlockedMax(gSurfelCounter[kSurfelCounterDbgGenSurfelMax], cellInfo.surfelCount);

    if (cellInfo.surfelCount < kPerCellSurfelLimit)
    {
        if (groupThreadID.x == x && groupThreadID.y == y)
        {
            if (electedCoverage <= kDefaultPlacementThreshold)
            {
                const float chance = pow(depth, kDefaultChancePower);
                if (randomState.next_float() < chance * kDefaultChanceMultiply)
                {
                    uint freeSurfelCount;
                    InterlockedAdd(gSurfelCounter[kSurfelCounterFree], (uint)-1, freeSurfelCount);
                    if (0 < freeSurfelCount && freeSurfelCount <= kTotalSurfelLimit)
                    {
                        uint validSurfelCount;
                        InterlockedAdd(gSurfelCounter[kSurfelCounterValid], 1u, validSurfelCount);
                        if (validSurfelCount < kTotalSurfelLimit)
                        {
                            uint newIndex = gSurfelFreeIndexBuffer[freeSurfelCount - 1];

                            float varRadius = calcSurfelRadius(
                                distance(cameraPos, v.posW),
                                fovy,
                                resolution,
                                kSurfelTargetArea,
                                kCellUnit);

                            Surfel newSurfel = Surfel(v.posW, v.normalW, varRadius);
                            newSurfel.radiance          = indirectLighting.xyz;
                            newSurfel.msmeData.mean     = indirectLighting.xyz;
                            newSurfel.msmeData.shortMean = indirectLighting.xyz;

                            gSurfelValidIndexBuffer[validSurfelCount] = newIndex;
                            gSurfelBuffer[newIndex] = newSurfel;
                            gSurfelRecycleInfoBuffer[newIndex] = { kMaxLife, 0u, 0u };
                            gSurfelGeometryBuffer[newIndex] = hitInfo.packed;
                            // Capture the anchor slot's current generation (packed.x is the
                            // instanceID). collectCellInfo invalidates this surfel if the slot
                            // is later reassigned to different geometry.
                            gSurfelSlotGeneration[newIndex] = gBindlessSlotGeneration[hitInfo.packed.x];
                            gSurfelRefCounter[newIndex] = 0u;
                        }
                    }
                }
            }
        }
    }

    if (cellInfo.surfelCount > 0)
    {
        if (groupThreadID.x == x && groupThreadID.y == y)
        {
            if (electedCoverage > kDefaultRemovalThreshold)
            {
                const float chance = pow(depth, kDefaultChancePower);
                if (randomState.next_float() < chance * kDefaultChanceMultiply)
                {
                    uint  electedContribData       = groupShareMaxContribution;
                    float electedMaxContribution   = f16tof32((electedContribData & 0xFFFF0000) >> 16);
                    uint  electedSurfelListIndex   = (electedContribData & 0x0000FFFF) >> 0;

                    uint toDestroySurfelIndex = gCellToSurfelBuffer[cellInfo.cellToSurfelBufferOffset + electedSurfelListIndex];
                    Surfel toDestroySurfel = gSurfelBuffer[toDestroySurfelIndex];
                    toDestroySurfel.radius = 0;
                    gSurfelBuffer[toDestroySurfelIndex] = toDestroySurfel;
                    // Keep the compact cull record in sync so other tiles this
                    // frame stop counting this surfel (matches the gSurfelBuffer
                    // removal the original loop relied on).
                    gSurfelBoundsBuffer[toDestroySurfelIndex].radius = 0;
                }
            }
        }
    }
}
