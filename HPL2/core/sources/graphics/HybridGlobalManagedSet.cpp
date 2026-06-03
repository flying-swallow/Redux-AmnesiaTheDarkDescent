#include "graphics/HybridGlobalManagedSet.h"

#include "graphics/Image.h"
#include "graphics/Material.h"
#include "graphics/MaterialResource.h"
#include "graphics/MaterialType.h"
#include "graphics/RIBootstrap.h"
#include "graphics/RIResourceUploader.h"
#include "graphics/RIVK.h"
#include "graphics/VertexBuffer.h"
#include "graphics/VertexBuffer_RI.h"
#include "math/Math.h"
#include "resources/Resources.h"
#include "resources/TextureManager.h"
#include "system/LowLevelSystem.h"

#include <cstring>
#include <iterator>
#include <span>

namespace hpl {

HybridGlobalManagedSet::HybridGlobalManagedSet()
    : m_objectSlots(kObjectSlotCapacity, /*frameInFlight*/ 0),
      m_diffuseMaterialBindless(kSolidMaterialCapacity, RI_NUMBER_FRAMES_FLIGHT),
      m_translucentMaterialBindless(kTranslucentMaterialCapacity, RI_NUMBER_FRAMES_FLIGHT),
      m_waterMaterialBindless(kWaterMaterialCapacity, RI_NUMBER_FRAMES_FLIGHT),
      m_textureBindless(kTextureSlotCapacity, RI_NUMBER_FRAMES_FLIGHT),
      m_textureCubeBindless(kTextureSlotCapacity, RI_NUMBER_FRAMES_FLIGHT) {}

void HybridGlobalManagedSet::initialize(RIDevice_s *device,
                                        cResources *resources) {
  {
    std::vector<RIBindlessDescriptorSet::Binding> bindings = {};
    // Stage mask shared by every binding the SurfelGI RT pipeline touches —
    // raygen/any-hit/closest-hit/miss all need bindless texture+vertex
    // access for the alpha test, camera-ray reconstruction, and (in Stage E)
    // material shading inside the path-tracer.
    const VkShaderStageFlags kRtSharedStages =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
        VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR |
        VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
        VK_SHADER_STAGE_ANY_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR;
    // textures_2d[] — sampled by the gbuffer, the composite, and the SurfelGI
    // RT pipeline (any-hit alpha test + closest-hit albedo).
    bindings.push_back(RIBindlessDescriptorSet::Binding{
        kBindingTextures2D, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        kTextureSlotCapacity, kRtSharedStages,
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT});
    // textures_cube[] — point-light gobos + env maps; also sampled by the RT
    // pipeline's miss shader (env-light contribution).
    bindings.push_back(RIBindlessDescriptorSet::Binding{
        kBindingTexturesCube, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        kTextureSlotCapacity, kRtSharedStages,
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT});
    // opaque*Handles bindings 3..8 — vertex pulling for the gbuffer VS,
    // triangle fetch in the composite, and barycentric hit fetches in the RT
    // pipeline.
    for (uint32_t i = 0; i < 6; ++i) {
      bindings.push_back(RIBindlessDescriptorSet::Binding{
          kBindingOpaquePositionHandles + i,
          VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kRtSharedStages, 0});
    }
    // materialSampler — paired with textures_2d at every sample site.
    bindings.push_back(RIBindlessDescriptorSet::Binding{
        kBindingMaterialSampler, VK_DESCRIPTOR_TYPE_SAMPLER, 1,
        kRtSharedStages, 0});
    // SurfelGI SSBOs. Reachable from compute, ray-tracing, and fragment stages
    // (the update/raytrace passes, the VBuffer/RT raygen, and the composite).
    const uint32_t kSurfelCellBindings[] = {
        kBindingSurfelCounter,      kBindingSurfelBuffer,
        kBindingSurfelGeometry,     kBindingSurfelValidIndex,
        kBindingSurfelDirtyIndex,  kBindingSurfelFreeIndex,
        kBindingSurfelRecycle,      kBindingSurfelRayResult,
        kBindingCellInfo,           kBindingCellToSurfel,
        kBindingSurfelRefCounter,  kBindingSurfelReservation,
        kBindingSurfelBounds,
        kBindingBindlessSlotGeneration, kBindingSurfelSlotGeneration,
        kBindingLightGridCount,         kBindingLightGridList,
        kBindingObjectDecalIndices,
    };
    const VkShaderStageFlags kSurfelStageFlags =
        VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
        VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
        VK_SHADER_STAGE_ANY_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR;
    for (uint32_t b : kSurfelCellBindings) {
      bindings.push_back(RIBindlessDescriptorSet::Binding{
          b, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kSurfelStageFlags, 0});
    }
    // Scene-object + diffuse-material tables. Read by the gbuffer, the
    // composite, and the RT pipeline's hit shaders.
    const uint32_t kSceneTableBindings[] = {
        kBindingSceneObjects,
        kBindingDiffuseMaterial,
    };
    for (uint32_t b : kSceneTableBindings) {
      bindings.push_back(RIBindlessDescriptorSet::Binding{
          b, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kRtSharedStages, 0});
    }
    // Point/spot-light SSBOs — read by the composite and the path-tracer (NEE).
    bindings.push_back(RIBindlessDescriptorSet::Binding{
        kBindingPointLights, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
        kRtSharedStages, 0});
    bindings.push_back(RIBindlessDescriptorSet::Binding{
        kBindingSpotLights, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
        kRtSharedStages, 0});
    bindings.push_back(RIBindlessDescriptorSet::Binding{
        kBindingFogAreas, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
        kRtSharedStages, 0});
    bindings.push_back(RIBindlessDescriptorSet::Binding{
        kBindingDecals, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
        kRtSharedStages, 0});
    bindings.push_back(RIBindlessDescriptorSet::Binding{
        kBindingWaterMaterial, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
        kRtSharedStages, 0});
    bindings.push_back(RIBindlessDescriptorSet::Binding{
        kBindingTranslucentMaterial, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
        kRtSharedStages, 0});

    // gSurfelDepthSampler stays on set 0 — immutable, never collides with the
    // in-flight frame. The surfel images + TLAS live on set 1 instead, pushed
    // per-dispatch from a frame-rotated pool (see RIProgram::bindDescriptors).
    bindings.push_back(RIBindlessDescriptorSet::Binding{
        kBindingSurfelDepthSampler, VK_DESCRIPTOR_TYPE_SAMPLER, 1,
        VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
            VK_SHADER_STAGE_RAYGEN_BIT_KHR |
            VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
            VK_SHADER_STAGE_ANY_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR,
        0});

    // Default light falloff LUT (core_falloff_linear) — one immutable sampled
    // image on set 0, written once at init; replaces the per-light bindless
    // resolve of the attenuation texture.
    bindings.push_back(RIBindlessDescriptorSet::Binding{
        kBindingAttenuationLut, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
        VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
            VK_SHADER_STAGE_RAYGEN_BIT_KHR |
            VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
            VK_SHADER_STAGE_ANY_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR,
        0});

    VkDescriptorPoolSize poolSizes[3] = {};
    // Sampled-image budget covers textures_2d[] + textures_cube[] + the
    // single global attenuation LUT.
    poolSizes[0] = VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                        kTextureSlotCapacity * 2 + 1};
    // Storage-buffer pool budget: 6 opaque*Handles + 17 surfel/cell bindings
    // (kSurfelCellBindings, incl. kBindingSurfelBounds, the two slot-generation
    // buffers, and the two light-grid buffers) + 2 scene/material + 3 light
    // SSBOs + 1 fog-area + 1 water-material + 3 decal (gDecals + the two decal-
    // grid buffers) = 33. Round up to 40 for slack.
    poolSizes[1] =
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 40};
    // Two samplers: gMaterialSampler + gSurfelDepthSampler.
    poolSizes[2] = VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLER, 2};

    m_bindlessSet.initialize(device, bindings, poolSizes);
  }

  const VkBufferUsageFlags kStorage =
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
      VK_BUFFER_USAGE_TRANSFER_DST_BIT |
      VK_BUFFER_USAGE_TRANSFER_SRC_BIT;  // surfelValid→surfelDirty ping-pong copy
  m_objectBuffer = detail::CreateBindlessSlotBuffer(
      device, kObjectSlotCapacity, sizeof(UniformObject), kStorage,
      /*deviceLocalOnly*/ true);
  m_opaquePositionHandles = detail::CreateBindlessSlotBuffer(
      device, kObjectSlotCapacity, sizeof(VkDeviceAddress), kStorage,
      /*deviceLocalOnly*/ true);
  m_opaqueTangentHandles = detail::CreateBindlessSlotBuffer(
      device, kObjectSlotCapacity, sizeof(VkDeviceAddress), kStorage,
      /*deviceLocalOnly*/ true);
  m_opaqueNormalHandles = detail::CreateBindlessSlotBuffer(
      device, kObjectSlotCapacity, sizeof(VkDeviceAddress), kStorage,
      /*deviceLocalOnly*/ true);
  m_opaqueUv0Handles = detail::CreateBindlessSlotBuffer(
      device, kObjectSlotCapacity, sizeof(VkDeviceAddress), kStorage,
      /*deviceLocalOnly*/ true);
  m_opaqueColorHandles = detail::CreateBindlessSlotBuffer(
      device, kObjectSlotCapacity, sizeof(VkDeviceAddress), kStorage,
      /*deviceLocalOnly*/ true);
  m_opaqueIndexHandles = detail::CreateBindlessSlotBuffer(
      device, kObjectSlotCapacity, sizeof(VkDeviceAddress), kStorage,
      /*deviceLocalOnly*/ true);

  // === SurfelGI SSBOs ===
  // Sizes from the kSurfel* / kCell* / kRayBudget constants:
  //   surfelCounter         : kSurfelCounterSlotCount × uint32
  //   surfel/geometry/etc   : kTotalSurfelLimit × element
  //   surfelRayResult       : kRayBudget × SurfelRayResult (~460 MB)
  //   cellInfo / reservation: kCellCount × element  (15.6M cells × 8B/4B)
  //   cellToSurfel          : kCellToSurfelCapacity × uint32  (75 MB)
  // Lock host sizeof against the Slang ArrayStride for every SSBO struct the
  // host allocates by sizeof(). Mismatches here silently undersize the
  // bindless buffer; robust-access turns the out-of-bounds writes into no-ops
  // and reads return zeros — which manifested as Cell=0/ReqRay=0 because every
  // dirty surfel's data came back as zero.
  //
  // The shaders compile with `-fvk-use-scalar-layout` (cmake/shaders.cmake),
  // so the GPU strides are the natural/packed sizes (float3 = 12B, no 16B
  // struct rounding) and match the host's plain-C layout. These are NOT the
  // std430 values (which would be 128/32/64). Re-measure after any struct or
  // layout-flag change: `slangc … -target spirv-assembly | grep ArrayStride`.
  static_assert(sizeof(Surfel)            == 104, "Surfel host size != Slang scalar ArrayStride (104); re-check struct layout / -fvk-use-scalar-layout");
  static_assert(sizeof(SurfelBounds)      == 28,  "SurfelBounds host size != Slang scalar ArrayStride (28)");
  static_assert(sizeof(SurfelRayResult)   == 48,  "SurfelRayResult host size != Slang scalar ArrayStride (48)");
  static_assert(sizeof(CellInfo)          == 8,   "CellInfo host size != Slang scalar ArrayStride (8)");
  static_assert(sizeof(SurfelRecycleInfo) == 6,   "SurfelRecycleInfo host size != Slang scalar ArrayStride (6)");

  m_surfelCounterBuffer = detail::CreateBindlessSlotBuffer(
      device, kSurfelCounterSlotCount, sizeof(uint32_t), kStorage,
      /*deviceLocalOnly*/ true);
  m_surfelBuffer = detail::CreateBindlessSlotBuffer(
      device, kTotalSurfelLimit, sizeof(Surfel), kStorage);
  m_surfelGeometryBuffer = detail::CreateBindlessSlotBuffer(
      device, kTotalSurfelLimit, sizeof(uint32_t) * 4u, kStorage);
  m_surfelValidBuffer = detail::CreateBindlessSlotBuffer(
      device, kTotalSurfelLimit, sizeof(uint32_t), kStorage);
  m_surfelDirtyIndexBuffer = detail::CreateBindlessSlotBuffer(
      device, kTotalSurfelLimit, sizeof(uint32_t), kStorage);
  m_surfelFreeBuffer = detail::CreateBindlessSlotBuffer(
      device, kTotalSurfelLimit, sizeof(uint32_t), kStorage);
  m_surfelRecycleBuffer = detail::CreateBindlessSlotBuffer(
      device, kTotalSurfelLimit, sizeof(SurfelRecycleInfo), kStorage);
  m_surfelRayResultBuffer = detail::CreateBindlessSlotBuffer(
      device, kRayBudget, sizeof(SurfelRayResult), kStorage);
  m_surfelRefCounterBuffer = detail::CreateBindlessSlotBuffer(
      device, kTotalSurfelLimit, sizeof(uint32_t), kStorage);
  m_surfelReservationBuffer = detail::CreateBindlessSlotBuffer(
      device, kCellCount, sizeof(uint32_t), kStorage);
  m_cellInfoBuffer = detail::CreateBindlessSlotBuffer(
      device, kCellCount, sizeof(CellInfo), kStorage);
  m_cellToSurfelBuffer = detail::CreateBindlessSlotBuffer(
      device, kCellToSurfelCapacity, sizeof(uint32_t), kStorage);
  // Compact cull record per surfel; written by collectCellInfo before the
  // generation pass reads it, so no defensive zeroing is needed (every index
  // pulled from a cell list has already passed through collect this frame).
  m_surfelBoundsBuffer = detail::CreateBindlessSlotBuffer(
      device, kTotalSurfelLimit, sizeof(SurfelBounds), kStorage);
  // Coarse world-space light grid. GPU-only: zeroed each frame via
  // vkCmdFillBuffer and (re)filled by LightGridBuildPass before the ray trace,
  // so no host seeding / mapping is needed (deviceLocalOnly).
  m_lightGridCountBuffer = detail::CreateBindlessSlotBuffer(
      device, kLightGridCellCount, sizeof(uint32_t), kStorage,
      /*deviceLocalOnly*/ true);
  m_lightGridListBuffer = detail::CreateBindlessSlotBuffer(
      device, kLightGridCellCount * kLightsPerCellMax, sizeof(uint32_t),
      kStorage, /*deviceLocalOnly*/ true);
  // (Per-object decal index pool m_objectDecalIndexBuffer is created with the
  // other host-uploaded SSBOs below, alongside m_decalBuffer.)

  // Seed the surfel free-list. gSurfelCounter[Free] is a stack pointer and
  // gSurfelFreeIndexBuffer holds the available slot indices; at boot every slot
  // is free, so Free = kTotalSurfelLimit and the index buffer is iota
  // [0..kTotalSurfelLimit). Without this the free list starts empty (Free=0):
  // SurfelGenerationPass's InterlockedAdd(Free,-1) underflows, Valid/Dirty stay
  // 0 forever => no rays => no indirect light. It can't bootstrap from the
  // recycle path either (collectCellInfo only frees *dirty* surfels, of which
  // there are none). m_surfelFreeBuffer is host-mapped (iota seeded below);
  // m_surfelCounterBuffer is device-local, so its Free seed stages through
  // m_surfelCounterMirror on the first flushMirrors().
  {
    auto *freeIdx =
        static_cast<uint32_t *>(m_surfelFreeBuffer.mappedAddress);
    for (uint32_t i = 0; i < kTotalSurfelLimit; ++i)
      freeIdx[i] = i;
  }

  // Slot-reuse generation buffers (see m_bindlessSlotGenerationBuffer). Both
  // start at 0; the host bumps a slot's generation to a unique nonzero value
  // the first time it's assigned (in Draw), so a real surfel always
  // captures a matching value and 0 can never alias a live slot.
  m_bindlessSlotGenerationBuffer = detail::CreateBindlessSlotBuffer(
      device, kObjectSlotCapacity, sizeof(uint32_t), kStorage,
      /*deviceLocalOnly*/ true);
  m_surfelSlotGenerationBuffer = detail::CreateBindlessSlotBuffer(
      device, kTotalSurfelLimit, sizeof(uint32_t), kStorage);
  // m_bindlessSlotGenerationBuffer is now device-local (no mappedAddress); the
  // CPU shadow mirror is zero-initialized by init() below and markAllDirty()
  // forces the first frame's flushMirrors() to seed the device buffer
  // to zero before collectCellInfo reads it. m_surfelSlotGenerationBuffer stays
  // host-mapped (GPU-written each frame; only this boot zero touches it).
  std::memset(m_surfelSlotGenerationBuffer.mappedAddress, 0,
              (size_t)kTotalSurfelLimit * sizeof(uint32_t));

  // Shadow mirrors for the seven device-local bindless slot buffers. All host
  // writes target these (never GPU-mapped memory); flushMirrors() stages the
  // dirty range once per frame. markAllDirty() seeds the device buffers from
  // the zeroed shadow on the first frame, giving a clean device == mirror
  // invariant for every slot.
  m_opaquePositionMirror.init(kObjectSlotCapacity, sizeof(VkDeviceAddress));
  m_opaqueTangentMirror.init(kObjectSlotCapacity, sizeof(VkDeviceAddress));
  m_opaqueNormalMirror.init(kObjectSlotCapacity, sizeof(VkDeviceAddress));
  m_opaqueUv0Mirror.init(kObjectSlotCapacity, sizeof(VkDeviceAddress));
  m_opaqueColorMirror.init(kObjectSlotCapacity, sizeof(VkDeviceAddress));
  m_opaqueIndexMirror.init(kObjectSlotCapacity, sizeof(VkDeviceAddress));
  m_bindlessSlotGenerationMirror.init(kObjectSlotCapacity, sizeof(uint32_t));
  m_opaquePositionMirror.markAllDirty();
  m_opaqueTangentMirror.markAllDirty();
  m_opaqueNormalMirror.markAllDirty();
  m_opaqueUv0Mirror.markAllDirty();
  m_opaqueColorMirror.markAllDirty();
  m_opaqueIndexMirror.markAllDirty();
  m_bindlessSlotGenerationMirror.markAllDirty();

  // Boot-seed the device-local surfel counter: init() zero-fills, then set
  // Free = kTotalSurfelLimit (the free-list stack pointer). markAllDirty()
  // stages the seed on the first frame's flushMirrors(), which lands ahead of
  // the surfel passes via RI.uploader's fenced pre-pass. The host never touches
  // this mirror again — the GPU owns the counter thereafter.
  m_surfelCounterMirror.init(kSurfelCounterSlotCount, sizeof(uint32_t));
  m_surfelCounterMirror.write<uint32_t>(kSurfelCounterFree, kTotalSurfelLimit);
  m_surfelCounterMirror.markAllDirty();

  // Light SSBOs (point/spot/box) + fog/decal/object-decal-index. Unlike the
  // bindless slot buffers above these are device-local — the per-frame fill in
  // Draw() stages through RI.uploader rather than memcpy'ing into mapped memory
  // the GPU may still be reading from a prior frame.
  {
    uint32_t queueFamilies[RI_QUEUE_LEN] = {0};
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    VK_ConfigureBufferQueueFamilies(&bci, device->queues, RI_QUEUE_LEN,
                                    queueFamilies, RI_QUEUE_LEN);
    bci.usage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    VmaAllocationCreateInfo aci = {};
    aci.usage = VMA_MEMORY_USAGE_AUTO;

    bci.size = (VkDeviceSize)kPointSlotLightCapacity * sizeof(PointLight);
    VK_WrapResult(vmaCreateBuffer(
        device->vk.vmaAllocator, &bci, &aci, &m_pointLightBuffer.vk.buffer,
        &m_pointLightBuffer.vk.allocation, nullptr));

    bci.size = (VkDeviceSize)kSpotSlotLightCapacity * sizeof(SpotLight);
    VK_WrapResult(vmaCreateBuffer(device->vk.vmaAllocator, &bci, &aci,
                                  &m_spotLightBuffer.vk.buffer,
                                  &m_spotLightBuffer.vk.allocation, nullptr));

    bci.size = (VkDeviceSize)kFogAreaCapacity * sizeof(FogAreaParams);
    VK_WrapResult(vmaCreateBuffer(device->vk.vmaAllocator, &bci, &aci,
                                  &m_fogAreaBuffer.vk.buffer,
                                  &m_fogAreaBuffer.vk.allocation, nullptr));

    bci.size = (VkDeviceSize)kMaxDecals * sizeof(GpuDecal);
    VK_WrapResult(vmaCreateBuffer(device->vk.vmaAllocator, &bci, &aci,
                                  &m_decalBuffer.vk.buffer,
                                  &m_decalBuffer.vk.allocation, nullptr));

    bci.size = (VkDeviceSize)kMaxObjectDecalIndices * sizeof(uint32_t);
    VK_WrapResult(vmaCreateBuffer(device->vk.vmaAllocator, &bci, &aci,
                                  &m_objectDecalIndexBuffer.vk.buffer,
                                  &m_objectDecalIndexBuffer.vk.allocation, nullptr));
  }

  m_diffuseMaterialBindless.reset(kSolidMaterialCapacity);
  m_translucentMaterialBindless.reset(kTranslucentMaterialCapacity);
  m_waterMaterialBindless.reset(kWaterMaterialCapacity);
  m_diffuseMaterialBuffer = detail::CreateBindlessSlotBuffer(
      device, kSolidMaterialCapacity, sizeof(DiffuseMaterial), kStorage,
      /*deviceLocalOnly*/ true);
  // Compact typed SSBOs for the translucent and water partitions of the
  // continuous material-id space. submitMaterial allocates a local slot from the
  // matching pool and stores (rangeBase + localSlot) in UniformObject.materialID;
  // the shaders de-bias with (materialID - rangeBase). See Constants.h ranges.
  m_translucentMaterialBuffer = detail::CreateBindlessSlotBuffer(
      device, kTranslucentMaterialCapacity, sizeof(TranslucentMaterial), kStorage,
      /*deviceLocalOnly*/ true);
  m_waterMaterialBuffer = detail::CreateBindlessSlotBuffer(
      device, kWaterMaterialCapacity, sizeof(WaterMaterial), kStorage,
      /*deviceLocalOnly*/ true);

  // Default linear/wrap sampler for all bindless texture fetches. The
  // engine's filter cache (RIBootstrap::resolve_filter_descriptor) hands
  // back a finalized RIDescriptor_s with a non-zero cookie, which is
  // exactly what bindDescriptors needs.
  m_materialSampler = RI.resolve_filter_descriptor(
      eTextureWrap_Repeat, eTextureWrap_Repeat, eTextureWrap_Repeat,
      eTextureFilter_Trilinear);

  {
    const VkDeviceSize kOpaqueHandleRange =
        kObjectSlotCapacity * sizeof(VkDeviceAddress);
    const struct {
      uint32_t binding;
      RIBuffer_s *buffer;
      VkDeviceSize range;
    } ssbos[] = {
        {kBindingOpaquePositionHandles, &m_opaquePositionHandles,
         kOpaqueHandleRange},
        {kBindingOpaqueTangentHandles, &m_opaqueTangentHandles,
         kOpaqueHandleRange},
        {kBindingOpaqueNormalHandles, &m_opaqueNormalHandles,
         kOpaqueHandleRange},
        {kBindingOpaqueUv0Handles, &m_opaqueUv0Handles, kOpaqueHandleRange},
        {kBindingOpaqueColorHandles, &m_opaqueColorHandles,
         kOpaqueHandleRange},
        {kBindingOpaqueIndexHandles, &m_opaqueIndexHandles,
         kOpaqueHandleRange},
        {kBindingSurfelCounter, &m_surfelCounterBuffer,
         kSurfelCounterSlotCount * sizeof(uint32_t)},
        {kBindingSurfelBuffer, &m_surfelBuffer,
         kTotalSurfelLimit * sizeof(Surfel)},
        {kBindingSurfelGeometry, &m_surfelGeometryBuffer,
         kTotalSurfelLimit * sizeof(uint32_t) * 4u},
        {kBindingSurfelValidIndex, &m_surfelValidBuffer,
         kTotalSurfelLimit * sizeof(uint32_t)},
        {kBindingSurfelDirtyIndex, &m_surfelDirtyIndexBuffer,
         kTotalSurfelLimit * sizeof(uint32_t)},
        {kBindingSurfelFreeIndex, &m_surfelFreeBuffer,
         kTotalSurfelLimit * sizeof(uint32_t)},
        {kBindingSurfelRecycle, &m_surfelRecycleBuffer,
         kTotalSurfelLimit * sizeof(SurfelRecycleInfo)},
        {kBindingSurfelRayResult, &m_surfelRayResultBuffer,
         kRayBudget * sizeof(SurfelRayResult)},
        {kBindingCellInfo, &m_cellInfoBuffer,
         kCellCount * sizeof(CellInfo)},
        {kBindingCellToSurfel, &m_cellToSurfelBuffer,
         kCellToSurfelCapacity * sizeof(uint32_t)},
        {kBindingSurfelRefCounter, &m_surfelRefCounterBuffer,
         kTotalSurfelLimit * sizeof(uint32_t)},
        {kBindingSurfelReservation, &m_surfelReservationBuffer,
         kCellCount * sizeof(uint32_t)},
        {kBindingSurfelBounds, &m_surfelBoundsBuffer,
         kTotalSurfelLimit * sizeof(SurfelBounds)},
        {kBindingBindlessSlotGeneration, &m_bindlessSlotGenerationBuffer,
         kObjectSlotCapacity * sizeof(uint32_t)},
        {kBindingSurfelSlotGeneration, &m_surfelSlotGenerationBuffer,
         kTotalSurfelLimit * sizeof(uint32_t)},
        {kBindingObjectDecalIndices, &m_objectDecalIndexBuffer,
         (size_t)kMaxObjectDecalIndices * sizeof(uint32_t)},
        {kBindingLightGridCount, &m_lightGridCountBuffer,
         kLightGridCellCount * sizeof(uint32_t)},
        {kBindingLightGridList, &m_lightGridListBuffer,
         (size_t)kLightGridCellCount * kLightsPerCellMax * sizeof(uint32_t)},
        {kBindingSceneObjects, &m_objectBuffer,
         kObjectSlotCapacity * sizeof(UniformObject)},
        {kBindingDiffuseMaterial, &m_diffuseMaterialBuffer,
         kSolidMaterialCapacity * sizeof(DiffuseMaterial)},
        {kBindingPointLights, &m_pointLightBuffer,
         kPointSlotLightCapacity * sizeof(PointLight)},
        {kBindingSpotLights, &m_spotLightBuffer,
         kSpotSlotLightCapacity * sizeof(SpotLight)},
        {kBindingFogAreas, &m_fogAreaBuffer,
         kFogAreaCapacity * sizeof(FogAreaParams)},
        {kBindingDecals, &m_decalBuffer,
         kMaxDecals * sizeof(GpuDecal)},
        {kBindingWaterMaterial, &m_waterMaterialBuffer,
         kWaterMaterialCapacity * sizeof(WaterMaterial)},
        {kBindingTranslucentMaterial, &m_translucentMaterialBuffer,
         kTranslucentMaterialCapacity * sizeof(TranslucentMaterial)},
    };

    RIBindlessDescriptorSet::WriteBinding writes[std::size(ssbos) + 3] = {};
    size_t count = 0;
    for (uint32_t i = 0; i < std::size(ssbos); ++i) {
      writes[count].binding = ssbos[i].binding;
      writes[count].arrayElement = 0;
      writes[count].descriptor.vk.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      writes[count].descriptor.vk.buffer.buffer = ssbos[i].buffer->vk.buffer;
      writes[count].descriptor.vk.buffer.offset = 0;
      writes[count].descriptor.vk.buffer.range = ssbos[i].range;
      count++;
    }
    writes[count].binding = kBindingMaterialSampler;
    writes[count].arrayElement = 0;
    writes[count].descriptor = *m_materialSampler;
    count++;
    // gSurfelDepthSampler — immutable bilinear clamp sampler, written
    // once at init time. The image views it samples (kBindingSurfelDepthSampled
    // / kBindingSurfelDepth) live on set 1 and are pushed per-dispatch.
    {
      RIDescriptor_s *surfelDepthDesc = RI.resolve_filter_descriptor(
          eTextureWrap_ClampToEdge, eTextureWrap_ClampToEdge,
          eTextureWrap_ClampToEdge, eTextureFilter_Bilinear);
      writes[count].binding = kBindingSurfelDepthSampler;
      writes[count].arrayElement = 0;
      writes[count].descriptor = *surfelDepthDesc;
      count++;
    }
    // gAttenuationLut — the default light falloff LUT (core_falloff_linear),
    // bound once here. It's identical for every light and never changes, so it
    // lives on set 0 instead of being resolved per-light through the bindless
    // texture pool. Create1DImage yields a 2D Nx1 texture with a 2D view, so it
    // samples fine as Texture2D in the shader.
    m_attenuationLut =
        resources->GetTextureManager()->Create1DImage("core_falloff_linear", false);
    if (auto lutTex = m_attenuationLut ? m_attenuationLut->GetTexture() : nullptr) {
      writes[count].binding = kBindingAttenuationLut;
      writes[count].arrayElement = 0;
      writes[count].descriptor.vk.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      writes[count].descriptor.vk.image.sampler = VK_NULL_HANDLE;
      writes[count].descriptor.vk.image.imageView =
          lutTex->binding.vk.image.imageView;
      writes[count].descriptor.vk.image.imageLayout =
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      count++;
    } else {
      Warning("Failed to load core_falloff_linear; light attenuation LUT unbound\n");
    }
    m_bindlessSet.writeDescriptors(device,
                                   std::span(writes).subspan(0, count));
  }
}

