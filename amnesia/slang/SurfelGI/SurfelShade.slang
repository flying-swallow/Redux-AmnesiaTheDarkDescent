#pragma once
// Shared surfel-GI surface shading helpers.
//
// Extracted from SurfelGIRenderPass.frag.slang so both the final composite
// pass and the reflection-resolve pass shade a resolved TriangleHit the same
// way: albedo + emission prep, NEE-over-all-lights direct lighting, and a
// world-space surfel-cache indirect gather. Keeping one copy avoids the two
// passes drifting on the deliberately non-PBR lighting convention (no 1/π on
// direct — see evalAnalyticLight).

#include "../HostDefinitions.h"
#include "../Constants.h"

import SceneTypes;
import Scene;
import Bindless.resource;
import Bindless.interface;
import PerFrame.resource;
import SurfelGI.SurfelTypes;
import SurfelGI.SurfelUtils;
import Grid;                    // gridCellPos / gridFlattenCellIndex / gridCellValid (light grid cull)

static const float kShadeRayEps = 1.0e-3f;

struct DiffuseShading
{
    float3 posW;
    float3 normalW;
    float3 albedo;
    float3 emission;
};

// emission = illumination-map texel (DiffuseMaterial slot 5) * per-object glow
// scalar (UniformObject.illuminationAmount). Without this the surfel composite
// ignores illumination entirely and glowing textures render dark.
DiffuseShading prepareDiffuseShading(VertexData v, uint materialID, float illuminationAmount)
{
    DiffuseShading sd;
    sd.posW    = v.posW;
    sd.normalW = normalize(v.normalW);
    DiffuseMaterial m = gScene.getDiffuseMaterial(materialID);
    uint diffuseTex = m.tex[0];
    if (diffuseTex == kInvalidTextureIndex)
        sd.albedo = float3(0.5f);
    else
        sd.albedo = gTextures2D[NonUniformResourceIndex(diffuseTex)]
                        .SampleLevel(gMaterialSampler, v.texC, 0.0f).rgb;

    uint illumTex = m.tex[5];
    if (illumTex == kInvalidTextureIndex || illuminationAmount <= 0.f)
        sd.emission = float3(0.f);
    else
        sd.emission = gTextures2D[NonUniformResourceIndex(illumTex)]
                          .SampleLevel(gMaterialSampler, v.texC, 0.0f).rgb
                      * illuminationAmount;
    return sd;
}

// NEE over all lights. Single sample per light, BRDF = albedo / π,
// shadow visibility via gScene.materials.traceShadowRay (alpha-tested
// non-opaque RayQuery — see the comment on that helper in Scene.slang
// for why a plain Proceed/CommittedStatus pair silently returns "visible"
// against this engine's non-opaque BLAS).
// One unified-index point/spot light: sample, NdotL-cull, ray-traced shadow,
// accumulate. `origin` is the plane-offset shadow-ray origin. Returns the
// radiance contribution (zero if out of range, back-facing, or occluded).
//
// Base game omits the 1/π Lambert factor (deferred_light_pointlight.frag.fsl /
// visibility_shade.frag: albedo·color·atten·ndl); match it so direct reads at
// the original brightness. Albedo is applied here (composite).
float3 shadeAnalyticLight(DiffuseShading sd, uint lightIndex, float3 origin)
{
    LightSample ls;
    if (!gScene.sampleLightByIndex(lightIndex, sd.posW, ls)) return float3(0.f);

    float NdotL = dot(sd.normalW, ls.dir);
    if (NdotL <= 0.f) return float3(0.f);

    if (!gScene.materials.traceShadowRay(origin, ls.dir, ls.distance)) return float3(0.f);

    return sd.albedo * ls.Li * NdotL;
}

