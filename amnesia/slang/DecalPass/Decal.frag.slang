/// Copyright © 2009-2020 Frictional Games
/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Slang port of AmnesiaTheDarkDescent/HPL2/resource/decal.frag.fsl.
//
// Faithful and minimal: sample the diffuse map, reject near-transparent
// fragments (alpha < 0.01, matching the reference's permissive ALPHA_REJECT
// for clean decal edges), and output diffuse × vertex colour. No lighting,
// fog, light-level or reflection — exactly like the reference.
//
// All blending is hardware blend state: DecalPipelineDesc stamps one pipeline
// per eMaterialBlendMode (Add/Mul/MulX2/Alpha/PremulAlpha) using the same
// blend factors as the translucent mesh path, which already match the legacy
// HPL2BlendTable for the colour channel. The shader therefore emits the raw
// pre-blend colour and lets the blender do the rest — no per-mode pre-bias
// needed (decals carry no light-level / fog, so the translucent shader's
// pre-bias would reduce to identity here anyway).

import Bindless.resource;
import SceneTypes;

#define DECAL_ALPHA_REJECT 0.01f

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
    DiffuseMaterial mat = gDiffuseMaterials[obj.materialID];

    float4 diffuseColor = float4(0.0f, 0.0f, 0.0f, 1.0f);
    uint   diffuseTex   = mat.tex[0];
    if (diffuseTex != kInvalidTextureIndex)
    {
        diffuseColor = gTextures2D[NonUniformResourceIndex(diffuseTex)]
                           .Sample(gMaterialSampler, psIn.uv);
    }

    if (diffuseColor.w < DECAL_ALPHA_REJECT)
    {
        discard;
    }
    return diffuseColor * psIn.color;
}