uint32_t HybridGlobalManagedSet::resolveTextureSlot(
    RIBootstrap::FrameContext *cntx, Image *img, uint32_t frameIndex) {
  if (!img)
    return kInvalidTextureIndex;
  auto texture = img->GetTexture();
  if (!texture)
    return kInvalidTextureIndex;
  const hash_t texture_cookie =
      hash_u64(HASH_INITIAL_VALUE, img->GetUniqueCookie());
  auto req = m_textureBindless.request(texture_cookie, frameIndex);
  if (req.exhausted)
    return kInvalidTextureIndex;
  cntx->textureLink.push_back(texture);
  // BindlessPool reports `found == true` only when the same cookie still
  // owns the slot. Fresh allocations and LRU recycles both come back with
  // `found == false`, so that's when we (re)stage the descriptor write at
  // textures_2d[req.id] (set 0, binding 0).
  if (!req.found) {
    RIBindlessDescriptorSet::WriteBinding binding = {};
    binding.binding = 0;
    binding.arrayElement = req.id;
    binding.descriptor.vk.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    binding.descriptor.vk.image.sampler = VK_NULL_HANDLE;
    binding.descriptor.vk.image.imageView = texture->binding.vk.image.imageView;
    binding.descriptor.vk.image.imageLayout =
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    m_bindlessSet.writeDescriptors(&RI.device, {&binding, 1});
  }
  return req.id;
}

