// Hand-written Metal port of amnesia/slang/WaterPass/Water.frag.slang
//
// Raster water surface fragment stage. The underwater refraction background is
// already in the pogo (the RT V-buffer's water refraction clobbered the primary
// hit). This pass adds the surface in TWO draws:
//   pass 0 (MUL blend): output surfaceTint·kWaterRefractionExposure (tints/lifts
//                       the refracted bottom).
//   pass 1 (ADD blend): wave normal + Fresnel·fade x an INLINE ray-traced LIT
//                       reflection -> dst += reflColor·fresnel.
//
// Slang's Scene.materials.traceReflectionHit + SurfelGI.SurfelShade (NEE direct +
// surfel-cache indirect) cannot be lowered by slangc to Metal, so the reflection
// shading is authored here against Metal's INLINE ray-query model — same pattern
// as SurfelRayTrace.rt.metal / SurfelVBuffer.rt.metal:
//   metal::raytracing::intersection_query drives the closest-hit reflection trace
//   and the per-light shadow rays; alpha-test runs INLINE in the candidate loop.
//
// Bindings:
//   set0 (bindless arg buffer) buffer(0); [[id]] = kBinding* (Constants.h):
//     gMaterialSampler 9   gSurfelBuffer 11   gSurfelRecycle 16   gCellInfo 18
//     gCellToSurfel 19   gSceneObjects 20   gDiffuseMaterials 21   gPointLights 22
//     gWaterMaterials 25   gSpotLights 29   gSurfelDepthSampled 34
//     gSurfelDepthSampler 35   gLightGridCount 40   gLightGridList 41
//     gAttenuationLut 45   gTextures2D[] 0 (remapped)   gTexturesCube[] 1 (remapped)
//     + the bindless geometry handle arrays (3/4/5/6/8) used by the triangle fetch
//   gPerFrame buffer(1) ; tlas buffer(1) id(1)
//   push constant (WaterPC) -> [[buffer(2)]]
#include <metal_stdlib>
#include <metal_raytracing>
using namespace metal;
using namespace raytracing;

// --- shared constants (mirrored from amnesia/slang/Constants.h) --------------
constant uint  kInvalidTextureIndex            = 0xffffffffu;
constant uint  kSolidMaterialCapacity          = 2048u;
constant uint  kTranslucentMaterialCapacity    = 256u;
constant uint  kTranslucentMaterialBase        = kSolidMaterialCapacity;                       // 2048
constant uint  kWaterMaterialBase              = kSolidMaterialCapacity + kTranslucentMaterialCapacity; // 2304

constant uint  kMaterialFlagEnableSpecular         = 1u << 2;
constant uint  kMaterialFlagEnableCubeMap          = 1u << 6;
constant uint  kMaterialFlagEnableDissolveAlpha    = 1u << 7;
constant uint  kMaterialFlagIsAlphaSingleChannel   = 1u << 10;
constant uint  kMaterialFlagUseDissolveFilter      = 1u << 14;

constant float kPointLightSourceRadiusSq = 0.25f;
constant float kDefaultDiffuseRoughness  = 1.0f;
constant uint  kDefaultBlendingDelay     = 30u;
constant uint  kDefaultRenderDirectLighting   = 1u;
constant uint  kDefaultRenderIndirectLighting = 1u;
constant float kShadeRayEps = 1.0e-3f;

// Water combine constants.
constant float kWaterReflectionExposure   = 1.0f;
constant float kWaterRefractionExposure   = 2.0f;
constant float kWaterReflectionTurbulence = 0.3f;

// Light-grid math constants (distinct dim from the surfel cell grid).
constant uint  kLightGridDim     = 128u;
constant uint  kLightsPerCellMax = 128u;
// kLightGridUnit = (kCellDimension * kCellUnit) / kLightGridDim = (250*0.5)/128.
constant float kLightGridUnit    = (250.0f * 0.5f) / 128.0f;

// kCellUnit (world cell size) is referenced by lookupSurfelCell in SurfelGather.metalh.
constant float kCellUnit = 0.5f;

