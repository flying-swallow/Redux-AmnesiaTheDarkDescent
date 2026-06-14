// Hand-written Metal port of amnesia/slang/WaterPass/Water.vert.slang
//
// Raster water surface vertex stage. Same fixed-function 5-stream layout +
// SV_InstanceID -> bindless OBJECT slot as Translucent.vert, but outputs
// WORLD-space position / normal / tangent (the frag's wave normal + inline RT
// reflection are world-space).
//
// 5-stream layout (matches WaterPipelineDesc / Translucent):
//   attribute 0:position 1:normal 2:tangent(w=handedness) 3:color 4:texcoord(.xy)
//
// Bindings:
//   set0 (bindless arg buffer) buffer(0); [[id]] = kBinding* (Constants.h):
//     gSceneObjects 20
//   gPerFrame buffer(1)
#include <metal_stdlib>
using namespace metal;

#include "SceneTypes.metalh"   // SceneConstants, MatStorage, UniformObject

struct Set0 {
    // BDA vertex/index handles live in the persistent bindless set 0 — declared
    // so the shared BindlessTriangle.metalh (included below for loadMat) compiles;
    // this fixed-stream vertex stage does not fetch geometry through them.
    device ulong*         gOpaquePositionHandles [[id(3)]];
    device ulong*         gOpaqueTangentHandles  [[id(4)]];
    device ulong*         gOpaqueNormalHandles   [[id(5)]];
    device ulong*         gOpaqueUv0Handles      [[id(6)]];
    device ulong*         gOpaqueIndexHandles    [[id(8)]];
    device UniformObject* gSceneObjects          [[id(20)]];
};
struct Set1 {
    constant SceneConstants* gPerFrame [[id(0)]];
};

#include "SurfelGI/BindlessTriangle.metalh"   // loadMat

struct VSIn {
    float3 a_position [[attribute(0)]];
    float3 a_normal   [[attribute(1)]];
    float4 a_tangent  [[attribute(2)]];
    float4 a_color    [[attribute(3)]];
    float2 a_texcoord [[attribute(4)]];
};

struct VSOut {
    float3 posW;
    float3 normalW;
    float3 tangentW;
    float2 uv;
    uint   objectId [[flat]];   // nointerpolation
    float4 posH     [[position]];
};

vertex VSOut vsMain(VSIn vin [[stage_in]],
                    uint drawInst [[instance_id]],
                    uint baseInst [[base_instance]],
                    device Set0& set0 [[buffer(0)]],
                    device Set1& set1 [[buffer(1)]]) {
    constant SceneConstants* gPerFrame = set1.gPerFrame;

    uint instanceId = drawInst + baseInst;
    UniformObject obj = set0.gSceneObjects[instanceId];

    float4x4 modelMat    = loadMat(obj.modelMat);
    float4x4 invModelMat = loadMat(obj.invModelMat);
    float4x4 uvMat       = loadMat(obj.uvMat);

    float4x4 modelView    = gPerFrame->viewMat * modelMat;
    float4x4 modelViewPrj = gPerFrame->projMat * modelView;

    // World-space normal matrix = inverse-transpose of model 3x3 = transpose(invModelRot).
    float3x3 invModelRot = float3x3(invModelMat[0].xyz, invModelMat[1].xyz, invModelMat[2].xyz);
    float3x3 normalMat   = transpose(invModelRot);
    float3x3 modelRot    = float3x3(modelMat[0].xyz, modelMat[1].xyz, modelMat[2].xyz);

    VSOut o;
    o.posW     = (modelMat * float4(vin.a_position, 1.0f)).xyz;
    o.normalW  = normalize(normalMat * vin.a_normal);
    o.tangentW = normalize(modelRot * vin.a_tangent.xyz);
    o.uv       = (uvMat * float4(vin.a_texcoord, 0.0f, 1.0f)).xy;
    o.objectId = instanceId;
    o.posH     = modelViewPrj * float4(vin.a_position, 1.0f);
    return o;
}
