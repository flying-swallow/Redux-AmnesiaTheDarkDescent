// Hand-written Metal port of amnesia/slang/DirectLightingPass/DirectSpatialReusePass.cs.slang
//
// Direct-lighting pass — stage 2 of 3 (ReSTIR DI spatial reuse + resolve),
// numthreads 16,16,1. Per pixel: reconstruct the surface, take the
// temporally-merged reservoir from DirectLightingPass, merge a few same-surface
// neighbours' reservoirs (geometry-rejected on the depth/normal key), then
// RESOLVE: one soft (blue-noise disk) shadow ray for the chosen light, weighted
// by the reservoir UCW -> demodulated irradiance. The merged reservoir is also
// the next frame's temporal history. Finally temporally-accumulate the resolved
// irradiance (reproject through gVelocity, EMA-blend by history length).
//
// The DXR shadow ray is authored against Metal's inline ray-query model (one
// intersection_query over an instance_acceleration_structure; first non-alpha-
// tested opaque candidate == occluded), exactly like SurfelRayTrace.rt.metal's
// traceShadowRay. The TLAS rides in Set1 at id(33) (kBindingTlas / mtl.index 33).
//
// Self-contained; scalar struct layouts (packed types). Set-0 bindless globals
// come through one Set0 argument buffer (buffer(0), [[id]] = kBinding*); the
// program-managed set 1 / set 2 [[id]] values are the mtl.index from
// kDirectSpatialCs in HybridRenderer.cpp.
//
// Bindings:
//   Set0 buffer(0): gOpaque*Handles 3/4/5/6/8  gMaterialSampler 9
//     gSceneObjects 20  gDiffuseMaterials 21  gPointLights 22  gSpotLights 29
//     gSurfelDepthSampler 35  gLightGridCount 40  gLightGridList 41
//     gAttenuationLut 45  gDissolveMap 48  gTextures2D id(49)  gTexturesCube id(16433)
//   Set1 buffer(1): gPerFrame id(34)  gPackedHitInfo id(8)[uint,read]  tlas id(33)
//   Set2 buffer(2): gPackedHitInfoRaster id(0)[uint,read]  gReservoirIn id(1)[float4,read]
//     gDirectKey id(2)[float4,read]  gReservoirOut id(3)[rw]  gDirectLighting id(4)[rw]
//     gVelocity id(5)[float2,read]  gDirectHistory id(6)[float4,sample]  gDirectKeyHistory id(7)[float4,read]
#include <metal_stdlib>
#include <metal_raytracing>
using namespace metal;
using namespace raytracing;

constant uint  kInvalidTextureIndex = 0xffffffffu;
constant float kDefaultDiffuseRoughness = 1.0f;
constant float kPointLightSourceRadiusSq = 0.25f;
constant uint  kSolidMaterialCapacity = 2048u;
constant uint  kMaterialFlagEnableSpecular        = 1u << 2;
constant uint  kMaterialFlagEnableCubeMap         = 1u << 6;
constant uint  kMaterialFlagEnableDissolveAlpha   = 1u << 7;
constant uint  kMaterialFlagIsAlphaSingleChannel  = 1u << 10;
constant uint  kMaterialFlagUseDissolveFilter     = 1u << 14;
// Light-grid geometry (Constants.h).
constant uint  kLightGridDim     = 128u;
constant float kLightGridUnit    = 0.9765625f;
constant uint  kLightsPerCellMax = 128u;
constant float kCellUnit = 0.5f;
constant float kShadeRayEps = 1.0e-3f;
// ReSTIR DI + temporal-accum tuning (Constants.h).
constant float kReprojZRelTol     = 0.05f;
constant float kReprojNormalCos   = 0.9f;
constant int   kSpatialSamples    = 4;
constant float kSpatialRadius     = 16.0f;
constant float kDirectTemporalAlpha = 0.05f;
constant float kDirectMaxAccum      = 32.0f;

constant float3 kReservoirLum = float3(0.2126f, 0.7152f, 0.0722f);

// --- Shared struct layouts + utilities (flattened in at stage time) ----------
#include "SceneTypes.metalh"            // SceneConstants, MatStorage, UniformObject, HitInfo, TriangleHit, VertexData, lights
#include "Random.metalh"                // RNG + rng_*
#include "Reservoir.metalh"             // Reservoir + init/merge/finalizeCombine + pack/unpack
#include "Grid.metalh"                  // gridCellValid

