// Surfel-GI per-pixel evaluation pass.
//
// For each pixel of `gPackedHitInfo` that landed on geometry, looks up
// the surfel cell, gathers surfel contributions, and writes the result
// into `gOutput`. Overlay modes other than 0 render diagnostic colors
// (variance, ray-count, ref-count, life, coverage).
//
// Ported from Falcor's SurfelGI/SurfelEvaluationPass.cs.slang. Buffers
// + the packed-hit texture + surfel depth atlas come from
// `Bindless.resource`; the per-pass output texture is pass-private (set 2).

#include "../HostDefinitions.h"
#include "../Constants.h"

import SceneTypes;
import Scene;
import Bindless.resource;
import Bindless.interface;
import PerFrame.resource;
import Random;
import HashUtils;
import SurfelGI.SurfelTypes;
import SurfelGI.SurfelUtils;

// Per-pixel evaluation has no runtime-tunable parameters; everything that
// used to live in SurfelEvaluationCB (placement / removal / blending /
// variance thresholds and the debug overlay selector) is sourced from
// `Constants.h`, and the per-frame counter comes from PerFrame.totalFrames.

[vk::binding(0, 2)]
RWTexture2D<float4> gOutput;

[numthreads(16, 16, 1)]
void csMain(
    uint3 dispatchThreadId : SV_DispatchThreadID,
    uint  groupIndex       : SV_GroupIndex,
    uint3 groupThreadID    : SV_GroupThreadID,
    uint3 groupdId         : SV_GroupID
)
{
    uint2 pixelPos = dispatchThreadId.xy;

    RNG randomState;
    randomState.init(pixelPos, gPerFrame.totalFrames);

    HitInfo hitInfo = HitInfo(gPackedHitInfo[pixelPos]);
    if (!hitInfo.isValid())
        return;

    TriangleHit triangleHit = hitInfo.getTriangleHit();
    VertexData v = gScene.getVertexData(triangleHit);
    float4 curPosH = mul(gScene.camera.getViewProjNoJitter(), float4(v.posW, 1.f));
    float depth = curPosH.z / curPosH.w;

    int3 cellPos = getCellPos(v.posW, gScene.camera.getPosition(), kCellUnit);
    if (!isCellValid(cellPos))
        return;

    uint flattenIndex = getFlattenCellIndex(cellPos);
    CellInfo cellInfo = gCellInfoBuffer[flattenIndex];

    float4 indirectLighting = float4(0.f);
    float coverage = 0.f;
    float varianceEx = 0.f;
    float rayCountEx = 0.f;
    uint  refCount = 0u;
    uint  life = 0u;
    float maxVariance = 0.f;

    for (uint i = 0; i < cellInfo.surfelCount; ++i)
    {
        uint surfelIndex = gCellToSurfelBuffer[cellInfo.cellToSurfelBufferOffset + i];
        Surfel surfel = gSurfelBuffer[surfelIndex];

        float3 bias = v.posW - surfel.position;
        float  dist2 = dot(bias, bias);
        if (dist2 >= surfel.radius * surfel.radius) continue;

        float3 normal = normalize(surfel.normal);
        float  dotN   = dot(v.normalW, normal);
        if (dotN <= 0.0f) continue;

        float dist = sqrt(dist2);
        float contribution = 1.f;
        contribution *= saturate(dotN);
        contribution *= saturate(1.0f - dist / surfel.radius);
        contribution = smoothstep(0.f, 1.f, contribution);

        coverage += contribution;

#ifdef USE_SURFEL_DEPTH
        {
            float2 uv = getSurfelDepthUV(surfelIndex, bias / dist, normal);
            float2 surfelDepth = gSurfelDepth.SampleLevel(gSurfelDepthSampler, uv, 0u);

            float mean = surfelDepth.x;
            float sqrMean = surfelDepth.y;

            if (dist > mean)
            {
                float variance = sqrMean - pow(mean, 2);
                contribution *= variance / (variance + pow(dist - mean, 2));
            }
        }
#endif

        SurfelRecycleInfo recycleInfo = gSurfelRecycleInfoBuffer[surfelIndex];

        // Delay blending until the surfel has accumulated enough frames of
        // samples (since samples update per-frame, ref `frame` not sample
        // count directly). The smoothstep takes `frame` as float — uint16_t
        // → float conversion is explicit to silence the warning.
        float blendDelayT = smoothstep(0.f, float(kDefaultBlendingDelay), float(recycleInfo.frame));
        indirectLighting += float4(surfel.radiance, 1.f) * contribution * blendDelayT;

        varianceEx += length(surfel.msmeData.variance) * contribution;
        rayCountEx += float(surfel.rayCount) * contribution;

        refCount = max(refCount, gSurfelRefCounter[surfelIndex]);
        life     = max(life, uint(recycleInfo.life));

        maxVariance = max(maxVariance, length(surfel.msmeData.variance));
    }

    if (indirectLighting.w > 0.f)
    {
        indirectLighting.xyz /= indirectLighting.w;
        indirectLighting.w    = saturate(indirectLighting.w);

        varianceEx /= indirectLighting.w;
        rayCountEx /= indirectLighting.w;

        // Write texture by overlay mode.
        if (kDefaultOverlayMode == 0u)
            gOutput[pixelPos] = indirectLighting;
        else if (kDefaultOverlayMode == 1u)
            gOutput[pixelPos] = float4(stepColor(maxVariance * kDefaultVarianceSensitivity, 0.8f, 0.5f), 1);
        else if (kDefaultOverlayMode == 2u)
            gOutput[pixelPos] = float4(stepColor(rayCountEx, 48.f, 32.f), 1);
        else if (kDefaultOverlayMode == 3u)
            gOutput[pixelPos] = float4(lerpColor(refCount / 256.f), 1);
        else if (kDefaultOverlayMode == 4u)
            gOutput[pixelPos] = float4(step(float(life), 0.f), step(1.f, float(life)), 0, 1);
        else if (kDefaultOverlayMode == 5u)
            gOutput[pixelPos] = float4(lerpColor(smoothstep(kDefaultPlacementThreshold, kDefaultRemovalThreshold, coverage)), 1);
    }
}
