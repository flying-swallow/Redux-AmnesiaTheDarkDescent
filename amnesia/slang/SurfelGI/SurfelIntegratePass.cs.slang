// SurfelIntegratePass — per-valid-surfel accumulator. Reads all ray results
// for the surfel, averages them with PDF weighting + MSME, optionally
// shares radiance from neighbour surfels and
// writes the surfel-depth atlas borders for filtered reads in the
// generation pass.
//
// Ported from SurfelGI/SurfelIntegratePass.cs.slang.

#include "Utils/Math/MathConstants.slangh"
#include "../Constants.h"

import bindless;
import SceneTypes;
import Scene;
import PerFrame.resource;     // gPerFrame (camera view-proj / eye position)
import Utils.Color.ColorHelpers;
import SurfelGI.SurfelTypes;
import SurfelGI.SurfelUtils;
import SurfelGI.SurfelGather;
import SurfelGI.MultiscaleMeanEstimator;

// gVarianceSensitivity / gShortMeanWindow used to live in SurfelIntegrateCB
// at set 2; they're now sourced from Constants.h (kDefaultVarianceSensitivity
// / kDefaultShortMeanWindow), matching the SurfelEvaluationPass/SurfelRayTrace
// convention so this shader uses sets 0 (bindless) + 1 (gPerFrame) only.

