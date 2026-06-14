// Hand-written Metal port of amnesia/slang/SurfelGI/SurfelRayTrace.rt.slang
//
// Surfel-GI scatter path tracer. Slang cannot lower the DXR model (TraceRay +
// shader table + inline RayQuery) to Metal, so this is authored against Metal's
// inline ray-query model: ONE compute kernel (rayGen) drives the path with
// metal::raytracing::intersection_query over an instance_acceleration_structure.
//   - "closest hit"  -> commit the nearest candidate that passes the alpha test
//   - "any hit"      -> the alpha test inside the candidate loop (no IFT needed)
//   - "miss"         -> the no-committed-hit branch (terminate)
//   - shadow ray     -> a second intersection_query; first opaque candidate = occluded
//
// Self-contained; scalar struct layouts (packed types). Argument buffers PER
// SET: buffer(0)=Set0 (bindless set 0, [[id]] = kBinding*), buffer(1)=Set1
// (gPerFrame, [[id(0)]]). The TLAS binds at buffer(2).
#include <metal_stdlib>
#include <metal_raytracing>
using namespace metal;
using namespace raytracing;

constant float kInvPi = 0.31830988618379067154f;
constant float kBrdfPi = 3.14159265358979323846f;
constant float kRayEps = 1.0e-3f;
constant uint  kInvalidTextureIndex = 0xffffffffu;
constant float kPointLightSourceRadiusSq = 0.25f;
constant float kMinRough = 0.04f;
constant float kDefaultDiffuseRoughness = 1.0f;
constant float kSurfelGIAlbedoBoostExp = 1.0f;
constant uint  kDefaultRayStep = 3u, kDefaultMaxStep = 6u;
constant uint  kSolidMaterialCapacity = 2048u;
constant float kCellUnit = 0.5f;
constant uint  kLightGridDim = 128u;
constant float kLightGridUnit = 0.9765625f;
constant uint  kLightsPerCellMax = 128u;
constant uint  kFinalizeCellBudget = 64u;
constant float kFinalizeBouncePr = 0.2f;
constant uint  kSleepingSpawnCellCap = 8u;
constant float kSleepingCullCoverage = 4.0f;
constant uint  kSurfelCounterValid = 0u, kSurfelCounterFree = 2u, kSurfelCounterRequestedRay = 4u, kSurfelCounterMissBounce = 5u;
constant uint  kMaterialFlagEnableSpecular = 1u << 2;
constant uint  kMaterialFlagEnableDissolveAlpha = 1u << 7;
constant uint  kMaterialFlagIsAlphaSingleChannel = 1u << 10;
constant uint  kMaterialFlagUseDissolveFilter = 1u << 14;

// --- Scalar struct layouts (packed types) -----------------------------------
// --- Shared struct layouts: mirrored from the Slang modules (flattened in
// at stage time by cmake/flatten_metal_includes.py). RT-unique structs
// (DiffuseMaterial / LightSample / DiffuseShading / SurfelHit) stay below.
#include "SceneTypes.metalh"            // SceneConstants, lights, MatStorage, UniformObject, TriangleHit, VertexData
#include "SurfelGI/SurfelTypes.metalh"  // Surfel, CellInfo, SurfelRayResult, SurfelRecycleInfo (+ MSMEData)
#include "Random.metalh"                // RNG + rng_*
#include "SurfelGI/SurfelUtils.metalh"  // getCellPos/isCellValid/getFlattenCellIndex, get_tangentspace, hemispherepoint_uniform (+ M_PI_, kCellDimension)
struct DiffuseMaterial { uint type; uint materialConfig; uint cubeMapTextureIndex; uint tex[8]; float heightMapScale, heightMapBias, frenselBias, frenselPow; };
struct LightSample { float3 Li; float pdf; float3 dir; float distance; };
struct DiffuseShading { float3 posW; float3 normalW; float3 albedo; float3 emission; float rough; };
// SurfelHit is provided by SurfelGI/SurfelGather.metalh (included after Set1).

