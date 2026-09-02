/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Raster water surface — fragment stage. The underwater background is already
// in the pogo (the composite-shaded scene behind the water, with NO refraction
// bend applied to it). This pass adds the surface on top in TWO draws
// (hardware blend can't tint-dst and add reflection in one):
//   pass 0 (MUL blend): output surfaceTint·refractionExposure → dst *= that
//                       (tints + lifts the refracted bottom, matching the old
//                        surfaceTint · (refr·kWaterRefractionExposure)).
//   pass 1 (ADD blend): wave normal + Fresnel·fade × an inline RT LIT reflection
//                       → dst += reflColor·fresnel.
// Combined: surfaceTint·refr + reflColor·fresnel — the old water combine.

#include "../HostDefinitions.h"
#include "../Constants.h"

import SceneTypes;
import Scene;
import Material;             // load*Material / materialAlphaTest / traceShadowRay / traceReflectionHit
import bindless;
import PerFrame.resource;
import Fog;                  // computeFog — fade the water surface into world/area fog
import Shading;                     // DiffuseShading / prepareDiffuseShading / evalAnalyticLight
import Brdf;                 // brdfTangentToWorld — local->world hemisphere basis
import Random;               // interleavedGradientNoise — per-pixel stable dither
import Translucent.WaterCommon; // waterWaveNormalTangent

struct WaterPC
{
    uint pass;   // 0 = tint (MUL), 1 = reflection (ADD)
    uint _p0;
    uint _p1;
    uint _p2;
};
[vk::push_constant] WaterPC pc;

struct WaterVSOut
{
    float3 posW                    : POSW;
    float3 normalW                 : NORMAL;
    float3 tangentW                : TANGENT;
    float2 uv                      : TEXCRD;
    nointerpolation uint objectId  : OBJECTID;
    float4 posH                    : SV_POSITION;
};

// Schlick-Fresnel — mirrors deferred_common.h.fsl::Fresnel(eDotN, bias, pw).
// Base clamped positive: eDotN can exceed 1.0 by a ULP (normalize rounding)
// and pow(negative, fractional) is NaN — see Translucent.frag.slang.
float computeFresnel(float edotN, float bias, float pw)
{
    float facing = max(1.0f - edotN, 1.0e-6f);
    return saturate(bias + (1.0f - bias) * pow(facing, pw));
}

