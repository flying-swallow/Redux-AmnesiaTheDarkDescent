#pragma once
// Light-selection reservoir for ReSTIR DI (Bitterli et al. 2020), the biased
// reuse variant.
//
// The "sample" stored per pixel is just the chosen analytic light's packed id
// (packLightId(type, slot), stable across frames) — NOT a point on the light. The soft-shadow disk point is drawn fresh
// (blue noise) at resolve time and smoothed by the temporal/spatial reservoir
// reuse plus the à-trous filter, so it needs no Jacobian and isn't persisted.
// This keeps the whole pipeline at one shadow ray per pixel per frame while the
// effective candidate count grows into the hundreds via reuse.
//
// Empty reservoir = W<=0 (or M<=0); a zero-cleared texel decodes to exactly
// that, so first-frame history reads are well defined and no NaN sentinel is
// stored (asfloat(0xFFFFFFFF) is avoided deliberately).

#include "HostDefinitions.h"
import Random;

public struct Reservoir
{
    public uint  y;       // selected light — packed id (packLightId(type, slot))
    public float W;       // unbiased contribution weight (UCW)
    public float M;       // confidence — candidate / merged sample count
    // Transient (build + combine), not persisted to the texture:
    public float wSum;    // running weight sum
    public float pHatY;   // target value of `y` at this pixel

    [mutating]
    public void init()
    {
        y = 0u; W = 0.f; M = 0.f; wSum = 0.f; pHatY = 0.f;
    }

    public bool isEmpty() { return W <= 0.f || M <= 0.f; }

    // RIS build: stream one candidate light whose target (pHat) at this pixel is
    // `pHatLi`. Candidates are the cell's lights enumerated uniformly.
    [mutating]
    public void streamCandidate(uint li, float pHatLi, inout RNG rng)
    {
        wSum += pHatLi;
        M    += 1.f;
        if (pHatLi > 0.f && rng.next_float() * wSum <= pHatLi)
        {
            y     = li;
            pHatY = pHatLi;
        }
    }

    // Finalize after a uniform-candidate RIS build: with source pdf 1/n over the
    // n enumerated lights, W reduces to (Σ pHat) / pHat(y).
    [mutating]
    public void finalizeBuild()
    {
        W = (pHatY > 0.f) ? wSum / pHatY : 0.f;
    }

    // Reservoir-of-reservoirs combine (temporal / spatial reuse): fold `other`
    // in as a single candidate of weight pHatAtX·other.W·other.M, where pHatAtX
    // is the target of other's selected light re-evaluated at THIS pixel.
    [mutating]
    public void merge(Reservoir other, float pHatAtX, inout RNG rng)
    {
        float w = pHatAtX * other.W * other.M;
        wSum += w;
        M    += other.M;
        if (w > 0.f && rng.next_float() * wSum <= w)
        {
            y     = other.y;
            pHatY = pHatAtX;
        }
    }

    // Finalize after merge()s: W = Σw / (M · pHat(y)).
    [mutating]
    public void finalizeCombine()
    {
        W = (pHatY > 0.f && M > 0.f) ? wSum / (M * pHatY) : 0.f;
    }
};

// RGBA32F texel <-> reservoir. Only the persisted fields (y, W, M) round-trip;
// the transient build state is reset on unpack.
public float4 packReservoir(Reservoir r)
{
    return float4(asfloat(r.y), r.W, r.M, 0.f);
}

public Reservoir unpackReservoir(float4 t)
{
    Reservoir r;
    r.y     = asuint(t.x);
    r.W     = t.y;
    r.M     = t.z;
    r.wSum  = 0.f;
    r.pHatY = 0.f;
    return r;
}
