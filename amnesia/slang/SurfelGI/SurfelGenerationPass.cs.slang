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

// Tile-wide placement election only — the surfel gather is a straight
// per-pixel loop (see below), so these two words are the pass's entire LDS
// footprint, keeping occupancy high.
groupshared uint groupShareMinCoverage;
groupshared uint groupShareMaxContribution;

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
    uint       myFlatten = 0u;   // set for valid pixels below; only used to fetch cellInfo

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

    // Gather the surfels covering this pixel's cell. A straight per-pixel loop
    // over the cell's surfel list (the reference SurfelGenerationPass layout):
    // each thread reads the compact SurfelBounds cull record and only pulls the
    // full Surfel for survivors. No groupshared streaming or leader election —
    // LDS stays at the two election words above, so occupancy stays high.
    // Inactive threads (no valid hit / off-grid cell) skip the loop but still
    // reach the group barrier below.
    if (active)
    {
        for (uint i = 0u; i < cellInfo.surfelCount; ++i)
        {
            uint         surfelIndex = gCellToSurfelBuffer[cellInfo.cellToSurfelBufferOffset + i];
            SurfelBounds sb          = gSurfelBoundsBuffer[surfelIndex];

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
                        // Survivor: only now pull the full surfel for the data the
                        // accumulation needs (radiance / variance / rayCount).
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