// RT-unique structs the shared headers need.
struct DiffuseMaterial { uint type; uint materialConfig; uint cubeMapTextureIndex; uint tex[8]; float heightMapScale, heightMapBias, frenselBias, frenselPow; };
struct LightSample { float3 Li; float pdf; float3 dir; float distance; };

// light-grid cell math (mirrors Grid.slang gridCellPos / gridFlattenCellIndex).
static int3 gridCellPos(float3 p, float3 c, float gu) { return int3(round((p - c) / gu)); }
static uint gridFlattenCellIndex(int3 c, uint dim) { uint3 u = uint3(c + int3(int(dim / 2u))); return u.z * dim * dim + u.y * dim + u.x; }

struct Set0 {
    device ulong*           gOpaquePositionHandles [[id(3)]];
    device ulong*           gOpaqueTangentHandles  [[id(4)]];
    device ulong*           gOpaqueNormalHandles   [[id(5)]];
    device ulong*           gOpaqueUv0Handles      [[id(6)]];
    device ulong*           gOpaqueIndexHandles    [[id(8)]];
    sampler                 gMaterialSampler       [[id(9)]];
    device UniformObject*   gSceneObjects          [[id(20)]];
    device DiffuseMaterial* gDiffuseMaterials      [[id(21)]];
    device PointLight*      gPointLights           [[id(22)]];
    device SpotLight*       gSpotLights            [[id(29)]];
    sampler                 gSurfelDepthSampler    [[id(35)]];
    device uint*            gLightGridCount        [[id(40)]];
    device uint*            gLightGridList         [[id(41)]];
    texture2d<float>        gAttenuationLut        [[id(45)]];
    texture2d<float>        gDissolveMap           [[id(48)]];
    array<texture2d<float>, 16384>   gTextures2D   [[id(49)]];     // kBindingTextures2D (remapped)
    array<texturecube<float>, 16384> gTexturesCube [[id(16433)]];  // kBindingTexturesCube (remapped)
};

// Set-1 / Set-2 program argument buffers; [[id]] = mtl.index (HybridRenderer.cpp).
// Metal argument-buffer members must be declared in ascending [[id]] order.
struct Set1 {
    texture2d<uint, access::read> gPackedHitInfo [[id(8)]];
    instance_acceleration_structure tlas    [[id(33)]];   // kBindingTlas (mtl.index 33)
    constant SceneConstants* gPerFrame      [[id(34)]];
};
struct Set2 {
    texture2d<uint,  access::read>       gPackedHitInfoRaster [[id(0)]];
    texture2d<float, access::read>       gReservoirIn         [[id(1)]];
    texture2d<float, access::read>       gDirectKey           [[id(2)]];
    texture2d<float, access::read_write> gReservoirOut        [[id(3)]];
    texture2d<float, access::read_write> gDirectLighting      [[id(4)]];
    texture2d<float, access::read>       gVelocity            [[id(5)]];
    texture2d<float, access::sample>     gDirectHistory       [[id(6)]];
    texture2d<float, access::read>       gDirectKeyHistory    [[id(7)]];
};

// Utility headers that reference `device Set0&` must come AFTER struct Set0.
#include "SurfelGI/BindlessTriangle.metalh"  // loadMat, getVertexData, getTriangleHit, hitIsValid
#include "Brdf.metalh"                        // decodeMaterialRoughness, evalDiffuseDemodulated
#include "DiffuseShading.metalh"              // DiffuseShading + prepareDiffuseShading + concentricDiskSample
#include "Light.metalh"                       // sampleLight / samplePointLight / sampleSpotLight

