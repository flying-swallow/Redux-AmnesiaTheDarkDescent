// Surfel-GI scatter ray-trace pipeline.
//
// Ported from Falcor's SurfelGI/SurfelRayTrace.rt.slang to use the
// project's Scene module + bindless set. The Falcor PBR stack
// (ShadingData / IMaterialInstance / BSDFSample / LobeType /
// AnalyticLightSample / sampleLight / envMap.eval / prepareShadingData /
// getMaterialInstance) is replaced with a hand-rolled diffuse-only
// shading path: albedo = diffuse texture sample, BSDF = albedo / π,
// sampling = cosine-weighted hemisphere, NEE via getLight().sample.
// No specular, no transmission, no env-map evaluation (miss returns
// zero). Surfel-radiance termination (`finalize`) keeps its original
// behavior. Inputs / outputs match the Falcor port — same SurfelRayResult
// layout, same gSurfel* lifecycle effects.

#include "../HostDefinitions.h"
#include "../Constants.h"

import SceneTypes;
import Scene;
import bindless;
import Brdf;                    // decodeMaterialRoughness (diffuse-lobe roughness)
import HitInfo;
import ILight;
import PerFrame.resource;
import SurfelGI.SurfelTypes;
import SurfelGI.SurfelUtils;
import SurfelGI.SurfelGather;    // cell lookup + coverage / kernel weight
import SurfelGI.SurfelLifecycle; // sleeping-surfel spawn ladder
import Grid;                    // gridCellPos / gridFlattenCellIndex / gridCellValid (light grid lookup)
import Random;
import DiffuseShading;          // shared DiffuseShading + prepareDiffuseShading

// Ray-step / max-step come from `Constants.h` (kDefaultRayStep / kDefaultMaxStep),
// and the per-frame counter is sourced from PerFrame.totalFrames — matches the
// SurfelEvaluationPass pattern, no runtime-tunable CB needed.

// One-over-pi factor for the diffuse BRDF normalization.
static const float kInvPi = 0.31830988618379067154f;

// Plane-offset epsilon for the shadow / scatter ray origin.
static const float kRayEps = 1.0e-3f;

// [status]
// 0x0001 : isSleeping
// 0x0002 : terminated
struct ScatterPayload
{
    float3 radiance;
    float3 thp;
    float3 origin;
    float3 direction;
    float firstRayLength;
    uint currStep;
    uint status;
    uint rngHi;     // RNG state carried in the payload so sampling is consistent across recursion steps
    uint rngLo;

    __init(uint2 rngState, bool isSleeping)
    {
        this.radiance = float3(0, 0, 0);
        this.thp = float3(1, 1, 1);
        this.origin = float3(0, 0, 0);
        this.direction = float3(0, 0, 0);
        this.firstRayLength = 0.f;
        this.currStep = 1u;
        this.status = isSleeping ? 0x0001 : 0x0000;
        this.rngHi = rngState.x;
        this.rngLo = rngState.y;
    }
}

// Shadow rays use inline RayQuery (no shadow-specific miss/anyhit entry
// points needed). RIProgram's RT pipeline layout is single-miss +
// single-hit-group, so keeping shadow off the SBT lets the whole
// pipeline build with one ray type.

// Plain POD for the diffuse shading state we hand-roll from VertexData +
// DiffuseShading + prepareDiffuseShading are shared with the composite stack via
// DiffuseShading (imported above). The path tracer wants per-texel
// roughness, so it calls prepareDiffuseShading(..., decodeRoughness=true).

// Cosine-weighted hemisphere sample in local-frame coords (z = up).
float3 sampleCosineHemisphere(float u1, float u2, out float pdf)
{
    float r   = sqrt(u1);
    float phi = 2.0f * 3.14159265358979f * u2;
    float3 dir;
    dir.x = r * cos(phi);
    dir.y = r * sin(phi);
    dir.z = sqrt(max(0.0f, 1.0f - u1));
    pdf   = dir.z * kInvPi;
    return dir;
}

