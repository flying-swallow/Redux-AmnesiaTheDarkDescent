#pragma once

#include "../HostDefinitions.h"

// Multi-Scale Mean Estimator
// https://github.com/Apress/ray-tracing-gems/blob/master/Ch_25_Hybrid_Rendering_for_Real-Time_Ray_Tracing/MultiscaleMeanEstimator.hlsl
//
// Shared with C++ and GLSL — the `__init` constructor + the `MSME`
// driver are gated to `__SLANG_COMPILER__` so non-Slang TUs see plain
// POD layout only. Body is wrapped in HOST_NAMESPACE_BEGIN/END so C++
// resolves `float3` to `hpl::float3`; Slang/GLSL see the macros expand
// to nothing.

HOST_NAMESPACE_BEGIN

// Field layout mirrors std430 explicitly: each float3 that's followed by
// another float3 needs a trailing-4-byte pad to push the next vec3 to a
// 16-aligned offset. float3-followed-by-float packs naturally (the scalar
// fills the vec3's 4-byte alignment tail), so vbbr/inconsistency don't
// need pads. Without these, host sizeof drifts from the Slang ArrayStride
// and bindless storage reads return zeros for embedded MSMEData.
SLANG_PUBLIC struct MSMEData
{
    SLANG_PUBLIC float3 mean;
    SLANG_PUBLIC float  _padMean;
    SLANG_PUBLIC float3 shortMean;
    SLANG_PUBLIC float vbbr;
    SLANG_PUBLIC float3 variance;
    SLANG_PUBLIC float inconsistency;

#ifdef __SLANG_COMPILER__
    public __init()
    {
        this.mean = float3(0.f);
        this._padMean = 0.f;
        this.shortMean = float3(0.f);
        this.vbbr = 0.f;
        this.variance = float3(0.f);
        this.inconsistency = 1.f;
    }
#endif
};

#ifdef __SLANG_COMPILER__

public float3 MSME(float3 y, inout MSMEData data, float shortWindowBlend = 0.08f)
{
    float3 mean = data.mean;
    float3 shortMean = data.shortMean;
    float vbbr = data.vbbr;
    float3 variance = data.variance;
    float inconsistency = data.inconsistency;

    // Suppress fireflies.
    {
        float3 dev = sqrt(max(1e-5, variance));
        float3 highThreshold = 0.1 + shortMean + dev * 8;
        float3 overflow = max(0, y - highThreshold);
        y -= overflow;
    }

    float3 delta = y - shortMean;
    shortMean = lerp(shortMean, y, shortWindowBlend);
    float3 delta2 = y - shortMean;

    // This should be a longer window than shortWindowBlend to avoid bias
    // from the variance getting smaller when the short-term mean does.
    float varianceBlend = shortWindowBlend * 0.5;
    variance = lerp(variance, delta * delta2, varianceBlend);
    float3 dev = sqrt(max(1e-5, variance));

    float3 shortDiff = mean - shortMean;

    float relativeDiff = dot(float3(0.299, 0.587, 0.114), abs(shortDiff) / max(1e-5, dev));
    inconsistency = lerp(inconsistency, relativeDiff, 0.08);

    float varianceBasedBlendReduction = clamp(dot(float3(0.299, 0.587, 0.114), 0.5 * shortMean / max(1e-5, dev)), 1.0 / 32, 1);

    float3 catchUpBlend = clamp(smoothstep(0, 1, relativeDiff * max(0.02, inconsistency - 0.2)), 1.0 / 256, 1);
    catchUpBlend *= vbbr;

    vbbr = lerp(vbbr, varianceBasedBlendReduction, 0.1);
    mean = lerp(mean, y, saturate(catchUpBlend));

    // Output
    data.mean = mean;
    data.shortMean = shortMean;
    data.vbbr = vbbr;
    data.variance = variance;
    data.inconsistency = inconsistency;

    return mean;
}

#endif

HOST_NAMESPACE_END
