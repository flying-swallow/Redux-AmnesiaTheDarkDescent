// Reference per-pixel path tracer — two-channel indirect (diffuse + specular).
//
// This is SurfelRayTrace.rt.slang with the surfel cache removed and the path
// re-rooted from surfels to screen pixels: no cache gather / finalize, no
// sleeping-surfel lifecycle, no gSurfel* buffers.
//
// BSDF: every scatter (primary and bounce) importance-samples the full
// Cook-Torrance GGX BSDF of Brdf.slang via sampleBrdf() — a diffuse lobe plus a
// GGX specular lobe, chosen stochastically. The returned weight is already
// f·N.L/pdf with the lobe-selection probability folded in, so nothing else
// (cosine, albedo, 1/pSpec) is applied on top. NEE weights the light's raw
// Li·NdotL with evalBrdf() for the same reason.
//
// TWO-CHANNEL SPLIT: the lobe picked at the PRIMARY hit routes the whole path
// into one of two outputs.
//   * gIndirectDiffuse is DEMODULATED: the primary throughput is seeded with
//     weight / albedo, so what lands there is indirect irradiance divided by
//     the primary surface's albedo — the quantity MainCompositePass re-
//     modulates with `sd.albedo * (direct + indirect)`.
//   * gIndirectSpecular is NOT demodulated: there is no single albedo to divide
//     out (F0 and roughness vary per texel), so it carries radiance directly.
// Bounce albedo (every vertex AFTER the primary hit) is inside the BSDF weight
// as usual — only the primary diffuse factor is divided out.
//
// There is no primary ray: the V-buffer already holds the primary hit, so
// rayGen recovers it the way MainCompositePass / DirectLightingPass do and
// traces only the scattered rays.

#include "../HostDefinitions.h"
#include "../Constants.h"

import SceneTypes;
import Scene;
import Material;             // load*Material / materialAlphaTest / traceShadowRay / traceReflectionHit
import bindless;             // gPackedHitInfo, gSceneObjects, gRtAccel, lights, grid
import Brdf;                 // SurfaceBRDF / prepareSurface / evalBrdf / sampleBrdf
import HitInfo;
import ILight;
import PerFrame.resource;
import Grid;                 // gridCellPos / gridFlattenCellIndex / gridCellValid (light grid lookup)
import Random;
import DiffuseShading;       // shared DiffuseShading + prepareDiffuseShading

// Pass-private (set 2). Names are resolved by reflection in
// RIProgram::bindDescriptors, so they must match the host side exactly.
[vk::binding(0, 2)] RWTexture2D<float4> gIndirectDiffuse;      // DEMODULATED indirect diffuse (.a = history count)
[vk::binding(1, 2)] RWTexture2D<float4> gIndirectKeyOut;       // surface key (viewZ, normal.xyz)
[vk::binding(2, 2)] Texture2D<uint4>    gPackedHitInfoRaster;  // raster V-buffer fallback
[vk::binding(3, 2)] RWTexture2D<float4> gIndirectSpecular;     // indirect specular, NOT demodulated
[vk::binding(4, 2)] RWTexture2D<float4> gIndirectKeyExtra;     // (.x = primary GGX alpha, yzw reserved)

// Plane-offset epsilon for the shadow / scatter ray origin.
static const float kRayEps = 1.0e-3f;

// [status]
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

    __init(uint2 rngState)
    {
        this.radiance = float3(0, 0, 0);
        this.thp = float3(1, 1, 1);
        this.origin = float3(0, 0, 0);
        this.direction = float3(0, 0, 0);
        this.firstRayLength = 0.f;
        this.currStep = 1u;
        this.status = 0x0000;
        this.rngHi = rngState.x;
        this.rngLo = rngState.y;
    }
}

// Shadow rays use inline RayQuery (no shadow-specific miss/anyhit entry
// points needed). RIProgram's RT pipeline layout is single-miss +
// single-hit-group, so keeping shadow off the SBT lets the whole
// pipeline build with one ray type.

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