// One traced bounce of indirect irradiance at a world-space point, ALBEDO-
// DEMODULATED like PathTracePass.rt.slang's output: the point's own albedo is
// deliberately left out (the caller re-modulates), the throughput starts at 1
// and the cosine pdf cancels the diffuse lobe's 1/pi, so each sample's
// contribution is just what the bounce vertex emits/reflects. Only ONE bounce:
// a fragment shader has no payload recursion under inline RayQuery and cannot
// afford a path loop per water pixel.
//
// SAMPLING IS DETERMINISTIC ON PURPOSE. Nothing denoises or temporally filters
// behind the water pass, so a per-frame random direction would boil visibly on
// the reflection. Instead the u1 (elevation) dimension is stratified over the
// sample count and u2 (azimuth) is rotated by a dither that depends only on the
// pixel - no gPerFrame.totalFrames. The tradeoff is deliberate: the estimate is
// biased into a fixed low-sample pattern (a faint static structure in the
// bounce) rather than unbiased-but-flickering, which reads far better with no
// denoiser behind it.
float3 traceIndirectAtHit(float3 posW, float3 normalW, uint2 pixel)
{
    float3x3 basis  = brdfTangentToWorld(normalW);   // rows (T,B,N): mul(local, basis) -> world
    float    dither = interleavedGradientNoise(pixel);
    float3   origin = posW + normalW * 1.0e-3f;
    float3   sum    = float3(0.f);

    const float invSamples = 1.0f / float(kWaterReflectionIndirectSamples);
    for (uint s = 0u; s < kWaterReflectionIndirectSamples; ++s)
    {
        // Stratified elevation, golden-ratio-advanced + pixel-rotated azimuth.
        float u1 = (float(s) + 0.5f) * invSamples;
        float u2 = frac(dither + float(s) * 0.61803398875f);

        // Cosine-weighted hemisphere sample (z = up) - the same concentric map
        // the path tracer's diffuse bounce uses.
        float  r   = sqrt(u1);
        float  phi = 2.0f * 3.14159265358979f * u2;
        float3 localDir = float3(r * cos(phi), r * sin(phi), sqrt(max(0.f, 1.0f - u1)));
        float3 dir = normalize(mul(localDir, basis));

        // A miss contributes nothing - there is no environment map and the path
        // tracer's miss shader is black too, so both agree that "sky" = 0.
        TriangleHit gh;
        if (!traceReflectionHit(origin, dir, 1.0e30f, gh))
            continue;

        uint       gMat = gSceneObjects[gh.instanceID].materialID;
        VertexData gv   = getVertexData(gh);
        float      gIll = gSceneObjects[gh.instanceID].illuminationAmount;
        DiffuseShading gsd = prepareDiffuseShading(gv, gMat, gIll, /*decodeRoughness=*/true);

        // Same energy math as PathTracePass.rt.slang::handleHit: emission enters
        // the path directly and the NEE direct term is scaled by the ART-DIRECTED
        // bounce albedo pow(albedo, kSurfelGIAlbedoBoostExp), so this producer
        // stays in the path tracer's units.
        float3 giAlbedo = pow(gsd.albedo, kSurfelGIAlbedoBoostExp);
        sum += gsd.emission + giAlbedo * evalAnalyticLight(gsd);
    }

    return sum * invSamples * kWaterReflectionIndirectIntensity;
}

// Fully (lit-)shade a reflection-ray hit: albedo·(direct NEE + one traced
// indirect bounce) + emission - the same energy convention as the primary
// composite, which is exactly albedo·(direct + indirect) + emission. The
// screen-space gIndirectLighting texture cannot be sampled at an arbitrary
// reflection-ray hit, so the indirect term is re-traced here instead; without
// it, geometry that is only GI-lit reflects as black.
float3 shadeReflectionHit(TriangleHit th, uint2 pixel)
{
    uint matID  = gSceneObjects[th.instanceID].materialID;
    VertexData v = getVertexData(th);
    float illum = gSceneObjects[th.instanceID].illuminationAmount;
    DiffuseShading sd = prepareDiffuseShading(v, matID, illum);
    float3 direct = (kDefaultRenderDirectLighting != 0u) ? evalAnalyticLight(sd) : float3(0.f);
    float3 indirect = (kDefaultRenderIndirectLighting != 0u)
                        ? traceIndirectAtHit(sd.posW, sd.normalW, pixel) : float3(0.f);
    // Both terms are demodulated irradiance - one albedo turns them into
    // outgoing radiance, matching the primary composite's convention.
    return sd.albedo * (direct + indirect) + sd.emission;
}

