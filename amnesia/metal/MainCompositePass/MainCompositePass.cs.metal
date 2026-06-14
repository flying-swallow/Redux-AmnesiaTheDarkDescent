// Hand-written Metal port of amnesia/slang/MainCompositePass/MainCompositePass.cs.slang
//
// Final composite pass for surfel-GI rendering (numthreads 16,16,1). For each
// pixel: resolve the V-buffer hit (RT, raster fallback), decode albedo +
// emission, composite this object's clustered OOB decals onto the albedo
// (display-space exact blend), then form  albedo*(direct + indirect) + emission,
// and apply per-pixel fog. Both direct and indirect are demodulated irradiance
// read from textures (DirectLightingPass / surfel cache); the composite applies
// albedo once. Misses emit black (no env map bound today).
//
// gRtAccel is VK-only (slangc drops it from the MSL kernel) so there is NO TLAS
// and NO shadow ray in this Metal port — direct lighting comes entirely from the
// gDirectLighting texture, matching the .slang which already reads it directly.
//
// Self-contained; scalar struct layouts (packed types). Set-0 bindless globals
// come through one Set0 argument buffer (buffer(0), [[id]] = kBinding*); the
// program-managed set 1 / set 2 [[id]] values are the mtl.index from
// kMainCompositeCs in HybridRenderer.cpp.
//
// Bindings:
//   Set0 buffer(0): gOpaque*Handles 3/4/5/6/8  gMaterialSampler 9
//     gSceneObjects 20  gDiffuseMaterials 21  gAttenuationLut 45  gDecals 46
//     gObjectDecalIndices 47  gFogAreas 42  gTextures2D id(49 remapped)
//   Set1 buffer(1): gPerFrame id(34)  gPackedHitInfo id(4)[uint,read]
//   Set2 buffer(2): gIndirectLighting id(0)[float4,read]  gDirectLighting id(1)
//     gPackedHitInfoRaster id(2)[uint,read]  gOutput id(3)[float4,read_write]
#include <metal_stdlib>
using namespace metal;

constant uint  kInvalidTextureIndex = 0xffffffffu;
constant float kDefaultDiffuseRoughness = 1.0f;
constant float kPointLightSourceRadiusSq = 0.25f;   // (unused; for Light/Brdf parity)
constant uint  kMaterialFlagEnableSpecular = 1u << 2;
constant uint  kMaterialFlagEnableCubeMap  = 1u << 6;
constant uint  kDefaultRenderDirectLighting   = 1u;
constant uint  kDefaultRenderIndirectLighting = 1u;
constant uint  kDefaultBlendingDelay = 30u;
constant float kCellUnit = 0.5f;
// Decal facing-cull threshold (composite slang kDecalFacingCos).
constant float kDecalFacingCos = 0.25f;
// Raw eMaterialBlendMode numbering (Constants.h kMaterialBlendMode*).
constant uint  kMaterialBlendModeNone        = 0u;
constant uint  kMaterialBlendModeAdd         = 1u;
constant uint  kMaterialBlendModeMul         = 2u;
constant uint  kMaterialBlendModeMulX2       = 3u;
constant uint  kMaterialBlendModeAlpha       = 4u;
constant uint  kMaterialBlendModePremulAlpha = 5u;

// --- Shared struct layouts + utilities (flattened in at stage time) ----------
#include "SceneTypes.metalh"            // SceneConstants, MatStorage, UniformObject, HitInfo, TriangleHit, VertexData, lights
#include "Random.metalh"                // RNG (Brdf's sampleBrdf signature needs it)
// Fog.metalh declares FogAreaParams (referenced by struct Set0 below) + the
// computeFog/applyFog helpers (neither depends on Set0), so include it ahead of Set0.
#include "Fog.metalh"                   // FogAreaParams + computeFog/applyFog

// RT-unique structs the shared headers need (mirrors SurfelRayTrace.rt.metal).
struct DiffuseMaterial { uint type; uint materialConfig; uint cubeMapTextureIndex; uint tex[8]; float heightMapScale, heightMapBias, frenselBias, frenselPow; };
struct LightSample { float3 Li; float pdf; float3 dir; float distance; };
// GpuDecal — scalar device-buffer layout mirroring SceneTypes.slang.
struct GpuDecal {
    float4x4      invModelMat;
    float4        color;
    uint          materialID;
    uint          receiverMask;
    uint          blendMode;
    uint          subDivX;
    uint          subDivY;
    uint          subDivIndex;
    packed_float3 projAxisWS;
    float         _pad;
};

