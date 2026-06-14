// Hand-written Metal port of amnesia/slang/SurfelVBuffer/SurfelVBuffer.rt.slang
//
// Slang cannot lower the DXR ray-tracing model (TraceRay + a raygeneration /
// miss / anyhit / closesthit shader table) to Metal, so this pass is authored by
// hand against Metal's INLINE ray-query model — the same model the completed
// reference SurfelGI/SurfelRayTrace.rt.metal uses:
//
//   metal::raytracing::intersection_query drives the trace from a COMPUTE
//   kernel. There is no intersection_function_table and there are no
//   [[intersection]] any-hit functions: the alpha-test "any hit" gate runs
//   INLINE inside the candidate-traversal loop, and "closest hit" / "miss" are
//   just the branches taken after the loop (committed-triangle vs. none).
//
// DXR entry-point  ->  Metal mapping (see the .rt.slang for the real logic):
//   raygeneration (raygen, line 293) -> the `rayGen` kernel below; seeds one
//        primary ray per pixel from the camera and writes gPackedHitInfo.
//   miss          (miss,   line  57) -> the `!hit` branch in the kernel (clears
//        gPackedHitInfo only for the primary ray, kind 0 / length 0).
//   anyhit        (anyHit, line  73) -> the `alphaTest` call inside the
//        `traceClosest` candidate loop (commit when alpha passes, skip otherwise).
//   closesthit    (closeHit,line  91) -> the committed-hit branch in the kernel;
//        packs (instanceID, primitiveIndex, barycentrics) into gPackedHitInfo,
//        then parallax-perturbs barycentrics and re-traces water/glass refraction
//        as a single kind-0 bounce (Metal has no recursive TraceRay, so the
//        single slang bounce length 0->1 is implemented as one inline re-trace).
//
// Self-contained at stage time: cmake/flatten_metal_includes.py inlines the
// quoted #includes below. The bindless Set0 argument buffer ([[id]] == VK
// kBinding*) and Set1 (gPerFrame / tlas / gPackedHitInfo) match the layout the
// RI Metal backend encodes. The engine's Metal RT runtime pipeline is a
// follow-up; this compiles standalone today.

#include <metal_stdlib>
#include <metal_raytracing>
using namespace metal;
using namespace raytracing;

// --- shared constants (mirrored from amnesia/slang/Constants.h) --------------
constant uint  kInvalidTextureIndex = 0xffffffffu;
constant uint  kSolidMaterialCapacity = 2048u;
constant uint  kTranslucentMaterialCapacity = 256u;
constant uint  kTranslucentMaterialBase = kSolidMaterialCapacity;                       // 2048
constant uint  kWaterMaterialBase = kSolidMaterialCapacity + kTranslucentMaterialCapacity; // 2304

constant uint  kMaterialFlagEnableHeight             = 1u << 4;
constant uint  kMaterialFlagEnableDissolveAlpha      = 1u << 7;
constant uint  kMaterialFlagIsHeightMapSingleChannel = 1u << 9;
constant uint  kMaterialFlagIsAlphaSingleChannel     = 1u << 10;
constant uint  kMaterialFlagUseDissolveFilter        = 1u << 14;
constant uint  kMaterialFlagHasRefraction            = 1u << 16;

constant float kParalaxScale = 0.4f;
constant float kWaterRefractionIntensity = 0.6f;

// --- Shared struct layouts: mirrored from the Slang modules (flattened in at
// stage time by cmake/flatten_metal_includes.py). ---
#include "SceneTypes.metalh"            // SceneConstants, MatStorage, UniformObject, TriangleHit, VertexData, HitInfo

