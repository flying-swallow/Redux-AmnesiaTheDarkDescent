// Water-guide reduction: full-resolution capture -> one representative per 2x2 block.
//
// Resources (set 2; names are resolved by host reflection):
//   gWaterGuidePosViewZ       RGBA32F  input  (xyz = un-biased world position, w = positive linear viewZ)
//   gWaterGuideNormalWeight   RGBA16F  input  (xyz = reflection normal, w = reflection contribution weight)
//   gWaterGuideVelocity       RG16F    input  (currentUV - previousUV, normalized to the full overscan image)
//   gWaterGuideHalfPosViewZ   RGBA32F  output (selected position and viewZ)
//   gWaterGuideHalfNormalWeight RGBA16F output (selected normal and weight)
//   gWaterGuideHalfVelocity   RG16F    output (selected full-image-normalized motion)
//   gWaterGuideHalfNormalSpread R16F    output (effective GGX alpha [0,1])
// A zero guide means uncovered or invalid; a covered guide with zero weight is
// also non-contributing and is reduced to zero. The capture represents ONE water
// renderable per clear: the host clears and reuses it for each renderable in the
// existing back-to-front order. It must never replace the opaque V-buffer or
// collapse overlapping water objects into one nearest-water guide.
//
// A later consumer forms the reflection-ray root as position + normal * ray
// epsilon. The stored position is deliberately UN-biased; the ray direction is
// formed from that same normal and the camera-to-surface direction.

import Water.WaterNormalSpread;

// Pass-private resources (set 2).
[vk::binding(0, 2)] Texture2D<float4>   gWaterGuidePosViewZ;
[vk::binding(1, 2)] Texture2D<float4>   gWaterGuideNormalWeight;
[vk::binding(2, 2)] Texture2D<float2>   gWaterGuideVelocity;
[vk::binding(3, 2)] RWTexture2D<float4> gWaterGuideHalfPosViewZ;
[vk::binding(4, 2)] RWTexture2D<float4> gWaterGuideHalfNormalWeight;
[vk::binding(5, 2)] RWTexture2D<float2> gWaterGuideHalfVelocity;
[vk::binding(6, 2)] RWTexture2D<float>  gWaterGuideHalfNormalSpread;

bool isValidWaterGuide(float4 posViewZ, float4 normalWeight)
{
    bool finitePosition = isfinite(posViewZ.x) && isfinite(posViewZ.y)
                       && isfinite(posViewZ.z);
    float normalLengthSquared = dot(normalWeight.xyz, normalWeight.xyz);
    return finitePosition
        && posViewZ.w > 0.0f
        && normalWeight.w > 0.0f
        && normalLengthSquared > 1.0e-8f;
}

