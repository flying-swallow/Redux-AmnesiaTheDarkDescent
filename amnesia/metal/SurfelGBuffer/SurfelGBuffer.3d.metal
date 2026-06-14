// Hand-written Metal port of amnesia/slang/SurfelGBuffer/SurfelGBuffer.3d.slang
//
// Raster G-buffer pass — both stage functions live in this single MSL library:
//   vsMain (vertex)  : BINDLESS-pull vertex stage. There are NO vertex input
//                      streams; position / uv are pulled from per-instance
//                      buffer-device-address arrays (gOpaque*Handles in Set0),
//                      keyed by [[vertex_id]] + [[instance_id]] + [[base_instance]],
//                      exactly mirroring the slang vsMain (slang lines 47-89).
//   psMain (fragment): diffuse alpha + dissolve gate (Scene.alphaTest), then
//                      writes the packed TriangleHit V-buffer + screen velocity.
//
// The packed TriangleHit output (uint4: .x=instanceID, .y=primitiveID,
// .z=packHalf2x16(b1,b2), .w=0) MUST match SurfelVBuffer.rt.metal's
// packTriangleHit() so the surfel passes decode raster + RT hits identically.
//
// Bindings:
//   set0 (bindless arg buffer) buffer(0); [[id]] == VK kBinding* (Constants.h):
//     gOpaquePositionHandles 3  gOpaqueUv0Handles 6  gOpaqueIndexHandles 8
//     gMaterialSampler 9  gSceneObjects 20  gDiffuseMaterials 21
//     gDissolveMap 48  gTextures2D 49 (kBindingTextures2D, remapped)
//   gPerFrame (SceneConstants) buffer(1) [[id(0)]]  (Set1, like the RT/compute ports)
//
// MRT output: color(0) = packed TriangleHit (uint4); color(1) = velocity (float2).
#include <metal_stdlib>
using namespace metal;

// --- shared constants (mirror amnesia/slang/Constants.h) ---------------------
constant uint kInvalidTextureIndex   = 0xffffffffu;
constant uint kSolidMaterialCapacity = 2048u;

constant uint kMaterialFlagEnableDissolveAlpha  = 1u << 7;
constant uint kMaterialFlagIsAlphaSingleChannel = 1u << 10;
constant uint kMaterialFlagUseDissolveFilter    = 1u << 14;

// --- Shared struct layouts: mirrored from the Slang modules (flattened in at
// stage time by cmake/flatten_metal_includes.py). ---
#include "SceneTypes.metalh"  // SceneConstants, MatStorage, UniformObject, VertexData

// Material table (mirror amnesia/slang/SceneTypes.slang DiffuseMaterial — scalar
// field order verbatim; same layout as SurfelVBuffer.rt.metal uses).
struct DiffuseMaterial { uint type; uint materialConfig; uint cubeMapTextureIndex; uint tex[8]; float heightMapScale, heightMapBias, frenselBias, frenselPow; };

// Set-0 bindless argument buffer (buffer(0)); [[id]] == VK kBinding*. Only the
// members this pass reads are modelled. gTextures2D rides LAST (its 16384-element
// array consumes consecutive ids); the C++ encoder remaps id(49) to
// kBindingTextures2D, exactly like SurfelVBuffer.rt.metal.
struct Set0 {
    device ulong*           gOpaquePositionHandles [[id(3)]];
    // Tangent/Normal handles unused by this pass, declared so the shared
    // BindlessTriangle.metalh compiles (its getVertexData references all five).
    device ulong*           gOpaqueTangentHandles  [[id(4)]];
    device ulong*           gOpaqueNormalHandles   [[id(5)]];
    device ulong*           gOpaqueUv0Handles      [[id(6)]];
    device ulong*           gOpaqueIndexHandles    [[id(8)]];
    sampler                 gMaterialSampler       [[id(9)]];
    device UniformObject*   gSceneObjects          [[id(20)]];
    device DiffuseMaterial* gDiffuseMaterials      [[id(21)]];
    texture2d<float>        gDissolveMap           [[id(48)]];
    array<texture2d<float>, 16384> gTextures2D     [[id(49)]];  // kBindingTextures2D (remapped)
};

// Set-1 program argument buffer (buffer(1)); per-frame Scene camera constants,
// id(0) as in the RT/compute ports.
struct Set1 {
    constant SceneConstants* gPerFrame [[id(0)]];
};

// Utility headers that reference `device Set0&` must come AFTER struct Set0.
// loadMat / getObject / getMaterialID / camera getViewProj*/alphaTest.
#include "SurfelGI/BindlessTriangle.metalh"  // loadMat (used by Scene.metalh)
#include "Scene.metalh"             // getObject, getMaterialID, cameraGetViewProj*, alphaTest

// --- interpolated vertex payload (mirror slang VSOut) ------------------------
struct VSOut {
    float4 posH [[position]];
    float2 texC;
    // SV_InstanceID / SV_MaterialID are nointerpolation in slang.
    uint   instanceID [[flat]];
    uint   materialID [[flat]];
    // No-jitter clip positions this frame and last, perspective-correct
    // interpolated, divided by w in the fragment stage.
    float4 curClip;
    float4 prevClip;
};

// --- MRT output (mirror slang GBufferOut) ------------------------------------
struct GBufferOut {
    uint4  hit      [[color(0)]];
    float2 velocity [[color(1)]];
};

