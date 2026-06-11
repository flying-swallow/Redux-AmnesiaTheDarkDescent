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
public DiffuseShading prepareDiffuseShading(VertexData v, uint materialID,
                                            float illuminationAmount,
                                            bool decodeRoughness = false)
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

    sd.rough = decodeRoughness ? decodeMaterialRoughness(m, v.texC)
                               : kDefaultDiffuseRoughness;
    return sd;
}

// --- Per-light analytic diffuse shading -----------------------------------
// Demodulated throughout: Li·NdotL·f_d/albedo (no albedo — the caller applies
// it). f_d is the standard diffuse lobe (kDiffuseBrdf, carries its 1/π). V is
// toward the camera for the composite/direct stack, back along the scatter ray
// for the surfel NEE.

// Concentric (low-distortion) unit-disk map of two [0,1) samples.
float2 concentricDiskSample(float2 u)
{
    float2 o = 2.0f * u - 1.0f;
    if (o.x == 0.0f && o.y == 0.0f) return float2(0.0f);
    float r, theta;
    const float kPiOver4 = 0.78539816f;
    const float kPiOver2 = 1.57079633f;
    if (abs(o.x) > abs(o.y)) { r = o.x; theta = kPiOver4 * (o.y / o.x); }
    else                     { r = o.y; theta = kPiOver2 - kPiOver4 * (o.x / o.y); }
    return r * float2(cos(theta), sin(theta));
}

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

    float3 sDir = ls.dir;
    float  sMax = ls.distance;
    // Source radius + light centre via the ILight interface — getLight() does the
    // point/spot index slicing, so this function no longer touches the typed SSBOs.
    ILight light = getLight(lightIndex);
    float  r     = light.getSourceRadius();
    float3 lpos  = light.getPosition();
    if (r > 0.f)
    {
        // Tangent basis perpendicular to the light direction.
        float3 t = normalize(abs(ls.dir.y) < 0.99f
                             ? cross(float3(0.f, 1.f, 0.f), ls.dir)
                             : cross(float3(1.f, 0.f, 0.f), ls.dir));
        float3 b = cross(ls.dir, t);
        float2 d = concentricDiskSample(rng.next_float2()) * r;
        float3 target = lpos + d.x * t + d.y * b;
        float3 toT = target - origin;
        sMax = length(toT);
        sDir = toT / max(sMax, 1.0e-6f);
    }

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