uint32_t HybridGlobalManagedSet::resolveCubeTextureSlot(
    RIBootstrap::FrameContext *cntx, Image *img, uint32_t frameIndex) {
  if (!img)
    return kInvalidTextureIndex;
  auto texture = img->GetTexture();
  if (!texture)
    return kInvalidTextureIndex;

  const hash_t texture_cookie =
      hash_u64(HASH_INITIAL_VALUE, img->GetUniqueCookie());
  auto req = m_textureCubeBindless.request(texture_cookie, frameIndex);
  if (req.exhausted)
    return kInvalidTextureIndex;
  cntx->textureLink.push_back(texture);
  if (!req.found) {
    RIBindlessDescriptorSet::WriteBinding binding = {};
    binding.binding = kBindingTexturesCube;
    binding.arrayElement = req.id;
    binding.descriptor.vk.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    binding.descriptor.vk.image.sampler = VK_NULL_HANDLE;
    binding.descriptor.vk.image.imageView = texture->binding.vk.image.imageView;
    binding.descriptor.vk.image.imageLayout =
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    m_bindlessSet.writeDescriptors(&RI.device, {&binding, 1});
  }
  return req.id;
}

HybridGlobalManagedSet::MaterialSubmitResult
HybridGlobalManagedSet::submitMaterial(RIBootstrap::FrameContext *cntx,
                                       cMaterial *mat, uint32_t frameIndex) {
  auto slotFor = [&](eMaterialTexture type) -> uint32_t {
    return resolveTextureSlot(cntx, mat->GetImage(type), frameIndex);
  };

  // tex[] order must match the DiffuseMaterial struct in SceneTypes.slang.
  DiffuseMaterial gpu = {};
  gpu.type = MATERIAL_TYPE_DIFFUSE;
  gpu.tex[0] = slotFor(eMaterialTexture_Diffuse);
  gpu.tex[1] = slotFor(eMaterialTexture_NMap);
  gpu.tex[2] = slotFor(eMaterialTexture_Alpha);
  gpu.tex[3] = slotFor(eMaterialTexture_Specular);
  gpu.tex[4] = slotFor(eMaterialTexture_Height);
  gpu.tex[5] = slotFor(eMaterialTexture_Illumination);
  gpu.tex[6] = slotFor(eMaterialTexture_DissolveAlpha);
  gpu.tex[7] = slotFor(eMaterialTexture_CubeMapAlpha);
  // Reflection cube map — separate bindless table (textures_cube[]), so resolve
  // via the cube allocator rather than slotFor (2D-only); stored outside tex[].
  gpu.cubeMapTextureIndex = resolveCubeTextureSlot(
      cntx, mat->GetImage(eMaterialTexture_CubeMap), frameIndex);
  // Material config bits — single source of truth in MaterialResource. The
  // composite reads bit 9 (IsHeightMapSingleChannel) to pick .r vs .a when
  // sampling the heightmap; without it the field sat at zero and parallax
  // always read .a (1.0 for single-channel heightmaps), running the full POM
  // loop every fragment and warping vertical walls at grazing angles.
  gpu.materialConfig = material::UniformMaterialBlock::CreateMaterailConfigFlags(*mat);
  // Scalars: only the solid path is mapped today. Other variants leave
  // these zero (the forward-diffuse fragment shader doesn't read them on
  // the solid path either).
  const ShaderMaterialData &desc = mat->Descriptor();
  if (desc.m_id == MaterialID::SolidDiffuse) {
    gpu.heightMapScale = desc.m_solid.m_heightMapScale;
    gpu.heightMapBias = desc.m_solid.m_heightMapBias;
    gpu.frenselBias = desc.m_solid.m_frenselBias;
    gpu.frenselPow = desc.m_solid.m_frenselPow;
  }
  // Translucent + Water have their own typed tables (handled below); the shared
  // DiffuseMaterial payload above still resolves their textures/config/cubemap,
  // which those branches copy out of `gpu`.

  hash_t cookie = hash_u64(HASH_INITIAL_VALUE, mat->GetUniqueCookie());
  cookie = hash_u64(cookie, (uint64_t)mat->Generation());
  cookie = hash_data(cookie, &gpu, sizeof(gpu));

  // Translucent (glass) materials live in their own compact table; the returned
  // id is offset into the translucent range (kTranslucentMaterialBase + slot),
  // stored in UniformObject.materialID and de-biased shader-side.
  if (desc.m_id == MaterialID::Translucent) {
    auto treq = m_translucentMaterialBindless.request(cookie, frameIndex);
    if (treq.exhausted) {
      Warning("Translucent material slot exhausted (>%u translucent materials); reusing slot 0\n",
              (unsigned)kTranslucentMaterialCapacity);
      return {kTranslucentMaterialBase}; // local slot 0 fallback
    }
    if (!treq.found) {
      TranslucentMaterial trans = {};
      trans.type                = MATERIAL_TYPE_TRANSLUCENT;
      trans.materialConfig      = gpu.materialConfig;
      trans.cubeMapTextureIndex = gpu.cubeMapTextureIndex;
      for (int i = 0; i < 8; ++i) trans.tex[i] = gpu.tex[i];
      trans.refractionScale     = desc.m_translucent.m_refractionScale;
      trans.frenselBias         = desc.m_translucent.m_frenselBias;
      trans.frenselPow          = desc.m_translucent.m_frenselPow;
      trans.rimLightMul         = desc.m_translucent.m_rimLightMul;
      trans.rimLightPow         = desc.m_translucent.m_rimLightPow;

      RIResourceBufferTransaction_s tx = {};
      tx.target = m_translucentMaterialBuffer;
      tx.size = sizeof(TranslucentMaterial);
      tx.offset = (size_t)treq.id * sizeof(TranslucentMaterial);
      tx.vk.current_stage = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT |
                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                            VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
      tx.vk.current_access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
      tx.vk.post_stage = tx.vk.current_stage;
      tx.vk.post_access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
      RI_ResourceBeginCopyBuffer(&RI.device, &RI.uploader, &tx);
      std::memcpy(tx.mapped.data, &trans, sizeof(trans));
      RI_ResourceEndCopyBuffer(&RI.device, &RI.uploader, &tx);
    }
    return {kTranslucentMaterialBase + treq.id};
  }

  // Water materials live in their own compact table — no diffuse entry. The
  // returned id is offset into the water range (kWaterMaterialBase + slot),
  // stored in UniformObject.materialID and de-biased shader-side.
  if (desc.m_id == MaterialID::Water) {
    auto wreq = m_waterMaterialBindless.request(cookie, frameIndex);
    if (wreq.exhausted) {
      Warning("Water material slot exhausted (>%u water materials); reusing slot 0\n",
              (unsigned)kWaterMaterialCapacity);
      return {kWaterMaterialBase}; // local slot 0 fallback
    }
    if (!wreq.found) {
      WaterMaterial water = {};
      water.type            = MATERIAL_TYPE_WATER;
      water.materialConfig  = gpu.materialConfig;
      for (int i = 0; i < 8; ++i) water.tex[i] = gpu.tex[i];
      water.refractionScale     = desc.m_water.m_refractionScale;
      water.frenselBias         = desc.m_water.m_frenselBias;
      water.frenselPow          = desc.m_water.m_frenselPow;
      water.reflectionFadeStart = desc.m_water.m_reflectionFadeStart;
      water.reflectionFadeEnd   = desc.m_water.m_reflectionFadeEnd;
      water.waveSpeed           = desc.m_water.m_waveSpeed;
      water.waveAmplitude       = desc.m_water.m_waveAmplitude;
      water.waveFreq            = desc.m_water.m_waveFreq;

      RIResourceBufferTransaction_s trans = {};
      trans.target = m_waterMaterialBuffer;
      trans.size = sizeof(WaterMaterial);
      trans.offset = (size_t)wreq.id * sizeof(WaterMaterial);
      trans.vk.current_stage = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT |
                               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                               VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
      trans.vk.current_access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
      trans.vk.post_stage = trans.vk.current_stage;
      trans.vk.post_access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
      RI_ResourceBeginCopyBuffer(&RI.device, &RI.uploader, &trans);
      std::memcpy(trans.mapped.data, &water, sizeof(water));
      RI_ResourceEndCopyBuffer(&RI.device, &RI.uploader, &trans);
    }
    return {kWaterMaterialBase + wreq.id};
  }

  auto req = m_diffuseMaterialBindless.request(cookie, frameIndex);
  if (req.exhausted)
    return {UINT32_MAX};
  if (req.found)
    return {req.id};
  {
    RIResourceBufferTransaction_s trans = {};
    trans.target = m_diffuseMaterialBuffer;
    trans.size = sizeof(DiffuseMaterial);
    trans.offset = (size_t)req.id * sizeof(DiffuseMaterial);
    trans.vk.current_stage = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT |
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                             VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
    trans.vk.current_access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    trans.vk.post_stage = trans.vk.current_stage;
    trans.vk.post_access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    RI_ResourceBeginCopyBuffer(&RI.device, &RI.uploader, &trans);
    std::memcpy(trans.mapped.data, &gpu, sizeof(gpu));
    RI_ResourceEndCopyBuffer(&RI.device, &RI.uploader, &trans);
  }

  return {req.id};
}