// Build a tangent-space basis from `n`. Picks a helper axis not collinear
// with `n` and Gram-Schmidt orthogonalizes. Returns a 3x3 matrix whose
// columns are (T, B, N) so `mul(localDir, basis)` rotates the local
// hemisphere sample into world space.
float3x3 buildTangentSpace(float3 n)
{
    float3 helper = abs(n.x) < 0.9f ? float3(1, 0, 0) : float3(0, 1, 0);
    float3 t = normalize(cross(helper, n));
    float3 b = cross(n, t);
    return float3x3(t, b, n);
}

// Shadow visibility moved to gScene.materials.traceShadowRay (Scene.slang)
// — the local implementation here skipped the candidate-commit loop
// required by the engine's non-opaque BLAS, silently reporting every NEE
// shadow ray as unoccluded. Use the shared helper instead.

void traceScatterRay(inout ScatterPayload scatterPayload)
{
    RayDesc ray;
    ray.Origin    = scatterPayload.origin;
    ray.TMin      = 0.f;
    ray.TMax      = 1e30f;
    ray.Direction = scatterPayload.direction;

    TraceRay(
        gRtAccel,
        RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_CULL_BACK_FACING_TRIANGLES,
        kRayMaskOpaque,
        0, /* hitIdx */
        1, /* rayTypeCount (scatter only — shadow uses inline RayQuery) */
        0, /* missIdx */
        ray,
        scatterPayload
    );
}

// finalize() tuning — named from the reference's inline literals (value-identical).
static const uint  kFinalizeCellBudget   = 64u;   // skip the cache when the cell is this crowded
static const float kFinalizeBouncePr     = 0.2f;  // chance to bounce through instead of finalizing
static const uint  kSleepingSpawnCellCap = 8u;    // per-cell sleeping reservation cap
static const float kSleepingCullCoverage = 4.0f;  // sleeping coverage budget before culling

// Result of one surfel-cache gather at a path vertex.
struct SurfelCacheGather
{
    float4 Lr;                      // weighted radiance + total weight
    float  sleepingCoverage;        // kernel sum over sleeping surfels only
    int    loudestSleepingSurfel;   // cull candidate, -1 if none
};

// The gather loop — kernel weight only. The reference finalize never applied
// the depth-function or blend-delay refinements (adding the atlas sample here
// would change termination radiance and cost in the RT hot loop), so this
// stays kernel-only; the screen passes' refined gather lives in
// SurfelGather.slang. Bumps each covering surfel's ref counter as a side
// effect.
SurfelCacheGather gatherCellRadiance(CellInfo cellInfo, float3 posW, float3 normalW)
{
    SurfelCacheGather g;
    g.Lr = float4(0.f);
    g.sleepingCoverage = 0.f;
    g.loudestSleepingSurfel = -1;
    float maxContribution = 0.f;

    for (uint i = 0; i < cellInfo.surfelCount; ++i)
    {
        uint surfelIndex = gCellToSurfelBuffer[cellInfo.cellToSurfelBufferOffset + i];
        Surfel surfel = gSurfelBuffer[surfelIndex];

        SurfelHit hit;
        if (!testSurfelCoverage(posW, normalW, surfel.position,
                                normalize(surfel.normal), surfel.radius, hit))
            continue;

        float contribution = surfelKernelWeight(hit.dotN, hit.dist, surfel.radius);
        g.Lr += float4(surfel.radiance, 1.f) * contribution;

        SurfelRecycleInfo info = gSurfelRecycleInfoBuffer[surfelIndex];
        if ((info.status & 0x0001u) != 0u)  // sleeping
        {
            g.sleepingCoverage += contribution;
            if (maxContribution < contribution)
            {
                maxContribution = contribution;
                g.loudestSleepingSurfel = int(surfelIndex);
            }
        }

        uint prevRef;
        InterlockedAdd(gSurfelRefCounter[surfelIndex], 1u, prevRef);
    }
    return g;
}

