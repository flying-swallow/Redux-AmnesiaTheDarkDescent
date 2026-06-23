#include "graphics/GlobalManagedSets.h"

#include "graphics/Image.h"
#include "graphics/Material.h"
#include "graphics/MaterialType.h"
#include "graphics/RIBootstrap.h"
#include "graphics/RIResourceUploader.h"
#include "graphics/RIVK.h"
#include "graphics/VertexBuffer.h"
#include "graphics/VertexBuffer.h"
#include "math/Math.h"
#include "resources/Resources.h"
#include "resources/TextureManager.h"
#include "system/LowLevelSystem.h"

#include <cstring>
#include <iterator>
#include <span>

namespace hpl {

GlobalManagedSets::GlobalManagedSets()
    : m_objectSlots(kObjectSlotCapacity, /*frameInFlight*/ 0),
      m_materialBindless(kMaterialCapacity, RI_NUMBER_FRAMES_FLIGHT) {}

// Out-of-line so the SharedResourceHandle<Image> members release here, where
// Image is complete.
GlobalManagedSets::~GlobalManagedSets() = default;

void GlobalManagedSets::initialize(RIDevice *device,
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
    // textures_2d_array[] — one Texture2DArray per animated image (frame = layer).
    // cTextureManager writes a slot's descriptor once at load (update-after-bind).
    bindings.push_back(RIBindlessDescriptorSet::Binding{
        kBindingTextures2DArray, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        kTexture2DArrayCapacity, kRtSharedStages,
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT});
    // gAnimTex — per-2D-array-slot animation record (frameCount/frameTime/mode),
    // read by the bindless sample helper. One bound buffer (contents written by
    // cTextureManager via the uploader; descriptor written once below).
    bindings.push_back(RIBindlessDescriptorSet::Binding{
        kBindingAnimTex, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kRtSharedStages, 0});
    // (Former opaque*Handles bindings 3..8 removed — the per-stream BDAs now
    // live in UniformObject; those binding slots are unused/reserved.)
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
    };
    const VkShaderStageFlags kSurfelStageFlags =
        VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
        VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
        VK_SHADER_STAGE_ANY_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR;
    for (uint32_t b : kSurfelCellBindings) {
      bindings.push_back(RIBindlessDescriptorSet::Binding{
          b, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kSurfelStageFlags, 0});
    }
    // Scene-object + flat material table. Read by the gbuffer, the composite,
    // and the RT pipeline's hit shaders.
    const uint32_t kSceneTableBindings[] = {
        kBindingSceneObjects,
        kBindingMaterials,
    };
    for (uint32_t b : kSceneTableBindings) {
      bindings.push_back(RIBindlessDescriptorSet::Binding{
          b, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kRtSharedStages, 0});
    }
    // Point/spot/area lights and fog areas no longer live on set 0. They are
    // cWorld-owned persistent per-world buffers bound on the dedicated per-world
    // set kWorldSet (program-managed + cached) by every pass that reads them —
    // see appendWorldLightFog in HybridRenderer.cpp. set 0 stays pure engine state.

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

    // core_dissolve noise — one immutable sampled image on set 0, written once
    // at init. UV-sampled by the shared SceneMaterials.alphaTest in every
    // alpha-testing context (gbuffer FS, V-buffer / surfel anyhit, shadow and
    // reflection RayQuery loops) — the legacy solid_z dissolve fade.
    bindings.push_back(RIBindlessDescriptorSet::Binding{
        kBindingDissolveMap, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
        VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
            VK_SHADER_STAGE_RAYGEN_BIT_KHR |
            VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
            VK_SHADER_STAGE_ANY_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR,
        0});