uint32_t HybridGlobalManagedSet::submitObject(uint64_t objectCookie,
                                              uint32_t frameIndex,
                                              VertexBuffer_RI *vb,
                                              const ObjectSubmitDesc &desc,
                                              uint32_t flags) {
  // Stable slot per object: keyed on the renderable's unique cookie only, so a
  // moving object keeps its slot (its surfels follow via object-space anchoring +
  // the per-frame modelMat upload below). frameInFlight = 0, so a slot is only
  // unavailable if every slot is already taken this frame.
  auto req = m_objectSlots.request(objectCookie, frameIndex);
  if (req.exhausted)
    return UINT32_MAX;
  const uint32_t slot = req.id;

  // Generation bump triggers (surfel anchor invalidation): a fresh occupant
  // (!found) or a topology change. The only hard fault is primitiveIndex >=
  // triangleCount, so a change in the VB's index count is the one geometry edit
  // that must invalidate anchored surfels; a same-count realloc just refreshes
  // the BDA (writeObjectStreamHandles, every frame) and keeps surfels valid.
  const uint32_t indexCount = vb ? (uint32_t)vb->GetIndexNum() : 0u;
  if (!req.found && vb) {
    // Bind the VB-destroy hook on a fresh slot: when the geometry is freed, bump
    // this slot's generation so surfels still anchored here go stale before they
    // dereference the freed vertex/index BDA. Writes the CPU shadow only (may
    // fire off-frame during teardown); flushMirrors() stages it next frame.
    req.state->onDestroy = EventHandler<>([this, slot]() {
      m_bindlessSlotGenerationMirror.write<uint32_t>(
          slot, ++m_nextSlotGeneration);
    });
    req.state->onDestroy.Connect(vb->OnDestroyed());
  }
  const bool bumpGeneration = !req.found || req.state->indexCount != indexCount;
  req.state->indexCount = indexCount;
  if (bumpGeneration)
    m_bindlessSlotGenerationMirror.write<uint32_t>(slot,
                                                   ++m_nextSlotGeneration);

  // kSubmitData: build the GPU payload from the descriptor (transpose the model
  // matrix to the GPU's float4x4, derive invModelMat, transpose the uv matrix,
  // copy scalars) and stage it into m_objectBuffer[slot].
  if (flags & kSubmitData) {
    UniformObject payload{};
    payload.dissolveAmount = desc.dissolveAmount;
    payload.materialID = desc.materialId;
    payload.lightLevel = desc.lightLevel;
    payload.illuminationAmount = desc.illuminationAmount;
    payload.decalList = desc.decalList;
    const ml::float4x4 modelF4 = cMath::ToFloatTranspose4x4(
        desc.modelMatrix ? *desc.modelMatrix : cMatrixf::Identity);
    std::memcpy(payload.modelMat, modelF4.a, sizeof(payload.modelMat));
    ml::float4x4 invF4 = modelF4;
    invF4.Invert();
    std::memcpy(payload.invModelMat, invF4.a, sizeof(payload.invModelMat));
    const ml::float4x4 uvF4 = cMath::ToFloatTranspose4x4(desc.uvMatrix);
    std::memcpy(payload.uvMat, uvF4.a, sizeof(payload.uvMat));

    RIResourceBufferTransaction_s trans = {};
    trans.target = m_objectBuffer;
    trans.size = sizeof(UniformObject);
    trans.offset = (size_t)slot * sizeof(UniformObject);
    trans.vk.current_stage = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT |
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                             VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
    trans.vk.current_access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    trans.vk.post_stage = trans.vk.current_stage;
    trans.vk.post_access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    RI_ResourceBeginCopyBuffer(&RI.device, &RI.uploader, &trans);
    std::memcpy(trans.mapped.data, &payload, sizeof(payload));
    RI_ResourceEndCopyBuffer(&RI.device, &RI.uploader, &trans);
  }

  // kSubmitVertex / kSubmitIndex: fan the VB's per-stream device addresses into
  // the parallel opaque*Handles mirrors at this slot, for bindless vertex pulling
  // (gbuffer VS / surfel-RT chit / particle VS). Absent streams resolve to 0.
  // Rewritten every frame: a SubmitToGPU realloc hands back a new device address,
  // so a once-only write would leave a slot pointing at a freed buffer.
  if (vb && (flags & (kSubmitVertex | kSubmitIndex))) {
    auto bdaOf = [&](eVertexBufferElement type) -> VkDeviceAddress {
      const auto *element = vb->GetElement(type);
      return (element && element->buffer)
                 ? element->buffer->GetDeviceHandle(&RI.device)
                 : 0;
    };
    if (flags & kSubmitVertex) {
      m_opaquePositionMirror.write<VkDeviceAddress>(
          slot, bdaOf(eVertexBufferElement_Position));
      m_opaqueNormalMirror.write<VkDeviceAddress>(
          slot, bdaOf(eVertexBufferElement_Normal));
      m_opaqueTangentMirror.write<VkDeviceAddress>(
          slot, bdaOf(eVertexBufferElement_Texture1Tangent));
      m_opaqueColorMirror.write<VkDeviceAddress>(
          slot, bdaOf(eVertexBufferElement_Color0));
      m_opaqueUv0Mirror.write<VkDeviceAddress>(
          slot, bdaOf(eVertexBufferElement_Texture0));
    }
    if (flags & kSubmitIndex) {
      const VkDeviceAddress idxAddr =
          vb->GetIndexRIBuffer()
              ? vb->GetIndexRIBuffer()->GetDeviceHandle(&RI.device)
              : 0;
      m_opaqueIndexMirror.write<VkDeviceAddress>(slot, idxAddr);
    }
  }
  return slot;
}