// Empty-gather path: reserve a sleeping surfel in this cell when below the
// spawn cap. Bail before any atomic counter touches if the committed hit's
// instanceID is out of the bindless slot table — capturing
// gBindlessSlotGeneration[triangleHit.instanceID] in publishNewSurfel would
// feed collectCellInfo an anchor that GPUVM-faults next frame.
void trySpawnSleepingSurfel(uint flattenIndex, CellInfo cellInfo,
                            const VertexData v, const TriangleHit triangleHit)
{
    if (cellInfo.surfelCount >= kSleepingSpawnCellCap ||
        triangleHit.instanceID >= kObjectSlotCapacity)
        return;

    uint reservedCount;
    InterlockedAdd(gSurfelReservationBuffer[flattenIndex], 1u, reservedCount);
    if (reservedCount >= kSleepingSpawnCellCap)
        return;

    uint newIndex, validSlot;
    if (!tryAllocateSurfelSlot(newIndex, validSlot))
        return;

    Surfel newSurfel = Surfel(v.posW, v.normalW, 1e-6f);
    SurfelRecycleInfo recycle;
    recycle.life   = 1;
    recycle.frame  = 0;
    recycle.status = 1; // sleeping
    publishNewSurfel(newIndex, validSlot, newSurfel, recycle,
                     triangleHit.pack(), triangleHit.instanceID,
                     /*initialRefCount=*/1u);
}

void countMissBounce()
{
    uint prev;
    InterlockedAdd(gSurfelCounter[kSurfelCounterMissBounce], 1u, prev);
}

// Try to terminate the path early by sampling the surfel cache instead of
// continuing to trace. Returns true when surfel radiance was accumulated;
// returns false when the cell is empty / over-budget and the caller should
// bounce one more step.
bool finalize(inout ScatterPayload scatterPayload, const VertexData v, const TriangleHit triangleHit, inout RNG rng)
{
    uint flattenIndex;
    CellInfo cellInfo;
    if (!lookupSurfelCell(v.posW, gScene.camera.getPosition(), flattenIndex, cellInfo))
        return false;

    // Crowded cell, or randomly bounce through to keep paths exploring (the
    // RNG draw short-circuits when the budget check already bails).
    if (cellInfo.surfelCount > kFinalizeCellBudget || rng.next_float() < kFinalizeBouncePr)
    {
        countMissBounce();
        return false;
    }

    SurfelCacheGather g = gatherCellRadiance(cellInfo, v.posW, v.normalW);

    if (g.Lr.w <= 0.f)
    {
        trySpawnSleepingSurfel(flattenIndex, cellInfo, v, triangleHit);
        countMissBounce();
        return false;
    }

    // Cull the loudest sleeping surfel when sleeping coverage is over-budget.
    if (g.sleepingCoverage >= kSleepingCullCoverage && g.loudestSleepingSurfel != -1)
    {
        Surfel s = gSurfelBuffer[g.loudestSleepingSurfel];
        s.radius = 0;
        gSurfelBuffer[g.loudestSleepingSurfel] = s;
    }

    scatterPayload.radiance += scatterPayload.thp * (g.Lr.xyz / g.Lr.w);
    return true;
}

