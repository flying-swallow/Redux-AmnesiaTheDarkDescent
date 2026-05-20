#ifndef LIGHT_FALLOFF_GLSL
#define LIGHT_FALLOFF_GLSL

// Bindless 1D-as-2D attenuation LUT sample. The canonical HPL2 falloff
// texture is an Nx1 ramp indexed by (d/r)²; we store it in the same
// textures_2d[] bindless array used for diffuse maps and sample at
// v = 0.5 (the texture is one texel tall anyway, so any v is fine).
//
// `attenSlot == INVALID_TEXTURE_INDEX` triggers an analytic fallback of
// `(1 - (d/r)²)²` — squared, not the bare legacy `1 - (d/r)²`. The squared
// taper matches the *visible* coverage of the previous in-repo glTF curve
// `(1 - (d/r)⁴) / d²` (≈0.56 at half-radius, ≈0.04 at 0.9·radius) so light
// data authored against that curve still looks like it ends where artists
// placed the radius. The bare `1 - r²` form lit out to nearly the full
// radius and caused spots/points to "blow past" their authored extent.
// Artist-authored LUT assets still win via the LUT path above and stay
// keyed on `(d/r)²` exactly as deferred_light_pointlight.frag.fsl does.
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
        vec2(clamp(r2, 0.0, 0.99), 0.5)).r;
}

// Spot-cone falloff. Legacy deferred_light_spotlight.frag.fsl indexes a 1D
// cone LUT by `(1 - cosTheta) / oneMinusCosHalfSpotFov` — i.e. 0 at the cone
// axis (cosTheta = 1) and 1 at the outer edge (cosTheta = cosOuterAngle).
// Fallback when no LUT is bound is a cubic smoothstep that's close in shape
// to the canonical authored ramp.
float sampleSpotCone(uint coneSlot, float cosTheta, float cosOuterAngle)
{
    if (coneSlot == INVALID_TEXTURE_INDEX) {
        return smoothstep(cosOuterAngle, 1.0, cosTheta);
    }
    float u = (1.0 - cosTheta) / max(1.0 - cosOuterAngle, 1e-6);
    return texture(
        sampler2D(textures_2d[nonuniformEXT(coneSlot)], materialSampler),
        vec2(clamp(u, 0.0, 0.99), 0.5)).r;
}

#endif // LIGHT_FALLOFF_GLSL
