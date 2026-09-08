#pragma once
/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Shared water-surface frame, wave-normal, Fresnel, and reflection-normal math.
// The geometric derivative frame is used instead of the vertex normal/tangent
// because many water meshes ship without usable normal/tangent streams; the
// usual +Z/+X fallback points sideways for an XZ water plane. Keeping the
// derivative inputs at the fragment call site also makes this module stage-free.

#include "../Constants.h"
import SceneTypes;
import bindless;
import Translucent.WaterCommon;

public struct WaterSurfaceFrame
{
    public float3 N;    // geometric surface normal, already flipped to face the camera
    public float3 T;    // unit tangent in the surface plane
    public float3 B;    // cross(N, T)
    public bool   valid; // false when the derivatives are degenerate/nonfinite
};

// dpdx/dpdy are supplied by the caller so this module has no derivative
// instructions and remains usable from stages other than a fragment shader.
// eye = normalize(posW - cameraPosW), i.e. camera -> surface.
public WaterSurfaceFrame waterSurfaceFrame(float3 dpdx, float3 dpdy, float3 eye)
{
    WaterSurfaceFrame frame;
    float3 c  = cross(dpdx, dpdy);
    float  c2 = dot(c, c);

    // Comparisons deliberately catch NaN without isnan(), which can be
    // optimized away under fast math. The upper bound also rejects infinity
    // and squared-length overflow before normalize() can produce NaN.
    frame.valid = (c2 > 1.0e-20f) && (c2 < 3.402823466e+38f);
    if (!frame.valid)
    {
        // A finite up-axis fallback, oriented toward the camera where possible.
        frame.N = float3(0.f, (eye.y > 0.f) ? -1.f : 1.f, 0.f);
        frame.T = normalize(cross(float3(1.f, 0.f, 0.f), frame.N));
        frame.B = cross(frame.N, frame.T);
        return frame;
    }

    float3 N = normalize(cross(dpdx, dpdy));
    if (dot(eye, N) > 0.0f)
        N = -N;                                       // face the camera
    float3 T  = dpdx - N * dot(dpdx, N);              // tangent in the surface plane
    float  tl = length(T);
    T = (tl > 1.0e-5f) ? T / tl
        : ((abs(N.y) < 0.99f) ? normalize(cross(float3(0.f, 1.f, 0.f), N))
                              : normalize(cross(float3(1.f, 0.f, 0.f), N)));
    float3 B = cross(N, T);

    frame.N = N;
    frame.T = T;
    frame.B = B;
    return frame;
}

// Wave normal in WORLD space: rotate the tangent-space wave normal through
// the derivative frame, then flip it to face the camera.
public float3 waterWaveNormalWorld(WaterMaterial water, float2 baseUv,
                                   float2 dBaseUvDx, float2 dBaseUvDy, float afT,
                                   WaterSurfaceFrame frame, float3 eye)
{
    float3 nT = waterWaveNormalTangent(water, baseUv, dBaseUvDx, dBaseUvDy, afT);
    float3 waveN = normalize(nT.x * frame.T + nT.y * frame.B + nT.z * frame.N);
    if (dot(eye, waveN) > 0.0f)
        waveN = -waveN;
    return waveN;
}

// Schlick-Fresnel — positive-clamped pow base, moved verbatim from
// Water.frag.slang::computeFresnel.
public float waterFresnel(float edotN, float bias, float pw)
{
    float facing = max(1.0f - edotN, 1.0e-6f);
    return saturate(bias + (1.0f - bias) * pow(facing, pw));
}

public float waterFresnelTerm(WaterMaterial water, float3 eye, float3 waveN)
{
    return waterFresnel(max(dot(-eye, waveN), 0.0f),
                        water.frenselBias, water.frenselPow);
}

public float waterDistanceFade(WaterMaterial water, float viewZ)
{
    if (water.reflectionFadeEnd > water.reflectionFadeStart)
    {
        float fadeLen = water.reflectionFadeEnd - water.reflectionFadeStart;
        return 1.0f - saturate((viewZ - water.reflectionFadeStart) / fadeLen);
    }
    return 1.0f;
}

public float3 waterReflectionNormal(WaterSurfaceFrame frame, float3 waveN)
{
    return normalize(lerp(frame.N, waveN, kWaterReflectionTurbulence));
}