[numthreads(32, 1, 1)]
void csMain(uint3 dispatchThreadId: SV_DispatchThreadID)
{
    uint validSurfelCount = gSurfelCounter[kSurfelCounterValid];
    if (dispatchThreadId.x >= validSurfelCount)
        return;

    uint surfelIndex = gSurfelValidIndexBuffer[dispatchThreadId.x];
    Surfel surfel = gSurfelBuffer[surfelIndex];

    if (surfel.rayCount == 0)
        return;

    uint2 surfelDepthLT = uint2(
        surfelIndex % (kSurfelDepthTextureRes.x / kSurfelDepthTextureUnit.x),
        surfelIndex / (kSurfelDepthTextureRes.x / kSurfelDepthTextureUnit.y));
    surfelDepthLT *= kSurfelDepthTextureUnit;

    float3 surfelRadiance = float3(0.f);
    for (uint rayIndex = 0; rayIndex < surfel.rayCount; ++rayIndex)
    {
        const SurfelRayResult rayResult = gSurfelRayResultBuffer[surfel.rayOffset + rayIndex];

        float3 Lr        = rayResult.radiance;
        float3 dirLocal  = rayResult.dirLocal;
        float3 dirWorld  = rayResult.dirWorld;
        float  pdf       = rayResult.pdf;
        float  surfelDepth = (rayResult.firstRayLength > 0)
                             ? clamp(rayResult.firstRayLength, 0, surfel.radius)
                             : surfel.radius;

        float2 uv = octEncode(normalize(dirLocal));

        uint2 surfelDepthTextureOffset = kSurfelDepthTextureHalfUnit + int2(
            sign(uv.x) * round(abs(uv.x * (kSurfelDepthTextureHalfUnit.x - 1))),
            sign(uv.y) * round(abs(uv.y * (kSurfelDepthTextureHalfUnit.y - 1))));
        uint2 surfelDepthTextureCoord = surfelDepthLT + surfelDepthTextureOffset;

        float2 signedUV = ((int2)surfelDepthTextureOffset - kSurfelDepthTextureHalfUnit)
                        / (kSurfelDepthTextureHalfUnit - int2(1, 1));
        float3 texelDir = octDecode(signedUV);
        float  depthWeight = saturate(dot(dirLocal, texelDir));

        float k2 = 1e-2f;
        float2 delta2 = float2(surfelDepth, pow(surfelDepth, 2)) - gSurfelDepthMap[surfelDepthTextureCoord];
        gSurfelDepthMap[surfelDepthTextureCoord] = gSurfelDepthMap[surfelDepthTextureCoord] + k2 * delta2 * depthWeight;

        // Monte Carlo estimator for irradiance/π (= what the composite's
        // albedo·indirect convention expects, matching the direct term's
        // evalDiffuseDemodulated 1/π). Weight = cosθ/(π·pdf); with the true
        // uniform-hemisphere pdf 1/(2π) from rayGen this is 2·cosθ, so the
        // accumulated mean = (1/N)·Σ 2·Lᵣ·cosθ = E/π.
        surfelRadiance += Lr * dot(dirWorld, surfel.normal) / (M_PI * max(1e-12f, pdf));
    }

    surfelRadiance /= surfel.rayCount;

    // Border replication for the surfel-depth atlas tile (used by the
    // sampled-view filtered read in generation/evaluation passes).
    for (uint xx = 1; xx < kSurfelDepthTextureUnit.x - 1; ++xx)
    {
        gSurfelDepthMap[surfelDepthLT + uint2(xx, 0)] =
            gSurfelDepthMap[surfelDepthLT + uint2(xx, 1)];
        gSurfelDepthMap[surfelDepthLT + uint2(xx, kSurfelDepthTextureUnit.y - 1)] =
            gSurfelDepthMap[surfelDepthLT + uint2(xx, kSurfelDepthTextureUnit.y - 2)];
    }
    for (uint yy = 1; yy < kSurfelDepthTextureUnit.y - 1; ++yy)
    {
        gSurfelDepthMap[surfelDepthLT + uint2(0, yy)] =
            gSurfelDepthMap[surfelDepthLT + uint2(1, yy)];
        gSurfelDepthMap[surfelDepthLT + uint2(kSurfelDepthTextureUnit.x - 1, yy)] =
            gSurfelDepthMap[surfelDepthLT + uint2(kSurfelDepthTextureUnit.x - 2, yy)];
    }
    gSurfelDepthMap[surfelDepthLT + uint2(0, 0)] = (
        gSurfelDepthMap[surfelDepthLT + uint2(0, 1)] +
        gSurfelDepthMap[surfelDepthLT + uint2(1, 0)]) / 2.f;
    gSurfelDepthMap[surfelDepthLT + uint2(0, kSurfelDepthTextureUnit.y - 1)] = (
        gSurfelDepthMap[surfelDepthLT + uint2(0, kSurfelDepthTextureUnit.y - 2)] +
        gSurfelDepthMap[surfelDepthLT + uint2(1, kSurfelDepthTextureUnit.y - 1)]) / 2.f;
    gSurfelDepthMap[surfelDepthLT + uint2(kSurfelDepthTextureUnit.x - 1, kSurfelDepthTextureUnit.y - 1)] = (
        gSurfelDepthMap[surfelDepthLT + uint2(kSurfelDepthTextureUnit.x - 1, kSurfelDepthTextureUnit.y - 2)] +
        gSurfelDepthMap[surfelDepthLT + uint2(kSurfelDepthTextureUnit.x - 2, kSurfelDepthTextureUnit.y - 1)]) / 2.f;
    gSurfelDepthMap[surfelDepthLT + uint2(kSurfelDepthTextureUnit.x - 1, 0)] = (
        gSurfelDepthMap[surfelDepthLT + uint2(kSurfelDepthTextureUnit.x - 1, 1)] +
        gSurfelDepthMap[surfelDepthLT + uint2(kSurfelDepthTextureUnit.x - 2, 0)]) / 2.f;

    float4 sharedRadiance = float4(0.f);
    uint flattenIndex;
    CellInfo cellInfo;
    if (lookupSurfelCell(surfel.position, gPerFrame.posW, flattenIndex, cellInfo))
    {
        const float3 centerPos    = surfel.position;
        const float3 centerNormal = surfel.normal;
        const float  affectRadius = kCellUnit * sqrt(2);

        for (uint i = 0; i < cellInfo.surfelCount; ++i)
        {
            uint neiSurfelIndex = gCellToSurfelBuffer[cellInfo.cellToSurfelBufferOffset + i];
            Surfel neiSurfel = gSurfelBuffer[neiSurfelIndex];

            // The share weight intentionally dots RAW (unnormalized) normals,
            // uses the fixed cell-diagonal affect radius (not the neighbour's
            // own radius), and weights by the CENTER surfel's depth function
            // (surfelIndex + centerNormal, not the neighbour's) — reference
            // behavior: the center's own depth occludes sharing from behind
            // it. Not copy-paste bugs to "fix".
            SurfelHit hit;
            if (!testSurfelCoverage(centerPos, centerNormal, neiSurfel.position,
                                    neiSurfel.normal, affectRadius, hit))
                continue;

            float contribution = surfelKernelWeight(hit.dotN, hit.dist, affectRadius);
            contribution *= surfelDepthWeight(surfelIndex, hit.dirUnit,
                                              centerNormal, hit.dist);

            sharedRadiance += float4(neiSurfel.radiance, 1.f) * contribution;
        }

        if (sharedRadiance.w > 0)
        {
            sharedRadiance.xyz /= sharedRadiance.w;
            surfelRadiance = lerp(
                surfelRadiance,
                sharedRadiance.xyz,
                saturate(length(surfel.msmeData.variance) * kDefaultVarianceSensitivity));
        }
    }

    float3 mean = MSME(surfelRadiance, surfel.msmeData, kDefaultShortMeanWindow);

    surfel.radiance = mean;

    gSurfelBuffer[surfelIndex] = surfel;
}
