// Direct-lighting pass — stage 2 of 3 (ReSTIR DI spatial reuse + resolve).
//
// Per pixel: reconstruct the surface, take the temporally-merged reservoir from
// DirectLightingPass, and merge a few same-surface neighbours' reservoirs
// (geometry-rejected on the depth/normal key, so light can't leak across
// surfaces). Then RESOLVE: one soft (blue-noise disk) shadow ray for the chosen
// light, weighted by the reservoir's UCW → demodulated irradiance that NrdPack
// sums into REBLUR's diffuse channel, which denoises it together with the path
// tracer's indirect bounce.
//
// This pass does NO temporal filtering of the resolved irradiance. It used to
// run an EMA accumulation here, but REBLUR now owns all temporal filtering for
// the diffuse channel; keeping a second accumulator upstream would double-filter
// and add lag on moving lights and shadows.
//
// The merged reservoir is also written out as the next frame's temporal history.
// One shadow ray per pixel per frame; the reuse drives the effective candidate
// count into the hundreds.

#include "../HostDefinitions.h"
#include "../Constants.h"

import SceneTypes;
import Scene;
import bindless;                       // gPackedHitInfo, gRtAccel, lights, grid
import HitInfo;
import PerFrame.resource;
import Shading;                        // prepareDiffuseShading / directTargetPdf / resolveDirectReservoir
import Random;
import Reservoir;

// Pass-private (set 2). Slot 0 is a gap; the remaining bindings keep their
// numbers so the host-side layout stays stable. Slots 5-7 (velocity and the
// two history textures) were dropped with the temporal accumulation.
[vk::binding(1, 2)] Texture2D<float4>   gReservoirIn;          // temporally-merged reservoir
[vk::binding(2, 2)] Texture2D<float4>   gDirectKey;            // current surface key (viewZ, normal.xyz)
[vk::binding(3, 2)] RWTexture2D<float4> gReservoirOut;         // output: final reservoir (next frame's history)
[vk::binding(4, 2)] RWTexture2D<float4> gDirectLighting;       // output: resolved demodulated irradiance (rgb; .a unused)

[shader("compute")]
[numthreads(16, 16, 1)]
void csMain(uint3 tid : SV_DispatchThreadID)
{
    uint outW, outH;
    gDirectLighting.GetDimensions(outW, outH);
    int2 p = int2(tid.xy);
    if (p.x >= int(outW) || p.y >= int(outH))
        return;

    // `gPackedHitInfo` already carries the raster hit (only the barycentrics
    // are POM-perturbed), so a miss here is a real miss.
    HitInfo hitInfo = HitInfo(gPackedHitInfo[uint2(p)]);
    if (!hitInfo.isValid())
    {
        gReservoirOut[p]  = float4(0.f, 0.f, 0.f, 0.f);
        gDirectLighting[p] = float4(0.f, 0.f, 0.f, 0.f);
        return;
    }

    TriangleHit th  = hitInfo.getTriangleHit();
    uint materialID = gSceneObjects[th.instanceID].materialID;
    VertexData v    = getVertexData(th);
    UniformObject obj = gSceneObjects[th.instanceID];
    DiffuseShading sd = prepareDiffuseShading(v, materialID, obj.illuminationAmount,
                                              /*decodeRoughness*/ false, /*applyNormalMap*/ true);

    float4 ckey = gDirectKey.Load(int3(p, 0));
    float  curZ = ckey.x;
    float3 curN = normalize(ckey.yzw);

    // Decorrelate this pass's RNG stream from the temporal pass (different pixel
    // hash) so the neighbour picks and the resolve disk sample aren't aliased.
    RNG rng;
    rng.init(uint2(p) + uint2(13u, 37u), gPerFrame.totalFrames);

    // Start the combination from the center (temporal) reservoir.
    Reservoir R; R.init();
    Reservoir c = unpackReservoir(gReservoirIn.Load(int3(p, 0)));
    R.merge(c, directTargetPdf(sd, c.y), rng);

    // Merge a few same-surface neighbours.
    for (int i = 0; i < kSpatialSamples; ++i)
    {
        float2 o = (rng.next_float2() * 2.0f - 1.0f) * kSpatialRadius;
        int2   q = p + int2(int(round(o.x)), int(round(o.y)));
        if (q.x < 0 || q.y < 0 || q.x >= int(outW) || q.y >= int(outH)) continue;
        if (q.x == p.x && q.y == p.y) continue;

        float4 nkey = gDirectKey.Load(int3(q, 0));
        float3 nN   = nkey.yzw;
        if (dot(nN, nN) <= 1.0e-8f) continue;                       // invalid neighbour
        if (abs(curZ - nkey.x) > kReprojZRelTol * max(curZ, 1.0e-3f)) continue;
        if (dot(curN, normalize(nN)) < kReprojNormalCos) continue;  // geometry reject

        Reservoir nb = unpackReservoir(gReservoirIn.Load(int3(q, 0)));
        R.merge(nb, directTargetPdf(sd, nb.y), rng);
    }

    R.finalizeCombine();
    gReservoirOut[p] = packReservoir(R);   // next frame's temporal history

    // Resolve the chosen light with one soft (blue-noise disk) shadow ray.
    float3 curResolved = resolveDirectReservoir(sd, R, rng);

    // No temporal accumulation here: REBLUR filters this channel downstream.
    // .a is written as 1 only to keep the RGBA16F layout; nothing reads it.
    gDirectLighting[p] = float4(curResolved, 1.0f);
}