[shader("compute")]
[numthreads(16, 16, 1)]
void csMain(uint3 tid : SV_DispatchThreadID)
{
    uint fullW, fullH;
    uint boundHalfW, boundHalfH;
    gWaterGuidePosViewZ.GetDimensions(fullW, fullH);
    gWaterGuideHalfPosViewZ.GetDimensions(boundHalfW, boundHalfH);
    uint boundSpreadW, boundSpreadH;
    gWaterGuideHalfNormalSpread.GetDimensions(boundSpreadW, boundSpreadH);

    // Ceil(full extent / 2), including odd-width and odd-height tails. The
    // bound destination dimensions protect every write if allocation is short;
    // the expected extent also prevents work on an oversized destination.
    uint halfW = (fullW + 1u) / 2u;
    uint halfH = (fullH + 1u) / 2u;
    uint2 p = tid.xy;
    if (p.x >= boundHalfW || p.y >= boundHalfH
        || p.x >= boundSpreadW || p.y >= boundSpreadH
        || p.x >= halfW || p.y >= halfH)
        return;

    // Category (b): p and sourceP are integer addresses, not camera samples or
    // normalized UVs. This explicit half-texel -> full-texel block mapping reads
    // the jittered full-resolution guide that rasterization already produced;
    // no half-grid jitter offset or UV rescaling belongs here.
    uint2 blockOrigin = p * 2u;
    bool  found = false;
    float bestViewZ = 0.0f;
    float4 bestPosViewZ = float4(0.0f);
    float4 bestNormalWeight = float4(0.0f);
    float2 bestVelocity = float2(0.0f);
    float3 candidateNormals[4];
    float candidateViewZ[4];
    uint candidateCount = 0u;

    // Fixed order is (0,0), (1,0), (0,1), (1,1). The strict '<' below keeps
    // the first sample on an exact viewZ tie, making the representative
    // deterministic.
    for (uint i = 0u; i < 4u; ++i)
    {
        uint2 sourceP = blockOrigin + uint2(i & 1u, i >> 1u);
        if (sourceP.x >= fullW || sourceP.y >= fullH)
            continue;

        float4 posViewZ = gWaterGuidePosViewZ.Load(int3(sourceP, 0));
        float4 normalWeight = gWaterGuideNormalWeight.Load(int3(sourceP, 0));
        if (!isValidWaterGuide(posViewZ, normalWeight))
            continue;

        // The guide pass emits unit normals, but RGBA16F round-tripping is not
        // required to preserve their length. Cache a normalized candidate so
        // the spread is a directional statistic rather than a format artifact.
        candidateNormals[candidateCount] = normalize(normalWeight.xyz);
        candidateViewZ[candidateCount] = posViewZ.w;
        candidateCount++;

        if (!found || posViewZ.w < bestViewZ)
        {
            found = true;
            bestViewZ = posViewZ.w;
            bestPosViewZ = posViewZ;
            bestNormalWeight = normalWeight;
            bestVelocity = gWaterGuideVelocity.Load(int3(sourceP, 0));
        }
    }

    if (found)
    {
        // The reducer sees exactly the valid full-resolution candidates that
        // define this half-resolution texel, including partially covered
        // blocks. That is the right place to measure guide-normal variation:
        // fragment ddx/ddy is a 2x2 quad finite difference polluted by helper
        // lanes and by fragments rejected by the guide's validity gates, while
        // this path needs no extra full-resolution render target.
        //
        // Keep only candidates on the representative's surface. A 1% relative
        // viewZ window prevents a block straddling a depth discontinuity from
        // turning the two surfaces' normal difference into false roughness.
        const float kSameSurfaceViewZRelativeEpsilon = 0.01f;
        float3 normalSum = float3(0.0f);
        uint sameSurfaceCount = 0u;
        for (uint i = 0u; i < candidateCount; ++i)
        {
            if (abs(candidateViewZ[i] - bestViewZ)
                <= kSameSurfaceViewZRelativeEpsilon * bestViewZ)
            {
                normalSum += candidateNormals[i];
                sameSurfaceCount++;
            }
        }
        float normalSpread = waterGuideNormalSpreadAlpha(normalSum,
                                                          sameSurfaceCount);

        // Category (b): copy all guides from the same nearest representative;
        // motion remains currentUV - previousUV in the full-resolution UV image
        // and is not doubled merely because the destination grid is half-size.
        gWaterGuideHalfPosViewZ[p] = bestPosViewZ;
        gWaterGuideHalfNormalWeight[p] = bestNormalWeight;
        gWaterGuideHalfVelocity[p] = bestVelocity;
        gWaterGuideHalfNormalSpread[p] = normalSpread;
    }
    else
    {
        // Explicit zeros prevent a reused or dirty half-resolution texture from
        // leaking a stale guide when the whole source block is empty/invalid.
        gWaterGuideHalfPosViewZ[p] = float4(0.0f);
        gWaterGuideHalfNormalWeight[p] = float4(0.0f);
        gWaterGuideHalfVelocity[p] = float2(0.0f);
        gWaterGuideHalfNormalSpread[p] = 0.0f;
    }
}
