// Hand-written Metal port of amnesia/slang/TranslucentPass/Translucent.frag.slang
//
// Raster translucent (mesh) fragment stage. diffuse x vertex color x tint, fog,
// per-blend-mode pre-bias, AffectedByLightLevel dim, static cube-map Fresnel
// reflection + rim, and the UseIlluminationTrans second-draw mask. Refraction is
// upstream (the RT V-buffer already bent the primary ray through refractive
// materials), so there is NO screen-color refraction block here. The display-
// space blend math runs in the authored gamma space, then decodes to linear once.
//
// Bindings:
//   set0 (bindless arg buffer) buffer(0); [[id]] = kBinding* (Constants.h):
//     gMaterialSampler 9   gSceneObjects 20   gTranslucentMaterials 24
//     gTexturesCube[] 1 (remapped)   gTextures2D[] 0 (remapped)
//   set1 buffer(1); [[id]] = VK binding within the set:
//     gPerFrame 0   gPointLights 22   gSpotLights 29   gLightGridCount 40
//     gLightGridList 41   gFogAreas 42
//   push constant (TranslucentPushConstants) -> [[buffer(2)]]
#include <metal_stdlib>
using namespace metal;

// --- shared constants (mirrored from amnesia/slang/Constants.h) --------------
constant uint  kInvalidTextureIndex            = 0xffffffffu;
constant uint  kSolidMaterialCapacity          = 2048u;
constant uint  kTranslucentMaterialCapacity    = 256u;
constant uint  kTranslucentMaterialBase        = kSolidMaterialCapacity;                       // 2048
constant uint  kWaterMaterialBase              = kSolidMaterialCapacity + kTranslucentMaterialCapacity; // 2304

constant uint  kMaterialFlagEnableDiffuse          = 1u << 0;
constant uint  kMaterialFlagEnableNormal           = 1u << 1;
constant uint  kMaterialFlagEnableCubeMap          = 1u << 6;
constant uint  kMaterialFlagEnableCubeMapAlpha     = 1u << 8;
constant uint  kMaterialFlagAffectedByLightLevel   = 1u << 17;

// Light-grid math (lightLevelAt) — distinct dim from the surfel cell grid.
constant uint  kLightGridDim     = 128u;
constant float kCellUnit         = 0.5f;
constant uint  kCellDimensionLG  = 250u;
constant float kLightGridUnit    = (float(kCellDimensionLG) * kCellUnit) / float(kLightGridDim);
constant uint  kLightsPerCellMax = 128u;

// TranslucencyFlags behaviour bit (mirrors Translucent.frag.slang).
#define TRANS_OPT_USE_ILLUMINATION (1u << 0)

// --- shared struct layouts (flattened in by cmake/flatten_metal_includes.py) ---
#include "SceneTypes.metalh"                 // SceneConstants, MatStorage, UniformObject, lights
#include "Utils/Color/ColorHelpers.metalh"   // sRGBToLinear / linearToSRGB
#include "Fog.metalh"                         // FogAreaParams, FogResult, computeFog (needs SceneConstants)
#include "BlendModes.metalh"                  // applyBlendModulation / encodeBlendOutputLinear / displaySpaceSample

// Material table (scalar field order verbatim — mirror SceneTypes.slang).
struct TranslucentMaterial { uint type; uint materialConfig; uint cubeMapTextureIndex; uint tex[8];
                             float refractionScale, frenselBias, frenselPow, rimLightMul, rimLightPow; };

// Set-0 bindless argument buffer (subset). Array textures consume consecutive
// [[id]] slots so they go LAST: gTextures2D at id(49) (49..16432), gTexturesCube
// at id(16433) (16433..32816); the C++ bindless encoder remaps them to
// kBindingTextures2D / kBindingTexturesCube. Every other member keeps id == kBinding*.
struct Set0 {
    sampler                          gMaterialSampler      [[id(9)]];
    device UniformObject*            gSceneObjects         [[id(20)]];
    device TranslucentMaterial*      gTranslucentMaterials [[id(24)]];
    array<texture2d<float>,   16384> gTextures2D           [[id(49)]];     // kBindingTextures2D (remapped)
    array<texturecube<float>, 16384> gTexturesCube         [[id(16433)]];  // kBindingTexturesCube (remapped)
};
// Set-1 program argument buffer.
struct Set1 {
    constant SceneConstants* gPerFrame       [[id(0)]];
    device PointLight*       gPointLights    [[id(22)]];
    device SpotLight*        gSpotLights     [[id(29)]];
    device uint*             gLightGridCount [[id(40)]];
    device uint*             gLightGridList  [[id(41)]];
    device FogAreaParams*    gFogAreas       [[id(42)]];
};

