// Direct-lighting pass — computes soft-shadowed analytic direct lighting per
// pixel and temporally accumulates it (reprojected through the velocity texture)
// so one noisy light-disk shadow sample per frame resolves into a smooth
// penumbra. The MainCompositePass reads the accumulated result instead of
// evaluating lights inline.
//
// Per pixel: reconstruct the surface from the V-buffer (RT hit, raster
// fallback), evaluate evalAnalyticLightSoft (one jittered shadow ray per light),
// reproject the previous accumulated direct by the motion vector, neighborhood-
// clamp it against this frame's 3x3 (in-tile) to limit ghosting, and EMA-blend.

#include "../HostDefinitions.h"
#include "../Constants.h"

import SceneTypes;
import Scene;
import bindless;                       // gPackedHitInfo, gRtAccel, gMaterialSampler, lights, grid
import HitInfo;
import PerFrame.resource;
import SurfelGI.SurfelShade;           // DiffuseShading / prepareDiffuseShading / evalAnalyticLightSoft
import Random;

// Pass-private (set 2).
[vk::binding(0, 2)] Texture2D<uint4>   gPackedHitInfoRaster;  // raster V-buffer fallback
[vk::binding(1, 2)] Texture2D<float2>  gVelocity;             // screen-space motion (RG16F)
[vk::binding(2, 2)] Texture2D<float4>  gDirectHistory;        // previous accumulated direct (rgb, count)
[vk::binding(3, 2)] RWTexture2D<float4> gDirectLighting;      // output: current accumulated direct
[vk::binding(4, 2)] Texture2D<float4>  gDirectKeyHistory;     // previous surface key (viewZ, normal.xyz)
[vk::binding(5, 2)] RWTexture2D<float4> gDirectKeyOut;        // output: current surface key

[shader("compute")]
[numthreads(16, 16, 1)]
void csMain(uint3 tid : SV_DispatchThreadID)
{
    uint outW, outH;
    gDirectLighting.GetDimensions(outW, outH);
    uint2 pixelPos = tid.xy;
    if (pixelPos.x >= outW || pixelPos.y >= outH)
        return;   // edge threads of the rounded-up dispatch

    // This-frame soft-shadowed direct (albedo baked in, matching the old inline
    // path). Miss → zero.
    HitInfo hitInfo = HitInfo(gPackedHitInfo[pixelPos]);
    if (!hitInfo.isValid())
        hitInfo = HitInfo(gPackedHitInfoRaster.Load(int3(pixelPos, 0)));
    if (!hitInfo.isValid())
    {
        gDirectLighting[pixelPos] = float4(0.f, 0.f, 0.f, 0.f);
        gDirectKeyOut[pixelPos]   = float4(0.f, 0.f, 0.f, 0.f); // zero normal → always rejected
        return;
    }

    TriangleHit th  = hitInfo.getTriangleHit();
    uint materialID = gScene.getMaterialID(th.instanceID);
    VertexData v    = gScene.getVertexData(th);
    UniformObject obj = gScene.getObject(th.instanceID);
    DiffuseShading sd = prepareDiffuseShading(v, materialID, obj.illuminationAmount);

    // Current surface key for disocclusion rejection: positive view-space depth
    // and world normal.
    float  curZ = -mul(gPerFrame.viewMat, float4(sd.posW, 1.0f)).z;
    float3 curN = sd.normalW;

    // Reproject through the motion vector and REJECT the history when its
    // reprojected surface doesn't match this pixel (off-screen, or depth / normal
    // mismatch) — a disocclusion, which would otherwise drag a different surface's
    // lighting (camera-motion smear). The key is point-sampled: it must not
    // bilinear-blend across edges.
    float2 dims   = float2(outW, outH);
    float2 curUV  = (float2(pixelPos) + 0.5f) / dims;
    float2 histUV = curUV - gVelocity.Load(int3(pixelPos, 0)).xy;

    bool valid = !(any(histUV < 0.0f) || any(histUV > 1.0f));
    if (valid)
    {
        int2 ht = clamp(int2(histUV * dims), int2(0), int2(dims) - 1);
        float4 k = gDirectKeyHistory.Load(int3(ht, 0));
        float  histZ = k.x;
        float3 histN = k.yzw;
        valid = abs(curZ - histZ) <= kReprojZRelTol * max(curZ, 1.0e-3f)
             && dot(curN, normalize(histN)) >= kReprojNormalCos;
    }

    RNG rng;
    rng.init(pixelPos, gPerFrame.totalFrames);

    // Temporal accumulation as a history-length running mean (alpha = 1/count,
    // floored).
    float3 result;
    float  count;
    if (valid)
    {
        // One cheap importance-sampled (RIS) shadow ray refines the running mean.
        float3 curDirect = evalAnalyticLightStochastic(sd, rng);
        float4 h = gDirectHistory.SampleLevel(gMaterialSampler, histUV, 0.0f);
        count = min(h.a + 1.0f, kDirectMaxAccum);
        float alpha = max(1.0f / count, kDirectTemporalAlpha);
        result = lerp(h.rgb, curDirect, alpha);
    }
    else
    {
        // Truly novel (no matching neighbor, e.g. a hard cut) → one RIS sample.
        result = evalAnalyticLightStochastic(sd, rng);
        count  = 1.0f;
    }

    gDirectLighting[pixelPos] = float4(result, count);
    gDirectKeyOut[pixelPos]   = float4(curZ, curN);
}
