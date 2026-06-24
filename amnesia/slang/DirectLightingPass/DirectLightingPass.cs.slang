// Direct-lighting pass — stage 1 of 3 (ReSTIR DI temporal reuse).
//
// Per pixel: reconstruct the surface from the V-buffer (RT hit, raster
// fallback), build a light-selection reservoir by streaming RIS over the cell's
// lights (unshadowed-luminance target, NO shadow ray), then temporally reuse the
// previous frame's reservoir — reprojected through the velocity texture and
// rejected on a depth/normal disocclusion test, with the previous confidence M
// clamped so stale lighting can't dominate. Writes the merged reservoir
// (consumed by DirectSpatialReusePass) and the current surface key.
//
// The single shadow ray + resolve to radiance happens in the spatial pass; this
// pass traces nothing. Demodulated irradiance throughout — the composite applies
// albedo once, to direct + indirect together.

#include "../HostDefinitions.h"
#include "../Constants.h"

import SceneTypes;
import Scene;
import bindless;                       // gPackedHitInfo, lights, grid
import HitInfo;
import PerFrame.resource;
import SurfelGI.SurfelShade;           // prepareDiffuseShading / buildDirectReservoir / directTargetPdf
import Random;
import Reservoir;

// Pass-private (set 2).
[vk::binding(0, 2)] Texture2D<uint4>    gPackedHitInfoRaster;  // raster V-buffer fallback
[vk::binding(1, 2)] Texture2D<float2>   gVelocity;             // screen-space motion (RG16F)
[vk::binding(2, 2)] Texture2D<float4>   gReservoirHistory;     // previous frame's final reservoir
[vk::binding(3, 2)] RWTexture2D<float4> gReservoirOut;         // output: temporally-merged reservoir
[vk::binding(4, 2)] Texture2D<float4>   gDirectKeyHistory;     // previous surface key (viewZ, normal.xyz)
[vk::binding(5, 2)] RWTexture2D<float4> gDirectKeyOut;         // output: current surface key

[shader("compute")]
[numthreads(16, 16, 1)]
void csMain(uint3 tid : SV_DispatchThreadID)
{
    uint outW, outH;
    gReservoirOut.GetDimensions(outW, outH);
    uint2 pixelPos = tid.xy;
    if (pixelPos.x >= outW || pixelPos.y >= outH)
        return;

    HitInfo hitInfo = HitInfo(gPackedHitInfo[pixelPos]);
    if (!hitInfo.isValid())
        hitInfo = HitInfo(gPackedHitInfoRaster.Load(int3(pixelPos, 0)));
    if (!hitInfo.isValid())
    {
        gReservoirOut[pixelPos] = float4(0.f, 0.f, 0.f, 0.f); // empty reservoir
        gDirectKeyOut[pixelPos] = float4(0.f, 0.f, 0.f, 0.f); // zero normal → rejected
        return;
    }

    TriangleHit th  = hitInfo.getTriangleHit();
    uint materialID = gSceneObjects[th.instanceID].materialID;
    VertexData v    = getVertexData(th);
    UniformObject obj = gSceneObjects[th.instanceID];
    DiffuseShading sd = prepareDiffuseShading(v, materialID, obj.illuminationAmount,
                                              /*decodeRoughness*/ false, /*applyNormalMap*/ true);

    float  curZ = -mul(gPerFrame.viewMat, float4(sd.posW, 1.0f)).z;
    // Reprojection/disocclusion key uses the GEOMETRIC normal (not the bumped
    // sd.normalW) so high-frequency normal-map detail doesn't cause false
    // history rejections; lighting above still uses the bumped sd.normalW.
    float3 curN = normalize(v.normalW);

    // Reproject through the motion vector; reject (disocclusion) on depth/normal
    // mismatch. The key is point-sampled — it must not bilinear-blend across edges.
    float2 dims   = float2(outW, outH);
    float2 curUV  = (float2(pixelPos) + 0.5f) / dims;
    float2 histUV = curUV - gVelocity.Load(int3(pixelPos, 0)).xy;

    bool valid = !(any(histUV < 0.0f) || any(histUV > 1.0f));
    int2 ht = int2(0, 0);
    if (valid)
    {
        ht = clamp(int2(histUV * dims), int2(0), int2(dims) - 1);
        float4 k = gDirectKeyHistory.Load(int3(ht, 0));
        float  histZ = k.x;
        float3 histN = k.yzw;
        valid = abs(curZ - histZ) <= kReprojZRelTol * max(curZ, 1.0e-3f)
             && dot(curN, normalize(histN)) >= kReprojNormalCos;
    }

    RNG rng;
    rng.init(pixelPos, gPerFrame.totalFrames);

    // Build this frame's reservoir (RIS over the cell lights), then fold it into
    // the combination as the leading candidate.
    Reservoir cur = buildDirectReservoir(sd, rng);

    Reservoir R; R.init();
    R.merge(cur, cur.pHatY, rng);

    if (valid)
    {
        // Reproject the previous reservoir (point-sampled — packed light index
        // can't interpolate). Clamp its confidence so it can't outweigh the
        // fresh sample by more than kReservoirMClamp×.
        Reservoir prev = unpackReservoir(gReservoirHistory.Load(int3(ht, 0)));
        prev.M = min(prev.M, kReservoirMClamp * max(cur.M, 1.0f));
        float pHatPrev = directTargetPdf(sd, prev.y);
        R.merge(prev, pHatPrev, rng);
    }

    R.finalizeCombine();

    gReservoirOut[pixelPos] = packReservoir(R);
    gDirectKeyOut[pixelPos] = float4(curZ, curN);
}