// Material tables (mirror amnesia/slang/SceneTypes.slang DiffuseMaterial /
// TranslucentMaterial / WaterMaterial — scalar field order verbatim).
struct DiffuseMaterial { uint type; uint materialConfig; uint cubeMapTextureIndex; uint tex[8]; float heightMapScale, heightMapBias, frenselBias, frenselPow; };
struct TranslucentMaterial { uint type; uint materialConfig; uint cubeMapTextureIndex; uint tex[8]; float refractionScale, frenselBias, frenselPow, rimLightMul, rimLightPow; };
struct WaterMaterial { uint type; uint materialConfig; uint tex[8]; float refractionScale, frenselBias, frenselPow, reflectionFadeStart, reflectionFadeEnd, waveSpeed, waveAmplitude, waveFreq; };

// Set-0 bindless argument buffer (buffer(0)); [[id]] == VK kBinding* (matches
// SurfelRayTrace.rt.metal). Members MUST be in ascending [[id]] order, and the
// two 16384-element texture arrays consume consecutive ids so they go LAST:
// gTextures2D at id(49) (49..16432); the C++ bindless encoder remaps it to
// kBindingTextures2D. Every other member keeps id == kBinding*.
struct Set0 {
    device ulong*           gOpaquePositionHandles  [[id(3)]];
    device ulong*           gOpaqueTangentHandles   [[id(4)]];
    device ulong*           gOpaqueNormalHandles    [[id(5)]];
    device ulong*           gOpaqueUv0Handles       [[id(6)]];
    device ulong*           gOpaqueIndexHandles     [[id(8)]];
    sampler                 gMaterialSampler        [[id(9)]];
    device UniformObject*   gSceneObjects           [[id(20)]];
    device DiffuseMaterial* gDiffuseMaterials       [[id(21)]];
    device TranslucentMaterial* gTranslucentMaterials [[id(24)]];
    device WaterMaterial*   gWaterMaterials         [[id(25)]];
    texture2d<float>        gDissolveMap            [[id(48)]];
    array<texture2d<float>, 16384> gTextures2D      [[id(49)]];  // kBindingTextures2D (remapped)
};

// Set-1 program argument buffer (buffer(1)). The TLAS rides at id(1) exactly
// like SurfelRayTrace.rt.metal (the RI Metal backend binds it via
// setAccelerationStructure). gPackedHitInfo is the V-buffer output image — the
// consumer passes (SurfelGenerationPass / SurfelEvaluationPass) read it as
// texture2d<uint, access::read>, so it is the matching write view here.
struct Set1 {
    constant SceneConstants*           gPerFrame      [[id(0)]];
    instance_acceleration_structure    tlas           [[id(1)]];
    texture2d<uint, access::write>     gPackedHitInfo [[id(2)]];
};

// Utility headers that reference `device Set0&` must come AFTER struct Set0.
#include "SurfelGI/BindlessTriangle.metalh"  // TriVtx, loadMat, getVertexData, fetchBindlessTriangle, packTriangleHit

// --- vertex data + triangle (mirror Scene.getVertexDataAndTri, Scene.slang:565)
// getVertexData (in BindlessTriangle.metalh) fetches the triangle once and
// discards it; parallax needs the raw per-vertex UVs to invert the UV offset
// back into a barycentric delta, so this returns both (same single BDA fetch).
static VertexData getVertexDataAndTri(TriangleHit hit, thread TriVtx& vtx, device Set0& set0) {
    fetchBindlessTriangle(hit.instanceID, hit.primitiveIndex, vtx, set0);

    float b1 = hit.barycentrics.x, b2 = hit.barycentrics.y, b0 = 1.0f - b1 - b2;
    float3 normalL = vtx.normalL[0] * b0 + vtx.normalL[1] * b1 + vtx.normalL[2] * b2;
    float3 tanL    = vtx.tangentL[0].xyz * b0 + vtx.tangentL[1].xyz * b1 + vtx.tangentL[2].xyz * b2;
    float2 uv      = vtx.uv[0] * b0 + vtx.uv[1] * b1 + vtx.uv[2] * b2;

    float4x4 M = loadMat(set0.gSceneObjects[hit.instanceID].modelMat);
    float3x3 nrmMat = float3x3(M[0].xyz, M[1].xyz, M[2].xyz);

    VertexData v;
    v.posW = (float4(vtx.posL[0] * b0 + vtx.posL[1] * b1 + vtx.posL[2] * b2, 1.0f) * M).xyz;
    v.normalW  = normalize(normalL * nrmMat);
    v.tangentW = normalize(tanL * nrmMat);
    v.texC = uv;
    return v;
}