// NOTE: Metal argument-buffer arrays consume *consecutive* [[id]] slots, so the
// two 16384-element texture arrays cannot sit at the Vulkan kBinding ids 0/1
// (they would overlap the buffers at id 3..48). They are placed AFTER the
// non-array resources: gTextures2D at id(49) (49..16432), gTexturesCube at
// id(16433) (16433..32816). The C++ bindless argument encoder must remap these
// two arrays to the same Metal ids (everything else keeps id == kBinding*).
// Members MUST be in ascending [[id]] order (Metal arg-buffer rule), and the
// two 16384-element texture arrays consume consecutive ids, so they go LAST:
// gTextures2D at id(49) (49..16432), gTexturesCube at id(16433) (16433..32816).
// The C++ bindless encoder must remap those two arrays to the same Metal ids;
// every other member keeps id == kBinding*.
struct Set0 {
    device ulong*          gOpaquePositionHandles [[id(3)]];
    device ulong*          gOpaqueTangentHandles  [[id(4)]];
    device ulong*          gOpaqueNormalHandles   [[id(5)]];
    device ulong*          gOpaqueUv0Handles      [[id(6)]];
    device ulong*          gOpaqueIndexHandles    [[id(8)]];
    sampler                gMaterialSampler       [[id(9)]];
    device uint*           gSurfelCounter         [[id(10)]];
    device Surfel*         gSurfelBuffer          [[id(11)]];
    device uint4*          gSurfelGeometryBuffer  [[id(12)]];
    device uint*           gSurfelValidIndexBuffer [[id(13)]];
    device uint*           gSurfelFreeIndexBuffer [[id(15)]];
    device SurfelRecycleInfo* gSurfelRecycleInfoBuffer [[id(16)]];
    device SurfelRayResult* gSurfelRayResultBuffer [[id(17)]];
    device CellInfo*       gCellInfoBuffer        [[id(18)]];
    device uint*           gCellToSurfelBuffer    [[id(19)]];
    device UniformObject*  gSceneObjects          [[id(20)]];
    device DiffuseMaterial* gDiffuseMaterials     [[id(21)]];
    device PointLight*     gPointLights           [[id(22)]];
    device uint*           gSurfelRefCounter      [[id(27)]];
    device uint*           gSurfelReservationBuffer [[id(28)]];
    device SpotLight*      gSpotLights            [[id(29)]];
    sampler                gSurfelDepthSampler    [[id(35)]];
    device uint*           gBindlessSlotGeneration [[id(38)]];
    device uint*           gSurfelSlotGeneration  [[id(39)]];
    device uint*           gLightGridCount        [[id(40)]];
    device uint*           gLightGridList         [[id(41)]];
    texture2d<float>       gAttenuationLut        [[id(45)]];
    texture2d<float>       gDissolveMap           [[id(48)]];
    array<texture2d<float>, 16384>   gTextures2D   [[id(49)]];     // kBindingTextures2D (remapped)
    array<texturecube<float>, 16384> gTexturesCube [[id(16433)]];  // kBindingTexturesCube (remapped)
};
// Set-1 program argument buffer (buffer(1)); [[id]] = VK binding within set 1.
// The TLAS rides in set 1 at kBindingTlas(36) so it flows through the same
// per-set bindDescriptors path (setAccelerationStructure) as the other
// program-managed resources.
struct Set1 {
    constant SceneConstants* gPerFrame [[id(0)]];
    instance_acceleration_structure tlas [[id(1)]];   // mtl.index in kSurfelRT
};

// Utility headers that reference `device Set0&` must come AFTER struct Set0.
#include "SurfelGI/BindlessTriangle.metalh"  // TriVtx, loadMat, getVertexData, packTriangleHit
#include "SurfelGI/SurfelGather.metalh"      // SurfelHit, testSurfelCoverage, surfelKernelWeight, lookupSurfelCell
#include "SurfelGI/SurfelLifecycle.metalh"   // makeMSMEData, makeSurfel, tryAllocateSurfelSlot, publishNewSurfel (+ kTotalSurfelLimit, kObjectSlotCapacity, kMaxLife)

// --- light-grid cell math (pass-local; the gridDim variant takes an explicit
// dimension, distinct from the surfel-cell helpers in SurfelUtils.metalh) -----
static bool  gridCellValid(int3 c, uint dim) { int h=int(dim/2u); return abs(c.x)<h && abs(c.y)<h && abs(c.z)<h; }
static int3  gridCellPos(float3 p, float3 c, float gu) { return int3(round((p-c)/gu)); }
static uint  gridFlattenCellIndex(int3 c, uint dim) { uint3 u=uint3(c+int3(int(dim/2u))); return u.z*dim*dim+u.y*dim+u.x; }
static float3 sampleCosineHemisphere(float u1, float u2, thread float& pdf) { float r=sqrt(u1); float phi=2.0f*M_PI_*u2; float3 d; d.x=r*cos(phi); d.y=r*sin(phi); d.z=sqrt(max(0.0f,1.0f-u1)); pdf=d.z*kInvPi; return d; }
static float3x3 buildTangentSpace(float3 n) { float3 h=abs(n.x)<0.9f?float3(1,0,0):float3(0,1,0); float3 t=normalize(cross(h,n)); float3 b=cross(n,t); return float3x3(t,b,n); }

