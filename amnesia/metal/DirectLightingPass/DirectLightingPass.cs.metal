// Hand-written Metal port of amnesia/slang/DirectLightingPass/DirectLightingPass.cs.slang
//
// Direct-lighting pass — stage 1 of 3 (ReSTIR DI temporal reuse), numthreads
// 16,16,1. Per pixel: reconstruct the surface (RT V-buffer hit, raster
// fallback), build a light-selection reservoir by streaming RIS over the cell's
// lights (UNSHADOWED-luminance target, NO shadow ray — this pass traces
// nothing), then temporally reuse the previous frame's reservoir (reprojected
// through gVelocity, rejected on a depth/normal disocclusion test, previous M
// clamped). Writes the merged reservoir + current surface key. The shadow ray /
// resolve happens in the spatial pass.
//
// Self-contained; scalar struct layouts (packed types). Set-0 bindless globals
// come through one Set0 argument buffer (buffer(0), [[id]] = kBinding*); the
// program-managed set 1 / set 2 [[id]] values are the mtl.index from
// kDirectLightingCs in HybridRenderer.cpp.
//
// Bindings:
//   Set0 buffer(0): gOpaque*Handles 3/4/5/6/8  gMaterialSampler 9
//     gSceneObjects 20  gDiffuseMaterials 21  gPointLights 22  gSpotLights 29
//     gSurfelDepthSampler 35  gLightGridCount 40  gLightGridList 41
//     gAttenuationLut 45  gTextures2D id(49 remapped)  gTexturesCube id(16433 remapped)
//   Set1 buffer(1): gPerFrame id(34)  gPackedHitInfo id(6)[uint,read]
//   Set2 buffer(2): gPackedHitInfoRaster id(0)[uint,read]  gVelocity id(1)[float2,read]
//     gReservoirHistory id(2)[float4,read]  gReservoirOut id(3)[float4,read_write]
//     gDirectKeyHistory id(4)[float4,read]  gDirectKeyOut id(5)[float4,read_write]
#include <metal_stdlib>
using namespace metal;

constant float kInvPi = 0.31830988618379067154f;
constant uint  kInvalidTextureIndex = 0xffffffffu;
constant float kDefaultDiffuseRoughness = 1.0f;
constant float kPointLightSourceRadiusSq = 0.25f;
constant uint  kMaterialFlagEnableSpecular = 1u << 2;
constant uint  kMaterialFlagEnableCubeMap  = 1u << 6;
// Light-grid geometry (Constants.h).
constant uint  kLightGridDim     = 128u;
constant float kLightGridUnit    = 0.9765625f;   // (kCellDimension*kCellUnit)/kLightGridDim = (250*0.5)/128
constant uint  kLightsPerCellMax = 128u;
constant float kCellUnit = 0.5f;
// ReSTIR DI temporal-reuse tuning (Constants.h).
constant float kReprojZRelTol   = 0.05f;
constant float kReprojNormalCos = 0.9f;
constant float kReservoirMClamp = 20.0f;

constant float3 kReservoirLum = float3(0.2126f, 0.7152f, 0.0722f);

// --- Shared struct layouts + utilities (flattened in at stage time) ----------
#include "SceneTypes.metalh"            // SceneConstants, MatStorage, UniformObject, HitInfo, TriangleHit, VertexData, lights
#include "Random.metalh"                // RNG + rng_*
#include "Reservoir.metalh"             // Reservoir + init/streamCandidate/finalizeBuild/merge/finalizeCombine + pack/unpack
#include "Grid.metalh"                  // gridCellValid

// RT-unique structs the shared headers need.
struct DiffuseMaterial { uint type; uint materialConfig; uint cubeMapTextureIndex; uint tex[8]; float heightMapScale, heightMapBias, frenselBias, frenselPow; };
struct LightSample { float3 Li; float pdf; float3 dir; float distance; };

