#pragma once
// Shared surfel-cache read-side helpers: cell lookup, coverage test, and the
// per-surfel contribution weights. The falloff kernel + Chebyshev
// depth-refinement math was previously copy-pasted into SurfelGenerationPass,
// SurfelEvaluationPass, SurfelShade.gatherSurfelIndirect, SurfelRayTrace's
// finalize and SurfelIntegratePass — every copy was the same math. Only WHICH
// factors a pass applies differs, so that choice stays at the call sites
// (finalize is kernel-only, coverage sums the kernel pre depth refinement,
// blend-delay weighting stays per-pass).

import bindless;                // gCellInfoBuffer, gSurfelDepth, gSurfelDepthSampler
import SceneTypes;              // kCellUnit (shared consts)
import SurfelGI.SurfelTypes;    // CellInfo
import SurfelGI.SurfelUtils;    // getCellPos / isCellValid / getFlattenCellIndex / getSurfelDepthUV

// Resolve the surfel-grid cell at posW. Returns false (outputs zeroed) when
// posW is outside the camera-centred grid.
public bool lookupSurfelCell(float3 posW, float3 cameraPosW,
                             out uint flattenIndex, out CellInfo cellInfo)
{
    flattenIndex = 0u;
    cellInfo = { 0u, 0u };
    int3 cellPos = getCellPos(posW, cameraPosW, kCellUnit);
    if (!isCellValid(cellPos))
        return false;
    flattenIndex = getFlattenCellIndex(cellPos);
    cellInfo = gCellInfoBuffer[flattenIndex];
    return true;
}

// Gather geometry for one covering surfel, produced by testSurfelCoverage.
public struct SurfelHit
{
    public float  dist;     // |posW - surfelPos|
    public float  dotN;     // dot(normalW, surfelNormal)
    public float3 dirUnit;  // (posW - surfelPos) / dist — surfel → shading point
};

// Coverage test for one surfel at a shading point: inside the radius and
// front-facing (dotN > 0). surfelNormal is dotted as-passed — callers pass
// exactly what the reference passed (SurfelBounds.normal is pre-normalized,
// Surfel.normal needs normalize(); the integrate pass's share loop
// intentionally dots raw normals).
public bool testSurfelCoverage(float3 posW, float3 normalW,
                               float3 surfelPos, float3 surfelNormal,
                               float radius, out SurfelHit hit)
{
    hit = { 0.f, 0.f, float3(0.f) };
    float3 bias  = posW - surfelPos;
    float  dist2 = dot(bias, bias);
    if (dist2 >= radius * radius)
        return false;
    float dotN = dot(normalW, surfelNormal);
    if (dotN <= 0.0f)
        return false;
    hit.dist    = sqrt(dist2);
    hit.dotN    = dotN;
    hit.dirUnit = bias / hit.dist;
    return true;
}

// Geometric falloff kernel: smoothstep(saturate(dotN) · saturate(1 − d/r)).
// This is the value coverage accumulates (pre depth refinement).
public float surfelKernelWeight(float dotN, float dist, float radius)
{
    float contribution = 1.f;
    contribution *= saturate(dotN);
    contribution *= saturate(1.0f - dist / radius);
    return smoothstep(0.f, 1.f, contribution);
}

// Variance-weighted (Chebyshev) attenuation from the surfel depth atlas —
// multiply into the radiance weight, NOT into coverage. dirUnit points from
// the surfel toward the shading point; surfelNormal must match the frame the
// surfel's atlas tile was written with. The integrate pass deliberately
// passes the CENTER surfel's index + normal when weighting neighbours (the
// center's depth function occludes sharing from behind it) — not a
// copy-paste bug.
public float surfelDepthWeight(uint surfelIndex, float3 dirUnit,
                               float3 surfelNormal, float dist)
{
    float2 uv = getSurfelDepthUV(surfelIndex, dirUnit, surfelNormal);
    float2 surfelDepth = gSurfelDepth.SampleLevel(gSurfelDepthSampler, uv, 0u);
    float mean    = surfelDepth.x;
    float sqrMean = surfelDepth.y;
    if (dist > mean)
    {
        float variance = sqrMean - pow(mean, 2);
        return variance / (variance + pow(dist - mean, 2));
    }
    return 1.f;
}
