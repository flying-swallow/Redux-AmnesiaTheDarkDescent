/// Copyright © 2009-2020 Frictional Games
/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Slang port of AmnesiaTheDarkDescent/HPL2/resource/decal.frag.fsl.
//
// Sample the diffuse map, reject near-transparent fragments (alpha < 0.01,
// matching the reference's permissive ALPHA_REJECT for clean decal edges),
// and output diffuse × vertex colour. No lighting, fog, light-level or
// reflection — exactly like the reference.
//
// Color space: the legacy renderer sampled the texture RAW (gamma) and
// hardware-blended into a display-space UNORM buffer; here the diffuse comes
// through an sRGB view (linear) and the target is the lit linear HDR pogo.
// Same scheme as Particle/Translucent.frag: re-encode the texel so the
// source product runs in the authored display space, then decode ONCE at
// output per blend mode. Critical for MULX2 decals (web_spider01 etc.):
// their authored 0.5-gray neutral must reach the 2·src·dst blender as 0.5 —
// the plain sRGB-decoded sample (0.214) darkened the whole quad to 0.43×dst
// and drew the quad border. ALPHA uses premultiplied factors with powered
// weights (see kPerceptualBlendExp).

#include "../Constants.h"

import bindless;
import SceneTypes;
import Scene;
import Utils.Color.ColorHelpers;   // linearToSRGB / sRGBToLinear

#define BLEND_MODE_ADD          0u
#define BLEND_MODE_MUL          1u
#define BLEND_MODE_MULX2        2u
#define BLEND_MODE_ALPHA        3u
#define BLEND_MODE_PREMUL_ALPHA 4u

#define DECAL_ALPHA_REJECT 0.01f

// Mirrors the PushBlock written at the decal draw site in cHybridRenderer
// (DecalPipelineDesc::BlendMode — same value layout as the defines above).
struct DecalPushConstants
{
    uint blendMode;
};

[vk::push_constant]
DecalPushConstants pc;

struct DecalVSOut
{
    float2 uv                      : TEXCRD;
    float4 color                   : COLOR;
    nointerpolation uint objectId  : OBJECTID;
    float4 posH                    : SV_POSITION;
};

[shader("fragment")]
float4 psMain(DecalVSOut psIn) : SV_TARGET
{
    UniformObject   obj = gSceneObjects[psIn.objectId];
    DiffuseMaterial mat = gScene.getDiffuseMaterial(obj.materialID);

    // Display-space source: re-encode the sRGB-decoded sample, multiply by
    // the raw authored vertex tint — exactly the legacy product.
    float4 diffuseColor = float4(0.0f, 0.0f, 0.0f, 1.0f);
    uint   diffuseTex   = mat.tex[0];
    if (diffuseTex != kInvalidTextureIndex)
    {
        float4 s = gTextures2D[NonUniformResourceIndex(diffuseTex)]
                       .Sample(gMaterialSampler, psIn.uv);
        diffuseColor = float4(linearToSRGB(s.rgb), s.a);
    }
    diffuseColor *= psIn.color;

    if (diffuseColor.w < DECAL_ALPHA_REJECT)
    {
        discard;
    }

    // Decode to linear once per blend mode (see Particle.frag.slang for the
    // derivation): products with dst (MUL/MULX2) are power-law exact; the
    // ALPHA/PREMUL sums use powered premultiplied weights.
    const float k = kPerceptualBlendExp;
    float4 finalColor = diffuseColor;
    switch (pc.blendMode)
    {
        case BLEND_MODE_ADD:
        case BLEND_MODE_MUL:
        {
            finalColor.xyz = sRGBToLinear(finalColor.xyz);
            break;
        }
        case BLEND_MODE_MULX2:
        {
            // Legacy: 2·dst·src; HW factors give 2·src·dst → src = toLin(2m)/2.
            // Neutral m = 0.5 → toLin(1)/2 = 0.5 → 2·0.5·dst = dst: invisible.
            finalColor.xyz = sRGBToLinear(2.0f * finalColor.xyz) * 0.5f;
            break;
        }
        case BLEND_MODE_ALPHA:
        {
            float a = saturate(finalColor.a);
            finalColor.xyz = sRGBToLinear(finalColor.xyz) * pow(a, k);
            finalColor.a   = 1.0f - pow(1.0f - a, k);
            break;
        }
        case BLEND_MODE_PREMUL_ALPHA:
        {
            float a = saturate(finalColor.a);
            finalColor.xyz = sRGBToLinear(finalColor.xyz);
            finalColor.a   = 1.0f - pow(1.0f - a, k);
            break;
        }
    }

    return finalColor;
}