// --- BRDF --------------------------------------------------------------------
static float fresnelSchlickF(float f0, float f90, float cosTheta) { float m=clamp(1.0f-cosTheta,0.0f,1.0f); float m2=m*m; return f0+(f90-f0)*(m2*m2*m); }
static float diffuseLobeWeight(float NoL, float NoV, float LoH, float rough) {
    float energyBias=mix(0.0f,0.5f,rough); float energyFactor=mix(1.0f,1.0f/1.51f,rough);
    float fd90=energyBias+2.0f*LoH*LoH*rough;
    return fresnelSchlickF(1.0f,fd90,NoL)*fresnelSchlickF(1.0f,fd90,NoV)*energyFactor;
}
static float evalDiffuseDemodulated(float3 N, float3 V, float3 L, float rough) {
    float NoL=saturate(dot(N,L)); float NoV=max(dot(N,V),1e-4f); float3 H=normalize(V+L); float LoH=saturate(dot(L,H));
    return diffuseLobeWeight(NoL,NoV,LoH,rough)*(1.0f/kBrdfPi);
}
static float glossToGGXAlpha(float gloss) { float n=exp2(gloss*10.0f)+1.0f; return sqrt(2.0f/(n+2.0f)); }
static float decodeMaterialRoughness(DiffuseMaterial m, float2 uv, device Set0& set0) {
    bool hasSpec=((m.materialConfig & kMaterialFlagEnableSpecular)!=0u) && (m.tex[3]!=kInvalidTextureIndex);
    if (!hasSpec) return kDefaultDiffuseRoughness;
    float gloss=set0.gTextures2D[m.tex[3]].sample(set0.gMaterialSampler, uv, level(0.0f)).y;
    return clamp(sqrt(glossToGGXAlpha(gloss)), kMinRough, 1.0f);
}

// loadMat / TriVtx / getVertexData are provided by SurfelGI/BindlessTriangle.metalh.

// --- material / lighting -----------------------------------------------------
static DiffuseShading prepareDiffuseShading(VertexData v, uint materialID, float illuminationAmount, device Set0& set0) {
    DiffuseShading sd; sd.posW=v.posW; sd.normalW=normalize(v.normalW);
    DiffuseMaterial m=set0.gDiffuseMaterials[materialID];
    uint diffuseTex=m.tex[0];
    sd.albedo = (diffuseTex==kInvalidTextureIndex) ? float3(0.5f)
              : set0.gTextures2D[diffuseTex].sample(set0.gMaterialSampler, v.texC, level(0.0f)).rgb;
    uint illumTex=m.tex[5];
    sd.emission = (illumTex==kInvalidTextureIndex || illuminationAmount<=0.0f) ? float3(0.0f)
                : set0.gTextures2D[illumTex].sample(set0.gMaterialSampler, v.texC, level(0.0f)).rgb * illuminationAmount;
    sd.rough = decodeMaterialRoughness(m, v.texC, set0);
    return sd;
}
static float sampleSpotCone(float cosTheta, float cosOuterAngle, device Set0& set0) {
    float u=(1.0f-cosTheta)/max(1.0f-cosOuterAngle,1e-6f);
    return set0.gAttenuationLut.sample(set0.gSurfelDepthSampler, float2(saturate(u),0.5f), level(0.0f)).r;
}
static bool sampleLight(uint lightIndex, float3 shadingPosW, thread LightSample& ls, device Set0& set0, constant SceneConstants* gPerFrame) {
    ls.Li=float3(0.0f); ls.pdf=1.0f; ls.dir=float3(0,0,1); ls.distance=0.0f;
    uint nPoint=gPerFrame->pointLightCount;
    if (lightIndex < nPoint) {
        PointLight pl=set0.gPointLights[lightIndex];
        float3 toLight=float3(pl.position)-shadingPosW; float dist=length(toLight); if (dist<=0.0f) return false;
        float3 L=toLight/dist; float atten=1.0f/(dist*dist+kPointLightSourceRadiusSq);
        float3 gobo=float3(1.0f);
        if (pl.goboTextureIndex != kInvalidTextureIndex) {
            float3x3 wToL=float3x3(float3(pl.worldToLightX),float3(pl.worldToLightY),float3(pl.worldToLightZ));
            float3 lld=wToL * (-L);
            gobo=set0.gTexturesCube[pl.goboTextureIndex].sample(set0.gMaterialSampler, lld, level(0.0f)).rgb;
        }
        ls.Li=float3(pl.color)*pl.intensity*gobo*atten; ls.dir=L; ls.distance=dist; return true;
    }
    SpotLight sl=set0.gSpotLights[lightIndex-nPoint];
    float3 toLight=float3(sl.position)-shadingPosW; float dist=length(toLight); if (dist<=0.0f) return false;
    float3 L=toLight/dist; float atten=1.0f/(dist*dist+kPointLightSourceRadiusSq);
    float3 gobo=float3(1.0f); float cone=1.0f;
    if (sl.goboTextureIndex != kInvalidTextureIndex) {
        float4 lc=loadMat(sl.viewProjection)*float4(shadingPosW,1.0f); if (lc.w<=0.0f) return false;
        float2 uv=lc.xy/lc.w; if (any(uv<float2(0.0f)) || any(uv>float2(1.0f))) return false;
        gobo=set0.gTextures2D[sl.goboTextureIndex].sample(set0.gMaterialSampler, uv, level(0.0f)).rgb;
    } else {
        float cosTheta=dot(-L, float3(sl.direction)); if (cosTheta < sl.cosOuterAngle) return false;
        cone=sampleSpotCone(cosTheta, sl.cosOuterAngle, set0);
    }
    ls.Li=float3(sl.color)*sl.intensity*gobo*cone*atten; ls.dir=L; ls.distance=dist; return true;
}

