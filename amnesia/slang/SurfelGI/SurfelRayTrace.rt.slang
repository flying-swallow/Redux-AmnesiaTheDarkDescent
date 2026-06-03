// Surfel-GI scatter ray-trace pipeline.
//
// Ported from Falcor's SurfelGI/SurfelRayTrace.rt.slang to use the
// project's Scene module + bindless set. The Falcor PBR stack
// (ShadingData / IMaterialInstance / BSDFSample / LobeType /
// AnalyticLightSample / sampleLight / envMap.eval / prepareShadingData /
// getMaterialInstance) is replaced with a hand-rolled diffuse-only
// shading path: albedo = diffuse texture sample, BSDF = albedo / π,
// sampling = cosine-weighted hemisphere, NEE via gScene.sampleLightByIndex.
// No specular, no transmission, no env-map evaluation (miss returns
// zero). Surfel-radiance termination (`finalize`) keeps its original
// behavior. Inputs / outputs match the Falcor port — same SurfelRayResult
// layout, same gSurfel* lifecycle effects.

#include "../HostDefinitions.h"
#include "../Constants.h"

import SceneTypes;
import Scene;
import bindless;
import HitInfo;
import ILight;
import PerFrame.resource;
import SurfelGI.SurfelTypes;
import SurfelGI.SurfelUtils;
import Grid;                    // gridCellPos / gridFlattenCellIndex / gridCellValid (light grid lookup)
import Random;

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
// DiffuseMaterial. Replaces Falcor's `ShadingData` / `IMaterialInstance`
// pair for the diffuse-only case.
struct DiffuseShading
{
    float3 posW;
    float3 normalW;
    float3 albedo;
    float3 emission;
};

// `illuminationAmount` is the per-object glow scalar (UniformObject); emission =
// illumination-map texel (DiffuseMaterial slot 5) * that scalar. Objects with no
// illumination map (slot 5 invalid) or amount 0 emit nothing.
DiffuseShading prepareDiffuseShading(VertexData v, uint materialID, float illuminationAmount)
{
    DiffuseShading sd;
    sd.posW    = v.posW;
    sd.normalW = normalize(v.normalW);
    DiffuseMaterial m = gScene.getDiffuseMaterial(materialID);
    uint diffuseTex = m.tex[0];
    if (diffuseTex == kInvalidTextureIndex)
        sd.albedo = float3(0.5f);
    else
        sd.albedo = gTextures2D[NonUniformResourceIndex(diffuseTex)]
                        .SampleLevel(gMaterialSampler, v.texC, 0.0f).rgb;

    uint illumTex = m.tex[5];
    if (illumTex == kInvalidTextureIndex || illuminationAmount <= 0.f)
        sd.emission = float3(0.f);
    else
        sd.emission = gTextures2D[NonUniformResourceIndex(illumTex)]
                          .SampleLevel(gMaterialSampler, v.texC, 0.0f).rgb
                      * illuminationAmount;
    return sd;
}

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

// Single-light NEE. Picks a light uniformly, evaluates BSDF * Li / pdf.
// `rng` is the call-site RNG (used for light index + sample randomness);
// no payload mutation here.
float3 evalAnalyticLight(DiffuseShading sd, inout RNG rng)
{
    uint lightCount = gScene.getLightCount();
    if (lightCount == 0) return float3(0.f);

    // Importance-sample lights from the world-space light grid: pick uniformly
    // among only the lights whose radius reaches this shading point's cell
    // (pdf = 1/listLen). Falls back to uniform-over-all when the cell is
    // out-of-grid or empty — both unbiased, and the fallback stops a missed bin
    // from dimming a lit region. next_uint(N) covers [0,N) (the old `N-1` form
    // dropped the last light).
    uint  lightIdx;
    float invPdf;
    int3  lcell = gridCellPos(sd.posW, gScene.camera.getPosition(), kLightGridUnit);
    if (gridCellValid(lcell, kLightGridDim))
    {
        uint flat = gridFlattenCellIndex(lcell, kLightGridDim);
        uint L    = min(gLightGridCount[flat], kLightsPerCellMax);
        if (L > 0u)
        {
            uint pick = rng.next_uint(L);
            lightIdx  = gLightGridList[flat * kLightsPerCellMax + pick];
            invPdf    = float(L);
        }
        else
        {
            return float3(0, 0, 0);
        }
    }
    else
    {
        return float3(0, 0, 0);
    }

    // Type-aware shade shared with the direct path (gScene.shadeAnalyticLight:
    // sample → NdotL-cull → shadow ray → albedo·Li·NdotL). Point lights use their
    // full sampler (incl. gobo cube); spot lights use the same cone-restricted
    // SpotLight.sample() as the direct path — matching the reference Falcor
    // SurfelGI, where a spot's analytic GI contribution is cone-shaped too. Box
    // lights aren't NEE-sampled (and aren't binned into the grid).
    float3 origin   = sd.posW + sd.normalW * kRayEps;
    float3 perLight = gScene.shadeAnalyticLight(sd.posW, sd.normalW, sd.albedo,
                                                lightIdx, origin);

    // Lambert diffuse BRDF = albedo/π. kIndirectLightScale is a non-physical
    // boost that cheats the GI/indirect term brighter without touching direct
    // (which lives in MainCompositePass). π cancels the 1/π so indirect matches
    // the base-game no-1/π convention; raise above π to over-cheat.
    return perLight * invPdf * kIndirectLightScale;
}

