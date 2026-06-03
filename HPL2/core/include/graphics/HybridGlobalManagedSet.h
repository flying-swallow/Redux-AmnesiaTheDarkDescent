#ifndef HPL_HYBRID_GLOBAL_MANAGED_SET_H
#define HPL_HYBRID_GLOBAL_MANAGED_SET_H

#include "graphics/BindlessPool.h"
#include "graphics/RIBootstrap.h"
#include "graphics/RIProgram.h"
#include "graphics/RIRenderer.h"
#include "graphics/RITypes.h"
#include "math/MathTypes.h"
#include "system/Event.h"
#include "system/Hasher.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "Constants.h"
#include "SceneTypes.slang"
#include "SurfelGI/SurfelTypes.slang"

namespace hpl {

class Image;
class cMaterial;
class cResources;
class VertexBuffer_RI;

// CPU shadow of a device-local bindless slot buffer (m_opaque*Handles /
// m_bindlessSlotGenerationBuffer). Those buffers have no mapped pointer, so
// per-slot host writes land here and flushMirrors() stages the dirty byte range
// into the device buffer once per frame. write<T>() addresses slot i at
// i*sizeof(T) (matching CreateBindlessSlotBuffer's layout) and widens the dirty
// range; markAllDirty() stages the whole shadow on the first frame to seed the
// device == shadow invariant.
struct BindlessShadowMirror {
  std::vector<uint8_t> shadow;
  size_t dirtyMinByte = 0;
  size_t dirtyMaxByte = 0;

  // slotCount * elemSize bytes, zero-filled; dirty range starts empty.
  void init(size_t slotCount, size_t elemSize) {
    shadow.assign(slotCount * elemSize, uint8_t(0));
    clearDirty();
  }

  // Store `value` at the slot's byte offset and grow the dirty range. An
  // out-of-range slot is dropped (mirrors GPU robust-access) rather than
  // overrunning the shadow.
  template <typename T> void write(uint32_t slot, T value) {
    const size_t off = (size_t)slot * sizeof(T);
    if (off + sizeof(T) > shadow.size())
      return;
    // Unchanged value: the device buffer already holds it (shadow mirrors
    // device), so don't widen the dirty range — keeps flushMirrors() from
    // re-uploading BDAs / slot generations that didn't move this frame.
    if (std::memcmp(shadow.data() + off, &value, sizeof(T)) == 0)
      return;
    std::memcpy(shadow.data() + off, &value, sizeof(T));
    if (off < dirtyMinByte)
      dirtyMinByte = off;
    if (off + sizeof(T) > dirtyMaxByte)
      dirtyMaxByte = off + sizeof(T);
  }

  void markAllDirty() {
    dirtyMinByte = 0;
    dirtyMaxByte = shadow.size();
  }

  // Empty range encoded as min > max so hasDirty() is false and a subsequent
  // write() always lowers min / raises max.
  void clearDirty() {
    dirtyMinByte = shadow.size();
    dirtyMaxByte = 0;
  }

