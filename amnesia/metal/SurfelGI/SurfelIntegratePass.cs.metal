// Hand-written Metal port of amnesia/slang/SurfelGI/SurfelIntegratePass.cs.slang
//
// Per-valid-surfel accumulator (numthreads 32,1,1). Reads all ray results for
// the surfel, averages with PDF weighting + MSME, optionally shares radiance
// from neighbour surfels, and writes the surfel-depth atlas borders for the
// filtered reads in the generation/evaluation passes.
//
// Self-contained. Struct layouts use packed_float3 to match the Slang scalar
// block layout the host uploads (Surfel == 104B, SurfelRayResult == 48B,
// CellInfo == 8B) — bare float3 (16-byte aligned) would NOT match.
//
// Bindings:
//   set0 (bindless arg buffer) buffer(0); [[id]] = kBinding* (Constants.h)
//     gSurfelCounter 10  gSurfelBuffer 11  gSurfelValidIndexBuffer 13
//     gSurfelRayResult 17  gCellInfo 18  gCellToSurfel 19
//   gPerFrame      buffer(1)
//   gSurfelDepthMap texture(0) [read_write]   gSurfelDepth texture(1) [sample]
//   gSurfelDepthSampler is bindless -> Set0 id(35) (kBindingSurfelDepthSampler)
#include <metal_stdlib>
using namespace metal;

constant float kCellUnit      = 0.5f;   // world units per cell (used by lookupSurfelCell)
constant uint2 kSurfelDepthTextureHalfUnit = uint2(3u, 3u);
constant float kDefaultVarianceSensitivity = 40.0f;
constant float kDefaultShortMeanWindow     = 0.03f;
constant uint  kSurfelCounterValid = 0u;

// --- Shared struct layouts + utilities: mirrored from the Slang modules (see
// the .metalh, flattened into this file at stage time by
// cmake/flatten_metal_includes.py).
#include "SceneTypes.metalh"            // SceneConstants
#include "SurfelGI/SurfelTypes.metalh"  // Surfel, CellInfo, SurfelRayResult (+ MSMEData)
#include "SurfelGI/SurfelUtils.metalh"  // cell math, octEncode/octDecode, get_tangentspace, getSurfelDepthUV (+ M_PI_, kCellDimension, depth-atlas constants)
#include "SurfelGI/SurfelGather.metalh" // SurfelHit, testSurfelCoverage, surfelKernelWeight, surfelDepthWeight, lookupSurfelCell

// Set-0 bindless argument buffer (subset this kernel reads). [[id]] = kBinding*.
struct Set0 {
    device uint*            gSurfelCounter         [[id(10)]];
    device Surfel*          gSurfelBuffer          [[id(11)]];
    device uint*            gSurfelValidIndexBuffer [[id(13)]];
    device SurfelRayResult* gSurfelRayResultBuffer [[id(17)]];
    device CellInfo*        gCellInfoBuffer        [[id(18)]];
    device uint*            gCellToSurfelBuffer    [[id(19)]];
    sampler                 gSurfelDepthSampler    [[id(35)]];   // kBindingSurfelDepthSampler
};
// Set-1 program argument buffer (buffer(1)); [[id]] = the resource's mtl.index
// in the C++ kSurfelIntegrateCs table (RIProgram encodes set 1/2 by mtl.index).
struct Set1 {
    constant SceneConstants* gPerFrame [[id(0)]];
    texture2d<float, access::read_write> gSurfelDepthMap [[id(1)]];
    texture2d<float, access::sample>     gSurfelDepth    [[id(2)]];
};

// --- MultiscaleMeanEstimator (pass-local) ------------------------------------
static float3 MSME(float3 y, thread MSMEData& data, float shortWindowBlend) {
    float3 mean = data.mean;
    float3 shortMean = data.shortMean;
    float  vbbr = data.vbbr;
    float3 variance = data.variance;
    float  inconsistency = data.inconsistency;

    {   // Suppress fireflies.
        float3 dev = sqrt(max(float3(1e-5f), variance));
        float3 highThreshold = 0.1f + shortMean + dev * 8.0f;
        float3 overflow = max(float3(0.0f), y - highThreshold);
        y -= overflow;
    }

    float3 delta = y - shortMean;
    shortMean = mix(shortMean, y, shortWindowBlend);
    float3 delta2 = y - shortMean;

    float varianceBlend = shortWindowBlend * 0.5f;
    variance = mix(variance, delta * delta2, varianceBlend);
    float3 dev = sqrt(max(float3(1e-5f), variance));

    float3 shortDiff = mean - shortMean;
    float relativeDiff = dot(float3(0.299f, 0.587f, 0.114f), abs(shortDiff) / max(float3(1e-5f), dev));
    inconsistency = mix(inconsistency, relativeDiff, 0.08f);

    float varianceBasedBlendReduction =
        clamp(dot(float3(0.299f, 0.587f, 0.114f), 0.5f * shortMean / max(float3(1e-5f), dev)), 1.0f / 32.0f, 1.0f);

    float3 catchUpBlend = clamp(smoothstep(0.0f, 1.0f, relativeDiff * max(0.02f, inconsistency - 0.2f)), float3(1.0f / 256.0f), float3(1.0f));
    catchUpBlend *= vbbr;

    vbbr = mix(vbbr, varianceBasedBlendReduction, 0.1f);
    mean = mix(mean, y, saturate(catchUpBlend));

    data.mean = mean;
    data.shortMean = shortMean;
    data.vbbr = vbbr;
    data.variance = variance;
    data.inconsistency = inconsistency;
    return mean;
}