// alphaTest — true == fragment is transparent (ignore the hit).
static bool alphaTest(VertexData v, uint materialID, float lod, float dissolveAmount, device Set0& set0) {
    if (materialID >= kSolidMaterialCapacity) return false;
    DiffuseMaterial m=set0.gDiffuseMaterials[materialID];
    float a=1.0f; uint alphaIdx=m.tex[2];
    if (alphaIdx != kInvalidTextureIndex) {
        float4 s=set0.gTextures2D[alphaIdx].sample(set0.gMaterialSampler, v.texC, level(lod));
        a=((m.materialConfig & kMaterialFlagIsAlphaSingleChannel)!=0u) ? s.r : s.a;
    } else if (dissolveAmount>=1.0f && (m.materialConfig & (kMaterialFlagEnableDissolveAlpha|kMaterialFlagUseDissolveFilter))==0u) {
        return false;
    }
    bool hasDissolveAlpha=(m.materialConfig & kMaterialFlagEnableDissolveAlpha)!=0u;
    bool hasDissolveFilter=(m.materialConfig & kMaterialFlagUseDissolveFilter)!=0u;
    if (dissolveAmount<1.0f || hasDissolveAlpha || hasDissolveFilter) {
        float d=set0.gDissolveMap.sample(set0.gMaterialSampler, v.texC, level(0.0f)).x;
        if (hasDissolveAlpha) {
            d=d*0.25f+0.75f; uint dat=m.tex[6]; float da=1.0f;
            if (dat!=kInvalidTextureIndex) da=set0.gTextures2D[dat].sample(set0.gMaterialSampler, v.texC, level(lod)).r;
            d -= (0.25f - da*0.25f);
        } else { d=d*0.5f+0.5f; }
        a=d-(1.0f-dissolveAmount*a)*0.5f;
    }
    return a < 0.5f;
}