// --- shared struct layouts (flattened in by cmake/flatten_metal_includes.py) ---
#include "SceneTypes.metalh"               // SceneConstants, MatStorage, UniformObject, TriangleHit, VertexData, lights
#include "SurfelGI/SurfelTypes.metalh"     // Surfel, CellInfo, SurfelRecycleInfo (+ MSMEData)
#include "SurfelGI/SurfelUtils.metalh"     // getCellPos/isCellValid/getFlattenCellIndex, getSurfelDepthUV (+ kCellDimension)

// Material tables (scalar field order verbatim — mirror SceneTypes.slang).
struct DiffuseMaterial { uint type; uint materialConfig; uint cubeMapTextureIndex; uint tex[8];
                         float heightMapScale, heightMapBias, frenselBias, frenselPow; };
struct WaterMaterial { uint type; uint materialConfig; uint tex[8];
                       float refractionScale, frenselBias, frenselPow, reflectionFadeStart, reflectionFadeEnd,
                       waveSpeed, waveAmplitude, waveFreq; };
// LightSample / DiffuseShading mirror the RT ports (Brdf/DiffuseShading helpers below).
struct LightSample { float3 Li; float pdf; float3 dir; float distance; };

// Set-0 bindless argument buffer (subset this stage reads). Array textures
// consume consecutive [[id]] slots so they go LAST: gTextures2D at id(49)
// (49..16432), gTexturesCube at id(16433) (16433..32816); the C++ bindless encoder
// remaps them to kBindingTextures2D / kBindingTexturesCube. Members in ascending id.
struct Set0 {
    device ulong*             gOpaquePositionHandles  [[id(3)]];
    device ulong*             gOpaqueTangentHandles   [[id(4)]];
    device ulong*             gOpaqueNormalHandles    [[id(5)]];
    device ulong*             gOpaqueUv0Handles       [[id(6)]];
    device ulong*             gOpaqueIndexHandles     [[id(8)]];
    sampler                   gMaterialSampler        [[id(9)]];
    device Surfel*            gSurfelBuffer           [[id(11)]];
    device SurfelRecycleInfo* gSurfelRecycleInfoBuffer [[id(16)]];
    device CellInfo*          gCellInfoBuffer         [[id(18)]];
    device uint*              gCellToSurfelBuffer     [[id(19)]];
    device UniformObject*     gSceneObjects           [[id(20)]];
    device DiffuseMaterial*   gDiffuseMaterials       [[id(21)]];
    device PointLight*        gPointLights            [[id(22)]];
    device WaterMaterial*     gWaterMaterials         [[id(25)]];
    device SpotLight*         gSpotLights             [[id(29)]];
    texture2d<float, access::sample> gSurfelDepth     [[id(34)]];   // kBindingSurfelDepthSampled
    sampler                   gSurfelDepthSampler     [[id(35)]];
    device uint*              gLightGridCount         [[id(40)]];
    device uint*              gLightGridList          [[id(41)]];
    texture2d<float>          gAttenuationLut         [[id(45)]];
    texture2d<float>          gDissolveMap            [[id(48)]];
    array<texture2d<float>,   16384> gTextures2D      [[id(49)]];     // kBindingTextures2D (remapped)
    array<texturecube<float>, 16384> gTexturesCube    [[id(16433)]];  // kBindingTexturesCube (remapped)
};
// Set-1 program argument buffer; the TLAS rides at id(1) (same as the RT ports).
struct Set1 {
    constant SceneConstants*        gPerFrame [[id(0)]];
    instance_acceleration_structure tlas      [[id(1)]];
};

struct WaterPC { uint pass; uint _p0; uint _p1; uint _p2; };

struct PSIn {
    float3 posW;
    float3 normalW;
    float3 tangentW;
    float2 uv;
    uint   objectId [[flat]];
    float4 posH     [[position]];
};

// --- Utility headers that reference `device Set0&` must come AFTER struct Set0. ---
#include "SurfelGI/BindlessTriangle.metalh"  // loadMat, TriVtx, fetchBindlessTriangle, getVertexData, packTriangleHit
#include "Scene.metalh"                       // getObject/getMaterialID/alphaTest (needs DiffuseMaterial + flags)
#include "Random.metalh"                      // RNG + rng_next_float (required by Brdf.metalh::sampleBrdf)
#include "Brdf.metalh"                        // diffuseLobeWeight, evalDiffuseDemodulated, decodeMaterialRoughness
#include "DiffuseShading.metalh"              // DiffuseShading, prepareDiffuseShading
#include "Light.metalh"                       // sampleLight (point/spot NEE) + LUT lookups
#include "SurfelGI/SurfelGather.metalh"       // SurfelHit, testSurfelCoverage, surfelKernelWeight, surfelDepthWeight, lookupSurfelCell
#include "TranslucentPass/WaterCommon.metalh" // waterWaveNormalTangent