void handleHit(TriangleHit triangleHit, inout ScatterPayload scatterPayload)
{
    VertexData v = getVertexData(triangleHit);
    uint materialID = gSceneObjects[triangleHit.instanceID].materialID;
    float illuminationAmount = gSceneObjects[triangleHit.instanceID].illuminationAmount;
    DiffuseShading sd = prepareDiffuseShading(v, materialID, illuminationAmount,
                                              /*decodeRoughness=*/true);
    // Full PBR surface for the BSDF (albedo / F0 / GGX alpha). prepareDiffuseShading
    // still supplies posW, normalW and emission.
    SurfaceBRDF s = prepareSurface(loadDiffuseMaterial(materialID), sd.normalW, v.texC);

    if (scatterPayload.currStep == 1u)
        scatterPayload.firstRayLength = distance(scatterPayload.origin, v.posW);

    // Emission — emissive (illumination-map) surfaces add radiance directly into
    // the path, so they bounce into the indirect term.
    scatterPayload.radiance += scatterPayload.thp * sd.emission;

    // Pull the RNG out of the payload, run NEE + bounce sample, push back.
    RNG rng;
    rng.s = uint2(scatterPayload.rngHi, scatterPayload.rngLo);

    // Direct lighting (one-sample NEE). scatterPayload.direction still holds
    // the incoming ray direction here (overwritten below for the next bounce),
    // so the outgoing/"view" direction ("V") at the hit is its negation; the
    // BSDF is view-dependent.
    //
    // Importance-sample lights from the world-space light grid: pick uniformly
    // among only the lights whose radius reaches this shading point's cell
    // (pdf = 1/count). Empty / out-of-grid cell → no contribution.
    CellLights cl = getCellLights(sd.posW);
    if (cl.count != 0u)
    {
        float3 V        = -scatterPayload.direction;
        uint   pick     = rng.next_uint(cl.count);
        uint   lightIdx = cellLightAt(cl, pick);
        float  invPdf   = float(cl.count);

        // Shared shadow-ray gate (shadeAnalyticLightRadiance: sample →
        // NdotL-cull → shadow ray → raw Li·NdotL, undemodulated and BRDF-free),
        // weighted here by the full GGX BSDF. Albedo is inside evalBrdf — this
        // is BOUNCE albedo; the primary hit's factor stays out of the diffuse
        // estimator (see file header).
        float3 origin = sd.posW + sd.normalW * kRayEps;
        float3 Ldir, LiNdotL;
        if (shadeAnalyticLightRadiance(sd.posW, sd.normalW, lightIdx, origin, Ldir, LiNdotL))
        {
            float3 perLight = evalBrdf(s, V, Ldir) * LiNdotL;
            scatterPayload.radiance += scatterPayload.thp * (perLight * invPdf);
        }
    }

    // Sample the next bounce from the full BSDF (diffuse or GGX lobe, picked
    // stochastically). `weight` is already f·N.L/pdf with the lobe-selection
    // probability folded in — no cosine / albedo / 1/pSpec on top. V is the
    // incoming ray negated, read BEFORE scatterPayload.direction is overwritten.
    float3 Vb = -scatterPayload.direction;
    float3 bounceL;
    float3 bounceWeight;
    bool   bounceSpecular;
    if (!sampleBrdf(s, Vb, rng, bounceL, bounceWeight, bounceSpecular))
    {
        scatterPayload.status |= 0x0002u;
        scatterPayload.rngHi = rng.s.x;
        scatterPayload.rngLo = rng.s.y;
        return;
    }

    scatterPayload.origin    = sd.posW + sd.normalW * kRayEps;
    scatterPayload.direction = bounceL;
    scatterPayload.thp      *= bounceWeight;

    // Russian roulette on luminance(thp), same as the surfel path tracer. With
    // no cache to finalize into, the path just keeps going until the bounce
    // budget (kPathTraceMaxBounces, enforced by the rayGen loop) or RR kills it.
    uint rayStep = kPathTraceMaxBounces;
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

    scatterPayload.currStep++;

    scatterPayload.rngHi = rng.s.x;
    scatterPayload.rngLo = rng.s.y;
}

// Miss / hit shaders.

[shader("miss")]
void ptMiss(inout ScatterPayload scatterPayload)
{
    // Environment map evaluator isn't ported — treat the miss as black
    // radiance and terminate. When an env map binding gets added, this is
    // where the lookup goes.
    scatterPayload.status |= 0x0002u;
}

[shader("anyhit")]
void ptAnyHit(inout ScatterPayload scatterPayload, BuiltInTriangleIntersectionAttributes attribs)
{
    // Alpha test (+ CoverageAmount dissolve) against the diffuse material —
    // the single shared gate, so fading objects also fade out of the GI.
    TriangleHit hit;
    hit.instanceID     = InstanceID();
    hit.primitiveIndex = PrimitiveIndex();
    hit.barycentrics   = attribs.barycentrics;
    VertexData v = getVertexData(hit);
    uint materialID = gSceneObjects[hit.instanceID].materialID;
    if (materialAlphaTest(v, materialID, 0.f,
                          gSceneObjects[hit.instanceID].dissolveAmount))
        IgnoreHit();
}

