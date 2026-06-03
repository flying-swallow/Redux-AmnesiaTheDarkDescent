#pragma once

// Engine-owned bindless descriptor set (set 0). Slang mirror of the GLSL
// declarations in amnesia/glsl/bindless.resource.glsl — every SSBO / texture
// the host binds at set 0 is declared here, indexed by the BINDING_* macros
// from Constants.h (single source of truth shared with C++ and GLSL).
//
// Usage:
//   import bindless;
//   ...
//   PointLight p = gPointLights[i];
//
// Bindless texture arrays require descriptor indexing
// (SPV_EXT_descriptor_indexing + VK_EXT_descriptor_indexing); slangc emits
// the capability automatically when the array type is bound by `[]`.

#include "Constants.h"
import SceneTypes;

import SurfelGI.SurfelTypes;



// -----------------------------------------------------------------------------
// Bindless texture pools (LRU-allocated by cHybridRenderer's bindless
// allocator). Combine with `gMaterialSampler` at sample sites via
// `Sampler2D(gTextures2D[i], gMaterialSampler)` (or HLSL-style
// `gTextures2D[i].Sample(gMaterialSampler, uv)`).
// -----------------------------------------------------------------------------
[vk::binding(kBindingTextures2D, 0)]
public Texture2D gTextures2D[];

[vk::binding(kBindingTexturesCube, 0)]
public TextureCube gTexturesCube[];

[vk::binding(kBindingMaterialSampler, 0)]
public SamplerState gMaterialSampler;

// -----------------------------------------------------------------------------
// Per-stream BDA handle arrays. Each slot id (bindless object id) indexes
// into these and yields a VkDeviceAddress for that stream of the mesh.
// Shaders cast to a local buffer_reference / buffer-pointer type before
// dereferencing.
// -----------------------------------------------------------------------------
[vk::binding(kBindingOpaquePositionHandles, 0)]
public StructuredBuffer<uint64_t> gOpaquePositionHandles;

[vk::binding(kBindingOpaqueTangentHandles, 0)]
public StructuredBuffer<uint64_t> gOpaqueTangentHandles;

[vk::binding(kBindingOpaqueNormalHandles, 0)]
public StructuredBuffer<uint64_t> gOpaqueNormalHandles;

[vk::binding(kBindingOpaqueUv0Handles, 0)]
public StructuredBuffer<uint64_t> gOpaqueUv0Handles;

[vk::binding(kBindingOpaqueColorHandles, 0)]
public StructuredBuffer<uint64_t> gOpaqueColorHandles;

[vk::binding(kBindingOpaqueIndexHandles, 0)]
public StructuredBuffer<uint64_t> gOpaqueIndexHandles;

// -----------------------------------------------------------------------------
// Surfel-GI buffers — mirror cHybridRenderer's m_surfel*Buffer / m_cell*Buffer.
// Counter is a flat uint[kSurfelCounterSlotCount] indexed via the
// SURFEL_COUNTER_* macros from forward_shared.h. Remaining buffers are sized
// by kTotalSurfelLimit (rayResult by kRayBudget, cellInfo by kCellCount,
// cellToSurfel by kCellToSurfelCapacity).
// -----------------------------------------------------------------------------
[vk::binding(kBindingSurfelCounter, 0)]
public RWStructuredBuffer<uint> gSurfelCounter;

[vk::binding(kBindingSurfelBuffer, 0)]
public RWStructuredBuffer<Surfel> gSurfelBuffer;

// Cached packed TriangleHit (uint4) per surfel — see HitInfo in SceneTypes.slang.
[vk::binding(kBindingSurfelGeometry, 0)]
public RWStructuredBuffer<uint4> gSurfelGeometryBuffer;

[vk::binding(kBindingSurfelValidIndex, 0)]
public RWStructuredBuffer<uint> gSurfelValidIndexBuffer;

[vk::binding(kBindingSurfelDirtyIndex, 0)]
public RWStructuredBuffer<uint> gSurfelDirtyIndexBuffer;