// Depth-atlas (RG32F) read/modify/write helpers.
static float2 dmRead(texture2d<float, access::read_write> t, uint2 c) { return t.read(c).xy; }
static void   dmWrite(texture2d<float, access::read_write> t, uint2 c, float2 v) { t.write(float4(v, 0.0f, 0.0f), c); }

[[kernel]] void csMain(uint3 dispatchThreadId [[thread_position_in_grid]],
                       device Set0& set0 [[buffer(0)]],
                       device Set1& set1 [[buffer(1)]]) {
    constant SceneConstants* gPerFrame = set1.gPerFrame;
    texture2d<float, access::read_write> gSurfelDepthMap = set1.gSurfelDepthMap;
    texture2d<float, access::sample>     gSurfelDepth    = set1.gSurfelDepth;
    uint validSurfelCount = set0.gSurfelCounter[kSurfelCounterValid];
    if (dispatchThreadId.x >= validSurfelCount)
        return;

    uint surfelIndex = set0.gSurfelValidIndexBuffer[dispatchThreadId.x];
    Surfel surfel = set0.gSurfelBuffer[surfelIndex];
    if (surfel.rayCount == 0u)
        return;

    uint denomX = kSurfelDepthTextureRes.x / kSurfelDepthTextureUnit.x;  // 585
    uint denomY = kSurfelDepthTextureRes.x / kSurfelDepthTextureUnit.y;  // 585
    uint2 surfelDepthLT = uint2(surfelIndex % denomX, surfelIndex / denomY) * kSurfelDepthTextureUnit;
    uint  unitX = kSurfelDepthTextureUnit.x;
    uint  unitY = kSurfelDepthTextureUnit.y;

    float3 surfelNormal = float3(surfel.normal);
    float3 surfelRadiance = float3(0.0f);
    for (uint rayIndex = 0u; rayIndex < surfel.rayCount; ++rayIndex) {
        SurfelRayResult rr = set0.gSurfelRayResultBuffer[surfel.rayOffset + rayIndex];
        float3 Lr       = float3(rr.radiance);
        float3 dirLocal = float3(rr.dirLocal);
        float3 dirWorld = float3(rr.dirWorld);
        float  pdf      = rr.pdf;
        float  surfelDepth = (rr.firstRayLength > 0.0f)
                           ? clamp(rr.firstRayLength, 0.0f, surfel.radius)
                           : surfel.radius;

        float2 uv = octEncode(normalize(dirLocal));
        // Mirror the Slang lowering exactly (incl. its unsigned signedUV math).
        int2 off = int2(int(float(int(sign(uv.x))) * round(abs(uv.x * float(kSurfelDepthTextureHalfUnit.x - 1u)))),
                        int(float(int(sign(uv.y))) * round(abs(uv.y * float(kSurfelDepthTextureHalfUnit.y - 1u)))));
        uint2 surfelDepthTextureOffset = kSurfelDepthTextureHalfUnit + uint2(off);
        uint2 surfelDepthTextureCoord  = surfelDepthLT + surfelDepthTextureOffset;

        uint2 sUV = (uint2(int2(surfelDepthTextureOffset)) - kSurfelDepthTextureHalfUnit)
                  / (kSurfelDepthTextureHalfUnit - uint2(int2(1, 1)));
        float depthWeight = saturate(dot(dirLocal, octDecode(float2(sUV))));

        float2 cur = dmRead(gSurfelDepthMap, surfelDepthTextureCoord);
        float2 delta2 = float2(surfelDepth, surfelDepth * surfelDepth) - cur;
        dmWrite(gSurfelDepthMap, surfelDepthTextureCoord, cur + 1e-2f * delta2 * depthWeight);

        // Monte Carlo estimator for irradiance/pi (see Slang note).
        surfelRadiance += Lr * dot(dirWorld, surfelNormal) / (M_PI_ * max(1e-12f, pdf));
    }
    surfelRadiance /= float(surfel.rayCount);

    // Border replication for the surfel-depth atlas tile.
    for (uint xx = 1u; xx < unitX - 1u; ++xx) {
        dmWrite(gSurfelDepthMap, surfelDepthLT + uint2(xx, 0u),         dmRead(gSurfelDepthMap, surfelDepthLT + uint2(xx, 1u)));
        dmWrite(gSurfelDepthMap, surfelDepthLT + uint2(xx, unitY - 1u), dmRead(gSurfelDepthMap, surfelDepthLT + uint2(xx, unitY - 2u)));
    }
    for (uint yy = 1u; yy < unitY - 1u; ++yy) {
        dmWrite(gSurfelDepthMap, surfelDepthLT + uint2(0u, yy),         dmRead(gSurfelDepthMap, surfelDepthLT + uint2(1u, yy)));
        dmWrite(gSurfelDepthMap, surfelDepthLT + uint2(unitX - 1u, yy), dmRead(gSurfelDepthMap, surfelDepthLT + uint2(unitX - 2u, yy)));
    }
    dmWrite(gSurfelDepthMap, surfelDepthLT + uint2(0u, 0u),
            (dmRead(gSurfelDepthMap, surfelDepthLT + uint2(0u, 1u)) + dmRead(gSurfelDepthMap, surfelDepthLT + uint2(1u, 0u))) * 0.5f);
    dmWrite(gSurfelDepthMap, surfelDepthLT + uint2(0u, unitY - 1u),
            (dmRead(gSurfelDepthMap, surfelDepthLT + uint2(0u, unitY - 2u)) + dmRead(gSurfelDepthMap, surfelDepthLT + uint2(1u, unitY - 1u))) * 0.5f);
    dmWrite(gSurfelDepthMap, surfelDepthLT + uint2(unitX - 1u, unitY - 1u),
            (dmRead(gSurfelDepthMap, surfelDepthLT + uint2(unitX - 1u, unitY - 2u)) + dmRead(gSurfelDepthMap, surfelDepthLT + uint2(unitX - 2u, unitY - 1u))) * 0.5f);
    dmWrite(gSurfelDepthMap, surfelDepthLT + uint2(unitX - 1u, 0u),
            (dmRead(gSurfelDepthMap, surfelDepthLT + uint2(unitX - 1u, 1u)) + dmRead(gSurfelDepthMap, surfelDepthLT + uint2(unitX - 2u, 0u))) * 0.5f);

    // Share radiance from neighbour surfels covering this one.
    float4 sharedRadiance = float4(0.0f);
    uint flattenIndex;
    CellInfo cellInfo;
    float3 surfelPos = float3(surfel.position);
    if (lookupSurfelCell(surfelPos, gPerFrame->posW, flattenIndex, cellInfo, set0.gCellInfoBuffer)) {
        const float3 centerPos    = surfelPos;
        const float3 centerNormal = surfelNormal;
        const float  affectRadius = kCellUnit * sqrt(2.0f);

        for (uint i = 0u; i < cellInfo.surfelCount; ++i) {
            uint neiSurfelIndex = set0.gCellToSurfelBuffer[cellInfo.cellToSurfelBufferOffset + i];
            Surfel neiSurfel = set0.gSurfelBuffer[neiSurfelIndex];

            // Reference behavior: raw (unnormalized) normals, fixed cell-diagonal
            // affect radius, center surfel's depth function. Not bugs to "fix".
            SurfelHit hit;
            if (!testSurfelCoverage(centerPos, centerNormal, float3(neiSurfel.position),
                                    float3(neiSurfel.normal), affectRadius, hit))
                continue;

            float contribution = surfelKernelWeight(hit.dotN, hit.dist, affectRadius);
            contribution *= surfelDepthWeight(surfelIndex, hit.dirUnit, centerNormal, hit.dist,
                                              gSurfelDepth, set0.gSurfelDepthSampler);
            sharedRadiance += float4(float3(neiSurfel.radiance), 1.0f) * contribution;
        }

        if (sharedRadiance.w > 0.0f) {
            sharedRadiance.xyz /= sharedRadiance.w;
            surfelRadiance = mix(surfelRadiance, sharedRadiance.xyz,
                                 saturate(length(float3(surfel.msmeData.variance)) * kDefaultVarianceSensitivity));
        }
    }

    float3 mean = MSME(surfelRadiance, surfel.msmeData, kDefaultShortMeanWindow);
    surfel.radiance = packed_float3(mean);
    set0.gSurfelBuffer[surfelIndex] = surfel;
}