void HybridGlobalManagedSet::flushMirrors(RIDevice_s *device) {
  struct Item {
    RIBuffer_s *buf;
    BindlessShadowMirror *mir;
  };
  const Item items[] = {
      {&m_opaquePositionHandles, &m_opaquePositionMirror},
      {&m_opaqueTangentHandles, &m_opaqueTangentMirror},
      {&m_opaqueNormalHandles, &m_opaqueNormalMirror},
      {&m_opaqueUv0Handles, &m_opaqueUv0Mirror},
      {&m_opaqueColorHandles, &m_opaqueColorMirror},
      {&m_opaqueIndexHandles, &m_opaqueIndexMirror},
      {&m_bindlessSlotGenerationBuffer, &m_bindlessSlotGenerationMirror},
      // Boot-seed only: dirty on frame 0 (Free = kTotalSurfelLimit), a no-op
      // every frame after — the GPU owns the counter once the passes run.
      {&m_surfelCounterBuffer, &m_surfelCounterMirror},
  };
  for (const auto &it : items) {
    if (!it.mir->hasDirty())
      continue;
    const size_t off = it.mir->dirtyMinByte;
    const size_t sz = it.mir->dirtyMaxByte - off;
    RIResourceBufferTransaction_s trans = {};
    trans.target = *it.buf;
    trans.size = sz;
    trans.offset = off;
    // Read as storage buffers by the gbuffer VS (vertex pull), the surfel
    // VBuffer / ray-trace chit+ahit, and collectCellInfo (compute). The WAR
    // pre-barrier (current_stage/access) waits on the prior frame's reads before
    // the staged copy overwrites the slot; this is the m_objectBuffer path.
    trans.vk.current_stage = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT |
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                             VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
    trans.vk.current_access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    trans.vk.post_stage = trans.vk.current_stage;
    trans.vk.post_access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    RI_ResourceBeginCopyBuffer(device, &RI.uploader, &trans);
    std::memcpy(trans.mapped.data, it.mir->shadow.data() + off, sz);
    RI_ResourceEndCopyBuffer(device, &RI.uploader, &trans);
    it.mir->clearDirty();
  }
}