// --- ray-query traversal -----------------------------------------------------
static bool traceClosest(float3 origin, float3 dir, instance_acceleration_structure tlas, device Set0& set0, thread TriangleHit& outHit) {
    ray r; r.origin=origin; r.direction=dir; r.min_distance=0.0f; r.max_distance=1e30f;
    intersection_query<triangle_data, instancing> q;
    q.reset(r, tlas, 0xFFu);
    while (q.next()) {
        if (q.get_candidate_intersection_type()==intersection_type::triangle) {
            TriangleHit h; h.instanceID=q.get_candidate_instance_id(); h.primitiveIndex=q.get_candidate_primitive_id(); h.barycentrics=q.get_candidate_triangle_barycentric_coord();
            VertexData v=getVertexData(h, set0);
            uint mat=set0.gSceneObjects[h.instanceID].materialID;
            float dissolve=set0.gSceneObjects[h.instanceID].dissolveAmount;
            if (!alphaTest(v, mat, 0.0f, dissolve, set0))
                q.commit_triangle_intersection();
        }
    }
    if (q.get_committed_intersection_type() != intersection_type::triangle) return false;
    outHit.instanceID=q.get_committed_instance_id(); outHit.primitiveIndex=q.get_committed_primitive_id(); outHit.barycentrics=q.get_committed_triangle_barycentric_coord();
    return true;
}
// Returns true == visible (unoccluded).
static bool traceShadowRay(float3 origin, float3 dir, float dist, instance_acceleration_structure tlas, device Set0& set0) {
    ray r; r.origin=origin; r.direction=dir; r.min_distance=0.0f; r.max_distance=dist;
    intersection_query<triangle_data, instancing> q;
    q.reset(r, tlas, 0xFFu);
    while (q.next()) {
        if (q.get_candidate_intersection_type()==intersection_type::triangle) {
            TriangleHit h; h.instanceID=q.get_candidate_instance_id(); h.primitiveIndex=q.get_candidate_primitive_id(); h.barycentrics=q.get_candidate_triangle_barycentric_coord();
            VertexData v=getVertexData(h, set0);
            uint mat=set0.gSceneObjects[h.instanceID].materialID;
            float dissolve=set0.gSceneObjects[h.instanceID].dissolveAmount;
            if (!alphaTest(v, mat, 0.0f, dissolve, set0)) return false;
        }
    }
    return true;
}
static float3 shadeAnalyticLight(float3 posW, float3 normalW, float3 V, float rough, uint lightIndex, float3 origin,
                                 instance_acceleration_structure tlas, device Set0& set0, constant SceneConstants* gPerFrame) {
    LightSample ls;
    if (!sampleLight(lightIndex, posW, ls, set0, gPerFrame)) return float3(0.0f);
    float NdotL=dot(normalW, ls.dir); if (NdotL<=0.0f) return float3(0.0f);
    float3 contrib=ls.Li*NdotL*evalDiffuseDemodulated(normalW, V, ls.dir, rough);
    if (!traceShadowRay(origin, ls.dir, ls.distance, tlas, set0)) return float3(0.0f);
    return contrib;
}

// makeMSMEData / makeSurfel / tryAllocateSurfelSlot / publishNewSurfel are
// provided by SurfelGI/SurfelLifecycle.metalh; packTriangleHit by BindlessTriangle.metalh.

