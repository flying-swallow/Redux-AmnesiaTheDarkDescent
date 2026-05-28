/// Copyright © 2009-2020 Frictional Games
/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Slang port of AmnesiaTheDarkDescent/HPL2/resource/translucency.vert.fsl,
// adapted for the bindless / BDA-fanout pipeline cHybridRenderer uses for
// every non-particle translucent mesh (glass, lamp glass, decals tagged
// translucent, etc.).
//
// Same shape as Particle.vert.slang — translucent meshes share the OBJECT_SLOT
// bindless pool with solid geometry and the particle pass, so cHybridRenderer
// writes the mesh vertex buffer's BDA into the same opaque*Handles[] arrays
// and SV_InstanceID selects the slot.
//
// Per-vertex stream strides match the opaque mesh layout (and happen to match
// the particle layout — see Particle.vert.slang for the particle-side
// rationale):
//   position : float x 4  (stride 16, only .xyz read)
//   color    : float x 4  (stride 16, full rgba)
//   texture0 : float x 3  (stride 12, only .xy read)
// `[vk::buffer_reference]` is scalar-layout in Slang — same memory model as
// GLSL `layout(buffer_reference, scalar)` — so float4/float3 match those
// strides exactly. SurfelGBuffer.3d.slang reads position/uv0 the same way.
//
// V1 deliberately does NOT read normal/tangent — the reference's full
// translucency.frag uses them for refraction sampling and cube-map Fresnel,
// neither of which is wired in the hybrid renderer yet (no screen-color copy,
// no reflection buffer). When that lands, mirror Particle.vert.slang's growth
// and extend the BDA reads here.

import Bindless.resource;
import SceneTypes;
import PerFrame.resource;

// BDA accesses use direct primitive pointer casts (`float4*`, `float3*`,
// `uint*`). The wrapper-struct-with-runtime-array pattern collapses to
// `Ptr<PhysicalStorageBuffer, uint>` in slangc 2026+, which silently
// returns zero. Primitive pointer types translate correctly. Same convention
// as Particle.vert.slang / BindlessTriangle.slang.

struct TranslucentVSOut
{
    float3 viewPos                 : VIEWPOS;   // view-space position (for fog)
    float2 uv                      : TEXCRD;
    float4 color                   : COLOR;
    nointerpolation uint objectId  : OBJECTID;
    float4 posH                    : SV_POSITION;
};

[shader("vertex")]
TranslucentVSOut vsMain(uint vertexId : SV_VertexID,
                        uint drawInst : SV_InstanceID,
                        uint baseInst : SV_StartInstanceLocation)
{
    // SV_InstanceID alone is the per-draw counter; slangc subtracts
    // BaseInstance from Vulkan's InstanceIndex to give DX semantics.
    // Bindless slot id arrives via firstInstance — reconstruct
    // gl_InstanceIndex by summing. Same fix as SurfelGBuffer.3d.slang.
    uint instanceId = drawInst + baseInst;

    uint64_t indexAddr = gOpaqueIndexHandles[instanceId];
    uint64_t posAddr   = gOpaquePositionHandles[instanceId];
    uint64_t uv0Addr   = gOpaqueUv0Handles[instanceId];
    uint64_t colAddr   = gOpaqueColorHandles[instanceId];

    uint idx = (indexAddr != 0ull)
        ? ((uint*)indexAddr)[vertexId]
        : vertexId;

    float3 a_position = (posAddr != 0ull)
        ? ((float4*)posAddr)[idx].xyz
        : float3(0.0f);
    float2 a_texcoord = (uv0Addr != 0ull)
        ? ((float3*)uv0Addr)[idx].xy
        : float2(0.0f);
    float4 a_color = (colAddr != 0ull)
        ? ((float4*)colAddr)[idx]
        : float4(1.0f);

    UniformObject obj     = gSceneObjects[instanceId];
    float4x4 modelView    = mul(gPerFrame.viewMat, obj.modelMat);
    float4x4 modelViewPrj = mul(gPerFrame.projMat, modelView);

    TranslucentVSOut vsOut;
    vsOut.viewPos  = mul(modelView, float4(a_position, 1.0f)).xyz;
    vsOut.uv       = mul(obj.uvMat, float4(a_texcoord, 0.0f, 1.0f)).xy;
    vsOut.color    = a_color;
    vsOut.objectId = instanceId;
    vsOut.posH     = mul(modelViewPrj, float4(a_position, 1.0f));
    return vsOut;
}
