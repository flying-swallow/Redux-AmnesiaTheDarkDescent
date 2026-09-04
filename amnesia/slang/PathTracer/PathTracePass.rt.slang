// Reference per-pixel path tracer — two-channel output (diffuse + specular).
//
// This pass outputs INDIRECT diffuse only (albedo-demodulated). The ReSTIR DI
// chain (DirectLightingPass -> DirectSpatialReusePass) still runs BEFORE this
// pass; NrdPack sums its resolved direct term into the diffuse channel just
// before packing, so one REBLUR instance denoises direct and indirect together.
// The sum is kept out of this shader so the tracer does not depend on the
// direct chain's output layout.
// Primary-hit emission is still the composite's job (see rayGen).
//
// History: derived from the since-deleted SurfelRayTrace.rt.slang when the
// surfel GI cache was retired. The path is rooted at screen pixels instead of
// surfels, and nothing of the cache survives — no gather / finalize, no
// lifecycle, no gSurfel* buffers.
//
// BSDF: every scatter (primary and bounce) importance-samples the full
// Cook-Torrance GGX BSDF of Brdf.slang via sampleBrdfLobe() — a diffuse lobe
// plus a GGX specular lobe. The PRIMARY lobe is picked by a screen-space Bayer
// dither with a clamped probability; bounces keep the 0.05/0.95 white-noise
// pick through sampleBrdf(). The returned weight is already
// f·N.L/pdf with the lobe-selection probability folded in, so nothing else
// (cosine, albedo, 1/pSpec) is applied on top. NEE weights the light's raw
// Li·NdotL with evalBrdf() for the same reason.
//
// TWO-CHANNEL SPLIT: the lobe picked at the PRIMARY hit routes the whole path
// into one of two outputs.
//   * gIndirectDiffuse is DEMODULATED: the primary throughput is seeded with
//     weight / albedo, so what lands there is indirect irradiance divided by
//     the primary surface's albedo — INDIRECT only. MainCompositePass fetches
//     the ReSTIR DI chain's demodulated direct irradiance separately and
//     re-modulates the two together with a single `sd.albedo *`.
//   * gIndirectSpecular is NOT demodulated: there is no single albedo to divide
//     out (F0 and roughness vary per texel), so it carries radiance directly.
// Bounce albedo (every vertex AFTER the primary hit) is inside the BSDF weight
// as usual — only the primary diffuse factor is divided out.
//
// There is no primary ray: the V-buffer already holds the primary hit — from
// the raster pass, POM-corrected — so rayGen recovers it the way
// MainCompositePass / DirectLightingPass do and traces only the scattered rays.

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
// Slot 2 is a gap; the remaining bindings keep their numbers so the host-side
// layout stays stable.
[vk::binding(3, 2)] RWTexture2D<float4> gIndirectSpecular;     // indirect specular, NOT demodulated
[vk::binding(4, 2)] RWTexture2D<float4> gIndirectKeyExtra;     // .x = primary GGX alpha; .y = diffuse primary hit distance (m); .z = specular primary hit distance (m); .w reserved
// Slot 5 is now free: the denoised ReSTIR DI result is consumed by MainCompositePass, not here.

// Plane-offset epsilon for the shadow / scatter ray origin.
static const float kRayEps = 1.0e-3f;

// AREA_3X3 requires the primary lobe probability to be clamped to [1/4; 3/4].
// The dither takes 4 distinct values per 2x2 tile, so this guarantees that any
// 2x2 block — and therefore any 3x3 window — contains at least one pixel that
// picked specular and at least one that picked diffuse, exactly what NRD's
// hit-distance reconstruction needs. The +frameIndex rotation is the "global
// temporal jitter" requested by NRD's README; it is a uniform permutation of
// the tile, so it preserves the guarantee. This replaces white noise for the
// primary lobe pick ONLY.
static const float kPrimaryLobeProbMin = 0.25f;

// Ordered 2x2 Bayer rank in [0;3] with a global temporal rotation.
uint bayer2x2ui(uint2 samplePos, uint frameIndex)
{
    uint2 p = samplePos & 1u;
    uint  b = (p.y << 1u) | (p.y ^ p.x);
    return (b + frameIndex) & 0x3u;
}

