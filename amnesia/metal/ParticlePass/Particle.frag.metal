// Hand-written Metal port of amnesia/slang/ParticlePass/Particle.frag.slang
//
// Bindless particle fragment stage. Samples the translucent material's diffuse
// in display space, multiplies by vertex color, applies per-pixel fog +
// optional AffectedByLightLevel dimming, then runs the shared legacy blend-mode
// source math (BlendModes.metalh) and decodes to linear once at output. Hardware
// blend state is selected on the C++ side (one pipeline per blend mode).
// Same set split / helper layout as the sibling Translucent.frag.metal.
//
// Bindings:
//   set0 (bindless arg buffer) buffer(0); [[id]] = kBinding* (Constants.h):
//     gMaterialSampler 9   gSceneObjects 20   gTranslucentMaterials 24
//     gTextures2D[] 0 (remapped, array LAST -> id 49)
//   set1 (program arg buffer) buffer(1); [[id]] = kBinding* within the set:
//     gPerFrame 0   gPointLights 22   gSpotLights 29   gLightGridCount 40
//     gLightGridList 41   gFogAreas 42
//   push constant (ParticlePushConstants) -> [[buffer(2)]]
#include <metal_stdlib>
using namespace metal;

// --- shared constants (mirrored from amnesia/slang/Constants.h) --------------
constant uint  kInvalidTextureIndex            = 0xffffffffu;
constant uint  kSolidMaterialCapacity          = 2048u;
constant uint  kTranslucentMaterialCapacity    = 256u;
constant uint  kTranslucentMaterialBase        = kSolidMaterialCapacity;  // 2048
constant uint  kMaterialFlagAffectedByLightLevel = 1u << 17;

// Light-grid math (lightLevelAt) — distinct dim from the surfel cell grid.
constant uint  kLightGridDim     = 128u;
constant float kCellUnit         = 0.5f;
constant uint  kCellDimensionLG  = 250u;
constant float kLightGridUnit    = (float(kCellDimensionLG) * kCellUnit) / float(kLightGridDim);
constant uint  kLightsPerCellMax = 128u;

// --- shared struct layouts (flattened in by cmake/flatten_metal_includes.py) ---
#include "SceneTypes.metalh"                 // SceneConstants, MatStorage, UniformObject, lights
#include "Utils/Color/ColorHelpers.metalh"   // sRGBToLinear / linearToSRGB
#include "Fog.metalh"                         // FogAreaParams, FogResult, computeFog (needs SceneConstants)
#include "BlendModes.metalh"                  // applyBlendModulation / encodeBlendOutputLinear / displaySpaceSample

// Material table (scalar field order verbatim — mirror SceneTypes.slang).
struct TranslucentMaterial { uint type; uint materialConfig; uint cubeMapTextureIndex; uint tex[8];
                             float refractionScale, frenselBias, frenselPow, rimLightMul, rimLightPow; };

// Set-0 bindless argument buffer (subset). The 16384-element texture array
// consumes consecutive [[id]] slots so it goes LAST: gTextures2D at id(49)
// (49..16432); the C++ bindless encoder remaps it to kBindingTextures2D.
struct Set0 {
    sampler                          gMaterialSampler      [[id(9)]];
    device UniformObject*            gSceneObjects         [[id(20)]];
    device TranslucentMaterial*      gTranslucentMaterials [[id(24)]];
    array<texture2d<float>, 16384>   gTextures2D           [[id(49)]];  // kBindingTextures2D (remapped)
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

struct ParticlePushConstants {
    uint  blendMode;
    float sceneAlpha;
};

struct ParticleVSOut {
    float4 posH                   [[position]];
    float3 viewPos;
    float2 uv;
    float4 color;
    uint   objectId [[flat]];
};

// --- light-grid math (mirror Grid.slang) -------------------------------------
static bool  gridCellValid(int3 c, uint dim) { int h = int(dim / 2u); return abs(c.x) < h && abs(c.y) < h && abs(c.z) < h; }
static int3  gridCellPos(float3 p, float3 cam, float gu) { return int3(round((p - cam) / gu)); }
static uint  gridFlattenCellIndex(int3 c, uint dim) { uint3 u = uint3(c + int3(int(dim / 2u))); return u.z*dim*dim + u.y*dim + u.x; }

// Scalar "surrounding light" heuristic for AffectedByLightLevel materials
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

fragment float4 psMain(ParticleVSOut psIn [[stage_in]],
                       device Set0& set0 [[buffer(0)]],
                       device Set1& set1 [[buffer(1)]],
                       constant ParticlePushConstants& pc [[buffer(2)]]) {
    constant SceneConstants* gPerFrame = set1.gPerFrame;

    UniformObject obj = set0.gSceneObjects[psIn.objectId];
    // Particle materials are all translucent — read from the translucent table.
    TranslucentMaterial mat = set0.gTranslucentMaterials[obj.materialID - kTranslucentMaterialBase];

    // Display-space diffuse source x vertex color.
    float4 diffuseColor = float4(0.0f, 0.0f, 0.0f, 1.0f);
    uint diffuseTex = mat.tex[0];
    if (diffuseTex != kInvalidTextureIndex) {
        float4 s = set0.gTextures2D[diffuseTex].sample(set0.gMaterialSampler, psIn.uv);
        diffuseColor = displaySpaceSample(s);
    }
    diffuseColor *= psIn.color;

    // Per-pixel fog (world + fog areas) -> visibility scalar folded into alpha.
    float3 surfacePosW = (gPerFrame->invViewMat * float4(psIn.viewPos, 1.0f)).xyz;
    FogResult fog = computeFog(surfacePosW, float3(gPerFrame->posW), set1.gFogAreas, gPerFrame);
    float finalAlpha = pc.sceneAlpha * fog.visibility;

    // AffectedByLightLevel dim.
    float lightLevel = 1.0f;
    if ((mat.materialConfig & kMaterialFlagAffectedByLightLevel) != 0u) {
        lightLevel = lightLevelAt(surfacePosW, set1, gPerFrame);
    }

    // Shared legacy blend math: display-space source modulation, then one decode.
    float4 modulated    = applyBlendModulation(pc.blendMode, diffuseColor, finalAlpha, lightLevel);
    float  displayAlpha = modulated.a;
    float4 finalColor   = encodeBlendOutputLinear(pc.blendMode, modulated);

    if (displayAlpha < 0.01f) {
        discard_fragment();
    }
    return finalColor;
}
