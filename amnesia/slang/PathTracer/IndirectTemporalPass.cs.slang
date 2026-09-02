// Temporal accumulation for the path-traced indirect term.
//
// The path-trace pass produces a 1-spp demodulated indirect irradiance that is
// far too noisy to show. This pass is the temporal half of its denoiser:
// reproject last frame's result through the velocity texture, reject it on the
// same depth/normal disocclusion test the direct pass uses (kReprojZRelTol /
// kReprojNormalCos — one predicate, one place to tune), and blend the current
// sample into the surviving history as a running mean. The spatial half is
// DirectAtrousPass, which is bound a second time with these textures.
//
// The alpha channel is a frame COUNT (1..kIndirectMaxAccumFrames), not a 0..1
// blend weight: 1/count is the running-mean lerp factor here, and the à-trous
// pass reads the same channel as a per-pixel confidence for its
// kDirectVarianceBoost term (fresh pixels get a wider luminance gate). Storing a
// normalised alpha would break both.
//
// gIndirectHistory is the previous frame's resolved result: the à-trous output
// when the spatial filter runs, otherwise this pass's own output. Feeding the
// unfiltered accumulation back while the filter is on would re-inject the
// per-frame speckle the spatial filter just removed, and the running mean would
// converge to the noise rather than away from it. With the filter off
// (kAtrousIterations == 0) there is nothing to re-inject, and seeding the
// history here is what keeps the temporal-only path accumulating at all.

#include "../HostDefinitions.h"
#include "../Constants.h"

import SceneTypes;   // kReprojZRelTol, kReprojNormalCos, kIndirectMaxAccumFrames

// Pass-private (set 2).
[vk::binding(0, 2)] Texture2D<float4>   gIndirectCurrent;     // this frame's raw path trace (rgb irradiance)
[vk::binding(1, 2)] Texture2D<float4>   gIndirectKey;         // current surface key (viewZ, normal.xyz)
[vk::binding(2, 2)] Texture2D<float4>   gIndirectHistory;     // previous frame's resolved result (rgb, count)
[vk::binding(3, 2)] Texture2D<float4>   gIndirectKeyHistory;  // previous surface key (viewZ, normal.xyz)
[vk::binding(4, 2)] Texture2D<float2>   gVelocity;            // screen-space motion (RG16F)
[vk::binding(5, 2)] RWTexture2D<float4> gIndirectOut;         // output: (rgb accumulated, count)

// Temporal-history seed: the same value written to gIndirectOut, stored into the
// resolved texture so next frame's gIndirectHistory is valid even when
// kAtrousIterations == 0 and the à-trous loop never runs. When the loop does run,
// its last iteration overwrites this texture with the filtered result.
[vk::binding(6, 2)] RWTexture2D<float4> gIndirectResolvedOut;

[shader("compute")]
[numthreads(16, 16, 1)]
void csMain(uint3 tid : SV_DispatchThreadID)
{
    uint w, h;
    gIndirectOut.GetDimensions(w, h);
    int2 p = int2(tid.xy);
    if (p.x >= int(w) || p.y >= int(h))
        return;

    float3 curRgb = gIndirectCurrent.Load(int3(p, 0)).rgb;

    float4 ckey = gIndirectKey.Load(int3(p, 0));
    float  curZ = ckey.x;
    float3 curN = ckey.yzw;

    // Zero normal = miss / invalid surface (the path-trace pass writes that for
    // background). Pass it through with a count of 1 — nothing to accumulate, and
    // the à-trous pass must not divide by zero on the confidence.
    if (dot(curN, curN) <= 1.0e-8f)
    {
        gIndirectOut[p]         = float4(curRgb, 1.0f);
        gIndirectResolvedOut[p] = float4(curRgb, 1.0f);
        return;
    }
    curN = normalize(curN);

    // Reproject through the motion vector; reject (disocclusion) on depth/normal
    // mismatch. The key is point-sampled — it must not bilinear-blend across edges.
    float2 dims   = float2(w, h);
    float2 curUV  = (float2(p) + 0.5f) / dims;
    float2 histUV = curUV - gVelocity.Load(int3(p, 0)).xy;

    bool valid = !(any(histUV < 0.0f) || any(histUV > 1.0f));
    int2 ht = int2(0, 0);
    if (valid)
    {
        ht = clamp(int2(histUV * dims), int2(0), int2(dims) - 1);
        float4 k = gIndirectKeyHistory.Load(int3(ht, 0));
        float  histZ = k.x;
        float3 histN = k.yzw;
        valid = dot(histN, histN) > 1.0e-8f
             && abs(curZ - histZ) <= kReprojZRelTol * max(curZ, 1.0e-3f)
             && dot(curN, normalize(histN)) >= kReprojNormalCos;
    }

    float3 outRgb;
    float  count;
    if (valid)
    {
        float4 hist = gIndirectHistory.Load(int3(ht, 0));
        count  = min(hist.a + 1.0f, kIndirectMaxAccumFrames);
        outRgb = lerp(hist.rgb, curRgb, 1.0f / count);
    }
    else
    {
        count  = 1.0f;
        outRgb = curRgb;
    }

    gIndirectOut[p]         = float4(outRgb, count);
    gIndirectResolvedOut[p] = float4(outRgb, count);
}
