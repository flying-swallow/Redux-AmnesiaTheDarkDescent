// Hand-written Metal port of amnesia/slang/TranslucentPass/Translucent.vert.slang
//
// Raster translucent (mesh) vertex stage. Fixed-function vertex fetch via MSL
// [[stage_in]] (5 streams, matching TranslucentMeshPipelineDesc / Translucent.vert.slang):
//   binding 0 / attribute 0 : position  (float3)
//   binding 1 / attribute 1 : normal    (float3)
//   binding 2 / attribute 2 : tangent   (float4, w = handedness)
//   binding 3 / attribute 3 : color     (float4)
//   binding 4 / attribute 4 : texcoord  (float2 — engine stream is float3, only .xy read)
//
// SV_InstanceID + SV_StartInstanceLocation -> [[instance_id]] + [[base_instance]];
// the sum reconstructs gl_InstanceIndex (bindless OBJECT slot), used to fetch
// UniformObject (model matrix / uv matrix / materialID).
//
// Bindings:
//   set0 (bindless arg buffer) buffer(0); [[id]] = kBinding* (Constants.h):
//     gSceneObjects 20
//   gPerFrame buffer(1)
#include <metal_stdlib>
using namespace metal;

// --- shared struct layouts (flattened in by cmake/flatten_metal_includes.py) ---
#include "SceneTypes.metalh"   // SceneConstants, MatStorage, UniformObject

// Set-0 bindless argument buffer (subset this stage reads).
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

// loadMat (MatStorage -> column-major float4x4) — needed for the per-object
// matrices stored as MatStorage in UniformObject.
#include "SurfelGI/BindlessTriangle.metalh"   // loadMat

struct VSIn {
    float3 a_position [[attribute(0)]];
    float3 a_normal   [[attribute(1)]];
    float4 a_tangent  [[attribute(2)]];
    float4 a_color    [[attribute(3)]];
    float2 a_texcoord [[attribute(4)]];
};

struct VSOut {
    float3 viewPos;                            // view-space position (fog + refraction invDist)
    float2 uv;
    float3 normal;                             // view-space normal
    float3 tangent;                            // view-space tangent
    float3 bitangent;                          // view-space bitangent
    float4 color;
    uint   objectId  [[flat]];                 // nointerpolation
    float4 posH      [[position]];
};

vertex VSOut vsMain(VSIn vin [[stage_in]],
                    uint drawInst [[instance_id]],
                    uint baseInst [[base_instance]],
                    device Set0& set0 [[buffer(0)]],
                    device Set1& set1 [[buffer(1)]]) {
    constant SceneConstants* gPerFrame = set1.gPerFrame;

    // SV_InstanceID + firstInstance: reconstruct gl_InstanceIndex (bindless slot).
    uint instanceId = drawInst + baseInst;

    UniformObject obj = set0.gSceneObjects[instanceId];
    float4x4 modelMat    = loadMat(obj.modelMat);
    float4x4 invModelMat = loadMat(obj.invModelMat);
    float4x4 uvMat       = loadMat(obj.uvMat);

    float4x4 modelView    = gPerFrame->viewMat * modelMat;
    float4x4 modelViewPrj = gPerFrame->projMat * modelView;

    // Normal matrix = (modelView^-1)^T = invView^T * invModel^T.
    float3x3 invModelRot = float3x3(invModelMat[0].xyz, invModelMat[1].xyz, invModelMat[2].xyz);
    float3x3 invViewRot  = float3x3(gPerFrame->invViewMat[0].xyz, gPerFrame->invViewMat[1].xyz, gPerFrame->invViewMat[2].xyz);
    float3x3 normalMat   = transpose(invViewRot) * transpose(invModelRot);

    VSOut o;
    o.viewPos   = (modelView * float4(vin.a_position, 1.0f)).xyz;
    o.uv        = (uvMat * float4(vin.a_texcoord, 0.0f, 1.0f)).xy;
    o.normal    = normalize(normalMat * vin.a_normal);
    o.tangent   = normalize(normalMat * vin.a_tangent.xyz);
    // Bitangent = cross(tangent, normal) * handedness (engine tangent.w convention).
    o.bitangent = normalize(normalMat * (cross(vin.a_tangent.xyz, vin.a_normal) * vin.a_tangent.w));
    o.color     = vin.a_color;
    o.objectId  = instanceId;
    o.posH      = modelViewPrj * float4(vin.a_position, 1.0f);
    return o;
}
