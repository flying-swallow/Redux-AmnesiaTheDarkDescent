#ifndef FRESNEL_GLSL
#define FRESNEL_GLSL

// Legacy Amnesia Fresnel — ported from
// AmnesiaTheDarkDescent/HPL2/resource/deferred_common.h.fsl:1-5.
// Cheap empirical fit, not Schlick. Used by the visibility-buffer composite
// to modulate environment-cube reflections by view angle and the per-material
// frenselBias / frenselPow knobs that ship on DiffuseMaterial.
float Fresnel(float eDotN, float bias, float p)
{
    float facing = 1.0 - eDotN;
    return max(bias + (1.0 - bias) * pow(abs(facing), p), 0.0);
}

#endif // FRESNEL_GLSL
