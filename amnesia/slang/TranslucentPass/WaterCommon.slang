#pragma once
// Shared water-surface helpers used by both the raster water shader
// (Water.frag.slang) and the ray-traced water bounce (SurfelVBuffer.rt.slang),
// so the wave-animated normal stays identical between the two passes.
//
// Ported from AmnesiaTheDarkDescent/HPL2/resource/translucency_water.frag.fsl:
// two scrolling normal-map samples (one slow + base frequency, one faster +
// 1.2x frequency) blended 70/30 to break up periodicity. Returned in TANGENT
// space; callers rotate into view (raster) or world (RT) space via their own
// TBN frame.

#include "Constants.h"
import bindless;
import SceneTypes;

// `afT` is the engine animation-time accumulator (gPerFrame.afT = GetTimeCount).
// Uses SampleLevel(...0) so it is valid from a ray-tracing stage (no implicit
// derivatives); the fragment path reads LOD 0 too, which is what water wants.
// waveAmplitude / waveFreq are scaled by the reference constants below — the
// material authors small whole numbers (e.g. amplitude 0.2, freq 1), which
// the legacy translucency_water.frag.fsl maps to a subtle ripple via these.
static const float kWaveAmplitudeScale = 0.04f;
static const float kWaveFrequencyScale = 10.0f;
float3 waterWaveNormalTangent(WaterMaterial water, float2 baseUv, float afT)
{
    float waveT     = afT * water.waveSpeed;
    float amplitude = water.waveAmplitude * kWaveAmplitudeScale;
    float frequency = water.waveFreq      * kWaveFrequencyScale;

    float fT1 = waveT * 0.8f;
    float2 uv1 = baseUv + waveT * 0.01f;
    uv1.x += sin(fT1 + uv1.y * frequency) * amplitude;
    uv1.y += sin(fT1 + uv1.x * frequency) * amplitude;

    float fT2 = waveT * -2.6f;
    float2 uv2 = baseUv + waveT * -0.012f;
    uv2.x += sin(fT2 + uv2.y * frequency * 1.2f) * amplitude * 0.75f;
    uv2.y += sin(fT2 + uv2.x * frequency * 1.2f) * amplitude * 0.75f;

    uint normalTex = water.tex[1];
    if (normalTex == kInvalidTextureIndex)
        return float3(0.0f, 0.0f, 1.0f);

    float3 n1 = sampleBindless2DLevel(normalTex, uv1, 0.0f).xyz * 2.0f - 1.0f;
    float3 n2 = sampleBindless2DLevel(normalTex, uv2, 0.0f).xyz * 2.0f - 1.0f;
    return normalize(n1 * 0.7f + n2 * 0.3f);
}
