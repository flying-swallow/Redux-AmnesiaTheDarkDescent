/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Raster water surface — vertex stage. Mirrors Translucent.vert's fixed-function
// 5-stream layout + SV_InstanceID → bindless OBJECT slot, but outputs WORLD-space
// position / normal / tangent (the frag's wave normal + inline RT reflection are
// world-space, like the ray-tracing passes). Drawn over the refracted background already in
// the pogo (the V-buffer's water refraction clobbered the primary hit).

import bindless;
import SceneTypes;
import PerFrame.resource;

// 5-stream layout (matches WaterPipelineDesc / Translucent):
//   0:position 1:normal 2:tangent(w=handedness) 3:color 4:texcoord(.xy)
struct WaterVSIn
{
    float3 a_position : POSITION;
    float3 a_normal   : NORMAL;
    float4 a_tangent  : TANGENT;
    float4 a_color    : COLOR;
    float2 a_texcoord : TEXCOORD0;
};

struct WaterVSOut
{
    float3 posW                    : POSW;
    float3 normalW                 : NORMAL;
    float3 tangentW                : TANGENT;
    float2 uv                      : TEXCRD;
    nointerpolation uint objectId  : OBJECTID;
    float4 posH                    : SV_POSITION;
};

[shader("vertex")]
WaterVSOut vsMain(WaterVSIn vin,
                  uint drawInst : SV_InstanceID,
                  uint baseInst : SV_StartInstanceLocation)
{
    uint instanceId = drawInst + baseInst;
    UniformObject obj = gSceneObjects[instanceId];

    float4x4 modelView    = mul(gPerFrame.viewMat, obj.modelMat);
    float4x4 modelViewPrj = mul(gPerFrame.projMat, modelView);

    // World-space normal matrix = inverse-transpose of model 3x3 = transpose(invModelRot).
    float3x3 invModelRot = float3x3(obj.invModelMat);
    float3x3 normalMat   = transpose(invModelRot);

    WaterVSOut o;
    o.posW     = mul(obj.modelMat, float4(vin.a_position, 1.0f)).xyz;
    o.normalW  = normalize(mul(normalMat, vin.a_normal));
    o.tangentW = normalize(mul((float3x3)obj.modelMat, vin.a_tangent.xyz));
    o.uv       = mul(obj.uvMat, float4(vin.a_texcoord, 0.0f, 1.0f)).xy;
    o.objectId = instanceId;
    o.posH     = mul(modelViewPrj, float4(vin.a_position, 1.0f));
    return o;
}
