// Hand-written Metal port of amnesia/slang/ParticlePass/Particle.vert.slang
//
// Bindless particle vertex stage. No fixed vertex stream: the VS pulls
// position / uv0 / color from per-stream BDA handle arrays on set 0
// (gOpaque*Handles), selecting the slot with SV_InstanceID + SV_StartInstance.
// BDA accesses use direct primitive pointer casts (float4*/float3*/uint*) — the
// engine stores MTL gpuAddress in gOpaque*Handles and keeps the geometry
// resident. Scalar buffer layout means position/color step 16B, uv0 steps 12B
// (only .xy read) — matching iParticleEmitter strides.
//
// Bindings:
//   set0 (bindless arg buffer) buffer(0); [[id]] = kBinding* (Constants.h):
//     gOpaquePositionHandles 3  gOpaqueUv0Handles 6  gOpaqueColorHandles 7
//     gOpaqueIndexHandles 8  gSceneObjects 20
//   gPerFrame (SceneConstants) set1 buffer(1) [[id(0)]]
//
// MatStorage (UniformObject.modelMat / uvMat) reconstructs via loadMat to a
// column-vector float4x4, so `M * v` reproduces Slang `mul(M, v)` (same as the
// sibling Translucent.vert.metal). SceneConstants.viewMat/projMat are native
// MSL float4x4 and also use `M * v`.
#include <metal_stdlib>
using namespace metal;

#include "SceneTypes.metalh"  // SceneConstants, MatStorage, UniformObject

struct Set0 {
    device ulong*         gOpaquePositionHandles [[id(3)]];
    // Tangent/Normal handles unused here, declared so the shared
    // BindlessTriangle.metalh (included below for loadMat) compiles.
    device ulong*         gOpaqueTangentHandles  [[id(4)]];
    device ulong*         gOpaqueNormalHandles   [[id(5)]];
    device ulong*         gOpaqueUv0Handles      [[id(6)]];
    device ulong*         gOpaqueColorHandles    [[id(7)]];
    device ulong*         gOpaqueIndexHandles    [[id(8)]];
    device UniformObject* gSceneObjects          [[id(20)]];
};

struct Set1 {
    constant SceneConstants* gPerFrame [[id(0)]];
};

// loadMat (MatStorage -> column-vector float4x4) — included AFTER struct Set0
// since the header references `device Set0&` (same pattern as Translucent.vert.metal).
#include "SurfelGI/BindlessTriangle.metalh"   // loadMat

struct ParticleVSOut {
    float4 posH                   [[position]];
    float3 viewPos;       // view-space position (for fog + lightLevelAt posW)
    float2 uv;
    float4 color;
    uint   objectId [[flat]];
};

vertex ParticleVSOut vsMain(uint vertexId [[vertex_id]],
                            uint drawInst  [[instance_id]],
                            uint baseInst  [[base_instance]],
                            device Set0& set0 [[buffer(0)]],
                            device Set1& set1 [[buffer(1)]]) {
    // Reconstruct the bindless slot id: Metal's [[instance_id]] is per-draw
    // (already base-relative), [[base_instance]] carries the slot id.
    uint instanceId = drawInst + baseInst;

    ulong indexAddr = set0.gOpaqueIndexHandles[instanceId];
    ulong posAddr   = set0.gOpaquePositionHandles[instanceId];
    ulong uv0Addr   = set0.gOpaqueUv0Handles[instanceId];
    ulong colAddr   = set0.gOpaqueColorHandles[instanceId];

    uint idx = (indexAddr != 0ul)
        ? ((device uint*)indexAddr)[vertexId]
        : vertexId;

    float3 a_position = (posAddr != 0ul)
        ? ((device float4*)posAddr)[idx].xyz
        : float3(0.0f);
    float2 a_texcoord = (uv0Addr != 0ul)
        ? ((device float3*)uv0Addr)[idx].xy
        : float2(0.0f);
    float4 a_color = (colAddr != 0ul)
        ? ((device float4*)colAddr)[idx]
        : float4(1.0f);

    UniformObject obj  = set0.gSceneObjects[instanceId];
    float4x4 modelMat  = loadMat(obj.modelMat);
    float4x4 uvMat     = loadMat(obj.uvMat);

    constant SceneConstants* gPerFrame = set1.gPerFrame;
    // Slang: modelView = mul(viewMat, modelMat); modelViewPrj = mul(projMat, modelView).
    // loadMat yields a column-vector matrix, so chain as M * v (same convention
    // as Translucent.vert.metal).
    float4x4 modelView    = gPerFrame->viewMat * modelMat;
    float4x4 modelViewPrj = gPerFrame->projMat * modelView;

    ParticleVSOut o;
    o.viewPos  = (modelView * float4(a_position, 1.0f)).xyz;
    o.uv       = (uvMat * float4(a_texcoord, 0.0f, 1.0f)).xy;
    o.color    = a_color;
    o.objectId = instanceId;
    o.posH     = modelViewPrj * float4(a_position, 1.0f);
    return o;
}
