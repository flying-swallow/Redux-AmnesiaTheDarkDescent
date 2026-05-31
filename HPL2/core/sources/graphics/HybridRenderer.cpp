#include "graphics/HybridRenderer.h"
#include "graphics/RITypes.h"

#include "graphics/GBufferMRTPipelineDesc.h"
#include "graphics/GraphicUtils.h"
#include "graphics/Graphics.h"
#include "graphics/Image.h"
#include "graphics/Material.h"
#include "graphics/MaterialResource.h"
#include "graphics/MaterialType.h"
#include "graphics/ParticlePipelineDesc.h"
#include "graphics/PostEffectComposite.h"
#include "graphics/PostEffectHelpers.h"
#include "graphics/RIBootstrap.h"
#include "graphics/TranslucentMeshPipelineDesc.h"
#include "graphics/DecalPipelineDesc.h"
#include "graphics/RIPogoBuffer.h"
#include "graphics/RIProgramHelpers.h"
#include "graphics/RIResourceUploader.h"
#include "graphics/RIVK.h"
#include "scene/Viewport.h"
#include "graphics/Renderable.h"
#include "graphics/VertexBuffer.h"
#include "graphics/VertexBuffer_RI.h"
#include "math/Frustum.h"
#include "math/Math.h"

#include "resources/Resources.h"
#include "resources/TextureManager.h"
#include "scene/Decal.h"
#include "scene/FogArea.h"
#include "scene/Light.h"
#include "scene/LightBox.h"
#include "scene/LightSpot.h"
#include "scene/ParticleEmitter.h"
#include "scene/RenderableContainer.h"
#include "scene/World.h"
#include "system/LowLevelSystem.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <iterator>
#include <unordered_set>
#include <vector>