  bool hasDirty() const { return dirtyMaxByte > dirtyMinByte; }
};

namespace detail {

// Allocate a bindless slot buffer: slotCount * elementStride bytes. When
// deviceLocalOnly the allocation has no host-mapped pointer (out.mappedAddress
// is null) and must be seeded through RI.uploader; otherwise it is persistently
// mapped for direct host writes. Shared by HybridGlobalManagedSet (set-0
// buffers) and cHybridRenderer (the indirect-draw buffer), so it lives in this
// header rather than a single .cpp.
static inline struct RIBuffer_s
CreateBindlessSlotBuffer(RIDevice_s *device, uint32_t slotCount,
                         size_t elementStride, VkBufferUsageFlags usage,
                         bool deviceLocalOnly = false) {
  uint32_t queueFamilies[RI_QUEUE_LEN] = {0};
  VkBufferCreateInfo bufferCreateInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  VK_ConfigureBufferQueueFamilies(&bufferCreateInfo, device->queues,
                                  RI_QUEUE_LEN, queueFamilies, RI_QUEUE_LEN);
  bufferCreateInfo.size = (VkDeviceSize)slotCount * elementStride;
  bufferCreateInfo.usage = usage;

  VmaAllocationCreateInfo allocInfo = {};
  if (deviceLocalOnly) {
    // Pure device-local heap. Caller must seed contents via RI.uploader
    // (RI_ResourceBeginCopyBuffer / EndCopyBuffer) — out.mappedAddress is null.
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
  } else {
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                      VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
  }

  VmaAllocationInfo allocationInfo = {};
  struct RIBuffer_s out;
  VK_WrapResult(vmaCreateBuffer(device->vk.vmaAllocator, &bufferCreateInfo,
                                &allocInfo, &out.vk.buffer, &out.vk.allocation,
                                &allocationInfo));
  out.mappedAddress = deviceLocalOnly ? nullptr : allocationInfo.pMappedData;
  return out;
}

} // namespace detail

// Per-object inputs for submitObject(): the part that varies per pass. The
// class does the matrix transpose / invert / uv-transpose + memcpy into the GPU
// UniformObject. Matrices are engine cMatrixf (row-major); modelMatrix == null
// means identity. Material sites set uvMatrix to pMat->GetUvMatrix(); others
// leave it identity. materialId comes from submitMaterial().
struct ObjectSubmitDesc {
  const cMatrixf *modelMatrix = nullptr;
  cMatrixf        uvMatrix    = cMatrixf::Identity;
  uint32_t        materialId  = 0;
  float           dissolveAmount     = 0.0f;
  float           lightLevel         = 1.0f;
  float           illuminationAmount = 0.0f;
  uint32_t        decalList   = 0;
};

// What submitObject() writes for the slot this frame. Slot allocation + the
// reuse-generation bump always run; these gate the per-slot data writes. The
// fixed-function raster passes (decal/water/translucent) pass kSubmitData only —
// they bind raw VkBuffers and never read the opaque*Handles. The bindless-pull
// passes (solids/TLAS/particle) add kSubmitVertex | kSubmitIndex.
enum ObjectSubmitFlags : uint32_t {
  kSubmitData   = 1u << 0, // stage the UniformObject payload into m_objectBuffer[slot]
  kSubmitVertex = 1u << 1, // write pos/normal/tangent/color/uv stream BDAs (absent → 0)
  kSubmitIndex  = 1u << 2, // write the index-buffer BDA (absent → 0)
};

// Owner of the renderer's single global descriptor set — set 0, the bindless
// RIBindlessDescriptorSet that nearly every GPU buffer is bound to. Builds the
// binding layout, creates + seeds every set-0 buffer, writes them into their
// descriptor slots, resolves textures / materials into bindless slots, and
// stages per-frame host writes through CPU shadow mirrors. cHybridRenderer holds
// one of these and pushes data into it; the members are public so the per-frame
// fill / copy / bind sites in Draw() reach them directly.
class HybridGlobalManagedSet {
public:
  HybridGlobalManagedSet();

  // Build the descriptor-set layout + pool, create and seed all set-0 buffers,
  // and run the one-time descriptor write batch.
  void initialize(RIDevice_s *device, cResources *resources);

  // Destroy all owned buffers and the descriptor set.
  void destroy(RIDevice_s *device);

  // Result of submitMaterial: the object's material id in the continuous
  // range-partitioned space, stored in UniformObject.materialID. Solids land in
  // [0, kSolidMaterialCapacity); water lands in
  // [kSolidMaterialCapacity, kSolidMaterialCapacity + kWaterMaterialCapacity).
  // materialId is UINT32_MAX when the material pool is exhausted.
  struct MaterialSubmitResult {
    uint32_t materialId = 0;
  };

  // Resolve + upload `mat`'s GPU material entry. Solid/translucent/decal
  // materials go to m_diffuseMaterialBuffer (DiffuseMaterial); water materials go
  // to their own compact m_waterMaterialBuffer (WaterMaterial) and return an id
  // offset into the water range. Allocates a slot on first sight, resolves and
  // uploads texture indices when the material's generation differs from the
  // cached one. materialId is UINT32_MAX when the material pool is exhausted.
  MaterialSubmitResult submitMaterial(RIBootstrap::FrameContext *cntx,
                                      cMaterial *mat, uint32_t frameIndex);

  // Map an Image* to its bindless slot in textures_2d[]. Used by
  // submitMaterial() and the light upload loops (spot falloff / gobo,
  // point/spot attenuation maps). Returns kInvalidTextureIndex when the
  // image is null or the bindless pool is exhausted.
  uint32_t resolveTextureSlot(RIBootstrap::FrameContext *cntx, Image *img,
                              uint32_t frameIndex);

