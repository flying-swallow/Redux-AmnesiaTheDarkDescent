// Hand-written Metal port of amnesia/slang/DirectLightingPass/DirectAtrousPass.cs.slang
//
// SVGF-lite edge-aware à-trous wavelet filter for the direct-lighting pass
// (numthreads 16,16,1). Runs kAtrousIterations times (host loop), tap spacing
// doubling each iteration (gAtrous.stepSize 1 -> 2 -> 4 ...). Edge-stopping
// weights gate on view depth (sigmaZ), world normal (sigmaN exponent) and
// luminance (sigmaL, scaled by a local 3x3 variance estimate, widened while the
// temporal history (count) is short). Filters demodulated irradiance only.
//
// This pass touches NO bindless set-0 / set-1 globals — everything is per-pass
// (set 2). Layout (mtl [[id]] == per-pass binding; see kDirectAtrousCs in
// HybridRenderer.cpp):
//   Set2 buffer(2): gAtrousIn id(0)[float4,sample]  gDirectKey id(1)[float4,sample]
//                   gAtrousOut id(2)[float4,read_write]  gAtrous(AtrousParams) id(3)
#include <metal_stdlib>
using namespace metal;

// Mirror of the slang AtrousParams UBO (padded to 16B).
struct AtrousParams { uint stepSize; uint _pad0; uint _pad1; uint _pad2; };

// Edge-stop constants (SceneTypes.slang / Constants.h). Declared inline here —
// this pass has no shared header carrying them.
constant float kSvgfSigmaZ        = 4.0f;
constant float kSvgfSigmaN        = 64.0f;
constant float kSvgfSigmaL        = 4.0f;
constant float kDirectVarianceBoost = 4.0f;

constant float3 kLum = float3(0.2126f, 0.7152f, 0.0722f);

static float luma(float3 c) { return dot(c, kLum); }

// 1D B3-spline a-trous kernel {3/8, 1/4, 1/16}.
static float atrousKernel(int o) {
    o = abs(o);
    if (o == 0) return 0.375f;
    if (o == 1) return 0.25f;
    return 0.0625f;
}

struct Set2 {
    texture2d<float, access::read>       gAtrousIn  [[id(0)]];
    texture2d<float, access::read>       gDirectKey [[id(1)]];
    texture2d<float, access::read_write> gAtrousOut [[id(2)]];
    constant AtrousParams*               gAtrous    [[id(3)]];
};

[[kernel]] void csMain(uint3 tid [[thread_position_in_grid]],
                       device Set2& set2 [[buffer(2)]]) {
    texture2d<float, access::read>       gAtrousIn  = set2.gAtrousIn;
    texture2d<float, access::read>       gDirectKey = set2.gDirectKey;
    texture2d<float, access::read_write> gAtrousOut = set2.gAtrousOut;
    constant AtrousParams*               gAtrous    = set2.gAtrous;

    uint w = gAtrousOut.get_width();
    uint h = gAtrousOut.get_height();
    int2 p = int2(tid.xy);
    if (p.x >= int(w) || p.y >= int(h))
        return;

    float4 center = gAtrousIn.read(uint2(p));
    float4 ckey   = gDirectKey.read(uint2(p));
    float  zc     = ckey.x;
    float3 nc     = ckey.yzw;

    // Zero normal = miss / invalid surface — pass it through untouched.
    if (dot(nc, nc) <= 1.0e-8f) {
        gAtrousOut.write(center, uint2(p));
        return;
    }
    nc = normalize(nc);

    float3 centerRgb   = center.rgb;
    float  centerCount = center.a;
    float  lumC        = luma(centerRgb);

    // Local 3x3 luminance variance estimate (unit spacing, same-surface taps),
    // widened while the temporal history is short.
    float mL = 0.0f, mL2 = 0.0f, wv = 0.0f;
    for (int dy = -1; dy <= 1; ++dy)
    for (int dx = -1; dx <= 1; ++dx) {
        int2 q = p + int2(dx, dy);
        if (q.x < 0 || q.y < 0 || q.x >= int(w) || q.y >= int(h)) continue;
        float4 qkey = gDirectKey.read(uint2(q));
        if (dot(qkey.yzw, qkey.yzw) <= 1.0e-8f) continue;
        float l = luma(gAtrousIn.read(uint2(q)).rgb);
        mL += l; mL2 += l * l; wv += 1.0f;
    }
    float variance = 0.0f;
    if (wv > 0.0f) {
        mL /= wv; mL2 /= wv;
        variance = max(0.0f, mL2 - mL * mL);
    }
    variance *= (1.0f + kDirectVarianceBoost / max(centerCount, 1.0f));
    float phiL = kSvgfSigmaL * sqrt(variance) + 1.0e-4f;

    int step = int(gAtrous->stepSize);

    float3 sum  = float3(0.0f);
    float  wsum = 0.0f;
    for (int dy = -2; dy <= 2; ++dy)
    for (int dx = -2; dx <= 2; ++dx) {
        int2 q = p + int2(dx, dy) * step;
        if (q.x < 0 || q.y < 0 || q.x >= int(w) || q.y >= int(h)) continue;

        float4 qkey = gDirectKey.read(uint2(q));
        float3 nq   = qkey.yzw;
        if (dot(nq, nq) <= 1.0e-8f) continue;   // skip invalid neighbours
        float  zq   = qkey.x;
        nq = normalize(nq);

        float3 sRgb = gAtrousIn.read(uint2(q)).rgb;

        // Edge-stopping weights.
        float wz = exp(-abs(zc - zq) / (kSvgfSigmaZ * 0.01f * max(zc, 1.0e-3f) + 1.0e-4f));
        float wn = pow(max(0.0f, dot(nc, nq)), kSvgfSigmaN);
        float wl = exp(-abs(lumC - luma(sRgb)) / phiL);
        float hw = atrousKernel(dx) * atrousKernel(dy);

        float wgt = hw * wz * wn * wl;
        sum  += sRgb * wgt;
        wsum += wgt;
    }

    float3 outRgb = wsum > 0.0f ? sum / wsum : centerRgb;
    gAtrousOut.write(float4(outRgb, centerCount), uint2(p));
}