float bayer2x2(uint2 samplePos, uint frameIndex)
{
    return (float(bayer2x2ui(samplePos, frameIndex)) + 0.5f) * 0.25f;
}

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
                                              /*decodeRoughness=*/true,
                                              /*applyNormalMap=*/true);
    // Full PBR surface for the BSDF (albedo / F0 / GGX alpha). prepareDiffuseShading
    // still supplies posW, the normal-mapped normalW (the BSDF shades with the
    // bumped normal, same as the direct pass) and emission.
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

    // Russian roulette on luminance(thp). There is nothing to finalize into, so
    // the path just keeps going until the bounce budget (kPathTraceMaxBounces,
    // enforced by the rayGen loop) or RR kills it.
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

    // Primary hit comes from the V-buffer (rasterized primary visibility with
    // POM-corrected barycentrics) — same as the composite and direct passes.
    // No primary ray is traced here. `gPackedHitInfo` already carries the
    // raster hit, so a miss here is a real miss.
    HitInfo hitInfo = HitInfo(gPackedHitInfo[pixel]);
    if (!hitInfo.isValid())
    {
        gIndirectDiffuse[pixel]  = float4(0.f, 0.f, 0.f, 0.f);
        gIndirectSpecular[pixel] = float4(0.f, 0.f, 0.f, 0.f);
        gIndirectKeyOut[pixel]   = float4(0.f, 0.f, 0.f, 0.f); // zero normal → rejected
        // NRD reserves a raw hit distance of 0 for no hit / sky.
        gIndirectKeyExtra[pixel] = float4(0.f, 0.f, 0.f, 0.f);
        return;
    }

    TriangleHit th = hitInfo.getTriangleHit();
    VertexData v = getVertexData(th);
    UniformObject obj = gSceneObjects[th.instanceID];
    DiffuseShading sd = prepareDiffuseShading(v, obj.materialID, obj.illuminationAmount,
                                              /*decodeRoughness=*/true,
                                              /*applyNormalMap=*/true);
    // Bumped normal for the BSDF; the reprojection key below deliberately keeps
    // the GEOMETRIC normal (same convention as DirectLightingPass).
    SurfaceBRDF s = prepareSurface(loadDiffuseMaterial(obj.materialID), sd.normalW, v.texC);

    RNG rng;
    rng.init(pixel, gPerFrame.totalFrames);

    float3 V = normalize(gPerFrame.posW - sd.posW);
    float pSpecPrimary = brdfSpecularProbability(s, V, kPrimaryLobeProbMin);

    float3 diffuseSum  = float3(0.f);
    float3 specularSum = float3(0.f);
    float  diffuseHitDistance  = 0.f;
    float  specularHitDistance = 0.f;
    for (uint sampleIdx = 0u; sampleIdx < kPathTraceSamplesPerPixel; ++sampleIdx)
    {
        // Pick the primary lobe with screen-space Bayer dithering. Adding
        // sampleIdx keeps the strata distinct if kPathTraceSamplesPerPixel is
        // ever raised above 1.
        float lobeU = bayer2x2(pixel, gPerFrame.totalFrames + sampleIdx);

        // Importance-sample the primary BSDF; the lobe that comes back decides
        // which channel this whole path lands in.
        float3 L;
        float3 weight;
        bool   sampledSpecular;
        if (!sampleBrdfLobe(s, V, pSpecPrimary, lobeU, rng, L, weight, sampledSpecular))
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

        // The traced ray IS the sampled lobe's ray, so its first-bounce distance
        // belongs to that lobe. The other lobe stays at zero this frame because
        // it was not sampled: this is NRD's "skipped lobe" encoding, which is
        // why the host enables HitDistanceReconstructionMode.
        if (sampledSpecular)
            specularHitDistance = payload.firstRayLength;
        else
            diffuseHitDistance = payload.firstRayLength;

        // The primary lobe probability floor raises the worst-case selection
        // boost from at most 20x (old 0.05 floor) to at most 4x (0.25 floor),
        // so fewer samples cross this threshold and the clamp fires less often;
        // this clamp never touches hit distance.
        // Firefly clamp: cap this sample's luminance at gPerFrame.fireflyClampThreshold
        // by scaling the whole float3, so hue survives where a per-channel clamp would
        // shift it. Note what is capped is DEMODULATED irradiance (diffuse has the
        // primary albedo divided out above), not final radiance. Threshold <= 0 disables.
        float3 sampleRadiance = payload.radiance;
        float  sampleLum      = brdfLuminance(sampleRadiance);
        if (gPerFrame.fireflyClampThreshold > 0.f && sampleLum > gPerFrame.fireflyClampThreshold)
            sampleRadiance *= gPerFrame.fireflyClampThreshold / sampleLum;

        if (sampledSpecular) specularSum += sampleRadiance;
        else                 diffuseSum  += sampleRadiance;

        // Carry the path's RNG state back so the next sample is decorrelated.
        rng.s = uint2(payload.rngHi, payload.rngLo);
    }

    // .a is unused downstream — NrdPack reads rgb only — and is written as 1
    // to keep the RGBA16F layout meaningful.
    //
    // With kPathTraceSamplesPerPixel == 1 the primary lobe is picked by a
    // screen-space Bayer dither with a clamped probability, so exactly ONE of
    // these two sums is non-zero for a given pixel/frame and the other writes
    // black. That is expected, not a bug: REBLUR's hit-distance reconstruction
    // and temporal accumulation resolve both channels over frames. Bounce lobe choices remain the stochastic white-noise
    // picks made by sampleBrdf().
    //
    // NOTE: primary-hit emission deliberately stays in MainCompositePass. This
    // output is denoised downstream by REBLUR; an illumination-map's sharp
    // emissive texels would be blurred across the surface if they went through
    // it.
    gIndirectDiffuse[pixel]  = float4(diffuseSum  / float(kPathTraceSamplesPerPixel), 1.0f);
    gIndirectSpecular[pixel] = float4(specularSum / float(kPathTraceSamplesPerPixel), 1.0f);

    // Surface key in the SAME encoding DirectLightingPass writes to gDirectKeyOut
    // (viewZ, geometric normal). NrdPack reads it to build IN_VIEWZ and
    // IN_NORMAL_ROUGHNESS.
    float  curZ = -mul(gPerFrame.viewMat, float4(sd.posW, 1.0f)).z;
    float3 curN = normalize(v.normalW);
    gIndirectKeyOut[pixel] = float4(curZ, curN);

    // Side-channel consumed by NrdPack.cs.slang: the primary hit's GGX
    // alpha plus per-lobe primary-bounce hit distances (.y diffuse, .z
    // specular). The traced ray belongs to whichever lobe was sampled, so the
    // other channel remains 0 for this frame — NRD's "skipped lobe" encoding.
    // The host enables HitDistanceReconstructionMode for this probabilistic
    // lobe-selection convention. Kept OUT of gIndirectKeyOut so that texture
    // keeps the same (viewZ, normal.xyz) layout the direct chain's key uses.
    gIndirectKeyExtra[pixel] =
        float4(s.rough, diffuseHitDistance, specularHitDistance, 0.f);
}
