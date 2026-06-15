// SurfelGenerationPass — per-pixel surfel placement decision and indirect-
// lighting composite. Reads the V-buffer at gPackedHitInfo, finds covering
// surfels in the local cell, accumulates their radiance into a smoothed
// indirect-lighting output, and probabilistically spawns / removes surfels
// based on coverage thresholds.
//
// Ported from SurfelGI/SurfelGenerationPass.cs.slang.

#include "../Constants.h"

import bindless;
import SceneTypes;
import PerFrame.resource;
import HitInfo;
import Scene;
import Random;
import SurfelGI.SurfelTypes;
import SurfelGI.SurfelUtils;
import SurfelGI.SurfelGather;
import SurfelGI.SurfelLifecycle;

// CB fields (placement / removal / blending / variance / chance) now come
// from Constants.h as `kDefault*`, matching the SurfelIntegratePass /
// SurfelRayTrace convention. The runtime overlay selector is supplied through
// push constants; only the indirect-lighting output stays private at set 2.

struct SurfelGenerationPushConstants
{
    uint overlayMode;
};

[vk::push_constant]
ConstantBuffer<SurfelGenerationPushConstants> gPushConstants;

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
    const uint overlayMode = gPushConstants.overlayMode;

    if (groupIndex == 0)
    {
        groupShareMinCoverage     = ~0u;
        groupShareMaxContribution = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    // No early return. Threads off-screen / without a valid V-buffer hit /
    // off the surfel grid are flagged inactive but stay resident so every
    // thread in the workgroup reaches the post-loop
    // GroupMemoryBarrierWithGroupSync below. Early-returning here is HLSL UB
    // (divergent barrier) and on RADV/AMD hardware reliably hangs the GPU —
    // exactly the regression that motivated this guard.
    uint2 resolution = uint2(gPerFrame.viewportSize);
    bool  active     = (dispatchThreadId.x < resolution.x &&
                        dispatchThreadId.y < resolution.y);

    uint2 pixelPos   = dispatchThreadId.xy;
    float3 cameraPos = gScene.camera.getPosition();
    float  fovy      = gPerFrame.cameraFov;

    RNG randomState;
    randomState.init(pixelPos, gPerFrame.totalFrames);

    // Resolve the hit to a world-space vertex + cell — mirrors
    // SurfelEvaluationPass. Without this, cellInfo.surfelCount stays 0, the
    // gather loop never runs, indirectLighting.w stays 0, and the `if
    // (indirectLighting.w > 0)` block (the only writer of gOutput) is skipped
    // for every pixel. Stays inside the `active` guard so off-screen threads
    // don't dereference gPackedHitInfo / gScene with invalid pixel coords.
    HitInfo  hitInfo;
    hitInfo.packed = uint4(0u, 0u, 0u, 0u);
    VertexData v        = {};
    float      depth    = 0.f;
    CellInfo   cellInfo = { 0u, 0u };

    if (active)
    {
        hitInfo = HitInfo(gPackedHitInfo[pixelPos]);
        if (!hitInfo.isValid())
        {
            active = false;
        }
        else
        {
            TriangleHit triangleHit = hitInfo.getTriangleHit();
            v = gScene.getVertexData(triangleHit);
            // Linear, scale-correct placement depth. The reference used NDC
            // z/w here, but under this engine's 0.05–1000 camera far plane that
            // ratio saturates to ~1 for all geometry past ~1 m, defeating the
            // distance-based placement throttle below (chance = pow(depth, P)).
            // Use linear camera distance normalized to the GI grid's half-extent
            // — the range where surfels actually live — so the bias ramps 0→1
            // across it and near-camera over-placement is curbed. Auto-scales
            // with kCellUnit.
            const float kPlacementDepthRange = max(1e-3f, (float)(kCellDimension / 2u) * kCellUnit);
            depth = saturate(distance(cameraPos, v.posW) / kPlacementDepthRange);

            int3 cellPos = getCellPos(v.posW, cameraPos, kCellUnit);
            if (!isCellValid(cellPos))
            {
                active = false;
            }
            else
            {
                cellInfo = gCellInfoBuffer[getFlattenCellIndex(cellPos)];
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
    // Inactive threads skip the loop but still reach the group barrier below.
    if (active)
    {
        for (uint i = 0u; i < cellInfo.surfelCount; ++i)
        {
            uint         surfelIndex = gCellToSurfelBuffer[cellInfo.cellToSurfelBufferOffset + i];
            SurfelBounds sb          = gSurfelBoundsBuffer[surfelIndex];

            // sb.normal is pre-normalized (collectCellInfo writes it that way),
            // so the coverage test dots it directly — no per-iteration rsqrt.
            SurfelHit hit;
            if (testSurfelCoverage(v.posW, v.normalW, sb.position, sb.normal,
                                   sb.radius, hit))
            {
                const SurfelRecycleInfo surfelRecycleInfo = gSurfelRecycleInfoBuffer[surfelIndex];
                bool isSleeping = surfelRecycleInfo.status & 0x0001;
                bool lastSeen   = surfelRecycleInfo.status & 0x0002;

                if (!isSleeping)
                {
                    // Survivor: only now pull the full surfel for the data the
                    // accumulation needs (radiance / variance / rayCount).
                    Surfel surfel = gSurfelBuffer[surfelIndex];

                    float contribution = surfelKernelWeight(hit.dotN, hit.dist, sb.radius);
                    coverage += contribution;
                    contribution *= surfelDepthWeight(surfelIndex, hit.dirUnit,
                                                      sb.normal, hit.dist);

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

                // Runs for ALL covering surfels (sleeping included): seen-this-
                // frame marking is what wakes / extends them in collectCellInfo.
                if (!lastSeen)
                    gSurfelRecycleInfoBuffer[surfelIndex].status |= 0x0002;
            }
        }

        const float totalWeight = indirectLighting.w;

        if (totalWeight > 0.f)
        {
            indirectLighting.xyz /= totalWeight;
            indirectLighting.w = saturate(totalWeight);

            varianceEx /= totalWeight;
            rayCountEx /= totalWeight;
        }

        // Coverage remains meaningful when no surfels contribute filtered lighting.
        if (overlayMode == 5u)
        {
            gOutput[pixelPos] = float4(lerpColor(smoothstep(kDefaultPlacementThreshold, kDefaultRemovalThreshold, coverage)), 1.f);
        }
        else if (totalWeight > 0.f)
        {
            if (overlayMode == 0u || overlayMode == 6u || overlayMode == 7u)
                gOutput[pixelPos] = indirectLighting;
            else if (overlayMode == 1u)
                gOutput[pixelPos] = float4(stepColor(maxVariance * kDefaultVarianceSensitivity, 0.8f, 0.5f), 1.f);
            else if (overlayMode == 2u)
                gOutput[pixelPos] = float4(stepColor(rayCountEx, 48.f, 32.f), 1.f);
            else if (overlayMode == 3u)
                gOutput[pixelPos] = float4(lerpColor(float(refCount) / 256.f), 1.f);
            else if (overlayMode == 4u)
                gOutput[pixelPos] = float4(step(float(life), 0.f), step(1.f, float(life)), 0.f, 1.f);
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

    if (cellInfo.surfelCount < kPerCellSurfelLimit)
    {
        if (groupThreadID.x == x && groupThreadID.y == y)
        {
            // Skip the spawn entirely if the cached primary hit's instanceID is
            // outside the bindless slot table. The spawn is the source of the
            // anchor that collectCellInfo will dereference next frame against
            // gBindlessSlotGeneration[hit.instanceID]; an OOB anchor would
            // GPUVM-fault there. Bailing here keeps the free / valid counters
            // untouched too.
            if (electedCoverage <= kDefaultPlacementThreshold &&
                hitInfo.packed.x < kObjectSlotCapacity)
            {
                const float chance = pow(depth, kDefaultChancePower);
                if (randomState.next_float() < chance * kDefaultChanceMultiply)
                {
                    uint newIndex, validSlot;
                    if (tryAllocateSurfelSlot(newIndex, validSlot))
                    {
                        float varRadius = calcSurfelRadius(
                            distance(cameraPos, v.posW),
                            fovy,
                            resolution,
                            kSurfelTargetArea,
                            kCellUnit);

                        Surfel newSurfel = Surfel(v.posW, v.normalW, varRadius);
                        newSurfel.radiance           = indirectLighting.xyz;
                        newSurfel.msmeData.mean      = indirectLighting.xyz;
                        newSurfel.msmeData.shortMean = indirectLighting.xyz;

                        // packed.x is the instanceID (already bounded above) —
                        // publishNewSurfel captures the anchor slot's generation
                        // from it.
                        SurfelRecycleInfo recycle = { kMaxLife, 0u, 0u };
                        publishNewSurfel(newIndex, validSlot, newSurfel, recycle,
                                         hitInfo.packed, hitInfo.packed.x,
                                         /*initialRefCount=*/0u);
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