// --- payload + path tracer ---------------------------------------------------
struct ScatterPayload { float3 radiance; float3 thp; float3 origin; float3 direction; float firstRayLength; uint currStep; uint status; uint rngHi; uint rngLo; };
static ScatterPayload makePayload(uint2 rngState, bool isSleeping) {
    ScatterPayload p; p.radiance=float3(0.0f); p.thp=float3(1.0f); p.origin=float3(0.0f); p.direction=float3(0.0f);
    p.firstRayLength=0.0f; p.currStep=1u; p.status=isSleeping?0x0001u:0x0000u; p.rngHi=rngState.x; p.rngLo=rngState.y; return p;
}
struct SurfelCacheGather { float4 Lr; float sleepingCoverage; int loudestSleepingSurfel; };
static SurfelCacheGather gatherCellRadiance(CellInfo cellInfo, float3 posW, float3 normalW, device Set0& set0) {
    SurfelCacheGather g; g.Lr=float4(0.0f); g.sleepingCoverage=0.0f; g.loudestSleepingSurfel=-1; float maxContribution=0.0f;
    for (uint i=0u;i<cellInfo.surfelCount;++i) {
        uint surfelIndex=set0.gCellToSurfelBuffer[cellInfo.cellToSurfelBufferOffset+i];
        Surfel surfel=set0.gSurfelBuffer[surfelIndex];
        SurfelHit hit;
        if (!testSurfelCoverage(posW, normalW, float3(surfel.position), normalize(float3(surfel.normal)), surfel.radius, hit)) continue;
        float contribution=surfelKernelWeight(hit.dotN, hit.dist, surfel.radius);
        g.Lr += float4(float3(surfel.radiance),1.0f)*contribution;
        SurfelRecycleInfo info=set0.gSurfelRecycleInfoBuffer[surfelIndex];
        if ((info.status & 0x0001u)!=0u) { g.sleepingCoverage+=contribution; if (maxContribution<contribution){ maxContribution=contribution; g.loudestSleepingSurfel=int(surfelIndex);} }
        atomic_fetch_add_explicit((device atomic_uint*)&set0.gSurfelRefCounter[surfelIndex], 1u, memory_order_relaxed);
    }
    return g;
}
static void countMissBounce(device Set0& set0) { atomic_fetch_add_explicit((device atomic_uint*)&set0.gSurfelCounter[kSurfelCounterMissBounce], 1u, memory_order_relaxed); }
static void trySpawnSleepingSurfel(uint flattenIndex, CellInfo cellInfo, VertexData v, TriangleHit triangleHit, device Set0& set0) {
    if (cellInfo.surfelCount>=kSleepingSpawnCellCap || triangleHit.instanceID>=kObjectSlotCapacity) return;
    uint reserved=atomic_fetch_add_explicit((device atomic_uint*)&set0.gSurfelReservationBuffer[flattenIndex], 1u, memory_order_relaxed);
    if (reserved>=kSleepingSpawnCellCap) return;
    uint newIndex, validSlot; if (!tryAllocateSurfelSlot(newIndex, validSlot, set0)) return;
    Surfel ns=makeSurfel(v.posW, v.normalW, 1e-6f);
    SurfelRecycleInfo recycle; recycle.life=1; recycle.frame=0; recycle.status=1;
    publishNewSurfel(newIndex, validSlot, ns, recycle, packTriangleHit(triangleHit), triangleHit.instanceID, 1u, set0);
}
static bool finalize(thread ScatterPayload& sp, VertexData v, TriangleHit triangleHit, thread RNG& rng, instance_acceleration_structure tlas, device Set0& set0, constant SceneConstants* gPerFrame) {
    uint flattenIndex; CellInfo cellInfo;
    if (!lookupSurfelCell(v.posW, gPerFrame->posW, flattenIndex, cellInfo, set0.gCellInfoBuffer)) return false;
    if (cellInfo.surfelCount>kFinalizeCellBudget || rng_next_float(rng)<kFinalizeBouncePr) { countMissBounce(set0); return false; }
    SurfelCacheGather g=gatherCellRadiance(cellInfo, v.posW, v.normalW, set0);
    if (g.Lr.w<=0.0f) { trySpawnSleepingSurfel(flattenIndex, cellInfo, v, triangleHit, set0); countMissBounce(set0); return false; }
    if (g.sleepingCoverage>=kSleepingCullCoverage && g.loudestSleepingSurfel!=-1) {
        Surfel s=set0.gSurfelBuffer[g.loudestSleepingSurfel]; s.radius=0.0f; set0.gSurfelBuffer[g.loudestSleepingSurfel]=s;
    }
    sp.radiance += sp.thp * (g.Lr.xyz/g.Lr.w);
    return true;
}
static void handleHit(TriangleHit triangleHit, thread ScatterPayload& sp, instance_acceleration_structure tlas, device Set0& set0, constant SceneConstants* gPerFrame) {
    VertexData v=getVertexData(triangleHit, set0);
    uint materialID=set0.gSceneObjects[triangleHit.instanceID].materialID;
    float illuminationAmount=set0.gSceneObjects[triangleHit.instanceID].illuminationAmount;
    DiffuseShading sd=prepareDiffuseShading(v, materialID, illuminationAmount, set0);

    if (sp.currStep==1u) sp.firstRayLength=distance(sp.origin, v.posW);
    sp.radiance += sp.thp * sd.emission;

    RNG rng; rng.s=uint2(sp.rngHi, sp.rngLo);
    float3 giAlbedo=pow(sd.albedo, float3(kSurfelGIAlbedoBoostExp));

    int3 lcell=gridCellPos(sd.posW, gPerFrame->posW, kLightGridUnit);
    uint lightCount=gPerFrame->pointLightCount+gPerFrame->spotLightCount;
    if (lightCount>0u && gridCellValid(lcell, kLightGridDim)) {
        uint lflat=gridFlattenCellIndex(lcell, kLightGridDim);
        uint cnt=min(set0.gLightGridCount[lflat], kLightsPerCellMax);
        if (cnt!=0u) {
            float3 Vd=-sp.direction;
            uint pick=rng_next_uint(rng, cnt);
            uint lightIdx=set0.gLightGridList[lflat*kLightsPerCellMax+pick];
            float invPdf=float(cnt);
            float3 origin=sd.posW + sd.normalW*kRayEps;
            float3 perLight=giAlbedo*shadeAnalyticLight(sd.posW, sd.normalW, Vd, sd.rough, lightIdx, origin, tlas, set0, gPerFrame);
            sp.radiance += sp.thp*(perLight*invPdf);
        }
    }

    float u1=rng_next_float(rng), u2=rng_next_float(rng); float bsdfPdf;
    float3 localDir=sampleCosineHemisphere(u1, u2, bsdfPdf);
    float3 worldDir=normalize(localDir * buildTangentSpace(sd.normalW));   // mul(localDir, basis)
    float bounceW;
    {
        float3 Vb=-sp.direction; float3 Hb=normalize(Vb+worldDir);
        bounceW=diffuseLobeWeight(saturate(dot(sd.normalW, worldDir)), max(dot(sd.normalW, Vb), 1e-4f), saturate(dot(worldDir, Hb)), sd.rough);
    }
    sp.origin=sd.posW + sd.normalW*kRayEps; sp.direction=worldDir; sp.thp *= giAlbedo*bounceW;

    uint rayStep=((sp.status & 0x0001u)!=0u) ? kDefaultRayStep*2u : kDefaultRayStep;
    if (sp.currStep<rayStep) {
        float rrValue=dot(sp.thp, float3(0.2126f,0.7152f,0.0722f)); float prob=max(0.0f, 1.0f-rrValue);
        if (rng_next_float(rng)>=prob) {
            sp.thp /= max(1e-12f, 1.0f-prob);
            if (any(sp.thp>0.0f)) sp.currStep++; else sp.status |= 0x0002u;
            sp.rngHi=rng.s.x; sp.rngLo=rng.s.y; return;
        }
    }
    if (finalize(sp, v, triangleHit, rng, tlas, set0, gPerFrame)) sp.status |= 0x0002u; else sp.currStep++;
    sp.rngHi=rng.s.x; sp.rngLo=rng.s.y;
}

