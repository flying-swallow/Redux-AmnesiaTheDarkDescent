// SurfelIntegratePass — per-valid-surfel accumulator. Reads all ray results
// for the surfel, averages them with PDF weighting + MSME, optionally
// shares radiance from neighbour surfels (USE_IRRADIANCE_SHARING) and
// writes the surfel-depth atlas borders for filtered reads in the
// generation pass.
//
// Ported from SurfelGI/SurfelIntegratePass.cs.slang.

#include "Utils/Math/MathConstants.slangh"
#include "../Constants.h"

import Bindless.resource;
import SceneTypes;
import Bindless.interface;
import Scene;
import Utils.Color.ColorHelpers;
import SurfelGI.SurfelTypes;
import SurfelGI.SurfelUtils;
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

    uint2 irrMapLT = uint2(
        surfelIndex % (kIrradianceMapRes.x / kIrradianceMapUnit.x),
        surfelIndex / (kIrradianceMapRes.x / kIrradianceMapUnit.y));
    irrMapLT *= kIrradianceMapUnit;

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

#ifdef USE_RAY_GUIDING
        uint2 irrMapOffset = kIrradianceMapHalfUnit + int2(
            sign(uv.x) * round(abs(uv.x * kIrradianceMapHalfUnit.x)),
            sign(uv.y) * round(abs(uv.y * kIrradianceMapHalfUnit.y)));
        uint2 irrMapCoord = irrMapLT + irrMapOffset;

        float k = 1e-2f;
        float delta = luminance(Lr) - gIrradianceMap[irrMapCoord];
        gIrradianceMap[irrMapCoord] = max(1e-3f, gIrradianceMap[irrMapCoord] + k * delta);
#endif

#ifdef USE_SURFEL_DEPTH
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
#endif

        // Monte Carlo estimator, matching the reference SurfelIntegratePass:
        // rays are uniform-hemisphere (pdf = 1/(16π) from rayGen), so the
        // weight cosθ·(1/(16π))/pdf collapses to cosθ.
        surfelRadiance += Lr * dot(dirWorld, surfel.normal) * ((1.f / (16.f * M_PI)) / max(1e-12f, pdf));
    }

    surfelRadiance /= surfel.rayCount;

#ifdef USE_RAY_GUIDING
    float sumLuminance = 0.f;
    bool  hasHole      = false;
    for (uint yy = 0; yy < kIrradianceMapUnit.y; ++yy)
    for (uint xx = 0; xx < kIrradianceMapUnit.x; ++xx)
    {
        const float l = gIrradianceMap[irrMapLT + uint2(xx, yy)];
        sumLuminance += l;
        if (!hasHole && step(l, 1e-12f)) hasHole = true;
    }
    surfel.sumLuminance = sumLuminance;
    surfel.hasHole      = hasHole ? 1u : 0u;
#endif

#ifdef USE_SURFEL_DEPTH
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
#endif

#ifdef USE_IRRADIANCE_SHARING
    float4 sharedRadiance = float4(0.f);
    float3 cameraPos = gScene.camera.getPosition();
    int3 cellPos = getCellPos(surfel.position, cameraPos, kCellUnit);
    if (isCellValid(cellPos))
    {
        const float3 centerPos    = surfel.position;
        const float3 centerNormal = surfel.normal;

        uint flattenIndex = getFlattenCellIndex(cellPos);
        CellInfo cellInfo = gCellInfoBuffer[flattenIndex];

        for (uint i = 0; i < cellInfo.surfelCount; ++i)
        {
            uint neiSurfelIndex = gCellToSurfelBuffer[cellInfo.cellToSurfelBufferOffset + i];
            Surfel neiSurfel = gSurfelBuffer[neiSurfelIndex];

            float3 bias = centerPos - neiSurfel.position;
            float dist2 = dot(bias, bias);
            float affectRadius = kCellUnit * sqrt(2);
            if (dist2 < pow(affectRadius, 2))
            {
                float dotN = dot(neiSurfel.normal, centerNormal);
                if (dotN > 0)
                {
                    float dist = sqrt(dist2);
                    float contribution = 1.f;
                    contribution *= saturate(dotN);
                    contribution *= saturate(1 - dist / affectRadius);
                    contribution  = smoothstep(0, 1, contribution);

                    #ifdef USE_SURFEL_DEPTH
                    {
                        float2 uv = getSurfelDepthUV(surfelIndex, bias / dist, centerNormal);
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

                    sharedRadiance += float4(neiSurfel.radiance, 1.f) * contribution;
                }
            }
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
#endif

    float3 mean = MSME(surfelRadiance, surfel.msmeData, kDefaultShortMeanWindow);

    // Box lights are volumetric ambient fills: they contribute to GI only
    // (never direct — the direct loop in SurfelGIRenderPass.frag skips them).
    // Add each box's color to surfels inside its (rotated) AABB, faded toward
    // the faces, so it bounces through the surfel cache. Added after MSME so the
    // deterministic fill isn't run through the temporal estimator.
    float3 boxFill = float3(0.f);
    uint boxCount = gScene.getBoxLightCount();
    for (uint b = 0; b < boxCount; ++b)
    {
        BoxLight box = gScene.getBoxLight(b);
        float3 d = surfel.position - box.center;
        // world -> light-local (axis-aligned boxes: identity rotation).
        float3 local = float3(dot(d, box.worldToLightX),
                              dot(d, box.worldToLightY),
                              dot(d, box.worldToLightZ));
        float3 q = abs(local) / max(box.halfSize, float3(1e-4f)); // 0 center, 1 face
        float m = max(q.x, max(q.y, q.z));
        float falloff = 1.f - smoothstep(kBoxFalloffBegin, 1.f, m); // 1 inside, 0 at face
        boxFill += box.color * falloff;
    }

    surfel.radiance = mean + boxFill * kBoxLightIndirectScale;

    gSurfelBuffer[surfelIndex] = surfel;
}
