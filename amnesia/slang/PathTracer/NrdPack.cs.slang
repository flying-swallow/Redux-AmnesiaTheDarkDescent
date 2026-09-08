// NRD front-end packing for the path-traced lighting inputs.
//
// Keep indirect diffuse and specular in separate NRD lobes. Direct lighting
// has its own RELAX denoiser and is added after REBLUR in the composite,
// so the indirect bounce's hit distance cannot widen direct shadow edges.
// NRD uses private targets for YCoCg radiance + normalized hit distance.

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
    // Camera-jitter audit (a): the engine-native inputs and pack targets here
    // are all full render extent, so `p` directly names the primary raster
    // sample. NRD's internal downsampled pool is entered after this pack and
    // does not create a half-resolution index for this shader.
    uint2 p = tid.xy;
    if (p.x >= w || p.y >= h)
        return;

    // Category (b): IN_MV is copied from the raster velocity texture unchanged.
    // The engine stores screen-space velocity as currentUV - previousUV, while
    // NRD's (-1,-1,0) motionVectorScale supplies its previousUV - currentUV
    // convention. Camera jitter is supplied separately through NRD
    // CommonSettings.cameraJitter, so it must not be baked into this texture.
    gNrdMotionVectors[p] = gVelocity.Load(int3(p, 0));

    float4 key   = gIndirectKey.Load(int3(p, 0));
    float4 extra = gIndirectKeyExtra.Load(int3(p, 0));
    // The tracer supplies its normal-mapped BSDF normal, not the geometric
    // normal used by ReSTIR's reservoir key. Preserve that detail for both lobes.
    float3 normal = key.yzw;
    bool validSurface = dot(normal, normal) > 1.0e-8f;

    // Category (b): IN_VIEWZ is the V-buffer surface key's unjittered
    // view-space depth. No camera ray or screen-space UV is reconstructed in
    // this packing pass, and invalid/sky pixels retain NRD's infinite depth.

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

    // Only indirect diffuse uses the indirect bounce's first-ray distance.
    float3 diffuseRadiance = gIndirectDiffuse.Load(int3(p, 0)).rgb;
    gNrdDiffuseRadianceHitDist[p] =
        REBLUR_FrontEnd_PackRadianceAndNormHitDist(
            diffuseRadiance, diffuseNormHitDist, true);
    gNrdSpecularRadianceHitDist[p] =
        REBLUR_FrontEnd_PackRadianceAndNormHitDist(
            gIndirectSpecular.Load(int3(p, 0)).rgb, specularNormHitDist, true);
}
