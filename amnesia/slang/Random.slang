#pragma once

#include "HostDefinitions.h"

// --- Spatiotemporal blue noise (asset-free) -------------------------------
// Used to draw the dominant per-frame sample dimensions (e.g. the soft-shadow
// disk sample) so the residual error is high-frequency in screen space and
// low-discrepancy in time — both the temporal accumulation and the SVGF-lite
// à-trous filter remove it far better than the white xoshiro stream, and raw
// frames look less clumpy. Spatial term: Jimenez interleaved-gradient noise
// (blue over the pixel grid). Temporal term: the R2 plastic-constant additive
// recurrence (Cranley-Patterson rotation per frame). A precomputed STBN texture
// would be slightly better and can be swapped in behind blueNoise2() later.
public float interleavedGradientNoise(uint2 p)
{
    return frac(52.9829189f * frac(0.06711056f * float(p.x) + 0.00583715f * float(p.y)));
}

// `dim` selects a decorrelated pair of sample dimensions for callers that need
// more than one 2D sample per pixel/frame.
public float2 blueNoise2(uint2 pixel, uint frame, uint dim)
{
    // 1/g, 1/g^2 of the plastic number g — the R2 low-discrepancy recurrence.
    const float a1 = 0.7548776662466927f;
    const float a2 = 0.5698402909980532f;
    uint2 dimOffset = uint2(dim * 59u, dim * 37u);
    float s0 = interleavedGradientNoise(pixel + dimOffset);
    float s1 = interleavedGradientNoise(pixel + dimOffset + uint2(101u, 71u));
    float2 r = float2(frac(float(frame) * a1), frac(float(frame) * a2));
    return frac(float2(s0, s1) + r);
}

// Concentric (low-distortion) unit-disk map of two [0,1) samples. Lives here as a
// shared sampling util so both the soft-shadow path (DiffuseShading) and the
// per-light source sampling (ILight) can use it.
public float2 concentricDiskSample(float2 u)
{
    float2 o = 2.0f * u - 1.0f;
    if (o.x == 0.0f && o.y == 0.0f) return float2(0.0f);
    float r, theta;
    const float kPiOver4 = 0.78539816f;
    const float kPiOver2 = 1.57079633f;
    if (abs(o.x) > abs(o.y)) { r = o.x; theta = kPiOver4 * (o.y / o.x); }
    else                     { r = o.y; theta = kPiOver2 - kPiOver4 * (o.x / o.y); }
    return r * float2(cos(theta), sin(theta));
}

public struct RNG
{
    public uint2 s;
    // Blue-noise sampling context (set by init): the originating pixel, the
    // frame index, and a running dimension counter advanced by next_blue2().
    public uint2 pixel;
    public uint  frame;
    public uint  dim;

    public uint rotl(uint x, uint k)
    {
        return (x << k) | (x >> (32 - k));
    }

    [mutating]
    public uint next()
    {
        uint result = s.x * 0x9e3779bb;

        s.y ^= s.x;
        s.x = rotl(s.x, 26) ^ s.y ^ (s.y << 9);
        s.y = rotl(s.y, 13);

        return result;
    }

    public uint hash(uint seed)
    {
        seed = (seed ^ 61) ^ (seed >> 16);
        seed *= 9;
        seed = seed ^ (seed >> 4);
        seed *= 0x27d4eb2d;
        seed = seed ^ (seed >> 15);
        return seed;
    }

    [mutating]
    public void init(uint2 id, uint frameIndex)
    {
        pixel = id;
        frame = frameIndex;
        dim   = 0u;
        id += uint2(frameIndex, frameIndex);
        uint s0 = (id.x << 16) | id.y;
        uint s1 = frameIndex;
        s.x = hash(s0);
        s.y = hash(s1);
        next();
    }

    // Spatiotemporal blue-noise 2D sample. Each call advances to a fresh,
    // decorrelated dimension pair so multiple soft-shadow disks per pixel don't
    // share the same offset.
    [mutating]
    public float2 next_blue2()
    {
        float2 bn = blueNoise2(pixel, frame, dim);
        dim += 1u;
        return bn;
    }

    [mutating]
    public float next_float()
    {
        uint u = 0x3f800000 | (next() >> 9);
        return asfloat(u) - 1.0;
    }

    [mutating]
    public uint next_uint(uint nmax)
    {
        float f = next_float();
        return uint(floor(f * nmax));
    }

    [mutating]
    public float2 next_float2()
    {
        return float2(next_float(), next_float());
    }

    [mutating]
    public float3 next_float3()
    {
        return float3(next_float(), next_float(), next_float());
    }
};