float3 evalAnalyticLight(DiffuseShading sd)
{
    uint lightCount = gScene.getLightCount();   // unified point+spot count

    float3 Lr = float3(0.f);
    float3 origin = sd.posW + sd.normalW * kShadeRayEps;

    // Walk only the point/spot lights binned to this point's cell in the coarse
    // world light grid (built each frame by LightGridBuildPass). Conservative
    // binning means every light whose reach-sphere touches the cell is present,
    // so the deterministic sum is exact apart from the kLightsPerCellMax cap
    // (the RT NEE path already lives with the same cap). This cuts the per-pixel
    // ray-traced shadow count from O(all scene lights) to O(lights overlapping
    // the cell). Geometry beyond the grid extent (out-of-grid cell) falls back
    // to the full light array so it isn't left unlit.
    int3 cell = gridCellPos(sd.posW, gScene.camera.getPosition(), kLightGridUnit);
    if (lightCount > 0u && gridCellValid(cell, kLightGridDim))
    {
        uint flat = gridFlattenCellIndex(cell, kLightGridDim);
        uint n    = min(gLightGridCount[flat], kLightsPerCellMax);
        for (uint k = 0u; k < n; ++k)
            Lr += shadeAnalyticLight(sd, gLightGridList[flat * kLightsPerCellMax + k], origin);
    }
    else
    {
        for (uint i = 0u; i < lightCount; ++i)
            Lr += shadeAnalyticLight(sd, i, origin);
    }

    // Box lights are a volumetric ambient fill, not a directional NEE light, so
    // they are handled here directly rather than through sampleLightByIndex (which
    // returns false for box indices). Matches the original deferred_light_box:
    // surface albedo * box color, additive, with the AABB falloff and no NdotL /
    // no shadow. Box light also bounces via the surfel cache (SurfelIntegratePass).
    uint boxCount = gScene.getBoxLightCount();
    for (uint b = 0; b < boxCount; ++b)
    {
        BoxLight box = gScene.getBoxLight(b);
        Lr += sd.albedo * box.color * boxLightFalloff(box, sd.posW);
    }
    return Lr;
}

// World-space surfel-cache indirect gather. Mirrors the per-pixel gather in
// SurfelEvaluationPass.cs.slang, but takes an explicit (posW, normalW) so it
// works for any shaded point — including a reflection / refraction hit that is
// not the primary camera pixel. Returns averaged incoming radiance (the cosθ
// factor is baked into the surfel cache), or float3(0) when no surfel covers
// the point. Multiply by albedo at the call site (f = albedo/π, pdf = cosθ/π
// → reduces to albedo) to get outgoing radiance.
float3 gatherSurfelIndirect(float3 posW, float3 normalW)
{
    int3 cellPos = getCellPos(posW, gScene.camera.getPosition(), kCellUnit);
    if (!isCellValid(cellPos))
        return float3(0.f);

    uint flattenIndex = getFlattenCellIndex(cellPos);
    CellInfo cellInfo = gCellInfoBuffer[flattenIndex];

    float4 indirect = float4(0.f);
    for (uint i = 0; i < cellInfo.surfelCount; ++i)
    {
        uint surfelIndex = gCellToSurfelBuffer[cellInfo.cellToSurfelBufferOffset + i];
        Surfel surfel = gSurfelBuffer[surfelIndex];

        float3 bias  = posW - surfel.position;
        float  dist2 = dot(bias, bias);
        if (dist2 >= surfel.radius * surfel.radius) continue;

        float3 sNormal = normalize(surfel.normal);
        float  dotN    = dot(normalW, sNormal);
        if (dotN <= 0.0f) continue;

        float dist = sqrt(dist2);
        float contribution = 1.f;
        contribution *= saturate(dotN);
        contribution *= saturate(1.0f - dist / surfel.radius);
        contribution  = smoothstep(0.f, 1.f, contribution);

#ifdef USE_SURFEL_DEPTH
        {
            float2 uv = getSurfelDepthUV(surfelIndex, bias / dist, sNormal);
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

        SurfelRecycleInfo recycleInfo = gSurfelRecycleInfoBuffer[surfelIndex];
        float blendDelayT = smoothstep(0.f, float(kDefaultBlendingDelay), float(recycleInfo.frame));
        indirect += float4(surfel.radiance, 1.f) * contribution * blendDelayT;
    }

    if (indirect.w > 0.f)
        return indirect.xyz / indirect.w;
    return float3(0.f);
}