// Set-0 bindless argument buffer (subset this kernel reads). Members in
// ascending [[id]] order; gTextures2D remapped to id(49) (Metal arg-buffer rule,
// same as SurfelRayTrace.rt.metal).
struct Set0 {
    device ulong*           gOpaquePositionHandles [[id(3)]];
    device ulong*           gOpaqueTangentHandles  [[id(4)]];
    device ulong*           gOpaqueNormalHandles   [[id(5)]];
    device ulong*           gOpaqueUv0Handles      [[id(6)]];
    device ulong*           gOpaqueIndexHandles    [[id(8)]];
    sampler                 gMaterialSampler       [[id(9)]];
    device UniformObject*   gSceneObjects          [[id(20)]];
    device DiffuseMaterial* gDiffuseMaterials      [[id(21)]];
    device FogAreaParams*   gFogAreas              [[id(42)]];
    texture2d<float>        gAttenuationLut        [[id(45)]];
    device GpuDecal*        gDecals                [[id(46)]];
    device uint*            gObjectDecalIndices    [[id(47)]];
    array<texture2d<float>, 16384> gTextures2D     [[id(49)]];  // kBindingTextures2D (remapped)
};

// Set-1 / Set-2 program argument buffers; [[id]] = mtl.index (HybridRenderer.cpp).
// Metal argument-buffer members must be declared in ascending [[id]] order.
struct Set1 {
    texture2d<uint, access::read> gPackedHitInfo [[id(4)]];
    constant SceneConstants* gPerFrame      [[id(34)]];
};
struct Set2 {
    texture2d<float, access::read>       gIndirectLighting    [[id(0)]];
    texture2d<float, access::read>       gDirectLighting      [[id(1)]];
    texture2d<uint,  access::read>       gPackedHitInfoRaster [[id(2)]];
    texture2d<float, access::read_write> gOutput              [[id(3)]];
};

// Utility headers that reference `device Set0&` must come AFTER struct Set0.
#include "SurfelGI/BindlessTriangle.metalh"  // loadMat, getVertexData, getTriangleHit, hitIsValid
#include "Brdf.metalh"                        // decodeMaterialRoughness (prepareDiffuseShading needs it)
#include "DiffuseShading.metalh"              // DiffuseShading + prepareDiffuseShading

// --- display-space sRGB helpers (Utils/Color/ColorHelpers.slang) -------------
static float sRGBToLinear1(float s) {
    return (s <= 0.04045f) ? s * (1.0f / 12.92f) : pow((s + 0.055f) * (1.0f / 1.055f), 2.4f);
}
static float3 sRGBToLinear(float3 s) { return float3(sRGBToLinear1(s.x), sRGBToLinear1(s.y), sRGBToLinear1(s.z)); }
static float linearToSRGB1(float l) {
    return (l <= 0.0031308f) ? l * 12.92f : pow(l, 1.0f / 2.4f) * 1.055f - 0.055f;
}
static float3 linearToSRGB(float3 l) { return float3(linearToSRGB1(l.x), linearToSRGB1(l.y), linearToSRGB1(l.z)); }

// EXACT display-space software blend (BlendModes.slang blendDisplaySpaceExact);
// raw eMaterialBlendMode numbering as stored in GpuDecal.blendMode.
static float3 blendDisplaySpaceExact(uint m, float3 dstDisplay, float4 srcDisplay) {
    if (m == kMaterialBlendModeAdd)         return dstDisplay + srcDisplay.rgb;
    if (m == kMaterialBlendModeMul)         return dstDisplay * srcDisplay.rgb;
    if (m == kMaterialBlendModeMulX2)       return 2.0f * dstDisplay * srcDisplay.rgb;
    if (m == kMaterialBlendModeAlpha)       return mix(dstDisplay, srcDisplay.rgb, srcDisplay.a);
    if (m == kMaterialBlendModePremulAlpha) return srcDisplay.rgb + dstDisplay * (1.0f - srcDisplay.a);
    return dstDisplay;   // None
}

