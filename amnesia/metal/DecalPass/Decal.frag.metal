// Hand-written Metal port of amnesia/slang/DecalPass/Decal.frag.slang
//
// Decal overlay fragment stage. Sample the diffuse map in display space,
// multiply by the raw authored vertex tint, alpha-reject near-transparent
// fragments, then decode to linear once per blend mode. No lighting, fog,
// light-level or reflection — exactly like the reference. Decals read the
// DIFFUSE material table (solid range, no de-bias); the modulation stage is the
// identity for decals (no sceneAlpha/fog/lightLevel), only the decode runs.
//
// Bindings:
//   set0 (bindless arg buffer) buffer(0); [[id]] = kBinding* (Constants.h):
//     gMaterialSampler 9  gSceneObjects 20  gDiffuseMaterials 21
//     gTextures2D (remapped, array LAST) 49
//   gPerFrame is unused by this stage (no set1 bound).
//   push constant (DecalPushConstants) -> buffer(2)
#include <metal_stdlib>
using namespace metal;

constant uint  kInvalidTextureIndex = 0xffffffffu;
constant float DECAL_ALPHA_REJECT   = 0.01f;

#include "SceneTypes.metalh"            // MatStorage, UniformObject (SceneConstants unused here)
#include "Utils/Color/ColorHelpers.metalh"  // sRGBToLinear / linearToSRGB
#include "BlendModes.metalh"           // displaySpaceSample / encodeBlendOutputLinear / kBlendMode*

// Material table (mirror amnesia/slang/SceneTypes.slang — scalar field order verbatim).
struct DiffuseMaterial { uint type; uint materialConfig; uint cubeMapTextureIndex; uint tex[8]; float heightMapScale, heightMapBias, frenselBias, frenselPow; };

// Set-0 bindless argument buffer. Members in ascending [[id]] order; the 16384-
// element texture array goes LAST (id 49.. — remapped to kBindingTextures2D).
struct Set0 {
    sampler                 gMaterialSampler  [[id(9)]];
    device UniformObject*   gSceneObjects     [[id(20)]];
    device DiffuseMaterial* gDiffuseMaterials [[id(21)]];
    array<texture2d<float>, 16384> gTextures2D [[id(49)]];  // kBindingTextures2D (remapped)
};

struct DecalPushConstants {
    uint blendMode;
};

struct DecalVSOut {
    float4 posH                   [[position]];
    float2 uv;
    float4 color;
    uint   objectId [[flat]];
};

fragment float4 psMain(DecalVSOut psIn [[stage_in]],
                       device Set0& set0 [[buffer(0)]],
                       constant DecalPushConstants& pc [[buffer(2)]]) {
    UniformObject   obj = set0.gSceneObjects[psIn.objectId];
    DiffuseMaterial mat = set0.gDiffuseMaterials[obj.materialID];  // decals: solid range, no de-bias

    float4 diffuseColor = float4(0.0f, 0.0f, 0.0f, 1.0f);
    uint   diffuseTex   = mat.tex[0];
    if (diffuseTex != kInvalidTextureIndex) {
        float4 s = set0.gTextures2D[diffuseTex].sample(set0.gMaterialSampler, psIn.uv);
        diffuseColor = displaySpaceSample(s);
    }
    diffuseColor *= psIn.color;

    if (diffuseColor.w < DECAL_ALPHA_REJECT) {
        discard_fragment();
    }

    return encodeBlendOutputLinear(pc.blendMode, diffuseColor);
}