[vk::binding(kBindingSurfelFreeIndex, 0)]
public RWStructuredBuffer<uint> gSurfelFreeIndexBuffer;

[vk::binding(kBindingSurfelRecycle, 0)]
public RWStructuredBuffer<SurfelRecycleInfo> gSurfelRecycleInfoBuffer;

[vk::binding(kBindingSurfelRayResult, 0)]
public RWStructuredBuffer<SurfelRayResult> gSurfelRayResultBuffer;

[vk::binding(kBindingCellInfo, 0)]
public RWStructuredBuffer<CellInfo> gCellInfoBuffer;

[vk::binding(kBindingCellToSurfel, 0)]
public RWStructuredBuffer<uint> gCellToSurfelBuffer;

[vk::binding(kBindingSurfelRefCounter, 0)]
public RWStructuredBuffer<uint> gSurfelRefCounter;

[vk::binding(kBindingSurfelReservation, 0)]
public RWStructuredBuffer<uint> gSurfelReservationBuffer;

// Compact per-surfel cull record (position/radius/pre-normalized normal).
// Written by SurfelUpdatePass::collectCellInfo, read by the
// SurfelGenerationPass per-pixel cull loop in place of the full gSurfelBuffer.
[vk::binding(kBindingSurfelBounds, 0)]
public RWStructuredBuffer<SurfelBounds> gSurfelBoundsBuffer;

// Per-object-slot reuse generation, plus the value each surfel captured at
// spawn. collectCellInfo compares them: a mismatch means the surfel's anchor
// bindless slot was reassigned to different geometry, so its cached TriangleHit
// (instanceID/primitiveIndex) is stale and must NOT be dereferenced through
// getVertexData. The host bumps gBindlessSlotGeneration whenever m_diffuseBindless
// (re)assigns a slot to a different object (see cHybridRenderer::Draw).
[vk::binding(kBindingBindlessSlotGeneration, 0)]
public RWStructuredBuffer<uint> gBindlessSlotGeneration;

[vk::binding(kBindingSurfelSlotGeneration, 0)]
public RWStructuredBuffer<uint> gSurfelSlotGeneration;

// Coarse world-space light grid (LightGridBuildPass writes, SurfelRayTrace NEE
// reads). gLightGridList stores unified light indices (point then spot) for
// each cell, packed kLightsPerCellMax per cell; gLightGridCount is the (capped)
// per-cell length.
[vk::binding(kBindingLightGridCount, 0)]
public RWStructuredBuffer<uint> gLightGridCount;

[vk::binding(kBindingLightGridList, 0)]
public RWStructuredBuffer<uint> gLightGridList;

// -----------------------------------------------------------------------------
// Scene-object + per-material-variant tables. Slot-allocated by the same
// bindless pool as opaque materials; each variant has its own table so
// structs stay at their natural size and shaders never branch on a runtime
// `type` discriminator merely to interpret field offsets (the discriminator
// is still present in `type` for cross-array iteration).
//
// Declared RW for uniform descriptor-set layout across all passes — passes
// that only read get write capability "for free" without changing the
// descriptor type. Host upload still goes through the staging-copy path,
// not via shader writes.
// -----------------------------------------------------------------------------
[vk::binding(kBindingSceneObjects, 0)]
public RWStructuredBuffer<UniformObject> gSceneObjects;

[vk::binding(kBindingDiffuseMaterial, 0)]
public RWStructuredBuffer<DiffuseMaterial> gDiffuseMaterials;

[vk::binding(kBindingTranslucentMaterial, 0)]
public RWStructuredBuffer<TranslucentMaterial> gTranslucentMaterials;

[vk::binding(kBindingWaterMaterial, 0)]
public RWStructuredBuffer<WaterMaterial> gWaterMaterials;

[vk::binding(kBindingDecalMaterial, 0)]
public RWStructuredBuffer<DecalMaterial> gDecalMaterials;