// --- light-grid cell math (pass-local; explicit-dim variants, distinct from the
// surfel-cell helpers in SurfelUtils.metalh). Mirrors Grid.slang gridCellPos /
// gridFlattenCellIndex (gridCellValid comes from Grid.metalh). -----------------
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
    array<texture2d<float>, 16384>   gTextures2D   [[id(49)]];     // kBindingTextures2D (remapped)
    array<texturecube<float>, 16384> gTexturesCube [[id(16433)]];  // kBindingTexturesCube (remapped)
};

// Set-1 / Set-2 program argument buffers; [[id]] = mtl.index (HybridRenderer.cpp).
// Metal argument-buffer members must be declared in ascending [[id]] order.
struct Set1 {
    texture2d<uint, access::read> gPackedHitInfo [[id(6)]];
    constant SceneConstants* gPerFrame      [[id(34)]];
};
struct Set2 {
    texture2d<uint,  access::read>       gPackedHitInfoRaster [[id(0)]];
    texture2d<float, access::read>       gVelocity            [[id(1)]];
    texture2d<float, access::read>       gReservoirHistory    [[id(2)]];
    texture2d<float, access::read_write> gReservoirOut        [[id(3)]];
    texture2d<float, access::read>       gDirectKeyHistory    [[id(4)]];
    texture2d<float, access::read_write> gDirectKeyOut        [[id(5)]];
};

// Utility headers that reference `device Set0&` must come AFTER struct Set0.
#include "SurfelGI/BindlessTriangle.metalh"  // loadMat, getVertexData, getTriangleHit, hitIsValid
#include "Brdf.metalh"                        // decodeMaterialRoughness, evalDiffuseDemodulated
#include "DiffuseShading.metalh"              // DiffuseShading + prepareDiffuseShading
#include "Light.metalh"                       // sampleLight / samplePointLight / sampleSpotLight

