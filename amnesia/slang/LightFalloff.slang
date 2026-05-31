#pragma once

// Slang port of amnesia/glsl/light_falloff.glsl — the two paths (forward
// composite at visibility_shade.frag and surfel NEE through interface.slang)
// must share one source of truth for radial / cone attenuation, otherwise
// direct vs indirect lighting drifts apart on artist-authored LUTs.
//
// Index conventions follow the legacy HPL2 falloff textures:
//   radial : 1D-as-2D LUT keyed on r² = (d/r)²; sampled at v = 0.5.
//   cone   : 1D LUT keyed on (1 - cosTheta) / (1 - cosOuterAngle); v = 0.5.
// Analytic fallbacks fire when the slot is kInvalidTextureIndex.

import SceneTypes;            // (shared constants)
import Bindless.resource;     // gAttenuationLut, gSurfelDepthSampler

// The default falloff LUT (core_falloff_linear) is bound once on set 0 as
// gAttenuationLut — it's identical for every light and never changes, so there's
// no per-light slot lookup and no analytic fallback. Sampled with gSurfelDepthSampler
// (ClampToEdge) over the full [0,1] lookup so the curve reaches the LUT end (~0) at
// the light radius, matching the base game's clamped nearestSampler.
public float sampleAttenuation(float r2)
{
    return gAttenuationLut.SampleLevel(gSurfelDepthSampler, float2(saturate(r2), 0.5), 0.0).r;
}

// Light reach (reach² = intensity / kPointLightCutoff) is inlined at its only
// caller, LightGridBuildPass.cs (the falloff in interface.slang is windowless,
// so the cutoff governs the grid bin only).

public float sampleSpotCone(float cosTheta, float cosOuterAngle)
{
    float u = (1.0 - cosTheta) / max(1.0 - cosOuterAngle, 1e-6);
    return gAttenuationLut.SampleLevel(gSurfelDepthSampler, float2(saturate(u), 0.5), 0.0).r;
}