[vk::binding(kBindingDecals, 0)]
public RWStructuredBuffer<GpuDecal> gDecals;

// Flat per-object decal-index pool. UniformObject.decalList packs (offset<<8)|count;
// compositeDecals reads gObjectDecalIndices[offset + k] → a stable gDecals[] index.
[vk::binding(kBindingObjectDecalIndices, 0)]
public RWStructuredBuffer<uint> gObjectDecalIndices;

// -----------------------------------------------------------------------------
// Per-frame light SSBOs. Counts come from SceneConstants on set 1 (the
// per-frame UBO). Iterate up to that count, not the array length.
// -----------------------------------------------------------------------------
[vk::binding(kBindingPointLights, 0)]
public RWStructuredBuffer<PointLight> gPointLights;

[vk::binding(kBindingSpotLights, 0)]
public RWStructuredBuffer<SpotLight> gSpotLights;

[vk::binding(kBindingFogAreas, 0)]
public RWStructuredBuffer<FogAreaParams> gFogAreas;

// -----------------------------------------------------------------------------
// Surfel V-buffer + irradiance / depth atlases. Written by SurfelVBuffer,
// consumed by SurfelGI integrate / evaluation / generation passes.
//
// These four images and the TLAS live on **set 1** (not the persistent
// bindless set 0). They're swapchain-indexed views / per-frame-rebuilt
// acceleration structures, so they'd collide with the previous frame's
// in-flight command buffer if rewritten in place on set 0. Set 1 is
// allocated by RIProgram from a per-frame-rotated descriptor pool
// (RI_NUMBER_FRAMES_FLIGHT), so each `bindDescriptors` write lands on
// a fresh, idle set. The host pushes them per-dispatch via the same
// mechanism `gPerFrame` uses.
//
//   gPackedHitInfo   : full-resolution V-buffer; uint4 = HitInfo.packed
//   gIrradianceMap   : 4K atlas, per-surfel 7×7 octahedral tile (R32F)
//   gSurfelDepthMap  : 4K atlas of (mean, sqr-mean) depth per surfel tile,
//                      RW storage view used for atomic / unfiltered writes.
//   gSurfelDepth     : same image as gSurfelDepthMap, sampled view paired
//                      with gSurfelDepthSampler for filtered reads inside
//                      the integrate / evaluation passes.
//
// gSurfelDepthSampler stays on set 0 (immutable — same sampler every frame,
// no collision concern).
// -----------------------------------------------------------------------------
[vk::binding(kBindingPackedHitInfo, 1)]
public RWTexture2D<uint4> gPackedHitInfo;

// (Per-bounce refraction/reflection V-buffers removed: water refraction now
// clobbers gPackedHitInfo like glass, and water reflection moved to the raster
// water pass.)

[vk::binding(kBindingIrradianceMap, 1)]
public RWTexture2D<float> gIrradianceMap;

[vk::binding(kBindingSurfelDepthMap, 1)]
public RWTexture2D<float2> gSurfelDepthMap;

[vk::binding(kBindingSurfelDepthSampled, 1)]
public Texture2D<float2> gSurfelDepth;

[vk::binding(kBindingSurfelDepthSampler, 0)]
public SamplerState gSurfelDepthSampler;

// Default light falloff LUT (core_falloff_linear), bound once on set 0 — it never
// changes, so it lives here as an immutable global rather than being resolved
// per-light through the bindless texture pool. Sample with gSurfelDepthSampler
// (clamp). Keyed on r²=(d/r)² for radial, (1-cosθ)/(1-cosOuter) for the spot cone.
[vk::binding(kBindingAttenuationLut, 0)]
public Texture2D<float4> gAttenuationLut;

// Surfel-GI scatter RT acceleration structure. Pushed on set 1
// per-dispatch (the TLAS is rebuilt every frame).
[vk::binding(kBindingTlas, 1)]
public RaytracingAccelerationStructure gRtAccel;