[shader("closesthit")]
void ptCloseHit(inout ScatterPayload scatterPayload, BuiltInTriangleIntersectionAttributes attribs)
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
    uint outW, outH;
    gIndirectDiffuse.GetDimensions(outW, outH);
    uint2 pixel = DispatchRaysIndex().xy;
    if (pixel.x >= outW || pixel.y >= outH)
        return;

    // Primary hit comes from the V-buffer (RT hit info, raster V-buffer as the
    // fallback for rasterized/alpha geometry) — same recovery as the composite
    // and direct passes. No primary ray is traced here.
    HitInfo hitInfo = HitInfo(gPackedHitInfo[pixel]);
    if (!hitInfo.isValid())
        hitInfo = HitInfo(gPackedHitInfoRaster.Load(int3(pixel, 0)));
    if (!hitInfo.isValid())
    {
        gIndirectDiffuse[pixel]  = float4(0.f, 0.f, 0.f, 0.f);
        gIndirectSpecular[pixel] = float4(0.f, 0.f, 0.f, 0.f);
        gIndirectKeyOut[pixel]   = float4(0.f, 0.f, 0.f, 0.f); // zero normal → rejected
        gIndirectKeyExtra[pixel] = float4(0.f, 0.f, 0.f, 0.f);
        return;
    }

    TriangleHit th = hitInfo.getTriangleHit();
    VertexData v = getVertexData(th);
    UniformObject obj = gSceneObjects[th.instanceID];
    DiffuseShading sd = prepareDiffuseShading(v, obj.materialID, obj.illuminationAmount,
                                              /*decodeRoughness=*/true);
    SurfaceBRDF s = prepareSurface(loadDiffuseMaterial(obj.materialID), sd.normalW, v.texC);

    RNG rng;
    rng.init(pixel, gPerFrame.totalFrames);

    float3 V = normalize(gPerFrame.posW - sd.posW);

    float3 diffuseSum  = float3(0.f);
    float3 specularSum = float3(0.f);
    for (uint sampleIdx = 0u; sampleIdx < kPathTraceSamplesPerPixel; ++sampleIdx)
    {
        // Importance-sample the primary BSDF; the lobe that comes back decides
        // which channel this whole path lands in.
        float3 L;
        float3 weight;
        bool   sampledSpecular;
        if (!sampleBrdf(s, V, rng, L, weight, sampledSpecular))
            continue;   // below the horizon / degenerate — nothing to trace

        ScatterPayload payload = ScatterPayload(rng.s);
        payload.origin    = sd.posW + sd.normalW * kRayEps;
        payload.direction = L;
        // Diffuse is demodulated: dividing the primary albedo out of the seed
        // throughput is what upholds the contract the composite's `sd.albedo *`
        // restores. Specular keeps its weight as-is — no single albedo exists to
        // divide by, and F0/roughness vary per texel.
        payload.thp = sampledSpecular ? weight
                                      : weight / max(sd.albedo, float3(1e-4f));

        while (!((payload.status & 0x0002u) != 0u) && payload.currStep <= kPathTraceMaxBounces)
            traceScatterRay(payload);

        if (sampledSpecular) specularSum += payload.radiance;
        else                 diffuseSum  += payload.radiance;

        // Carry the path's RNG state back so the next sample is decorrelated.
        rng.s = uint2(payload.rngHi, payload.rngLo);
    }

    // .a is the history / confidence count consumed by the temporal pass and
    // DirectAtrousPass — a freshly traced pixel carries exactly one sample.
    //
    // With kPathTraceSamplesPerPixel == 1 the lobe choice is a coin flip, so
    // exactly ONE of these two sums is non-zero for a given pixel/frame and the
    // other writes black. That is expected, not a bug: the temporal accumulator
    // resolves both channels over frames.
    gIndirectDiffuse[pixel]  = float4(diffuseSum  / float(kPathTraceSamplesPerPixel), 1.0f);
    gIndirectSpecular[pixel] = float4(specularSum / float(kPathTraceSamplesPerPixel), 1.0f);

    // Surface key in the SAME encoding DirectLightingPass writes to gDirectKeyOut
    // (viewZ, geometric normal) — DirectAtrousPass hard-codes that layout and its
    // kSvgfSigmaZ scaling assumes this viewZ.
    float  curZ = -mul(gPerFrame.viewMat, float4(sd.posW, 1.0f)).z;
    float3 curN = normalize(v.normalW);
    gIndirectKeyOut[pixel] = float4(curZ, curN);

    // Side-channel for a future roughness-aware denoiser: the primary hit's GGX
    // alpha. Kept OUT of gIndirectKeyOut because DirectAtrousPass is shared with
    // the direct chain and hard-codes that (viewZ, normal.xyz) layout.
    gIndirectKeyExtra[pixel] = float4(s.rough, 0.f, 0.f, 0.f);
}
