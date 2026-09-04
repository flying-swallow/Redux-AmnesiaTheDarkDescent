// NRD front-end packing for the path-traced lighting inputs.
//
// REBLUR splits by LOBE at the primary vertex, not by direct/indirect: its
// specular path needs roughness-driven reprojection and virtual history, so a
// channel mixing lobes is invalid input. This pass therefore sums the ReSTIR DI
// direct term (diffuse-only, already albedo-demodulated by
// DirectSpatialReusePass) into the diffuse channel alongside the path tracer's
// indirect diffuse, and packs the GGX lobe as the specular channel.
//
// The sum lives here rather than in PathTracePass so the tracer stays free of a
// dependency on the direct chain's output layout; NrdPack already runs after
// both. It writes its own set of textures because NRD's layout (YCoCg radiance
// + normalized hit distance) differs from the tracer's.

#include "../HostDefinitions.h"
#include "../Constants.h"
// Try the vendored NRD front-end directly so its compile-time encoding and
// normalization implementation stay authoritative.
#include "../../../HPL2/extern/NRD/Shaders/NRD.hlsli"

import SceneTypes;
import PerFrame.resource;

// Pass-private (set 2). Resource names are resolved by reflection on the host.
[vk::binding(0, 2)] Texture2D<float4>   gIndirectKey;
[vk::binding(1, 2)] Texture2D<float4>   gIndirectKeyExtra;
[vk::binding(2, 2)] Texture2D<float4>   gIndirectDiffuse;
[vk::binding(3, 2)] Texture2D<float4>   gIndirectSpecular;
[vk::binding(8, 2)] Texture2D<float2>   gVelocity;
// ReSTIR DI's resolved, albedo-demodulated direct irradiance (rgb; .a unused).
// Same units and same demodulation as gIndirectDiffuse, so the two just add.
[vk::binding(10, 2)] Texture2D<float4>  gDirectLighting;

// These are new NRD frontend targets; none aliases an existing SVGF texture.
[vk::binding(4, 2)] RWTexture2D<float4> gNrdNormalRoughness;
[vk::binding(5, 2)] RWTexture2D<float>  gNrdViewZ;
[vk::binding(6, 2)] RWTexture2D<float4> gNrdDiffuseRadianceHitDist;
[vk::binding(7, 2)] RWTexture2D<float4> gNrdSpecularRadianceHitDist;
[vk::binding(9, 2)] RWTexture2D<float2> gNrdMotionVectors;

[shader("compute")]
[numthreads(16, 16, 1)]
void csMain(uint3 tid : SV_DispatchThreadID)
{
    uint w, h;
    gNrdViewZ.GetDimensions(w, h);
    uint2 p = tid.xy;
    if (p.x >= w || p.y >= h)
        return;

    // The engine stores screen-space velocity as currentUV - previousUV.
    // NRD's (-1,-1,0) motionVectorScale supplies its previousUV - currentUV
    // convention when this unchanged RG16F payload is consumed.
    gNrdMotionVectors[p] = gVelocity.Load(int3(p, 0));

    float4 key   = gIndirectKey.Load(int3(p, 0));
    float4 extra = gIndirectKeyExtra.Load(int3(p, 0));
    float3 normal = key.yzw;
    bool validSurface = dot(normal, normal) > 1.0e-8f;

    // NRD's configured normal encoding is R10G10B10A2_UNORM. A zero normal
    // cannot be passed to the packer (it normalizes internally), so sky gets a
    // zero guide. NRD uses an infinite viewZ for invalid/sky pixels.
    if (validSurface)
    {
        float linearRoughness = sqrt(saturate(extra.x)); // extra.x stores GGX alpha=m
        gNrdNormalRoughness[p] = NRD_FrontEnd_PackNormalAndRoughness(
            normalize(normal), linearRoughness, 0.0f);
        gNrdViewZ[p] = key.x;
    }
    else
    {
        gNrdNormalRoughness[p] = float4(0.0f);
        gNrdViewZ[p] = NRD_INF;
    }

    float diffuseNormHitDist = 0.0f;
    float specularNormHitDist = 0.0f;
    if (validSurface)
    {
        // A raw distance of zero means the lobe was skipped or the ray saw
        // sky. Do not call GetNormHitDist for that case: NRD reserves zero for
        // no data, while the helper deliberately replaces zero with epsilon.
        if (extra.y > 0.0f)
            diffuseNormHitDist = REBLUR_FrontEnd_GetNormHitDist(
                extra.y, key.x, kNrdHitDistanceParameters, 1.0f);
        if (extra.z > 0.0f)
            specularNormHitDist = REBLUR_FrontEnd_GetNormHitDist(
                extra.z, key.x, kNrdHitDistanceParameters,
                sqrt(saturate(extra.x)));
    }

    // Diffuse channel = ReSTIR DI direct + path-traced indirect diffuse. Both are
    // demodulated irradiance in the same units, so this is a plain add. Alpha is
    // unused in both sources; read rgb only and write NRD's YCoCg +
    // normalized-hit-distance representation.
    //
    // The hit distance stays the indirect bounce's first-ray length. The direct
    // term's own "distance" is the shadow ray, which does not fit REBLUR's model;
    // ReSTIR DI already delivers a low-noise direct term, so REBLUR's blur radius
    // matters less for it. Revisit with a luminance-weighted blend only if
    // direct-lit regions read as over-blurred.
    float3 diffuseRadiance = gIndirectDiffuse.Load(int3(p, 0)).rgb
                           + gDirectLighting.Load(int3(p, 0)).rgb;
    gNrdDiffuseRadianceHitDist[p] =
        REBLUR_FrontEnd_PackRadianceAndNormHitDist(
            diffuseRadiance, diffuseNormHitDist, true);
    gNrdSpecularRadianceHitDist[p] =
        REBLUR_FrontEnd_PackRadianceAndNormHitDist(
            gIndirectSpecular.Load(int3(p, 0)).rgb, specularNormHitDist, true);
}