// --- material accessors (mirror Scene.slang) ---------------------------------
static WaterMaterial getWaterMaterial(uint materialID, device Set0& set0) {
    return set0.gWaterMaterials[materialID - kWaterMaterialBase];
}

// Schlick-Fresnel — base clamped positive (see Translucent.frag).
static float computeFresnel(float edotN, float bias, float pw) {
    float facing = max(1.0f - edotN, 1.0e-6f);
    return saturate(bias + (1.0f - bias) * pow(facing, pw));
}

// --- light-grid math (mirror Grid.slang) -------------------------------------
static bool  gridCellValid(int3 c, uint dim) { int h = int(dim / 2u); return abs(c.x) < h && abs(c.y) < h && abs(c.z) < h; }
static int3  gridCellPos(float3 p, float3 cam, float gu) { return int3(round((p - cam) / gu)); }
static uint  gridFlattenCellIndex(int3 c, uint dim) { uint3 u = uint3(c + int3(int(dim / 2u))); return u.z*dim*dim + u.y*dim + u.x; }

// --- inline ray-query traversal (mirror traceReflectionHit / traceShadowRay) --
// Both cull back-facing triangles (the slang traceReflectionHit uses
// CULL_BACK_FACING_TRIANGLES); the shadow ray uses the same alpha-tested commit.
static bool traceClosest(float3 origin, float3 dir, float tMin, float tMax,
                         instance_acceleration_structure tlas, device Set0& set0, thread TriangleHit& outHit) {
    ray r; r.origin = origin; r.direction = dir; r.min_distance = tMin; r.max_distance = tMax;
    intersection_query<triangle_data, instancing> q;
    q.reset(r, tlas, 0xFFu);
    while (q.next()) {
        if (q.get_candidate_intersection_type() == intersection_type::triangle) {
            TriangleHit h;
            h.instanceID     = q.get_candidate_instance_id();
            h.primitiveIndex = q.get_candidate_primitive_id();
            h.barycentrics   = q.get_candidate_triangle_barycentric_coord();
            VertexData v = getVertexData(h, set0);
            uint mat      = set0.gSceneObjects[h.instanceID].materialID;
            float dissolve = set0.gSceneObjects[h.instanceID].dissolveAmount;
            if (!alphaTest(v, mat, 0.0f, dissolve, set0))
                q.commit_triangle_intersection();
        }
    }
    if (q.get_committed_intersection_type() != intersection_type::triangle) return false;
    outHit.instanceID     = q.get_committed_instance_id();
    outHit.primitiveIndex = q.get_committed_primitive_id();
    outHit.barycentrics   = q.get_committed_triangle_barycentric_coord();
    return true;
}
// Returns true == visible (unoccluded). Alpha-tested first-hit occlusion.
static bool traceShadowRay(float3 origin, float3 dir, float dist,
                           instance_acceleration_structure tlas, device Set0& set0) {
    ray r; r.origin = origin; r.direction = dir; r.min_distance = 0.0f; r.max_distance = dist;
    intersection_query<triangle_data, instancing> q;
    q.reset(r, tlas, 0xFFu);
    while (q.next()) {
        if (q.get_candidate_intersection_type() == intersection_type::triangle) {
            TriangleHit h;
            h.instanceID     = q.get_candidate_instance_id();
            h.primitiveIndex = q.get_candidate_primitive_id();
            h.barycentrics   = q.get_candidate_triangle_barycentric_coord();
            VertexData v = getVertexData(h, set0);
            uint mat      = set0.gSceneObjects[h.instanceID].materialID;
            float dissolve = set0.gSceneObjects[h.instanceID].dissolveAmount;
            if (!alphaTest(v, mat, 0.0f, dissolve, set0)) return false;
        }
    }
    return true;
}