// --- alpha test (Scene.slang materials.alphaTest; lifted from SurfelRayTrace) -
// true == fragment is transparent (ignore the hit).
static bool alphaTest(VertexData v, uint materialID, float lod, float dissolveAmount, device Set0& set0) {
    if (materialID >= kSolidMaterialCapacity) return false;
    DiffuseMaterial m = set0.gDiffuseMaterials[materialID];
    float a = 1.0f; uint alphaIdx = m.tex[2];
    if (alphaIdx != kInvalidTextureIndex) {
        float4 s = set0.gTextures2D[alphaIdx].sample(set0.gMaterialSampler, v.texC, level(lod));
        a = ((m.materialConfig & kMaterialFlagIsAlphaSingleChannel) != 0u) ? s.r : s.a;
    } else if (dissolveAmount >= 1.0f &&
               (m.materialConfig & (kMaterialFlagEnableDissolveAlpha | kMaterialFlagUseDissolveFilter)) == 0u) {
        return false;
    }
    bool hasDissolveAlpha  = (m.materialConfig & kMaterialFlagEnableDissolveAlpha) != 0u;
    bool hasDissolveFilter = (m.materialConfig & kMaterialFlagUseDissolveFilter)  != 0u;
    if (dissolveAmount < 1.0f || hasDissolveAlpha || hasDissolveFilter) {
        float d = set0.gDissolveMap.sample(set0.gMaterialSampler, v.texC, level(0.0f)).x;
        if (hasDissolveAlpha) {
            d = d * 0.25f + 0.75f; uint dat = m.tex[6]; float da = 1.0f;
            if (dat != kInvalidTextureIndex) da = set0.gTextures2D[dat].sample(set0.gMaterialSampler, v.texC, level(lod)).r;
            d -= (0.25f - da * 0.25f);
        } else { d = d * 0.5f + 0.5f; }
        a = d - (1.0f - dissolveAmount * a) * 0.5f;
    }
    return a < 0.5f;
}

// --- shadow ray (Scene.slang materials.traceShadowRay; inline ray-query, same
// model as SurfelRayTrace.rt.metal). Returns true == visible (unoccluded). -----
static bool traceShadowRay(float3 origin, float3 dir, float dist,
                           instance_acceleration_structure tlas, device Set0& set0) {
    ray r; r.origin = origin; r.direction = dir; r.min_distance = 0.0f; r.max_distance = dist;
    intersection_query<triangle_data, instancing> q;
    q.reset(r, tlas, 0xFFu);
    while (q.next()) {
        if (q.get_candidate_intersection_type() == intersection_type::triangle) {
            TriangleHit h;
            h.instanceID = q.get_candidate_instance_id();
            h.primitiveIndex = q.get_candidate_primitive_id();
            h.barycentrics = q.get_candidate_triangle_barycentric_coord();
            VertexData v = getVertexData(h, set0);
            uint mat = set0.gSceneObjects[h.instanceID].materialID;
            float dissolve = set0.gSceneObjects[h.instanceID].dissolveAmount;
            if (!alphaTest(v, mat, 0.0f, dissolve, set0)) return false;
        }
    }
    return true;
}

// --- cell-light resolution (Scene.slang getCellLights / cellLightAt) ----------
struct CellLights { uint flat; uint count; bool valid; };
static CellLights getCellLights(float3 posW, device Set0& set0, constant SceneConstants* gPerFrame) {
    CellLights c;
    uint lightCount = gPerFrame->pointLightCount + gPerFrame->spotLightCount;
    int3 cell = gridCellPos(posW, gPerFrame->posW, kLightGridUnit);
    c.valid = (lightCount > 0u) && gridCellValid(cell, kLightGridDim);
    if (!c.valid) { c.flat = 0u; c.count = 0u; return c; }
    c.flat  = gridFlattenCellIndex(cell, kLightGridDim);
    c.count = min(set0.gLightGridCount[c.flat], kLightsPerCellMax);
    return c;
}

// per-light unshadowed demodulated contribution (no shadow ray).
static float3 unshadowedAnalyticLight(float3 posW, float3 normalW, float3 V, float rough,
                                      uint lightIndex, device Set0& set0, constant SceneConstants* gPerFrame) {
    LightSample ls;
    if (!sampleLight(lightIndex, posW, ls, set0, gPerFrame)) return float3(0.0f);
    float NdotL = dot(normalW, ls.dir);
    if (NdotL <= 0.0f) return float3(0.0f);
    return ls.Li * NdotL * evalDiffuseDemodulated(normalW, V, ls.dir, rough);
}

static float directTargetPdf(DiffuseShading sd, uint lightIndex, device Set0& set0, constant SceneConstants* gPerFrame) {
    float3 V = normalize(gPerFrame->posW - sd.posW);
    return dot(unshadowedAnalyticLight(sd.posW, sd.normalW, V, sd.rough, lightIndex, set0, gPerFrame), kReservoirLum);
}

