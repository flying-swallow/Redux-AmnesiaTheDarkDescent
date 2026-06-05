#include "graphics/HybridRenderer.h"
#include "graphics/RITypes.h"

#include "graphics/DebugDraw.h"
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
#include "graphics/RIViewportTarget.h"
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

// sRGB → linear transfer (IEC 61966-2-1). Inverse of the swapchain's write
// encode; brings artist-authored cColor.rgb into the shaders' linear lighting.
static inline float sRGBToLinear(float c) {
  if (c <= 0.04045f) return c / 12.92f;
  return std::pow((c + 0.055f) / 1.055f, 2.4f);
}

// CreateBindlessSlotBuffer moved to graphics/HybridGlobalManagedSet.h (shared
// with HybridGlobalManagedSet); still reachable here as
// detail::CreateBindlessSlotBuffer for the indirect-draw buffer below.

// Resolve a renderable's 5 fixed-function vertex streams (+ index) for the
// translucent-layout raster passes (translucent / water / decal), substitute
// the global single-vertex default buffers (RI.fallback*Vertex) for any absent
// optional stream, and bind them on `cmd`. Returns false — after a warning
// tagged with `passLabel` — when the mesh lacks position/index, so the caller
// can skip the renderable. On success *outPresentMask receives a bitset of
// eVertexElementFlag_* naming the streams supplied by real buffers; the caller
// passes it to the pipeline desc so an absent stream's binding stride is zeroed
// (the single-vertex fallback then feeds every vertex) and the variant gets its
// own cached pipeline.
static inline bool BindVertexStreams(struct RICmd_s *cmd, iVertexBuffer *pVB,
                                     const char *passLabel,
                                     uint32_t *outPresentMask) {
  auto *vbri = static_cast<VertexBuffer_RI *>(pVB);
  auto bufOf = [&](eVertexBufferElement type) -> RIBuffer_s * {
    const auto *element = vbri->GetElement(type);
    return (element && element->buffer) ? element->buffer.get() : nullptr;
  };
  // Position + index are the only truly required streams — without geometry
  // there's nothing to draw.
  RIBuffer_s *pos = bufOf(eVertexBufferElement_Position);
  const auto &idxRI = vbri->GetIndexRIBuffer();
  RIBuffer_s *idx = idxRI ? idxRI.get() : nullptr;
  if (!pos || !idx) {
    Warning("%s mesh missing position / index — skipping", passLabel);
    return false;
  }
  // Optional streams: bind the real buffer when present, else the global
  // single-vertex default (normal = +Z, tangent = +X/handedness, color = white,
  // uv = 0). No capacity limit — the pipeline zeroes the absent binding's
  // stride, so the one fallback element is reread for every vertex.
  RIBuffer_s *nrm = bufOf(eVertexBufferElement_Normal);
  RIBuffer_s *tan = bufOf(eVertexBufferElement_Texture1Tangent);
  RIBuffer_s *col = bufOf(eVertexBufferElement_Color0);
  RIBuffer_s *uv  = bufOf(eVertexBufferElement_Texture0);
  uint32_t mask = eVertexElementFlag_Position; // required, present per check above
  if (nrm) mask |= eVertexElementFlag_Normal;
  if (tan) mask |= eVertexElementFlag_Texture1;
  if (col) mask |= eVertexElementFlag_Color0;
  if (uv)  mask |= eVertexElementFlag_Texture0;
  if (outPresentMask)
    *outPresentMask = mask;
  RIBuffer_s *vertBufs[5] = {
      pos,
      nrm ? nrm : &RI.fallbackNormalVertex,
      tan ? tan : &RI.fallbackTangentVertex,
      col ? col : &RI.fallbackColorVertex,
      uv  ? uv  : &RI.fallbackUv0Vertex,
  };
  CmdBindVertexBuffers<5>(cmd, 0, 5, vertBufs);
  CmdBindIndexBuffer(cmd, idx, 0, VK_INDEX_TYPE_UINT32);
  return true;
}

} // namespace detail

// A renderable needs a BLAS only if it can become a TLAS instance — i.e. it's a
// mesh. Particles/billboards/beams/ropes/decals are never ray-traced (the TLAS
// gather skips them), so their per-frame BLAS build is dead work.
static bool renderableNeedsBlas(iRenderable *apObject) {
  return apObject && apObject->GetRenderType() == eRenderableType_SubMesh;
}

