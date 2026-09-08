#pragma once
// Water-surface helpers. Water.frag.slang and WaterGuide.frag.slang consume
// this through WaterSurface.slang; it is kept in its own module so any future
// water pass can share the exact same wave-animated normal.
//
// Ported from AmnesiaTheDarkDescent/HPL2/resource/translucency_water.frag.fsl:
// two scrolling normal-map samples (one slow + base frequency, one faster +
// 1.2x frequency) blended 70/30 to break up periodicity. Returned in TANGENT
// space; callers rotate it into their own space (world, for the raster passes)
// via their own TBN frame.

#include "Constants.h"
import bindless;
import SceneTypes;

// `afT` is the engine animation-time accumulator (gPerFrame.afT = GetTimeCount).
// Normal-map samples use caller-supplied gradients with SampleGrad. This module
// remains stage-free because it emits no derivative instruction; a future
// ray-tracing caller would need to supply a cone-derived footprint instead (see
// notes/ray_cone_texture_lod.md and RayCone.slang::kTexLodTopMipBase).
// waveAmplitude / waveFreq are scaled by the reference constants below — the
// material authors small whole numbers (e.g. amplitude 0.2, freq 1), which
// the legacy translucency_water.frag.fsl maps to a subtle ripple via these.
static const float kWaveAmplitudeScale = 0.04f;
static const float kWaveFrequencyScale = 10.0f;

void waterWaveSampleGradients(float j01, float j10,
                              float2 dBaseUvDx, float2 dBaseUvDy,
                              out float2 dUvDx, out float2 dUvDy)
{
    float j11 = 1.0f + j10 * j01;
    dUvDx = float2(dBaseUvDx.x + j01 * dBaseUvDx.y,
                   j10 * dBaseUvDx.x + j11 * dBaseUvDx.y);
    dUvDy = float2(dBaseUvDy.x + j01 * dBaseUvDy.y,
                   j10 * dBaseUvDy.x + j11 * dBaseUvDy.y);
}

float3 waterWaveNormalTangent(WaterMaterial water, float2 baseUv,
                              float2 dBaseUvDx, float2 dBaseUvDy, float afT)
{
    float waveT     = afT * water.waveSpeed;
    float amplitude = water.waveAmplitude * kWaveAmplitudeScale;
    float frequency = water.waveFreq      * kWaveFrequencyScale;

    float fT1 = waveT * 0.8f;
    float2 uv1 = baseUv + waveT * 0.01f;
    float sample1Arg1 = fT1 + uv1.y * frequency;
    float c1 = cos(sample1Arg1);
    uv1.x += sin(sample1Arg1) * amplitude;
    float sample1Arg2 = fT1 + uv1.x * frequency;
    float c2 = cos(sample1Arg2);
    uv1.y += sin(sample1Arg2) * amplitude;
    float2 duv1dx;
    float2 duv1dy;
    waterWaveSampleGradients(amplitude * frequency * c1,
                             amplitude * frequency * c2,
                             dBaseUvDx, dBaseUvDy, duv1dx, duv1dy);

    float fT2 = waveT * -2.6f;
    float2 uv2 = baseUv + waveT * -0.012f;
    float amplitude2 = amplitude * 0.75f;
    float frequency2 = frequency * 1.2f;
    float sample2Arg1 = fT2 + uv2.y * frequency2;
    float c3 = cos(sample2Arg1);
    uv2.x += sin(sample2Arg1) * amplitude2;
    float sample2Arg2 = fT2 + uv2.x * frequency2;
    float c4 = cos(sample2Arg2);
    uv2.y += sin(sample2Arg2) * amplitude2;
    float2 duv2dx;
    float2 duv2dy;
    waterWaveSampleGradients(amplitude2 * frequency2 * c3,
                             amplitude2 * frequency2 * c4,
                             dBaseUvDx, dBaseUvDy, duv2dx, duv2dy);

    uint normalTex = water.tex[1];
    if (normalTex == kInvalidTextureIndex)
        return float3(0.0f, 0.0f, 1.0f);

    float3 n1 = sampleBindless2DGrad(normalTex, uv1, duv1dx, duv1dy).xyz * 2.0f - 1.0f;
    float3 n2 = sampleBindless2DGrad(normalTex, uv2, duv2dx, duv2dy).xyz * 2.0f - 1.0f;
    return normalize(n1 * 0.7f + n2 * 0.3f);
}