// Try to terminate the path early by sampling the surfel cache instead of
// continuing to trace. Returns true when surfel radiance was accumulated;
// returns false when the cell is empty / over-budget and the caller should
// bounce one more step.
bool finalize(inout ScatterPayload scatterPayload, const VertexData v, const TriangleHit triangleHit, inout RNG rng)
{
    float4 Lr = float4(0.f);

    int3 cellPos = getCellPos(v.posW, gScene.camera.getPosition(), kCellUnit);
    if (!isCellValid(cellPos)) return false;

    uint flattenIndex = getFlattenCellIndex(cellPos);
    CellInfo cellInfo = gCellInfoBuffer[flattenIndex];

    if (cellInfo.surfelCount > 64u || rng.next_float() < 0.2f)
    {
        uint prev;
        InterlockedAdd(gSurfelCounter[kSurfelCounterMissBounce], 1u, prev);
        return false;
    }

    float sleepingCoverage = 0.f;
    float maxContribution = 0.f;
    int   maxContributionSleepingSurfelIndex = -1;

    for (uint i = 0; i < cellInfo.surfelCount; ++i)
    {
        uint surfelIndex = gCellToSurfelBuffer[cellInfo.cellToSurfelBufferOffset + i];
        Surfel surfel = gSurfelBuffer[surfelIndex];
        SurfelRecycleInfo info = gSurfelRecycleInfoBuffer[surfelIndex];
        bool isSleeping = (info.status & 0x0001u) != 0u;

        float3 bias = v.posW - surfel.position;
        float  dist2 = dot(bias, bias);
        if (dist2 >= surfel.radius * surfel.radius) continue;

        float3 normal = normalize(surfel.normal);
        float  dotN   = dot(v.normalW, normal);
        if (dotN <= 0.0f) continue;

        float dist = sqrt(dist2);
        float contribution = 1.f;
        contribution *= saturate(dotN);
        contribution *= saturate(1.0f - dist / surfel.radius);
        contribution = smoothstep(0.f, 1.f, contribution);

        Lr += float4(surfel.radiance, 1.f) * contribution;

        if (isSleeping)
        {
            sleepingCoverage += contribution;
            if (maxContribution < contribution)
            {
                maxContribution = contribution;
                maxContributionSleepingSurfelIndex = int(surfelIndex);
            }
        }

        uint prevRef;
        InterlockedAdd(gSurfelRefCounter[surfelIndex], 1u, prevRef);
    }

    if (Lr.w <= 0.f)
    {
        // Reserve a sleeping surfel in this cell when below the spawn cap.
        // Bail before any atomic counter touches if the committed hit's
        // instanceID is out of the bindless slot table — capturing
        // gBindlessSlotGeneration[triangleHit.instanceID] below would feed
        // collectCellInfo an anchor that GPUVM-faults next frame.
        if (cellInfo.surfelCount < 8u &&
            triangleHit.instanceID < kObjectSlotCapacity)
        {
            uint reservedCount;
            InterlockedAdd(gSurfelReservationBuffer[flattenIndex], 1u, reservedCount);

            if (reservedCount < 8u)
            {
                uint freeSurfelCount;
                InterlockedAdd(gSurfelCounter[kSurfelCounterFree], (uint)-1, freeSurfelCount);

                if (0u < freeSurfelCount && freeSurfelCount <= kTotalSurfelLimit)
                {
                    uint validSurfelCount;
                    InterlockedAdd(gSurfelCounter[kSurfelCounterValid], 1u, validSurfelCount);

                    if (validSurfelCount < kTotalSurfelLimit)
                    {
                        uint newIndex = gSurfelFreeIndexBuffer[freeSurfelCount - 1u];
                        Surfel newSurfel = Surfel(v.posW, v.normalW, 1e-6f);
                        gSurfelValidIndexBuffer[validSurfelCount] = newIndex;
                        gSurfelBuffer[newIndex] = newSurfel;
                        SurfelRecycleInfo recycle;
                        recycle.life   = 1;
                        recycle.frame  = 0;
                        recycle.status = 1; // sleeping
                        gSurfelRecycleInfoBuffer[newIndex] = recycle;
                        gSurfelGeometryBuffer[newIndex] = triangleHit.pack();
                        // Capture the anchor slot's current generation
                        // (instanceID bounded above) so collectCellInfo can
                        // invalidate this surfel if the slot is later
                        // reassigned to different geometry.
                        gSurfelSlotGeneration[newIndex] = gBindlessSlotGeneration[triangleHit.instanceID];
                        gSurfelRefCounter[newIndex] = 1u;
                    }
                }
            }
        }

        uint prev;
        InterlockedAdd(gSurfelCounter[kSurfelCounterMissBounce], 1u, prev);
        return false;
    }

    // Cull the loudest sleeping surfel when sleeping coverage is over-budget.
    if (sleepingCoverage >= 4.0f && maxContributionSleepingSurfelIndex != -1)
    {
        Surfel s = gSurfelBuffer[maxContributionSleepingSurfelIndex];
        s.radius = 0;
        gSurfelBuffer[maxContributionSleepingSurfelIndex] = s;
    }

    Lr.xyz /= Lr.w;
    scatterPayload.radiance += scatterPayload.thp * Lr.xyz;

    return true;
}

