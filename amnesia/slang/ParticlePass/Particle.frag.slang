/// Copyright © 2009-2020 Frictional Games
/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Slang port of amnesia/glsl/particle.frag. Like its GLSL twin, this is the
// bindless port of AmnesiaTheDarkDescent/HPL2/resource/translucency_particle.frag.fsl.
// Hardware blend state is selected on the C++ side (one pipeline per blend
// mode); this shader applies the matching per-pixel color-space transform
// the legacy shader did so the output going into the blender is in the
// right space (e.g. mul/mulx2 pre-bias before SRC*DST blend).

import bindless;
import SceneTypes;
import PerFrame.resource;
import Scene;

#define BLEND_MODE_ADD          0u
#define BLEND_MODE_MUL          1u
#define BLEND_MODE_MULX2        2u
#define BLEND_MODE_ALPHA        3u
#define BLEND_MODE_PREMUL_ALPHA 4u

// Mirrors the `PushBlock` written from cHybridRenderer::Draw at the per-
// particle-draw site (uint blendMode, float sceneAlpha). Fragment-only push
// range — matches VK_SHADER_STAGE_FRAGMENT_BIT on the vkCmdPushConstants
// call.
struct ParticlePushConstants
{
    uint  blendMode;
    float sceneAlpha;
};

[vk::push_constant]
ParticlePushConstants pc;

struct ParticleVSOut
{
    float3 viewPos                 : VIEWPOS;
    float2 uv                      : TEXCRD;
    float4 color                   : COLOR;
    nointerpolation uint objectId  : OBJECTID;
    float4 posH                    : SV_POSITION;
};

[shader("fragment")]
float4 psMain(ParticleVSOut psIn) : SV_TARGET
{
    UniformObject       obj = gSceneObjects[psIn.objectId];
    // Particle materials are all Type="translucent", so submitMaterial() places
    // them in the translucent table (id >= kTranslucentMaterialBase), not the
    // diffuse table. Read them the same way Translucent.frag.slang does.
    TranslucentMaterial mat = gScene.getTranslucentMaterial(obj.materialID);

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

    if (finalColor.a < 0.01f)
    {
        discard;
    }

    return finalColor;
}
