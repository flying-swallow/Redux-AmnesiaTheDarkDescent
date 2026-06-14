// Hand-written Metal port of amnesia/slang/DecalPass/Decal.vert.slang
//
// Decal overlay vertex stage. Uses fixed-function vertex bindings (the same
// 5-stream layout as the translucent mesh path, declared in DecalPipelineDesc):
//   binding/attribute 0 : position  (R32G32B32_SFLOAT)
//   binding/attribute 1 : normal    (R32G32B32_SFLOAT)      — bound, unused
//   binding/attribute 2 : tangent   (R32G32B32A32_SFLOAT)   — bound, unused
//   binding/attribute 3 : color     (R32G32B32A32_SFLOAT)
//   binding/attribute 4 : texcoord  (R32G32_SFLOAT, engine stream float3, .xy)
// SV_InstanceID + SV_StartInstanceLocation reconstruct the bindless OBJECT slot
// id, which indexes gSceneObjects for the UniformObject (model / uv matrix).
//
// Bindings:
//   set0 (bindless arg buffer) buffer(0); [[id]] = kBinding* (Constants.h):
//     gSceneObjects 20
//   gPerFrame (SceneConstants) set1 buffer(1) [[id(0)]]
//
// MatStorage (modelMat / uvMat) -> loadMat yields a column-vector float4x4, so
// `M * v` == Slang `mul(M, v)` (same convention as Translucent.vert.metal).
// SceneConstants.viewMat/projMat are native MSL float4x4, also `M * v`.
#include <metal_stdlib>
using namespace metal;

#include "SceneTypes.metalh"  // SceneConstants, MatStorage, UniformObject

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

// loadMat (MatStorage -> column-vector float4x4) — included AFTER struct Set0
// since the header references `device Set0&` (same pattern as Translucent.vert.metal).
#include "SurfelGI/BindlessTriangle.metalh"   // loadMat

struct DecalVSIn {
    float3 a_position [[attribute(0)]];
    float3 a_normal   [[attribute(1)]];  // unused
    float4 a_tangent  [[attribute(2)]];  // unused
    float4 a_color    [[attribute(3)]];
    float2 a_texcoord [[attribute(4)]];
};

struct DecalVSOut {
    float4 posH                   [[position]];
    float2 uv;
    float4 color;
    uint   objectId [[flat]];
};

vertex DecalVSOut vsMain(DecalVSIn vin [[stage_in]],
                         uint drawInst [[instance_id]],
                         uint baseInst [[base_instance]],
                         device Set0& set0 [[buffer(0)]],
                         device Set1& set1 [[buffer(1)]]) {
    uint instanceId = drawInst + baseInst;

    UniformObject obj = set0.gSceneObjects[instanceId];
    float4x4 modelMat = loadMat(obj.modelMat);
    float4x4 uvMat    = loadMat(obj.uvMat);

    constant SceneConstants* gPerFrame = set1.gPerFrame;
    // Slang: modelViewPrj = mul(projMat, mul(viewMat, modelMat)); chain as M * v.
    float4x4 modelViewPrj = gPerFrame->projMat * gPerFrame->viewMat * modelMat;

    DecalVSOut o;
    o.uv       = (uvMat * float4(vin.a_texcoord, 0.0f, 1.0f)).xy;
    o.color    = vin.a_color;   // raw authored vertex tint (frag runs legacy display-space math)
    o.objectId = instanceId;
    o.posH     = modelViewPrj * float4(vin.a_position, 1.0f);
    return o;
}
