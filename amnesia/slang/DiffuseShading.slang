#pragma once
// Shared diffuse-shading helpers, used by both the composite / direct stack
// (SurfelShade.slang) and the surfel path tracer (SurfelRayTrace.rt.slang):
//   * DiffuseShading + prepareDiffuseShading — surface shading state from a hit.
//   * shadeAnalyticLight / *Soft / unshadowedAnalyticLight — the per-light
//     diffuse shade (hard / jittered-disk / no shadow ray). These were Scene
//     methods, but they're lighting-evaluation utilities (not scene-data
//     accessors), so they live here next to the eval/reservoir helpers that use
//     them; they compose Scene's data primitives (getLight().sample /
//     getSourceRadius / getPosition, materials.traceShadowRay).
// Previously copy-pasted into each consumer.

#include "HostDefinitions.h"
#include "Constants.h"

import SceneTypes;          // VertexData, DiffuseMaterial, kInvalidTextureIndex, kDefaultDiffuseRoughness
import Scene;               // gScene data primitives (+ ILight/LightSample via __exported)
import bindless;            // gTextures2D, gMaterialSampler
import Brdf;                // decodeMaterialRoughness, evalDiffuseDemodulated
import PerFrame.resource;   // gPerFrame.point/spotLightCount
import Random;              // RNG for the soft-shadow disk sample

public struct DiffuseShading
{
    public float3 posW;
    public float3 normalW;
    public float3 albedo;
    public float3 emission;
    public float  rough;    // perceptual diffuse-lobe roughness
};

// emission = illumination-map texel (DiffuseMaterial slot 5) * per-object glow
// scalar (UniformObject.illuminationAmount). Without this the surfel composite
// ignores illumination entirely and glowing textures render dark.
//
// decodeRoughness: the path tracer decodes per-texel roughness
// (decodeMaterialRoughness); the composite/direct stack uses the flat
// kDefaultDiffuseRoughness (decode was disabled there). Defaulting to false
// preserves the composite-side behavior for its callers.
//
// applyNormalMap: perturb normalW with the material's tangent-space normal map
// (tex[1], gated by kMaterialFlagEnableNormal). Defaults to false so existing
// callers (composite / water / surfel) are unchanged; the ReSTIR direct pass
// opts in so its lighting picks up bump detail. World-space TBN built from the
// interpolated normal + tangent — same decode/convention as SurfelVBuffer.rt.
public DiffuseShading prepareDiffuseShading(VertexData v, uint materialID,
                                            float illuminationAmount,
                                            bool decodeRoughness = false,
                                            bool applyNormalMap   = false)
{
    DiffuseShading sd;
    sd.posW    = v.posW;
    sd.normalW = normalize(v.normalW);
    DiffuseMaterial m = gScene.getDiffuseMaterial(materialID);

    if (applyNormalMap && (m.materialConfig & kMaterialFlagEnableNormal) != 0u &&
        m.tex[1] != kInvalidTextureIndex)
    {
        float3 N = sd.normalW;
        float3 T = v.tangentW - N * dot(N, v.tangentW);   // Gram-Schmidt orthogonalize
        if (dot(T, T) > 1e-8f)
        {
            T = normalize(T);
            float3 B = cross(N, T);                        // world-space TBN (tangent.w dropped)
            float3 mn = sampleBindless2DLevel(m.tex[1], v.texC, 0.0f).xyz * 2.0f - 1.0f;
            sd.normalW = normalize(mn.x * T + mn.y * B + mn.z * N);
        }
    }

    uint diffuseTex = m.tex[0];
    if (diffuseTex == kInvalidTextureIndex)
        sd.albedo = float3(0.5f);
    else
        sd.albedo = sampleBindless2DLevel(diffuseTex, v.texC, 0.0f).rgb;

    uint illumTex = m.tex[5];
    if (illumTex == kInvalidTextureIndex || illuminationAmount <= 0.f)
        sd.emission = float3(0.f);
    else
        sd.emission = sampleBindless2DLevel(illumTex, v.texC, 0.0f).rgb
                      * illuminationAmount;

    sd.rough = decodeRoughness ? decodeMaterialRoughness(m, v.texC)
                               : kDefaultDiffuseRoughness;
    return sd;
}

// --- Per-light analytic diffuse shading -----------------------------------
// Demodulated throughout: Li·NdotL·f_d/albedo (no albedo — the caller applies
// it). f_d is the standard diffuse lobe (kDiffuseBrdf, carries its 1/π). V is
// toward the camera for the composite/direct stack, back along the scatter ray
// for the surfel NEE.

// Shared core: sample + back-face cull + demodulated contribution. Returns false
// (contrib unset) when the sample fails or N·L ≤ 0. The three public variants
// differ ONLY in the shadow ray.
bool sampleLightShade(uint lightIndex, float3 posW, float3 normalW, float3 V,
                      float rough, out LightSample ls, out float3 contrib)
{
    contrib = float3(0.f);
    if (!getLight(lightIndex).sample(posW, ls)) return false;
    float NdotL = dot(normalW, ls.dir);
    if (NdotL <= 0.f) return false;
    contrib = ls.Li * NdotL * evalDiffuseDemodulated(normalW, V, ls.dir, rough);
    return true;
}

// Hard shadow: trace one ray at the light centre. `origin` is the
// (epsilon-offset) shadow-ray origin the caller computes.
public float3 shadeAnalyticLight(float3 posW, float3 normalW, float3 V,
                                 float rough, uint lightIndex, float3 origin)
{
    LightSample ls;
    float3 contrib;
    if (!sampleLightShade(lightIndex, posW, normalW, V, rough, ls, contrib))
        return float3(0.f);
    if (!gScene.materials.traceShadowRay(origin, ls.dir, ls.distance)) return float3(0.f);
    return contrib;
}

// Soft shadow: for a non-zero sourceRadius, aim the shadow ray at a jittered
// point on the light disk (facing the shading point), so one noisy sample per
// frame, accumulated temporally, resolves to a penumbra. Energy still uses the
// light centre — only occlusion is softened. sourceRadius == 0 falls back to the
// hard centre ray.
public float3 shadeAnalyticLightSoft(float3 posW, float3 normalW, float3 V,
                                     float rough, uint lightIndex,
                                     float3 origin, inout RNG rng)
{
    LightSample ls;
    float3 contrib;
    if (!sampleLightShade(lightIndex, posW, normalW, V, rough, ls, contrib))
        return float3(0.f);

    // Aim the shadow ray at a jittered point on the light's source via the ILight
    // interface — getLight() does the point/spot/area index slicing, so this
    // function no longer touches the typed SSBOs or knows the source shape. Spheres
    // (point/spot) return a disk point; area lights sample their rectangle; a
    // degenerate source returns the centre (hard ray).
    float3 target = getLight(lightIndex).sampleSourcePoint(ls.dir, rng.next_float2());
    float3 toT  = target - origin;
    float  sMax = length(toT);
    float3 sDir = toT / max(sMax, 1.0e-6f);

    if (!gScene.materials.traceShadowRay(origin, sDir, sMax)) return float3(0.f);
    return contrib;
}

// Unshadowed contribution (no ray). Used as the RIS target weight for stochastic
// light selection — keeping the lobe keeps the target proportional to the true
// integrand.
public float3 unshadowedAnalyticLight(float3 posW, float3 normalW, float3 V,
                                      float rough, uint lightIndex)
{
    LightSample ls;
    float3 contrib;
    sampleLightShade(lightIndex, posW, normalW, V, rough, ls, contrib);
    return contrib;   // contrib stays float3(0) on sample-fail / back-face
}