// --- Scene material accessors (mirror Scene.slang) ---------------------------
static uint getMaterialID(uint objectSlot, device Set0& set0) { return set0.gSceneObjects[objectSlot].materialID; }
static DiffuseMaterial getDiffuseMaterial(uint materialID, device Set0& set0) { return set0.gDiffuseMaterials[materialID]; }
static TranslucentMaterial getTranslucentMaterial(uint materialID, device Set0& set0) { return set0.gTranslucentMaterials[materialID - kTranslucentMaterialBase]; }
static WaterMaterial getWaterMaterial(uint materialID, device Set0& set0) { return set0.gWaterMaterials[materialID - kWaterMaterialBase]; }
// SceneMaterials.isTranslucent / isWater / isRefractive (Scene.slang:202/227/214).
static bool isTranslucent(uint materialID) { return materialID >= kTranslucentMaterialBase && materialID < kWaterMaterialBase; }
static bool isWater(uint materialID) { return materialID >= kWaterMaterialBase; }
static bool isRefractive(uint materialID, device Set0& set0) {
    if (!isTranslucent(materialID)) return false;
    TranslucentMaterial m = set0.gTranslucentMaterials[materialID - kTranslucentMaterialBase];
    return (m.materialConfig & kMaterialFlagHasRefraction) != 0u;
}