// =============================================================================
// Vertex stage — bindless pull (mirror slang vsMain, lines 47-89).
// =============================================================================
vertex VSOut vsMain(uint vertexID  [[vertex_id]],
                    uint drawInst   [[instance_id]],
                    uint baseInst   [[base_instance]],
                    device Set0& set0 [[buffer(0)]],
                    device Set1& set1 [[buffer(1)]]) {
    constant SceneConstants* gPerFrame = set1.gPerFrame;

    // SV_InstanceID is the per-draw counter (0 here, instanceCount==1); the
    // bindless slot id arrives via firstInstance ([[base_instance]]), so
    // reconstruct Vulkan's gl_InstanceIndex by summing (mirrors slang line 56).
    uint instanceID = drawInst + baseInst;
    VSOut o;

    ulong indexAddr = set0.gOpaqueIndexHandles[instanceID];
    ulong posAddr   = set0.gOpaquePositionHandles[instanceID];
    ulong uv0Addr   = set0.gOpaqueUv0Handles[instanceID];

    // Missing index buffer -> non-indexed (each vertexID is its own vertex).
    // Cannot cast a scalar ulong straight to a pointer; assign via (device T*).
    uint idx = vertexID;
    if (indexAddr != 0ul) {
        device uint* ib = (device uint*)indexAddr;
        idx = ib[vertexID];
    }

    float3 aPos = float3(0.0f);
    if (posAddr != 0ul) {
        device float4* pb = (device float4*)posAddr;
        aPos = pb[idx].xyz;
    }

    // UV0 stream is tightly packed vec3 (12-byte stride) — float3* indexing
    // depends on that, matching the slang twin.
    float2 aUv = float2(0.0f);
    if (uv0Addr != 0ul) {
        device packed_float3* ub = (device packed_float3*)uv0Addr;
        aUv = float3(ub[idx]).xy;
    }

    UniformObject obj = getObject(instanceID, set0);

    // mul(M, v) -> M * v (column-major). loadMat reconstructs the matrix exactly
    // as the slang lowering does.
    float4x4 modelMat     = loadMat(obj.modelMat);
    float4x4 prevModelMat = loadMat(obj.prevModelMat);
    float4x4 uvMat        = loadMat(obj.uvMat);

    float4 worldP     = modelMat     * float4(aPos, 1.0f);
    float4 prevWorldP = prevModelMat * float4(aPos, 1.0f);
    o.posH       = cameraGetViewProj(gPerFrame)        * worldP;
    o.curClip    = cameraGetViewProjNoJitter(gPerFrame) * worldP;
    o.prevClip   = cameraGetPrevViewProj(gPerFrame)    * prevWorldP;
    o.texC       = (uvMat * float4(aUv, 0.0f, 1.0f)).xy;
    o.instanceID = instanceID;
    o.materialID = getMaterialID(instanceID, set0);
    return o;
}

// =============================================================================
// Fragment stage — alpha gate + packed TriangleHit + velocity (mirror psMain).
//
// NOTE (Metal-availability HARD POINT): the slang psMain consumes SV_PrimitiveID
// and SV_Barycentrics. MSL exposes these as fragment inputs ONLY on supported
// GPUs / feature sets: [[primitive_id]] and [[barycentric_coord]] require
// Metal 2.3+ (and a GPU family that advertises barycentric-coordinate support,
// e.g. Apple7+/Mac2). On older targets this shader will fail to compile or the
// values are unavailable — the host must gate this pass on that capability (or
// fall back to the RT V-buffer path). The barycentric_coord vector is (b0,b1,b2)
// matching the three triangle vertices; b1/b2 map to SV_Barycentrics.yz, which
// is what the surfel decode (getTriangleHit) reconstructs.
// =============================================================================
fragment GBufferOut psMain(VSOut vsOut [[stage_in]],
                           uint   primitiveID [[primitive_id]],
                           float3 barys       [[barycentric_coord]],
                           device Set0& set0  [[buffer(0)]]) {
    // Diffuse alpha gate + CoverageAmount dissolve — same reject the legacy
    // z-prepass applied. Discarded pixels write neither target.
    VertexData v;
    v.posW = float3(0.0f);
    v.normalW = float3(0.0f);
    v.tangentW = float3(0.0f);
    v.texC = vsOut.texC;
    if (alphaTest(v, vsOut.materialID, 0.0f,
                  set0.gSceneObjects[vsOut.instanceID].dissolveAmount, set0))
        discard_fragment();

    GBufferOut o;

    // Packed TriangleHit — must match SurfelVBuffer.rt.metal's packTriangleHit()
    // so the surfel passes / composite decode raster + RT hits identically:
    //   .x = instanceID  .y = primitiveID  .z = packHalf2x16(b1,b2)  .w = 0
    // f32tof16(x) -> bit-reinterpret a half as its 16-bit pattern:
    //   as_type<ushort>(half(x)), then pack low/high. getTriangleHit() inverts
    //   this with as_type<half>((ushort)(z & 0xFFFF)) etc.
    uint b1 = uint(as_type<ushort>(half(barys.y)));
    uint b2 = uint(as_type<ushort>(half(barys.z)));
    o.hit.x = vsOut.instanceID;
    o.hit.y = primitiveID;
    o.hit.z = b1 | (b2 << 16);
    o.hit.w = 0u;

    // Screen-space velocity = curUV - prevUV (resolve samples history at
    // uv - velocity). NDC->UV uses the engine's y-down convention.
    float2 curUV  = (vsOut.curClip.xy  / vsOut.curClip.w)  * float2(0.5f, -0.5f) + 0.5f;
    float2 prevUV = (vsOut.prevClip.xy / vsOut.prevClip.w) * float2(0.5f, -0.5f) + 0.5f;
    o.velocity = curUV - prevUV;
    return o;
}