[[kernel]] void rayGen(uint3 tid [[thread_position_in_grid]],
                       device Set0& set0 [[buffer(0)]],
                       device Set1& set1 [[buffer(1)]]) {
    constant SceneConstants* gPerFrame = set1.gPerFrame;
    instance_acceleration_structure tlas = set1.tlas;
    uint totalRayCount = atomic_load_explicit((device atomic_uint*)&set0.gSurfelCounter[kSurfelCounterRequestedRay], memory_order_relaxed);
    if (tid.x >= totalRayCount) return;

    uint rayIndex=tid.x;
    SurfelRayResult srr=set0.gSurfelRayResultBuffer[rayIndex];
    uint surfelIndex=srr.surfelIndex;
    Surfel surfel=set0.gSurfelBuffer[surfelIndex];
    SurfelRecycleInfo info=set0.gSurfelRecycleInfoBuffer[surfelIndex];
    bool isSleeping=(info.status & 0x0001u)!=0u;

    RNG rng; rng_init(rng, uint2(rayIndex, rayIndex), gPerFrame->totalFrames);

    float3 dirLocal=normalize(hemispherepoint_uniform(rng_next_float(rng), rng_next_float(rng)));
    float  pdfLocal=kInvPi*0.5f;   // 1/(2pi)
    float3 dirWorld=normalize(dirLocal * get_tangentspace(float3(surfel.normal)));   // mul(dirLocal, basis)

    ScatterPayload payload=makePayload(rng.s, isSleeping);
    payload.origin=float3(surfel.position); payload.direction=dirWorld;

    srr.dirLocal=dirLocal; srr.dirWorld=dirWorld; srr.pdf=pdfLocal;

    uint maxStep=isSleeping ? kDefaultMaxStep*2u : kDefaultMaxStep;
    while (((payload.status & 0x0002u)==0u) && payload.currStep<=maxStep) {
        TriangleHit hit;
        if (traceClosest(payload.origin, payload.direction, tlas, set0, hit))
            handleHit(hit, payload, tlas, set0, gPerFrame);
        else
            payload.status |= 0x0002u;
    }

    srr.firstRayLength=payload.firstRayLength; srr.radiance=payload.radiance;
    set0.gSurfelRayResultBuffer[rayIndex]=srr;
}