void handleHit(TriangleHit triangleHit, inout ScatterPayload scatterPayload)
{
    VertexData v = gScene.getVertexData(triangleHit);
    uint materialID = gScene.getMaterialID(triangleHit.instanceID);
    float illuminationAmount = gScene.getObject(triangleHit.instanceID).illuminationAmount;
    DiffuseShading sd = prepareDiffuseShading(v, materialID, illuminationAmount,
                                              /*decodeRoughness=*/true);

    if (scatterPayload.currStep == 1u)
        scatterPayload.firstRayLength = distance(scatterPayload.origin, v.posW);

    // Emission — emissive (illumination-map) surfaces add radiance directly into
    // the path, so they bounce into the surfel cache. Matches the reference's
    // `radiance += thp * emission`.
    scatterPayload.radiance += scatterPayload.thp * sd.emission;

    // Pull the RNG out of the payload, run NEE + bounce sample, push back.
    RNG rng;
    rng.s = uint2(scatterPayload.rngHi, scatterPayload.rngLo);

    // Art-directed bounce albedo (kSurfelGIAlbedoBoostExp): Amnesia's dark
    // diffuse content (~0.04 linear at GI hits) makes a physically-correct
    // bounce nearly black; the boost applies to the GI ray path only.
    float3 giAlbedo = pow(sd.albedo, kSurfelGIAlbedoBoostExp);

    // Direct lighting (one-sample NEE). scatterPayload.direction still holds
    // the incoming ray direction here (overwritten below for the next bounce),
    // so the outgoing/"view" direction ("V") at the hit is its negation; the
    // diffuse lobe (kDiffuseBrdf) is view-dependent.
    //
    // Importance-sample lights from the world-space light grid: pick uniformly
    // among only the lights whose radius reaches this shading point's cell
    // (pdf = 1/count). Empty / out-of-grid cell → no contribution.
    // next_uint(N) covers [0,N) (the old `N-1` form dropped the last light).
    CellLights cl = gScene.getCellLights(sd.posW);
    if (cl.count != 0u)
    {
        float3 V        = -scatterPayload.direction;
        uint   pick     = rng.next_uint(cl.count);
        uint   lightIdx = gScene.cellLightAt(cl, pick);
        float  invPdf   = float(cl.count);

        // Type-aware shade shared with the direct path (shadeAnalyticLight:
        // sample → NdotL-cull → shadow ray → Li·NdotL, demodulated). Point lights
        // use their full sampler (incl. gobo cube); spot lights use the same
        // cone-restricted SpotLight.sample() as the direct path — matching the
        // reference Falcor SurfelGI, where a spot's analytic GI contribution is
        // cone-shaped too. Box lights aren't NEE-sampled (and aren't binned into
        // the grid). The bounce needs outgoing radiance at the hit surface, so
        // albedo re-modulates the standard diffuse lobe's 1/π here. No scale knobs
        // (Falcor parity): the NEE shares shadeAnalyticLight and the same Li units
        // as the direct pass, so indirect lands in direct's units by construction.
        float3 origin   = sd.posW + sd.normalW * kRayEps;
        float3 perLight = giAlbedo * shadeAnalyticLight(sd.posW, sd.normalW,
                                                        V, sd.rough,
                                                        lightIdx, origin);
        scatterPayload.radiance += scatterPayload.thp * (perLight * invPdf);
    }

    // Sample the next bounce direction (cosine-hemisphere, local frame
    // around sd.normalW). BSDF * cosθ / pdf for cosine sampling =
    // albedo · diffuseLobeWeight (the lobe's 1/π cancels against the pdf;
    // at kDiffuseBrdfLambert the weight is 1 and this reduces to albedo).
    float3 localDir;
    float  bsdfPdf;
    {
        float u1 = rng.next_float();
        float u2 = rng.next_float();
        localDir = sampleCosineHemisphere(u1, u2, bsdfPdf);
    }
    float3 worldDir = normalize(mul(localDir, buildTangentSpace(sd.normalW)));

    // Lobe weight uses the same V as the NEE above (incoming ray, negated) —
    // computed BEFORE scatterPayload.direction is overwritten for the bounce.
    float bounceW;
    {
        float3 Vb = -scatterPayload.direction;
        float3 Hb = normalize(Vb + worldDir);
        bounceW = diffuseLobeWeight(saturate(dot(sd.normalW, worldDir)),
                                    max(dot(sd.normalW, Vb), 1e-4f),
                                    saturate(dot(worldDir, Hb)), sd.rough);
    }

    scatterPayload.origin    = sd.posW + sd.normalW * kRayEps;
    scatterPayload.direction = worldDir;
    scatterPayload.thp      *= giAlbedo * bounceW;

    // Last step / Russian roulette / surfel-cache finalize, same as the
    // Falcor port — the russian-roulette probability uses luminance(thp).
    uint rayStep = (scatterPayload.status & 0x0001u) != 0u ? kDefaultRayStep * 2u : kDefaultRayStep;
    if (scatterPayload.currStep < rayStep)
    {
        float rrValue = dot(scatterPayload.thp, float3(0.2126f, 0.7152f, 0.0722f));
        float prob    = max(0.f, 1.f - rrValue);
        if (rng.next_float() >= prob)
        {
            scatterPayload.thp /= max(1e-12f, 1.f - prob);
            if (any(scatterPayload.thp > 0.f))
                scatterPayload.currStep++;
            else
                scatterPayload.status |= 0x0002u;

            scatterPayload.rngHi = rng.s.x;
            scatterPayload.rngLo = rng.s.y;
            return;
        }
    }

    if (finalize(scatterPayload, v, triangleHit, rng))
        scatterPayload.status |= 0x0002u;
    else
        scatterPayload.currStep++;

    scatterPayload.rngHi = rng.s.x;
    scatterPayload.rngLo = rng.s.y;
}