void handleHit(TriangleHit triangleHit, inout ScatterPayload scatterPayload)
{
    VertexData v = gScene.getVertexData(triangleHit);
    uint materialID = gScene.getMaterialID(triangleHit.instanceID);
    float illuminationAmount = gScene.getObject(triangleHit.instanceID).illuminationAmount;
    DiffuseShading sd = prepareDiffuseShading(v, materialID, illuminationAmount);

    if (scatterPayload.currStep == 1u)
        scatterPayload.firstRayLength = distance(scatterPayload.origin, v.posW);

    // Emission — emissive (illumination-map) surfaces add radiance directly into
    // the path, so they bounce into the surfel cache. Matches the reference's
    // `radiance += thp * emission`.
    scatterPayload.radiance += scatterPayload.thp * sd.emission;

    // Pull the RNG out of the payload, run NEE + bounce sample, push back.
    RNG rng;
    rng.s = uint2(scatterPayload.rngHi, scatterPayload.rngLo);

    // Direct lighting (one-sample NEE).
    float3 Lr = evalAnalyticLight(sd, rng);
    scatterPayload.radiance += scatterPayload.thp * Lr;

    // Sample the next bounce direction (cosine-hemisphere, local frame
    // around sd.normalW). BSDF * cosθ / pdf for cosine sampling reduces
    // to albedo (the cosθ / pdf factors cancel).
    float3 localDir;
    float  bsdfPdf;
    {
        float u1 = rng.next_float();
        float u2 = rng.next_float();
        localDir = sampleCosineHemisphere(u1, u2, bsdfPdf);
    }
    float3 worldDir = normalize(mul(localDir, buildTangentSpace(sd.normalW)));

    scatterPayload.origin    = sd.posW + sd.normalW * kRayEps;
    scatterPayload.direction = worldDir;
    scatterPayload.thp      *= sd.albedo;

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
    // Alpha test against the diffuse material's alpha texture.
    TriangleHit hit;
    hit.instanceID     = InstanceID();
    hit.primitiveIndex = PrimitiveIndex();
    hit.barycentrics   = attribs.barycentrics;
    VertexData v = gScene.getVertexData(hit);
    uint materialID = gScene.getMaterialID(hit.instanceID);
    if (gScene.materials.alphaTest(v, materialID, 0.f))
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

    // Uniform-hemisphere first ray (matches the reference rayGen, pdf = 1/(16π);
    // SurfelIntegratePass divides by this pdf). Ray-guiding path stays out
    // (USE_RAY_GUIDING is off by default).
    float3 dirLocal = normalize(hemispherepoint_uniform(rng.next_float(), rng.next_float()));
    float  pdfLocal = kInvPi / 16.f;
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
