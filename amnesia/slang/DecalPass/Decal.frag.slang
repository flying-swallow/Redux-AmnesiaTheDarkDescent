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
// Same scheme as Particle/Translucent.frag, shared via BlendModes.slang:
// re-encode the texel so the source product runs in the authored display
// space, then decode ONCE at output per blend mode. Critical for MULX2
// decals (web_spider01 etc.): their authored 0.5-gray neutral must reach the
// 2·src·dst blender as 0.5 — the plain sRGB-decoded sample (0.214) darkened
// the whole quad to 0.43×dst and drew the quad border. Decals have no
// fog/sceneAlpha/lightLevel, so the modulation stage is the identity and
// only the decode (encodeBlendOutputLinear) runs.

#include "../Constants.h"

import bindless;
import SceneTypes;
import Scene;
import BlendModes;   // displaySpaceSample / encodeBlendOutputLinear / kBlendMode*

#define DECAL_ALPHA_REJECT 0.01f

// Mirrors the PushBlock written at the decal draw site in cHybridRenderer
// (DecalPipelineDesc::BlendMode — same value layout as kBlendMode*).
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
        float4 s = sampleBindless2D(diffuseTex, psIn.uv);
        diffuseColor = displaySpaceSample(s);
    }
    diffuseColor *= psIn.color;

    if (diffuseColor.w < DECAL_ALPHA_REJECT)
    {
        discard;
    }

    // Decode to linear once per blend mode — shared with Particle/Translucent
    // (see BlendModes.slang for the derivation).
    return encodeBlendOutputLinear(pc.blendMode, diffuseColor);
}