  // Same as resolveTextureSlot() but writes into the textures_cube[] bindless
  // array at kBindingTexturesCube. The 2D and cube pools cannot share slot
  // ids because they index different descriptor arrays.
  uint32_t resolveCubeTextureSlot(RIBootstrap::FrameContext *cntx, Image *img,
                                  uint32_t frameIndex);

  // Stage every dirty mirror into its matching device-local buffer. Called once
  // at the end of Draw() so all bindless handle / slot-generation writes land
  // ahead of the primary submit's reads regardless of recording order.
  void flushMirrors(RIDevice_s *device);

  // Own the object's stable slot and stage its payload. Finds/allocates the slot
  // for `objectCookie` (stable per object — keyed on the renderable's unique
  // cookie, NOT its transform, so a moving object keeps its slot and its surfels
  // follow via object-space anchoring), then builds a UniformObject from `desc`
  // and stages it into m_objectBuffer[slot] every frame. Bumps the slot's reuse
  // generation when the slot is (re)assigned to a different object or `vb`'s index
  // count changes (the only `primitiveIndex >= triangleCount` fault), and binds a
  // VB-destroy hook on a fresh slot that bumps the generation when the geometry is
  // freed. Returns the slot, or UINT32_MAX when the pool is exhausted this frame.
  uint32_t submitObject(uint64_t objectCookie, uint32_t frameIndex,
                        VertexBuffer_RI *vb, const ObjectSubmitDesc &desc,
                        uint32_t flags = kSubmitData);

  // === Public state (same names as the former cHybridRenderer members) ===

  RIBindlessDescriptorSet m_bindlessSet;

  // Scene-object table (set 0, kBindingSceneObjects): one UniformObject per
  // assigned object slot, filled per-frame in Draw() from the object cache.
  struct RIBuffer_s m_objectBuffer;

  // Per-object-slot state in the object cache (m_objectSlots). The cache hands a
  // stable slot per object (keyed on the renderable's unique cookie); this rides
  // along so submitObject can detect a topology change (index count) and bump the
  // slot's reuse generation when the geometry is destroyed.
  struct ObjectSlotState {
    EventHandler<> onDestroy;     // VB-destroy → bump this slot's generation
    uint32_t       indexCount = 0; // last-seen index count; change → bump generation
    // Last frame's GPU modelMat (row-major float4x4), staged into
    // UniformObject.prevModelMat for motion vectors. hasPrev is false until the
    // first data submit and is treated as reset when a new object takes the slot.
    float          prevModelMat[16] = {};
    // Last UniformObject staged into m_objectBuffer[slot]. submitObject compares
    // the freshly-built payload against this and skips the upload when identical
    // (static objects: matrices + material never change frame-to-frame).
    UniformObject  lastPayload = {};
  };
  // Stable object-slot cache. frameInFlight = 0: a slot is reusable the frame
  // after its last use (the object-data path is synchronized), and is only
  // "exhausted" when every slot is already taken within the current frame.
  LRUCacheState<ObjectSlotState> m_objectSlots;

  // Per-frame light SSBOs. Device-local; refilled each frame through
  // RI.uploader (see Draw()). The m_*Scratch arrays are CPU staging,
  // reserved once at init so the per-frame fill doesn't reallocate.
  struct RIBuffer_s m_pointLightBuffer = {};
  std::array<PointLight, kPointSlotLightCapacity> m_pointLightScratch;
  struct RIBuffer_s m_spotLightBuffer = {};
  std::array<SpotLight, kSpotSlotLightCapacity> m_spotLightScratch;

  // Per-frame fog-area SSBO. One entry per cFogArea visible this frame.
  struct RIBuffer_s m_fogAreaBuffer = {};
  std::array<FogAreaParams, kFogAreaCapacity> m_fogAreaScratch;

  // Clustered OOB decals (cDecal). Packed each frame from eRenderListType_Decal
  // into gDecals[]; the projection pass reads them. Capped at kMaxDecals.
  struct RIBuffer_s m_decalBuffer = {};
  std::array<GpuDecal, kMaxDecals> m_decalScratch;

  struct RIBuffer_s m_opaquePositionHandles;
  struct RIBuffer_s m_opaqueTangentHandles;
  struct RIBuffer_s m_opaqueNormalHandles;
  struct RIBuffer_s m_opaqueUv0Handles;
  struct RIBuffer_s m_opaqueColorHandles;
  struct RIBuffer_s m_opaqueIndexHandles;