// Soft-shadow per-light shade (DiffuseShading.slang shadeAnalyticLightSoft):
// sample + back-face cull + demodulated contribution, then one jittered-disk
// shadow ray (or the hard centre ray when sourceRadius == 0). Energy uses the
// light centre; only occlusion is softened.
static float3 shadeAnalyticLightSoft(float3 posW, float3 normalW, float3 V, float rough,
                                     uint lightIndex, float3 origin, thread RNG& rng,
                                     instance_acceleration_structure tlas,
                                     device Set0& set0, constant SceneConstants* gPerFrame) {
    LightSample ls;
    if (!sampleLight(lightIndex, posW, ls, set0, gPerFrame)) return float3(0.0f);
    float NdotL = dot(normalW, ls.dir);
    if (NdotL <= 0.0f) return float3(0.0f);
    float3 contrib = ls.Li * NdotL * evalDiffuseDemodulated(normalW, V, ls.dir, rough);

    // Source radius + light centre via the typed SSBO (getLight() index slicing).
    uint  nPoint = gPerFrame->pointLightCount;
    float r;
    float3 lpos;
    if (lightIndex < nPoint) {
        r    = set0.gPointLights[lightIndex].sourceRadius;
        lpos = float3(set0.gPointLights[lightIndex].position);
    } else {
        r    = set0.gSpotLights[lightIndex - nPoint].sourceRadius;
        lpos = float3(set0.gSpotLights[lightIndex - nPoint].position);
    }

    float3 sDir = ls.dir;
    float  sMax = ls.distance;
    if (r > 0.0f) {
        float3 t = normalize(abs(ls.dir.y) < 0.99f
                             ? cross(float3(0.0f, 1.0f, 0.0f), ls.dir)
                             : cross(float3(1.0f, 0.0f, 0.0f), ls.dir));
        float3 b = cross(ls.dir, t);
        // Sequence the two RNG draws explicitly: MSL leaves argument evaluation
        // order unspecified, but Slang's next_float2 is (x = first draw, y = second).
        float du0 = rng_next_float(rng);
        float du1 = rng_next_float(rng);
        float2 d = concentricDiskSample(float2(du0, du1)) * r;
        float3 target = lpos + d.x * t + d.y * b;
        float3 toT = target - origin;
        sMax = length(toT);
        sDir = toT / max(sMax, 1.0e-6f);
    }

    if (!traceShadowRay(origin, sDir, sMax, tlas, set0)) return float3(0.0f);
    return contrib;
}

// Resolve a reservoir to demodulated irradiance: one soft shadow ray, UCW-weighted.
static float3 resolveDirectReservoir(DiffuseShading sd, Reservoir r, thread RNG& rng,
                                     instance_acceleration_structure tlas,
                                     device Set0& set0, constant SceneConstants* gPerFrame) {
    if (reservoirIsEmpty(r)) return float3(0.0f);
    float3 V = normalize(gPerFrame->posW - sd.posW);
    float3 origin = sd.posW + sd.normalW * kShadeRayEps;
    float3 shaded = shadeAnalyticLightSoft(sd.posW, sd.normalW, V, sd.rough, r.y, origin, rng, tlas, set0, gPerFrame);
    return shaded * r.W;
}