[shader("fragment")]
float4 psMain(WaterVSOut psIn) : SV_TARGET
{
    UniformObject obj   = gSceneObjects[psIn.objectId];
    WaterMaterial water = loadWaterMaterial(obj.materialID);

    // World + area fog at the water surface. We fog the water exactly like the
    // opaque composite does (applyFog: dst = visibility·color + fogColor), just
    // split across the two hardware blend passes (MUL then ADD):
    //   pass 0 MUL: dst *= tint·exp·visibility  -> refr·tint·exp·visibility
    //   pass 1 ADD: dst += reflColor·fresnel·visibility + fog.color
    //   sum = visibility·(refr·tint·exp + reflColor·fresnel) + fogColor.
    // At full fog visibility→0 so the surface collapses to fogColor and the water
    // dissolves into the fog like everything else, regardless of how shallow /
    // weakly-fogged the refracted background is.
    FogResult fog = computeFog(psIn.posW, gPerFrame.posW);

    if (pc.pass == 0u)
    {
        // Tint (+ refraction-exposure lift) of the refracted background (MUL),
        // faded to zero as fog thickens.
        float3 tint = float3(1.f);
        uint   diffuseTex = water.tex[0];
        if (diffuseTex != kInvalidTextureIndex)
            tint = sampleBindless2D(diffuseTex, psIn.uv).rgb;
        return float4(tint * kWaterRefractionExposure * fog.visibility, 1.0f);
    }

    // pass 1 — Fresnel-weighted lit reflection (ADD).
    //
    // Build the surface frame from screen-space posW derivatives instead of the
    // interpolated vertex normal/tangent. Water plane meshes frequently ship
    // without usable normal/tangent streams; we'd then bind the +Z/+X vertex
    // fallback, which for an XZ-authored water plane points *sideways*, sending
    // the reflection ray into walls (dark) — the tint still renders because it
    // doesn't use the normal, so the symptom is "water but no reflection". The
    // geometric normal from ddx/ddy(posW) is exact for the flat surface and
    // independent of the mesh's vertex attributes.
    float3 eye  = normalize(psIn.posW - gPerFrame.posW); // camera -> surface
    float3 dpdx = ddx(psIn.posW);
    float3 dpdy = ddy(psIn.posW);
    float3 N    = normalize(cross(dpdx, dpdy));
    if (dot(eye, N) > 0.0f)
        N = -N;                                       // face the camera
    float3 T  = dpdx - N * dot(dpdx, N);              // tangent in the surface plane
    float  tl = length(T);
    T = (tl > 1.0e-5f) ? T / tl
        : ((abs(N.y) < 0.99f) ? normalize(cross(float3(0.f, 1.f, 0.f), N))
                              : normalize(cross(float3(1.f, 0.f, 0.f), N)));
    float3 B = cross(N, T);

    float3 nT = waterWaveNormalTangent(water, psIn.uv, gPerFrame.afT);
    float3 waveN = normalize(nT.x * T + nT.y * B + nT.z * N);
    if (dot(eye, waveN) > 0.0f)
        waveN = -waveN;
    float edotN   = max(dot(-eye, waveN), 0.0f);
    float fresnel = computeFresnel(edotN, water.frenselBias, water.frenselPow);

    // Distance fade: fade the reflection out with view depth. Only when a real
    // fade range is authored — without this guard a 0 reflectionFadeEnd makes
    // fadeLen ~1e-6 and saturate()→1, which zeroes the reflection everywhere.
    if (water.reflectionFadeEnd > water.reflectionFadeStart)
    {
        float viewZ   = -mul(gPerFrame.viewMat, float4(psIn.posW, 1.0f)).z;
        float fadeLen = water.reflectionFadeEnd - water.reflectionFadeStart;
        fresnel *= 1.0f - saturate((viewZ - water.reflectionFadeStart) / fadeLen);
    }

    // Reflection direction: calmer than refraction (blend wave normal toward flat).
    float3 flatN = N;  // already flipped to face the camera above
    float3 reflN = normalize(lerp(flatN, waveN, kWaterReflectionTurbulence));
    float3 reflectedDir = normalize(reflect(eye, reflN));

    float3 origin = psIn.posW + waveN * 1.0e-3f;
    TriangleHit rh;
    float3 reflColor = float3(0.0f);
    if (traceReflectionHit(origin, reflectedDir, 1.0e30f, rh))
        reflColor = shadeReflectionHit(rh, uint2(psIn.posH.xy)) * kWaterReflectionExposure;

    // Fade the reflection by fog visibility, then add the fog colour exactly
    // once (the MUL pass already zeroed the surface at full fog) so the water
    // composites to fogColor in the distance — matching opaque applyFog.
    return float4(reflColor * fresnel * fog.visibility + fog.color, 1.0f);
}
