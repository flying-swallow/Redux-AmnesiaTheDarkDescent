// SVGF-lite edge-aware à-trous wavelet filter for the direct-lighting pass.
//
// The direct pass produces a temporally-accumulated but still 1-spp-noisy
// demodulated irradiance (rgb) plus a history length (count) and a surface key
// (view depth + world normal). This pass is the spatial half of the denoiser:
// run kAtrousIterations times with the tap spacing doubling each iteration
// (stepSize 1 → 2 → 4 …), sharing the estimate across same-surface neighbours.
// Edge-stopping weights gate on depth (σZ), normal (σN exponent) and luminance
// (σL, scaled by a local variance estimate so sharp lighting features survive).
// The count is carried through unfiltered as the per-pixel confidence; while it
// is low (fresh / disoccluded pixel) the luminance gate is widened
// (kDirectVarianceBoost) so the noise spike is blurred harder before it has had
// frames to converge.
//
// Filters the demodulated irradiance only — the composite multiplies albedo
// afterwards, so cross-surface colour bleed is impossible from this filter.

#include "../HostDefinitions.h"
#include "../Constants.h"

import SceneTypes;   // kSvgfSigma*, kAtrousIterations, kDirectVarianceBoost

// Per-iteration parameters (UBO). Padded to 16 bytes to match the host struct.
struct AtrousParams
{
    uint stepSize;
    uint _pad0;
    uint _pad1;
    uint _pad2;
};

// Pass-private (set 2).
[vk::binding(0, 2)] Texture2D<float4>    gAtrousIn;   // (rgb irradiance, count)
[vk::binding(1, 2)] Texture2D<float4>    gDirectKey;  // (viewZ, normal.xyz)
[vk::binding(2, 2)] RWTexture2D<float4>  gAtrousOut;  // (rgb filtered, count)
[vk::binding(3, 2)] ConstantBuffer<AtrousParams> gAtrous;

static const float3 kLum = float3(0.2126f, 0.7152f, 0.0722f);

float luma(float3 c) { return dot(c, kLum); }

// 1D B3-spline à-trous kernel {3/8, 1/4, 1/16}.
float atrousKernel(int o)
{
    o = abs(o);
    if (o == 0) return 0.375f;
    if (o == 1) return 0.25f;
    return 0.0625f;
}

[shader("compute")]
[numthreads(16, 16, 1)]
void csMain(uint3 tid : SV_DispatchThreadID)
{
    uint w, h;
    gAtrousOut.GetDimensions(w, h);
    int2 p = int2(tid.xy);
    if (p.x >= int(w) || p.y >= int(h))
        return;

    float4 center = gAtrousIn.Load(int3(p, 0));
    float4 ckey   = gDirectKey.Load(int3(p, 0));
    float  zc     = ckey.x;
    float3 nc     = ckey.yzw;

    // Zero normal = miss / invalid surface (the direct pass writes that for
    // background). Pass it through untouched.
    if (dot(nc, nc) <= 1.0e-8f)
    {
        gAtrousOut[p] = center;
        return;
    }
    nc = normalize(nc);

    float3 centerRgb   = center.rgb;
    float  centerCount = center.a;
    float  lumC        = luma(centerRgb);

    // Local 3×3 luminance variance estimate (unit spacing, same-surface taps),
    // widened while the temporal history is short.
    float  mL = 0.f, mL2 = 0.f, wv = 0.f;
    for (int dy = -1; dy <= 1; ++dy)
    for (int dx = -1; dx <= 1; ++dx)
    {
        int2 q = p + int2(dx, dy);
        if (q.x < 0 || q.y < 0 || q.x >= int(w) || q.y >= int(h)) continue;
        float4 qkey = gDirectKey.Load(int3(q, 0));
        if (dot(qkey.yzw, qkey.yzw) <= 1.0e-8f) continue;
        float l = luma(gAtrousIn.Load(int3(q, 0)).rgb);
        mL += l; mL2 += l * l; wv += 1.f;
    }
    float variance = 0.f;
    if (wv > 0.f)
    {
        mL /= wv; mL2 /= wv;
        variance = max(0.f, mL2 - mL * mL);
    }
    variance *= (1.f + kDirectVarianceBoost / max(centerCount, 1.f));
    float phiL = kSvgfSigmaL * sqrt(variance) + 1.0e-4f;

    int step = int(gAtrous.stepSize);

    float3 sum  = float3(0.f);
    float  wsum = 0.f;
    for (int dy = -2; dy <= 2; ++dy)
    for (int dx = -2; dx <= 2; ++dx)
    {
        int2 q = p + int2(dx, dy) * step;
        if (q.x < 0 || q.y < 0 || q.x >= int(w) || q.y >= int(h)) continue;

        float4 qkey = gDirectKey.Load(int3(q, 0));
        float3 nq   = qkey.yzw;
        if (dot(nq, nq) <= 1.0e-8f) continue;   // skip invalid neighbours
        float  zq   = qkey.x;
        nq = normalize(nq);

        float3 sRgb = gAtrousIn.Load(int3(q, 0)).rgb;

        // Edge-stopping weights.
        float wz = exp(-abs(zc - zq) / (kSvgfSigmaZ * 0.01f * max(zc, 1.0e-3f) + 1.0e-4f));
        float wn = pow(max(0.f, dot(nc, nq)), kSvgfSigmaN);
        float wl = exp(-abs(lumC - luma(sRgb)) / phiL);
        float hw = atrousKernel(dx) * atrousKernel(dy);

        float wgt = hw * wz * wn * wl;
        sum  += sRgb * wgt;
        wsum += wgt;
    }

    float3 outRgb = wsum > 0.f ? sum / wsum : centerRgb;
    gAtrousOut[p] = float4(outRgb, centerCount);
}