// --- NEE direct lighting (mirror SurfelShade.evalAnalyticLight + the shared
// DiffuseShading.shadeAnalyticLight). Demodulated: albedo-free radiance. -------
static float3 shadeAnalyticLight(float3 posW, float3 normalW, float3 V, float rough,
                                 uint lightIndex, float3 origin,
                                 instance_acceleration_structure tlas, device Set0& set0,
                                 constant SceneConstants* gPerFrame) {
    LightSample ls;
    if (!sampleLight(lightIndex, posW, ls, set0, gPerFrame)) return float3(0.0f);
    float NdotL = dot(normalW, ls.dir);
    if (NdotL <= 0.0f) return float3(0.0f);
    float3 contrib = ls.Li * NdotL * evalDiffuseDemodulated(normalW, V, ls.dir, rough);
    if (!traceShadowRay(origin, ls.dir, ls.distance, tlas, set0)) return float3(0.0f);
    return contrib;
}
static float3 evalAnalyticLight(DiffuseShading sd, instance_acceleration_structure tlas,
                                device Set0& set0, constant SceneConstants* gPerFrame) {
    float3 Lr = float3(0.0f);
    float3 origin = sd.posW + sd.normalW * kShadeRayEps;
    float3 V = normalize(float3(gPerFrame->posW) - sd.posW);

    uint nPoint = gPerFrame->pointLightCount;
    uint nSpot  = gPerFrame->spotLightCount;
    if (nPoint + nSpot == 0u) return Lr;

    int3 cell = gridCellPos(sd.posW, float3(gPerFrame->posW), kLightGridUnit);
    if (!gridCellValid(cell, kLightGridDim)) return Lr;
    uint flat  = gridFlattenCellIndex(cell, kLightGridDim);
    uint count = min(set0.gLightGridCount[flat], kLightsPerCellMax);

    for (uint k = 0u; k < count; ++k) {
        uint li = set0.gLightGridList[flat * kLightsPerCellMax + k];
        Lr += shadeAnalyticLight(sd.posW, sd.normalW, V, sd.rough, li, origin, tlas, set0, gPerFrame);
    }
    return Lr;
}

// --- world-space surfel-cache indirect gather (mirror SurfelShade.gatherSurfelIndirect) ---
static float3 gatherSurfelIndirect(float3 posW, float3 normalW, device Set0& set0,
                                   constant SceneConstants* gPerFrame) {
    uint flat; CellInfo cellInfo;
    if (!lookupSurfelCell(posW, float3(gPerFrame->posW), flat, cellInfo, set0.gCellInfoBuffer))
        return float3(0.0f);

    float4 indirect = float4(0.0f);
    for (uint i = 0u; i < cellInfo.surfelCount; ++i) {
        uint surfelIndex = set0.gCellToSurfelBuffer[cellInfo.cellToSurfelBufferOffset + i];
        Surfel surfel = set0.gSurfelBuffer[surfelIndex];

        float3 sNormal = normalize(float3(surfel.normal));
        SurfelHit hit;
        if (!testSurfelCoverage(posW, normalW, float3(surfel.position), sNormal, surfel.radius, hit))
            continue;

        float contribution = surfelKernelWeight(hit.dotN, hit.dist, surfel.radius);
        contribution *= surfelDepthWeight(surfelIndex, hit.dirUnit, sNormal, hit.dist,
                                          set0.gSurfelDepth, set0.gSurfelDepthSampler);

        SurfelRecycleInfo recycleInfo = set0.gSurfelRecycleInfoBuffer[surfelIndex];
        float blendDelayT = smoothstep(0.0f, float(kDefaultBlendingDelay), float(recycleInfo.frame));
        indirect += float4(float3(surfel.radiance), 1.0f) * contribution * blendDelayT;
    }
    if (indirect.w > 0.0f) return indirect.xyz / indirect.w;
    return float3(0.0f);
}