void HybridGlobalManagedSet::destroy(RIDevice_s *device) {
  if (m_pointLightBuffer.vk.buffer) {
    vmaDestroyBuffer(device->vk.vmaAllocator, m_pointLightBuffer.vk.buffer,
                     m_pointLightBuffer.vk.allocation);
    m_pointLightBuffer = {};
  }
  if (m_lightGridCountBuffer.vk.buffer) {
    vmaDestroyBuffer(device->vk.vmaAllocator, m_lightGridCountBuffer.vk.buffer,
                     m_lightGridCountBuffer.vk.allocation);
    m_lightGridCountBuffer = {};
  }
  if (m_lightGridListBuffer.vk.buffer) {
    vmaDestroyBuffer(device->vk.vmaAllocator, m_lightGridListBuffer.vk.buffer,
                     m_lightGridListBuffer.vk.allocation);
    m_lightGridListBuffer = {};
  }
  if (m_spotLightBuffer.vk.buffer) {
    vmaDestroyBuffer(device->vk.vmaAllocator, m_spotLightBuffer.vk.buffer,
                     m_spotLightBuffer.vk.allocation);
    m_spotLightBuffer = {};
  }
  if (m_fogAreaBuffer.vk.buffer) {
    vmaDestroyBuffer(device->vk.vmaAllocator, m_fogAreaBuffer.vk.buffer,
                     m_fogAreaBuffer.vk.allocation);
    m_fogAreaBuffer = {};
  }
  if (m_decalBuffer.vk.buffer) {
    vmaDestroyBuffer(device->vk.vmaAllocator, m_decalBuffer.vk.buffer,
                     m_decalBuffer.vk.allocation);
    m_decalBuffer = {};
  }
  if (m_objectDecalIndexBuffer.vk.buffer) {
    vmaDestroyBuffer(device->vk.vmaAllocator, m_objectDecalIndexBuffer.vk.buffer,
                     m_objectDecalIndexBuffer.vk.allocation);
    m_objectDecalIndexBuffer = {};
  }
  m_bindlessSet.destroy(device);
}

} // namespace hpl
