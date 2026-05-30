/// Copyright © 2009-2020 Frictional Games
/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Slang port of AmnesiaTheDarkDescent/HPL2/resource/decal.vert.fsl.
//
// Decals are a thin post-geometry overlay: a clipped mesh carrying
// position / uv / vertex-color, projected with the object's model matrix and
// the per-object uvMat, depth-tested (≤) against the GBuffer depth but never
// writing depth. The fragment shader (Decal.frag.slang) samples the diffuse
// map and multiplies by vertex colour — no lighting, fog, or reflection.
//
// Like the translucent mesh path this uses **fixed-function vertex bindings**
// (vkCmdBindVertexBuffers) declared in DecalPipelineDesc, and reuses the same
// 5-stream layout so the existing translucent fallback buffers and draw loop
// apply unchanged. Only position / texcoord / color are consumed here; normal
// and tangent are bound (with fallbacks) but ignored.
//
// SV_InstanceID + SV_StartInstanceLocation reconstruct the bindless OBJECT
// slot id (firstInstance), which indexes gSceneObjects for the UniformObject
// (model / uv matrix, materialID). Same fix as Translucent.vert.slang.

import Bindless.resource;
import SceneTypes;
import PerFrame.resource;
import Utils.Color.ColorHelpers;   // adobeRGBToLinear

// Per-vertex inputs — match the bindings declared in DecalPipelineDesc
// (identical layout to TranslucentMeshPipelineDesc):
//   binding 0 / location 0 : position  (R32G32B32_SFLOAT, stride 16)
//   binding 1 / location 1 : normal    (R32G32B32_SFLOAT, stride 12)  — unused
//   binding 2 / location 2 : tangent   (R32G32B32A32_SFLOAT, stride 16) — unused
//   binding 3 / location 3 : color     (R32G32B32A32_SFLOAT, stride 16)
//   binding 4 / location 4 : texcoord  (R32G32_SFLOAT, stride 12 — engine stream is float3, only .xy read)
struct DecalVSIn
{
    float3 a_position : POSITION;
    float3 a_normal   : NORMAL;
    float4 a_tangent  : TANGENT;
    float4 a_color    : COLOR;
    float2 a_texcoord : TEXCOORD0;
};

struct DecalVSOut
{
    float2 uv                      : TEXCRD;
    float4 color                   : COLOR;
    nointerpolation uint objectId  : OBJECTID;
    float4 posH                    : SV_POSITION;
};

[shader("vertex")]
DecalVSOut vsMain(DecalVSIn vin,
                  uint drawInst : SV_InstanceID,
                  uint baseInst : SV_StartInstanceLocation)
{
    uint instanceId = drawInst + baseInst;

    UniformObject obj     = gSceneObjects[instanceId];
    float4x4 modelViewPrj = mul(gPerFrame.projMat,
                                mul(gPerFrame.viewMat, obj.modelMat));

    DecalVSOut vsOut;
    vsOut.uv       = mul(obj.uvMat, float4(vin.a_texcoord, 0.0f, 1.0f)).xy;
    // Decal vertex color (DecalCreator's cColor) is authored in Adobe RGB (1998);
    // decode to linear so the multiply/blend into the LINEAR albedo buffer is
    // correct (a raw perceptual tint reads too bright in its low channels). Alpha
    // is coverage/fade — leave it.
    vsOut.color    = float4(adobeRGBToLinear(vin.a_color.rgb), vin.a_color.a);
    vsOut.objectId = instanceId;
    vsOut.posH     = mul(modelViewPrj, float4(vin.a_position, 1.0f));
    return vsOut;
}