    VkDescriptorPoolSize poolSizes[3] = {};
    // Sampled-image budget covers textures_2d[] + textures_cube[] +
    // textures_2d_array[] + the dissolve noise map.
    poolSizes[0] = VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                        kTextureSlotCapacity * 2 + kTexture2DArrayCapacity + 2};
    // Storage-buffer pool budget: 17 surfel/cell bindings (kSurfelCellBindings,
    // incl. kBindingSurfelBounds, the two slot-generation buffers, and the two
    // light-grid buffers) + 2 scene/material + 1 animTex ≈ 20. The per-world
    // light/fog SSBOs are no longer here (they ride kWorldSet). 40 keeps slack.
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
  // (The six per-stream BDA handle buffers are gone — their addresses now ride
  // the UniformObject in m_objectBuffer.)

  // Per-2D-array-slot animation record table (gAnimTex). Contents written by
  // cTextureManager (uploader copies) when an animated image is assigned its
  // slot, so it must be device-local with TRANSFER_DST. Built through
  // CreateBindlessSlotBuffer like every other set-0 buffer — `kStorage` is raw
  // VkBufferUsageFlags, which that helper feeds straight into VkBufferCreateInfo.
  // (RIBuffer::create instead expects RI_BUFFER_USAGE_* flags, so passing kStorage
  // there mistranslated to STORAGE|INDIRECT and dropped TRANSFER_DST.)
  m_animTexBuffer = detail::CreateBindlessSlotBuffer(
      device, kTexture2DArrayCapacity, sizeof(AnimTexRec), kStorage,
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

  // Shadow mirror for the device-local slot-generation buffer. Host writes
  // target this (never GPU-mapped memory); flushMirrors() stages the dirty range
  // once per frame. markAllDirty() seeds the device buffer from the zeroed shadow
  // on the first frame, giving a clean device == mirror invariant for every slot.
  // (Per-stream BDA handles now ride the UniformObject upload — no mirrors.)
  m_bindlessSlotGenerationMirror.init(kObjectSlotCapacity, sizeof(uint32_t));
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
  // Point/spot/area light buffers and the fog buffer are no longer owned here —
  // cWorld owns them as persistent per-world buffers on the per-world set
  // kWorldSet, bound by each consuming pass.

  // One flat material table (Falcor MaterialSystem model): a fixed-size
  // MaterialDataBlob per slot, indexed by a flat materialID. submitMaterial packs
  // the typed material struct into a blob (type tag in the header) and uploads it;
  // the shader reinterprets the blob into the concrete struct. One LRU pool.
  m_materialBindless.reset(kMaterialCapacity);
  m_materialBuffer = detail::CreateBindlessSlotBuffer(
      device, kMaterialCapacity, sizeof(MaterialDataBlob), kStorage,
      /*deviceLocalOnly*/ true);

  // Default linear/wrap sampler for all bindless texture fetches. The
  // engine's filter cache (RIBootstrap::resolve_filter_descriptor) hands
  // back a finalized RIDescriptor with a non-zero cookie, which is
  // exactly what bindDescriptors needs.
  m_materialSampler = RI.resolve_filter_descriptor(
      eTextureWrap_Repeat, eTextureWrap_Repeat, eTextureWrap_Repeat,
      eTextureFilter_Trilinear);

  {
    const struct {
      uint32_t binding;
      RIBuffer *buffer;
      VkDeviceSize range;
    } ssbos[] = {
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
        {kBindingLightGridCount, &m_lightGridCountBuffer,
         kLightGridCellCount * sizeof(uint32_t)},
        {kBindingLightGridList, &m_lightGridListBuffer,
         (size_t)kLightGridCellCount * kLightsPerCellMax * sizeof(uint32_t)},
        {kBindingSceneObjects, &m_objectBuffer,
         kObjectSlotCapacity * sizeof(UniformObject)},
        {kBindingMaterials, &m_materialBuffer,
         kMaterialCapacity * sizeof(MaterialDataBlob)},
        {kBindingAnimTex, &m_animTexBuffer,
         kTexture2DArrayCapacity * sizeof(AnimTexRec)},
        // The per-world light/fog SSBOs are not here — they ride kWorldSet, bound
        // per-pass from cWorld's persistent buffers.
    };

    RIBindlessDescriptorSet::WriteBinding writes[std::size(ssbos) + 4] = {};
    size_t count = 0;
    for (uint32_t i = 0; i < std::size(ssbos); ++i) {
      writes[count].binding = ssbos[i].binding;
      writes[count].arrayElement = 0;
      writes[count].descriptor = RIDescriptor::storageBuffer(
          device, ssbos[i].buffer, 0, ssbos[i].range);
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
      auto surfelDepthDesc = RI.resolve_filter_descriptor(
          eTextureWrap_ClampToEdge, eTextureWrap_ClampToEdge,
          eTextureWrap_ClampToEdge, eTextureFilter_Bilinear);
      writes[count].binding = kBindingSurfelDepthSampler;
      writes[count].arrayElement = 0;
      writes[count].descriptor = *surfelDepthDesc;
      count++;
    }
    // gDissolveMap — the legacy 128×128 dissolve noise (solid_z.frag.fsl),
    // bound once here on set 0. UV-sampled by the shared
    // SceneMaterials.alphaTest for the CoverageAmount fade.
    m_dissolveMap =
        resources->GetTextureManager()->Create2DImage("core_dissolve.tga", false);
    if (auto disTex = m_dissolveMap ? m_dissolveMap->GetTexture() : nullptr) {
      writes[count].binding = kBindingDissolveMap;
      writes[count].arrayElement = 0;
      writes[count].descriptor = disTex->descriptor();
      count++;
    } else {
      Warning("Failed to load core_dissolve.tga; dissolve fade unbound\n");
    }
    m_bindlessSet.writeDescriptors(device,
                                   std::span(writes).subspan(0, count));
  }
}

