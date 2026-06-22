// Raster G-buffer pass — Slang port of amnesia/glsl/gbuffer.{vert,frag}.
//
// vsMain pulls position / uv from per-instance VkDeviceAddress arrays
// (gOpaque*Handles in bindless), keyed by SV_VertexID +
// SV_InstanceID. The pipeline is declared with zero VkVertexInputBindings;
// indexing is done in-shader via the optional per-instance index buffer,
// matching the GLSL twin.
//
// psMain alpha-tests against the diffuse material's alpha slot, then writes
// the closest hit (instanceID, primitiveIndex, barycentrics) into the
// project's packed TriangleHit format — uint4 with the same layout
// surfel_vbuffer.rgen produces and surfel_generation_pass.comp /
// surfel_update_collect.comp consume via unpackHit().

import Scene;
import SceneTypes;
import bindless;

// Per-stream BDA reads use direct primitive pointer casts (`float4*`,
// `float3*`, `uint*`). The wrapper-struct-with-runtime-array pattern that
// the GLSL twin uses via `buffer_reference` (and that BindlessTriangle.slang
// still uses) silently miscompiles in slangc 2026+: all
// `((Wrapper*)addr).v[i]` sites collapse to `Ptr<PhysicalStorageBuffer, uint>`
// at the SPIR-V level, so the load yields zero. `[vk::buffer_reference]` is
// also unrecognized in 2026+. Primitive pointer types translate correctly
// (see Slang docs: a2-01-spirv-target-specific.md "Global memory pointers").

struct VSOut
{
    float4 posH                          : SV_POSITION;
    float2 texC                          : TEXCOORD0;
    nointerpolation uint instanceID      : INSTANCE_ID;
    nointerpolation uint materialID      : MATERIAL_ID;
    // No-jitter clip positions this frame and last frame, for motion vectors.
    // Interpolated (perspective-correct), divided by w in the FS.
    float4 curClip                       : CUR_CLIP;
    float4 prevClip                      : PREV_CLIP;
}

// Two render targets: packed TriangleHit + screen-space velocity (curUV-prevUV).
struct GBufferOut
{
    uint4  hit      : SV_TARGET0;
    float2 velocity : SV_TARGET1;
};

[shader("vertex")]
VSOut vsMain(uint vertexID    : SV_VertexID,
             uint drawInst    : SV_InstanceID,
             uint baseInst    : SV_StartInstanceLocation)
{
    // SV_InstanceID alone is the per-draw instance counter (always 0 here
    // since instanceCount=1) — slangc translates it with DX semantics
    // (InstanceIndex - BaseInstance). The bindless slot id arrives via
    // firstInstance, so reconstruct Vulkan's gl_InstanceIndex by summing.
    uint instanceID = drawInst + baseInst;
    VSOut o;

    // Stream BDAs now live in the per-object UniformObject (folded in from the
    // former gOpaque*Handles buffers).
    UniformObject obj = gScene.getObject(instanceID);
    uint64_t indexAddr = obj.indexHandle;
    uint64_t posAddr   = obj.posHandle;
    uint64_t uv0Addr   = obj.uv0Handle;

    // Missing index buffer → non-indexed (each vertexID is its own vertex).
    // Missing attribute streams → safe zero so the transform doesn't NaN out.
    uint idx = (indexAddr != 0ull)
        ? ((uint*)indexAddr)[vertexID]
        : vertexID;

    float3 aPos = (posAddr != 0ull)
        ? ((float4*)posAddr)[idx].xyz
        : float3(0.0f);
    // UV0 stream is tightly packed vec3 (12-byte stride) — float3* indexing
    // depends on that. If asset packing changes, switch to a uint-stride load.
    float2 aUv  = (uv0Addr != 0ull)
        ? ((float3*)uv0Addr)[idx].xy
        : float2(0.0f);

    float4 worldP     = mul(obj.modelMat,     float4(aPos, 1.0f));
    float4 prevWorldP = mul(obj.prevModelMat, float4(aPos, 1.0f));
    o.posH       = mul(gScene.camera.getViewProj(), worldP);
    o.curClip    = mul(gScene.camera.getViewProjNoJitter(), worldP);
    o.prevClip   = mul(gScene.camera.getPrevViewProj(), prevWorldP);
    o.texC       = mul(obj.uvMat, float4(aUv, 0.0f, 1.0f)).xy;
    o.instanceID = instanceID;
    o.materialID = gScene.getMaterialID(instanceID);
    return o;
}

[shader("pixel")]
GBufferOut psMain(VSOut vsOut,
                 uint   primitiveID : SV_PrimitiveID,
                 float3 barys       : SV_Barycentrics)
{
    // Diffuse alpha gate + CoverageAmount dissolve — same reject the legacy
    // z-prepass applied before any pixel could survive (alpha cutout plus the
    // noise dissolve fade, solid_z.frag.fsl). Discarded pixels write neither
    // target.
    VertexData v = {};
    v.texC = vsOut.texC;
    if (gScene.materials.alphaTest(v, vsOut.materialID, 0.0f,
                                   gSceneObjects[vsOut.instanceID].dissolveAmount))
        discard;

    GBufferOut o;

    // Packed TriangleHit — must match SurfelVBuffer.rt's pack() so the surfel
    // passes and the composite decode raster + RT hits identically:
    //   .x = instanceID  .y = primitiveIndex  .z = packHalf2x16(b1,b2)  .w = 0
    uint b1 = f32tof16(barys.y);
    uint b2 = f32tof16(barys.z);
    o.hit.x = vsOut.instanceID;
    o.hit.y = primitiveID;
    o.hit.z = b1 | (b2 << 16);
    o.hit.w = 0u;

    // Screen-space velocity = curUV - prevUV (resolve samples history at
    // uv - velocity). NDC→UV uses the engine's y-down convention
    // (uv = ndc * (0.5,-0.5) + 0.5, see computeNonNormalizedRayDirPinhole).
    float2 curUV  = (vsOut.curClip.xy  / vsOut.curClip.w)  * float2(0.5f, -0.5f) + 0.5f;
    float2 prevUV = (vsOut.prevClip.xy / vsOut.prevClip.w) * float2(0.5f, -0.5f) + 0.5f;
    o.velocity = curUV - prevUV;
    return o;
}
