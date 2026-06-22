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
import Fog;          // computeFog — shared world + fog-area visibility
import BlendModes;   // displaySpaceSample / applyBlendModulation / encodeBlendOutputLinear

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
    // Particle materials are all Type="translucent", so the material blob is
    // tagged MATERIAL_TYPE_TRANSLUCENT. Read them the same way Translucent.frag does.
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
        float4 s = sampleBindless2D(diffuseTex, psIn.uv);
        diffuseColor = displaySpaceSample(s);
    }
    diffuseColor *= psIn.color;

    // Per-pixel fog via the shared helper (world + fog areas), matching
    // Translucent.frag: the visibility scalar folds into the per-pixel alpha.
    // The world position is reconstructed from the interpolated view-space
    // position — one reconstruction shared with the lightLevel path below.
    // (Replaces an inline view-depth world-fog variant that ignored
    // gFogAreas; world-distance fog is slightly radial vs the old planar
    // falloff, and particles now fade inside fog areas like mesh
    // translucents — restoring legacy parity.)
    float3 surfacePosW = mul(gPerFrame.invViewMat, float4(psIn.viewPos, 1.0f)).xyz;
    FogResult fog = computeFog(surfacePosW, gPerFrame.posW);
    float finalAlpha = pc.sceneAlpha * fog.visibility;

    // AffectedByLightLevel materials dim with the surrounding analytic light
    // (base-game afLightLevel behavior — see gScene.lightLevelAt). Per-pixel,
    // so large billboards get an exact gradient across the quad.
    float lightLevel = 1.0f;
    if ((mat.materialConfig & kMaterialFlagAffectedByLightLevel) != 0u)
    {
        lightLevel = gScene.lightLevelAt(surfacePosW);
    }

    // Shared legacy blend math (see BlendModes.slang): display-space source
    // modulation, then one decode to linear. The modulated .a is the legacy
    // display-alpha discard operand.
    float4 modulated    = applyBlendModulation(pc.blendMode, diffuseColor,
                                               finalAlpha, lightLevel);
    float  displayAlpha = modulated.a;
    float4 finalColor   = encodeBlendOutputLinear(pc.blendMode, modulated);

    if (displayAlpha < 0.01f)
    {
        discard;
    }

    return finalColor;
}