[[kernel]] void csMain(uint3 tid [[thread_position_in_grid]],
                       device Set0& set0 [[buffer(0)]],
                       device Set1& set1 [[buffer(1)]],
                       device Set2& set2 [[buffer(2)]]) {
    constant SceneConstants* gPerFrame = set1.gPerFrame;
    texture2d<uint, access::read> gPackedHitInfo = set1.gPackedHitInfo;
    instance_acceleration_structure tlas = set1.tlas;
    texture2d<uint,  access::read>       gPackedHitInfoRaster = set2.gPackedHitInfoRaster;
    texture2d<float, access::read>       gReservoirIn         = set2.gReservoirIn;
    texture2d<float, access::read>       gDirectKey           = set2.gDirectKey;
    texture2d<float, access::read_write> gReservoirOut        = set2.gReservoirOut;
    texture2d<float, access::read_write> gDirectLighting      = set2.gDirectLighting;
    texture2d<float, access::read>       gVelocity            = set2.gVelocity;
    texture2d<float, access::sample>     gDirectHistory       = set2.gDirectHistory;
    texture2d<float, access::read>       gDirectKeyHistory    = set2.gDirectKeyHistory;

    uint outW = gDirectLighting.get_width();
    uint outH = gDirectLighting.get_height();
    int2 p = int2(tid.xy);
    if (p.x >= int(outW) || p.y >= int(outH))
        return;

    HitInfo hitInfo;
    hitInfo.packed = gPackedHitInfo.read(uint2(p));
    if (!hitIsValid(hitInfo))
        hitInfo.packed = gPackedHitInfoRaster.read(uint2(p));
    if (!hitIsValid(hitInfo)) {
        gReservoirOut.write(float4(0.0f), uint2(p));
        gDirectLighting.write(float4(0.0f), uint2(p));
        return;
    }

    TriangleHit th  = getTriangleHit(hitInfo);
    uint materialID = set0.gSceneObjects[th.instanceID].materialID;
    VertexData v    = getVertexData(th, set0);
    UniformObject obj = set0.gSceneObjects[th.instanceID];
    DiffuseShading sd = prepareDiffuseShading(v, materialID, obj.illuminationAmount, set0);

    float4 ckey = gDirectKey.read(uint2(p));
    float  curZ = ckey.x;
    float3 curN = normalize(ckey.yzw);

    // Decorrelate this pass's RNG stream from the temporal pass.
    RNG rng;
    rng_init(rng, uint2(p) + uint2(13u, 37u), gPerFrame->totalFrames);

    // Start the combination from the center (temporal) reservoir.
    Reservoir R; reservoirInit(R);
    Reservoir c = unpackReservoir(gReservoirIn.read(uint2(p)));
    reservoirMerge(R, c, directTargetPdf(sd, c.y, set0, gPerFrame), rng);

    // Merge a few same-surface neighbours.
    for (int i = 0; i < kSpatialSamples; ++i) {
        float ox = rng_next_float(rng);
        float oy = rng_next_float(rng);
        float2 o = (float2(ox, oy) * 2.0f - 1.0f) * kSpatialRadius;
        int2   q = p + int2(int(round(o.x)), int(round(o.y)));
        if (q.x < 0 || q.y < 0 || q.x >= int(outW) || q.y >= int(outH)) continue;
        if (q.x == p.x && q.y == p.y) continue;

        float4 nkey = gDirectKey.read(uint2(q));
        float3 nN   = nkey.yzw;
        if (dot(nN, nN) <= 1.0e-8f) continue;                       // invalid neighbour
        if (abs(curZ - nkey.x) > kReprojZRelTol * max(curZ, 1.0e-3f)) continue;
        if (dot(curN, normalize(nN)) < kReprojNormalCos) continue;  // geometry reject

        Reservoir nb = unpackReservoir(gReservoirIn.read(uint2(q)));
        reservoirMerge(R, nb, directTargetPdf(sd, nb.y, set0, gPerFrame), rng);
    }

    reservoirFinalizeCombine(R);
    gReservoirOut.write(packReservoir(R), uint2(p));   // next frame's temporal history

    // Resolve the chosen light with one soft (blue-noise disk) shadow ray.
    float3 curResolved = resolveDirectReservoir(sd, R, rng, tlas, set0, gPerFrame);

    // Temporal accumulation of the RESOLVED irradiance (SVGF temporal stage).
    float2 dims   = float2(outW, outH);
    float2 curUV  = (float2(p) + 0.5f) / dims;
    float2 histUV = curUV - gVelocity.read(uint2(p)).xy;

    bool tvalid = !(any(histUV < 0.0f) || any(histUV > 1.0f));
    if (tvalid) {
        int2 ht = clamp(int2(histUV * dims), int2(0), int2(dims) - 1);
        float4 hk = gDirectKeyHistory.read(uint2(ht));
        tvalid = abs(curZ - hk.x) <= kReprojZRelTol * max(curZ, 1.0e-3f)
              && dot(curN, normalize(hk.yzw)) >= kReprojNormalCos;
    }

    float3 result;
    float  count;
    if (tvalid) {
        float4 h = gDirectHistory.sample(set0.gMaterialSampler, histUV, level(0.0f));
        count = min(h.a + 1.0f, kDirectMaxAccum);
        float alpha = max(1.0f / count, kDirectTemporalAlpha);
        result = mix(h.rgb, curResolved, alpha);
    } else {
        result = curResolved;
        count  = 1.0f;
    }

    gDirectLighting.write(float4(result, count), uint2(p));
}