  // Per-object-slot reuse generation (sized kObjectSlotCapacity). Bumped to a
  // fresh monotonic value each time the object cache (re)assigns a slot to a
  // different object; surfels capture it at spawn so a reused slot
  // self-invalidates the stale anchor in collectCellInfo. m_nextSlotGeneration
  // is the host-side source of new values so we never read back the
  // write-combined mapped buffer.
  struct RIBuffer_s m_bindlessSlotGenerationBuffer;
  uint32_t m_nextSlotGeneration = 0;

  // CPU shadows of the seven device-local bindless slot buffers above (the six
  // per-stream BDA handle buffers + the slot-generation buffer). All host
  // per-slot writes go through these mirrors; flushMirrors() stages each
  // mirror's dirty byte range into its device buffer once per frame.
  BindlessShadowMirror m_opaquePositionMirror;
  BindlessShadowMirror m_opaqueTangentMirror;
  BindlessShadowMirror m_opaqueNormalMirror;
  BindlessShadowMirror m_opaqueUv0Mirror;
  BindlessShadowMirror m_opaqueColorMirror;
  BindlessShadowMirror m_opaqueIndexMirror;
  BindlessShadowMirror m_bindlessSlotGenerationMirror;

  // Boot-seed-only shadow for the device-local m_surfelCounterBuffer.
  BindlessShadowMirror m_surfelCounterMirror;

  // === SurfelGI resources (set=0 bindings 10..19, 27..28, 31..33) ===
  struct RIBuffer_s m_surfelCounterBuffer;
  struct RIBuffer_s m_surfelBuffer;
  struct RIBuffer_s m_surfelGeometryBuffer;
  struct RIBuffer_s m_surfelValidBuffer;
  struct RIBuffer_s m_surfelDirtyIndexBuffer;
  struct RIBuffer_s m_surfelFreeBuffer;
  struct RIBuffer_s m_surfelRecycleBuffer;
  struct RIBuffer_s m_surfelRayResultBuffer;
  struct RIBuffer_s m_cellInfoBuffer;
  struct RIBuffer_s m_cellToSurfelBuffer;
  struct RIBuffer_s m_surfelRefCounterBuffer;
  struct RIBuffer_s m_surfelReservationBuffer;
  // Compact per-surfel cull record (SurfelBounds) read by the generation
  // pass's hot loop in place of the full m_surfelBuffer gather.
  struct RIBuffer_s m_surfelBoundsBuffer;

  // Coarse world-space light grid (LightGridBuildPass writes, SurfelRayTrace
  // NEE reads): per-cell light count + packed per-cell unified-light-index list.
  struct RIBuffer_s m_lightGridCountBuffer;
  struct RIBuffer_s m_lightGridListBuffer;
  // Flat per-object decal-index pool (cWorld::Compile association), uploaded into
  // gObjectDecalIndices; replaces the old spatial decal grid.
  struct RIBuffer_s m_objectDecalIndexBuffer = {};

  // Per-surfel copy of its anchor slot's generation, captured at spawn and
  // compared against m_bindlessSlotGenerationBuffer in collectCellInfo.
  struct RIBuffer_s m_surfelSlotGenerationBuffer;

  // Bindless material wiring. The material-id space is range-partitioned across
  // three typed pools/buffers: solid+decal (m_diffuseMaterialBindless / DiffuseMaterial),
  // translucent (m_translucentMaterialBindless / TranslucentMaterial), and water
  // (m_waterMaterialBindless / WaterMaterial). See Constants.h for the ranges.
  LRUCache m_diffuseMaterialBindless;
  LRUCache m_translucentMaterialBindless;
  LRUCache m_waterMaterialBindless;
  struct RIBuffer_s m_diffuseMaterialBuffer;
  struct RIBuffer_s m_translucentMaterialBuffer = {};
  struct RIBuffer_s m_waterMaterialBuffer = {};
  struct RIDescriptor_s *m_materialSampler = nullptr;

  // Default light falloff LUT (core_falloff_linear), bound once to set 0 as the
  // immutable gAttenuationLut. Held resident for the renderer's lifetime.
  Image *m_attenuationLut = nullptr;

  LRUCache m_textureBindless;
  // Separate LRU for cube textures. Slot ids index textures_cube[] (set 0,
  // binding 1) and must not be confused with textures_2d[] ids.
  LRUCache m_textureCubeBindless;
};

} // namespace hpl

#endif