// --- alphaTest (mirror SceneMaterials.alphaTest, Scene.slang:142 — identical to
// SurfelRayTrace.rt.metal). true == fragment is transparent (ignore the hit). --
static bool alphaTest(VertexData v, uint materialID, float lod, float dissolveAmount, device Set0& set0) {
    if (materialID >= kSolidMaterialCapacity) return false;
    DiffuseMaterial m = set0.gDiffuseMaterials[materialID];
    float a = 1.0f; uint alphaIdx = m.tex[2];
    if (alphaIdx != kInvalidTextureIndex) {
        float4 s = set0.gTextures2D[alphaIdx].sample(set0.gMaterialSampler, v.texC, level(lod));
        a = ((m.materialConfig & kMaterialFlagIsAlphaSingleChannel) != 0u) ? s.r : s.a;
    } else if (dissolveAmount >= 1.0f && (m.materialConfig & (kMaterialFlagEnableDissolveAlpha | kMaterialFlagUseDissolveFilter)) == 0u) {
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

// --- parallax occlusion mapping (mirror amnesia/slang/Parallax.slang) ---------
static float sampleHeight(uint heightTexIdx, float2 uv, bool singleChannel, device Set0& set0) {
    float4 s = set0.gTextures2D[heightTexIdx].sample(set0.gMaterialSampler, uv, level(0.0f));
    return singleChannel ? s.r : s.a;
}
static float2 parallaxOcclusionUV(uint heightTexIdx, float heightMapScale, bool singleChannel,
                                  float3 worldViewDir, float3 N, float3 T, float3 B, float2 baseUv,
                                  device Set0& set0) {
    if (heightTexIdx == kInvalidTextureIndex) return baseUv;

    float3 eye = float3(dot(worldViewDir, T), dot(worldViewDir, B), dot(-worldViewDir, N));
    // !(eye.z > k) so a NaN eye.z also bails (see slang for the rationale).
    if (!(eye.z > 1.0e-3f)) return baseUv;

    eye *= (1.0f / eye.z);
    eye.xy *= heightMapScale;

    int nSteps = (int)floor((1.0f - eye.z / max(length(eye), 1.0e-6f)) * 18.0f) + 2;
    nSteps = clamp(nSteps, 2, 20);
    float fSteps = (float)nSteps;

    float3 pos  = float3(baseUv, 0.0f);
    float3 step = eye / fSteps;

    // Linear search to the first intersection.
    for (int i = 0; i < nSteps - 1; ++i) {
        float h = sampleHeight(heightTexIdx, pos.xy, singleChannel, set0);
        if (pos.z < h) pos += step;
    }
    // Binary refine around the crossing.
    for (uint i = 0u; i < 6u; ++i) {
        float h = sampleHeight(heightTexIdx, pos.xy, singleChannel, set0);
        if (pos.z < h) pos += step;
        step *= 0.5f;
        pos  -= step;
    }
    return pos.xy;
}

// --- water wave normal (mirror TranslucentPass/WaterCommon.slang:24) ----------
constant float kWaveAmplitudeScale = 0.04f;
constant float kWaveFrequencyScale = 10.0f;
static float3 waterWaveNormalTangent(WaterMaterial water, float2 baseUv, float afT, device Set0& set0) {
    float waveT     = afT * water.waveSpeed;
    float amplitude = water.waveAmplitude * kWaveAmplitudeScale;
    float frequency = water.waveFreq      * kWaveFrequencyScale;

    float fT1 = waveT * 0.8f;
    float2 uv1 = baseUv + waveT * 0.01f;
    uv1.x += sin(fT1 + uv1.y * frequency) * amplitude;
    uv1.y += sin(fT1 + uv1.x * frequency) * amplitude;

    float fT2 = waveT * -2.6f;
    float2 uv2 = baseUv + waveT * -0.012f;
    uv2.x += sin(fT2 + uv2.y * frequency * 1.2f) * amplitude * 0.75f;
    uv2.y += sin(fT2 + uv2.x * frequency * 1.2f) * amplitude * 0.75f;

    uint normalTex = water.tex[1];
    if (normalTex == kInvalidTextureIndex) return float3(0.0f, 0.0f, 1.0f);

    float3 n1 = set0.gTextures2D[normalTex].sample(set0.gMaterialSampler, uv1, level(0.0f)).xyz * 2.0f - 1.0f;
    float3 n2 = set0.gTextures2D[normalTex].sample(set0.gMaterialSampler, uv2, level(0.0f)).xyz * 2.0f - 1.0f;
    return normalize(n1 * 0.7f + n2 * 0.3f);
}

// --- camera (mirror SceneCamera.computeRayPinhole, Scene.slang:105) -----------
// Falcor-compatible Ray (Scene.slang:40). No jitter applied here (the V-buffer
// raygen calls computeRayPinhole with the default applyJitter=true).
struct Ray { float3 origin; float tMin; float3 dir; float tMax; };
static float3 computeNonNormalizedRayDirPinhole(uint2 pixel, uint2 frameDim, bool applyJitter, constant SceneConstants* gPerFrame) {
    float2 p = (float2(pixel) + float2(0.5f, 0.5f)) / float2(frameDim);
    if (applyJitter) p += float2(-gPerFrame->jitterX, gPerFrame->jitterY);
    float2 ndc = float2(2.0f, -2.0f) * p + float2(-1.0f, 1.0f);
    return ndc.x * float3(gPerFrame->cameraU) + ndc.y * float3(gPerFrame->cameraV) + float3(gPerFrame->cameraW);
}
static Ray computeRayPinhole(uint2 pixel, uint2 frameDim, constant SceneConstants* gPerFrame) {
    Ray r;
    r.origin = float3(gPerFrame->posW);
    r.dir    = normalize(computeNonNormalizedRayDirPinhole(pixel, frameDim, true, gPerFrame));
    float invCos = 1.0f / dot(normalize(float3(gPerFrame->cameraW)), r.dir);
    r.tMin = gPerFrame->zNear * invCos;
    r.tMax = (gPerFrame->zFar > 0.0f ? gPerFrame->zFar : 1e6f) * invCos;
    return r;
}

// --- inline ray-query traversal (mirror the DXR anyhit/closesthit/miss; same
// pattern as SurfelRayTrace.rt.metal traceClosest). Alpha-test runs inline on
// each candidate; returns true on a committed triangle (closest hit), false on
// miss. The slang primary ray culls back-facing triangles; the kind-0
// refraction re-trace does too (it travels INTO the surface), so a single
// back-face cull serves both. -----------------------------------------------
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

// Payload mirror (Payload, slang:48). origin/direction/length/kind/source carry
// the single refraction re-trace; only `direction`/`length` matter for the
// inline single-bounce model below, the rest are kept for fidelity.
struct Payload { float3 origin; float3 direction; uint length; uint kind; uint sourceInstanceID; };

[[kernel]] void rayGen(uint2 tid [[thread_position_in_grid]],
                       device Set0& set0 [[buffer(0)]],
                       device Set1& set1 [[buffer(1)]]) {
    constant SceneConstants* gPerFrame = set1.gPerFrame;
    instance_acceleration_structure tlas = set1.tlas;
    texture2d<uint, access::write> gPackedHitInfo = set1.gPackedHitInfo;

    uint2 dims = uint2(gPerFrame->viewportSize);
    if (tid.x >= dims.x || tid.y >= dims.y) return;
    uint2 pixelPos = tid;

    // --- raygen (slang:293): primary ray from the pinhole camera. -----------
    Ray rayD = computeRayPinhole(pixelPos, dims, gPerFrame);

    Payload payload;
    payload.origin           = rayD.origin;
    payload.direction        = rayD.dir;
    payload.length           = 0u;
    payload.kind             = 0u;
    payload.sourceInstanceID = 0xffffffffu;

    // --- closest hit / miss (the TraceRay above resolves to this branch). ---
    TriangleHit triangleHit;
    if (!traceClosest(payload.origin, payload.direction, rayD.tMin, rayD.tMax, tlas, set0, triangleHit)) {
        // miss (slang:57): only the PRIMARY miss (kind 0, length 0) clears.
        gPackedHitInfo.write(uint4(0u), pixelPos);
        return;
    }

    // closeHit (slang:91)
    uint materialID = getMaterialID(triangleHit.instanceID, set0);

    // Parallax occlusion mapping for primary hits on height-mapped opaque
    // diffuse surfaces — expressed as a barycentric perturbation (slang:109-159).
    if (payload.length == 0u
        && materialID < kSolidMaterialCapacity
        && !isWater(materialID)
        && !isRefractive(materialID, set0)) {
        DiffuseMaterial m = getDiffuseMaterial(materialID, set0);
        if ((m.materialConfig & kMaterialFlagEnableHeight) != 0u
            && m.tex[4] != kInvalidTextureIndex) {
            TriVtx vtx;
            VertexData v = getVertexDataAndTri(triangleHit, vtx, set0);

            // World TBN — legacy parallax basis: cross(N,T) * handedness.
            float3 N = normalize(v.normalW);
            float3 T = normalize(v.tangentW - N * dot(N, v.tangentW));
            float3 B = normalize(cross(N, T)) * vtx.tangentL[0].w;

            bool single = (m.materialConfig & kMaterialFlagIsHeightMapSingleChannel) != 0u;
            float2 newUv = parallaxOcclusionUV(
                m.tex[4], m.heightMapScale * kParalaxScale, single,
                normalize(payload.direction), N, T, B, v.texC, set0);
            float2 duv = newUv - v.texC;

            // Invert the UV delta into a barycentric delta via the triangle's
            // UV edge vectors (UV is affine in barycentrics within a triangle).
            float2 E1 = vtx.uv[1] - vtx.uv[0];
            float2 E2 = vtx.uv[2] - vtx.uv[0];
            float det = E1.x * E2.y - E2.x * E1.y;
            if (abs(det) >= 1.0e-12f) {
                float invDet = 1.0f / det;
                float db1 = invDet * ( E2.y * duv.x - E2.x * duv.y);
                float db2 = invDet * (-E1.y * duv.x + E1.x * duv.y);

                float nb1 = triangleHit.barycentrics.x + db1;
                float nb2 = triangleHit.barycentrics.y + db2;
                float nb0 = 1.0f - nb1 - nb2;
                if (nb0 >= 0.0f && nb1 >= 0.0f && nb2 >= 0.0f)
                    triangleHit.barycentrics = float2(nb1, nb2);
            }
        }
    }

    // All hits write the primary V-buffer (slang:165).
    gPackedHitInfo.write(packTriangleHit(triangleHit), pixelPos);

    // Only the primary ray spawns a follow-up bounce (slang:169).
    if (payload.length != 0u) return;

    // Water surface: re-trace the refraction ray as a kind-0 bounce that
    // clobbers gPackedHitInfo with the underwater background (slang:179-225).
    if (isWater(materialID)) {
        VertexData v = getVertexData(triangleHit, set0);
        WaterMaterial water = getWaterMaterial(materialID, set0);

        float3 N = normalize(v.normalW);
        float3 T = normalize(v.tangentW);
        float3 B = normalize(cross(N, T));
        float3 nT = waterWaveNormalTangent(water, v.texC, gPerFrame->afT, set0);
        float3 waveN = normalize(nT.x * T + nT.y * B + nT.z * N);

        float3 incident = normalize(payload.direction);
        // Camera rays hit water from outside -> always ENTERING.
        if (dot(incident, waveN) > 0.0f) waveN = -waveN;

        float refractScale = max(water.refractionScale, 0.0f);
        float  eta          = 1.0f / (1.0f + refractScale);
        float3 refractedDir = refract(incident, waveN, eta * kWaterRefractionIntensity);
        if (dot(refractedDir, refractedDir) >= 1.0e-6f) {
            refractedDir = normalize(refractedDir);
            payload.origin           = v.posW + refractedDir * 1.0e-4f;
            payload.direction        = refractedDir;
            payload.length++;                 // 0 -> 1 (don't recurse again)
            payload.kind             = 0u;
            payload.sourceInstanceID = triangleHit.instanceID;

            TriangleHit bounceHit;
            if (traceClosest(payload.origin, payload.direction, 0.0f, 1.0e30f, tlas, set0, bounceHit))
                gPackedHitInfo.write(packTriangleHit(bounceHit), pixelPos);
            // On TIR / miss leave the water-surface hit already written above.
        }
        return;
    }

    // Refractive translucents (glass): re-trace the refraction ray as a kind-0
    // bounce that clobbers gPackedHitInfo with the refracted background
    // (slang:231-286).
    if (isRefractive(materialID, set0)) {
        VertexData v          = getVertexData(triangleHit, set0);
        float3 incidentRayDir = normalize(payload.direction);
        float3 normalW        = normalize(v.normalW);

        TranslucentMaterial mat = getTranslucentMaterial(materialID, set0);
        float refractScale = max(mat.refractionScale, 0.0f);

        float  cosI    = dot(incidentRayDir, normalW);
        bool   exiting = cosI > 0.0f;
        float3 surfN   = exiting ? -normalW : normalW;
        float  eta     = exiting ? (1.0f + refractScale) : (1.0f / (1.0f + refractScale));

        float3 refractedDir = refract(incidentRayDir, surfN, eta);
        if (dot(refractedDir, refractedDir) >= 1.0e-6f) {
            refractedDir = normalize(refractedDir);
            payload.origin    = v.posW + refractedDir * 1.0e-4f;
            payload.direction = refractedDir;
            payload.length++;
            payload.kind             = 0u;
            payload.sourceInstanceID = triangleHit.instanceID;

            TriangleHit bounceHit;
            if (traceClosest(payload.origin, payload.direction, 0.0f, 1.0e30f, tlas, set0, bounceHit))
                gPackedHitInfo.write(packTriangleHit(bounceHit), pixelPos);
            // On TIR / miss leave the glass-surface hit already written above.
        }
    }
}
