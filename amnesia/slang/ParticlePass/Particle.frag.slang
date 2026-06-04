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

#include "../Constants.h"   // kMaterialFlagAffectedByLightLevel

import bindless;
import SceneTypes;
import PerFrame.resource;
import Scene;
import Utils.Color.ColorHelpers;   // linearToSRGB / sRGBToLinear (display-space source math)

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

    // Legacy display-space source: the base game blended raw gamma texels ×
    // authored vertex colors in a UNORM display target. Re-encode the
    // hardware-sRGB-decoded sample so the product happens in the space it was
    // authored for; the per-blend-mode source math below also runs in display
    // space and is decoded to linear ONCE at output (exact for products and
    // dst-free lerps — toLin distributes over products via the power law).
    float4 diffuseColor = float4(0.0f, 0.0f, 0.0f, 1.0f);
    uint diffuseTex = mat.tex[0];
    if (diffuseTex != kInvalidTextureIndex)
    {
        float4 s = gTextures2D[NonUniformResourceIndex(diffuseTex)]
                       .Sample(gMaterialSampler, psIn.uv);
        diffuseColor = float4(linearToSRGB(s.rgb), s.a);
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

    // AffectedByLightLevel materials dim with the surrounding analytic light
    // (base-game afLightLevel behavior — see gScene.lightLevelAt). Per-pixel:
    // the world position is reconstructed from the interpolated view-space
    // position, so large billboards get an exact gradient across the quad.
    float lightLevel = 1.0f;
    if ((mat.materialConfig & kMaterialFlagAffectedByLightLevel) != 0u)
    {
        float3 surfacePosW = mul(gPerFrame.invViewMat, float4(psIn.viewPos, 1.0f)).xyz;
        lightLevel = gScene.lightLevelAt(surfacePosW);
    }

    // Per-blend-mode legacy source math in display space, then one decode to
    // linear. The blend-with-dst sum can't be decoded exactly (toLin doesn't
    // distribute over sums and dst is unknown), so ALPHA/PREMUL distribute the
    // decode over the blend weights instead: src·pow(α,k) + dst·pow(1−α,k)
    // via premultiplied factors. Endpoint-exact; k = kPerceptualBlendExp
    // (2.2 = pure power-law distribution) is the tuning knob.
    const float k = kPerceptualBlendExp;
    float4 finalColor = diffuseColor;
    float displayAlpha = diffuseColor.a;   // legacy discard-threshold operand
    switch (pc.blendMode)
    {
        case BLEND_MODE_ADD:
        {
            // Legacy: dst + rgb·α·L. The whole src term is dst-free →
            // decode the product exactly; the linear add is the approx.
            finalColor.xyz = sRGBToLinear(finalColor.xyz * (finalAlpha * lightLevel));
            break;
        }
        case BLEND_MODE_MUL:
        {
            // Legacy: dst × lerp(1, rgb, α). toLin distributes over the dst
            // product, so decoding the multiplier is power-law exact.
            finalColor.xyz = sRGBToLinear(lerp(float3(1.0f), finalColor.xyz, finalAlpha));
            break;
        }
        case BLEND_MODE_MULX2:
        {
            // Legacy: 2·dst·lerp(0.5, rgb·L, α); HW factors give 2·src·dst →
            // src = toLin(2·m)/2.
            float blendMulAlpha = lightLevel * finalAlpha;
            float3 m = finalColor.xyz * blendMulAlpha
                     + float3(0.5f) * (1.0f - blendMulAlpha);
            finalColor.xyz = sRGBToLinear(2.0f * m) * 0.5f;
            break;
        }
        case BLEND_MODE_ALPHA:
        {
            // Legacy: rgb·L·a + dst·(1−a) in display space. Premultiplied
            // powered weights (pipeline: ONE / 1−srcA).
            float a = saturate(finalColor.a * finalAlpha);
            displayAlpha   = a;
            finalColor.xyz = sRGBToLinear(finalColor.xyz * lightLevel) * pow(a, k);
            finalColor.a   = 1.0f - pow(1.0f - a, k);
            break;
        }
        case BLEND_MODE_PREMUL_ALPHA:
        {
            // Legacy: rgb·α·L + dst·(1−a); rgb already premultiplied content.
            float a = saturate(finalColor.a * finalAlpha);
            displayAlpha   = a;
            finalColor.xyz = sRGBToLinear(finalColor.xyz * (finalAlpha * lightLevel));
            finalColor.a   = 1.0f - pow(1.0f - a, k);
            break;
        }
    }

    if (displayAlpha < 0.01f)
    {
        discard;
    }

    return finalColor;
}