// Miss / hit shaders.

[shader("miss")]
void scatterMiss(inout ScatterPayload scatterPayload)
{
    // Environment map evaluator isn't ported — treat the miss as black
    // radiance and terminate. When an env map binding gets added, this is
    // where the lookup goes.
    scatterPayload.status |= 0x0002u;
}

[shader("anyhit")]
void scatterAnyHit(inout ScatterPayload scatterPayload, BuiltInTriangleIntersectionAttributes attribs)
{
    // Alpha test (+ CoverageAmount dissolve) against the diffuse material —
    // the single shared gate, so fading objects also fade out of the GI.
    TriangleHit hit;
    hit.instanceID     = InstanceID();
    hit.primitiveIndex = PrimitiveIndex();
    hit.barycentrics   = attribs.barycentrics;
    VertexData v = gScene.getVertexData(hit);
    uint materialID = gScene.getMaterialID(hit.instanceID);
    if (gScene.materials.alphaTest(v, materialID, 0.f,
                                   gSceneObjects[hit.instanceID].dissolveAmount))
        IgnoreHit();
}

[shader("closesthit")]
void scatterCloseHit(inout ScatterPayload scatterPayload, BuiltInTriangleIntersectionAttributes attribs)
{
    TriangleHit triangleHit;
    triangleHit.instanceID     = InstanceID();
    triangleHit.primitiveIndex = PrimitiveIndex();
    triangleHit.barycentrics   = attribs.barycentrics;
    handleHit(triangleHit, scatterPayload);
}

[shader("raygeneration")]
void rayGen()
{
    uint totalRayCount;
    InterlockedAdd(gSurfelCounter[kSurfelCounterRequestedRay], 0u, totalRayCount);
    if (DispatchRaysIndex().x >= totalRayCount) return;

    uint rayIndex = DispatchRaysIndex().x;
    SurfelRayResult surfelRayResult = gSurfelRayResultBuffer[rayIndex];
    uint surfelIndex = surfelRayResult.surfelIndex;
    Surfel surfel = gSurfelBuffer[surfelIndex];
    SurfelRecycleInfo info = gSurfelRecycleInfoBuffer[surfelIndex];
    bool isSleeping = (info.status & 0x0001u) != 0u;

    RNG rng;
    rng.init(uint2(rayIndex, rayIndex), gPerFrame.totalFrames);

    // Uniform-hemisphere first ray. pdf is the TRUE uniform-hemisphere density
    // 1/(2π) — SurfelIntegratePass divides by it to form the irradiance/π
    // estimator. (The old 1/(16π) here was a bogus constant that cancelled
    // against a matching 1/(16π) in the integrator, leaving the cache 2× dark.)
    // Ray-guiding path stays out (USE_RAY_GUIDING is off by default).
    float3 dirLocal = normalize(hemispherepoint_uniform(rng.next_float(), rng.next_float()));
    float  pdfLocal = kInvPi * 0.5f; // 1/(2π)
    float3 dirWorld = normalize(mul(dirLocal, get_tangentspace(surfel.normal)));

    ScatterPayload payload = ScatterPayload(rng.s, isSleeping);
    payload.origin    = surfel.position;
    payload.direction = dirWorld;

    surfelRayResult.dirLocal = dirLocal;
    surfelRayResult.dirWorld = dirWorld;
    surfelRayResult.pdf      = pdfLocal;

    uint maxStep = isSleeping ? kDefaultMaxStep * 2u : kDefaultMaxStep;
    while (!((payload.status & 0x0002u) != 0u) && payload.currStep <= maxStep)
        traceScatterRay(payload);

    surfelRayResult.firstRayLength = payload.firstRayLength;
    surfelRayResult.radiance       = payload.radiance;

    gSurfelRayResultBuffer[rayIndex] = surfelRayResult;
}