// --- per-light unshadowed demodulated contribution (DiffuseShading.slang
// unshadowedAnalyticLight + sampleLightShade). No shadow ray. V is toward the
// camera. Returns float3(0) on sample-fail / back-face. -----------------------
static float3 unshadowedAnalyticLight(float3 posW, float3 normalW, float3 V, float rough,
                                      uint lightIndex, device Set0& set0, constant SceneConstants* gPerFrame) {
    LightSample ls;
    if (!sampleLight(lightIndex, posW, ls, set0, gPerFrame)) return float3(0.0f);
    float NdotL = dot(normalW, ls.dir);
    if (NdotL <= 0.0f) return float3(0.0f);
    return ls.Li * NdotL * evalDiffuseDemodulated(normalW, V, ls.dir, rough);
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
static uint cellLightAt(CellLights c, uint k, device Set0& set0) {
    return set0.gLightGridList[c.flat * kLightsPerCellMax + k];
}

// Target function (luminance of the unshadowed demodulated contribution).
static float directTargetPdf(DiffuseShading sd, uint lightIndex, device Set0& set0, constant SceneConstants* gPerFrame) {
    float3 V = normalize(gPerFrame->posW - sd.posW);
    return dot(unshadowedAnalyticLight(sd.posW, sd.normalW, V, sd.rough, lightIndex, set0, gPerFrame), kReservoirLum);
}

// Streaming RIS over the cell's lights (unshadowed-luminance target).
static Reservoir buildDirectReservoir(DiffuseShading sd, thread RNG& rng, device Set0& set0, constant SceneConstants* gPerFrame) {
    Reservoir r; reservoirInit(r);
    CellLights cl = getCellLights(sd.posW, set0, gPerFrame);
    float3 V = normalize(gPerFrame->posW - sd.posW);
    for (uint k = 0u; k < cl.count; ++k) {
        uint li = cellLightAt(cl, k, set0);
        float pHat = dot(unshadowedAnalyticLight(sd.posW, sd.normalW, V, sd.rough, li, set0, gPerFrame), kReservoirLum);
        reservoirStreamCandidate(r, li, pHat, rng);
    }
    reservoirFinalizeBuild(r);
    return r;
}

[[kernel]] void csMain(uint3 tid [[thread_position_in_grid]],
                       device Set0& set0 [[buffer(0)]],
                       device Set1& set1 [[buffer(1)]],
                       device Set2& set2 [[buffer(2)]]) {
    constant SceneConstants* gPerFrame = set1.gPerFrame;
    texture2d<uint, access::read> gPackedHitInfo       = set1.gPackedHitInfo;
    texture2d<uint,  access::read>       gPackedHitInfoRaster = set2.gPackedHitInfoRaster;
    texture2d<float, access::read>       gVelocity            = set2.gVelocity;
    texture2d<float, access::read>       gReservoirHistory    = set2.gReservoirHistory;
    texture2d<float, access::read_write> gReservoirOut        = set2.gReservoirOut;
    texture2d<float, access::read>       gDirectKeyHistory    = set2.gDirectKeyHistory;
    texture2d<float, access::read_write> gDirectKeyOut        = set2.gDirectKeyOut;

    uint outW = gReservoirOut.get_width();
    uint outH = gReservoirOut.get_height();
    uint2 pixelPos = tid.xy;
    if (pixelPos.x >= outW || pixelPos.y >= outH)
        return;

    HitInfo hitInfo;
    hitInfo.packed = gPackedHitInfo.read(pixelPos);
    if (!hitIsValid(hitInfo))
        hitInfo.packed = gPackedHitInfoRaster.read(pixelPos);
    if (!hitIsValid(hitInfo)) {
        gReservoirOut.write(float4(0.0f), pixelPos);   // empty reservoir
        gDirectKeyOut.write(float4(0.0f), pixelPos);   // zero normal -> rejected
        return;
    }

    TriangleHit th  = getTriangleHit(hitInfo);
    uint materialID = set0.gSceneObjects[th.instanceID].materialID;
    VertexData v    = getVertexData(th, set0);
    UniformObject obj = set0.gSceneObjects[th.instanceID];
    DiffuseShading sd = prepareDiffuseShading(v, materialID, obj.illuminationAmount, set0);

    float  curZ = -(gPerFrame->viewMat * float4(sd.posW, 1.0f)).z;
    float3 curN = sd.normalW;

    float2 dims   = float2(outW, outH);
    float2 curUV  = (float2(pixelPos) + 0.5f) / dims;
    float2 histUV = curUV - gVelocity.read(pixelPos).xy;

    bool valid = !(any(histUV < 0.0f) || any(histUV > 1.0f));
    int2 ht = int2(0, 0);
    if (valid) {
        ht = clamp(int2(histUV * dims), int2(0), int2(dims) - 1);
        float4 k = gDirectKeyHistory.read(uint2(ht));
        float  histZ = k.x;
        float3 histN = k.yzw;
        valid = abs(curZ - histZ) <= kReprojZRelTol * max(curZ, 1.0e-3f)
             && dot(curN, normalize(histN)) >= kReprojNormalCos;
    }

    RNG rng;
    rng_init(rng, pixelPos, gPerFrame->totalFrames);

    Reservoir cur = buildDirectReservoir(sd, rng, set0, gPerFrame);

    Reservoir R; reservoirInit(R);
    reservoirMerge(R, cur, cur.pHatY, rng);

    if (valid) {
        Reservoir prev = unpackReservoir(gReservoirHistory.read(uint2(ht)));
        prev.M = min(prev.M, kReservoirMClamp * max(cur.M, 1.0f));
        float pHatPrev = directTargetPdf(sd, prev.y, set0, gPerFrame);
        reservoirMerge(R, prev, pHatPrev, rng);
    }

    reservoirFinalizeCombine(R);

    gReservoirOut.write(packReservoir(R), pixelPos);
    gDirectKeyOut.write(float4(curZ, curN), pixelPos);
}