namespace hpl {

namespace detail {

// sRGB → linear transfer (IEC 61966-2-1). Inverse of the encode the SRGB
// swapchain applies on write, ussed to bring artist-authored cColor.rgb light
// values into the linear-space lighting math the shaders now run in.
static inline float sRGBToLinear(float c) {
  if (c <= 0.04045f) return c / 12.92f;
  return std::pow((c + 0.055f) / 1.055f, 2.4f);
}

static struct RIBuffer_s CreateBindlessSlotBuffer(RIDevice_s *device,
                                                  uint32_t slotCount,
                                                  size_t elementStride,
                                                  VkBufferUsageFlags usage,
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


cHybridRenderer::cHybridRenderer(cGraphics *apGraphics, cResources *apResources)
    : iRenderer("Hybrid", apGraphics, apResources, 0),
      m_diffuseBindless(kObjectSlotCapacity, RI_NUMBER_FRAMES_FLIGHT),
      m_textureBindless(kTextureSlotCapacity, RI_NUMBER_FRAMES_FLIGHT),
      m_textureCubeBindless(kTextureSlotCapacity, RI_NUMBER_FRAMES_FLIGHT),
      m_materialBindless(kMaterialSlotCapacity, RI_NUMBER_FRAMES_FLIGHT) {
  {
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
      // textures_2d[] — sampled by gbuffer (FS), visibility_shade (FS), and
      // the SurfelGI RT pipeline's any-hit alpha test + closest-hit albedo.
      bindings.push_back(RIBindlessDescriptorSet::Binding{
          kBindingTextures2D, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          kTextureSlotCapacity, kRtSharedStages,
          VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
              VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT});
      // textures_cube[] — point-light gobos + env maps; also sampled by
      // the RT pipeline's miss shader (env-light contribution, Stage E).
      bindings.push_back(RIBindlessDescriptorSet::Binding{
          kBindingTexturesCube, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          kTextureSlotCapacity, kRtSharedStages,
          VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
              VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT});
      // opaque*Handles bindings 3..8 — vertex pulling for gbuffer VS,
      // bindless triangle fetch in visibility_shade FS, and barycentric
      // hit fetches in the SurfelGI RT pipeline.
      for (uint32_t i = 0; i < 6; ++i) {
        bindings.push_back(RIBindlessDescriptorSet::Binding{
            kBindingOpaquePositionHandles + i,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kRtSharedStages, 0});
      }
      // materialSampler — paired with textures_2d at every sample site.
      bindings.push_back(RIBindlessDescriptorSet::Binding{
          kBindingMaterialSampler, VK_DESCRIPTOR_TYPE_SAMPLER, 1,
          kRtSharedStages, 0});
      // SurfelGI SSBOs (bindings 10..19, 27..28). Reachable from compute,
      // ray-tracing, and fragment stages — visibility_shade.frag samples the
      // resulting indirect texture, the update/raytrace passes use compute,
      // and the VBuffer rgen / surfel_rt.rgen write/read these bindings via
      // the ray-tracing pipeline (Stages B, E).
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
          kBindingDecalGridCount,         kBindingDecalGridList,
      };
      const VkShaderStageFlags kSurfelStageFlags =
          VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
          VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
          VK_SHADER_STAGE_ANY_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR;
      for (uint32_t b : kSurfelCellBindings) {
        bindings.push_back(RIBindlessDescriptorSet::Binding{
            b, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kSurfelStageFlags, 0});
      }
      // Scene-object + opaque-material tables (bindings 20..21). Read by the
      // gbuffer pipeline (VS / FS), by visibility_shade.frag, and by the
      // SurfelGI RT pipeline at every hit-shader site.
      const uint32_t kSceneTableBindings[] = {
          kBindingSceneObjects,
          kBindingOpaqueMaterial,
      };
      for (uint32_t b : kSceneTableBindings) {
        bindings.push_back(RIBindlessDescriptorSet::Binding{
            b, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kRtSharedStages, 0});
      }
      // Point/spot/box-light SSBOs (bindings 22, 29, 30). visibility_shade
      // reads all three; the Stage E path-tracer also reads them for NEE.
      bindings.push_back(RIBindlessDescriptorSet::Binding{
          kBindingPointLights, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
          kRtSharedStages, 0});
      bindings.push_back(RIBindlessDescriptorSet::Binding{
          kBindingSpotLights, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
          kRtSharedStages, 0});
      bindings.push_back(RIBindlessDescriptorSet::Binding{
          kBindingBoxLights, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
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

      // gSurfelDepthSampler stays on set 0 — it's an immutable sampler
      // that never collides with the in-flight frame. All other surfel
      // images (gPackedHitInfo / gIrradianceMap / gSurfelDepthMap /
      // gSurfelDepth) plus the TLAS now live on set 1 and get pushed
      // per-dispatch via RIProgram::bindDescriptors (the same path
      // gPerFrame uses), since RIProgram allocates set 1 from a
      // frame-rotated pool that doesn't collide with in-flight frames.
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

      m_bindlessSet.initialize(&RI.device, bindings, poolSizes);
    }

    const VkDescriptorSetLayout externalLayouts[] = {
        m_bindlessSet.vk.m_bindlessSetLayout};
    {
      // Slang gbuffer pass: one .spv with two named entry points
      // (vsMain / psMain). slangc was invoked with
      // -fvk-use-entrypoint-name so the names survive into SPIR-V and the
      // RIProgram loader can request them through pName.
      auto gbuffer_bin = RIProgram::loadShaderStage(
          apResources->GetFileSearcher(), "SurfelGBuffer.3d.spv");
      std::array<RIProgram::ModuleStage, 2> stages = {
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, gbuffer_bin,
                                 "vsMain"},
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, gbuffer_bin,
                                 "psMain"}};
      m_gbuffer.initialize(&RI.device, stages, externalLayouts);
    }

    // SurfelGI compute / RT programs are introduced in stages B–F of the
    // port. The old surfel_prepare / surfel_update / surfel_raytrace /
    // surfel_integrate / surfel_generation_pass programs are gone because
    // their on-disk .comp sources no longer compile against the new shared
    // structs in forward_shared.h; replacements live alongside the new
    // VBuffer (Stage B), update (Stage D), ray-trace (Stage E), and
    // integrate / generation passes (Stage F).
    // SurfelVBuffer — single Slang .spv with four [shader(...)]-attributed
    // entry points (rayGen / miss / closeHit / anyHit). slangc was invoked
    // with -fvk-use-entrypoint-name so the names survive into SPIR-V; share
    // one blob across all four ModuleStage entries, same pattern as
    // m_surfelUpdate*.
    {
      auto vb_bin = RIProgram::loadShaderStage(
          apResources->GetFileSearcher(), "SurfelVBuffer.rt.spv");
      std::array<RIProgram::ModuleStage, 4> stages = {
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_RAYGEN,      vb_bin, "rayGen"},
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_MISS,        vb_bin, "miss"},
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_CLOSEST_HIT, vb_bin, "closeHit"},
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_ANY_HIT,     vb_bin, "anyHit"}};
      m_surfelVBuffer.initialize(&RI.device, stages, externalLayouts);
    }
    auto loadComputeProgram = [&](RIProgram &prog, const char *name) {
      auto bin = RIProgram::loadShaderStage(apResources->GetFileSearcher(), name);
      std::array<RIProgram::ModuleStage, 1> stages = {RIProgram::ModuleStage{
          RIProgram::PROGRAM_STAGE_COMPUTE, bin}};
      prog.initialize(&RI.device, stages, externalLayouts);
    };
    // Slang-compiled compute. Same load path, but the entry-point name in
    // the SPV is the Slang function name (slangc is invoked with
    // -fvk-use-entrypoint-name), so we pass it through to ModuleStage.
    auto loadSlangCompute = [&](RIProgram &prog, const char *name,
                                const char *entryPoint) {
      auto bin = RIProgram::loadShaderStage(apResources->GetFileSearcher(), name);
      std::array<RIProgram::ModuleStage, 1> stages = {RIProgram::ModuleStage{
          RIProgram::PROGRAM_STAGE_COMPUTE, bin, entryPoint}};
      prog.initialize(&RI.device, stages, externalLayouts);
    };
    // SurfelPreparePass: the only buffer it touches is gSurfelCounter
    // (kBindingSurfelCounter on the bindless set 0), so the dispatch
    // site is unchanged — only the loaded SPV + entry point differ.
    loadSlangCompute(m_surfelPrepare, "SurfelPreparePass.cs.spv", "csMain");
    // Cell-clearing + ref-counter-clearing are now folded into the
    // SurfelPreparePass + SurfelUpdatePass Slang side; the dedicated
    // GLSL clear passes were removed in the GLSL wipe.
    // SurfelUpdatePass — single .spv with three [numthreads]-marked entry
    // points (collectCellInfo / accumulateCellInfo / updateCellToSurfelBuffer).
    // Load the blob once and reuse the std::span<char> across three
    // initializations; the vector must stay in scope through all three
    // `initialize()` calls because ModuleStage holds a non-owning view.
    {
      auto upd_bin = RIProgram::loadShaderStage(
          apResources->GetFileSearcher(), "SurfelUpdatePass.cs.spv");
      auto initFromBlob = [&](RIProgram &prog, const char *entryPoint) {
        std::array<RIProgram::ModuleStage, 1> stages = {RIProgram::ModuleStage{
            RIProgram::PROGRAM_STAGE_COMPUTE, upd_bin, entryPoint}};
        prog.initialize(&RI.device, stages, externalLayouts);
      };
      initFromBlob(m_surfelUpdateCollect,    "collectCellInfo");
      initFromBlob(m_surfelUpdateAccumulate, "accumulateCellInfo");
      initFromBlob(m_surfelUpdateScatter,    "updateCellToSurfelBuffer");
    }
    // SurfelRayTrace — single Slang .spv with four [shader(...)] entry points
    // (rayGen / scatterMiss / scatterCloseHit / scatterAnyHit). Shadow rays
    // use inline RayQuery in the same shader so no second miss / anyhit
    // group is needed — keeps the SBT inside RIProgram's single-ray-type
    // hit-group layout.
    {
      auto rt_bin = RIProgram::loadShaderStage(
          apResources->GetFileSearcher(), "SurfelRayTrace.rt.spv");
      std::array<RIProgram::ModuleStage, 4> stages = {
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_RAYGEN,      rt_bin, "rayGen"},
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_MISS,        rt_bin, "scatterMiss"},
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_CLOSEST_HIT, rt_bin, "scatterCloseHit"},
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_ANY_HIT,     rt_bin, "scatterAnyHit"}};
      m_surfelRT.initialize(&RI.device, stages, externalLayouts);
    }
    // LightGridBuildPass — single compute entry (binLights) that bins point/spot
    // lights into the coarse world-space light grid each frame.
    loadSlangCompute(m_lightGridBin, "LightGridBuildPass.cs.spv", "binLights");
    loadSlangCompute(m_decalGridBin, "DecalGridBuildPass.cs.spv", "binDecals");
    loadSlangCompute(m_surfelIntegrate,  "SurfelIntegratePass.cs.spv",   "csMain");
    loadSlangCompute(m_surfelGenerate,   "SurfelGenerationPass.cs.spv",  "csMain");
    // SurfelGIRender — now a graphics pass writing into the pogo buffer's
    // color attachment (was a compute pass writing into the swapchain as
    // a storage image). The post-effect chain ping-pongs through pogo
    // and a tail blit copies the final pogo half into the swapchain;
    // keeping the surfel composite as a fragment pass means the same
    // COLOR_ATTACHMENT ↔ FRAGMENT_SHADER barrier helpers in RIPogoBuffer
    // cover the entire chain.
    LoadSlangGraphics(&RI.device, m_surfelGIRender, apResources,
                      "posteffect_fullscreen.vert.spv",
                      "SurfelGIRenderPass.frag.spv", "vsMain", "psMain",
                      externalLayouts);
    // (Albedo resolve pass removed — SurfelGIRenderPass now computes albedo +
    // composites decals inline.)
    // The pogo "read" half -> swapchain tail blit now lives in cScene (after the
    // viewport's post-effect composite), using RI.postEffectBlit.
    {
      // Slang-compiled (amnesia/slang/ParticlePass) — entry-point names go in
      // the SPV as-is because slangc is invoked with -fvk-use-entrypoint-name.
      auto p_vert = RIProgram::loadShaderStage(apResources->GetFileSearcher(),
                                               "Particle.vert.spv");
      auto p_frag = RIProgram::loadShaderStage(apResources->GetFileSearcher(),
                                               "Particle.frag.spv");
      std::array<RIProgram::ModuleStage, 2> stages = {
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, p_vert, "vsMain"},
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, p_frag, "psMain"}};
      m_particle.initialize(&RI.device, stages, externalLayouts);
    }
    {
      // Translucent mesh pass (amnesia/slang/TranslucentPass). Shares
      // externalLayouts with m_particle so the same bindless set / per-frame
      // UBO bindings light up.
      auto t_vert = RIProgram::loadShaderStage(apResources->GetFileSearcher(),
                                               "Translucent.vert.spv");
      auto t_frag = RIProgram::loadShaderStage(apResources->GetFileSearcher(),
                                               "Translucent.frag.spv");
      std::array<RIProgram::ModuleStage, 2> stages = {
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, t_vert, "vsMain"},
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, t_frag, "psMain"}};
      m_translucentMesh.initialize(&RI.device, stages, externalLayouts);
    }
    {
      // Decal pass (amnesia/slang/DecalPass). Shares externalLayouts with
      // m_translucentMesh so the same bindless set / per-frame UBO bindings
      // light up; reuses the translucent 5-stream vertex layout. Port of
      // decal.vert.fsl / decal.frag.fsl.
      auto d_vert = RIProgram::loadShaderStage(apResources->GetFileSearcher(),
                                               "Decal.vert.spv");
      auto d_frag = RIProgram::loadShaderStage(apResources->GetFileSearcher(),
                                               "Decal.frag.spv");
      std::array<RIProgram::ModuleStage, 2> stages = {
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, d_vert, "vsMain"},
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, d_frag, "psMain"}};
      m_decal.initialize(&RI.device, stages, externalLayouts);
    }
    // Water is no longer a raster pass — it's composited in
    // SurfelGIRenderPass.frag (isWater branch) from the primary hit + the
    // refraction / reflection bounce V-buffers.

    const VkBufferUsageFlags kStorage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT;  // surfelValid→surfelDirty ping-pong copy
    m_diffuseObjectBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, kObjectSlotCapacity, sizeof(UniformObject), kStorage,
        /*deviceLocalOnly*/ true);
    m_opaquePositionHandles = detail::CreateBindlessSlotBuffer(
        &RI.device, kObjectSlotCapacity, sizeof(VkDeviceAddress), kStorage,
        /*deviceLocalOnly*/ true);
    m_opaqueTangentHandles = detail::CreateBindlessSlotBuffer(
        &RI.device, kObjectSlotCapacity, sizeof(VkDeviceAddress), kStorage,
        /*deviceLocalOnly*/ true);
    m_opaqueNormalHandles = detail::CreateBindlessSlotBuffer(
        &RI.device, kObjectSlotCapacity, sizeof(VkDeviceAddress), kStorage,
        /*deviceLocalOnly*/ true);
    m_opaqueUv0Handles = detail::CreateBindlessSlotBuffer(
        &RI.device, kObjectSlotCapacity, sizeof(VkDeviceAddress), kStorage,
        /*deviceLocalOnly*/ true);
    m_opaqueColorHandles = detail::CreateBindlessSlotBuffer(
        &RI.device, kObjectSlotCapacity, sizeof(VkDeviceAddress), kStorage,
        /*deviceLocalOnly*/ true);
    m_opaqueIndexHandles = detail::CreateBindlessSlotBuffer(
        &RI.device, kObjectSlotCapacity, sizeof(VkDeviceAddress), kStorage,
        /*deviceLocalOnly*/ true);

    RISegmentAllocDesc_s indirectDesc = {};
    indirectDesc.numSegments = RI_NUMBER_FRAMES_FLIGHT;
    indirectDesc.elementStride = sizeof(VkDrawIndirectCommand);
    indirectDesc.maxElements = (uint16_t)kObjectSlotCapacity;
    m_indirectSegment = RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS>(&indirectDesc);
    m_indirectDrawBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, indirectDesc.maxElements, sizeof(VkDrawIndirectCommand),
        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    // === SurfelGI SSBOs (bindless.resource.glsl set=0, bindings 10..19, 27..28) ===
    // Sizes come from forward_shared.h:
    //   surfelCounter         : kSurfelCounterSlotCount × uint32
    //   surfel/geometry/etc   : kTotalSurfelLimit × element
    //   surfelRayResult       : kRayBudget × SurfelRayResult (~460 MB)
    //   cellInfo / reservation: kCellCount × element  (15.6M cells × 8B/4B)
    //   cellToSurfel          : kCellToSurfelCapacity × uint32  (75 MB)
    // The reference layout adds m_surfelGeometryBuffer (cached uint4 triangle
    // hit per surfel) and m_surfelRayResultBuffer (replaces the old SurfelRay
    // type with a larger SurfelRayResult). m_cellCounterBuffer is gone — its
    // contents fold into surfelCounter[kSurfelCounterCell].
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
        &RI.device, kSurfelCounterSlotCount, sizeof(uint32_t), kStorage,
        /*deviceLocalOnly*/ true);
    m_surfelBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, kTotalSurfelLimit, sizeof(Surfel), kStorage);
    m_surfelGeometryBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, kTotalSurfelLimit, sizeof(uint32_t) * 4u, kStorage);
    m_surfelValidBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, kTotalSurfelLimit, sizeof(uint32_t), kStorage);
    m_surfelDirtyIndexBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, kTotalSurfelLimit, sizeof(uint32_t), kStorage);
    m_surfelFreeBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, kTotalSurfelLimit, sizeof(uint32_t), kStorage);
    m_surfelRecycleBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, kTotalSurfelLimit, sizeof(SurfelRecycleInfo), kStorage);
    m_surfelRayResultBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, kRayBudget, sizeof(SurfelRayResult), kStorage);
    m_surfelRefCounterBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, kTotalSurfelLimit, sizeof(uint32_t), kStorage);
    m_surfelReservationBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, kCellCount, sizeof(uint32_t), kStorage);
    m_cellInfoBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, kCellCount, sizeof(CellInfo), kStorage);
    m_cellToSurfelBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, kCellToSurfelCapacity, sizeof(uint32_t), kStorage);
    // Compact cull record per surfel; written by collectCellInfo before the
    // generation pass reads it, so no defensive zeroing is needed (every index
    // pulled from a cell list has already passed through collect this frame).
    m_surfelBoundsBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, kTotalSurfelLimit, sizeof(SurfelBounds), kStorage);
    // Coarse world-space light grid. GPU-only: zeroed each frame via
    // vkCmdFillBuffer and (re)filled by LightGridBuildPass before the ray trace,
    // so no host seeding / mapping is needed (deviceLocalOnly).
    m_lightGridCountBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, kLightGridCellCount, sizeof(uint32_t), kStorage,
        /*deviceLocalOnly*/ true);
    m_lightGridListBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, kLightGridCellCount * kLightsPerCellMax, sizeof(uint32_t),
        kStorage, /*deviceLocalOnly*/ true);
    // Decal grid (its own cell layout, independent of the light grid). GPU-only:
    // zeroed each frame via vkCmdFillBuffer and (re)filled by DecalGridBuildPass
    // before the albedo resolve.
    m_decalGridCountBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, kDecalGridCellCount, sizeof(uint32_t), kStorage,
        /*deviceLocalOnly*/ true);
    m_decalGridListBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, kDecalGridCellCount * kDecalsPerCellMax, sizeof(uint32_t),
        kStorage, /*deviceLocalOnly*/ true);

    // Seed the surfel free-list. gSurfelCounter[Free] is a stack pointer and
    // gSurfelFreeIndexBuffer holds the available slot indices; at boot every
    // slot is free, so Free = kTotalSurfelLimit and the index buffer is iota
    // [0..kTotalSurfelLimit). Without this the free list starts empty (Free=0):
    // SurfelGenerationPass can never allocate a surfel (its InterlockedAdd(Free,
    // -1) underflows past zero, wrapping to ~2^32 — which SurfelPreparePass then
    // clamps back to 0 via clamp(asint(Free),0,limit)), so Valid/Dirty stay 0
    // forever => no rays => no indirect light. The free list can't bootstrap
    // from the recycle path either: collectCellInfo only pushes freed slots for
    // *dirty* surfels, and there are never any. The Falcor reference seeds the
    // equivalent via kInitialStatus={0,0,kTotalSurfelLimit,...} + an iota free
    // buffer. m_surfelFreeBuffer is host-mapped so its iota seed is written
    // directly here; m_surfelCounterBuffer is device-local, so its Free seed is
    // staged through m_surfelCounterMirror (set up with the other mirrors below)
    // on the first frame's flushBindlessMirrors().
    {
      // m_surfelCounterBuffer is now device-local; its boot seed (Free =
      // kTotalSurfelLimit) is staged through m_surfelCounterMirror below.
      // m_surfelFreeBuffer stays host-mapped, so seed its iota free list here.
      auto *freeIdx =
          static_cast<uint32_t *>(m_surfelFreeBuffer.mappedAddress);
      for (uint32_t i = 0; i < kTotalSurfelLimit; ++i)
        freeIdx[i] = i;
    }

    // Slot-reuse generation buffers (see m_bindlessSlotGenerationBuffer). Both
    // start at 0; the host bumps a slot's generation to a unique nonzero value
    // the first time it's assigned (below in Draw), so a real surfel always
    // captures a matching value and 0 can never alias a live slot.
    m_bindlessSlotGenerationBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, kObjectSlotCapacity, sizeof(uint32_t), kStorage,
        /*deviceLocalOnly*/ true);
    m_surfelSlotGenerationBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, kTotalSurfelLimit, sizeof(uint32_t), kStorage);
    // m_bindlessSlotGenerationBuffer is now device-local (no mappedAddress); the
    // CPU shadow mirror is zero-initialized by init() below and markAllDirty()
    // forces the first frame's flushBindlessMirrors() to seed the device buffer
    // to zero before collectCellInfo reads it. m_surfelSlotGenerationBuffer stays
    // host-mapped (GPU-written each frame; only this boot zero touches it).
    std::memset(m_surfelSlotGenerationBuffer.mappedAddress, 0,
                (size_t)kTotalSurfelLimit * sizeof(uint32_t));
    // Per-slot "geometry rebuilt" flags, parallel to the generation buffer.
    m_slotGeomDirty.assign(kObjectSlotCapacity, 0u);

    // Shadow mirrors for the seven device-local bindless slot buffers. All host
    // writes target these (never GPU-mapped memory); flushBindlessMirrors()
    // stages the dirty range once per frame. markAllDirty() seeds the device
    // buffers from the zeroed shadow on the first frame, giving a clean
    // device == mirror invariant for every slot.
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
    // stages the seed on the first frame's flushBindlessMirrors(), which lands
    // ahead of the surfel passes via RI.uploader's fenced pre-pass. The host
    // never touches this mirror again — the GPU owns the counter thereafter.
    m_surfelCounterMirror.init(kSurfelCounterSlotCount, sizeof(uint32_t));
    m_surfelCounterMirror.write<uint32_t>(kSurfelCounterFree, kTotalSurfelLimit);
    m_surfelCounterMirror.markAllDirty();


    // Light SSBOs (point/spot/box). Unlike the bindless slot buffers above
    // these are device-local — the per-frame fill in Draw() stages through
    // RI.uploader rather than memcpy'ing into mapped memory the GPU may
    // still be reading from a prior frame.
    {
      uint32_t queueFamilies[RI_QUEUE_LEN] = {0};
      VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
      VK_ConfigureBufferQueueFamilies(&bci, RI.device.queues, RI_QUEUE_LEN,
                                      queueFamilies, RI_QUEUE_LEN);
      bci.usage =
          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

      VmaAllocationCreateInfo aci = {};
      aci.usage = VMA_MEMORY_USAGE_AUTO;

      bci.size = (VkDeviceSize)kPointSlotLightCapacity * sizeof(PointLight);
      VK_WrapResult(vmaCreateBuffer(
          RI.device.vk.vmaAllocator, &bci, &aci, &m_pointLightBuffer.vk.buffer,
          &m_pointLightBuffer.vk.allocation, nullptr));

      bci.size = (VkDeviceSize)kSpotSlotLightCapacity * sizeof(SpotLight);
      VK_WrapResult(vmaCreateBuffer(RI.device.vk.vmaAllocator, &bci, &aci,
                                    &m_spotLightBuffer.vk.buffer,
                                    &m_spotLightBuffer.vk.allocation, nullptr));

      bci.size = (VkDeviceSize)kBoxSlotLightCapacity * sizeof(BoxLight);
      VK_WrapResult(vmaCreateBuffer(RI.device.vk.vmaAllocator, &bci, &aci,
                                    &m_boxLightBuffer.vk.buffer,
                                    &m_boxLightBuffer.vk.allocation, nullptr));

      bci.size = (VkDeviceSize)kFogAreaCapacity * sizeof(FogAreaParams);
      VK_WrapResult(vmaCreateBuffer(RI.device.vk.vmaAllocator, &bci, &aci,
                                    &m_fogAreaBuffer.vk.buffer,
                                    &m_fogAreaBuffer.vk.allocation, nullptr));

      bci.size = (VkDeviceSize)kMaxDecals * sizeof(GpuDecal);
      VK_WrapResult(vmaCreateBuffer(RI.device.vk.vmaAllocator, &bci, &aci,
                                    &m_decalBuffer.vk.buffer,
                                    &m_decalBuffer.vk.allocation, nullptr));

      // Default-value fallback vertex buffers for translucent renderables
      // missing one or more streams (cBillboard, cBeam — see translucent
      // loop). Each buffer's stride matches its binding slot in
      // TranslucentMeshPipelineDesc; filled once below via the resource
      // uploader. Reads from absent-stream renderables (no NormalMap
      // material on billboards/beams) don't visually consume these values
      // in Translucent.frag.slang.
      bci.usage =
          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

      struct FallbackSpec {
        RIBuffer_s  *target;
        uint32_t     componentsWritten;  // per vertex
        uint32_t     stride;             // binding stride in TranslucentMeshPipelineDesc
        float        value[4];           // pad unused components with 0
      };
      const FallbackSpec specs[] = {
          {&m_translucentNormalFallback,  3, 12, {0.f, 0.f, 1.f, 0.f}},  // +Z
          {&m_translucentTangentFallback, 4, 16, {1.f, 0.f, 0.f, 1.f}},  // +X, handedness +1
          {&m_translucentColorFallback,   4, 16, {1.f, 1.f, 1.f, 1.f}},  // white
          {&m_translucentUv0Fallback,     3, 12, {0.f, 0.f, 0.f, 0.f}},  // origin
      };
      for (const FallbackSpec &s : specs) {
        bci.size = (VkDeviceSize)kTranslucentFallbackVerts *
                   (VkDeviceSize)s.stride;
        VK_WrapResult(vmaCreateBuffer(RI.device.vk.vmaAllocator, &bci, &aci,
                                      &s.target->vk.buffer,
                                      &s.target->vk.allocation, nullptr));

        RIResourceBufferTransaction_s trans = {};
        trans.target = *s.target;
        trans.size = (size_t)bci.size;
        trans.offset = 0;
        trans.vk.current_stage = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
        trans.vk.current_access = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
        trans.vk.post_stage = trans.vk.current_stage;
        trans.vk.post_access = trans.vk.current_access;
        RI_ResourceBeginCopyBuffer(&RI.device, &RI.uploader, &trans);
        uint8_t *dst = reinterpret_cast<uint8_t *>(trans.mapped.data);
        // Zero the whole buffer so any pad bytes between strided entries
        // are deterministic, then stamp the components per vertex.
        std::memset(dst, 0, (size_t)bci.size);
        for (uint32_t v = 0; v < kTranslucentFallbackVerts; ++v) {
          float *fdst = reinterpret_cast<float *>(dst + (size_t)v * s.stride);
          for (uint32_t c = 0; c < s.componentsWritten; ++c)
            fdst[c] = s.value[c];
        }
        RI_ResourceEndCopyBuffer(&RI.device, &RI.uploader, &trans);
      }
    }

    // Surfel-generation output image — one storage texture per swapchain
    // image. RGBA16F so HDR radiance survives; SAMPLED so a future
    // composite pass can read it back. View aspect color, full mip 0.
    // Sized at FULL swapchain resolution: surfel_generation_pass dispatches
    // at imageRes = viewportSize and writes every pixel, so every texel of
    // this image is initialised each frame. visibility_shade.frag samples
    // uv01 across the full [0,1] range and reads valid radiance.
    m_surfelResultWidth  = RI.swapchain.width;
    m_surfelResultHeight = RI.swapchain.height;
    for (uint32_t i = 0; i < RI.swapchain.imageCount; ++i) {
      uint32_t queueFamilies[RI_QUEUE_LEN] = {0};
      VkImageCreateInfo imgInfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      imgInfo.imageType = VK_IMAGE_TYPE_2D;
      imgInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
      imgInfo.extent = {m_surfelResultWidth, m_surfelResultHeight, 1};
      imgInfo.mipLevels = 1;
      imgInfo.arrayLayers = 1;
      imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
      imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
      // TRANSFER_DST is needed for the per-frame vkCmdClearColorImage in
      // Draw() — the generation pass only writes pixels with surfel
      // contribution, so the rest must be explicitly zeroed each frame.
      imgInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT |
                      VK_IMAGE_USAGE_SAMPLED_BIT |
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      VK_ConfigureImageQueueFamilies(&imgInfo, RI.device.queues, RI_QUEUE_LEN,
                                     queueFamilies, RI_QUEUE_LEN);
      imgInfo.pQueueFamilyIndices = queueFamilies;

      VmaAllocationCreateInfo alloc = {};
      alloc.usage = VMA_MEMORY_USAGE_AUTO;
      VK_WrapResult(vmaCreateImage(RI.device.vk.vmaAllocator, &imgInfo, &alloc,
                                   &m_surfelResultTexture[i].vk.image,
                                   &m_surfelResultTexture[i].vk.allocation,
                                   NULL));

      VkImageViewCreateInfo viewInfo = {
          VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      viewInfo.image = m_surfelResultTexture[i].vk.image;
      viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
      viewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
      viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      VK_WrapResult(vkCreateImageView(RI.device.vk.device, &viewInfo, NULL,
                                      &m_surfelResultView[i].vk.image));
    }

    // (Screen-space albedo buffer removed — SurfelGIRenderPass computes albedo
    // inline; no resolve target / sampled buffer needed.)

    // Stage B packed visibility — RGBA32UI storage image written by the
    // surfel_vbuffer RT pipeline and sampled by Stage D / F passes. Same
    // swapchain-sized footprint as m_surfelResultTexture.
    for (uint32_t i = 0; i < RI.swapchain.imageCount; ++i) {
      uint32_t queueFamilies[RI_QUEUE_LEN] = {0};
      VkImageCreateInfo imgInfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      imgInfo.imageType = VK_IMAGE_TYPE_2D;
      imgInfo.format = VK_FORMAT_R32G32B32A32_UINT;
      imgInfo.extent = {m_surfelResultWidth, m_surfelResultHeight, 1};
      imgInfo.mipLevels = 1;
      imgInfo.arrayLayers = 1;
      imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
      imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
      imgInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
      imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      VK_ConfigureImageQueueFamilies(&imgInfo, RI.device.queues, RI_QUEUE_LEN,
                                     queueFamilies, RI_QUEUE_LEN);
      imgInfo.pQueueFamilyIndices = queueFamilies;

      VmaAllocationCreateInfo alloc = {};
      alloc.usage = VMA_MEMORY_USAGE_AUTO;
      VK_WrapResult(vmaCreateImage(RI.device.vk.vmaAllocator, &imgInfo, &alloc,
                                   &m_packedHitInfoTexture[i].vk.image,
                                   &m_packedHitInfoTexture[i].vk.allocation,
                                   NULL));

      VkImageViewCreateInfo viewInfo = {
          VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      viewInfo.image = m_packedHitInfoTexture[i].vk.image;
      viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
      viewInfo.format = VK_FORMAT_R32G32B32A32_UINT;
      viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      VK_WrapResult(vkCreateImageView(RI.device.vk.device, &viewInfo, NULL,
                                      &m_packedHitInfoView[i].vk.image));
    }

    // Per-bounce V-buffers — same format / dims / usage as m_packedHitInfoTexture
    // plus TRANSFER_DST so we can vkCmdClearColorImage them to uint4(0) at
    // the start of each frame's RT pass. SurfelVBuffer.rt.slang's closeHit
    // writes only the pixels whose primary hit was refractive / reflective;
    // the rest stay at the cleared "invalid" sentinel.
    {
      struct PerBounceTarget {
        RITexture_s     (&tex)[RI_MAX_SWAPCHAIN_IMAGES];
        RITextureView_s (&view)[RI_MAX_SWAPCHAIN_IMAGES];
      };
      PerBounceTarget targets[2] = {
          {m_packedRefractionHitInfoTexture, m_packedRefractionHitInfoView},
          {m_packedReflectionHitInfoTexture, m_packedReflectionHitInfoView},
      };
      for (auto &t : targets) {
        for (uint32_t i = 0; i < RI.swapchain.imageCount; ++i) {
          uint32_t queueFamilies[RI_QUEUE_LEN] = {0};
          VkImageCreateInfo imgInfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
          imgInfo.imageType = VK_IMAGE_TYPE_2D;
          imgInfo.format = VK_FORMAT_R32G32B32A32_UINT;
          imgInfo.extent = {m_surfelResultWidth, m_surfelResultHeight, 1};
          imgInfo.mipLevels = 1;
          imgInfo.arrayLayers = 1;
          imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
          imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
          imgInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT |
                          VK_IMAGE_USAGE_SAMPLED_BIT |
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT;
          imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
          VK_ConfigureImageQueueFamilies(&imgInfo, RI.device.queues, RI_QUEUE_LEN,
                                         queueFamilies, RI_QUEUE_LEN);
          imgInfo.pQueueFamilyIndices = queueFamilies;

          VmaAllocationCreateInfo alloc = {};
          alloc.usage = VMA_MEMORY_USAGE_AUTO;
          VK_WrapResult(vmaCreateImage(RI.device.vk.vmaAllocator, &imgInfo,
                                       &alloc, &t.tex[i].vk.image,
                                       &t.tex[i].vk.allocation, NULL));

          VkImageViewCreateInfo viewInfo = {
              VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
          viewInfo.image = t.tex[i].vk.image;
          viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
          viewInfo.format = VK_FORMAT_R32G32B32A32_UINT;
          viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
          VK_WrapResult(vkCreateImageView(RI.device.vk.device, &viewInfo, NULL,
                                          &t.view[i].vk.image));
        }
      }
    }

    // Surfel-ray irradiance atlas — single-channel R16F, 4096x4096 fits
    // kTotalSurfelLimit surfels at 6x6 cells each (the shader computes
    // tile pos as `surfelIndex % (W/6), surfelIndex / (W/6)`). SAMPLED so the
    // raytrace shader's ray-guiding branch can read it; STORAGE so a future
    // accumulation pass can write into it. Untouched here — stays at the
    // SHADER_READ_ONLY_OPTIMAL layout after the first transition below.
    for (uint32_t i = 0; i < RI.swapchain.imageCount; ++i) {
      uint32_t queueFamilies[RI_QUEUE_LEN] = {0};
      VkImageCreateInfo imgInfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      imgInfo.imageType = VK_IMAGE_TYPE_2D;
      imgInfo.format = VK_FORMAT_R16_SFLOAT;
      imgInfo.extent = {4096u, 4096u, 1u};
      imgInfo.mipLevels = 1;
      imgInfo.arrayLayers = 1;
      imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
      imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
      // TRANSFER_DST: this atlas is seeded once via vkCmdClearColorImage.
      imgInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      VK_ConfigureImageQueueFamilies(&imgInfo, RI.device.queues, RI_QUEUE_LEN,
                                     queueFamilies, RI_QUEUE_LEN);
      imgInfo.pQueueFamilyIndices = queueFamilies;

      VmaAllocationCreateInfo alloc = {};
      alloc.usage = VMA_MEMORY_USAGE_AUTO;
      VK_WrapResult(vmaCreateImage(RI.device.vk.vmaAllocator, &imgInfo, &alloc,
                                   &m_surfelIrradianceTexture[i].vk.image,
                                   &m_surfelIrradianceTexture[i].vk.allocation,
                                   NULL));

      VkImageViewCreateInfo viewInfo = {
          VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      viewInfo.image = m_surfelIrradianceTexture[i].vk.image;
      viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
      viewInfo.format = VK_FORMAT_R16_SFLOAT;
      viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      VK_WrapResult(vkCreateImageView(RI.device.vk.device, &viewInfo, NULL,
                                      &m_surfelIrradianceView[i].vk.image));
    }

    // Surfel-ray depth atlas — RGBA16F storing the (E[z], E[z^2]) Chebyshev
    // pair per octahedral tile texel for each surfel (only .rg are populated;
    // .ba left zero). Same 4096x4096 footprint as the irradiance atlas so the
    // integrate shader's tile-index math (surfelIndex % (W/6), surfelIndex /
    // (W/6)) resolves identically for both atlases. STORAGE for integrate
    // writes, SAMPLED for integrate's own readback.
    for (uint32_t i = 0; i < RI.swapchain.imageCount; ++i) {
      uint32_t queueFamilies[RI_QUEUE_LEN] = {0};
      VkImageCreateInfo imgInfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      imgInfo.imageType = VK_IMAGE_TYPE_2D;
      imgInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
      imgInfo.extent = {4096u, 4096u, 1u};
      imgInfo.mipLevels = 1;
      imgInfo.arrayLayers = 1;
      imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
      imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
      // TRANSFER_DST: this atlas is seeded once via vkCmdClearColorImage.
      imgInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      VK_ConfigureImageQueueFamilies(&imgInfo, RI.device.queues, RI_QUEUE_LEN,
                                     queueFamilies, RI_QUEUE_LEN);
      imgInfo.pQueueFamilyIndices = queueFamilies;

      VmaAllocationCreateInfo alloc = {};
      alloc.usage = VMA_MEMORY_USAGE_AUTO;
      VK_WrapResult(vmaCreateImage(RI.device.vk.vmaAllocator, &imgInfo, &alloc,
                                   &m_surfelDepthTexture[i].vk.image,
                                   &m_surfelDepthTexture[i].vk.allocation,
                                   NULL));

      VkImageViewCreateInfo viewInfo = {
          VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      viewInfo.image = m_surfelDepthTexture[i].vk.image;
      viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
      viewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
      viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      VK_WrapResult(vkCreateImageView(RI.device.vk.device, &viewInfo, NULL,
                                      &m_surfelDepthView[i].vk.image));
    }

    m_materialBindless.reset(kMaterialSlotCapacity);
    m_opaqueMaterialBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, kMaterialSlotCapacity, sizeof(DiffuseMaterial), kStorage,
        /*deviceLocalOnly*/ true);
    // Parallel WaterMaterial SSBO — shares the m_materialBindless slot
    // numbering, populated alongside DiffuseMaterial in resolveMaterial when
    // the cMaterial's MaterialID is Water. Water.frag.slang reads
    // gWaterMaterials[materialID] for wave / Fresnel / fade scalars and
    // still falls through to gDiffuseMaterials[materialID] for the cube-map
    // slot.
    m_waterMaterialBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, kMaterialSlotCapacity, sizeof(WaterMaterial), kStorage,
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
          {kBindingDecalGridCount, &m_decalGridCountBuffer,
           kDecalGridCellCount * sizeof(uint32_t)},
          {kBindingDecalGridList, &m_decalGridListBuffer,
           (size_t)kDecalGridCellCount * kDecalsPerCellMax * sizeof(uint32_t)},
          {kBindingLightGridCount, &m_lightGridCountBuffer,
           kLightGridCellCount * sizeof(uint32_t)},
          {kBindingLightGridList, &m_lightGridListBuffer,
           (size_t)kLightGridCellCount * kLightsPerCellMax * sizeof(uint32_t)},
          {kBindingSceneObjects, &m_diffuseObjectBuffer,
           kObjectSlotCapacity * sizeof(UniformObject)},
          {kBindingOpaqueMaterial, &m_opaqueMaterialBuffer,
           kMaterialSlotCapacity * sizeof(DiffuseMaterial)},
          {kBindingPointLights, &m_pointLightBuffer,
           kPointSlotLightCapacity * sizeof(PointLight)},
          {kBindingSpotLights, &m_spotLightBuffer,
           kSpotSlotLightCapacity * sizeof(SpotLight)},
          {kBindingBoxLights, &m_boxLightBuffer,
           kBoxSlotLightCapacity * sizeof(BoxLight)},
          {kBindingFogAreas, &m_fogAreaBuffer,
           kFogAreaCapacity * sizeof(FogAreaParams)},
          {kBindingDecals, &m_decalBuffer,
           kMaxDecals * sizeof(GpuDecal)},
          {kBindingWaterMaterial, &m_waterMaterialBuffer,
           kMaterialSlotCapacity * sizeof(WaterMaterial)},
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
          mpResources->GetTextureManager()->Create1DImage("core_falloff_linear", false);
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
      m_bindlessSet.writeDescriptors(&RI.device,
                                     std::span(writes).subspan(0, count));
    }
  }
}

uint32_t cHybridRenderer::resolveTextureSlot(RIBootstrap::FrameContext *cntx,
                                             Image *img, uint32_t frameIndex) {
  if (!img)
    return kInvalidTextureIndex;
  auto texture = img->GetTexture();
  if (!texture)
    return kInvalidTextureIndex;
  const hash_t texture_cookie =
      hash_u64(HASH_INITIAL_VALUE, (uint64_t)(uintptr_t)texture.get());
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

uint32_t cHybridRenderer::resolveCubeTextureSlot(
    RIBootstrap::FrameContext *cntx, Image *img, uint32_t frameIndex) {
  if (!img)
    return kInvalidTextureIndex;
  auto texture = img->GetTexture();
  if (!texture)
    return kInvalidTextureIndex;

  const hash_t texture_cookie =
      hash_u64(HASH_INITIAL_VALUE, (uint64_t)(uintptr_t)texture.get());
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

uint32_t cHybridRenderer::resolveMaterial(RIBootstrap::FrameContext *cntx,
                                          cMaterial *mat, uint32_t frameIndex) {
  auto slotFor = [&](eMaterialTexture type) -> uint32_t {
    return resolveTextureSlot(cntx, mat->GetImage(type), frameIndex);
  };

  // Slot layout must match the DiffuseMaterial_*Texture_ID accessors in
  // amnesia/glsl/per_frame.resource.glsl. One uint32 per texture index.
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
  // Reflection cube map — separate bindless table (textures_cube[]), so
  // resolve via the cube allocator rather than slotFor (which only handles
  // 2D). Lives outside tex[] in the GPU struct to mirror the legacy
  // shader-global cubeMap.
  gpu.cubeMapTextureIndex = resolveCubeTextureSlot(
      cntx, mat->GetImage(eMaterialTexture_CubeMap), frameIndex);
  // Material config bits — single source of truth lives in MaterialResource.
  // The visibility composite reads bit 9 (IsHeightMapSingleChannel) to pick
  // .r vs .a when sampling the heightmap. Without this assignment the field
  // sat at zero and parallax always read .a (constant 1.0 for typical single-
  // channel Amnesia heightmaps), running the full POM loop every fragment and
  // producing severe warping at grazing angles on vertical walls.
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
  } else if (desc.m_id == MaterialID::Translucent) {
    // Translucent shares the DiffuseMaterial slot — Fresnel/rim/refraction
    // scalars feed the cube-map reflection + screen-color refraction paths
    // in Translucent.frag.slang.
    gpu.frenselBias     = desc.m_translucent.m_frenselBias;
    gpu.frenselPow      = desc.m_translucent.m_frenselPow;
    gpu.refractionScale = desc.m_translucent.m_refractionScale;
    gpu.rimLightMul     = desc.m_translucent.m_rimLightMul;
    gpu.rimLightPow     = desc.m_translucent.m_rimLightPow;
  }

  hash_t cookie = hash_u64(HASH_INITIAL_VALUE, (uint64_t)(uintptr_t)mat);
  cookie = hash_u64(cookie, (uint64_t)mat->Generation());
  cookie = hash_data(cookie, &gpu, sizeof(gpu));
  auto req = m_materialBindless.request(cookie, frameIndex);
  if (req.exhausted)
    return UINT32_MAX;
  if (req.found)
    return req.id;
  {
    RIResourceBufferTransaction_s trans = {};
    trans.target = m_opaqueMaterialBuffer;
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

  // Water materials get a parallel WaterMaterial entry at the same slot so
  // Water.frag.slang's gWaterMaterials[materialID] lookup lands on the
  // wave / Fresnel / reflection-fade scalars authored for this material.
  if (desc.m_id == MaterialID::Water) {
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
    trans.offset = (size_t)req.id * sizeof(WaterMaterial);
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

  return req.id;
}

void cHybridRenderer::Draw(RIBootstrap::FrameContext *cntx, cViewport *viewport,
                           float afFrameTime, cFrustum *apFrustum,
                           cWorld *apWorld, cRenderSettings *apSettings,
                           bool abSendFrameBufferToPostEffects) {

  ml::float4x4 mainFrustumViewInvMat = apFrustum->GetViewMat();
  mainFrustumViewInvMat.Invert();
  const ml::float4x4 mainFrustumViewMat = apFrustum->GetViewMat();
  const ml::float4x4 mainFrustumProjMat = apFrustum->GetProjectionMat();
  ml::float4x4 mainFrustumProjInvMat = mainFrustumProjMat;
  mainFrustumProjInvMat.Invert();
  {
    m_rendererList.BeginAndReset(afFrameTime, apFrustum);
    auto *dynamicContainer =
        apWorld->GetRenderableContainer(eWorldContainerType_Dynamic);
    auto *staticContainer =
        apWorld->GetRenderableContainer(eWorldContainerType_Static);
    dynamicContainer->UpdateBeforeRendering();
    staticContainer->UpdateBeforeRendering();

    auto prepareObjectHandler = [&](iRenderable *pObject) {
      if (!rendering::IsObjectIsVisible(
              pObject, eRenderableFlag_VisibleInNonReflection, {})) {
        return;
      }
      m_rendererList.AddObject(pObject);
    };
    rendering::WalkAndPrepareRenderList(dynamicContainer, apFrustum,
                                        prepareObjectHandler,
                                        eRenderableFlag_VisibleInNonReflection);
    rendering::WalkAndPrepareRenderList(staticContainer, apFrustum,
                                        prepareObjectHandler,
                                        eRenderableFlag_VisibleInNonReflection);
    m_rendererList.End(
        eRenderListCompileFlag_Diffuse | eRenderListCompileFlag_Translucent |
        eRenderListCompileFlag_Decal | eRenderListCompileFlag_Illumination |
        eRenderListCompileFlag_FogArea);

    // Park every BLAS-backed geometry on this frame's context, unfiltered by
    // frustum/visibility culling. The TLAS (m_tlas) is persistent and only
    // rebuilt on frames that gather visible instances (see "TLAS build"
    // below, guarded by `!tlasInstances.empty()`); a stale m_tlas keeps
    // referencing the BLAS device addresses of geometry that was freed on a
    // map transition, and the surfel RT passes trace it every frame -> a
    // dangling acceleration-structure / vertex-buffer dereference -> GPUVM
    // read fault -> device lost. AttachResourceToCntx pushes the BLAS handle,
    // its storage, and the vertex/index buffers onto accelLink/bufferLink,
    // which defer release by frames-in-flight, so any BLAS the TLAS can still
    // reference outlives the in-flight window even after its owning renderable
    // is destroyed. Only geometry with a built BLAS is parked (the rest can't
    // be in the TLAS).
    std::function<void(iRenderableContainerNode *)> retainGeometryBlas =
        [&](iRenderableContainerNode *node) {
          node->UpdateBeforeUse();
          for (auto *child : node->GetChildNodes())
            retainGeometryBlas(child);
          for (auto *pObject : node->GetObjects()) {
            auto *pVB =
                static_cast<VertexBuffer_RI *>(pObject->GetVertexBuffer());
            if (pVB && pVB->accelStructure())
              pVB->AttachResourceToCntx(cntx);
          }
        };
    retainGeometryBlas(dynamicContainer->GetRoot());
    retainGeometryBlas(staticContainer->GetRoot());
  }

  // --------------------------------------------------------------------
  // Per-frame prepare for every translucent renderable (particles + meshes
  // + billboards + beams). UpdateGraphicsForFrame / UpdateGraphicsForViewport
  // are the per-renderable hooks that recompute dynamic geometry — billboard
  // camera-facing rotation, beam endpoint stretch, particle emitter step,
  // etc. — and mark the VB dirty. SubmitToGPU then allocates vk.buffer on
  // first call, re-uploads dirty streams, and rebuilds the BLAS; subsequent
  // calls in the same frame are no-op via the generation check. Running this
  // once here lets the TLAS-instance-build (refractive translucents), the
  // particle raster pass, and the translucent mesh raster pass all consume
  // already-prepared buffers without duplicating the per-renderable update.
  //
  // Must run BEFORE any vkCmdBeginRendering so the resource-uploader's
  // pipeline barriers and BLAS-build cmds don't collide with a dynamic-
  // rendering scope.
  for (iRenderable *pObj :
       m_rendererList.GetRenderableItems(eRenderListType_Translucent)) {
    if (!pObj)
      continue;
    pObj->UpdateGraphicsForFrame(afFrameTime);
    pObj->UpdateGraphicsForViewport(apFrustum, afFrameTime);
    iVertexBuffer *pVB = pObj->GetVertexBuffer();
    if (pVB) {
      auto *vbri = static_cast<VertexBuffer_RI *>(pVB);
      vbri->SubmitToGPU(&RI.blasSubmit.cmds[0], &RI.device, cntx);
      vbri->AttachResourceToCntx(cntx);
    }
  }

  // Same prepare step for decals (eRenderListType_Decal is a separate list
  // from Translucent — cRenderList::AddObject routes IsDecal() materials only
  // into mvDecalObjects). Their UpdateGraphicsForFrame/ForViewport already ran
  // in AddObject, but SubmitToGPU (which allocates vk.buffer and uploads the
  // position/uv/color streams) is renderer-side and must run here, before any
  // vkCmdBeginRendering, so the decal raster pass below finds valid buffers.
  // Without this the decal pass skips every decal on the missing-position
  // guard and nothing draws.
  for (iRenderable *pObj :
       m_rendererList.GetRenderableItems(eRenderListType_Decal)) {
    if (!pObj)
      continue;

    iVertexBuffer *pVB = pObj->GetVertexBuffer();
    if (pVB) {
      auto *vbri = static_cast<VertexBuffer_RI *>(pVB);
      vbri->SubmitToGPU(&RI.blasSubmit.cmds[0], &RI.device, cntx);
      vbri->AttachResourceToCntx(cntx);
    }
  }

  SceneConstants perFrame{};
  std::memcpy(perFrame.viewMat, mainFrustumViewMat.a, sizeof(perFrame.viewMat));
  std::memcpy(perFrame.invViewMat, mainFrustumViewInvMat.a,
              sizeof(perFrame.invViewMat));
  std::memcpy(perFrame.projMat, mainFrustumProjMat.a, sizeof(perFrame.projMat));
  std::memcpy(perFrame.invProjMat, mainFrustumProjInvMat.a,
              sizeof(perFrame.invProjMat));
  // viewProjMat = proj * view (column-major); fill via direct ml composition
  // when needed. Leaving as identity-stub for now — first pass writes only
  // visibility; lighting in the FS reads viewMat/invViewMat which are correct.
  perFrame.viewportSize[0] = (float)RI.swapchain.width;
  perFrame.viewportSize[1] = (float)RI.swapchain.height;
  perFrame.viewTexel[0] =
      RI.swapchain.width ? 1.0f / (float)RI.swapchain.width : 0.0f;
  perFrame.viewTexel[1] =
      RI.swapchain.height ? 1.0f / (float)RI.swapchain.height : 0.0f;
  // Accumulated animation time (iRenderer::mfTimeCount, advanced each frame in
  // iRenderer::Update via cGraphics::Update) — NOT the per-frame delta. The
  // water wave phase is afT * waveSpeed; feeding the delta froze the waves and
  // jittered them with frametime variance (stutter-in-place). Matches the
  // reference's afT = GetTimeCount().
  perFrame.afT = GetTimeCount();
  perFrame.totalFrames = RI.frameIndex;
  perFrame.cameraFov = apFrustum->GetFOV();
  perFrame.fireflyClampThreshold = 10.0f;
  perFrame.zNear = apFrustum->GetNearPlane();
  perFrame.zFar = apFrustum->GetFarPlane();
  // invViewRotationMat = rotation part of the inverse view matrix (camera
  // world-space basis, translation zeroed). Translucent.frag rotates the
  // view-space cube-map reflection vector into world space with it (matching
  // the base game's a_mtxInvViewRotation). Translation lives at .a[12..14]
  // (the camera-basis extraction below reads posW from invV[12,13,14]); zero
  // it so a direction (passed w=1 in the shader) isn't offset by the camera
  // position.
  {
    ml::float4x4 invViewRot = mainFrustumViewInvMat;
    invViewRot.a[12] = 0.0f;
    invViewRot.a[13] = 0.0f;
    invViewRot.a[14] = 0.0f;
    std::memcpy(perFrame.invViewRotationMat, invViewRot.a,
                sizeof(perFrame.invViewRotationMat));
  }
  // Fog params + worldFogColor default to zero — fine for the first pass;
  // populate when the deferred-fog path needs them.

  // Falcor-style pinhole camera basis. mainFrustumViewInvMat memory is laid
  // out so each 4-float "row" corresponds to one column of the logical
  // (column-vector math) view-inverse — that is, the camera's world-space
  // basis vectors (right, up, back, origin). Slang's -matrix-layout-column-
  // major flag flips the interpretation back to column-vector math GPU-side,
  // so the offsets line up with what the shader expects.
  {
    const float *invV = mainFrustumViewInvMat.a;
    const hpl::float3 rightW{invV[0], invV[1], invV[2]};
    const hpl::float3 upW{invV[4], invV[5], invV[6]};
    const hpl::float3 backW{invV[8], invV[9], invV[10]};
    const hpl::float3 posW{invV[12], invV[13], invV[14]};

    const float aspect = apFrustum->GetAspect();
    const float tanHalfFov = std::tan(0.5f * apFrustum->GetFOV());
    constexpr float focalLength = 1.0f;
    const float uScale = focalLength * tanHalfFov * aspect;
    const float vScale = focalLength * tanHalfFov;

    perFrame.posW = posW;
    perFrame.cameraU = {uScale * rightW.x, uScale * rightW.y, uScale * rightW.z};
    perFrame.cameraV = {vScale * upW.x, vScale * upW.y, vScale * upW.z};
    // cameraW points from the camera through the image-plane center =
    // focalLength * forward. The view-inverse stores back (negative forward)
    // in column 2, so negate.
    perFrame.cameraW = {-focalLength * backW.x, -focalLength * backW.y,
                        -focalLength * backW.z};
    perFrame.jitterX = 0.0f; // TAA not yet wired up; computeRayPinhole's
    perFrame.jitterY = 0.0f; // applyJitter=true is a no-op while these are 0.
  }

  auto solids = m_rendererList.GetSolidObjects();
  auto lights = m_rendererList.GetLights();
  RISegmentReq_s indirectReq = {};
  const bool indirectOk =
      m_indirectSegment.request(RI.frameIndex, solids.size(), &indirectReq);
  assert(indirectOk);
  auto *indirectDst = reinterpret_cast<VkDrawIndirectCommand *>(
      static_cast<uint8_t *>(m_indirectDrawBuffer.mappedAddress) +
      (size_t)indirectReq.elementOffset * sizeof(VkDrawIndirectCommand));
  uint32_t writtenDraws = 0;

  // TLAS instance accumulator. Sized for the shadow caster set (every shadow
  // caster contributes at most one TLAS instance).
  std::vector<VkAccelerationStructureInstanceKHR> tlasInstances;
  tlasInstances.reserve(solids.size());

  size_t num_point_lights = 0;
  for (iLight *pLight : lights) {
    if (pLight->GetLightType() != eLightType_Point)
      continue;
    if (num_point_lights >= kPointSlotLightCapacity) {
      Warning("Point-light slot capacity exhausted; dropping remaining lights");
      break;
    }
    PointLight pl{};
    pl.type = LIGHT_TYPE_POINT;
    const cVector3f pos = pLight->GetWorldPosition();
    pl.position[0] = pos.x;
    pl.position[1] = pos.y;
    pl.position[2] = pos.z;
    const cColor c = pLight->GetDiffuseColor();
    // sRGB authored → linear, matching the diffuse-texture sRGB decode and the
    // output sRGB encode so a fully-lit surface round-trips to base-game brightness.
    // (An earlier "over-saturated" result came from reading the color *raw* with
    // no decode at all — not from a proper sRGB decode like this.)
    pl.color[0] = detail::sRGBToLinear(c.r);
    pl.color[1] = detail::sRGBToLinear(c.g);
    pl.color[2] = detail::sRGBToLinear(c.b);
    // Store the light's intensity = authoredRadius² · scale. It multiplies the
    // radiance (color · intensity · inverse-square) — no luminance here, color
    // already carries brightness — and the grid binning derives the bin reach from
    // the radiance floor (maxChannel(color)·intensity / kLightRadianceFloor).
    const float authored = pLight->GetRadius();
    pl.intensity = authored * authored * kPointLightIntensityScale;
    // Precompute the light-grid bin reach here so LightGridBuildPass (one thread
    // per cell, looping all lights) doesn't recompute it per (cell,light). reach²
    // = maxChannel(color)·intensity / floor − sourceRadius²; ≤0 ⇒ too dim to bin.
    {
      const float maxC = std::max(pl.color[0], std::max(pl.color[1], pl.color[2]));
      const float reachSq =
          maxC * pl.intensity / kLightRadianceFloor - kPointLightSourceRadiusSq;
      pl.radius = reachSq > 0.f ? std::sqrt(reachSq) : 0.f;
    }
    pl.goboTextureIndex = resolveCubeTextureSlot(
        cntx, pLight->GetGoboImage(), (uint32_t)RI.frameIndex);
    const cMatrixf &world = pLight->GetWorldMatrix();
    pl.worldToLightX[0] = world.m[0][0];
    pl.worldToLightX[1] = world.m[0][1];
    pl.worldToLightX[2] = world.m[0][2];
    pl.worldToLightY[0] = world.m[1][0];
    pl.worldToLightY[1] = world.m[1][1];
    pl.worldToLightY[2] = world.m[1][2];
    pl.worldToLightZ[0] = world.m[2][0];
    pl.worldToLightZ[1] = world.m[2][1];
    pl.worldToLightZ[2] = world.m[2][2];
    m_pointLightScratch[num_point_lights++] = pl;
  }
  perFrame.pointLightCount = static_cast<uint32_t>(num_point_lights);

  if (num_point_lights > 0) {
    const size_t uploadBytes = num_point_lights * sizeof(PointLight);
    RIResourceBufferTransaction_s trans = {};
    trans.target = m_pointLightBuffer;
    trans.size = uploadBytes;
    trans.offset = 0;
    // After the first frame the buffer was previously read as a storage
    // resource; tell the uploader to barrier from that to TRANSFER_WRITE
    // and back. On the very first frame the buffer is uninitialised, so
    // the src side of the barrier is a no-op against zero contents — safe.
    trans.vk.current_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    trans.vk.current_access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    trans.vk.post_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    trans.vk.post_access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;

    RI_ResourceBeginCopyBuffer(&RI.device, &RI.uploader, &trans);
    std::memcpy(trans.mapped.data, m_pointLightScratch.data(), uploadBytes);
    RI_ResourceEndCopyBuffer(&RI.device, &RI.uploader, &trans);
  }

  // Spot lights. Same upload envelope as point lights — RI.uploader owns the
  // TRANSFER_WRITE ↔ SHADER_READ barrier, so no extra barrier needed here.
  size_t num_spot_lights = 0;
  for (iLight *pLight : lights) {
    if (pLight->GetLightType() != eLightType_Spot)
      continue;
    if (num_spot_lights >= kSpotSlotLightCapacity) {
      Warning("Spot-light slot capacity exhausted; dropping remaining lights");
      break;
    }
    cLightSpot *pSpot = static_cast<cLightSpot *>(pLight);
    SpotLight sl{};
    sl.type = LIGHT_TYPE_SPOT;
    const cVector3f pos = pLight->GetWorldPosition();
    sl.position[0] = pos.x;
    sl.position[1] = pos.y;
    sl.position[2] = pos.z;
    // HPL2 lights shine down -Z in world space. World matrix is column-major
    // with the 3rd column = +Z basis; negate it to get the outward forward.
    const cMatrixf &world = pLight->GetWorldMatrix();
    sl.direction[0] = -world.m[0][2];
    sl.direction[1] = -world.m[1][2];
    sl.direction[2] = -world.m[2][2];
    // Normalize defensively — the spot may carry non-unit scale.
    {
      float len = std::sqrt(sl.direction[0] * sl.direction[0] +
                            sl.direction[1] * sl.direction[1] +
                            sl.direction[2] * sl.direction[2]);
      if (len > 1e-6f) {
        sl.direction[0] /= len;
        sl.direction[1] /= len;
        sl.direction[2] /= len;
      }
    }
    sl.cosOuterAngle = std::cos(pSpot->GetFOV() * 0.5f);
    const cColor c = pLight->GetDiffuseColor();
    // Linear-throughout pipeline: see point-light upload above for rationale.
    // sRGB authored → linear (same as point lights).
    sl.color[0] = detail::sRGBToLinear(c.r);
    sl.color[1] = detail::sRGBToLinear(c.g);
    sl.color[2] = detail::sRGBToLinear(c.b);
    // Store the light's intensity = authoredRadius² · scale (same as point lights):
    // it multiplies the radiance (color · intensity · inverse-square) — no luminance
    // here, color carries brightness — and the grid binning derives the bin reach from
    // the radiance floor (maxChannel(color)·intensity / kLightRadianceFloor).
    const float authored = pLight->GetRadius();
    sl.intensity = authored * authored * kPointLightIntensityScale;
    // Precompute the light-grid bin reach (same as point lights) so the per-cell
    // gather in LightGridBuildPass just reads it. The spot cone ⊂ the radius
    // sphere, so the radial reach is a conservative bound.
    {
      const float maxC = std::max(sl.color[0], std::max(sl.color[1], sl.color[2]));
      const float reachSq =
          maxC * sl.intensity / kLightRadianceFloor - kPointLightSourceRadiusSq;
      sl.radius = reachSq > 0.f ? std::sqrt(reachSq) : 0.f;
    }
    sl.goboTextureIndex = resolveTextureSlot(cntx, pLight->GetGoboImage(),
                                             (uint32_t)RI.frameIndex);
    sl.shadowEnabled = pLight->GetCastShadows() ? 1u : 0u;
    // Light-space ViewProj for projecting gobo UVs (and any future shadow UV)
    // into the cone. Transposed to match the GLSL mat4 column-major upload.
    const ml::float4x4 vpF4 =
        cMath::ToFloatTranspose4x4(pSpot->GetViewProjMatrix());
    std::memcpy(sl.viewProjection, vpF4.a, sizeof(sl.viewProjection));
    m_spotLightScratch[num_spot_lights++] = sl;
  }
  perFrame.spotLightCount = static_cast<uint32_t>(num_spot_lights);

  if (num_spot_lights > 0) {
    const size_t uploadBytes = num_spot_lights * sizeof(SpotLight);
    RIResourceBufferTransaction_s trans = {};
    trans.target = m_spotLightBuffer;
    trans.size = uploadBytes;
    trans.offset = 0;
    trans.vk.current_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    trans.vk.current_access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    trans.vk.post_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    trans.vk.post_access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    RI_ResourceBeginCopyBuffer(&RI.device, &RI.uploader, &trans);
    std::memcpy(trans.mapped.data, m_spotLightScratch.data(), uploadBytes);
    RI_ResourceEndCopyBuffer(&RI.device, &RI.uploader, &trans);
  }

  // Box lights. Add-blend only (eLightBoxBlendFunc_Replace is silently
  // treated as Add for now — visibility_shade.frag has no Replace path).
  size_t num_box_lights = 0;
  for (iLight *pLight : lights) {
    if (pLight->GetLightType() != eLightType_Box)
      continue;
    continue;
    if (num_box_lights >= kBoxSlotLightCapacity) {
      Warning("Box-light slot capacity exhausted; dropping remaining lights");
      break;
    }
    cLightBox *pBox = static_cast<cLightBox *>(pLight);
    BoxLight bl{};
    bl.type = LIGHT_TYPE_BOX;
    bl.blendFunc = (pBox->GetBlendFunc() == eLightBoxBlendFunc_Replace) ? 0u : 1u;
    // Matches RendererDeferred's box-light proxy: AABB at the light's world position, 
    // now correctly supporting entity rotation using a world-to-local rotation matrix.
    const cVector3f center = pLight->GetWorldPosition();
    bl.center[0] = center.x;
    bl.center[1] = center.y;
    bl.center[2] = center.z;
    const cVector3f half = pBox->GetSize() * 0.5f;
    bl.halfSize[0] = half.x;
    bl.halfSize[1] = half.y;
    bl.halfSize[2] = half.z;
    // Legacy `deferred_light_box.frag.fsl` discards alpha entirely, so
    // GetDiffuseColor().a is intentionally not uploaded — artist-authored
    // brightness lives in the rgb channels. Box lights are an additive
    // volume tint (no Lambert BRDF), so they only get sRGB→linear with no
    // π compensation — that's only for the point/spot direct path.
    const cColor c = pLight->GetDiffuseColor();
    bl.color[0] = detail::sRGBToLinear(c.r) * kBoxLightIntensityAdjustment;
    bl.color[1] = detail::sRGBToLinear(c.g) * kBoxLightIntensityAdjustment;
    bl.color[2] = detail::sRGBToLinear(c.b) * kBoxLightIntensityAdjustment;

    const cMatrixf &world = pLight->GetWorldMatrix();
    bl.worldToLightX[0] = world.m[0][0];
    bl.worldToLightX[1] = world.m[0][1];
    bl.worldToLightX[2] = world.m[0][2];
    bl.worldToLightY[0] = world.m[1][0];
    bl.worldToLightY[1] = world.m[1][1];
    bl.worldToLightY[2] = world.m[1][2];
    bl.worldToLightZ[0] = world.m[2][0];
    bl.worldToLightZ[1] = world.m[2][1];
    bl.worldToLightZ[2] = world.m[2][2];

    m_boxLightScratch[num_box_lights++] = bl;
  }
  perFrame.boxLightCount = static_cast<uint32_t>(num_box_lights);

  if (num_box_lights > 0) {
    const size_t uploadBytes = num_box_lights * sizeof(BoxLight);
    RIResourceBufferTransaction_s trans = {};
    trans.target = m_boxLightBuffer;
    trans.size = uploadBytes;
    trans.offset = 0;
    trans.vk.current_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    trans.vk.current_access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    trans.vk.post_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    trans.vk.post_access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    RI_ResourceBeginCopyBuffer(&RI.device, &RI.uploader, &trans);
    std::memcpy(trans.mapped.data, m_boxLightScratch.data(), uploadBytes);
    RI_ResourceEndCopyBuffer(&RI.device, &RI.uploader, &trans);
  }

  // Fog areas — one FogAreaParams per visible cFogArea, capped at
  // kFogAreaCapacity. The shader walks gFogAreas[i] per pixel up to
  // gPerFrame.fogAreaCount; everything past that index is stale data and
  // gets skipped.
  size_t num_fog_areas = 0;
  for (cFogArea *pFogArea : m_rendererList.GetFogAreas()) {
    if (!pFogArea) continue;
    if (num_fog_areas >= kFogAreaCapacity) {
      Warning("Fog-area capacity exhausted; dropping remaining areas");
      break;
    }
    FogAreaParams fa{};
    const cMatrixf inv =
        cMath::MatrixInverse(*pFogArea->GetModelMatrixPtr());
    const ml::float4x4 invF4 = cMath::ToFloatTranspose4x4(inv);
    std::memcpy(fa.invModelMat, invF4.a, sizeof(fa.invModelMat));
    const cColor c = pFogArea->GetColor();
    // Fog colour stays sRGB-authored — same as worldFogColor. The opaque
    // composite blends linearly so sRGB→linear conversion mirrors the
    // box-light path.
    fa.color   = float3{detail::sRGBToLinear(c.r),
                        detail::sRGBToLinear(c.g),
                        detail::sRGBToLinear(c.b)};
    fa.colorA       = c.a;
    fa.start        = pFogArea->GetStart();
    fa.end          = pFogArea->GetEnd();
    fa.falloffExp   = pFogArea->GetFalloffExp();
    fa.flags        = (pFogArea->GetShowBacksideWhenInside()  ? 1u : 0u)
                    | (pFogArea->GetShowBacksideWhenOutside() ? 2u : 0u);
    m_fogAreaScratch[num_fog_areas++] = fa;
  }
  perFrame.fogAreaCount = static_cast<uint32_t>(num_fog_areas);

  if (num_fog_areas > 0) {
    const size_t uploadBytes = num_fog_areas * sizeof(FogAreaParams);
    RIResourceBufferTransaction_s trans = {};
    trans.target = m_fogAreaBuffer;
    trans.size = uploadBytes;
    trans.offset = 0;
    trans.vk.current_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    trans.vk.current_access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    trans.vk.post_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    trans.vk.post_access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    RI_ResourceBeginCopyBuffer(&RI.device, &RI.uploader, &trans);
    std::memcpy(trans.mapped.data, m_fogAreaScratch.data(), uploadBytes);
    RI_ResourceEndCopyBuffer(&RI.device, &RI.uploader, &trans);
  }

  // Clustered OOB decals — one GpuDecal per visible cDecal (no geometry). The
  // projection pass reads gDecals[] and projects each onto the V-buffer surface,
  // filtered by receiverMask. Capped at kMaxDecals.
  size_t num_decals = 0;
  for (iRenderable *pObject :
       m_rendererList.GetRenderableItems(eRenderListType_Decal)) {
    if (!pObject || pObject->GetRenderType() != eRenderableType_Decal)
      continue;
    cDecal *pDecal = static_cast<cDecal *>(pObject);
    cMaterial *pMat = pDecal->GetMaterial();
    if (!pMat)
      continue;
    if (num_decals >= kMaxDecals) {
      Warning("Decal capacity exhausted; dropping remaining decals");
      break;
    }

    uint32_t materialSlot = resolveMaterial(cntx, pMat, (uint32_t)RI.frameIndex);
    if (materialSlot == UINT32_MAX) {
      Warning("Decal material slot exhausted");
      materialSlot = 0;
    }

    GpuDecal d{};
    cMatrixf *pMtx = pDecal->GetModelMatrix(apFrustum);
    const cMatrixf wm = pMtx ? *pMtx : cMatrixf::Identity;
    ml::float4x4 invF4 = cMath::ToFloatTranspose4x4(wm);
    invF4.Invert();
    std::memcpy(d.invModelMat, invF4.a, sizeof(d.invModelMat));

    // Bounding sphere for DecalGridBuildPass: center = box origin; radius = half
    // the box diagonal (rotation-invariant) = 0.5*length(scale), scale = basis
    // column lengths of the world matrix.
    const cVector3f wc = wm.GetTranslation();
    d.center = float3{wc.x, wc.y, wc.z};
    d.radius = 0.5f * sqrtf(wm.GetRight().SqrLength() + wm.GetUp().SqrLength() +
                            wm.GetForward().SqrLength());
    const cColor c = pDecal->GetDecalColor();
    d.color = float4{c.r, c.g, c.b, c.a};
    d.materialID = materialSlot;
    d.receiverMask = (uint32_t)pDecal->GetReceiverMask();
    d.blendMode = (uint32_t)pMat->GetBlendMode();
    const cVector2l sd = pDecal->GetSubDiv();
    d.subDivX = (uint32_t)sd.x;
    d.subDivY = (uint32_t)sd.y;
    m_decalScratch[num_decals++] = d;
  }
  perFrame.decalCount = static_cast<uint32_t>(num_decals);

  if (num_decals > 0) {
    const size_t uploadBytes = num_decals * sizeof(GpuDecal);
    RIResourceBufferTransaction_s trans = {};
    trans.target = m_decalBuffer;
    trans.size = uploadBytes;
    trans.offset = 0;
    trans.vk.current_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    trans.vk.current_access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    trans.vk.post_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    trans.vk.post_access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    RI_ResourceBeginCopyBuffer(&RI.device, &RI.uploader, &trans);
    std::memcpy(trans.mapped.data, m_decalScratch.data(), uploadBytes);
    RI_ResourceEndCopyBuffer(&RI.device, &RI.uploader, &trans);
  }

  for (iRenderable *pObject : solids) {
    cMatrixf *pMtx = pObject->GetModelMatrix(apFrustum);
    iVertexBuffer *pVB = pObject->GetVertexBuffer();
    cMaterial *pMat = pObject->GetMaterial();
    if (!pVB || !pMat)
      continue;

    
    uint32_t materialSlot = 0;
    if (pMat) {
      materialSlot = resolveMaterial(cntx, pMat, (uint32_t)RI.frameIndex);
      if (materialSlot == UINT32_MAX) {
        Warning("Material Slot exhausted");
        materialSlot = 0;
      }
    }

    UniformObject payload{};
    payload.dissolveAmount = pObject->GetCoverageAmount();
    payload.materialID = materialSlot;
    payload.lightLevel = 1.0f;
    payload.illuminationAmount = pObject->GetIlluminationAmount();
    // Decal receiver category (replaces edit-time IsAffectedByDecal). Static
    // geometry covers StaticObject + Primitive (combined at load, not separable
    // at runtime); dynamic renderables are entities.
    payload.decalReceiver = pObject->IsStatic()
        ? (uint32_t)(eDecalReceiver_Static | eDecalReceiver_Primitive)
        : (uint32_t)eDecalReceiver_Entity;
    const ml::float4x4 modelF4 =
        cMath::ToFloatTranspose4x4(pMtx ? *pMtx : cMatrixf::Identity);
    std::memcpy(payload.modelMat, modelF4.a, sizeof(payload.modelMat));
    ml::float4x4 invF4 = modelF4;
    invF4.Invert();
    std::memcpy(payload.invModelMat, invF4.a, sizeof(payload.invModelMat));
    const ml::float4x4 uvF4 = cMath::ToFloatTranspose4x4(cMatrixf::Identity);
    std::memcpy(payload.uvMat, uvF4.a, sizeof(payload.uvMat));

    const hash_t payloadHash =
        hash_data(hash_u64(HASH_INITIAL_VALUE, (uint64_t)(uintptr_t)pObject),
                  &payload, sizeof(payload));
    auto req = m_diffuseBindless.request(payloadHash, (uint32_t)RI.frameIndex);
    if (req.exhausted) {
      // TODO: will probably resize the buffer and goto the beginning and
      // reconstruct the data
      Error("bindless pool is exhausted");
      // Drop this draw rather than writing through a sentinel req.id — the
      // downstream payload / handle writes index the bindless buffer at
      // req.id and feed instanceCustomIndex into the TLAS instance.
      continue;
    }

    auto *vb = static_cast<VertexBuffer_RI *>(pVB);
    vb->SubmitToGPU(&RI.blasSubmit.cmds[0], &RI.device, cntx);
    vb->AttachResourceToCntx(cntx);
    // The first time this bindless slot is (re)assigned to this VB
    // (req.found == false), park a destroy handler in the slot's cache state.
    // On the VB's destruction it bumps the slot's reuse generation, so any
    // surfel still anchored here (cached primitiveIndex against the now-freed
    // geometry) trips collectCellInfo's slotStale guard and recycles before it
    // dereferences the freed vertex/index BDA (GPUVM fault) — destruction is
    // just one more reason the slot's geometry is no longer what the surfel
    // captured, handled by the same generation bump as slot reuse / rebuild
    // below. Single-threaded game, so the mapped write needs no lock. Capturing
    // `this` is safe: the handler lives in m_diffuseBindless (a renderer
    // member), so it can't fire after the renderer is gone. On a cache hit the
    // handler is already bound to this VB; on eviction the cache reset the
    // slot's state for us.
    if (!req.found && req.state) {
      req.state->onDestroy = EventHandler<>([this, slot = req.id]() {
        // Writes the CPU shadow only (may fire off-frame during teardown); the
        // next Draw()'s flushBindlessMirrors() stages it before the surfel
        // passes read it, keeping the stale-anchor invalidation race-free.
        m_bindlessSlotGenerationMirror.write<uint32_t>(slot,
                                                       ++m_nextSlotGeneration);
      });
      req.state->onDestroy.Connect(vb->OnDestroyed());
      // Geometry-rebuild hook: a Compile / SubmitToGPU realloc on this VB marks
      // the slot dirty so the generation bump below invalidates anchored surfels
      // whose cached primitiveIndex no longer fits the rebuilt geometry.
      req.state->onGeometryChanged = EventHandler<>(
          [this, slot = req.id]() { m_slotGeomDirty[slot] = 1u; });
      req.state->onGeometryChanged.Connect(vb->OnGeometryChanged());
    }
    if (!req.found) {
      // This slot now hosts a different object (fresh allocation or
      // eviction-reuse). Bump its generation so any surfel still anchored to it
      // — carrying a cached primitiveIndex from the *previous* occupant's mesh —
      // is detected as stale by collectCellInfo before it dereferences the new
      // geometry's vertex/index BDA (the GPUVM-fault path). A unique monotonic
      // value lets us write the slot without reading back the GPU buffer.
      m_bindlessSlotGenerationMirror.write<uint32_t>(req.id,
                                                     ++m_nextSlotGeneration);
      {
        RIResourceBufferTransaction_s trans = {};
        trans.target = m_diffuseObjectBuffer;
        trans.size = sizeof(UniformObject);
        trans.offset = (size_t)req.id * sizeof(UniformObject);
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
    } else if (m_slotGeomDirty[req.id]) {
      // Cache hit, but this VB's geometry was rebuilt since we last visited the
      // slot — onGeometryChanged fired from Compile, or from the SubmitToGPU
      // realloc just above (synchronously, so an in-loop realloc is caught this
      // same frame). The handle BDAs below now point at the new geometry, but
      // surfels still carry a primitiveIndex from the old layout; bump the slot
      // generation so collectCellInfo treats them as stale before the OOB deref
      // — same mechanism as the miss path above.
      m_bindlessSlotGenerationMirror.write<uint32_t>(req.id,
                                                     ++m_nextSlotGeneration);
    }
    m_slotGeomDirty[req.id] = 0u; // consumed (the miss path already bumped)

    // Per-stream VkDeviceAddress fan-out into the parallel handle buffers.
    // Missing streams write 0 — shaders branch on non-zero before deref.
    //
    // Rewritten EVERY frame, not just on cache miss: the diffuse cache key is
    // pObject + transform (see m_diffuseBindless.request above), which does NOT
    // capture the VB's device address. SubmitToGPU hands back a brand-new
    // VkBuffer (new device address) on first submit, shadow-data growth, or the
    // CreateCopy sentinel (VertexBuffer_RI.cpp). On a cache *hit* after such a
    // realloc, a once-only write would leave this slot pointing at the freed
    // address; gbuffer.vert / the surfel RT vbuffer chit then deref a non-null
    // dangling pointer -> GPUVM read fault -> device lost. The particle path
    // dodges this by folding RI.frameIndex into its key; opaque draws share a
    // slot across frames, so the handle itself must be refreshed here.
    auto bdaOf = [&](eVertexBufferElement type) -> VkDeviceAddress {
      const auto *element = vb->GetElement(type);
      if (!element || !element->buffer)
        return 0;
      return element->buffer->GetDeviceHandle(&RI.device);
    };

    const VkDeviceAddress addrs[] = {
        bdaOf(eVertexBufferElement_Position),
        bdaOf(eVertexBufferElement_Texture1Tangent),
        bdaOf(eVertexBufferElement_Normal),
        bdaOf(eVertexBufferElement_Texture0),
        bdaOf(eVertexBufferElement_Color0),
        vb->GetIndexRIBuffer()
            ? vb->GetIndexRIBuffer()->GetDeviceHandle(&RI.device)
            : 0,
    };
    BindlessShadowMirror *const handleMirrors[] = {
        &m_opaquePositionMirror, &m_opaqueTangentMirror,
        &m_opaqueNormalMirror,   &m_opaqueUv0Mirror,
        &m_opaqueColorMirror,    &m_opaqueIndexMirror,
    };
    for (size_t i = 0; i < std::size(handleMirrors); ++i) {
      handleMirrors[i]->write<VkDeviceAddress>(req.id, addrs[i]);
    }

    vb->AttachResourceToCntx(cntx);

    // firstInstance carries the slot id to the VS via gl_InstanceIndex;
    // the VS pulls vertex / index data via BDA from the bindless set 0 SSBOs,
    // so vertexCount is the index count (one VS invocation per index).
    // Only frustum-visible objects emit an indirect draw — shadow-only casters
    // contribute via the TLAS instance below.
    if (writtenDraws < indirectReq.numElements) {
      indirectDst[writtenDraws++] = VkDrawIndirectCommand{
          /*vertexCount   =*/(uint32_t)pVB->GetIndexNum(),
          /*instanceCount =*/1u,
          /*firstVertex   =*/0u,
          /*firstInstance =*/req.id,
      };
    }

    // BLAS was recorded by SubmitToGPU above into the same primary cmd buffer;
    // the accel-build→accel-build barrier below guarantees the TLAS read sees
    // the BLAS writes.
    auto blas = vb->accelStructure();
    if (blas && blas->vk.handle != VK_NULL_HANDLE) {
      // VkAccelerationStructureInstanceKHR::transform is row-major 3x4
      // (matrix[row][col]), translation at matrix[r][3]. payload.modelMat
      // holds column-major storage (GLSL mat4 reading in gbuffer.vert), so
      // index it as [col*4 + row] to extract entries row-by-row.
      VkAccelerationStructureInstanceKHR inst = {};
      for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
          inst.transform.matrix[r][c] = payload.modelMat[c * 4 + r];
        }
      }
      inst.instanceCustomIndex = req.id;
      inst.mask = kRayMaskOpaque;
      inst.instanceShaderBindingTableRecordOffset = 0;
      inst.flags = RI_ACCEL_INSTANCE_TRIANGLE_CULL_DISABLE;
      assert(blas->vk.deviceAddress != 0);
      inst.accelerationStructureReference = blas->vk.deviceAddress;
      tlasInstances.push_back(inst);
    }
  }

  // ---------- Translucent meshes → TLAS ----------
  // Translucents enter the TLAS when the SurfelVBuffer.rt closeHit needs to
  // bounce through them — that's **refractive** materials (ray-bend into
  // gPackedRefractionHitInfo) AND **reflective** materials (cube-map glass:
  // mirror-bounce into gPackedReflectionHitInfo). The reflective signal is a
  // cube-map texture, exactly as the reference keys EnableCubeMap off
  // GetImage(eMaterialTexture_CubeMap) (MaterialResource.cpp) and as the
  // shader's isReflective() checks cubeMapTextureIndex. Plain
  // non-refractive / non-cube-map translucents (additive overlays, dissolve
  // sprites) are still pruned: adding them put a bindless DiffuseMaterial
  // slot with no albedo + zero solid scalars into the surfel ray cone, which
  // fed NaN into rayResult.radiance -> surfel.radiance (MSME) ->
  // SurfelGenerationPass.gOutput. The kRayMaskTranslucent category mask also
  // keeps the indirect-lighting path tracer from bouncing off whatever does
  // enter here. Particle emitters are skipped (procedural VBs, no stable BLAS).
  //
  // The bindless object slot allocated here is intentionally distinct from
  // the slot the translucent mesh pass will allocate later for this same
  // renderable (different cookie salt) — the two passes write different
  // UniformObject payloads (rasterised draws need lightLevel,
  // illuminationAmount, etc.; the TLAS path only needs materialID +
  // modelMat + BDA handles). The cost is one extra bindless slot per
  // refractive translucent mesh, well within kObjectSlotCapacity.
  for (iRenderable *pObj :
       m_rendererList.GetRenderableItems(eRenderListType_Translucent)) {
    if (!pObj || pObj->GetRenderType() == eRenderableType_ParticleEmitter)
      continue;
    cMaterial *pMat = pObj->GetMaterial();
    if (!pMat)
      continue;
    if (!pMat->HasRefraction())
      continue;
    iVertexBuffer *pVB = pObj->GetVertexBuffer();
    if (!pVB || pVB->GetIndexNum() <= 0)
      continue;

    // VB upload + BLAS build already happened in the consolidated translucent
    // prepare loop near the top of Draw(); just pick up the cached BLAS here.
    auto *vbri = static_cast<VertexBuffer_RI *>(pVB);
    auto blas = vbri->accelStructure();
    if (!blas || blas->vk.handle == VK_NULL_HANDLE)
      continue;

    uint32_t materialSlot =
        resolveMaterial(cntx, pMat, (uint32_t)RI.frameIndex);
    if (materialSlot == UINT32_MAX)
      continue;

    UniformObject payload{};
    payload.materialID = materialSlot;
    payload.lightLevel = 1.0f;
    payload.dissolveAmount = 0.0f;
    payload.illuminationAmount = 0.0f;
    cMatrixf *pMtx = pObj->GetModelMatrix(apFrustum);
    const ml::float4x4 modelF4 =
        cMath::ToFloatTranspose4x4(pMtx ? *pMtx : cMatrixf::Identity);
    std::memcpy(payload.modelMat, modelF4.a, sizeof(payload.modelMat));
    ml::float4x4 invF4 = modelF4;
    invF4.Invert();
    std::memcpy(payload.invModelMat, invF4.a, sizeof(payload.invModelMat));
    const ml::float4x4 uvF4 =
        cMath::ToFloatTranspose4x4(cMatrixf::Identity);
    std::memcpy(payload.uvMat, uvF4.a, sizeof(payload.uvMat));

    // Salted cookie keeps this slot disjoint from the translucent mesh
    // pass's per-frame slot for the same renderable.
    const hash_t cookie = hash_u32(
        hash_u64(HASH_INITIAL_VALUE, (uint64_t)(uintptr_t)pObj),
        0x71A57AA5u);
    auto req = m_diffuseBindless.request(cookie, (uint32_t)RI.frameIndex);
    if (req.exhausted)
      continue;

    if (!req.found) {
      m_bindlessSlotGenerationMirror.write<uint32_t>(req.id,
                                                     ++m_nextSlotGeneration);
    }

    {
      RIResourceBufferTransaction_s trans = {};
      trans.target = m_diffuseObjectBuffer;
      trans.size = sizeof(UniformObject);
      trans.offset = (size_t)req.id * sizeof(UniformObject);
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

    auto bdaOf = [&](eVertexBufferElement type) -> VkDeviceAddress {
      const auto *element = vbri->GetElement(type);
      if (!element || !element->buffer)
        return 0;
      return element->buffer->GetDeviceHandle(&RI.device);
    };
    m_opaquePositionMirror.write<VkDeviceAddress>(
        req.id, bdaOf(eVertexBufferElement_Position));
    m_opaqueUv0Mirror.write<VkDeviceAddress>(
        req.id, bdaOf(eVertexBufferElement_Texture0));
    m_opaqueColorMirror.write<VkDeviceAddress>(
        req.id, bdaOf(eVertexBufferElement_Color0));
    m_opaqueNormalMirror.write<VkDeviceAddress>(
        req.id, bdaOf(eVertexBufferElement_Normal));
    m_opaqueTangentMirror.write<VkDeviceAddress>(
        req.id, bdaOf(eVertexBufferElement_Texture1Tangent));
    m_opaqueIndexMirror.write<VkDeviceAddress>(
        req.id, vbri->GetIndexRIBuffer()
                    ? vbri->GetIndexRIBuffer()->GetDeviceHandle(&RI.device)
                    : 0);

    VkAccelerationStructureInstanceKHR inst = {};
    for (int r = 0; r < 3; ++r) {
      for (int c = 0; c < 4; ++c) {
        inst.transform.matrix[r][c] = payload.modelMat[c * 4 + r];
      }
    }
    inst.instanceCustomIndex = req.id;
    inst.mask = kRayMaskTranslucent;
    inst.instanceShaderBindingTableRecordOffset = 0;
    inst.flags = RI_ACCEL_INSTANCE_TRIANGLE_CULL_DISABLE;
    assert(blas->vk.deviceAddress != 0);
    inst.accelerationStructureReference = blas->vk.deviceAddress;
    tlasInstances.push_back(inst);
  }

  // ---------- TLAS build ----------
  // Walks the BLAS instances accumulated above and emits one TLAS build into
  // the primary cmd buffer. The TLAS isn't bound to any shader yet (phase 4);
  // building it here exercises the path so RenderDoc / validation can verify
  // correctness.
  if (!tlasInstances.empty()) {
    const uint32_t instanceCount = (uint32_t)tlasInstances.size();

    auto destroyBuffer = [](RIBuffer_s *b) {
      if (b->vk.buffer) {
        auto *cntx = RI.GetActiveSet();
        cntx->freelist.push_back(RIFree(b->vk.buffer));
        cntx->freelist.push_back(RIFree(b->vk.allocation));
      }
      delete b;
    };

    // Grow the instance buffer on demand. Old buffer goes onto the active
    // freelist so any in-flight build that referenced it stays valid until
    // frames-in-flight roll.
    if (instanceCount > m_tlasCapacity) {
      uint32_t newCap = m_tlasCapacity ? m_tlasCapacity : 256;
      while (newCap < instanceCount)
        newCap += (newCap >> 1);
      if (m_tlasInstanceBuffer.vk.buffer) {
        cntx->freelist.push_back(RIFree(m_tlasInstanceBuffer.vk.buffer));
        cntx->freelist.push_back(RIFree(m_tlasInstanceBuffer.vk.allocation));
        m_tlasInstanceBuffer = {};
      }
      // Device-local: the instance buffer is a transfer destination written
      // each frame via the resource uploader. A persistent host mapping would
      // race the GPU's TLAS read for the previous frame still in flight.
      uint32_t qf[RI_QUEUE_LEN] = {0};
      VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
      VK_ConfigureBufferQueueFamilies(&bci, RI.device.queues, RI_QUEUE_LEN, qf,
                                      RI_QUEUE_LEN);
      bci.size = (VkDeviceSize)newCap * sizeof(VkAccelerationStructureInstanceKHR);
      bci.usage =
          VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
          VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      VmaAllocationCreateInfo aci = {};
      aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
      VK_WrapResult(vmaCreateBufferWithAlignment(
          RI.device.vk.vmaAllocator, &bci, &aci, 16,
          &m_tlasInstanceBuffer.vk.buffer, &m_tlasInstanceBuffer.vk.allocation,
          nullptr));
      m_tlasCapacity = newCap;
    }

    // Stage the instance array through the resource uploader so frame N+1's
    // write doesn't clobber the buffer mid-build for frame N. The uploader
    // owns the previous-use ↔ TRANSFER_WRITE ↔ next-use barrier pair.
    {
      RIResourceBufferTransaction_s trans = {};
      trans.target = m_tlasInstanceBuffer;
      trans.size = (size_t)instanceCount *
                   sizeof(VkAccelerationStructureInstanceKHR);
      trans.offset = 0;
      trans.vk.current_stage =
          VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
      trans.vk.current_access =
          VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
      trans.vk.post_stage =
          VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
      trans.vk.post_access = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
      RI_ResourceBeginCopyBuffer(&RI.device, &RI.uploader, &trans);
      std::memcpy(trans.mapped.data, tlasInstances.data(), trans.size);
      RI_ResourceEndCopyBuffer(&RI.device, &RI.uploader, &trans);
    }

    // BLAS builds are recorded into RI.blasSubmit (a separate command buffer
    // submitted + semaphore-synced ahead of the primary in CloseAndSubmitActiveSet),
    // so they are guaranteed complete before this primary buffer's TLAS build runs.
    // No inline accel-build→accel-build barrier is needed here.

    // Size the TLAS for the worst-case instance count we've seen. Re-init when
    // the instance count exceeds what the current TLAS storage was sized for.
    RIAccelStructureDesc_s tlasDesc = {};
    tlasDesc.type = RI_ACCEL_STRUCTURE_TYPE_TOP_LEVEL;
    tlasDesc.flags = RI_ACCEL_BUILD_PREFER_FAST_TRACE;
    tlasDesc.geometryOrInstanceNum = instanceCount;

    uint64_t tlasStorageSize = 0;
    uint64_t tlasBuildScratch = 0;
    GetRIAccelStructureMemoryReqs(&RI.device, &tlasDesc, &tlasStorageSize,
                                  &tlasBuildScratch, nullptr);

    if ((m_tlas.vk.handle == VK_NULL_HANDLE) ||
        (m_tlasStorage.vk.buffer == VK_NULL_HANDLE || tlasStorageSize > m_tlasStorageCapacity)) {
      if (m_tlas.vk.handle != VK_NULL_HANDLE) {
        cntx->freelist.push_back(RIFree(m_tlas.vk.handle));
        cntx->freelist.push_back(RIFree(m_tlasStorage.vk.allocation));
        cntx->freelist.push_back(RIFree(m_tlasStorage.vk.buffer));
        m_tlas = {};
      }
      uint32_t qf[RI_QUEUE_LEN] = {0};
      VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
      VK_ConfigureBufferQueueFamilies(&bci, RI.device.queues, RI_QUEUE_LEN, qf,
                                      RI_QUEUE_LEN);
      bci.size = tlasStorageSize;
      bci.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                  VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
      VmaAllocationCreateInfo aci = {};
      aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
      m_tlasStorage = RIBuffer_s::VK_createFromVMA(&RI.device, &bci, &aci);
      tlasDesc.storage = &m_tlasStorage;
      tlasDesc.storageOffset = 0;
      tlasDesc.storageSize = tlasStorageSize;
      if (InitRIAccelStructure(&RI.device, &tlasDesc, &m_tlas) != RI_SUCCESS) {
        // Leave m_tlas zeroed; skip the build this frame.
        m_tlas = {};
      }
      m_tlasStorageCapacity = tlasStorageSize;
    }

    if (m_tlas.vk.handle != VK_NULL_HANDLE) {
      // Source TLAS build scratch from the per-frame accel pool. The pool
      // recycles its blocks across frames and uses the oversized one-shot
      // path for builds that exceed blockSize. RIBlockMem_s embeds an
      // RIBuffer_s, so we hand its address straight to the build desc.
      RIBufferScratchAllocReq_s scratchReq = RIAllocBufferFromScratchAlloc(
          &RI.device, &cntx->accelScratchAlloc, tlasBuildScratch);

      RIBuildTlasDesc_s build = {};
      build.dst = &m_tlas;
      build.src = nullptr;
      build.mode = RI_ACCEL_BUILD_MODE_BUILD;
      build.instanceNum = instanceCount;
      build.instanceBuffer = &m_tlasInstanceBuffer;
      build.instanceOffset = 0;
      build.scratchBuffer = &scratchReq.block.buffer;
      build.scratchOffset = scratchReq.bufferOffset;
      CmdBuildRITlas(&RI.device, &RI.primary.cmds[0], &build, 1);

      VkMemoryBarrier2 tlasToShader = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
      tlasToShader.srcStageMask =
          VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
      tlasToShader.srcAccessMask =
          VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
      // Also visible to the Stage B SurfelGI VBuffer RT pipeline (rgen +
      // chit + ahit + miss) — kept in one barrier with the fragment-shader
      // ray-query consumer in visibility_shade.frag.
      tlasToShader.dstStageMask =
          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
          VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
      tlasToShader.dstAccessMask =
          VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
      VkDependencyInfo dep2 = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      dep2.memoryBarrierCount = 1;
      dep2.pMemoryBarriers = &tlasToShader;
      vkCmdPipelineBarrier2(RI.primary.cmds[0].vk.cmd, &dep2);
    }
  }

  // m_packedHitInfoView / m_surfelIrradianceView / m_surfelDepthView and the
  // freshly built TLAS now live on set 1 and are pushed per-dispatch via
  // RIProgram::bindDescriptors below (see the m_surfelVBuffer / m_surfelRT
  // / m_surfelIntegrate / m_surfelGenerate / m_surfelGIRender call sites).
  // Set 1 is allocated from a frame-rotated pool, so each frame's writes
  // land on an idle descriptor set.

  VkCommandBuffer cmd = RI.primary.cmds[0].vk.cmd;
  std::vector<RIProgram::DescriptorBinding> bindings;
  bindings.reserve(16);
  auto pushBinding = [&](const char *name, const RIDescriptor_s &desc,
                         uint32_t registerOffset = 0) {
    RIProgram::DescriptorBinding b;
    b.handle = DescriptorBindingID::Create(name);
    b.registerOffset = registerOffset;
    b.descriptor = desc;
    bindings.push_back(b);
  };

  // Per-dispatch helpers for the set-1 surfel image / TLAS pushes —
  // `gPackedHitInfo` / `gIrradianceMap` / `gSurfelDepthMap` are
  // RWTexture2D (GENERAL layout, storage image), `gSurfelDepth` is the
  // sampled view of the same depth image (still GENERAL since the
  // image stays GENERAL across the frame and GENERAL satisfies both
  // storage + sampled access). `gRtAccel` is the freshly built TLAS.
  // Each helper appends to a local std::vector<DescriptorBinding> so
  // multiple shaders can mix-and-match the subset they need.
  auto pushSurfelStorageImage =
      [&](std::vector<RIProgram::DescriptorBinding> &v, const char *name,
          VkImageView view) {
        RIProgram::DescriptorBinding b;
        b.handle = DescriptorBindingID::Create(name);
        b.descriptor.vk.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        b.descriptor.vk.image.sampler = VK_NULL_HANDLE;
        b.descriptor.vk.image.imageView = view;
        b.descriptor.vk.image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        RIFinalizeDescriptor(&RI.device, &b.descriptor);
        v.push_back(b);
      };
  auto pushSurfelSampledImage =
      [&](std::vector<RIProgram::DescriptorBinding> &v, const char *name,
          VkImageView view) {
        RIProgram::DescriptorBinding b;
        b.handle = DescriptorBindingID::Create(name);
        b.descriptor.vk.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        b.descriptor.vk.image.sampler = VK_NULL_HANDLE;
        b.descriptor.vk.image.imageView = view;
        b.descriptor.vk.image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        RIFinalizeDescriptor(&RI.device, &b.descriptor);
        v.push_back(b);
      };
  auto pushTlas = [&](std::vector<RIProgram::DescriptorBinding> &v) {
    RIProgram::DescriptorBinding b;
    b.handle = DescriptorBindingID::Create("gRtAccel");
    b.descriptor.vk.type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    b.descriptor.vk.accelStructure = m_tlas.vk.handle;
    RIFinalizeDescriptor(&RI.device, &b.descriptor);
    v.push_back(b);
  };

  // sceneObjectsBuf / opaqueMaterialBuf now live in the bindless set (set=0
  // bindings 20..21), wired up by bindBindlessDescriptorSet() below — no
  // per-draw pushBinding needed.

  {
    RIProgram::DescriptorBinding b;
    b.handle = DescriptorBindingID::Create("gPerFrame");
    RI.UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
    bindings.push_back(b);
  }

  // Scene's rendering was already ended above (before the BLAS/TLAS work).
  // Transition the MRT target (single packed-TriangleHit attachment) and
  // the depth image into their gbuffer-pass layouts. Both use the
  // UNDEFINED-discard pattern: loadOp=CLEAR on both attachments means we
  // never need prior contents preserved, so it doesn't matter what layout
  // the previous frame's last consumer left them in (depth is left in
  // DEPTH_READ_ONLY_OPTIMAL by the translucent/decal flipDepthToReadOnly
  // path below, which the gbuffer's expected DEPTH_ATTACHMENT_OPTIMAL
  // wouldn't otherwise match).
  {
    VkImageMemoryBarrier2 attachmentBarriers[2] = {
        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2},
        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2}};

    VkImageMemoryBarrier2 &toColor = attachmentBarriers[0];
    toColor.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    toColor.srcAccessMask = 0;
    toColor.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    toColor.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    toColor.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toColor.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toColor.image = RI.visibilityTexture[RI.swapchainIndex].vk.image;

    VkImageMemoryBarrier2 &toDepth = attachmentBarriers[1];
    toDepth.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    toDepth.srcAccessMask = 0;
    toDepth.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                           VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    toDepth.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    toDepth.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toDepth.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    toDepth.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    toDepth.image = RI.depthTextures[RI.swapchainIndex].vk.image;

    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 2;
    dep.pImageMemoryBarriers = attachmentBarriers;
    vkCmdPipelineBarrier2(cmd, &dep);
  }

  VkRenderingAttachmentInfo colorAttachment = {
      VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  colorAttachment.imageView = RI.visibilityView[RI.swapchainIndex].vk.image;
  colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  // Cleared to all-zero. psMain writes .w=0 for hit; the depth test at the
  // composite gates miss pixels independently, so no miss sentinel needed.
  colorAttachment.clearValue.color = {{0u, 0u, 0u, 0u}};

  VkRenderingAttachmentInfo depthAttachment = {
      VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  // MRT now owns the per-frame depth clear (Scene no longer pre-clears).
  RI_VK_FillDepthAttachment(&depthAttachment, &RI.depthView[RI.swapchainIndex],
                            /*attachAndClear=*/true);

  // SurfelGI compute passes are temporarily skipped during the
  // /home/m_pol/.claude/plans/can-we-discard-most-crispy-hennessy.md port.
  // The frame still produces a working image because
  // m_surfelResultTexture is cleared to zero below before the composite
  // samples it — visibility_shade reads vec3(0) for indirect and falls
  // back to direct + specular lighting only. Stages B–F will reintroduce
  // the prepare / VBuffer / update / raytrace / integrate / generation
  // dispatches around this scaffold.

  // ----------------------------------------------------------------------
  // Stage D — surfel prepare + cell/ref-counter clear.
  //
  // Runs first each frame: promote the previous frame's ValidSurfel count
  // to DirtySurfel, zero the per-frame counters, and clear the cellInfo /
  // surfelReservation / surfelRefCounter buffers so the update pass can
  // accumulate from scratch.
  //
  // Stage E (raytrace) and Stage F (integrate / generation) aren't here
  // yet, so the surfel pool stays empty across frames — these dispatches
  // currently no-op (DirtySurfel = 0) but they must run to keep the
  // counter state consistent.
  // ----------------------------------------------------------------------
  {
    VkComputePipelineCreateInfo computeCreate = {
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    m_surfelPrepare.bindComputePipeline(&RI.device, &RI.primary.cmds[0],
                                        kHash, "SurfelPreparePass.cs",
                                        &computeCreate);
    m_surfelPrepare.bindBindlessDescriptorSet(
        &RI.primary.cmds[0], &m_bindlessSet, 0,
        VK_PIPELINE_BIND_POINT_COMPUTE);
    CmdDispatch(&RI.primary.cmds[0], 1u, 1u, 1u);
  }

  // Ping-pong the surfel-index buffers: copy this frame's previously-valid
  // indices into the dirty buffer so surfel_update_collect.comp can walk
  // them. SurfelGI's reference declares gSurfelValidIndexBuffer (RW) and
  // gSurfelDirtyIndexBuffer (R/O) as separate Slang bindings; Falcor's host
  // either aliases or ping-pongs them across frames. We do a straight copy
  // each frame — simplest, and the cost is negligible (≤600 KB).
  //
  // Source data was last written by the previous frame's update_collect /
  // generation_pass / rchit::finalize. The engine frame fence already
  // ordered that work against this frame's command buffer, so we don't
  // need a transition barrier before the copy. The post-copy barrier
  // (combined with the prepare-pass writes above) covers both the copy's
  // TRANSFER_WRITE and prepare's SHADER_WRITE against update_collect's
  // SHADER_READ.
  {
    VkBufferCopy region = {};
    region.srcOffset = 0;
    region.dstOffset = 0;
    region.size = (VkDeviceSize)kTotalSurfelLimit * sizeof(uint32_t);
    vkCmdCopyBuffer(cmd,
                    m_surfelValidBuffer.vk.buffer,
                    m_surfelDirtyIndexBuffer.vk.buffer,
                    1, &region);
  }
  {
    VkMemoryBarrier2 mem = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mem.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                       VK_PIPELINE_STAGE_2_COPY_BIT;
    mem.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                        VK_ACCESS_2_TRANSFER_WRITE_BIT;
    mem.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mem.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                        VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mem;
    vkCmdPipelineBarrier2(cmd, &dep);
  }
  // Cell + ref-counter clears are now done by SurfelPreparePass +
  // SurfelUpdatePass on the Slang side. No standalone clear dispatch.
  {
    VkMemoryBarrier2 mem = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mem.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mem.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    mem.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                       VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
    mem.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                        VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mem;
    vkCmdPipelineBarrier2(cmd, &dep);
  }

  // ----------------------------------------------------------------------
  // Stage B — primary-ray VBuffer.
  //
  // RT pipeline that traces one ray per swapchain pixel through m_tlas and
  // packs the closest-hit (instanceCustomIndex, primitiveID, attribs) into
  // m_packedHitInfoTexture. Stages D/F consume this image; for the current
  // frame the output goes unused so the dispatch's correctness needs to be
  // verified through validation-layer signals (no SBT/descriptor errors,
  // no VK_ERROR_DEVICE_LOST).
  // ----------------------------------------------------------------------
  {
    // All three V-buffer images transition UNDEFINED -> GENERAL. The primary
    // image will be written by the RT pipeline directly; the two bounce
    // images are first cleared to uint4(0) so refractive/reflective-only
    // pixels overwrite a known sentinel and everywhere else stays "invalid"
    // (.w == 0). The clear is a transfer-stage write; the RT closeHit needs
    // a sync after it before it can store into the same image.
    VkImageMemoryBarrier2 toGeneral[3] = {};
    VkImage images[3] = {
        m_packedHitInfoTexture[RI.swapchainIndex].vk.image,
        m_packedRefractionHitInfoTexture[RI.swapchainIndex].vk.image,
        m_packedReflectionHitInfoTexture[RI.swapchainIndex].vk.image,
    };
    for (int idx = 0; idx < 3; ++idx) {
      toGeneral[idx].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
      toGeneral[idx].srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
      toGeneral[idx].srcAccessMask = 0;
      // Primary goes straight to the RT pipeline; bounce images first see a
      // transfer-stage clear.
      toGeneral[idx].dstStageMask =
          (idx == 0) ? VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR
                     : VK_PIPELINE_STAGE_2_CLEAR_BIT;
      toGeneral[idx].dstAccessMask =
          (idx == 0) ? VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                     : VK_ACCESS_2_TRANSFER_WRITE_BIT;
      toGeneral[idx].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      toGeneral[idx].newLayout = VK_IMAGE_LAYOUT_GENERAL;
      toGeneral[idx].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      toGeneral[idx].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      toGeneral[idx].image = images[idx];
      toGeneral[idx].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 3;
    dep.pImageMemoryBarriers = toGeneral;
    vkCmdPipelineBarrier2(cmd, &dep);

    // Clear the two bounce V-buffers to uint4(0). Per-frame so the closeHit-
    // miss path can stay silent and consumers detect "no bounce here" via
    // the valid bit in .w (== 0 after the clear).
    VkClearColorValue clearColor = {};  // value-init -> {{0,0,0,0}}
    VkImageSubresourceRange clearRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd,
                         m_packedRefractionHitInfoTexture[RI.swapchainIndex].vk.image,
                         VK_IMAGE_LAYOUT_GENERAL, &clearColor, 1, &clearRange);
    vkCmdClearColorImage(cmd,
                         m_packedReflectionHitInfoTexture[RI.swapchainIndex].vk.image,
                         VK_IMAGE_LAYOUT_GENERAL, &clearColor, 1, &clearRange);

    // Sync clear-write -> RT-shader-write on the two bounce images.
    VkImageMemoryBarrier2 clearDone[2] = {};
    VkImage boundceImages[2] = {
        m_packedRefractionHitInfoTexture[RI.swapchainIndex].vk.image,
        m_packedReflectionHitInfoTexture[RI.swapchainIndex].vk.image,
    };
    for (int idx = 0; idx < 2; ++idx) {
      clearDone[idx].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
      clearDone[idx].srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
      clearDone[idx].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
      clearDone[idx].dstStageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
      clearDone[idx].dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
      clearDone[idx].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
      clearDone[idx].newLayout = VK_IMAGE_LAYOUT_GENERAL;
      clearDone[idx].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      clearDone[idx].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      clearDone[idx].image = boundceImages[idx];
      clearDone[idx].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    VkDependencyInfo dep2 = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep2.imageMemoryBarrierCount = 2;
    dep2.pImageMemoryBarriers = clearDone;
    vkCmdPipelineBarrier2(cmd, &dep2);
  }

  if (m_tlas.vk.handle != VK_NULL_HANDLE) {
    VkRayTracingPipelineCreateInfoKHR rtCreate = {
        VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
    // closesthit in SurfelVBuffer.rt.slang fires one mirror-reflection
    // TraceRay before recording the second hit (Falcor's one-bounce
    // V-buffer trick), so the pipeline needs depth=2: raygen→chit is
    // depth 1, the recursive TraceRay from inside chit lands at depth 2.
    // depth=1 silently leaves hit pixels unwritten and produced "holes"
    // in gPackedHitInfo against the UNDEFINED initial image contents.
    rtCreate.maxPipelineRayRecursionDepth = 2;
    const hash_t kVBufferHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    m_surfelVBuffer.bindRayTracingPipeline(&RI.device, &RI.primary.cmds[0],
                                           kVBufferHash, "SurfelVBuffer.rt",
                                           &rtCreate);
    m_surfelVBuffer.bindBindlessDescriptorSet(
        &RI.primary.cmds[0], &m_bindlessSet, 0,
        VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR);

    std::vector<RIProgram::DescriptorBinding> vbBindings;
    vbBindings.reserve(3);
    {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("gPerFrame");
      RI.UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
      vbBindings.push_back(b);
    }
    pushSurfelStorageImage(vbBindings, "gPackedHitInfo",
                           m_packedHitInfoView[RI.swapchainIndex].vk.image);
    pushSurfelStorageImage(vbBindings, "gPackedRefractionHitInfo",
                           m_packedRefractionHitInfoView[RI.swapchainIndex].vk.image);
    pushSurfelStorageImage(vbBindings, "gPackedReflectionHitInfo",
                           m_packedReflectionHitInfoView[RI.swapchainIndex].vk.image);
    pushTlas(vbBindings);

    m_surfelVBuffer.bindDescriptors(
        &RI.device, &RI.primary.cmds[0], RI.frameIndex, vbBindings.data(),
        vbBindings.size(), VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR);

    m_surfelVBuffer.traceRays(&RI.primary.cmds[0], kVBufferHash,
                              RI.swapchain.width, RI.swapchain.height, 1u);
  }

  {
    // m_packedHitInfoTexture is written by the rgen + miss + chit triplet
    // above. Transition it to SHADER_READ_ONLY_OPTIMAL so consumers can
    // Downstream consumers (SurfelEvaluation / SurfelGeneration /
    // SurfelGIRender) read gPackedHitInfo via direct `[pixel]` storage
    // loads on the bindless RWTexture2D — layout must stay GENERAL
    // through the frame, so this is just a memory/execution sync, not
    // a layout transition. The bindless descriptor was written with
    // VK_IMAGE_LAYOUT_GENERAL at frame start.
    VkMemoryBarrier2 vbufferToConsumers = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    vbufferToConsumers.srcStageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
    vbufferToConsumers.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    vbufferToConsumers.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                       VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
    vbufferToConsumers.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &vbufferToConsumers;
    vkCmdPipelineBarrier2(cmd, &dep);
  }

  // ----------------------------------------------------------------------
  // (Surfels anchored to destroyed geometry are handled before the opaque
  // draw loop above: the retired slots' stream handles are zeroed, so
  // collectCellInfo below recycles those surfels via its free-list branch
  // instead of dereferencing a freed vertex buffer-device-address. Map-load
  // wholesale resets are handled separately by resetSurfelState().)
  // ----------------------------------------------------------------------

  // ----------------------------------------------------------------------
  // Stage D — surfel update (collect → accumulate → scatter).
  //
  // collect: per dirty surfel, refresh pos/normal from gSurfelGeometryBuffer,
  //          allocate ray budget by MSME variance, bump cellInfo counts
  // accumulate: prefix-sum the per-cell counts into cellToSurfelBufferOffset
  // scatter: per valid surfel, write its index into cellToSurfel[] for each
  //          of the 125 neighbour cells it intersects.
  //
  // The geometry buffer (cached uint4 hits) is filled by the Stage F
  // generation pass; until that lands DirtySurfel stays 0 and these
  // dispatches are no-ops.
  // ----------------------------------------------------------------------
  {
    VkComputePipelineCreateInfo computeCreate = {
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    m_surfelUpdateCollect.bindComputePipeline(
        &RI.device, &RI.primary.cmds[0], kHash, "SurfelUpdatePass.cs:collectCellInfo",
        &computeCreate);
    m_surfelUpdateCollect.bindBindlessDescriptorSet(
        &RI.primary.cmds[0], &m_bindlessSet, 0,
        VK_PIPELINE_BIND_POINT_COMPUTE);

    std::vector<RIProgram::DescriptorBinding> bnd;
    bnd.reserve(1);
    {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("gPerFrame");
      RI.UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
      bnd.push_back(b);
    }
    m_surfelUpdateCollect.bindDescriptors(
        &RI.device, &RI.primary.cmds[0], RI.frameIndex, bnd.data(),
        bnd.size(), VK_PIPELINE_BIND_POINT_COMPUTE);

    CmdDispatch(&RI.primary.cmds[0], (kTotalSurfelLimit + 31u) / 32u, 1u, 1u);
  }
  // RAW: accumulate reads cellInfo.surfelCount (collect-written) and reads
  // surfelCounter (collect-incremented). Accumulate's own writes are synced by
  // the next barrier — dstAccess=READ is sufficient and avoids an L2 flush of
  // every other SSBO collect touched (valid/free/recycle/rayResult/refCounter).
  {
    VkMemoryBarrier2 mem = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mem.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mem.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    mem.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mem.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mem;
    vkCmdPipelineBarrier2(cmd, &dep);
  }
  {
    VkComputePipelineCreateInfo computeCreate = {
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    m_surfelUpdateAccumulate.bindComputePipeline(
        &RI.device, &RI.primary.cmds[0], kHash, "SurfelUpdatePass.cs:accumulateCellInfo",
        &computeCreate);
    m_surfelUpdateAccumulate.bindBindlessDescriptorSet(
        &RI.primary.cmds[0], &m_bindlessSet, 0,
        VK_PIPELINE_BIND_POINT_COMPUTE);
    const uint32_t groups = (kCellDimension + 3u) / 4u;
    CmdDispatch(&RI.primary.cmds[0], groups, groups, groups);
  }
  // RAW: scatter reads cellInfo.cellToSurfelBufferOffset (accumulate-written)
  // and RMWs cellInfo.surfelCount (accumulate zeroed it). Scatter's writes are
  // synced by the next barrier. Atomic RMWs only need the prior write visible
  // for the read half — dstAccess=READ suffices.
  {
    VkMemoryBarrier2 mem = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mem.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mem.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    mem.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mem.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mem;
    vkCmdPipelineBarrier2(cmd, &dep);
  }
  {
    VkComputePipelineCreateInfo computeCreate = {
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    m_surfelUpdateScatter.bindComputePipeline(
        &RI.device, &RI.primary.cmds[0], kHash, "SurfelUpdatePass.cs:updateCellToSurfelBuffer",
        &computeCreate);
    m_surfelUpdateScatter.bindBindlessDescriptorSet(
        &RI.primary.cmds[0], &m_bindlessSet, 0,
        VK_PIPELINE_BIND_POINT_COMPUTE);

    std::vector<RIProgram::DescriptorBinding> bnd;
    bnd.reserve(1);
    {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("gPerFrame");
      RI.UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
      bnd.push_back(b);
    }
    m_surfelUpdateScatter.bindDescriptors(
        &RI.device, &RI.primary.cmds[0], RI.frameIndex, bnd.data(),
        bnd.size(), VK_PIPELINE_BIND_POINT_COMPUTE);

    CmdDispatch(&RI.primary.cmds[0], (kTotalSurfelLimit + 31u) / 32u, 1u, 1u);
  }
  {
    VkMemoryBarrier2 mem = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mem.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mem.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    mem.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                       VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    mem.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mem;
    vkCmdPipelineBarrier2(cmd, &dep);
  }

  // ----------------------------------------------------------------------
  // World-space light grid build (feeds the surfel ray-trace NEE importance
  // sampling + the SurfelGIRenderPass direct cull). Per-cell gather: one thread
  // per grid cell walks the light list and writes that cell's count + list. The
  // light SSBOs were uploaded + barriered to SHADER_READ earlier this frame, so
  // binLights reads them directly. No per-cell count clear is needed — every
  // cell's count is written unconditionally by its thread. Placed here (after
  // the cell scatter, before Stage E) for ordering only — independent of the
  // surfel cell grid.
  // ----------------------------------------------------------------------
  {
    VkComputePipelineCreateInfo computeCreate = {
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    m_lightGridBin.bindComputePipeline(&RI.device, &RI.primary.cmds[0], kHash,
                                       "LightGridBuildPass.cs:binLights",
                                       &computeCreate);
    m_lightGridBin.bindBindlessDescriptorSet(&RI.primary.cmds[0], &m_bindlessSet,
                                             0, VK_PIPELINE_BIND_POINT_COMPUTE);
    std::vector<RIProgram::DescriptorBinding> bnd;
    bnd.reserve(1);
    {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("gPerFrame");
      RI.UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
      bnd.push_back(b);
    }
    m_lightGridBin.bindDescriptors(&RI.device, &RI.primary.cmds[0], RI.frameIndex,
                                   bnd.data(), bnd.size(),
                                   VK_PIPELINE_BIND_POINT_COMPUTE);
    // One thread per grid cell (the shader early-outs past kLightGridCellCount).
    CmdDispatch(&RI.primary.cmds[0],
                (kLightGridCellCount + 63u) / 64u, 1u, 1u);
  }

  // ----------------------------------------------------------------------
  // Decal grid build (feeds the albedo-resolve decal lookup). Same shape as the
  // light grid: zero the per-cell counts, bin decals (gDecals[] was uploaded
  // earlier this frame), then make the result visible to the fragment stage.
  // ----------------------------------------------------------------------
  {
    vkCmdFillBuffer(cmd, m_decalGridCountBuffer.vk.buffer, 0, VK_WHOLE_SIZE, 0u);
    VkMemoryBarrier2 mem = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mem.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
    mem.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    mem.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mem.dstAccessMask =
        VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mem;
    vkCmdPipelineBarrier2(cmd, &dep);
  }
  {
    VkComputePipelineCreateInfo computeCreate = {
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    m_decalGridBin.bindComputePipeline(&RI.device, &RI.primary.cmds[0], kHash,
                                       "DecalGridBuildPass.cs:binDecals",
                                       &computeCreate);
    m_decalGridBin.bindBindlessDescriptorSet(&RI.primary.cmds[0], &m_bindlessSet,
                                             0, VK_PIPELINE_BIND_POINT_COMPUTE);
    std::vector<RIProgram::DescriptorBinding> bnd;
    bnd.reserve(1);
    {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("gPerFrame");
      RI.UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
      bnd.push_back(b);
    }
    m_decalGridBin.bindDescriptors(&RI.device, &RI.primary.cmds[0], RI.frameIndex,
                                   bnd.data(), bnd.size(),
                                   VK_PIPELINE_BIND_POINT_COMPUTE);
    // One thread per decal over capacity (shader early-outs past decalCount).
    CmdDispatch(&RI.primary.cmds[0], (kMaxDecals + 63u) / 64u, 1u, 1u);
  }
  {
    // binDecals writes -> albedo-resolve reads (fragment) later this frame.
    VkMemoryBarrier2 mem = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mem.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mem.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    mem.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    mem.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mem;
    vkCmdPipelineBarrier2(cmd, &dep);
  }
  {
    // binLights writes are read by two consumers later this frame: the surfel
    // ray-trace NEE (ray tracing) and the SurfelGIRenderPass direct-lighting
    // cull (fragment — SurfelShade.evalAnalyticLight walks the per-cell light
    // list). Both stages must be in dst or the fragment reads see an empty grid
    // and drop every point/spot light. Compute kept in dst for safety.
    VkMemoryBarrier2 mem = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mem.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mem.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    mem.dstStageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    mem.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mem;
    vkCmdPipelineBarrier2(cmd, &dep);
  }

  // One-shot UNDEFINED -> GENERAL transition for both surfel atlases the
  // first time each swapchain image appears. Subsequent frames skip this —
  // the atlas stays in GENERAL for life so integrate's EMA-blend reads keep
  // working and cross-frame sync goes through the engine frame fence.
  // Must run before Stage E because surfel_rt.rgen's ray-guiding branch
  // imageLoads gSurfelIrradianceMap; the dst stage mask covers both the
  // rgen read and the later integrate read/write.
  if (!m_surfelAtlasesInitialized[RI.swapchainIndex]) {
    // First touch of this swapchain image's atlases: UNDEFINED -> GENERAL and
    // *seed their contents*. SurfelIntegratePass accumulates into both via an
    // EMA read-modify-write (gIrradianceMap / gSurfelDepthMap), so the starting
    // value must be defined — UNDEFINED discards it, leaving uninitialized /
    // NaN reads that poison the EMA and the generation pass's Chebyshev weight.
    // Irradiance (ray-guiding) seeds to 0. The depth atlas stores (E[z], E[z^2])
    // per octahedral texel; seed E[z^2] high so the generation pass's weight
    //   variance / (variance + (dist - mean)^2),  variance = E[z^2] - E[z]^2
    // starts ~1 (no visibility suppression) and tightens only as real
    // first-bounce depths converge. Clearing to 0 instead would make variance=0
    // => weight 0 => every surfel contribution zeroed until the atlas fills in
    // (~hundreds of frames), i.e. a multi-second indirect black-out on enable.
    VkImageMemoryBarrier2 toClear[2] = {
        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2},
        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2}};
    for (uint32_t i = 0; i < 2; ++i) {
      toClear[i].srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
      toClear[i].srcAccessMask = 0;
      toClear[i].dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
      toClear[i].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
      toClear[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      toClear[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
      toClear[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      toClear[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      toClear[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    toClear[0].image = m_surfelIrradianceTexture[RI.swapchainIndex].vk.image;
    toClear[1].image = m_surfelDepthTexture[RI.swapchainIndex].vk.image;
    {
      VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      dep.imageMemoryBarrierCount = 2;
      dep.pImageMemoryBarriers = toClear;
      vkCmdPipelineBarrier2(RI.primary.cmds[0].vk.cmd, &dep);
    }

    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkClearColorValue irrClear = {}; // 0
    vkCmdClearColorImage(RI.primary.cmds[0].vk.cmd,
                         m_surfelIrradianceTexture[RI.swapchainIndex].vk.image,
                         VK_IMAGE_LAYOUT_GENERAL, &irrClear, 1, &range);
    VkClearColorValue depthClear = {};
    depthClear.float32[0] = 0.0f;     // E[z]
    depthClear.float32[1] = 60000.0f; // E[z^2] seeded high (half-float safe)
    vkCmdClearColorImage(RI.primary.cmds[0].vk.cmd,
                         m_surfelDepthTexture[RI.swapchainIndex].vk.image,
                         VK_IMAGE_LAYOUT_GENERAL, &depthClear, 1, &range);

    // Clear (transfer) -> integrate's storage RW + ray-trace / generation reads
    // (sampled). Same GENERAL layout, availability/visibility only.
    VkImageMemoryBarrier2 toShader[2] = {
        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2},
        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2}};
    for (uint32_t i = 0; i < 2; ++i) {
      toShader[i].srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
      toShader[i].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
      toShader[i].dstStageMask =
          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
          VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
      toShader[i].dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
                                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
      toShader[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
      toShader[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
      toShader[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      toShader[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      toShader[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    toShader[0].image = m_surfelIrradianceTexture[RI.swapchainIndex].vk.image;
    toShader[1].image = m_surfelDepthTexture[RI.swapchainIndex].vk.image;
    {
      VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      dep.imageMemoryBarrierCount = 2;
      dep.pImageMemoryBarriers = toShader;
      vkCmdPipelineBarrier2(RI.primary.cmds[0].vk.cmd, &dep);
    }
    m_surfelAtlasesInitialized[RI.swapchainIndex] = true;
  }

  // ----------------------------------------------------------------------
  // Stage E — surfel ray-trace.
  //
  // One TraceRay per pending ray slot. The rgen pulls RequestedRay from
  // the counter, derives a tangent-space hemisphere direction per surfel,
  // and iteratively bounces (closest-hit updates the payload, miss / max
  // step / RR / surfel-cache finalize all terminate the path). NEE inside
  // the closest-hit uses an inline ray query so no recursion is needed
  // (maxPipelineRayRecursionDepth = 1).
  //
  // The reference's expected per-surfel ray count is bounded by
  // gMaxRayCount = 64; the dispatched width is kRayBudget = 9.6M, the rgen
  // early-outs past RequestedRay so unused slots cost nothing.
  // ----------------------------------------------------------------------
  if (m_tlas.vk.handle != VK_NULL_HANDLE) {
    VkRayTracingPipelineCreateInfoKHR rtCreate = {
        VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
    rtCreate.maxPipelineRayRecursionDepth = 1;
    const hash_t kRtHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    m_surfelRT.bindRayTracingPipeline(&RI.device, &RI.primary.cmds[0],
                                      kRtHash, "SurfelRayTrace.rt",
                                      &rtCreate);
    m_surfelRT.bindBindlessDescriptorSet(
        &RI.primary.cmds[0], &m_bindlessSet, 0,
        VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR);

    std::vector<RIProgram::DescriptorBinding> rtBnd;
    rtBnd.reserve(3);
    {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("gPerFrame");
      RI.UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
      rtBnd.push_back(b);
    }
    pushTlas(rtBnd);
    // Ray-guiding read of the surfel irradiance map. The atlas was
    // transitioned to GENERAL once at first appearance (see
    // m_surfelAtlasesInitialized above) and stays GENERAL across the
    // frame so the integrate pass's prior-frame store is read-visible
    // through the engine frame fence.
    pushSurfelStorageImage(rtBnd, "gIrradianceMap",
                           m_surfelIrradianceView[RI.swapchainIndex].vk.image);
    m_surfelRT.bindDescriptors(
        &RI.device, &RI.primary.cmds[0], RI.frameIndex, rtBnd.data(),
        rtBnd.size(), VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR);

    m_surfelRT.traceRays(&RI.primary.cmds[0], kRtHash, kRayBudget, 1u, 1u);
  }
  {
    VkMemoryBarrier2 mem = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mem.srcStageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
    mem.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    mem.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    mem.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mem;
    vkCmdPipelineBarrier2(cmd, &dep);
  }

  VkRenderingInfo renderingInfo = {VK_STRUCTURE_TYPE_RENDERING_INFO};
  renderingInfo.renderArea = {{0, 0},
                              {RI.swapchain.width, RI.swapchain.height}};
  renderingInfo.layerCount = 1;
  renderingInfo.colorAttachmentCount = 1;
  renderingInfo.pColorAttachments = &colorAttachment;
  renderingInfo.pDepthAttachment = &depthAttachment;

  vkCmdBeginRendering(cmd, &renderingInfo);

  VkViewport vkViewport = {0,
                           (float)RI.swapchain.height,
                           (float)RI.swapchain.width,
                           -(float)RI.swapchain.height,
                           0.0f,
                           1.0f};
  VkRect2D scissor = {{0, 0}, {RI.swapchain.width, RI.swapchain.height}};
  vkCmdSetViewport(cmd, 0, 1, &vkViewport);
  vkCmdSetScissor(cmd, 0, 1, &scissor);

  if (writtenDraws > 0) {
    GBufferMRTPipelineDesc pipelineDesc(RIBootstrap::VisibilityFormat,
                                        RIBootstrap::DepthFormat);
    m_gbuffer.bindPipeline(&RI.device, &RI.primary.cmds[0], pipelineDesc.hash,
                           "SurfelGBuffer.3d", &pipelineDesc.createInfo);
    m_gbuffer.bindBindlessDescriptorSet(&RI.primary.cmds[0], &m_bindlessSet, 0);
    m_gbuffer.bindDescriptors(&RI.device, &RI.primary.cmds[0], RI.frameIndex,
                              bindings.data(), bindings.size());
    CmdDrawIndirect(&RI.primary.cmds[0], &m_indirectDrawBuffer,
                    (VkDeviceSize)indirectReq.elementOffset *
                        sizeof(VkDrawIndirectCommand),
                    writtenDraws, (uint32_t)sizeof(VkDrawIndirectCommand));
  }

  vkCmdEndRendering(cmd);

  // Gbuffer output -> SHADER_READ_ONLY for the surfel-generation compute
  // pass (and any later fragment consumer). Includes depth, which the
  // gbuffer left in DEPTH_STENCIL_ATTACHMENT_OPTIMAL. The surfel result
  // image transitions UNDEFINED -> GENERAL for its first compute write.
  {
    VkImageMemoryBarrier2 toRead[3] = {
        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2},
        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2},
        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2}};
    toRead[0].srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    toRead[0].srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    toRead[0].dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    toRead[0].dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    toRead[0].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toRead[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toRead[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toRead[0].image = RI.visibilityTexture[RI.swapchainIndex].vk.image;

    // Depth -> SHADER_READ_ONLY for the compute pass.
    toRead[1].srcStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                             VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    toRead[1].srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    toRead[1].dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    toRead[1].dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    toRead[1].oldLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    toRead[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toRead[1].subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    toRead[1].image = RI.depthTextures[RI.swapchainIndex].vk.image;

    // Surfel-result image: surfel_generation_pass only writes gOutput for
    // pixels actually covered by a surfel (indirectLighting.w > 0). Valid-hit
    // pixels with no surfel coverage — and out-of-cell pixels — are left
    // untouched (the pass was refactored to drop its miss-write, so it no
    // longer "unconditionally writes every in-bounds pixel"). But the composite
    // (SurfelGIRenderPass.frag) samples gIndirectLighting for EVERY valid-hit
    // pixel, so with sparse surfel coverage most of the screen would sample
    // undefined memory. Clear to (0,0,0,1) each frame so uncovered pixels read
    // as zero indirect instead of garbage. UNDEFINED oldLayout discards stale
    // contents; the clear (transfer) is handed off to the compute write below.
    toRead[2].srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    toRead[2].srcAccessMask = 0;
    toRead[2].dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
    toRead[2].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    toRead[2].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toRead[2].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    toRead[2].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toRead[2].image = m_surfelResultTexture[RI.swapchainIndex].vk.image;

    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 3;
    dep.pImageMemoryBarriers = toRead;
    vkCmdPipelineBarrier2(cmd, &dep);
  }

  // Clear the surfel-result image to zero indirect (see toRead[2] above), then
  // make the clear visible to surfel_generation_pass's storage write.
  {
    VkClearColorValue clearColor = {};
    clearColor.float32[3] = 1.0f; // (0,0,0,1): zero radiance, opaque alpha
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, m_surfelResultTexture[RI.swapchainIndex].vk.image,
                         VK_IMAGE_LAYOUT_GENERAL, &clearColor, 1, &range);

    VkImageMemoryBarrier2 afterClear = {
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    afterClear.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
    afterClear.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    afterClear.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    afterClear.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                               VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    afterClear.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    afterClear.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    afterClear.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    afterClear.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    afterClear.image = m_surfelResultTexture[RI.swapchainIndex].vk.image;
    afterClear.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &afterClear;
    vkCmdPipelineBarrier2(cmd, &dep);
  }

  // ----------------------------------------------------------------------
  // Stage F — surfel integrate + generation.
  //
  // integrate: per valid surfel, MSME-blend the per-frame raytraced
  //            radiance (from gSurfelRayResultBuffer) into surfel.radiance
  // generate:  per pixel, walk the visibility-buffer cell's surfel list,
  //            output the indirect-lighting term into m_surfelResultTexture
  //            (which visibility_shade.frag samples), and spawn / recycle
  //            surfels based on coverage thresholds.
  // ----------------------------------------------------------------------
  {
    VkComputePipelineCreateInfo computeCreate = {
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    m_surfelIntegrate.bindComputePipeline(&RI.device, &RI.primary.cmds[0],
                                          kHash, "SurfelIntegratePass.cs",
                                          &computeCreate);
    m_surfelIntegrate.bindBindlessDescriptorSet(
        &RI.primary.cmds[0], &m_bindlessSet, 0,
        VK_PIPELINE_BIND_POINT_COMPUTE);

    // gSurfelDepthSampler is bindless (set 0). The image views and the
    // CB are gone — params come from Constants.h. gPerFrame, the
    // irradiance atlas, and the depth atlas (RW + sampled views of the
    // same image) push into set 1.
    std::vector<RIProgram::DescriptorBinding> bnd;
    bnd.reserve(4);
    {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("gPerFrame");
      RI.UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
      bnd.push_back(b);
    }
    pushSurfelStorageImage(bnd, "gIrradianceMap",
                           m_surfelIrradianceView[RI.swapchainIndex].vk.image);
    pushSurfelStorageImage(bnd, "gSurfelDepthMap",
                           m_surfelDepthView[RI.swapchainIndex].vk.image);
    pushSurfelSampledImage(bnd, "gSurfelDepth",
                           m_surfelDepthView[RI.swapchainIndex].vk.image);
    m_surfelIntegrate.bindDescriptors(
        &RI.device, &RI.primary.cmds[0], RI.frameIndex, bnd.data(),
        bnd.size(), VK_PIPELINE_BIND_POINT_COMPUTE);
    CmdDispatch(&RI.primary.cmds[0], (kTotalSurfelLimit + 31u) / 32u, 1u, 1u);
  }
  {
    VkMemoryBarrier2 mem = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mem.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mem.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    mem.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    // SAMPLED_READ: with USE_SURFEL_DEPTH the generation pass samples the depth
    // atlas (gSurfelDepth) that integrate just wrote via a storage image; the
    // sampled read needs the write made visible to it, not just storage reads.
    mem.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mem;
    vkCmdPipelineBarrier2(cmd, &dep);
  }
  {
    VkComputePipelineCreateInfo computeCreate = {
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    m_surfelGenerate.bindComputePipeline(&RI.device, &RI.primary.cmds[0],
                                         kHash, "SurfelGenerationPass.cs",
                                         &computeCreate);
    m_surfelGenerate.bindBindlessDescriptorSet(
        &RI.primary.cmds[0], &m_bindlessSet, 0,
        VK_PIPELINE_BIND_POINT_COMPUTE);

    // gPerFrame + the set-1 surfel images (gPackedHitInfo, gSurfelDepth
    // sampled view) plus the per-pixel indirect-lighting output `gOutput`
    // at set 2 binding 0.
    std::vector<RIProgram::DescriptorBinding> bnd;
    bnd.reserve(4);
    {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("gPerFrame");
      RI.UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
      bnd.push_back(b);
    }
    pushSurfelStorageImage(bnd, "gPackedHitInfo",
                           m_packedHitInfoView[RI.swapchainIndex].vk.image);
    pushSurfelSampledImage(bnd, "gSurfelDepth",
                           m_surfelDepthView[RI.swapchainIndex].vk.image);
    {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("gOutput");
      b.descriptor.vk.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      b.descriptor.vk.image.sampler = VK_NULL_HANDLE;
      b.descriptor.vk.image.imageView =
          m_surfelResultView[RI.swapchainIndex].vk.image;
      b.descriptor.vk.image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
      RIFinalizeDescriptor(&RI.device, &b.descriptor);
      bnd.push_back(b);
    }
    m_surfelGenerate.bindDescriptors(
        &RI.device, &RI.primary.cmds[0], RI.frameIndex, bnd.data(),
        bnd.size(), VK_PIPELINE_BIND_POINT_COMPUTE);

    const uint32_t fullW = RI.swapchain.width;
    const uint32_t fullH = RI.swapchain.height;
    CmdDispatch(&RI.primary.cmds[0], (fullW + 15u) / 16u,
                (fullH + 15u) / 16u, 1u);
  }

  // --------------------------------------------------------------------
  // SurfelGIRenderPass — graphics fullscreen pass.
  //
  // Replaces the legacy visibility_shade composite (and the prior compute
  // version of this same Slang module). Reads gIndirectLighting
  // (m_surfelResultView, written by SurfelGenerationPass above) plus
  // gPackedHitInfo / gPackedHitInfoRaster / TLAS / gPerFrame, and emits
  // its color into the pogo buffer's COLOR_ATTACHMENT side.
  //
  // The post-effect chain ping-pongs through pogo from there; a tail
  // blit lower in this function copies the final pogo "read" half into
  // the swapchain image, which the particle/decal pass then composites
  // on top of.
  // --------------------------------------------------------------------

  RI_PogoBuffer *pogo = &RI.pogoBuffer[RI.swapchainIndex];

  // Barrier 1: gIndirectLighting GENERAL→SHADER_READ_ONLY (compute write
  // is now consumed by a fragment-stage sample) + first-frame pogo init
  // (UNDEFINED → COLOR_ATTACHMENT_OPTIMAL on the attach half, UNDEFINED →
  // SHADER_READ_ONLY_OPTIMAL on the other half, matching the steady-state
  // the toggle helpers expect).
  {
    VkMemoryBarrier2 mem = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mem.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mem.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    mem.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    // SAMPLED_READ for the gIndirectLighting image sample; STORAGE_READ so the
    // isWater branch's gatherSurfelIndirect can read the surfel-cache SSBOs
    // (gSurfelBuffer / gCellInfoBuffer / gCellToSurfelBuffer), written by the
    // surfel compute passes earlier this frame, from the fragment stage.
    mem.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
                        VK_ACCESS_2_SHADER_STORAGE_READ_BIT;

    std::vector<VkImageMemoryBarrier2> imageBarriers;
    imageBarriers.reserve(3);

    {
      VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
      b.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
      b.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
      b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
      b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
      b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
      b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.image = m_surfelResultTexture[RI.swapchainIndex].vk.image;
      b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      imageBarriers.push_back(b);
    }

    if (!m_pogoInitialized[RI.swapchainIndex]) {
      const uint32_t readIdx = (pogo->attachmentIndex + 1u) % 2u;
      imageBarriers.push_back(VK_RI_PogoAttachmentMemoryBarrier2(
          pogo->textures[pogo->attachmentIndex].vk.image, /*initial=*/true));
      imageBarriers.push_back(VK_RI_PogoShaderMemoryBarrier2(
          pogo->textures[readIdx].vk.image, /*initial=*/true));
      m_pogoInitialized[RI.swapchainIndex] = true;
    }

    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mem;
    dep.imageMemoryBarrierCount =
        static_cast<uint32_t>(imageBarriers.size());
    dep.pImageMemoryBarriers = imageBarriers.data();
    vkCmdPipelineBarrier2(RI.primary.cmds[0].vk.cmd, &dep);
  }

  // Depth flip shared by the decal pre-pass (below) and the particle /
  // translucent passes further down: depth arrives in SHADER_READ_ONLY from
  // surfel-generate and flips once to DEPTH_READ_ONLY for any depth-tested pass.
  bool depthFlippedForReadOnly = false;
  auto flipDepthToReadOnly = [&]() {
    if (depthFlippedForReadOnly)
      return;
    VkImageMemoryBarrier2 depthBarrier = {
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    depthBarrier.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    depthBarrier.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
                                 VK_ACCESS_2_SHADER_READ_BIT;
    depthBarrier.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    depthBarrier.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    depthBarrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    depthBarrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    depthBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    depthBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    depthBarrier.image = RI.depthTextures[RI.swapchainIndex].vk.image;
    depthBarrier.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &depthBarrier;
    vkCmdPipelineBarrier2(RI.primary.cmds[0].vk.cmd, &dep);
    depthFlippedForReadOnly = true;
  };


  // Surfel-GI graphics pass — writes color into the pogo attach side.
  {
    VkRenderingAttachmentInfo colorAttach = {
        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    colorAttach.imageView =
        pogo->pogoAttachment[pogo->attachmentIndex].vk.image.imageView;
    colorAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo renderInfo = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    renderInfo.renderArea = {{0, 0},
                             {RI.swapchain.width, RI.swapchain.height}};
    renderInfo.layerCount = 1;
    renderInfo.colorAttachmentCount = 1;
    renderInfo.pColorAttachments    = &colorAttach;

    vkCmdBeginRendering(RI.primary.cmds[0].vk.cmd, &renderInfo);

    VkViewport vp = {0.0f,
                     0.0f,
                     static_cast<float>(RI.swapchain.width),
                     static_cast<float>(RI.swapchain.height),
                     0.0f,
                     1.0f};
    vkCmdSetViewport(RI.primary.cmds[0].vk.cmd, 0, 1, &vp);
    VkRect2D sc = {{0, 0}, {RI.swapchain.width, RI.swapchain.height}};
    vkCmdSetScissor(RI.primary.cmds[0].vk.cmd, 0, 1, &sc);

    PostEffectPipelineState surfelState{};
    InitPostEffectPipelineState(surfelState, RIBootstrap::PogoColorFormatVk,
                                /*alphaBlend=*/false);

    const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    m_surfelGIRender.bindPipeline(&RI.device, &RI.primary.cmds[0], kHash,
                                  "SurfelGIRenderPass",
                                  &surfelState.createInfo);
    m_surfelGIRender.bindBindlessDescriptorSet(
        &RI.primary.cmds[0], &m_bindlessSet, 0,
        VK_PIPELINE_BIND_POINT_GRAPHICS);

    std::vector<RIProgram::DescriptorBinding> bnd;
    bnd.reserve(5);
    {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("gPerFrame");
      RI.UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
      bnd.push_back(b);
    }
    pushSurfelStorageImage(bnd, "gPackedHitInfo",
                           m_packedHitInfoView[RI.swapchainIndex].vk.image);
    pushSurfelStorageImage(bnd, "gPackedRefractionHitInfo",
                           m_packedRefractionHitInfoView[RI.swapchainIndex].vk.image);
    // The water branch in SurfelGIRenderPass.frag also reads the reflection
    // bounce V-buffer to shade water reflections inline.
    pushSurfelStorageImage(bnd, "gPackedReflectionHitInfo",
                           m_packedReflectionHitInfoView[RI.swapchainIndex].vk.image);
    pushTlas(bnd);
    {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("gIndirectLighting");
      b.descriptor.vk.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      b.descriptor.vk.image.sampler = VK_NULL_HANDLE;
      b.descriptor.vk.image.imageView =
          m_surfelResultView[RI.swapchainIndex].vk.image;
      b.descriptor.vk.image.imageLayout =
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      RIFinalizeDescriptor(&RI.device, &b.descriptor);
      bnd.push_back(b);
    }
    {
      // Rasterized V-buffer fallback — SurfelGBuffer writes
      // RI.visibilityTexture earlier this frame and the toRead[] barriers
      // upstream already transitioned it to SHADER_READ_ONLY_OPTIMAL.
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("gPackedHitInfoRaster");
      b.descriptor.vk.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      b.descriptor.vk.image.sampler = VK_NULL_HANDLE;
      b.descriptor.vk.image.imageView =
          RI.visibilityView[RI.swapchainIndex].vk.image;
      b.descriptor.vk.image.imageLayout =
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      RIFinalizeDescriptor(&RI.device, &b.descriptor);
      bnd.push_back(b);
    }
    m_surfelGIRender.bindDescriptors(
        &RI.device, &RI.primary.cmds[0], RI.frameIndex, bnd.data(),
        bnd.size(), VK_PIPELINE_BIND_POINT_GRAPHICS);

    vkCmdDraw(RI.primary.cmds[0].vk.cmd, 3, 1, 0, 0);
    vkCmdEndRendering(RI.primary.cmds[0].vk.cmd);
  }


  // Toggle: just-written attach → SHADER_READ_ONLY (now the "read" half)
  // so downstream post-effects + tail blit can sample it.
  RI_PogoBufferToggle(&RI.device, pogo, &RI.primary.cmds[0]);

  // The viewport's post-effect composite and the pogo->swapchain tail blit run
  // in cScene::Render after this Draw returns — Draw just leaves the finished
  // scene (incl. particles) in the pogo "read" half.

  // (depthFlippedForReadOnly + flipDepthToReadOnly are defined above, before the
  // decal pre-pass, and shared with the particle / translucent passes below.)


  // --------------------------------------------------------------------
  // Translucent pass — two sub-passes, both into the pogo "read" half.
  //
  //   1. Particle pass    (this block)            — particle emitters only.
  //   2. Mesh pass        (block immediately below) — non-particle, non-
  //                       refraction, non-reflection translucent meshes.
  //
  // Each sub-pass opens its own vkCmdBeginRendering/EndRendering and runs
  // its own pogo barriers, so the two are independently skippable when one
  // side has no work. Refraction and world-reflection materials are still
  // out of scope (the hybrid renderer has no screen-color copy and no
  // reflection buffer); both are filtered out in the mesh-pass collection.
  //
  // Fog-area visibility for translucents is computed per-pixel inside
  // Translucent.frag.slang by iterating the gFogAreas SSBO — see the
  // per-frame fog-area upload above for where that buffer is filled.
  //
  // Resources reused from the opaque path (both sub-passes):
  //   - m_diffuseBindless / m_diffuseObjectBuffer (per-renderable OBJECT slot)
  //   - m_opaque*Handles  (BDA fan-out — particles and mesh translucents
  //                        overload position/uv0/color/index with their own
  //                        VB device addresses)
  //   - m_materialBindless / m_opaqueMaterialBuffer (material slot; only
  //                        the diffuse texture index is read by the shaders)
  //
  // Sync setup:
  //   (a) Swapchain stays in COLOR_ATTACHMENT_OPTIMAL from the visibility
  //       composite — load to preserve the composite output.
  //   (b) Depth was last transitioned to SHADER_READ_ONLY for surfel-
  //       generate; flipDepthToReadOnly() (idempotent) moves it back to
  //       DEPTH_READ_ONLY_OPTIMAL — shared with the decal pass.
  // --------------------------------------------------------------------
  {
    // Collect particle emitters from the translucent list once so we can
    // skip the whole pass (and its barriers/begin-rendering) when empty.
    std::vector<iParticleEmitter *> emitters;
    for (iRenderable *pObj :
         m_rendererList.GetRenderableItems(eRenderListType_Translucent)) {
      if (!pObj || pObj->GetRenderType() != eRenderableType_ParticleEmitter)
        continue;
      cMaterial *pMat = pObj->GetMaterial();
      if (!pMat)
        continue;
      const eMaterialBlendMode mode = pMat->GetBlendMode();
      if (mode == eMaterialBlendMode_None ||
          mode >= eMaterialBlendMode_LastEnum)
        continue;
      emitters.push_back(static_cast<iParticleEmitter *>(pObj));
    }

    if (!emitters.empty()) {
      // Per-emitter UpdateGraphicsForFrame / UpdateGraphicsForViewport +
      // SubmitToGPU already happened in the consolidated translucent prepare
      // loop near the top of Draw(); the particle VBs are uploaded and
      // attached to the frame context by the time we get here.
      flipDepthToReadOnly();

      // Render translucent particles INTO the pogo "read" half (which holds the
      // composited + post-effected scene), not the swapchain — so the pogo (and
      // anything that samples it, e.g. the menu/inventory screen capture) carries
      // particles too. Flip that half COLOR_ATTACHMENT for the draw, then back to
      // SHADER_READ after, so the tail blit below can sample it.
      const int   pogoReadIdx   = (pogo->attachmentIndex + 1) % 2;
      VkImage     pogoReadImage = pogo->textures[pogoReadIdx].vk.image;
      VkImageView pogoReadView  = pogo->pogoAttachment[pogoReadIdx].vk.image.imageView;
      const RI_Format_e particleTargetFormat = RIBootstrap::PogoColorFormat;
      {
        VkImageMemoryBarrier2 b =
            VK_RI_PogoAttachmentMemoryBarrier2(pogoReadImage, /*initial=*/false);
        VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers    = &b;
        vkCmdPipelineBarrier2(RI.primary.cmds[0].vk.cmd, &dep);
      }

      VkRenderingAttachmentInfo colorAttach = {
          VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
      colorAttach.imageView = pogoReadView;
      colorAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      colorAttach.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      colorAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

      VkRenderingAttachmentInfo depthAttach = {
          VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
      depthAttach.imageView = RI.depthView[RI.swapchainIndex].vk.image;
      depthAttach.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
      depthAttach.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      depthAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

      VkRenderingInfo renderInfo = {VK_STRUCTURE_TYPE_RENDERING_INFO};
      renderInfo.renderArea = {{0, 0},
                               {RI.swapchain.width, RI.swapchain.height}};
      renderInfo.layerCount = 1;
      renderInfo.colorAttachmentCount = 1;
      renderInfo.pColorAttachments = &colorAttach;
      renderInfo.pDepthAttachment = &depthAttach;

      vkCmdBeginRendering(RI.primary.cmds[0].vk.cmd, &renderInfo);

      VkViewport vp = {0.0f,
                       (float)RI.swapchain.height,
                       (float)RI.swapchain.width,
                       -(float)RI.swapchain.height,
                       0.0f,
                       1.0f};
      VkRect2D sc = {{0, 0}, {RI.swapchain.width, RI.swapchain.height}};
      vkCmdSetViewport(RI.primary.cmds[0].vk.cmd, 0, 1, &vp);
      vkCmdSetScissor(RI.primary.cmds[0].vk.cmd, 0, 1, &sc);

      m_particle.bindBindlessDescriptorSet(&RI.primary.cmds[0], &m_bindlessSet,
                                           0);

      std::vector<RIProgram::DescriptorBinding> particleBindings;
      particleBindings.reserve(1);
      {
        RIProgram::DescriptorBinding b;
        b.handle = DescriptorBindingID::Create("gPerFrame");
        RI.UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
        particleBindings.push_back(b);
      }
      m_particle.bindDescriptors(&RI.device, &RI.primary.cmds[0], RI.frameIndex,
                                 particleBindings.data(),
                                 particleBindings.size());

      // Map eMaterialBlendMode -> ParticlePipelineDesc::BlendMode +
      // shader-side BLEND_MODE_*. eMaterialBlendMode_None is filtered above.
      auto remapBlend = [](eMaterialBlendMode m) {
        switch (m) {
        case eMaterialBlendMode_Add:
          return ParticlePipelineDesc::BLEND_ADD;
        case eMaterialBlendMode_Mul:
          return ParticlePipelineDesc::BLEND_MUL;
        case eMaterialBlendMode_MulX2:
          return ParticlePipelineDesc::BLEND_MULX2;
        case eMaterialBlendMode_Alpha:
          return ParticlePipelineDesc::BLEND_ALPHA;
        case eMaterialBlendMode_PremulAlpha:
          return ParticlePipelineDesc::BLEND_PREMUL_ALPHA;
        default:
          return ParticlePipelineDesc::BLEND_ADD;
        }
      };

      struct PushBlock {
        uint32_t blendMode;
        float sceneAlpha;
      };

      for (iParticleEmitter *pEmitter : emitters) {
        iVertexBuffer *pVB = pEmitter->GetVertexBuffer();
        cMaterial *pMat = pEmitter->GetMaterial();
        if (!pVB || !pMat)
          continue;
        int indexCount = pVB->GetElementNum();   // live count: mlNumOfParticles * 6
        if (indexCount < 0)                       // -1 == "not set" -> draw all
          indexCount = pVB->GetIndexNum();
        if (indexCount <= 0)                      // 0 == emitter requested skip
          continue;

        uint32_t materialSlot =
            resolveMaterial(cntx, pMat, (uint32_t)RI.frameIndex);
        if (materialSlot == UINT32_MAX) {
          Warning("Material Slot exhausted (particle)");
          continue;
        }

        cMatrixf *pMtx = pEmitter->GetModelMatrix(apFrustum);
        UniformObject payload{};
        payload.dissolveAmount = 0.0f;
        payload.materialID = materialSlot;
        payload.lightLevel = 1.0f;
        payload.illuminationAmount = 0.0f;
        const ml::float4x4 modelF4 =
            cMath::ToFloatTranspose4x4(pMtx ? *pMtx : cMatrixf::Identity);
        std::memcpy(payload.modelMat, modelF4.a, sizeof(payload.modelMat));
        ml::float4x4 invF4 = modelF4;
        invF4.Invert();
        std::memcpy(payload.invModelMat, invF4.a, sizeof(payload.invModelMat));
        const ml::float4x4 uvF4 =
            cMath::ToFloatTranspose4x4(cMatrixf::Identity);
        std::memcpy(payload.uvMat, uvF4.a, sizeof(payload.uvMat));

        // Particle VB contents change every frame, so hash a per-emitter
        // identity (pointer + frame counter) rather than the payload contents
        // — payload-hash collisions across frames would skip the BDA refresh
        // even though the VB device addresses might have changed.
        hash_t cookie = hash_u64(HASH_INITIAL_VALUE,
                                 (uint64_t)(uintptr_t)pEmitter);
        cookie = hash_u32(cookie, (uint32_t)RI.frameIndex);
        auto req =
            m_diffuseBindless.request(cookie, (uint32_t)RI.frameIndex);
        if (req.exhausted) {
          Warning("bindless pool exhausted (particle)");
          continue;
        }

        // Particles share m_diffuseBindless with opaque solids. When the LRU
        // hands this slot to a particle (a fresh assignment, req.found == false)
        // the handle BDAs below get overwritten with the particle's smaller
        // vertex/index buffers. A surfel still anchored to the slot's previous
        // opaque occupant would otherwise pass collectCellInfo's slotStale guard
        // (its captured generation still matches) and feed its stale opaque
        // primitiveIndex into fetchBindlessTriangle against the particle mesh —
        // an unbounded raw-BDA read off the end of the index/vertex buffer ->
        // GPUVM fault. Bump the slot generation on (re)assignment, exactly like
        // the opaque miss path, so those surfels self-invalidate. The bump is
        // staged with every other mirror write at the Draw() tail and applied by
        // the uploader pre-pass before this same frame's surfel passes run.
        if (!req.found) {
          m_bindlessSlotGenerationMirror.write<uint32_t>(req.id,
                                                         ++m_nextSlotGeneration);
        }

        {
          RIResourceBufferTransaction_s trans = {};
          trans.target = m_diffuseObjectBuffer;
          trans.size = sizeof(UniformObject);
          trans.offset = (size_t)req.id * sizeof(UniformObject);
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

        auto *vbri = static_cast<VertexBuffer_RI *>(pVB);
        auto bdaOf = [&](eVertexBufferElement type) -> VkDeviceAddress {
          const auto *element = vbri->GetElement(type);
          if (!element || !element->buffer)
            return 0;
          return element->buffer->GetDeviceHandle(&RI.device);
        };
        const VkDeviceAddress posAddr =
            bdaOf(eVertexBufferElement_Position);
        const VkDeviceAddress uv0Addr =
            bdaOf(eVertexBufferElement_Texture0);
        const VkDeviceAddress colAddr =
            bdaOf(eVertexBufferElement_Color0);
        const VkDeviceAddress idxAddr =
            vbri->GetIndexRIBuffer()
                ? vbri->GetIndexRIBuffer()->GetDeviceHandle(&RI.device)
                : 0;

        // Only the four streams the particle VS reads need to be valid;
        // tangent/normal stay zero so any leftover handles from a prior
        // opaque draw at this slot are deref-safe-but-unused.
        auto writeSlot = [&](BindlessShadowMirror &mir, VkDeviceAddress addr) {
          mir.write<VkDeviceAddress>(req.id, addr);
        };
        writeSlot(m_opaquePositionMirror, posAddr);
        writeSlot(m_opaqueUv0Mirror, uv0Addr);
        writeSlot(m_opaqueColorMirror, colAddr);
        writeSlot(m_opaqueIndexMirror, idxAddr);
        writeSlot(m_opaqueNormalMirror, 0);
        writeSlot(m_opaqueTangentMirror, 0);

        const ParticlePipelineDesc::BlendMode mode =
            remapBlend(pMat->GetBlendMode());
        ParticlePipelineDesc pipelineDesc(particleTargetFormat,
                                          RIBootstrap::DepthFormat, mode);
        m_particle.bindPipeline(&RI.device, &RI.primary.cmds[0],
                                pipelineDesc.hash, "Particle",
                                &pipelineDesc.createInfo);

        // Fog (world + per-area) is applied per-pixel in Particle.frag.slang
        // by walking gFogAreas. sceneAlpha is now the unmodified per-object
        // scalar (1.0 by default) — kept in the push block for parity with the
        // mesh path and any future per-object alpha gates.
        const float sceneAlpha = 1.0f;
        PushBlock push = {(uint32_t)mode, sceneAlpha};
        vkCmdPushConstants(RI.primary.cmds[0].vk.cmd,
                           m_particle.getPipelineLayout(),
                           VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push),
                           &push);

        vkCmdDraw(RI.primary.cmds[0].vk.cmd, (uint32_t)indexCount, 1u, 0u,
                  req.id);
      }

      vkCmdEndRendering(RI.primary.cmds[0].vk.cmd);

      // pogo "read" half back to SHADER_READ_ONLY so the tail blit can sample it.
      {
        VkImageMemoryBarrier2 b =
            VK_RI_PogoShaderMemoryBarrier2(pogoReadImage, /*initial=*/false);
        VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers    = &b;
        vkCmdPipelineBarrier2(RI.primary.cmds[0].vk.cmd, &dep);
      }
    }
  }

  // --------------------------------------------------------------------
  // Translucent mesh pass (sub-pass 2 — see the comment above the particle
  // block for the overall plan). Runs after particles so any glow billboards
  // composite under solid translucent geometry, mirroring the legacy
  // back-to-front order. SortFunc_Translucent in RenderList.cpp already
  // ordered the translucent list back-to-front, so iterating in list order
  // here gives correct Alpha-blend results.
  //
  // Refraction is now handled upstream by SurfelVBuffer.rt's primary-ray
  // bend (closeHit::isRefractive branch) — the GI composite already shows
  // the refracted background under refractive translucents by the time
  // this pass runs. Water is not rasterized here at all — it's composited
  // entirely in SurfelGIRenderPass.frag (isWater branch) and is filtered
  // out of the mesh collection below.
  // --------------------------------------------------------------------
  {
    std::vector<iRenderable *> meshes;
    for (iRenderable *pObj :
         m_rendererList.GetRenderableItems(eRenderListType_Translucent)) {
      if (!pObj)
        continue;
      if (pObj->GetRenderType() == eRenderableType_ParticleEmitter)
        continue;
      cMaterial *pMat = pObj->GetMaterial();
      if (!pMat)
        continue;
      const eMaterialBlendMode mode = pMat->GetBlendMode();
      if (mode == eMaterialBlendMode_None ||
          mode >= eMaterialBlendMode_LastEnum)
        continue;
      // No material-flag filtering here: HasRefraction draws through the
      // standard blend pipeline (the background already comes pre-refracted
      // from the RT V-buffer); HasWorldReflection is harmless (the
      // legacy-only planar reflection buffer is unused).
      // Water is NOT rasterized — it's composited entirely in
      // SurfelGIRenderPass.frag (isWater branch) from the primary hit + the
      // refraction / reflection bounce V-buffers, so skip it here.
      if (pMat->Descriptor().m_id == MaterialID::Water)
        continue;
      iVertexBuffer *pVB = pObj->GetVertexBuffer();
      if (!pVB || pVB->GetIndexNum() <= 0)
        continue;
      meshes.push_back(pObj);
    }

    if (!meshes.empty()) {
      // Per-mesh UpdateGraphicsForFrame / UpdateGraphicsForViewport +
      // SubmitToGPU already happened in the consolidated translucent prepare
      // loop near the top of Draw(); billboards / beams / glass / water all
      // have valid vk.buffer + (where applicable) BLAS by the time we get
      // here. Depth flip is idempotent — safe to call regardless of whether
      // the particle pass ran above.
      flipDepthToReadOnly();

      const int   pogoReadIdx   = (pogo->attachmentIndex + 1) % 2;
      VkImage     pogoReadImage = pogo->textures[pogoReadIdx].vk.image;
      VkImageView pogoReadView  = pogo->pogoAttachment[pogoReadIdx].vk.image.imageView;
      const RI_Format_e meshTargetFormat = RIBootstrap::PogoColorFormat;

      // SHADER_READ_ONLY → COLOR_ATTACHMENT_OPTIMAL. If the particle pass
      // ran above, that block left the pogo half in SHADER_READ_ONLY (for
      // a tail blit that never got to run); if it didn't, the visibility
      // composite + post-effect chain also left it in SHADER_READ_ONLY. The
      // barrier helper handles either source state.
      {
        VkImageMemoryBarrier2 b =
            VK_RI_PogoAttachmentMemoryBarrier2(pogoReadImage, /*initial=*/false);
        VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers    = &b;
        vkCmdPipelineBarrier2(RI.primary.cmds[0].vk.cmd, &dep);
      }

      VkRenderingAttachmentInfo colorAttach = {
          VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
      colorAttach.imageView = pogoReadView;
      colorAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      colorAttach.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      colorAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

      VkRenderingAttachmentInfo depthAttach = {
          VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
      depthAttach.imageView = RI.depthView[RI.swapchainIndex].vk.image;
      depthAttach.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
      depthAttach.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      depthAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

      VkRenderingInfo renderInfo = {VK_STRUCTURE_TYPE_RENDERING_INFO};
      renderInfo.renderArea = {{0, 0},
                               {RI.swapchain.width, RI.swapchain.height}};
      renderInfo.layerCount = 1;
      renderInfo.colorAttachmentCount = 1;
      renderInfo.pColorAttachments = &colorAttach;
      renderInfo.pDepthAttachment = &depthAttach;

      vkCmdBeginRendering(RI.primary.cmds[0].vk.cmd, &renderInfo);

      VkViewport vp = {0.0f,
                       (float)RI.swapchain.height,
                       (float)RI.swapchain.width,
                       -(float)RI.swapchain.height,
                       0.0f,
                       1.0f};
      VkRect2D sc = {{0, 0}, {RI.swapchain.width, RI.swapchain.height}};
      vkCmdSetViewport(RI.primary.cmds[0].vk.cmd, 0, 1, &vp);
      vkCmdSetScissor(RI.primary.cmds[0].vk.cmd, 0, 1, &sc);

      m_translucentMesh.bindBindlessDescriptorSet(&RI.primary.cmds[0],
                                                  &m_bindlessSet, 0);

      std::vector<RIProgram::DescriptorBinding> meshBindings;
      meshBindings.reserve(1);
      {
        RIProgram::DescriptorBinding b;
        b.handle = DescriptorBindingID::Create("gPerFrame");
        RI.UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
        meshBindings.push_back(b);
      }
      m_translucentMesh.bindDescriptors(&RI.device, &RI.primary.cmds[0],
                                        RI.frameIndex, meshBindings.data(),
                                        meshBindings.size());

      // Map eMaterialBlendMode -> TranslucentMeshPipelineDesc::BlendMode +
      // shader-side BLEND_MODE_*. Mirrors the same remap used by the
      // particle pass; eMaterialBlendMode_None is filtered above.
      auto remapBlend = [](eMaterialBlendMode m) {
        switch (m) {
        case eMaterialBlendMode_Add:
          return TranslucentMeshPipelineDesc::BLEND_ADD;
        case eMaterialBlendMode_Mul:
          return TranslucentMeshPipelineDesc::BLEND_MUL;
        case eMaterialBlendMode_MulX2:
          return TranslucentMeshPipelineDesc::BLEND_MULX2;
        case eMaterialBlendMode_Alpha:
          return TranslucentMeshPipelineDesc::BLEND_ALPHA;
        case eMaterialBlendMode_PremulAlpha:
          return TranslucentMeshPipelineDesc::BLEND_PREMUL_ALPHA;
        default:
          return TranslucentMeshPipelineDesc::BLEND_ADD;
        }
      };

      // Mirrors TranslucentPushConstants in Translucent.frag.slang. Options
      // bitfield carries TRANS_OPT_USE_ILLUMINATION for the optional second
      // cube-map-only draw — main draw passes options=0.
      struct PushBlock {
        uint32_t blendMode;
        float    sceneAlpha;
        uint32_t options;
        uint32_t _pad;
      };
      constexpr uint32_t kTransOptUseIllumination = 1u << 0;

      for (iRenderable *pObj : meshes) {
        iVertexBuffer *pVB = pObj->GetVertexBuffer();
        cMaterial *pMat = pObj->GetMaterial();
        // Filter above already rejected null pVB / pMat and 0-index VBs.
        const int indexCount = pVB->GetIndexNum();

        uint32_t materialSlot =
            resolveMaterial(cntx, pMat, (uint32_t)RI.frameIndex);
        if (materialSlot == UINT32_MAX) {
          Warning("Material Slot exhausted (translucent mesh)");
          continue;
        }

        cMatrixf *pMtx = pObj->GetModelMatrix(apFrustum);

        // Affecting-light accumulation mirrors RendererDeferred::
        // cmdBindMaterialAndObject (RendererDeferred.cpp:3746-3773). Iterate
        // the visible-light list, take the max-channel intensity scaled by
        // distance falloff for spot/point and full intensity for box lights,
        // clamp the running sum at 1.0. Only translucent materials carry the
        // m_isAffectedByLightLevel flag, so the check sits behind a
        // MaterialID::Translucent gate (Water/Decal don't have this field on
        // the descriptor union).
        float lightLevel = 1.0f;
        const ShaderMaterialData &desc = pMat->Descriptor();
        const bool affectedByLights =
            desc.m_id == MaterialID::Translucent &&
            desc.m_translucent.m_isAffectedByLightLevel;
        if (affectedByLights) {
          const cVector3f vCenterPos =
              pObj->GetBoundingVolume()->GetWorldCenter();
          float fLightAmount = 0.0f;
          for (iLight *pLight : m_rendererList.GetLights()) {
            if (!pLight->CheckObjectIntersection(pObj))
              continue;
            const cColor &c = pLight->GetDiffuseColor();
            const float maxColor =
                cMath::Max(cMath::Max(c.r, c.g), c.b);
            if (pLight->GetLightType() == eLightType_Box) {
              fLightAmount += maxColor;
            } else {
              const float fDist = cMath::Vector3Dist(
                  pLight->GetWorldPosition(), vCenterPos);
              fLightAmount +=
                  maxColor *
                  cMath::Max(1.0f - (fDist / pLight->GetRadius()), 0.0f);
            }
            if (fLightAmount >= 1.0f) {
              fLightAmount = 1.0f;
              break;
            }
          }
          lightLevel = fLightAmount;
        }

        UniformObject payload{};
        payload.dissolveAmount = pObj->GetCoverageAmount();
        payload.materialID = materialSlot;
        payload.lightLevel = lightLevel;
        payload.illuminationAmount = 0.0f;
        const ml::float4x4 modelF4 =
            cMath::ToFloatTranspose4x4(pMtx ? *pMtx : cMatrixf::Identity);
        std::memcpy(payload.modelMat, modelF4.a, sizeof(payload.modelMat));
        ml::float4x4 invF4 = modelF4;
        invF4.Invert();
        std::memcpy(payload.invModelMat, invF4.a, sizeof(payload.invModelMat));
        const ml::float4x4 uvF4 =
            cMath::ToFloatTranspose4x4(pMat->GetUvMatrix());
        std::memcpy(payload.uvMat, uvF4.a, sizeof(payload.uvMat));

        // Same per-renderable cookie shape as the particle path: pointer +
        // frame counter. See the long comment in the particle loop for why
        // we don't payload-hash here.
        hash_t cookie = hash_u64(HASH_INITIAL_VALUE,
                                 (uint64_t)(uintptr_t)pObj);
        cookie = hash_u32(cookie, (uint32_t)RI.frameIndex);
        auto req =
            m_diffuseBindless.request(cookie, (uint32_t)RI.frameIndex);
        if (req.exhausted) {
          Warning("bindless pool exhausted (translucent mesh)");
          continue;
        }

        // Bump slot generation on (re)assignment for the same surfel
        // self-invalidate reason documented in the particle path above —
        // surfels still anchored to this slot's previous opaque occupant
        // would otherwise dereference stale primitiveIndex against the
        // wrong VB/IB.
        if (!req.found) {
          m_bindlessSlotGenerationMirror.write<uint32_t>(req.id,
                                                         ++m_nextSlotGeneration);
        }

        {
          RIResourceBufferTransaction_s trans = {};
          trans.target = m_diffuseObjectBuffer;
          trans.size = sizeof(UniformObject);
          trans.offset = (size_t)req.id * sizeof(UniformObject);
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

        auto *vbri = static_cast<VertexBuffer_RI *>(pVB);

        // Per-vertex streams move to fixed-function vertex fetch (the
        // pipeline now declares them as VkVertexInputBindingDescriptions
        // — see TranslucentMeshPipelineDesc). Bindless OBJECT-slot
        // handle arrays (m_opaque*Mirror) stay populated only on the
        // opaque + TLAS paths, where they're consumed by surfel ray
        // traces and the opaque/particle vertex pulls.
        auto bufOf = [&](eVertexBufferElement type) -> VkBuffer {
          const auto *element = vbri->GetElement(type);
          if (!element || !element->buffer)
            return VK_NULL_HANDLE;
          return element->buffer->vk.buffer;
        };
        const VkBuffer posBuf = bufOf(eVertexBufferElement_Position);
        VkBuffer       nrmBuf = bufOf(eVertexBufferElement_Normal);
        VkBuffer       tanBuf = bufOf(eVertexBufferElement_Texture1Tangent);
        VkBuffer       colBuf = bufOf(eVertexBufferElement_Color0);
        VkBuffer       uvBuf  = bufOf(eVertexBufferElement_Texture0);
        const auto &idxRI     = vbri->GetIndexRIBuffer();
        const VkBuffer idxBuf = idxRI ? idxRI->vk.buffer : VK_NULL_HANDLE;
        if (!posBuf || !idxBuf) {
          // Position + index are truly required — without geometry there's
          // nothing to draw. The other streams have default-value fallback
          // buffers (see m_translucent*Fallback) so cBillboard / cBeam (which
          // omit tangent) and other lean layouts still render.
          Warning("translucent mesh missing position / index — skipping");
          continue;
        }
        // Substitute fallback buffers for absent optional streams. Each
        // fallback was filled at init with a sensible default
        // (normal = +Z, tangent = +X/handedness, color = white, uv = 0).
        // Capacity guard: every fallback was sized to
        // kTranslucentFallbackVerts; warn-and-skip rather than fault if a
        // pathologically large renderable arrives.
        const uint32_t vertCount = (uint32_t)pVB->GetVertexNum();
        const bool needsFallback = (!nrmBuf || !tanBuf || !colBuf || !uvBuf);
        if (needsFallback && vertCount > kTranslucentFallbackVerts) {
          Warning("translucent mesh vertex count exceeds fallback "
                  "capacity — skipping");
          continue;
        }
        if (!nrmBuf) nrmBuf = m_translucentNormalFallback.vk.buffer;
        if (!tanBuf) tanBuf = m_translucentTangentFallback.vk.buffer;
        if (!colBuf) colBuf = m_translucentColorFallback.vk.buffer;
        if (!uvBuf)  uvBuf  = m_translucentUv0Fallback.vk.buffer;
        const VkBuffer     vertBufs[5]  = {posBuf, nrmBuf, tanBuf, colBuf, uvBuf};
        const VkDeviceSize vertOffsets[5] = {0, 0, 0, 0, 0};

        const TranslucentMeshPipelineDesc::BlendMode mode =
            remapBlend(pMat->GetBlendMode());
        TranslucentMeshPipelineDesc pipelineDesc(meshTargetFormat,
                                                 RIBootstrap::DepthFormat,
                                                 mode);
        m_translucentMesh.bindPipeline(&RI.device, &RI.primary.cmds[0],
                                       pipelineDesc.hash, "TranslucentMesh",
                                       &pipelineDesc.createInfo);

        // Fog (world + per-area) is applied per-pixel in Translucent.frag.slang
        // by walking gFogAreas. sceneAlpha stays 1.0 for the no-extra-alpha
        // common path.
        const float sceneAlpha = 1.0f;
        PushBlock push = {(uint32_t)mode, sceneAlpha, 0u, 0u};
        vkCmdPushConstants(RI.primary.cmds[0].vk.cmd,
                           m_translucentMesh.getPipelineLayout(),
                           VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push),
                           &push);

        vkCmdBindVertexBuffers(RI.primary.cmds[0].vk.cmd, 0, 5,
                               vertBufs, vertOffsets);
        vkCmdBindIndexBuffer(RI.primary.cmds[0].vk.cmd, idxBuf, 0,
                             VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(RI.primary.cmds[0].vk.cmd, (uint32_t)indexCount, 1u,
                         0u, 0, req.id);

        // Second draw for the cube-map Fresnel + rim contribution.
        // Reference gates this on `cubeMap && !isRefraction`
        // (RendererDeferred.cpp:4660); under the RT ray-bend model
        // isRefraction surfaces already get their refracted background
        // from the GI composite, so the cube-map second draw stays gated
        // only on whether the material carries a cube map.
        if (pMat->GetImage(eMaterialTexture_CubeMap)) {
          TranslucentMeshPipelineDesc addDesc(
              meshTargetFormat, RIBootstrap::DepthFormat,
              TranslucentMeshPipelineDesc::BLEND_ADD);
          m_translucentMesh.bindPipeline(&RI.device, &RI.primary.cmds[0],
                                         addDesc.hash, "TranslucentMeshIllum",
                                         &addDesc.createInfo);
          PushBlock pushIllum = {
              (uint32_t)TranslucentMeshPipelineDesc::BLEND_ADD, sceneAlpha,
              kTransOptUseIllumination, 0u};
          vkCmdPushConstants(RI.primary.cmds[0].vk.cmd,
                             m_translucentMesh.getPipelineLayout(),
                             VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                             sizeof(pushIllum), &pushIllum);
          // Vertex / index buffers stay bound from the main draw above —
          // same renderable, just a second pipeline + push-constant set.
          vkCmdDrawIndexed(RI.primary.cmds[0].vk.cmd, (uint32_t)indexCount,
                           1u, 0u, 0, req.id);
        }
      }

      vkCmdEndRendering(RI.primary.cmds[0].vk.cmd);

      // pogo "read" half back to SHADER_READ_ONLY so the tail blit can sample it.
      {
        VkImageMemoryBarrier2 b =
            VK_RI_PogoShaderMemoryBarrier2(pogoReadImage, /*initial=*/false);
        VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers    = &b;
        vkCmdPipelineBarrier2(RI.primary.cmds[0].vk.cmd, &dep);
      }
    }
  }


  // Restore depth to DEPTH_ATTACHMENT_OPTIMAL before yielding the command
  // buffer: RI_VK_FillDepthAttachment hardcodes that layout, and depth ends
  // here in either SHADER_READ_ONLY_OPTIMAL (surfel-only) or
  // DEPTH_READ_ONLY_OPTIMAL (flipDepthToReadOnly ran for particle/decal).
  {
    const VkImageLayout currentLayout =
        depthFlippedForReadOnly ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL
                                : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const VkPipelineStageFlags2 srcStage =
        depthFlippedForReadOnly
            ? (VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
               VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT)
            : (VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    const VkAccessFlags2 srcAccess =
        depthFlippedForReadOnly
            ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT
            : (VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
               VK_ACCESS_2_SHADER_READ_BIT);

    VkImageMemoryBarrier2 restoreDepth = {
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    restoreDepth.srcStageMask = srcStage;
    restoreDepth.srcAccessMask = srcAccess;
    restoreDepth.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    restoreDepth.dstAccessMask =
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    restoreDepth.oldLayout = currentLayout;
    restoreDepth.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    restoreDepth.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    restoreDepth.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    restoreDepth.image = RI.depthTextures[RI.swapchainIndex].vk.image;
    restoreDepth.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};

    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &restoreDepth;
    vkCmdPipelineBarrier2(RI.primary.cmds[0].vk.cmd, &dep);
  }

  // Commit every bindless handle / slot-generation write made this frame. The
  // uploader records into its own transfer cmd buffer, flushed as a fenced
  // pre-pass the primary submit waits on (RIBootstrap), so this single tail call
  // lands all copies ahead of every primary read regardless of recording order.
  flushBindlessMirrors();
}

void cHybridRenderer::flushBindlessMirrors() {
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
    // the staged copy overwrites the slot; this is the m_diffuseObjectBuffer path.
    trans.vk.current_stage = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT |
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                             VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
    trans.vk.current_access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    trans.vk.post_stage = trans.vk.current_stage;
    trans.vk.post_access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    RI_ResourceBeginCopyBuffer(&RI.device, &RI.uploader, &trans);
    std::memcpy(trans.mapped.data, it.mir->shadow.data() + off, sz);
    RI_ResourceEndCopyBuffer(&RI.device, &RI.uploader, &trans);
    it.mir->clearDirty();
  }
}

cHybridRenderer::~cHybridRenderer() {
  // TLAS handle goes through the device's destroy; the storage buffer +
  // instance buffer share the deferred-free path via their shared_ptr deleter
  // (storage) and explicit queue here (instance). Caller guarantees the device
  // is idle by the time this fires.
  if (m_tlas.vk.handle != VK_NULL_HANDLE) {
    vkDestroyAccelerationStructureKHR(RI.device.vk.device, m_tlas.vk.handle,
                                      NULL);
    m_tlas = {};
  }
  if (m_tlasInstanceBuffer.vk.buffer) {
    vmaDestroyBuffer(RI.device.vk.vmaAllocator, m_tlasInstanceBuffer.vk.buffer,
                     m_tlasInstanceBuffer.vk.allocation);
    m_tlasInstanceBuffer = {};
  }
  if (m_pointLightBuffer.vk.buffer) {
    vmaDestroyBuffer(RI.device.vk.vmaAllocator, m_pointLightBuffer.vk.buffer,
                     m_pointLightBuffer.vk.allocation);
    m_pointLightBuffer = {};
  }
  if (m_lightGridCountBuffer.vk.buffer) {
    vmaDestroyBuffer(RI.device.vk.vmaAllocator, m_lightGridCountBuffer.vk.buffer,
                     m_lightGridCountBuffer.vk.allocation);
    m_lightGridCountBuffer = {};
  }
  if (m_lightGridListBuffer.vk.buffer) {
    vmaDestroyBuffer(RI.device.vk.vmaAllocator, m_lightGridListBuffer.vk.buffer,
                     m_lightGridListBuffer.vk.allocation);
    m_lightGridListBuffer = {};
  }
  if (m_spotLightBuffer.vk.buffer) {
    vmaDestroyBuffer(RI.device.vk.vmaAllocator, m_spotLightBuffer.vk.buffer,
                     m_spotLightBuffer.vk.allocation);
    m_spotLightBuffer = {};
  }
  if (m_boxLightBuffer.vk.buffer) {
    vmaDestroyBuffer(RI.device.vk.vmaAllocator, m_boxLightBuffer.vk.buffer,
                     m_boxLightBuffer.vk.allocation);
    m_boxLightBuffer = {};
  }
  if (m_fogAreaBuffer.vk.buffer) {
    vmaDestroyBuffer(RI.device.vk.vmaAllocator, m_fogAreaBuffer.vk.buffer,
                     m_fogAreaBuffer.vk.allocation);
    m_fogAreaBuffer = {};
  }
  if (m_decalBuffer.vk.buffer) {
    vmaDestroyBuffer(RI.device.vk.vmaAllocator, m_decalBuffer.vk.buffer,
                     m_decalBuffer.vk.allocation);
    m_decalBuffer = {};
  }
  {
    RIBuffer_s *fallbacks[] = {
        &m_translucentNormalFallback,
        &m_translucentTangentFallback,
        &m_translucentColorFallback,
        &m_translucentUv0Fallback,
    };
    for (RIBuffer_s *b : fallbacks) {
      if (b->vk.buffer) {
        vmaDestroyBuffer(RI.device.vk.vmaAllocator, b->vk.buffer,
                         b->vk.allocation);
        *b = {};
      }
    }
  }
  for (uint32_t i = 0; i < RI_MAX_SWAPCHAIN_IMAGES; ++i) {
    if (m_surfelResultView[i].vk.image != VK_NULL_HANDLE) {
      vkDestroyImageView(RI.device.vk.device, m_surfelResultView[i].vk.image,
                         NULL);
      m_surfelResultView[i] = {};
    }
    if (m_surfelResultTexture[i].vk.image != VK_NULL_HANDLE) {
      vmaDestroyImage(RI.device.vk.vmaAllocator, m_surfelResultTexture[i].vk.image, m_surfelResultTexture[i].vk.allocation);
      m_surfelResultTexture[i] = {};
    }
    if (m_packedHitInfoView[i].vk.image != VK_NULL_HANDLE) {
      vkDestroyImageView(RI.device.vk.device, m_packedHitInfoView[i].vk.image,
                         NULL);
      m_packedHitInfoView[i] = {};
    }
    if (m_packedHitInfoTexture[i].vk.image != VK_NULL_HANDLE) {
      vmaDestroyImage(RI.device.vk.vmaAllocator,
                      m_packedHitInfoTexture[i].vk.image,
                      m_packedHitInfoTexture[i].vk.allocation);
      m_packedHitInfoTexture[i] = {};
    }
    if (m_packedRefractionHitInfoView[i].vk.image != VK_NULL_HANDLE) {
      vkDestroyImageView(RI.device.vk.device,
                         m_packedRefractionHitInfoView[i].vk.image, NULL);
      m_packedRefractionHitInfoView[i] = {};
    }
    if (m_packedRefractionHitInfoTexture[i].vk.image != VK_NULL_HANDLE) {
      vmaDestroyImage(RI.device.vk.vmaAllocator,
                      m_packedRefractionHitInfoTexture[i].vk.image,
                      m_packedRefractionHitInfoTexture[i].vk.allocation);
      m_packedRefractionHitInfoTexture[i] = {};
    }
    if (m_packedReflectionHitInfoView[i].vk.image != VK_NULL_HANDLE) {
      vkDestroyImageView(RI.device.vk.device,
                         m_packedReflectionHitInfoView[i].vk.image, NULL);
      m_packedReflectionHitInfoView[i] = {};
    }
    if (m_packedReflectionHitInfoTexture[i].vk.image != VK_NULL_HANDLE) {
      vmaDestroyImage(RI.device.vk.vmaAllocator,
                      m_packedReflectionHitInfoTexture[i].vk.image,
                      m_packedReflectionHitInfoTexture[i].vk.allocation);
      m_packedReflectionHitInfoTexture[i] = {};
    }
    if (m_surfelIrradianceView[i].vk.image != VK_NULL_HANDLE) {
      vkDestroyImageView(RI.device.vk.device, m_surfelIrradianceView[i].vk.image, NULL);
      m_surfelIrradianceView[i] = {};
    }
    if (m_surfelIrradianceTexture[i].vk.image != VK_NULL_HANDLE) {
      vmaDestroyImage(RI.device.vk.vmaAllocator, m_surfelIrradianceTexture[i].vk.image, m_surfelIrradianceTexture[i].vk.allocation);
      m_surfelIrradianceTexture[i] = {};
    }
    if (m_surfelDepthView[i].vk.image != VK_NULL_HANDLE) {
      vkDestroyImageView(RI.device.vk.device, m_surfelDepthView[i].vk.image,
                         NULL);
      m_surfelDepthView[i] = {};
    }
    if (m_surfelDepthTexture[i].vk.image != VK_NULL_HANDLE) {
      vmaDestroyImage(RI.device.vk.vmaAllocator,
                      m_surfelDepthTexture[i].vk.image,
                      m_surfelDepthTexture[i].vk.allocation);
      m_surfelDepthTexture[i] = {};
    }
  }
  m_bindlessSet.destroy(&RI.device);
}

} // namespace hpl