GlobalManagedSets::MaterialSubmitResult
GlobalManagedSets::submitMaterial(RIBootstrap::FrameContext *cntx,
                                       cMaterial *mat, uint32_t frameIndex) {
  // cTextureManager stamps each Image with a lifetime-stable bindless slot at
  // load (binding 0 = textures_2d[], binding 1 = textures_cube[]); read it AND
  // pin the Image for this frame. The hybrid renderer references material
  // textures purely by slot (no per-draw descriptor binding), so without this pin
  // an entity destroyed mid-frame — e.g. a picked-up item in
  // UpdateToBeDestroyedEntities — would drop the Image's last ref and free its
  // image view (cTexture::~cTexture) while the GPU's bindless set still references
  // it (VUID-vkDestroyImageView-imageView-01026). The pin parks the Image in
  // graphicsDefer until the GPU passes this frame, then it frees safely.
  auto slotFor = [&](eMaterialTexture type) -> uint32_t {
    Image *img = mat->GetImage(type);
    if (!img)
      return kInvalidTextureIndex;
    RI.graphicsDefer.push(PinResource(img));
    return img->GetBindlessSlot();
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
  // Reflection cube map — separate bindless table (textures_cube[]). A cube
  // Image's slot indexes textures_cube[] (cTextureManager assigned it from the
  // cube pool), so slotFor's GetBindlessSlot() read + per-frame pin works here
  // too; stored outside tex[].
  gpu.cubeMapTextureIndex = slotFor(eMaterialTexture_CubeMap);


  auto isSingleChannel = [](const Image *image) {
    if (!image) {
      return false;
    }
    const auto texture = image->GetTexture();
    return texture && RIFormatChannelCount(texture->format) == 1;
  };
  const Image *alphaImage = mat->GetImage(eMaterialTexture_Alpha);
  const Image *heightImage = mat->GetImage(eMaterialTexture_Height);
  gpu.materialConfig =
      (mat->GetImage(eMaterialTexture_Diffuse) ? kMaterialFlagEnableDiffuse : 0) |
      (mat->GetImage(eMaterialTexture_NMap) ? kMaterialFlagEnableNormal : 0) |
      (mat->GetImage(eMaterialTexture_Specular) ? kMaterialFlagEnableSpecular : 0) |
      (alphaImage ? kMaterialFlagEnableAlpha : 0) |
      (isSingleChannel(alphaImage) ? kMaterialFlagIsAlphaSingleChannel : 0) |
      (heightImage ? kMaterialFlagEnableHeight : 0) |
      (isSingleChannel(heightImage) ? kMaterialFlagIsHeightMapSingleChannel : 0) |
      (mat->GetImage(eMaterialTexture_Illumination) ? kMaterialFlagEnableIllumination : 0) |
      (mat->GetImage(eMaterialTexture_CubeMap) ? kMaterialFlagEnableCubeMap : 0) |
      (mat->GetImage(eMaterialTexture_DissolveAlpha) ? kMaterialFlagEnableDissolveAlpha : 0) |
      (mat->GetImage(eMaterialTexture_CubeMapAlpha) ? kMaterialFlagEnableCubeMapAlpha : 0);

  // The typed material tables share the same leading layout
  // (type, materialConfig, tex[8]); only the trailing scalars differ. Copy the
  // already-resolved config + texture slots out of `gpu` so each branch below
  // only has to fill its own scalar tail.
  auto copyShared = [&](auto &dst) {
    dst.materialConfig = gpu.materialConfig;
    std::memcpy(dst.tex, gpu.tex, sizeof(gpu.tex));
  };
  // Dispatch on the per-type data blob: each alternative builds its typed GPU
  // material struct (folding its config flags + scalars into the shared `gpu`
  // payload), packs it into a fixed-size MaterialDataBlob (type tag in the
  // header), then uploads the blob to the single flat material table and returns
  // a flat materialID. The shader reinterprets the blob via its type tag.
  return std::visit(
      [&](const auto &data) -> MaterialSubmitResult {
        using T = std::decay_t<decltype(data)>;

        static_assert(sizeof(MaterialDataBlob) >= sizeof(DiffuseMaterial) &&
                          sizeof(MaterialDataBlob) >= sizeof(TranslucentMaterial) &&
                          sizeof(MaterialDataBlob) >= sizeof(WaterMaterial),
                      "MaterialDataBlob must hold the largest typed material struct");
        MaterialDataBlob blob = {};

        if constexpr (std::is_same_v<T, MaterialTranslucent>) {
          gpu.materialConfig |=
              (data.m_refractionNormals ? kMaterialFlagUseRefractionNormals : 0) |
              (mat->HasRefraction() && data.m_refractionEdgeCheck ? kMaterialFlagUseRefractionEdgeCheck : 0) |
              (mat->HasRefraction() ? kMaterialFlagHasRefraction : 0) |
              (data.m_isAffectedByLightLevel ? kMaterialFlagAffectedByLightLevel : 0);
          TranslucentMaterial trans = {};
          trans.type                = MATERIAL_TYPE_TRANSLUCENT;
          copyShared(trans);
          trans.cubeMapTextureIndex = gpu.cubeMapTextureIndex;
          trans.refractionScale     = data.m_refractionScale;
          trans.frenselBias         = data.m_frenselBias;
          trans.frenselPow          = data.m_frenselPow;
          trans.rimLightMul         = data.m_rimLightMul;
          trans.rimLightPow         = data.m_rimLightPow;
          std::memcpy(blob.data, &trans, sizeof(trans));
        } else if constexpr (std::is_same_v<T, MaterialWater>) {
          // Water always refracts + reflects via the SurfelVBuffer.rt wave-animated
          // bounce. IsWater drives that branch; HasRefraction makes the GIRenderPass
          // swap show the refracted background.
          gpu.materialConfig |= kMaterialFlagIsWater | kMaterialFlagHasRefraction;
          WaterMaterial water = {};
          water.type            = MATERIAL_TYPE_WATER;
          copyShared(water);
          water.refractionScale     = data.m_refractionScale;
          water.frenselBias         = data.m_frenselBias;
          water.frenselPow          = data.m_frenselPow;
          water.reflectionFadeStart = data.m_reflectionFadeStart;
          water.reflectionFadeEnd   = data.m_reflectionFadeEnd;
          water.waveSpeed           = data.m_waveSpeed;
          water.waveAmplitude       = data.m_waveAmplitude;
          water.waveFreq            = data.m_waveFreq;
          std::memcpy(blob.data, &water, sizeof(water));
        } else {
          // SolidDiffuse, Decal and the blank/unknown (monostate) material all
          // use the DiffuseMaterial layout.
          if constexpr (std::is_same_v<T, MaterialDiffuseSolid>) {
            gpu.materialConfig |= (data.m_alphaDissolveFilter ? kMaterialFlagUseDissolveFilter : 0);
            gpu.heightMapScale = data.m_heightMapScale;
            gpu.heightMapBias = data.m_heightMapBias;
            gpu.frenselBias = data.m_frenselBias;
            gpu.frenselPow = data.m_frenselPow;
          }
          std::memcpy(blob.data, &gpu, sizeof(gpu));
        }

        // One flat table, keyed by a cookie over the packed blob (folds in the
        // material generation so edits re-upload). Allocate a slot on first
        // sight; re-upload only when the blob changed.
        hash_t cookie = hash_u64(HASH_INITIAL_VALUE, mat->GetUniqueCookie());
        cookie = hash_u64(cookie, (uint64_t)mat->Generation());
        cookie = hash_data(cookie, &blob, sizeof(blob));

        auto req = m_materialBindless.request(cookie, frameIndex);
        if (req.exhausted)
          return {UINT32_MAX};
        if (req.found)
          return {req.id};

        RIResourceBufferTransaction trans = {};
        trans.target = m_materialBuffer;
        trans.size = sizeof(MaterialDataBlob);
        trans.offset = (size_t)req.id * sizeof(MaterialDataBlob);
        trans.currentState = RI_RESOURCE_STATE_UNORDERED_ACCESS;
        trans.currentStages = RI_STAGE_ALL_SHADER;
        trans.postState = RI_RESOURCE_STATE_UNORDERED_ACCESS;
        trans.postStages = RI_STAGE_ALL_SHADER;
        RI_ResourceBeginCopyBuffer(&RI.device, &RI.uploader, &trans);
        std::memcpy(trans.mapped.data, &blob, sizeof(blob));
        RI_ResourceEndCopyBuffer(&RI.device, &RI.uploader, &trans);
        return {req.id};
      },
      mat->Data());
}

uint32_t GlobalManagedSets::submitObject(uint64_t objectCookie,
                                              uint32_t frameIndex,
                                              cVertexBuffer *vb,
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
    payload.illuminationAmount = desc.illuminationAmount;
    payload.decalList = desc.decalList;
    const ml::float4x4 modelF4 = cMath::ToFloatTranspose4x4(
        desc.modelMatrix ? *desc.modelMatrix : cMatrixf::Identity);
    std::memcpy(payload.modelMat, modelF4.a, sizeof(payload.modelMat));
    ml::float4x4 invF4 = modelF4;
    invF4.Invert();
    std::memcpy(payload.invModelMat, invF4.a, sizeof(payload.invModelMat));
    // prevModelMat for motion vectors. Rotate prev<-cur ONLY ONCE per frame per
    // slot (keyed on frameIndex): submitObject may run multiple times per frame for
    // the same object (cWorld::PrepareFrame's whole-scene submit + the renderer's
    // raster loop). Every call must publish the SAME prev (last frame's matrix);
    // rotating on each call would set prev==cur on the 2nd call and zero the
    // object's velocity → temporal smear on animated/physics objects. New occupant
    // (!found): prev = cur so the first frame reads zero velocity, not a teleport.
    if (!req.found) {
      std::memcpy(req.state->prevModelMat, modelF4.a, sizeof(req.state->prevModelMat));
      std::memcpy(req.state->curModelMat, modelF4.a, sizeof(req.state->curModelMat));
    } else if (req.state->lastSubmitFrame != frameIndex) {
      std::memcpy(req.state->prevModelMat, req.state->curModelMat,
                  sizeof(req.state->prevModelMat));
      std::memcpy(req.state->curModelMat, modelF4.a, sizeof(req.state->curModelMat));
    }
    req.state->lastSubmitFrame = frameIndex;
    std::memcpy(payload.prevModelMat, req.state->prevModelMat,
                sizeof(payload.prevModelMat));
    const ml::float4x4 uvF4 = cMath::ToFloatTranspose4x4(desc.uvMatrix);
    std::memcpy(payload.uvMat, uvF4.a, sizeof(payload.uvMat));

    // Per-stream vertex/index BDAs, folded into the UniformObject (were the six
    // gOpaque*Handles buffers). Priority: explicit override (particles → scratch
    // ring) > derive from vb (vertex-pull passes; absent stream/flag → 0) > carry
    // forward the slot's existing handles (data-only passes that bind raw
    // VkBuffers — don't zero a slot a handle-reading pass populated). Rewritten
    // every frame so a SubmitToGPU realloc can't dangle them; the memcmp-skip
    // below keeps a stable-source object's upload skipped after frame 0.
    if (desc.streamHandles.set) {
      payload.posHandle     = desc.streamHandles.pos;
      payload.normalHandle  = desc.streamHandles.normal;
      payload.tangentHandle = desc.streamHandles.tangent;
      payload.uv0Handle     = desc.streamHandles.uv0;
      payload.colorHandle   = desc.streamHandles.color;
      payload.indexHandle   = desc.streamHandles.index;
    } else if (vb && (flags & (kSubmitVertex | kSubmitIndex))) {
      auto bdaOf = [&](eVertexBufferElement type) -> uint64_t {
        const auto *element = vb->GetElement(type);
        RIBuffer *buf = element ? element->GetBuffer() : nullptr;
        return buf ? buf->GetDeviceHandle(&RI.device) : 0;
      };
      if (flags & kSubmitVertex) {
        payload.posHandle     = bdaOf(eVertexBufferElement_Position);
        payload.normalHandle  = bdaOf(eVertexBufferElement_Normal);
        payload.tangentHandle = bdaOf(eVertexBufferElement_Texture1Tangent);
        payload.colorHandle   = bdaOf(eVertexBufferElement_Color0);
        payload.uv0Handle     = bdaOf(eVertexBufferElement_Texture0);
      }
      if (flags & kSubmitIndex)
        payload.indexHandle = vb->GetIndexRIBuffer()
                                  ? vb->GetIndexRIBuffer()->GetDeviceHandle(&RI.device)
                                  : 0;
    } else if (req.found) {
      payload.posHandle     = req.state->lastPayload.posHandle;
      payload.normalHandle  = req.state->lastPayload.normalHandle;
      payload.tangentHandle = req.state->lastPayload.tangentHandle;
      payload.uv0Handle     = req.state->lastPayload.uv0Handle;
      payload.colorHandle   = req.state->lastPayload.colorHandle;
      payload.indexHandle   = req.state->lastPayload.indexHandle;
    }

    // Permissive upload: a new occupant (!found, so lastPayload still belongs to
    // the prior object — guard with req.found) or any field change re-stages; an
    // unchanged static object skips the uploader entirely. m_objectBuffer is a
    // single persistent device buffer, so the slot keeps last frame's value when
    // skipped.
    const bool payloadChanged = !req.found || std::memcmp(&payload, &req.state->lastPayload, sizeof(payload)) != 0;
    if (payloadChanged) {
      RIResourceBufferTransaction trans = {};
      trans.target = m_objectBuffer;
      trans.size = sizeof(UniformObject);
      trans.offset = (size_t)slot * sizeof(UniformObject);
      trans.currentState = RI_RESOURCE_STATE_UNORDERED_ACCESS;
      trans.currentStages = RI_STAGE_ALL_SHADER;
      trans.postState = RI_RESOURCE_STATE_UNORDERED_ACCESS;
      trans.postStages = RI_STAGE_ALL_SHADER;
      RI_ResourceBeginCopyBuffer(&RI.device, &RI.uploader, &trans);
      std::memcpy(trans.mapped.data, &payload, sizeof(payload));
      RI_ResourceEndCopyBuffer(&RI.device, &RI.uploader, &trans);
      req.state->lastPayload = payload;
    }
  }

  // (The per-stream BDA handles are now part of the UniformObject payload built
  // above, staged in the single m_objectBuffer[slot] copy — no separate buffers.)
  return slot;
}