struct TranslucentPushConstants {
    uint  blendMode;
    float sceneAlpha;
    uint  options;
    uint  _pad;
};

struct PSIn {
    float3 viewPos;
    float2 uv;
    float3 normal;
    float3 tangent;
    float3 bitangent;
    float4 color;
    uint   objectId [[flat]];
    float4 posH     [[position]];
};

// Schlick-Fresnel — base clamped to a tiny positive epsilon (pow of a negative
// base is NaN, which MUL blending would bake permanently).
static float computeFresnel(float afEDotN, float afFresnelBias, float afFresnelPow) {
    float fFacing = max(1.0f - afEDotN, 1.0e-6f);
    return saturate(afFresnelBias + (1.0f - afFresnelBias) * pow(fFacing, afFresnelPow));
}

// --- light-grid math (mirror Grid.slang) -------------------------------------
static bool  gridCellValid(int3 c, uint dim) { int h = int(dim / 2u); return abs(c.x) < h && abs(c.y) < h && abs(c.z) < h; }
static int3  gridCellPos(float3 p, float3 cam, float gu) { return int3(round((p - cam) / gu)); }
static uint  gridFlattenCellIndex(int3 c, uint dim) { uint3 u = uint3(c + int3(int(dim / 2u))); return u.z*dim*dim + u.y*dim + u.x; }

// Scalar "surrounding light" heuristic for AffectedByLightLevel translucents
// (mirror Scene.lightLevelAt): sum maxChannel(authored sRGB color) ·
// saturate(1 - d/authoredRadius) over the cell's lights, saturated at 1.
// Out-of-grid points return 1.0 (don't darken).
static float lightLevelAt(float3 posW, device Set1& set1, constant SceneConstants* gPerFrame) {
    uint nPoint = gPerFrame->pointLightCount;
    uint nSpot  = gPerFrame->spotLightCount;
    if (nPoint + nSpot == 0u) return 1.0f;

    int3 cell = gridCellPos(posW, float3(gPerFrame->posW), kLightGridUnit);
    if (!gridCellValid(cell, kLightGridDim)) return 1.0f;

    uint flat  = gridFlattenCellIndex(cell, kLightGridDim);
    uint count = min(set1.gLightGridCount[flat], kLightsPerCellMax);

    float sum = 0.0f;
    for (uint k = 0u; k < count; ++k) {
        uint li = set1.gLightGridList[flat * kLightsPerCellMax + k];
        float3 lpos; float3 lcolor; float lintensity;
        if (li < nPoint) {
            PointLight pl = set1.gPointLights[li];
            lpos = float3(pl.position); lcolor = float3(pl.color); lintensity = pl.intensity;
        } else {
            uint si = li - nPoint;
            if (si >= nSpot) continue;
            SpotLight sl = set1.gSpotLights[si];
            lpos = float3(sl.position); lcolor = float3(sl.color); lintensity = sl.intensity;
        }
        float authoredRadius = lintensity;
        if (authoredRadius <= 0.0f) continue;
        float maxC = linearToSRGB(max(lcolor.x, max(lcolor.y, lcolor.z)));
        float dist = length(posW - lpos);
        sum += maxC * saturate(1.0f - dist / authoredRadius);
        if (sum >= 1.0f) return 1.0f;
    }
    return sum;
}

