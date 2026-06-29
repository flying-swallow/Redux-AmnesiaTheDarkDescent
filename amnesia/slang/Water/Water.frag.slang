/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Raster water surface — fragment stage. The underwater refraction background is
// already in the pogo (SurfelVBuffer.rt's water refraction clobbered the primary
// hit, shaded by the GI composite). This pass adds the surface on top in TWO draws
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
import SurfelGI.SurfelShade;        // DiffuseShading / prepareDiffuseShading / evalAnalyticLight / gatherSurfelIndirect
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

// Fully (lit-)shade a reflection-ray hit: albedo·(direct NEE + surfel-indirect)
// + emission — the same energy convention as the primary composite.
float3 shadeReflectionHit(TriangleHit th)
{
    uint matID  = gSceneObjects[th.instanceID].materialID;
    VertexData v = getVertexData(th);
    float illum = gSceneObjects[th.instanceID].illuminationAmount;
    DiffuseShading sd = prepareDiffuseShading(v, matID, illum);
    float3 direct   = (kDefaultRenderDirectLighting   != 0u) ? evalAnalyticLight(sd) : float3(0.f);
    float3 indirect = (kDefaultRenderIndirectLighting != 0u)
                      ? gatherSurfelIndirect(sd.posW, sd.normalW) : float3(0.f);
    // Both terms are demodulated irradiance — one albedo turns them into
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
        reflColor = shadeReflectionHit(rh) * kWaterReflectionExposure;

    // Fade the reflection by fog visibility, then add the fog colour exactly
    // once (the MUL pass already zeroed the surface at full fog) so the water
    // composites to fogColor in the distance — matching opaque applyFog.
    return float4(reflColor * fresnel * fog.visibility + fog.color, 1.0f);
}