// --- decal projection (composite slang projectDecal/compositeDecals) ---------
static float3 projectDecal(uint di, float3 albedo, float3 posW, float3 normalW, device Set0& set0) {
    GpuDecal d = set0.gDecals[di];

    float3 lp = (d.invModelMat * float4(posW, 1.0f)).xyz;   // world -> box-local
    if (any(abs(lp) > 0.5f)) return albedo;

    float3x3 invM3 = float3x3(d.invModelMat[0].xyz, d.invModelMat[1].xyz, d.invModelMat[2].xyz);
    float3 nLocal = invM3 * normalW;
    float  nLen   = length(nLocal);
    if (nLen < 1.0e-6f || abs(nLocal.y) <= kDecalFacingCos * nLen) return albedo;

    uint  sx = max(d.subDivX, 1u);
    uint  sy = max(d.subDivY, 1u);
    float fU = float(d.subDivIndex % sx);
    float fV = float(d.subDivIndex / sx);
    float2 uv = float2(saturate((fU + (lp.x + 0.5f)) / float(sx)),
                       1.0f - saturate((fV + (lp.z + 0.5f)) / float(sy)));

    DiffuseMaterial dm = set0.gDiffuseMaterials[d.materialID];
    uint dtex = dm.tex[0];
    if (dtex == kInvalidTextureIndex) return albedo;

    float4 s = set0.gTextures2D[dtex].sample(set0.gMaterialSampler, uv, level(0.0f));
    float4 srcD = float4(linearToSRGB(s.rgb), s.a) * d.color;
    if (srcD.a < 0.01f) return albedo;   // legacy ALPHA_REJECT

    return sRGBToLinear(blendDisplaySpaceExact(d.blendMode, linearToSRGB(albedo), srcD));
}

static float3 compositeDecals(float3 albedo, float3 posW, float3 normalW, uint decalList, device Set0& set0) {
    uint count  = decalList & 0xFFu;
    uint offset = decalList >> 8;
    for (uint k = 0u; k < count; ++k)
        albedo = projectDecal(set0.gObjectDecalIndices[offset + k], albedo, posW, normalW, set0);
    return albedo;
}

[[kernel]] void csMain(uint3 tid [[thread_position_in_grid]],
                       device Set0& set0 [[buffer(0)]],
                       device Set1& set1 [[buffer(1)]],
                       device Set2& set2 [[buffer(2)]]) {
    constant SceneConstants* gPerFrame = set1.gPerFrame;
    texture2d<uint, access::read> gPackedHitInfo       = set1.gPackedHitInfo;
    texture2d<float, access::read> gIndirectLighting    = set2.gIndirectLighting;
    texture2d<float, access::read> gDirectLighting      = set2.gDirectLighting;
    texture2d<uint,  access::read> gPackedHitInfoRaster = set2.gPackedHitInfoRaster;
    texture2d<float, access::read_write> gOutput        = set2.gOutput;

    uint2 pixelPos = tid.xy;
    uint outW = gOutput.get_width();
    uint outH = gOutput.get_height();
    if (pixelPos.x >= outW || pixelPos.y >= outH)
        return;

    HitInfo hitInfo;
    hitInfo.packed = gPackedHitInfo.read(pixelPos);

    if (!hitIsValid(hitInfo)) {
        hitInfo.packed = gPackedHitInfoRaster.read(pixelPos);
        if (!hitIsValid(hitInfo)) {
            gOutput.write(float4(0.0f, 0.0f, 0.0f, 1.0f), pixelPos);
            return;
        }
    }

    TriangleHit triangleHit = getTriangleHit(hitInfo);
    uint materialID = set0.gSceneObjects[triangleHit.instanceID].materialID;

    VertexData v = getVertexData(triangleHit, set0);
    UniformObject obj = set0.gSceneObjects[triangleHit.instanceID];
    DiffuseShading sd = prepareDiffuseShading(v, materialID, obj.illuminationAmount, set0);

    sd.albedo = compositeDecals(sd.albedo, v.posW, sd.normalW, obj.decalList, set0);

    bool wantDirect   = kDefaultRenderDirectLighting   != 0u;
    bool wantIndirect = kDefaultRenderIndirectLighting != 0u;

    float3 direct   = wantDirect   ? gDirectLighting.read(pixelPos).xyz   : float3(0.0f);
    float3 indirect = wantIndirect ? gIndirectLighting.read(pixelPos).xyz : float3(0.0f);

    float3 finalGather = sd.albedo * (direct + indirect);
    finalGather += sd.emission;

    finalGather = applyFog(finalGather, v.posW, gPerFrame->posW, set0.gFogAreas, gPerFrame);

    gOutput.write(float4(finalGather, 1.0f), pixelPos);
}