fragment float4 psMain(PSIn psIn [[stage_in]],
                       device Set0& set0 [[buffer(0)]],
                       device Set1& set1 [[buffer(1)]],
                       constant TranslucentPushConstants& pc [[buffer(2)]]) {
    constant SceneConstants* gPerFrame = set1.gPerFrame;

    UniformObject       obj = set0.gSceneObjects[psIn.objectId];
    TranslucentMaterial mat = set0.gTranslucentMaterials[obj.materialID - kTranslucentMaterialBase];

    // Second cube-map-only draw mask: switch off diffuse/illumination sampling.
    uint textureConfig = mat.materialConfig;
    if ((pc.options & TRANS_OPT_USE_ILLUMINATION) != 0u) {
        textureConfig &= (kMaterialFlagEnableNormal
                        | kMaterialFlagEnableCubeMap
                        | kMaterialFlagEnableCubeMapAlpha);
    }

    // Display-space diffuse source x vertex color.
    float4 diffuseColor = float4(0.0f, 0.0f, 0.0f, 1.0f);
    if ((textureConfig & kMaterialFlagEnableDiffuse) != 0u) {
        uint diffuseTex = mat.tex[0];
        if (diffuseTex != kInvalidTextureIndex) {
            float4 s = set0.gTextures2D[diffuseTex].sample(set0.gMaterialSampler, psIn.uv);
            diffuseColor = displaySpaceSample(s);
        }
        diffuseColor *= psIn.color;
    }

    // Per-pixel fog -> visibility scalar folded into alpha.
    float3 surfacePosW = (gPerFrame->invViewMat * float4(psIn.viewPos, 1.0f)).xyz;
    FogResult fog = computeFog(surfacePosW, float3(gPerFrame->posW), set1.gFogAreas, gPerFrame);
    float finalAlpha = pc.sceneAlpha * fog.visibility;

    // AffectedByLightLevel dim (gated on the raw material config).
    float lightLevel = 1.0f;
    if ((mat.materialConfig & kMaterialFlagAffectedByLightLevel) != 0u) {
        lightLevel = lightLevelAt(surfacePosW, set1, gPerFrame);
    }

    // Stage 1: display-space source modulation (legacy per-mode pre-bias).
    float4 finalColor = applyBlendModulation(pc.blendMode, diffuseColor, finalAlpha, lightLevel);

    // Fresnel cube-map reflection + rim (display space).
    bool hasCubeMap = (textureConfig & kMaterialFlagEnableCubeMap) != 0u
                      && mat.cubeMapTextureIndex != kInvalidTextureIndex;
    if (hasCubeMap) {
        float3 screenNormal;
        if ((textureConfig & kMaterialFlagEnableNormal) != 0u) {
            uint normalTex = mat.tex[1];
            if (normalTex != kInvalidTextureIndex) {
                float3 mapNormal = set0.gTextures2D[normalTex].sample(set0.gMaterialSampler, psIn.uv).xyz * 2.0f - 1.0f;
                screenNormal = normalize(mapNormal.x * psIn.tangent
                                       + mapNormal.y * psIn.bitangent
                                       + mapNormal.z * psIn.normal);
            } else {
                screenNormal = normalize(psIn.normal);
            }
        } else {
            screenNormal = normalize(psIn.normal);
        }

        float3 eyeVec  = normalize(psIn.viewPos);
        float  edotN   = max(dot(-eyeVec, screenNormal), 0.0f);
        float  fresnel = computeFresnel(edotN, mat.frenselBias, mat.frenselPow);

        float3 envDirView  = reflect(eyeVec, screenNormal);
        float3 envDirWorld = (gPerFrame->invViewRotationMat * float4(envDirView, 1.0f)).xyz;
        float4 reflectionColor =
            set0.gTexturesCube[mat.cubeMapTextureIndex].sample(set0.gMaterialSampler, envDirWorld);

        if ((textureConfig & kMaterialFlagEnableCubeMapAlpha) != 0u) {
            uint cubeAlphaTex = mat.tex[7];
            if (cubeAlphaTex != kInvalidTextureIndex) {
                float fEnvMapAlpha = set0.gTextures2D[cubeAlphaTex].sample(set0.gMaterialSampler, psIn.uv).w;
                reflectionColor *= fEnvMapAlpha;
            }
        }

        finalColor.xyz += reflectionColor.xyz * fresnel * finalAlpha * lightLevel;

        // Rim reflection term — view-space normal vs -Z.
        if (mat.rimLightMul > 0.0f) {
            float fRim = dot(screenNormal, float3(0.0f, 0.0f, -1.0f));
            fRim = pow(max(1.0f - abs(fRim), 1.0e-6f), mat.rimLightPow) * mat.rimLightMul;
            finalColor.xyz += reflectionColor.xyz * fRim * finalAlpha * lightLevel;
        }
    }

    // Strict zero-alpha discard (matches the reference, on display-space alpha).
    float displayAlpha = finalColor.a;
    if (displayAlpha <= 0.0f)
        discard_fragment();

    // Stage 2: decode display-space to linear once for the hardware blender.
    return encodeBlendOutputLinear(pc.blendMode, finalColor);
}