cHybridRenderer::cHybridRenderer(cGraphics *apGraphics, cResources *apResources)
    : iRenderer("Hybrid", apGraphics, apResources, 0) {
  {
    // Build the global bindless descriptor set (set 0) and create + seed every
    // buffer bound to it. All set-0 state now lives in m_global.
    m_global.initialize(&RI.device, mpResources);

    const VkDescriptorSetLayout externalLayouts[] = {
        m_global.m_bindlessSet.vk.m_bindlessSetLayout};
    {
      // Gbuffer pass: one .spv, two entry points (vsMain / psMain).
      auto gbuffer_bin = RIProgram::loadShaderStage(
          apResources->GetFileSearcher(), "SurfelGBuffer.3d.spv");
      std::array<RIProgram::ModuleStage, 2> stages = {
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, gbuffer_bin,
                                 "vsMain"},
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, gbuffer_bin,
                                 "psMain"}};
      m_gbuffer.initialize(&RI.device, stages, externalLayouts);
    }

    // SurfelVBuffer — one .spv, four entry points (rayGen / miss / closeHit /
    // anyHit) sharing a single blob.
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
    // Compute load that passes the Slang entry-point name through to ModuleStage.
    auto loadSlangCompute = [&](RIProgram &prog, const char *name,
                                const char *entryPoint) {
      auto bin = RIProgram::loadShaderStage(apResources->GetFileSearcher(), name);
      std::array<RIProgram::ModuleStage, 1> stages = {RIProgram::ModuleStage{
          RIProgram::PROGRAM_STAGE_COMPUTE, bin, entryPoint}};
      prog.initialize(&RI.device, stages, externalLayouts);
    };
    loadSlangCompute(m_surfelPrepare, "SurfelPreparePass.cs.spv", "csMain");
    // SurfelUpdatePass — one .spv, three entry points (collectCellInfo /
    // accumulateCellInfo / updateCellToSurfelBuffer). The blob vector must
    // outlive all three initialize() calls — ModuleStage holds a non-owning view.
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
    // SurfelRayTrace — one .spv, four entry points (rayGen / scatterMiss /
    // scatterCloseHit / scatterAnyHit). Shadow rays use inline RayQuery, so no
    // second hit group is needed (SBT stays single-ray-type).
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
    loadSlangCompute(m_surfelIntegrate,  "SurfelIntegratePass.cs.spv",   "csMain");
    loadSlangCompute(m_surfelGenerate,   "SurfelGenerationPass.cs.spv",  "csMain");
    // MainComposite — compute pass: one thread per pixel writes the composite
    // (albedo + inline decals + lighting) into the pogo attach bound as gOutput.
    // The renderer transitions the attach to GENERAL around the dispatch and back
    // to COLOR_ATTACHMENT_OPTIMAL afterwards.
    loadSlangCompute(m_mainComposite, "MainCompositePass.cs.spv", "csMain");
    loadSlangCompute(m_directLighting, "DirectLightingPass.cs.spv", "csMain");
    {
      // Particle pass (amnesia/slang/ParticlePass).
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
      // Decal pass (amnesia/slang/DecalPass). Reuses the translucent 5-stream
      // vertex layout + bindless/UBO layouts.
      auto d_vert = RIProgram::loadShaderStage(apResources->GetFileSearcher(),
                                               "Decal.vert.spv");
      auto d_frag = RIProgram::loadShaderStage(apResources->GetFileSearcher(),
                                               "Decal.frag.spv");
      std::array<RIProgram::ModuleStage, 2> stages = {
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, d_vert, "vsMain"},
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, d_frag, "psMain"}};
      m_decal.initialize(&RI.device, stages, externalLayouts);
    }
    {
      // Water pass (amnesia/slang/WaterPass). Reuses the translucent 5-stream
      // layout (TranslucentMeshPipelineDesc) + bindless/UBO layouts; draws the
      // water surface (tint + inline-RT lit reflection) over the refracted
      // background in two blend passes.
      auto w_vert = RIProgram::loadShaderStage(apResources->GetFileSearcher(),
                                               "Water.vert.spv");
      auto w_frag = RIProgram::loadShaderStage(apResources->GetFileSearcher(),
                                               "Water.frag.spv");
      std::array<RIProgram::ModuleStage, 2> stages = {
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, w_vert, "vsMain"},
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, w_frag, "psMain"}};
      m_water.initialize(&RI.device, stages, externalLayouts);
    }

    RISegmentAllocDesc_s indirectDesc = {};
    indirectDesc.numSegments = RI_NUMBER_FRAMES_FLIGHT;
    indirectDesc.elementStride = sizeof(VkDrawIndirectCommand);
    indirectDesc.maxElements = (uint16_t)kObjectSlotCapacity;
    m_indirectSegment = RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS>(&indirectDesc);
    m_indirectDrawBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, indirectDesc.maxElements, sizeof(VkDrawIndirectCommand),
        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    // Surfel-generation output image — one RGBA16F (HDR) storage texture per
    // swapchain image, full swapchain resolution. The generation pass dispatches
    // at viewportSize and writes every pixel, so the whole image is fresh each
    // frame.
    for (uint32_t i = 0; i < RI.swapchain.imageCount; ++i) {
      uint32_t queueFamilies[RI_QUEUE_LEN] = {0};
      VkImageCreateInfo imgInfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      imgInfo.imageType = VK_IMAGE_TYPE_2D;
      imgInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
      imgInfo.extent = {RI.renderWidth, RI.renderHeight, 1};
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

    // Packed visibility — RGBA32UI storage image written by the SurfelVBuffer RT
    // pipeline, sampled by the update / generation passes. Swapchain-sized.
    for (uint32_t i = 0; i < RI.swapchain.imageCount; ++i) {
      uint32_t queueFamilies[RI_QUEUE_LEN] = {0};
      VkImageCreateInfo imgInfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      imgInfo.imageType = VK_IMAGE_TYPE_2D;
      imgInfo.format = VK_FORMAT_R32G32B32A32_UINT;
      imgInfo.extent = {RI.renderWidth, RI.renderHeight, 1};
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

    // Velocity (motion vectors) — RG16F, the gbuffer's 2nd color target.
    // COLOR_ATTACHMENT for the write + SAMPLED so temporal passes can read it.
    for (uint32_t i = 0; i < RI.swapchain.imageCount; ++i) {
      uint32_t queueFamilies[RI_QUEUE_LEN] = {0};
      VkImageCreateInfo imgInfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      imgInfo.imageType = VK_IMAGE_TYPE_2D;
      imgInfo.format = VK_FORMAT_R16G16_SFLOAT;
      imgInfo.extent = {RI.renderWidth, RI.renderHeight, 1};
      imgInfo.mipLevels = 1;
      imgInfo.arrayLayers = 1;
      imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
      imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
      imgInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                      VK_IMAGE_USAGE_SAMPLED_BIT;
      imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      VK_ConfigureImageQueueFamilies(&imgInfo, RI.device.queues, RI_QUEUE_LEN,
                                     queueFamilies, RI_QUEUE_LEN);
      imgInfo.pQueueFamilyIndices = queueFamilies;

      VmaAllocationCreateInfo alloc = {};
      alloc.usage = VMA_MEMORY_USAGE_AUTO;
      VK_WrapResult(vmaCreateImage(RI.device.vk.vmaAllocator, &imgInfo, &alloc,
                                   &m_velocityTexture[i].vk.image,
                                   &m_velocityTexture[i].vk.allocation, NULL));

      VkImageViewCreateInfo viewInfo = {
          VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      viewInfo.image = m_velocityTexture[i].vk.image;
      viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
      viewInfo.format = VK_FORMAT_R16G16_SFLOAT;
      viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      VK_WrapResult(vkCreateImageView(RI.device.vk.device, &viewInfo, NULL,
                                      &m_velocityView[i].vk.image));
    }

    // Direct-lighting accumulation ping-pong — RGBA16F, STORAGE (compute write)
    // + SAMPLED (history reproject + composite read). Two textures, kept in
    // GENERAL; toggled per frame. Not swapchain-indexed (history spans frames).
    for (uint32_t i = 0; i < 2; ++i) {
      uint32_t queueFamilies[RI_QUEUE_LEN] = {0};
      VkImageCreateInfo imgInfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      imgInfo.imageType = VK_IMAGE_TYPE_2D;
      imgInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
      imgInfo.extent = {RI.renderWidth, RI.renderHeight, 1};
      imgInfo.mipLevels = 1;
      imgInfo.arrayLayers = 1;
      imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
      imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
      imgInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      VK_ConfigureImageQueueFamilies(&imgInfo, RI.device.queues, RI_QUEUE_LEN,
                                     queueFamilies, RI_QUEUE_LEN);
      imgInfo.pQueueFamilyIndices = queueFamilies;

      VmaAllocationCreateInfo alloc = {};
      alloc.usage = VMA_MEMORY_USAGE_AUTO;
      VK_WrapResult(vmaCreateImage(RI.device.vk.vmaAllocator, &imgInfo, &alloc,
                                   &m_directLightingTexture[i].vk.image,
                                   &m_directLightingTexture[i].vk.allocation,
                                   NULL));

      VkImageViewCreateInfo viewInfo = {
          VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      viewInfo.image = m_directLightingTexture[i].vk.image;
      viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
      viewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
      viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      VK_WrapResult(vkCreateImageView(RI.device.vk.device, &viewInfo, NULL,
                                      &m_directLightingView[i].vk.image));

      // Parallel surface-key texture (viewZ, normal.xyz) for disocclusion reject.
      VK_WrapResult(vmaCreateImage(RI.device.vk.vmaAllocator, &imgInfo, &alloc,
                                   &m_directKeyTexture[i].vk.image,
                                   &m_directKeyTexture[i].vk.allocation, NULL));
      viewInfo.image = m_directKeyTexture[i].vk.image;
      VK_WrapResult(vkCreateImageView(RI.device.vk.device, &viewInfo, NULL,
                                      &m_directKeyView[i].vk.image));
    }

    // (Per-bounce refraction/reflection V-buffers removed: water refraction now
    // clobbers the primary gPackedHitInfo like glass; water reflection is drawn
    // in the raster water pass.)

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

  }
}

void cHybridRenderer::Draw(RIBootstrap::FrameContext *cntx, cViewport *viewport,
                           float afFrameTime, cFrustum *apFrustum,
                           cWorld *apWorld, cRenderSettings *apSettings,
                           bool abSendFrameBufferToPostEffects) {

  // Offscreen viewport target (editor panes / previews): render at the
  // target's own 1:1 extent — no guard band — reusing the global overscan
  // intermediates (renderPogo / depth / visibility are all sized to the
  // overscan extent, which every pane fits inside), and deliver the finished
  // frame via the tail blit into the target's sampled texture instead of the
  // authored pogo crop.
  cViewportTarget *offTarget =
      viewport ? viewport->GetViewportTarget() : nullptr;
  if (offTarget && !offTarget->IsValid()) {
    offTarget = nullptr;
  }
  const uint32_t renderWidth =
      offTarget ? offTarget->GetWidth() : RI.renderWidth;
  const uint32_t renderHeight =
      offTarget ? offTarget->GetHeight() : RI.renderHeight;
  assert(renderWidth <= RI.renderWidth && renderHeight <= RI.renderHeight &&
         "offscreen target larger than the shared overscan intermediates");

  ml::float4x4 mainFrustumViewInvMat = apFrustum->GetViewMat();
  mainFrustumViewInvMat.Invert();
  const ml::float4x4 mainFrustumViewMat = apFrustum->GetViewMat();
  ml::float4x4 mainFrustumProjMat = apFrustum->GetProjectionMat();
  // Guard band: widen the FOV by the overscan factor so the cropped center keeps
  // the authored FOV. Scaling the projection's x/y focal terms (diagonal a[0],
  // a[5]) by 1/(1+2f) zooms out symmetrically about the centre. Done here so the
  // widened projection flows into perFrame.projMat, invProjMat, and m_prevProjMat
  // (velocity) — and cameraU/V are widened to match below.
  // Offscreen targets render 1:1 with no crop, so they keep the authored FOV.
  if (!offTarget) {
    const float gb = 1.0f + 2.0f * kGuardBandFraction;
    mainFrustumProjMat.a[0] /= gb;
    mainFrustumProjMat.a[5] /= gb;
  }
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
  // Per-frame prepare for every translucent renderable (particles + meshes +
  // billboards + beams). UpdateGraphicsForFrame/ForViewport recompute dynamic
  // geometry (billboard facing, beam stretch, emitter step) and mark the VB
  // dirty; SubmitToGPU then allocates/uploads dirty streams and rebuilds the
  // BLAS (no-op on repeat via the generation check). Done once here so the TLAS
  // build, particle pass, and mesh pass all consume already-prepared buffers.
  //
  // Must run BEFORE any vkCmdBeginRendering so the uploader's barriers and
  // BLAS-build cmds don't collide with a dynamic-rendering scope.
  for (iRenderable *pObj :
       m_rendererList.GetRenderableItems(eRenderListType_Translucent)) {
    if (!pObj)
      continue;
    pObj->UpdateGraphicsForFrame(afFrameTime);
    pObj->UpdateGraphicsForViewport(apFrustum, afFrameTime);
    iVertexBuffer *pVB = pObj->GetVertexBuffer();
    if (pVB) {
      auto *vbri = static_cast<VertexBuffer_RI *>(pVB);
      // Particles/billboards/beams/ropes are translucent but never TLAS
      // instances — upload their streams for the raster pass, skip the BLAS.
      vbri->SubmitToGPU(&RI.blasSubmit.cmds[0], &RI.device, cntx,
                        renderableNeedsBlas(pObj));
      vbri->AttachResourceToCntx(cntx);
    }
  }

  // Same prepare for decals (a separate list from Translucent). Their
  // Update*ForFrame already ran in AddObject, but SubmitToGPU (allocates
  // vk.buffer + uploads streams) is renderer-side and must run here, before any
  // vkCmdBeginRendering — else the decal pass hits the missing-position guard
  // and draws nothing.
  for (iRenderable *pObj :
       m_rendererList.GetRenderableItems(eRenderListType_Decal)) {
    if (!pObj)
      continue;

    iVertexBuffer *pVB = pObj->GetVertexBuffer();
    if (pVB) {
      auto *vbri = static_cast<VertexBuffer_RI *>(pVB);
      // Decals are never TLAS instances — upload streams, skip the BLAS.
      vbri->SubmitToGPU(&RI.blasSubmit.cmds[0], &RI.device, cntx,
                        /*abBuildBlas=*/false);
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
  // Previous-frame view/proj for motion vectors (gbuffer velocity target). On
  // the first frame use this frame's matrices so velocity reads ~zero, then
  // remember this frame's for the next.
  if (!m_hasPrevCamera) {
    std::memcpy(m_prevViewMat, mainFrustumViewMat.a, sizeof(m_prevViewMat));
    std::memcpy(m_prevProjMat, mainFrustumProjMat.a, sizeof(m_prevProjMat));
    m_hasPrevCamera = true;
  }
  std::memcpy(perFrame.prevViewMat, m_prevViewMat, sizeof(perFrame.prevViewMat));
  std::memcpy(perFrame.prevProjMat, m_prevProjMat, sizeof(perFrame.prevProjMat));
  std::memcpy(m_prevViewMat, mainFrustumViewMat.a, sizeof(m_prevViewMat));
  std::memcpy(m_prevProjMat, mainFrustumProjMat.a, sizeof(m_prevProjMat));
  // viewProjMat = proj * view (column-major); fill via direct ml composition
  // when needed. Leaving as identity-stub for now — first pass writes only
  // visibility; lighting in the FS reads viewMat/invViewMat which are correct.
  perFrame.viewportSize[0] = (float)renderWidth;
  perFrame.viewportSize[1] = (float)renderHeight;
  perFrame.viewTexel[0] =
      renderWidth ? 1.0f / (float)renderWidth : 0.0f;
  perFrame.viewTexel[1] =
      renderHeight ? 1.0f / (float)renderHeight : 0.0f;
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

  // Pinhole camera basis from the view-inverse rows (camera world-space
  // right/up/back/origin). The -matrix-layout-column-major slangc flag makes
  // these offsets line up with the shader's column-vector math.
  {
    const float *invV = mainFrustumViewInvMat.a;
    const hpl::float3 rightW{invV[0], invV[1], invV[2]};
    const hpl::float3 upW{invV[4], invV[5], invV[6]};
    const hpl::float3 backW{invV[8], invV[9], invV[10]};
    const hpl::float3 posW{invV[12], invV[13], invV[14]};

    const float aspect = apFrustum->GetAspect();
    const float tanHalfFov = std::tan(0.5f * apFrustum->GetFOV());
    constexpr float focalLength = 1.0f;
    // Guard band: widen the ray cone by (1+2f) to match the widened projMat above,
    // so the RT primary rays + velocity cover the overscan frame.
    const float gb = 1.0f + 2.0f * kGuardBandFraction;
    const float uScale = gb * focalLength * tanHalfFov * aspect;
    const float vScale = gb * focalLength * tanHalfFov;

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
    pl.intensity = authored  * kPointLightIntensityScale;
    // Precompute the light-grid bin reach here so LightGridBuildPass (one thread
    // per cell, looping all lights) doesn't recompute it per (cell,light). reach²
    // = maxChannel(color)·intensity / floor − sourceRadius²; ≤0 ⇒ too dim to bin.
    // The reach uses the UNSCALED authored intensity (kPointLightIntensityScale
    // omitted): the scale is a visual brightness knob and must NOT re-cull lights
    // — folding it into the reach made dimming shrink the bin range and pop lights
    // out. Binning stays tied to the artist's authored radius; brightness only
    // scales the shader radiance above.
    {
      const float maxC = std::max(pl.color[0], std::max(pl.color[1], pl.color[2]));
      const float reachSq = ((maxC * authored) / kLightRadianceFloor) - kPointLightSourceRadiusSq;
      const float calculatedReach = reachSq > 0.f ? std::sqrt(reachSq) : 0.f;
      pl.radius = calculatedReach;
    }
    // Physical source radius drives the soft-shadow penumbra in the direct pass.
    // Authored per-light via cLight::SetSourceRadius; when a light authors none
    // (0), fall back to a fraction of its authored reach radius so it's softly
    // shadowed by default instead of hard. An explicit author value always wins.
    const float authoredSourceRadius = pLight->GetSourceRadius();
    pl.sourceRadius = authoredSourceRadius > 0.f
                          ? authoredSourceRadius
                          : authored * kPointLightDefaultSourceRadiusFrac;
    pl.goboTextureIndex = m_global.resolveCubeTextureSlot(
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
    m_global.m_pointLightScratch[num_point_lights++] = pl;
  }
  perFrame.pointLightCount = static_cast<uint32_t>(num_point_lights);

  if (num_point_lights > 0) {
    const size_t uploadBytes = num_point_lights * sizeof(PointLight);
    RIResourceBufferTransaction_s trans = {};
    trans.target = m_global.m_pointLightBuffer;
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
    std::memcpy(trans.mapped.data, m_global.m_pointLightScratch.data(), uploadBytes);
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
    sl.intensity = authored  * kPointLightIntensityScale;
    // Precompute the light-grid bin reach (same as point lights) so the per-cell
    // gather in LightGridBuildPass just reads it. The spot cone ⊂ the radius
    // sphere, so the radial reach is a conservative bound. Uses the UNSCALED
    // authored intensity (kPointLightIntensityScale omitted) so the brightness
    // knob doesn't re-cull lights — see the point-light note above.
    {
      const float maxC = std::max(sl.color[0], std::max(sl.color[1], sl.color[2]));
      const float reachSq =
          ((maxC * authored)/ kLightRadianceFloor) - kPointLightSourceRadiusSq;
      sl.radius = reachSq > 0.f ? std::sqrt(reachSq) : 0.f;
    }
    // Physical source radius drives the soft-shadow penumbra in the direct pass
    // (same as point lights above). Authored per-light via cLight::SetSourceRadius;
    // when a light authors none (0), fall back to a fraction of its authored reach
    // so it's softly shadowed by default. An explicit author value always wins.
    {
      const float authoredSourceRadius = pLight->GetSourceRadius();
      sl.sourceRadius = authoredSourceRadius > 0.f
                            ? authoredSourceRadius
                            : authored * kPointLightDefaultSourceRadiusFrac;
    }
    sl.goboTextureIndex = m_global.resolveTextureSlot(cntx, pLight->GetGoboImage(),
                                             (uint32_t)RI.frameIndex);
    sl.shadowEnabled = pLight->GetCastShadows() ? 1u : 0u;
    // Light-space ViewProj for projecting gobo UVs (and any future shadow UV)
    // into the cone. Transposed to match the GLSL mat4 column-major upload.
    const ml::float4x4 vpF4 =
        cMath::ToFloatTranspose4x4(pSpot->GetViewProjMatrix());
    std::memcpy(sl.viewProjection, vpF4.a, sizeof(sl.viewProjection));
    m_global.m_spotLightScratch[num_spot_lights++] = sl;
  }
  perFrame.spotLightCount = static_cast<uint32_t>(num_spot_lights);

  if (num_spot_lights > 0) {
    const size_t uploadBytes = num_spot_lights * sizeof(SpotLight);
    RIResourceBufferTransaction_s trans = {};
    trans.target = m_global.m_spotLightBuffer;
    trans.size = uploadBytes;
    trans.offset = 0;
    trans.vk.current_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    trans.vk.current_access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    trans.vk.post_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    trans.vk.post_access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    RI_ResourceBeginCopyBuffer(&RI.device, &RI.uploader, &trans);
    std::memcpy(trans.mapped.data, m_global.m_spotLightScratch.data(), uploadBytes);
    RI_ResourceEndCopyBuffer(&RI.device, &RI.uploader, &trans);
  }

  // Box lights are not represented in this renderer (cLightBox stays an engine
  // entity for map loading / the legacy deferred backend, but contributes no
  // hybrid GPU lighting).

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
    m_global.m_fogAreaScratch[num_fog_areas++] = fa;
  }
  perFrame.fogAreaCount = static_cast<uint32_t>(num_fog_areas);

  if (num_fog_areas > 0) {
    const size_t uploadBytes = num_fog_areas * sizeof(FogAreaParams);
    RIResourceBufferTransaction_s trans = {};
    trans.target = m_global.m_fogAreaBuffer;
    trans.size = uploadBytes;
    trans.offset = 0;
    trans.vk.current_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    trans.vk.current_access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    trans.vk.post_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    trans.vk.post_access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    RI_ResourceBeginCopyBuffer(&RI.device, &RI.uploader, &trans);
    std::memcpy(trans.mapped.data, m_global.m_fogAreaScratch.data(), uploadBytes);
    RI_ResourceEndCopyBuffer(&RI.device, &RI.uploader, &trans);
  }

  // Clustered OOB decals — one GpuDecal per cDecal, uploaded in stable world
  // order (cWorld::GetDecals) so the per-object association in gObjectDecalIndices
  // (built by cWorld::Compile) indexes gDecals[] directly. ALL decals upload each
  // frame (the static set, ≤kMaxDecals) — not the visible subset — so those
  // indices stay valid; per-frame upload also keeps materialID fresh. One slot per
  // world decal, never skipped, to preserve the stable index ↔ gDecals[] mapping.
  size_t num_decals = 0;
  for (cDecal *pDecal : apWorld->GetDecals()) {
    if (num_decals >= kMaxDecals) {
      Warning("Decal capacity exhausted; dropping remaining decals");
      break;
    }
    cMaterial *pMat = pDecal ? pDecal->GetMaterial() : nullptr;

    uint32_t materialId =
        pMat ? m_global.submitMaterial(cntx, pMat, (uint32_t)RI.frameIndex).materialId
             : 0u;
    if (materialId == UINT32_MAX) {
      Warning("Decal material slot exhausted");
      materialId = 0;
    }

    GpuDecal d{};
    cMatrixf *pMtx = pDecal->GetModelMatrix(apFrustum);
    const cMatrixf wm = pMtx ? *pMtx : cMatrixf::Identity;
    ml::float4x4 invF4 = cMath::ToFloatTranspose4x4(wm);
    invF4.Invert();
    std::memcpy(d.invModelMat, invF4.a, sizeof(d.invModelMat));

    // World-space projection axis = the decal box's local +Y in world space
    // (the editor projects along this; DecalCreator's per-triangle backface cull
    // uses it). The box maps the unit cube via wm, so its up basis column is the
    // axis; normalize out the box scale.
    cVector3f up = wm.GetUp();
    const float upLen = up.Length();
    up = upLen > 1e-6f ? up / upLen : cVector3f(0, 1, 0);
    d.projAxisWS = float3{up.x, up.y, up.z};
    const cColor c = pDecal->GetDecalColor();
    d.color = float4{c.r, c.g, c.b, c.a};
    d.materialID = materialId;
    d.receiverMask = (uint32_t)pDecal->GetReceiverMask();
    d.blendMode = pMat ? (uint32_t)pMat->GetBlendMode() : 0u;
    const cVector2l sd = pDecal->GetSubDiv();
    d.subDivX = (uint32_t)sd.x;
    d.subDivY = (uint32_t)sd.y;
    d.subDivIndex = (uint32_t)pDecal->GetCurrentSubDiv();
    m_global.m_decalScratch[num_decals++] = d;
  }
  perFrame.decalCount = static_cast<uint32_t>(num_decals);

  if (num_decals > 0) {
    const size_t uploadBytes = num_decals * sizeof(GpuDecal);
    RIResourceBufferTransaction_s trans = {};
    trans.target = m_global.m_decalBuffer;
    trans.size = uploadBytes;
    trans.offset = 0;
    trans.vk.current_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    trans.vk.current_access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    trans.vk.post_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    trans.vk.post_access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    RI_ResourceBeginCopyBuffer(&RI.device, &RI.uploader, &trans);
    std::memcpy(trans.mapped.data, m_global.m_decalScratch.data(), uploadBytes);
    RI_ResourceEndCopyBuffer(&RI.device, &RI.uploader, &trans);
  }

  // Per-object decal-index pool (cWorld::Compile static association → frag
  // gObjectDecalIndices). Static per world; uploaded each frame for simplicity.
  {
    const std::vector<uint32_t> &pool = apWorld->GetDecalObjectIndices();
    const size_t poolCount =
        std::min(pool.size(), (size_t)kMaxObjectDecalIndices);
    if (poolCount > 0) {
      const size_t uploadBytes = poolCount * sizeof(uint32_t);
      RIResourceBufferTransaction_s trans = {};
      trans.target = m_global.m_objectDecalIndexBuffer;
      trans.size = uploadBytes;
      trans.offset = 0;
      trans.vk.current_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
      trans.vk.current_access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
      trans.vk.post_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
      trans.vk.post_access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
      RI_ResourceBeginCopyBuffer(&RI.device, &RI.uploader, &trans);
      std::memcpy(trans.mapped.data, pool.data(), uploadBytes);
      RI_ResourceEndCopyBuffer(&RI.device, &RI.uploader, &trans);
    }
  }

  for (iRenderable *pObject : solids) {
    cMatrixf *pMtx = pObject->GetModelMatrix(apFrustum);
    iVertexBuffer *pVB = pObject->GetVertexBuffer();
    cMaterial *pMat = pObject->GetMaterial();
    if (!pVB || !pMat)
      continue;

    
    uint32_t materialId = 0;
    if (pMat) {
      materialId =
          m_global.submitMaterial(cntx, pMat, (uint32_t)RI.frameIndex).materialId;
      if (materialId == UINT32_MAX) {
        Warning("Material Slot exhausted");
        materialId = 0;
      }
    }

    ObjectSubmitDesc d;          // opaque solids: identity uv
    d.modelMatrix = pMtx;
    d.materialId = materialId;
    d.dissolveAmount = pObject->GetCoverageAmount();
    d.illuminationAmount = pObject->GetIlluminationAmount();
    // Precomputed static decal list (cWorld::Compile): (offset<<8)|count into
    // gObjectDecalIndices. Dynamic objects keep the default (0,0) → no decals,
    // so a movable object can't receive a decal it merely passes through.
    {
      const uint32_t off = (uint32_t)pObject->GetDecalListOffset();
      const uint32_t cnt = (uint32_t)pObject->GetDecalListCount();
      d.decalList = (off << 8) | (cnt & 0xFFu);
    }
    auto *vb = static_cast<VertexBuffer_RI *>(pVB);
    // Upload the VB geometry first so submitObject sees the post-realloc index
    // count — an in-loop realloc that changes the triangle count must bump the
    // slot generation this same frame (anchored surfels with a now-out-of-range
    // primitiveIndex go stale before the OOB deref).
    vb->SubmitToGPU(&RI.blasSubmit.cmds[0], &RI.device, cntx);
    vb->AttachResourceToCntx(cntx);

    // Stable object slot, keyed on the renderable's unique cookie (NOT its
    // transform — a moving object keeps its slot, and its surfels follow via
    // object-space anchoring + the per-frame modelMat upload). submitObject owns
    // the request, the slot-generation bump (new occupant / index-count change /
    // VB destroy), and the payload upload.
    // kSubmitVertex|kSubmitIndex: submitObject also fans the VB's per-stream BDAs
    // into the slot's opaque*Handles for bindless pulling (gbuffer VS / surfel-RT
    // chit), rewritten every frame so a SubmitToGPU realloc can't dangle them.
    const uint32_t slot = m_global.submitObject(
        pObject->GetUniqueCookie(), (uint32_t)RI.frameIndex, vb, d,
        kSubmitData | kSubmitVertex | kSubmitIndex);
    if (slot == UINT32_MAX) {
      Error("bindless pool is exhausted");
      continue;
    }

    // Transposed model matrix for the TLAS instance transform below.
    const ml::float4x4 modelF4 =
        cMath::ToFloatTranspose4x4(pMtx ? *pMtx : cMatrixf::Identity);

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
          /*firstInstance =*/slot,
      };
    }

    // BLAS was recorded by SubmitToGPU above into the same primary cmd buffer;
    // the accel-build→accel-build barrier below guarantees the TLAS read sees
    // the BLAS writes.
    auto blas = vb->accelStructure();
    if (blas && blas->vk.handle != VK_NULL_HANDLE) {
      // VkAccelerationStructureInstanceKHR::transform is row-major 3x4
      // (matrix[row][col]), translation at matrix[r][3]. modelF4 holds
      // column-major storage (GLSL mat4 reading in gbuffer.vert), so index it
      // as [col*4 + row] to extract entries row-by-row.
      VkAccelerationStructureInstanceKHR inst = {};
      for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
          inst.transform.matrix[r][c] = modelF4.a[c * 4 + r];
        }
      }
      inst.instanceCustomIndex = slot;
      inst.mask = kRayMaskOpaque;
      inst.instanceShaderBindingTableRecordOffset = 0;
      // FLIP_FACING, not CULL_DISABLE: this engine's content + raster chain is
      // clockwise-front (GBufferMRTPipelineDesc frontFace=CLOCKWISE + the
      // negative-height viewport), while VK RT defines front-facing as
      // counter-clockwise from the ray origin. Flipping the per-instance
      // facing makes the shaders' RAY_FLAG_CULL_BACK_FACING_TRIANGLES cull the
      // same side raster culls. The previous blanket CULL_DISABLE neutralized
      // backface culling entirely, so "double-sided by sandwiching" assets
      // (e.g. banner_long01: two exactly-coplanar quads with opposed winding)
      // hit BOTH sheets at identical t — the closest-hit tie broke per-pixel
      // and painted concentric z-fighting rings into the V-buffer.
      inst.flags = RI_ACCEL_INSTANCE_TRIANGLE_FLIP_FACING;
      // Solid meshes (no alpha-cutout texture) never alpha-reject a ray, so mark
      // them FORCE_OPAQUE: the driver auto-commits the first hit and skips the
      // RayQuery Proceed()/alphaTest loop entirely — a universal speedup for
      // shadow / primary V-buffer / surfel rays. Alpha-cutout meshes (foliage,
      // grates — they carry an alpha texture) stay non-opaque so their
      // see-through shadows / hits are preserved.
      //
      // FORCE_OPAQUE also skips the anyhit/RayQuery alphaTest (which now
      // carries the CoverageAmount dissolve for visibility, shadows, and GI
      // alike), so anything that must dissolve-test a ray has to stay
      // non-opaque: an in-progress fade, and materials whose dissolve
      // variants run even at full coverage (DissolveAlpha map /
      // AlphaDissolveFilter — dithered alpha edges). The instance list is
      // rebuilt every frame, so a finished fade returns to the opaque fast
      // path next frame.
      const ShaderMaterialData &solidDesc = pMat->Descriptor();
      const bool dissolveFlags =
          pMat->GetImage(eMaterialTexture_DissolveAlpha) ||
          (solidDesc.m_id == MaterialID::SolidDiffuse &&
           solidDesc.m_solid.m_alphaDissolveFilter);
      if (!pMat->GetImage(eMaterialTexture_Alpha) &&
          pObject->GetCoverageAmount() >= 1.0f && !dissolveFlags)
        inst.flags |= RI_ACCEL_INSTANCE_FORCE_OPAQUE;
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
  // cube-map texture (matching the shader's isReflective() check on
  // cubeMapTextureIndex). Plain
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
  // UniformObject payloads (rasterised draws need uvMat, dissolveAmount,
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

    auto mat = m_global.submitMaterial(cntx, pMat, (uint32_t)RI.frameIndex);
    if (mat.materialId == UINT32_MAX)
      continue;

    cMatrixf *pMtx = pObj->GetModelMatrix(apFrustum);
    ObjectSubmitDesc d;          // identity uv; dissolve/illum 0
    d.modelMatrix = pMtx;
    d.materialId = mat.materialId; // water ids fall in the water range of materialID

    // Salted cookie keeps this slot disjoint from the translucent mesh pass's
    // slot for the same renderable (a refractive mesh occupies both). Stable per
    // object — no transform/frame term.
    const hash_t cookie = hash_u32(
        hash_u64(HASH_INITIAL_VALUE, pObj->GetUniqueCookie()),
        0x71A57AA5u);
    const uint32_t slot =
        m_global.submitObject(cookie, (uint32_t)RI.frameIndex, vbri, d,
                              kSubmitData | kSubmitVertex | kSubmitIndex);
    if (slot == UINT32_MAX)
      continue;

    // Transposed model matrix for the TLAS instance (row-major 3x4).
    const ml::float4x4 modelF4 =
        cMath::ToFloatTranspose4x4(pMtx ? *pMtx : cMatrixf::Identity);
    VkAccelerationStructureInstanceKHR inst = {};
    for (int r = 0; r < 3; ++r) {
      for (int c = 0; c < 4; ++c) {
        inst.transform.matrix[r][c] = modelF4.a[c * 4 + r];
      }
    }
    inst.instanceCustomIndex = slot;
    inst.mask = kRayMaskTranslucent;
    inst.instanceShaderBindingTableRecordOffset = 0;
    // FLIP_FACING to match the raster clockwise-front convention — see the
    // solids loop above for the full rationale (coplanar sandwich z-fighting).
    inst.flags = RI_ACCEL_INSTANCE_TRIANGLE_FLIP_FACING;
    assert(blas->vk.deviceAddress != 0);
    inst.accelerationStructureReference = blas->vk.deviceAddress;
    tlasInstances.push_back(inst);
  }

  // ---------- TLAS build ----------
  // Walks the BLAS instances accumulated above and emits one TLAS build into
  // the primary cmd buffer. Also runs once with zero instances when no TLAS
  // exists yet (empty editor world): every RT descriptor push below requires
  // a valid handle, and rays into an empty TLAS just miss.
  if (!tlasInstances.empty() || m_tlas.vk.handle == VK_NULL_HANDLE) {
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
    // frames-in-flight roll. Keep at least one element so the empty-TLAS
    // build still has a valid instance-buffer device address.
    const uint32_t instanceCapacityNeeded = std::max(instanceCount, 1u);
    if (instanceCapacityNeeded > m_tlasCapacity) {
      uint32_t newCap = m_tlasCapacity ? m_tlasCapacity : 256;
      while (newCap < instanceCapacityNeeded)
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
    if (instanceCount > 0) {
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
      // Consumed by both the RT pipelines and fragment-stage ray queries —
      // one barrier covers both.
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
  // / m_surfelIntegrate / m_surfelGenerate / m_mainComposite call sites).
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
    VkImageMemoryBarrier2 attachmentBarriers[3] = {
        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2},
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

    // Velocity MRT — same UNDEFINED→COLOR transition as the visibility target
    // (loadOp=CLEAR, so prior contents don't matter).
    VkImageMemoryBarrier2 &toVelocity = attachmentBarriers[2];
    toVelocity.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    toVelocity.srcAccessMask = 0;
    toVelocity.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    toVelocity.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    toVelocity.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toVelocity.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toVelocity.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toVelocity.image = m_velocityTexture[RI.swapchainIndex].vk.image;

    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 3;
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

  // Velocity MRT (SV_TARGET1). Cleared to 0 → static/uncovered pixels read zero
  // motion.
  VkRenderingAttachmentInfo velocityAttachment = {
      VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  velocityAttachment.imageView = m_velocityView[RI.swapchainIndex].vk.image;
  velocityAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  velocityAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  velocityAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  velocityAttachment.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};

  const VkRenderingAttachmentInfo gbufferColorAttachments[2] = {
      colorAttachment, velocityAttachment};

  VkRenderingAttachmentInfo depthAttachment = {
      VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  // MRT owns the per-frame depth clear.
  RI_VK_FillDepthAttachment(&depthAttachment, &RI.depthView[RI.swapchainIndex],
                            /*attachAndClear=*/true);

  // ----------------------------------------------------------------------
  // Surfel prepare + cell/ref-counter clear. Runs first each frame: promote
  // the previous frame's ValidSurfel count to DirtySurfel, zero the per-frame
  // counters, and clear the cellInfo / surfelReservation / surfelRefCounter
  // buffers so the update pass accumulates from scratch.
  // ----------------------------------------------------------------------
  {
    VkComputePipelineCreateInfo computeCreate = {
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    m_surfelPrepare.bindComputePipeline(&RI.device, &RI.primary.cmds[0],
                                        kHash, "SurfelPreparePass.cs",
                                        &computeCreate);
    m_surfelPrepare.bindBindlessDescriptorSet(
        &RI.primary.cmds[0], &m_global.m_bindlessSet, 0,
        VK_PIPELINE_BIND_POINT_COMPUTE);
    CmdDispatch(&RI.primary.cmds[0], 1u, 1u, 1u);
  }

  // Ping-pong the surfel-index buffers: copy this frame's previously-valid
  // indices into the dirty buffer so update_collect can walk them. A straight
  // copy each frame — negligible cost (≤600 KB).
  //
  // The source was last written by the previous frame, already ordered by the
  // frame fence, so no pre-copy barrier is needed. The post-copy barrier
  // (with the prepare-pass writes above) covers the copy's TRANSFER_WRITE and
  // prepare's SHADER_WRITE against update_collect's SHADER_READ.
  {
    VkBufferCopy region = {};
    region.srcOffset = 0;
    region.dstOffset = 0;
    region.size = (VkDeviceSize)kTotalSurfelLimit * sizeof(uint32_t);
    vkCmdCopyBuffer(cmd,
                    m_global.m_surfelValidBuffer.vk.buffer,
                    m_global.m_surfelDirtyIndexBuffer.vk.buffer,
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
    // Primary V-buffer: UNDEFINED -> GENERAL for the RT pipeline to store into.
    // (No bounce buffers to clear anymore — water/glass refraction clobbers this
    // primary image, water reflection is in the raster water pass.)
    VkImageMemoryBarrier2 toGeneral = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    toGeneral.srcStageMask  = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    toGeneral.srcAccessMask = 0;
    toGeneral.dstStageMask  = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
    toGeneral.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    toGeneral.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toGeneral.image = m_packedHitInfoTexture[RI.swapchainIndex].vk.image;
    toGeneral.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &toGeneral;
    vkCmdPipelineBarrier2(cmd, &dep);
  }

  if (m_tlas.vk.handle != VK_NULL_HANDLE) {
    VkRayTracingPipelineCreateInfoKHR rtCreate = {
        VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
    // closeHit fires one recursive TraceRay before recording the second hit,
    // so the pipeline needs depth=2 (raygen→chit = depth 1, the recursive
    // TraceRay lands at depth 2). depth=1 silently leaves hit pixels unwritten
    // — "holes" in gPackedHitInfo against the UNDEFINED initial contents.
    rtCreate.maxPipelineRayRecursionDepth = 2;
    const hash_t kVBufferHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    m_surfelVBuffer.bindRayTracingPipeline(&RI.device, &RI.primary.cmds[0],
                                           kVBufferHash, "SurfelVBuffer.rt",
                                           &rtCreate);
    m_surfelVBuffer.bindBindlessDescriptorSet(
        &RI.primary.cmds[0], &m_global.m_bindlessSet, 0,
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
    pushTlas(vbBindings);

    m_surfelVBuffer.bindDescriptors(
        &RI.device, &RI.primary.cmds[0], RI.frameIndex, vbBindings.data(),
        vbBindings.size(), VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR);

    m_surfelVBuffer.traceRays(&RI.primary.cmds[0], kVBufferHash,
                              renderWidth, renderHeight, 1u);
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
        &RI.primary.cmds[0], &m_global.m_bindlessSet, 0,
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
        &RI.primary.cmds[0], &m_global.m_bindlessSet, 0,
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
        &RI.primary.cmds[0], &m_global.m_bindlessSet, 0,
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
  // sampling + the MainCompositePass direct cull). Per-cell gather: one thread
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
    m_lightGridBin.bindBindlessDescriptorSet(&RI.primary.cmds[0], &m_global.m_bindlessSet,
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

  {
    // binLights writes are read by two consumers later this frame: the surfel
    // ray-trace NEE (ray tracing) and the MainCompositePass direct-lighting
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
        &RI.primary.cmds[0], &m_global.m_bindlessSet, 0,
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
                              {renderWidth, renderHeight}};
  renderingInfo.layerCount = 1;
  renderingInfo.colorAttachmentCount = 2;
  renderingInfo.pColorAttachments = gbufferColorAttachments;
  renderingInfo.pDepthAttachment = &depthAttachment;

  vkCmdBeginRendering(cmd, &renderingInfo);

  VkViewport vkViewport = {0,
                           (float)renderHeight,
                           (float)renderWidth,
                           -(float)renderHeight,
                           0.0f,
                           1.0f};
  VkRect2D scissor = {{0, 0}, {renderWidth, renderHeight}};
  vkCmdSetViewport(cmd, 0, 1, &vkViewport);
  vkCmdSetScissor(cmd, 0, 1, &scissor);

  if (writtenDraws > 0) {
    GBufferMRTPipelineDesc pipelineDesc(RIBootstrap::VisibilityFormat,
                                        RIBootstrap::VelocityFormat,
                                        RIBootstrap::DepthFormat);
    m_gbuffer.bindPipeline(&RI.device, &RI.primary.cmds[0], pipelineDesc.hash,
                           "SurfelGBuffer.3d", &pipelineDesc.createInfo);
    m_gbuffer.bindBindlessDescriptorSet(&RI.primary.cmds[0], &m_global.m_bindlessSet, 0);
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
    VkImageMemoryBarrier2 toRead[4] = {
        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2},
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
    // (MainCompositePass) samples gIndirectLighting for EVERY valid-hit
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

    // Velocity (gbuffer MRT) -> SHADER_READ for the direct-lighting pass.
    toRead[3].srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    toRead[3].srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    toRead[3].dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    toRead[3].dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    toRead[3].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toRead[3].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toRead[3].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toRead[3].image = m_velocityTexture[RI.swapchainIndex].vk.image;

    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 4;
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
  // Surfel integrate + generation.
  //   integrate: per valid surfel, MSME-blend the per-frame raytraced radiance
  //              (gSurfelRayResultBuffer) into surfel.radiance.
  //   generate:  per pixel, walk the visibility cell's surfel list, write the
  //              indirect term into m_surfelResultTexture (sampled by the
  //              composite), and spawn / recycle surfels by coverage.
  // ----------------------------------------------------------------------
  {
    VkComputePipelineCreateInfo computeCreate = {
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    m_surfelIntegrate.bindComputePipeline(&RI.device, &RI.primary.cmds[0],
                                          kHash, "SurfelIntegratePass.cs",
                                          &computeCreate);
    m_surfelIntegrate.bindBindlessDescriptorSet(
        &RI.primary.cmds[0], &m_global.m_bindlessSet, 0,
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
        &RI.primary.cmds[0], &m_global.m_bindlessSet, 0,
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

    const uint32_t fullW = renderWidth;
    const uint32_t fullH = renderHeight;
    CmdDispatch(&RI.primary.cmds[0], (fullW + 15u) / 16u,
                (fullH + 15u) / 16u, 1u);
  }

  // --------------------------------------------------------------------
  // Direct-lighting pass — soft-shadowed analytic direct lighting, temporally
  // accumulated through the velocity texture, into the ping-pong direct texture
  // the composite samples. The V-buffer (gPackedHitInfo), raster fallback, and
  // velocity are all ready by here.
  // --------------------------------------------------------------------
  {
    const uint32_t dlCur  = m_directLightingIndex;
    const uint32_t dlPrev = dlCur ^ 1u;

    if (!m_directLightingInit) {
      // First use: the colour + key ping-pong textures UNDEFINED -> GENERAL +
      // cleared so the history reads are defined; they stay GENERAL thereafter.
      VkImageMemoryBarrier2 toGen[4] = {
          {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2},
          {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2},
          {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2},
          {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2}};
      VkImage dirImgs[4] = {
          m_directLightingTexture[0].vk.image, m_directLightingTexture[1].vk.image,
          m_directKeyTexture[0].vk.image,      m_directKeyTexture[1].vk.image};
      for (uint32_t i = 0; i < 4; ++i) {
        toGen[i].srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        toGen[i].dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
        toGen[i].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        toGen[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toGen[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        toGen[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        toGen[i].image = dirImgs[i];
      }
      VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      dep.imageMemoryBarrierCount = 4;
      dep.pImageMemoryBarriers = toGen;
      vkCmdPipelineBarrier2(cmd, &dep);

      VkClearColorValue clr = {};
      VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      for (uint32_t i = 0; i < 4; ++i)
        vkCmdClearColorImage(cmd, dirImgs[i], VK_IMAGE_LAYOUT_GENERAL, &clr, 1,
                             &range);

      VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
      mb.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
      mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
      mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
      mb.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
                         VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
      VkDependencyInfo dep2 = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      dep2.memoryBarrierCount = 1;
      dep2.pMemoryBarriers = &mb;
      vkCmdPipelineBarrier2(cmd, &dep2);
      m_directLightingInit = true;
    } else {
      // Make last frame's writes to the ping-pong textures visible (history
      // sampled-read + current write-after-read/write). Both stay GENERAL.
      VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
      mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
      mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
      mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
      mb.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
                         VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
      VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      dep.memoryBarrierCount = 1;
      dep.pMemoryBarriers = &mb;
      vkCmdPipelineBarrier2(cmd, &dep);
    }

    VkComputePipelineCreateInfo ci = {
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    m_directLighting.bindComputePipeline(&RI.device, &RI.primary.cmds[0], kHash,
                                         "DirectLightingPass.cs", &ci);
    m_directLighting.bindBindlessDescriptorSet(
        &RI.primary.cmds[0], &m_global.m_bindlessSet, 0,
        VK_PIPELINE_BIND_POINT_COMPUTE);

    auto pushSampled = [&](const char *name, VkImageView view,
                           VkImageLayout layout) {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create(name);
      b.descriptor.vk.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      b.descriptor.vk.image.sampler = VK_NULL_HANDLE;
      b.descriptor.vk.image.imageView = view;
      b.descriptor.vk.image.imageLayout = layout;
      RIFinalizeDescriptor(&RI.device, &b.descriptor);
      return b;
    };

    std::vector<RIProgram::DescriptorBinding> bnd;
    bnd.reserve(8);
    {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("gPerFrame");
      RI.UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
      bnd.push_back(b);
    }
    pushSurfelStorageImage(bnd, "gPackedHitInfo",
                           m_packedHitInfoView[RI.swapchainIndex].vk.image);
    pushTlas(bnd);
    bnd.push_back(pushSampled("gPackedHitInfoRaster",
                              RI.visibilityView[RI.swapchainIndex].vk.image,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
    bnd.push_back(pushSampled("gVelocity",
                              m_velocityView[RI.swapchainIndex].vk.image,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
    bnd.push_back(pushSampled("gDirectHistory",
                              m_directLightingView[dlPrev].vk.image,
                              VK_IMAGE_LAYOUT_GENERAL));
    bnd.push_back(pushSampled("gDirectKeyHistory",
                              m_directKeyView[dlPrev].vk.image,
                              VK_IMAGE_LAYOUT_GENERAL));
    pushSurfelStorageImage(bnd, "gDirectLighting",
                           m_directLightingView[dlCur].vk.image);
    pushSurfelStorageImage(bnd, "gDirectKeyOut",
                           m_directKeyView[dlCur].vk.image);

    m_directLighting.bindDescriptors(&RI.device, &RI.primary.cmds[0],
                                     RI.frameIndex, bnd.data(), bnd.size(),
                                     VK_PIPELINE_BIND_POINT_COMPUTE);
    CmdDispatch(&RI.primary.cmds[0], (renderWidth + 15u) / 16u,
                (renderHeight + 15u) / 16u, 1u);

    // Current direct texture write -> composite sampled read (stays GENERAL).
    VkMemoryBarrier2 done = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    done.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    done.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    done.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    done.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    VkDependencyInfo depDone = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    depDone.memoryBarrierCount = 1;
    depDone.pMemoryBarriers = &done;
    vkCmdPipelineBarrier2(cmd, &depDone);
  }

  // --------------------------------------------------------------------
  // MainCompositePass — compute pass. Reads gIndirectLighting
  // (m_surfelResultView, from SurfelGenerationPass above) + gPackedHitInfo /
  // gPackedHitInfoRaster / TLAS / gPerFrame, and writes the composited color
  // into the pogo buffer. The post-effect chain ping-pongs through pogo from
  // there; a tail blit lower in this function copies the final pogo "read" half
  // into the swapchain, which the particle/decal pass composites on top of.
  // --------------------------------------------------------------------

  // Composite + forward passes render into the OVERSCAN render-pogo (guard band);
  // cropped 1:1 center into the authored RI.pogoBuffer at the end of Draw, which
  // Scene.cpp's post-effect chain then consumes.
  RI_PogoBuffer *pogo = &RI.renderPogo[RI.swapchainIndex];

  // Barrier: make the surfel cache + gIndirectLighting visible to the COMPUTE
  // SurfelGI composite, and put the pogo attach into GENERAL for the storage
  // write. (The RT V-buffer pass (2956) and the raster visibility buffer (3016)
  // were already barriered to the COMPUTE stage upstream.)
  {
    VkMemoryBarrier2 mem = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mem.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mem.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    mem.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    // SAMPLED_READ for the gIndirectLighting image sample; STORAGE_READ so the
    // isWater branch's gatherSurfelIndirect can read the surfel-cache SSBOs
    // (gSurfelBuffer / gCellInfoBuffer / gCellToSurfelBuffer), written by the
    // surfel compute passes earlier this frame.
    mem.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
                        VK_ACCESS_2_SHADER_STORAGE_READ_BIT;

    std::vector<VkImageMemoryBarrier2> imageBarriers;
    imageBarriers.reserve(3);

    {
      // gIndirectLighting GENERAL -> SHADER_READ_ONLY, now consumed by compute.
      VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
      b.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
      b.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
      b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
      b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
      b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
      b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.image = m_surfelResultTexture[RI.swapchainIndex].vk.image;
      b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      imageBarriers.push_back(b);
    }

    {
      // Pogo attach -> GENERAL for the compute storage write. Discard prior
      // contents (UNDEFINED): the dispatch writes every pixel, matching the old
      // fragment pass's LOAD_OP_DONT_CARE. This also covers the first-frame
      // init for the attach half.
      VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
      b.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
      b.srcAccessMask = VK_ACCESS_2_NONE;
      b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
      b.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
      b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
      b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.image = pogo->textures[pogo->attachmentIndex].vk.image;
      b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      imageBarriers.push_back(b);
    }

    if (!m_pogoInitialized[RI.swapchainIndex]) {
      // First frame: bring the OTHER (read) half to SHADER_READ_ONLY so the
      // post-effect chain / next-frame toggle find it in a defined layout. The
      // attach half is handled by the UNDEFINED->GENERAL barrier above.
      const uint32_t readIdx = (pogo->attachmentIndex + 1u) % 2u;
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


  // Surfel-GI compute pass — one thread per pixel writes the composite into the
  // pogo attach bound as gOutput (storage image, GENERAL).
  {
    VkComputePipelineCreateInfo surfelCreate = {
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    m_mainComposite.bindComputePipeline(&RI.device, &RI.primary.cmds[0], kHash,
                                         "MainCompositePass.cs", &surfelCreate);
    m_mainComposite.bindBindlessDescriptorSet(
        &RI.primary.cmds[0], &m_global.m_bindlessSet, 0,
        VK_PIPELINE_BIND_POINT_COMPUTE);

    std::vector<RIProgram::DescriptorBinding> bnd;
    bnd.reserve(8);
    {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("gPerFrame");
      RI.UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
      bnd.push_back(b);
    }
    pushSurfelStorageImage(bnd, "gPackedHitInfo",
                           m_packedHitInfoView[RI.swapchainIndex].vk.image);
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
      // upstream already transitioned it to SHADER_READ_ONLY_OPTIMAL (visible to
      // COMPUTE).
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
    // gDirectLighting — this frame's accumulated direct (sampled, GENERAL).
    pushSurfelSampledImage(
        bnd, "gDirectLighting",
        m_directLightingView[m_directLightingIndex].vk.image);
    // gOutput — the pogo attach view bound as a storage image (GENERAL).
    pushSurfelStorageImage(
        bnd, "gOutput",
        pogo->pogoAttachment[pogo->attachmentIndex].vk.image.imageView);

    m_mainComposite.bindDescriptors(
        &RI.device, &RI.primary.cmds[0], RI.frameIndex, bnd.data(),
        bnd.size(), VK_PIPELINE_BIND_POINT_COMPUTE);

    CmdDispatch(&RI.primary.cmds[0], (renderWidth + 15u) / 16u,
                (renderHeight + 15u) / 16u, 1u);
  }

  // Toggle the direct-lighting ping-pong: this frame's write becomes next
  // frame's history.
  m_directLightingIndex ^= 1u;

  // Pogo attach: GENERAL (compute write) -> COLOR_ATTACHMENT_OPTIMAL so the
  // unchanged RI_PogoBufferToggle below + the downstream raster passes find the
  // layout they expect.
  {
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    b.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    b.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = pogo->textures[pogo->attachmentIndex].vk.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(RI.primary.cmds[0].vk.cmd, &dep);
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
  // Decal mesh pass — flat decal-quad static meshes (Type="Decal" materials:
  // pool_wine/dirt_floor/moist_wall/cobweb, etc.) routed to eRenderListType_Decal.
  // A thin overlay on the lit opaque scene: depth-tested ≤ against the gbuffer
  // depth, no depth write (DecalPipelineDesc), hardware-blended per the material's
  // blend mode. Runs before the translucent particle/mesh passes so decals sit
  // under translucents. Their VBs were uploaded in the decal prepare loop earlier
  // in Draw(); they're never TLAS instances. Mirrors the translucent mesh pass
  // minus the light-level calc, cube-map second draw, and push constant (Decal.frag
  // emits raw diffuse*color and lets the blender do the rest).
  // --------------------------------------------------------------------
  {
    std::vector<iRenderable *> decals;
    for (iRenderable *pObj :
         m_rendererList.GetRenderableItems(eRenderListType_Decal)) {
      if (!pObj)
        continue;
      cMaterial *pMat = pObj->GetMaterial();
      if (!pMat)
        continue;
      const eMaterialBlendMode mode = pMat->GetBlendMode();
      if (mode == eMaterialBlendMode_None || mode >= eMaterialBlendMode_LastEnum)
        continue;
      iVertexBuffer *pVB = pObj->GetVertexBuffer();
      if (!pVB || pVB->GetIndexNum() <= 0)
        continue;
      decals.push_back(pObj);
    }

    if (!decals.empty()) {
      flipDepthToReadOnly();

      const int   pogoReadIdx   = (pogo->attachmentIndex + 1) % 2;
      VkImage     pogoReadImage = pogo->textures[pogoReadIdx].vk.image;
      VkImageView pogoReadView  = pogo->pogoAttachment[pogoReadIdx].vk.image.imageView;

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
      colorAttach.imageView   = pogoReadView;
      colorAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      colorAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD;
      colorAttach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;

      VkRenderingAttachmentInfo depthAttach = {
          VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
      depthAttach.imageView   = RI.depthView[RI.swapchainIndex].vk.image;
      depthAttach.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
      depthAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD;
      depthAttach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;

      VkRenderingInfo renderInfo = {VK_STRUCTURE_TYPE_RENDERING_INFO};
      renderInfo.renderArea           = {{0, 0}, {renderWidth, renderHeight}};
      renderInfo.layerCount           = 1;
      renderInfo.colorAttachmentCount = 1;
      renderInfo.pColorAttachments    = &colorAttach;
      renderInfo.pDepthAttachment     = &depthAttach;

      vkCmdBeginRendering(RI.primary.cmds[0].vk.cmd, &renderInfo);

      VkViewport vp = {0.0f, (float)renderHeight, (float)renderWidth,
                       -(float)renderHeight, 0.0f, 1.0f};
      VkRect2D sc = {{0, 0}, {renderWidth, renderHeight}};
      vkCmdSetViewport(RI.primary.cmds[0].vk.cmd, 0, 1, &vp);
      vkCmdSetScissor(RI.primary.cmds[0].vk.cmd, 0, 1, &sc);

      m_decal.bindBindlessDescriptorSet(&RI.primary.cmds[0], &m_global.m_bindlessSet, 0);
      {
        RIProgram::DescriptorBinding b;
        b.handle = DescriptorBindingID::Create("gPerFrame");
        RI.UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
        m_decal.bindDescriptors(&RI.device, &RI.primary.cmds[0], RI.frameIndex,
                                &b, 1);
      }

      auto remapDecalBlend = [](eMaterialBlendMode m) {
        switch (m) {
        case eMaterialBlendMode_Add:        return DecalPipelineDesc::BLEND_ADD;
        case eMaterialBlendMode_Mul:        return DecalPipelineDesc::BLEND_MUL;
        case eMaterialBlendMode_MulX2:      return DecalPipelineDesc::BLEND_MULX2;
        case eMaterialBlendMode_Alpha:      return DecalPipelineDesc::BLEND_ALPHA;
        case eMaterialBlendMode_PremulAlpha:return DecalPipelineDesc::BLEND_PREMUL_ALPHA;
        default:                            return DecalPipelineDesc::BLEND_ALPHA;
        }
      };

      for (iRenderable *pObj : decals) {
        iVertexBuffer *pVB = pObj->GetVertexBuffer();
        cMaterial *pMat = pObj->GetMaterial();
        const int indexCount = pVB->GetIndexNum();

        uint32_t materialId =
            m_global.submitMaterial(cntx, pMat, (uint32_t)RI.frameIndex).materialId;
        if (materialId == UINT32_MAX) {
          Warning("Material Slot exhausted (decal)");
          continue;
        }

        ObjectSubmitDesc d;          // decals are unlit overlays
        d.modelMatrix = pObj->GetModelMatrix(apFrustum);
        d.uvMatrix = pMat->GetUvMatrix();
        d.materialId = materialId;
        d.dissolveAmount = pObj->GetCoverageAmount();

        const uint32_t slot = m_global.submitObject(
            pObj->GetUniqueCookie(), (uint32_t)RI.frameIndex,
            static_cast<VertexBuffer_RI *>(pVB), d);
        if (slot == UINT32_MAX) {
          Warning("bindless pool exhausted (decal)");
          continue;
        }

        uint32_t vtxMask = 0;
        if (!detail::BindVertexStreams(&RI.primary.cmds[0], pVB, "decal", &vtxMask))
          continue;

        const DecalPipelineDesc::BlendMode mode =
            remapDecalBlend(pMat->GetBlendMode());
        DecalPipelineDesc pipelineDesc(RIBootstrap::PogoColorFormat,
                                       RIBootstrap::DepthFormat, mode, vtxMask);
        m_decal.bindPipeline(&RI.device, &RI.primary.cmds[0], pipelineDesc.hash,
                             "Decal", &pipelineDesc.createInfo);

        // Decal.frag's per-blend-mode output conversion (display-space source
        // → linear) needs the mode — mirrors the particle pass push block.
        const uint32_t push = (uint32_t)mode;
        vkCmdPushConstants(RI.primary.cmds[0].vk.cmd, m_decal.getPipelineLayout(),
                           VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);

        vkCmdDrawIndexed(RI.primary.cmds[0].vk.cmd, (uint32_t)indexCount, 1u, 0u, 0,
                         slot);
      }

      vkCmdEndRendering(RI.primary.cmds[0].vk.cmd);

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
  // Water pass — raster the water surface over the refracted background that the
  // GI composite already shaded (SurfelVBuffer.rt's water refraction clobbered
  // the primary hit). Two draws per mesh: MUL (tint + refraction exposure) then
  // ADD (inline-RT lit reflection × Fresnel). Reuses the translucent 5-stream
  // layout + TranslucentMeshPipelineDesc state (depth ≤, no write); the m_water
  // program supplies the shaders (Water.vert/frag). Pogo-read-half barriers as the
  // other translucent sub-passes.
  // --------------------------------------------------------------------
  {
    std::vector<iRenderable *> waters;
    for (iRenderable *pObj :
         m_rendererList.GetRenderableItems(eRenderListType_Translucent)) {
      if (!pObj)
        continue;
      cMaterial *pMat = pObj->GetMaterial();
      if (!pMat || pMat->Descriptor().m_id != MaterialID::Water)
        continue;
      iVertexBuffer *pVB = pObj->GetVertexBuffer();
      if (!pVB || pVB->GetIndexNum() <= 0)
        continue;
      waters.push_back(pObj);
    }

    if (!waters.empty()) {
      flipDepthToReadOnly();

      const int   pogoReadIdx   = (pogo->attachmentIndex + 1) % 2;
      VkImage     pogoReadImage = pogo->textures[pogoReadIdx].vk.image;
      VkImageView pogoReadView  = pogo->pogoAttachment[pogoReadIdx].vk.image.imageView;

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
      colorAttach.imageView   = pogoReadView;
      colorAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      colorAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD;
      colorAttach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;

      VkRenderingAttachmentInfo depthAttach = {
          VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
      depthAttach.imageView   = RI.depthView[RI.swapchainIndex].vk.image;
      depthAttach.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
      depthAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD;
      depthAttach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;

      VkRenderingInfo renderInfo = {VK_STRUCTURE_TYPE_RENDERING_INFO};
      renderInfo.renderArea           = {{0, 0}, {renderWidth, renderHeight}};
      renderInfo.layerCount           = 1;
      renderInfo.colorAttachmentCount = 1;
      renderInfo.pColorAttachments    = &colorAttach;
      renderInfo.pDepthAttachment     = &depthAttach;

      vkCmdBeginRendering(RI.primary.cmds[0].vk.cmd, &renderInfo);

      VkViewport vp = {0.0f, (float)renderHeight, (float)renderWidth,
                       -(float)renderHeight, 0.0f, 1.0f};
      VkRect2D sc = {{0, 0}, {renderWidth, renderHeight}};
      vkCmdSetViewport(RI.primary.cmds[0].vk.cmd, 0, 1, &vp);
      vkCmdSetScissor(RI.primary.cmds[0].vk.cmd, 0, 1, &sc);

      m_water.bindBindlessDescriptorSet(&RI.primary.cmds[0], &m_global.m_bindlessSet, 0);
      {
        // set 1: gPerFrame + gRtAccel (binding 36). The water frag does inline
        // RayQuery reflection (traceReflectionHit), so the TLAS must be bound —
        // the raster pipeline doesn't get it for free like the compute/RT passes.
        std::vector<RIProgram::DescriptorBinding> wbnd;
        {
          RIProgram::DescriptorBinding b;
          b.handle = DescriptorBindingID::Create("gPerFrame");
          RI.UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
          wbnd.push_back(b);
        }
        pushTlas(wbnd);
        m_water.bindDescriptors(&RI.device, &RI.primary.cmds[0], RI.frameIndex,
                                wbnd.data(), (uint32_t)wbnd.size());
      }

      struct WaterPush { uint32_t pass; uint32_t p0, p1, p2; };

      for (iRenderable *pObj : waters) {
        iVertexBuffer *pVB = pObj->GetVertexBuffer();
        cMaterial *pMat = pObj->GetMaterial();
        const int indexCount = pVB->GetIndexNum();

        auto mat = m_global.submitMaterial(cntx, pMat, (uint32_t)RI.frameIndex);
        if (mat.materialId == UINT32_MAX) {
          Warning("Material Slot exhausted (water)");
          continue;
        }

        ObjectSubmitDesc d;
        d.modelMatrix = pObj->GetModelMatrix(apFrustum);
        d.uvMatrix = pMat->GetUvMatrix();
        d.materialId = mat.materialId; // water ids fall in the water range of materialID
        d.dissolveAmount = pObj->GetCoverageAmount();

        const uint32_t slot = m_global.submitObject(
            pObj->GetUniqueCookie(), (uint32_t)RI.frameIndex,
            static_cast<VertexBuffer_RI *>(pVB), d);
        if (slot == UINT32_MAX) {
          Warning("bindless pool exhausted (water)");
          continue;
        }

        uint32_t vtxMask = 0;
        if (!detail::BindVertexStreams(&RI.primary.cmds[0], pVB, "water", &vtxMask))
          continue;

        // Two draws into the pogo: tint (MUL) then lit reflection (ADD). Salt the
        // pipeline hash so it doesn't collide with the translucent program's cache
        // (same TranslucentMeshPipelineDesc state, different program/shaders).
        const TranslucentMeshPipelineDesc::BlendMode modes[2] = {
            TranslucentMeshPipelineDesc::BLEND_MUL,
            TranslucentMeshPipelineDesc::BLEND_ADD};
        for (uint32_t pass = 0; pass < 2u; ++pass) {
          TranslucentMeshPipelineDesc pd(RIBootstrap::PogoColorFormat,
                                         RIBootstrap::DepthFormat, modes[pass],
                                         vtxMask);
          const hash_t waterHash = hash_u32(pd.hash, 0x57415445u /*'WATE'*/);
          m_water.bindPipeline(&RI.device, &RI.primary.cmds[0], waterHash, "Water",
                               &pd.createInfo);
          WaterPush push = {pass, 0u, 0u, 0u};
          vkCmdPushConstants(RI.primary.cmds[0].vk.cmd, m_water.getPipelineLayout(),
                             VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
          vkCmdDrawIndexed(RI.primary.cmds[0].vk.cmd, (uint32_t)indexCount, 1u, 0u,
                           0, slot);
        }
      }

      vkCmdEndRendering(RI.primary.cmds[0].vk.cmd);

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
  // Translucent pass — two sub-passes, both into the pogo "read" half:
  //   1. Particle pass (this block) — particle emitters only.
  //   2. Mesh pass (below)          — non-particle translucent meshes.
  // Each opens its own begin/endRendering + pogo barriers, so either is
  // skippable when empty.
  //
  // Resources reused from the opaque path:
  //   - m_global.m_objectSlots / m_objectBuffer (per-renderable OBJECT slot)
  //   - m_opaque*Handles (BDA fan-out: particles/meshes overload
  //                       position/uv0/color/index with their own VB addresses)
  //   - m_global.m_diffuseMaterialBindless / m_diffuseMaterialBuffer (material slot)
  //
  // Sync: (a) swapchain stays COLOR_ATTACHMENT_OPTIMAL from the composite
  //           (load to preserve it); (b) flipDepthToReadOnly() moves depth back
  //           to DEPTH_READ_ONLY_OPTIMAL (shared with the decal pass).
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
                               {renderWidth, renderHeight}};
      renderInfo.layerCount = 1;
      renderInfo.colorAttachmentCount = 1;
      renderInfo.pColorAttachments = &colorAttach;
      renderInfo.pDepthAttachment = &depthAttach;

      vkCmdBeginRendering(RI.primary.cmds[0].vk.cmd, &renderInfo);

      VkViewport vp = {0.0f,
                       (float)renderHeight,
                       (float)renderWidth,
                       -(float)renderHeight,
                       0.0f,
                       1.0f};
      VkRect2D sc = {{0, 0}, {renderWidth, renderHeight}};
      vkCmdSetViewport(RI.primary.cmds[0].vk.cmd, 0, 1, &vp);
      vkCmdSetScissor(RI.primary.cmds[0].vk.cmd, 0, 1, &sc);

      m_particle.bindBindlessDescriptorSet(&RI.primary.cmds[0], &m_global.m_bindlessSet,
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

        uint32_t materialId =
            m_global.submitMaterial(cntx, pMat, (uint32_t)RI.frameIndex).materialId;
        if (materialId == UINT32_MAX) {
          Warning("Material Slot exhausted (particle)");
          continue;
        }

        ObjectSubmitDesc d;          // particle: identity uv, no dissolve/illum
        d.modelMatrix = pEmitter->GetModelMatrix(apFrustum);
        d.materialId = materialId;

        // Particles share the object-slot pool with opaque solids. submitObject
        // bumps the slot generation when this slot is (re)assigned to the emitter
        // — so a surfel still anchored to the slot's previous opaque occupant
        // self-invalidates before it feeds a stale opaque primitiveIndex into the
        // particle mesh's smaller index/vertex BDA (unbounded read → GPUVM fault).
        // The particle VS pulls pos/uv0/color/index via BDA, so submitObject
        // writes the stream + index handles too. (normal/tangent are written as
        // well but never read for a particle slot — harmless.)
        auto *vbri = static_cast<VertexBuffer_RI *>(pVB);
        const uint32_t slot = m_global.submitObject(
            pEmitter->GetUniqueCookie(), (uint32_t)RI.frameIndex, vbri, d,
            kSubmitData | kSubmitVertex | kSubmitIndex);
        if (slot == UINT32_MAX) {
          Warning("bindless pool exhausted (particle)");
          continue;
        }

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
                  slot);
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
  // Refraction is handled upstream by SurfelVBuffer.rt's primary-ray bend
  // (closeHit::isRefractive branch) — the GI composite already shows the
  // refracted background under refractive translucents by the time this pass
  // runs. Water is filtered out of the mesh collection below; it has its own
  // raster pass (m_water) over that refracted background.
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
      // MainCompositePass (isWater branch) from the primary hit + the
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
                               {renderWidth, renderHeight}};
      renderInfo.layerCount = 1;
      renderInfo.colorAttachmentCount = 1;
      renderInfo.pColorAttachments = &colorAttach;
      renderInfo.pDepthAttachment = &depthAttach;

      vkCmdBeginRendering(RI.primary.cmds[0].vk.cmd, &renderInfo);

      VkViewport vp = {0.0f,
                       (float)renderHeight,
                       (float)renderWidth,
                       -(float)renderHeight,
                       0.0f,
                       1.0f};
      VkRect2D sc = {{0, 0}, {renderWidth, renderHeight}};
      vkCmdSetViewport(RI.primary.cmds[0].vk.cmd, 0, 1, &vp);
      vkCmdSetScissor(RI.primary.cmds[0].vk.cmd, 0, 1, &sc);

      m_translucentMesh.bindBindlessDescriptorSet(&RI.primary.cmds[0],
                                                  &m_global.m_bindlessSet, 0);

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

        uint32_t materialId =
            m_global.submitMaterial(cntx, pMat, (uint32_t)RI.frameIndex).materialId;
        if (materialId == UINT32_MAX) {
          Warning("Material Slot exhausted (translucent mesh)");
          continue;
        }

        cMatrixf *pMtx = pObj->GetModelMatrix(apFrustum);

        // AffectedByLightLevel dimming is evaluated per-vertex on the GPU
        // (Translucent.vert.slang → gScene.lightLevelAt, gated on
        // kMaterialFlagAffectedByLightLevel in the material config) — the
        // legacy per-object CPU light loop that used to live here is gone.

        ObjectSubmitDesc d;
        d.modelMatrix = pMtx;
        d.uvMatrix = pMat->GetUvMatrix();
        d.materialId = materialId;
        d.dissolveAmount = pObj->GetCoverageAmount();

        // Stable slot keyed on the renderable's unique cookie. submitObject
        // bumps the slot generation on (re)assignment so a surfel anchored to a
        // previous opaque occupant self-invalidates before dereferencing the
        // wrong VB/IB.
        const uint32_t slot = m_global.submitObject(
            pObj->GetUniqueCookie(), (uint32_t)RI.frameIndex,
            static_cast<VertexBuffer_RI *>(pVB), d);
        if (slot == UINT32_MAX) {
          Warning("bindless pool exhausted (translucent mesh)");
          continue;
        }

        // Per-vertex streams use fixed-function vertex fetch (the pipeline
        // declares them as VkVertexInputBindingDescriptions — see
        // TranslucentMeshPipelineDesc). Bindless OBJECT-slot handle arrays
        // (m_opaque*Mirror) stay populated only on the opaque + TLAS paths.
        // detail::BindVertexStreams binds pos/normal/tangent/color/uv0 + index
        // (with fallbacks for absent optional streams) for both draws below.
        uint32_t vtxMask = 0;
        if (!detail::BindVertexStreams(&RI.primary.cmds[0], pVB, "translucent",
                                       &vtxMask))
          continue;

        const TranslucentMeshPipelineDesc::BlendMode mode =
            remapBlend(pMat->GetBlendMode());
        TranslucentMeshPipelineDesc pipelineDesc(meshTargetFormat,
                                                 RIBootstrap::DepthFormat,
                                                 mode, vtxMask);
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

        vkCmdDrawIndexed(RI.primary.cmds[0].vk.cmd, (uint32_t)indexCount, 1u,
                         0u, 0, slot);

        // Second draw for the cube-map Fresnel + rim contribution.
        // Reference gates this on `cubeMap && !isRefraction`
        // (RendererDeferred.cpp:4660); under the RT ray-bend model
        // isRefraction surfaces already get their refracted background
        // from the GI composite, so the cube-map second draw stays gated
        // only on whether the material carries a cube map.
        if (pMat->GetImage(eMaterialTexture_CubeMap)) {
          TranslucentMeshPipelineDesc addDesc(
              meshTargetFormat, RIBootstrap::DepthFormat,
              TranslucentMeshPipelineDesc::BLEND_ADD, vtxMask);
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
                           1u, 0u, 0, slot);
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

  // Offscreen delivery: the finished scene sits in the pogo read half at the
  // viewport's 1:1 extent (no guard band was applied). Blit {0,0,w,h} into the
  // viewport target's sampled texture and leave it SHADER_READ_ONLY for the
  // GUI / a later DebugDraw overlay pass. The authored RI.pogoBuffer is left
  // untouched — Scene.cpp skips the post-effect chain + swapchain tail blit
  // for offscreen viewports.
  if (offTarget) {
    const uint32_t srcReadIdx = (pogo->attachmentIndex + 1u) % 2u;
    VkImage srcImage = pogo->textures[srcReadIdx].vk.image;
    VkImage dstImage = offTarget->GetTexture()->handle.vk.image;

    // Pin the target texture on the frame context so a mid-flight Resize
    // can't free it before this frame's GPU work completes.
    cntx->textureLink.push_back(offTarget->GetTexture());

    // DebugDraw overlay (editor grid / gizmos / icons, enqueued by the
    // viewport's OnPreWorldDraw callbacks): draw into the finished scene in
    // the pogo read half against the scene depth, so the blit below carries
    // the overlay into the target with it. The read half sits in
    // SHADER_READ_ONLY (left by the composite toggle / last forward pass);
    // flip it to COLOR for the overlay pass.
    DebugDraw *debugDraw = mpGraphics->GetDebugDraw();
    const bool debugOverlayDrawn = debugDraw && debugDraw->HasRequests();
    if (debugOverlayDrawn) {
      {
        VkImageMemoryBarrier2 toColor = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        toColor.srcStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        toColor.srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
        toColor.dstStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        toColor.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        toColor.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toColor.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toColor.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toColor.image = srcImage;
        toColor.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &toColor;
        vkCmdPipelineBarrier2(RI.primary.cmds[0].vk.cmd, &dep);
      }
      {
        VkRenderingAttachmentInfo colorAttachment = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        colorAttachment.imageView = pogo->pogoAttachment[srcReadIdx].vk.image.imageView;
        colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

        // Scene depth, restored to DEPTH_ATTACHMENT_OPTIMAL just above; the
        // overlay pipelines test against it but never write.
        VkRenderingAttachmentInfo depthStencil = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        RI_VK_FillDepthAttachment(&depthStencil, &RI.depthView[RI.swapchainIndex], /*attachAndClear=*/false);

        VkRenderingInfo renderingInfo = {VK_STRUCTURE_TYPE_RENDERING_INFO};
        renderingInfo.renderArea = VkRect2D{{0, 0}, {renderWidth, renderHeight}};
        renderingInfo.layerCount = 1;
        renderingInfo.colorAttachmentCount = 1;
        renderingInfo.pColorAttachments = &colorAttachment;
        renderingInfo.pDepthAttachment = &depthStencil;
        vkCmdBeginRendering(RI.primary.cmds[0].vk.cmd, &renderingInfo);

        debugDraw->flush(cntx, &RI.primary.cmds[0], apFrustum, renderWidth,
                         renderHeight, RIBootstrap::PogoColorFormatVk);

        vkCmdEndRendering(RI.primary.cmds[0].vk.cmd);
      }
    }

    {
      VkImageMemoryBarrier2 pre[2] = {};
      pre[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
      if (debugOverlayDrawn) {
        pre[0].srcStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        pre[0].srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        pre[0].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      } else {
        pre[0].srcStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        pre[0].srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
        pre[0].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      }
      pre[0].dstStageMask  = VK_PIPELINE_STAGE_2_BLIT_BIT;
      pre[0].dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
      pre[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      pre[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      pre[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      pre[0].image = srcImage;
      pre[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

      // Fully overwritten every frame — UNDEFINED discard also covers the
      // image's very first use, and orders against any prior-frame GUI
      // sampling on this queue.
      pre[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
      pre[1].srcStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
      pre[1].srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
      pre[1].dstStageMask  = VK_PIPELINE_STAGE_2_BLIT_BIT;
      pre[1].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
      pre[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      pre[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      pre[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      pre[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      pre[1].image = dstImage;
      pre[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

      VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      dep.imageMemoryBarrierCount = 2;
      dep.pImageMemoryBarriers = pre;
      vkCmdPipelineBarrier2(RI.primary.cmds[0].vk.cmd, &dep);
    }

    VkImageBlit region = {};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.srcOffsets[0]  = {0, 0, 0};
    region.srcOffsets[1]  = {(int32_t)renderWidth, (int32_t)renderHeight, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstOffsets[0]  = {0, 0, 0};
    region.dstOffsets[1]  = {(int32_t)renderWidth, (int32_t)renderHeight, 1};
    vkCmdBlitImage(RI.primary.cmds[0].vk.cmd, srcImage,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstImage,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
                   VK_FILTER_NEAREST);

    // Post-blit: src back to SHADER_READ_ONLY (the pogo invariant the next
    // frame's toggle expects), dst → SHADER_READ_ONLY for GUI sampling.
    {
      VkImageMemoryBarrier2 post[2] = {};
      post[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
      post[0].srcStageMask  = VK_PIPELINE_STAGE_2_BLIT_BIT;
      post[0].srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
      post[0].dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
      post[0].dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
      post[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      post[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      post[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      post[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      post[0].image = srcImage;
      post[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

      post[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
      post[1].srcStageMask  = VK_PIPELINE_STAGE_2_BLIT_BIT;
      post[1].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
      post[1].dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
      post[1].dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
      post[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      post[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      post[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      post[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      post[1].image = dstImage;
      post[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

      VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      dep.imageMemoryBarrierCount = 2;
      dep.pImageMemoryBarriers = post;
      vkCmdPipelineBarrier2(RI.primary.cmds[0].vk.cmd, &dep);
    }
  }
  // Guard-band crop: the composite + every forward pass rendered the overscan
  // frame into `pogo` (= RI.renderPogo[...]); its finished scene sits in the
  // read half (SHADER_READ_ONLY, left by the composite toggle / last forward
  // pass). Blit the center renderWidth/Height window down to the authored-size
  // RI.pogoBuffer so Scene.cpp's post-effect chain + tail blit consume a normal
  // display-size pogo. The displayed FOV is unchanged because the projection was
  // widened by the same guard-band factor up front, so the cropped center equals
  // the pre-overscan image.
  else {
    RI_PogoBuffer *authored = &RI.pogoBuffer[RI.swapchainIndex];
    const uint32_t srcReadIdx    = (pogo->attachmentIndex + 1u) % 2u;
    const uint32_t authReadIdx   = (authored->attachmentIndex + 1u) % 2u;
    const uint32_t authAttachIdx = authored->attachmentIndex;
    VkImage srcImage    = pogo->textures[srcReadIdx].vk.image;
    VkImage dstImage    = authored->textures[authReadIdx].vk.image;
    VkImage authAttach  = authored->textures[authAttachIdx].vk.image;

    const uint32_t W = RI.swapchain.width;
    const uint32_t H = RI.swapchain.height;
    const int32_t  offX = (int32_t)(renderWidth  - W) / 2;
    const int32_t  offY = (int32_t)(renderHeight - H) / 2;

    // Pre-blit transitions: src read half → TRANSFER_SRC, authored read half →
    // TRANSFER_DST (UNDEFINED discard — fully overwritten), authored attach half
    // UNDEFINED → COLOR so the post-effect chain (which renders into the attach
    // half with no barrier of its own) finds it ready.
    {
      VkImageMemoryBarrier2 pre[3] = {};
      pre[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
      pre[0].srcStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
      pre[0].srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
      pre[0].dstStageMask  = VK_PIPELINE_STAGE_2_BLIT_BIT;
      pre[0].dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
      pre[0].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      pre[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      pre[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      pre[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      pre[0].image = srcImage;
      pre[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

      pre[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
      pre[1].srcStageMask  = VK_PIPELINE_STAGE_2_NONE;
      pre[1].srcAccessMask = VK_ACCESS_2_NONE;
      pre[1].dstStageMask  = VK_PIPELINE_STAGE_2_BLIT_BIT;
      pre[1].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
      pre[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      pre[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      pre[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      pre[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      pre[1].image = dstImage;
      pre[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

      pre[2].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
      pre[2].srcStageMask  = VK_PIPELINE_STAGE_2_NONE;
      pre[2].srcAccessMask = VK_ACCESS_2_NONE;
      pre[2].dstStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
      pre[2].dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
      pre[2].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      pre[2].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      pre[2].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      pre[2].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      pre[2].image = authAttach;
      pre[2].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

      VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      dep.imageMemoryBarrierCount = 3;
      dep.pImageMemoryBarriers = pre;
      vkCmdPipelineBarrier2(RI.primary.cmds[0].vk.cmd, &dep);
    }

    VkImageBlit region = {};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.srcOffsets[0]  = {offX, offY, 0};
    region.srcOffsets[1]  = {offX + (int32_t)W, offY + (int32_t)H, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstOffsets[0]  = {0, 0, 0};
    region.dstOffsets[1]  = {(int32_t)W, (int32_t)H, 1};
    vkCmdBlitImage(RI.primary.cmds[0].vk.cmd, srcImage,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstImage,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
                   VK_FILTER_NEAREST);

    // Post-blit: src back to SHADER_READ_ONLY (the layout next frame's pogo
    // toggle expects for the half it re-acquires as the render target), authored
    // read half → SHADER_READ_ONLY for the post-effect chain / tail blit.
    {
      VkImageMemoryBarrier2 post[2] = {};
      post[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
      post[0].srcStageMask  = VK_PIPELINE_STAGE_2_BLIT_BIT;
      post[0].srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
      post[0].dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
      post[0].dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
      post[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      post[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      post[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      post[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      post[0].image = srcImage;
      post[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

      post[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
      post[1].srcStageMask  = VK_PIPELINE_STAGE_2_BLIT_BIT;
      post[1].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
      post[1].dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
      post[1].dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
      post[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      post[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      post[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      post[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      post[1].image = dstImage;
      post[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

      VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      dep.imageMemoryBarrierCount = 2;
      dep.pImageMemoryBarriers = post;
      vkCmdPipelineBarrier2(RI.primary.cmds[0].vk.cmd, &dep);
    }
  }

  // Commit every bindless handle / slot-generation write made this frame. The
  // uploader records into its own transfer cmd buffer, flushed as a fenced
  // pre-pass the primary submit waits on (RIBootstrap), so this single tail call
  // lands all copies ahead of every primary read regardless of recording order.
  m_global.flushMirrors(&RI.device);
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
  m_global.destroy(&RI.device);
  // Fallback vertex streams are RIBootstrap globals (process-lifetime, like
  // RI.nulVertexBuffer); not freed here.
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
    if (m_velocityView[i].vk.image != VK_NULL_HANDLE) {
      vkDestroyImageView(RI.device.vk.device, m_velocityView[i].vk.image, NULL);
      m_velocityView[i] = {};
    }
    if (m_velocityTexture[i].vk.image != VK_NULL_HANDLE) {
      vmaDestroyImage(RI.device.vk.vmaAllocator, m_velocityTexture[i].vk.image,
                      m_velocityTexture[i].vk.allocation);
      m_velocityTexture[i] = {};
    }
    if (i < 2) {
      if (m_directLightingView[i].vk.image != VK_NULL_HANDLE) {
        vkDestroyImageView(RI.device.vk.device,
                           m_directLightingView[i].vk.image, NULL);
        m_directLightingView[i] = {};
      }
      if (m_directLightingTexture[i].vk.image != VK_NULL_HANDLE) {
        vmaDestroyImage(RI.device.vk.vmaAllocator,
                        m_directLightingTexture[i].vk.image,
                        m_directLightingTexture[i].vk.allocation);
        m_directLightingTexture[i] = {};
      }
      if (m_directKeyView[i].vk.image != VK_NULL_HANDLE) {
        vkDestroyImageView(RI.device.vk.device, m_directKeyView[i].vk.image,
                           NULL);
        m_directKeyView[i] = {};
      }
      if (m_directKeyTexture[i].vk.image != VK_NULL_HANDLE) {
        vmaDestroyImage(RI.device.vk.vmaAllocator,
                        m_directKeyTexture[i].vk.image,
                        m_directKeyTexture[i].vk.allocation);
        m_directKeyTexture[i] = {};
      }
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
}

} // namespace hpl
