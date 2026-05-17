#ifndef LIGHT_FALLOFF_GLSL
#define LIGHT_FALLOFF_GLSL

// Bindless 1D-as-2D attenuation LUT sample. The canonical HPL2 falloff
// texture is an Nx1 ramp indexed by (d/r)²; we store it in the same
// textures_2d[] bindless array used for diffuse maps and sample at
// v = 0.5 (the texture is one texel tall anyway, so any v is fine).
// `attenSlot == INVALID_TEXTURE_INDEX` is the sentinel for "no map" and
// triggers the saturate(1 - (d/r)²) fallback from the clustered .fsl.
//
// Pulls bindless.resource.glsl directly so the bindless declarations
// (textures_2d, materialSampler, INVALID_TEXTURE_INDEX) are visible when
// the helper's body is parsed.
#extension GL_EXT_nonuniform_qualifier : require

#include "bindless.resource.glsl"

float sampleAttenuation(uint attenSlot, float r2)
{
    if (attenSlot == INVALID_TEXTURE_INDEX) {
        return clamp(1.0 - r2, 0.0, 1.0);
    }
    return texture(
        sampler2D(textures_2d[nonuniformEXT(attenSlot)], materialSampler),
        vec2(clamp(r2, 0.0, 1.0), 0.5)).r;
}

#endif // LIGHT_FALLOFF_GLSL