void GlobalManagedSets::flushMirrors(RIDevice *device) {
  struct Item {
    RIBuffer *buf;
    BindlessShadowMirror *mir;
  };
  const Item items[] = {
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
    RIResourceBufferTransaction trans = {};
    trans.target = *it.buf;
    trans.size = sz;
    trans.offset = off;
    // Read as storage buffers by the gbuffer VS (vertex pull), the surfel
    // VBuffer / ray-trace chit+ahit, and collectCellInfo (compute). The WAR
    // pre-barrier (currentState/currentStages) waits on the prior frame's reads before
    // the staged copy overwrites the slot; this is the m_objectBuffer path.
    trans.currentState = RI_RESOURCE_STATE_UNORDERED_ACCESS;
    trans.currentStages = RI_STAGE_ALL_SHADER;
    trans.postState = RI_RESOURCE_STATE_UNORDERED_ACCESS;
    trans.postStages = RI_STAGE_ALL_SHADER;
    RI_ResourceBeginCopyBuffer(device, &RI.uploader, &trans);
    std::memcpy(trans.mapped.data, it.mir->shadow.data() + off, sz);
    RI_ResourceEndCopyBuffer(device, &RI.uploader, &trans);
    it.mir->clearDirty();
  }
}

void GlobalManagedSets::destroy(RIDevice *device) {
  m_lightGridCountBuffer.dispose(device);
  m_lightGridCountBuffer = {};
  m_lightGridListBuffer.dispose(device);
  m_lightGridListBuffer = {};
  // Point/spot/area light buffers + the fog buffer are owned + disposed by cWorld.
  m_animTexBuffer.dispose(device);
  m_animTexBuffer = {};
  m_materialBuffer.dispose(device);
  m_materialBuffer = {};
  m_bindlessSet.destroy(device);
}

// === Engine-lifetime set, owned by RIBootstrap (RI.globalset) ===
// One global set 0 for the whole engine. Heap-owned through the RI global so
// there is a single entry point; a pointer (not a value member on RIBootstrap)
// keeps GlobalManagedSets.h's include of RIBootstrap.h cycle-free.
void InitGlobalManagedSets(RIDevice *device, cResources *resources) {
  // Runs in cGraphics::Init before any managed texture is created, so textures
  // write their descriptors directly at load (no catch-up pass needed).
  RI.globalset = new GlobalManagedSets();
  RI.globalset->initialize(device, resources);
}

void ShutdownGlobalManagedSets(RIDevice *device) {
  if (RI.globalset) {
    RI.globalset->destroy(device);
    delete RI.globalset;
    RI.globalset = nullptr;
  }
}

} // namespace hpl
