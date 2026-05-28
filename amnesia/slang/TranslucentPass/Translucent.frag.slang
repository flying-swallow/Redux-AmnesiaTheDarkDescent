/// Copyright © 2009-2020 Frictional Games
/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Slang port of AmnesiaTheDarkDescent/HPL2/resource/translucency.frag.fsl —
// the non-particle (mesh) translucent path. Hardware blend state is selected
// on the C++ side (one pipeline per blend mode); this shader applies the
// matching per-pixel color-space transform the reference shader did so the
// output going into the blender is in the right space (mul/mulx2 pre-bias
// before SRC*DST blend, etc.).
//
// V1 scope: diffuse sample × vertex color × tint, fog, per-blend-mode
// pre-bias, alpha discard. The reference's refraction (screen-color sample
// through normal-mapped offset) and cube-map Fresnel/rim reflections require
// infrastructure (refraction copy, view-rotation matrix, cube-map binding)
// the hybrid renderer doesn't have yet — both paths are skipped and tagged
// TODO so the next iteration knows where to wire them.

import Bindless.resource;
import SceneTypes;
import PerFrame.resource;

#define BLEND_MODE_ADD          0u
#define BLEND_MODE_MUL          1u
#define BLEND_MODE_MULX2        2u
#define BLEND_MODE_ALPHA        3u
#define BLEND_MODE_PREMUL_ALPHA 4u

// Mirrors the PushBlock written from cHybridRenderer at the per-mesh draw
// site (uint blendMode, float sceneAlpha). Fragment-only push range —
// matches VK_SHADER_STAGE_FRAGMENT_BIT on the vkCmdPushConstants call.
struct TranslucentPushConstants
{
    uint  blendMode;
    float sceneAlpha;
};

[vk::push_constant]
TranslucentPushConstants pc;

struct TranslucentVSOut
{
    float3 viewPos                 : VIEWPOS;
    float2 uv                      : TEXCRD;
    float4 color                   : COLOR;
    nointerpolation uint objectId  : OBJECTID;
    float4 posH                    : SV_POSITION;
};

[shader("fragment")]
float4 psMain(TranslucentVSOut psIn) : SV_TARGET
{
    UniformObject   obj = gSceneObjects[psIn.objectId];
    DiffuseMaterial mat = gDiffuseMaterials[obj.materialID];

    float4 diffuseColor = float4(0.0f, 0.0f, 0.0f, 1.0f);
    uint diffuseTex = mat.tex[0];
    if (diffuseTex != kInvalidTextureIndex)
    {
        diffuseColor = gTextures2D[NonUniformResourceIndex(diffuseTex)]
                           .Sample(gMaterialSampler, psIn.uv);
    }
    diffuseColor *= psIn.color;

    float finalAlpha = pc.sceneAlpha;
    if (gPerFrame.worldFogLength > 0.0f)
    {
        float fogAmount = pow(
            saturate((-psIn.viewPos.z - gPerFrame.worldFogStart)
                     / gPerFrame.worldFogLength),
            gPerFrame.fogFalloffExp);
        finalAlpha = (gPerFrame.oneMinusFogAlpha * fogAmount
                      + (1.0f - fogAmount)) * pc.sceneAlpha;
    }

    float4 finalColor = diffuseColor;
    switch (pc.blendMode)
    {
        case BLEND_MODE_ADD:
        {
            finalColor.xyz *= finalAlpha * obj.lightLevel;
            break;
        }
        case BLEND_MODE_MUL:
        {
            finalColor.xyz += (float3(1.0f) - finalColor.xyz) * (1.0f - finalAlpha);
            break;
        }
        case BLEND_MODE_MULX2:
        {
            float blendMulAlpha = obj.lightLevel * finalAlpha;
            finalColor.xyz = finalColor.xyz * blendMulAlpha
                           + float3(0.5f) * (1.0f - blendMulAlpha);
            break;
        }
        case BLEND_MODE_ALPHA:
        {
            finalColor.xyz *= obj.lightLevel;
            finalColor.a   *= finalAlpha;
            break;
        }
        case BLEND_MODE_PREMUL_ALPHA:
        {
            finalColor      *= finalAlpha;
            finalColor.xyz  *= obj.lightLevel;
            break;
        }
    }

    // TODO(translucent v2): refraction sample through normal-mapped offset
    // (reference's UseRefractionTrans branch). Needs a screen-color copy
    // resource bound here.
    //
    // TODO(translucent v2): cube-map Fresnel + rim reflections (reference's
    // HasCubeMap branch). Needs invViewRotationMat in gPerFrame + a cube-map
    // sampler array binding.

    if (finalColor.a < 0.01f)
    {
        discard;
    }

    return finalColor;
}