// Fully (lit-)shade a reflection-ray hit: albedo·(direct NEE + surfel-indirect) +
// emission — the same energy convention as the primary composite.
static float3 shadeReflectionHit(TriangleHit th, instance_acceleration_structure tlas,
                                 device Set0& set0, constant SceneConstants* gPerFrame) {
    uint matID  = set0.gSceneObjects[th.instanceID].materialID;
    VertexData v = getVertexData(th, set0);
    float illum  = set0.gSceneObjects[th.instanceID].illuminationAmount;
    DiffuseShading sd = prepareDiffuseShading(v, matID, illum, set0);
    float3 direct   = (kDefaultRenderDirectLighting   != 0u) ? evalAnalyticLight(sd, tlas, set0, gPerFrame) : float3(0.0f);
    float3 indirect = (kDefaultRenderIndirectLighting != 0u)
                      ? gatherSurfelIndirect(sd.posW, sd.normalW, set0, gPerFrame) : float3(0.0f);
    return sd.albedo * (direct + indirect) + sd.emission;
}

fragment float4 psMain(PSIn psIn [[stage_in]],
                       device Set0& set0 [[buffer(0)]],
                       device Set1& set1 [[buffer(1)]],
                       constant WaterPC& pc [[buffer(2)]]) {
    constant SceneConstants*        gPerFrame = set1.gPerFrame;
    instance_acceleration_structure tlas      = set1.tlas;

    UniformObject obj   = set0.gSceneObjects[psIn.objectId];
    WaterMaterial water = getWaterMaterial(obj.materialID, set0);

    if (pc.pass == 0u) {
        // Tint (+ refraction-exposure lift) of the refracted background (MUL).
        float3 tint = float3(1.0f);
        uint   diffuseTex = water.tex[0];
        if (diffuseTex != kInvalidTextureIndex)
            tint = set0.gTextures2D[diffuseTex].sample(set0.gMaterialSampler, psIn.uv).rgb;
        return float4(tint * kWaterRefractionExposure, 1.0f);
    }

    // pass 1 — Fresnel-weighted lit reflection (ADD).
    // Build the surface frame from screen-space posW derivatives (exact geometric
    // normal, independent of the mesh's vertex attributes).
    float3 eye  = normalize(psIn.posW - float3(gPerFrame->posW));   // camera -> surface
    float3 dpdx = dfdx(psIn.posW);
    float3 dpdy = dfdy(psIn.posW);
    float3 N    = normalize(cross(dpdx, dpdy));
    if (dot(eye, N) > 0.0f)
        N = -N;                                       // face the camera
    float3 T  = dpdx - N * dot(dpdx, N);              // tangent in the surface plane
    float  tl = length(T);
    T = (tl > 1.0e-5f) ? T / tl
        : ((abs(N.y) < 0.99f) ? normalize(cross(float3(0.0f, 1.0f, 0.0f), N))
                              : normalize(cross(float3(1.0f, 0.0f, 0.0f), N)));
    float3 B = cross(N, T);

    float3 nT = waterWaveNormalTangent(water, psIn.uv, gPerFrame->afT, set0);
    float3 waveN = normalize(nT.x * T + nT.y * B + nT.z * N);
    if (dot(eye, waveN) > 0.0f)
        waveN = -waveN;
    float edotN   = max(dot(-eye, waveN), 0.0f);
    float fresnel = computeFresnel(edotN, water.frenselBias, water.frenselPow);

    // Distance fade (only when a real fade range is authored).
    if (water.reflectionFadeEnd > water.reflectionFadeStart) {
        float viewZ   = -(gPerFrame->viewMat * float4(psIn.posW, 1.0f)).z;
        float fadeLen = water.reflectionFadeEnd - water.reflectionFadeStart;
        fresnel *= 1.0f - saturate((viewZ - water.reflectionFadeStart) / fadeLen);
    }

    // Reflection direction: blend wave normal toward flat (calmer than refraction).
    float3 flatN = N;  // already flipped to face the camera
    float3 reflN = normalize(mix(flatN, waveN, kWaterReflectionTurbulence));
    float3 reflectedDir = normalize(reflect(eye, reflN));

    float3 origin = psIn.posW + waveN * 1.0e-3f;
    TriangleHit rh;
    float3 reflColor = float3(0.0f);
    if (traceClosest(origin, reflectedDir, 0.0f, 1.0e30f, tlas, set0, rh))
        reflColor = shadeReflectionHit(rh, tlas, set0, gPerFrame) * kWaterReflectionExposure;

    return float4(reflColor * fresnel, 1.0f);
}
