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

import SceneTypes;            // kInvalidTextureIndex sentinel
import Bindless.resource;     // gTextures2D, gMaterialSampler

public float sampleAttenuation(uint attenSlot, float r2)
{
    if (attenSlot == kInvalidTextureIndex)
        return saturate(1.0 - r2);
    return gTextures2D[NonUniformResourceIndex(attenSlot)]
        .SampleLevel(gMaterialSampler, float2(clamp(r2, 0.0, 0.99), 0.5), 0.0).r;
}

public float sampleSpotCone(uint coneSlot, float cosTheta, float cosOuterAngle)
{
    if (coneSlot == kInvalidTextureIndex)
        return smoothstep(cosOuterAngle, 1.0, cosTheta);
    float u = (1.0 - cosTheta) / max(1.0 - cosOuterAngle, 1e-6);
    return gTextures2D[NonUniformResourceIndex(coneSlot)]
        .SampleLevel(gMaterialSampler, float2(clamp(u, 0.0, 0.99), 0.5), 0.0).r;
}
