#include "graphics/HybridRenderer.h"
#include "graphics/Image.h" // Image::GetBindlessSlot (light gobos read the slot)
#include "graphics/RITypes.h"

#include "graphics/DebugDraw.h"
#include "graphics/DecalPipelineDesc.h"
#include "graphics/GBufferMRTPipelineDesc.h"
#include "graphics/GraphicUtils.h"
#include "graphics/Graphics.h"
#include "graphics/Material.h"
#include "graphics/MaterialType.h"
#include "graphics/ParticlePipelineDesc.h"
#include "graphics/PostEffectComposite.h"
#include "graphics/RIBootstrap.h"
#include "graphics/RIPogoBuffer.h"
#include "graphics/RIResourceUploader.h"
#include "graphics/RIVK.h"
#include "graphics/Renderable.h"
#include "graphics/TranslucentMeshPipelineDesc.h"
#include "graphics/VertexBuffer.h"
#include "math/Frustum.h"
#include "math/Math.h"
#include "scene/Viewport.h"

#include "resources/Resources.h"
#include "resources/TextureManager.h"
#include "scene/Decal.h"
#include "scene/FogArea.h"
#include "scene/Light.h"
#include "scene/LightSpot.h"
#include "scene/LightArea.h"
#include "scene/ParticleEmitter.h"
#include "scene/RenderableContainer.h"
#include "scene/World.h"
#include "system/LowLevelSystem.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>

namespace hpl {

namespace detail {

static inline bool BindVertexStreams(struct RICmd *cmd, cVertexBuffer *pVB,
                                     const char *passLabel,
                                     uint32_t *outPresentMask) {
  auto *vbri = static_cast<cVertexBuffer *>(pVB);
  auto bufOf = [&](eVertexBufferElement type) -> RIBuffer * {
    const auto *element = vbri->GetElement(type);
    return element ? element->GetBuffer() : nullptr;
  };
  // Position + index are the only truly required streams — without geometry
  // there's nothing to draw.
  RIBuffer *pos = bufOf(eVertexBufferElement_Position);
  RIBuffer *idx = vbri->GetIndexRIBuffer();
  if (!pos || !idx) {
    Warning("%s mesh missing position / index — skipping", passLabel);
    return false;
  }
  // Optional streams: bind the real buffer when present, else the global
  // single-vertex default (normal = +Z, tangent = +X/handedness, color = white,
  // uv = 0). No capacity limit — the pipeline zeroes the absent binding's
  // stride, so the one fallback element is reread for every vertex.
  RIBuffer *nrm = bufOf(eVertexBufferElement_Normal);
  RIBuffer *tan = bufOf(eVertexBufferElement_Texture1Tangent);
  RIBuffer *col = bufOf(eVertexBufferElement_Color0);
  RIBuffer *uv = bufOf(eVertexBufferElement_Texture0);
  uint32_t mask =
      eVertexElementFlag_Position; // required, present per check above
  if (nrm)
    mask |= eVertexElementFlag_Normal;
  if (tan)
    mask |= eVertexElementFlag_Texture1;
  if (col)
    mask |= eVertexElementFlag_Color0;
  if (uv)
    mask |= eVertexElementFlag_Texture0;
  if (outPresentMask)
    *outPresentMask = mask;
  RIBuffer *vertBufs[5] = {
      pos,
      nrm ? nrm : &RI.fallbackNormalVertex,
      tan ? tan : &RI.fallbackTangentVertex,
      col ? col : &RI.fallbackColorVertex,
      uv ? uv : &RI.fallbackUv0Vertex,
  };
  cmd->bindVertexBuffers<5>(0, 5, vertBufs);
  cmd->bindIndexBuffer(&RI.device, idx, 0, RI_INDEX_TYPE_32);
  return true;
}

} // namespace detail

cHybridRenderer::cHybridRenderer(cGraphics *apGraphics, cResources *apResources)
    : iRenderer("Hybrid", apGraphics, apResources) {
  {
    // The global bindless descriptor set (set 0) and every buffer bound to it
    // are an engine-lifetime singleton, constructed in cGraphics::Init via
    // InitGlobalManagedSets() before any renderer exists. We just borrow its
    // layout here.
    const VkDescriptorSetLayout externalLayouts[] = {
        RI.globalset->m_bindlessSet.vk.m_bindlessSetLayout};
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

    auto loadComputeProgram = [&](RIProgram &prog, const char *name) {
      auto bin =
          RIProgram::loadShaderStage(apResources->GetFileSearcher(), name);
      std::array<RIProgram::ModuleStage, 1> stages = {
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_COMPUTE, bin}};
      prog.initialize(&RI.device, stages, externalLayouts);
    };
    // Compute load that passes the Slang entry-point name through to
    // ModuleStage.
    auto loadSlangCompute = [&](RIProgram &prog, const char *name,
                                const char *entryPoint) {
      auto bin =
          RIProgram::loadShaderStage(apResources->GetFileSearcher(), name);
      std::array<RIProgram::ModuleStage, 1> stages = {RIProgram::ModuleStage{
          RIProgram::PROGRAM_STAGE_COMPUTE, bin, entryPoint}};
      prog.initialize(&RI.device, stages, externalLayouts);
    };
    // SurfelPomBary — compute pass that copies the raster V-buffer into
    // packedHitInfoTexture and applies parallax-occlusion barycentric
    // correction for height-mapped diffuse surfaces.
    loadSlangCompute(m_surfelPomBary, "SurfelPomBary.cs.spv", "csMain");
    loadSlangCompute(m_surfelPrepare, "SurfelPreparePass.cs.spv", "csMain");
    // SurfelUpdatePass — one .spv, three entry points (collectCellInfo /
    // accumulateCellInfo / updateCellToSurfelBuffer). The blob vector must
    // outlive all three initialize() calls — ModuleStage holds a non-owning
    // view.
    {
      auto upd_bin = RIProgram::loadShaderStage(apResources->GetFileSearcher(),
                                                "SurfelUpdatePass.cs.spv");
      auto initFromBlob = [&](RIProgram &prog, const char *entryPoint) {
        std::array<RIProgram::ModuleStage, 1> stages = {RIProgram::ModuleStage{
            RIProgram::PROGRAM_STAGE_COMPUTE, upd_bin, entryPoint}};
        prog.initialize(&RI.device, stages, externalLayouts);
      };
      initFromBlob(m_surfelUpdateCollect, "collectCellInfo");
      initFromBlob(m_surfelUpdateAccumulate, "accumulateCellInfo");
      initFromBlob(m_surfelUpdateScatter, "updateCellToSurfelBuffer");
    }
    // SurfelRayTrace — one .spv, four entry points (rayGen / scatterMiss /
    // scatterCloseHit / scatterAnyHit). Shadow rays use inline RayQuery, so no
    // second hit group is needed (SBT stays single-ray-type).
    {
      auto rt_bin = RIProgram::loadShaderStage(apResources->GetFileSearcher(),
                                               "SurfelRayTrace.rt.spv");
      std::array<RIProgram::ModuleStage, 4> stages = {
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_RAYGEN, rt_bin,
                                 "rayGen"},
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_MISS, rt_bin,
                                 "scatterMiss"},
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_CLOSEST_HIT, rt_bin,
                                 "scatterCloseHit"},
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_ANY_HIT, rt_bin,
                                 "scatterAnyHit"}};
      m_surfelRT.initialize(&RI.device, stages, externalLayouts);
    }
    // LightGridBuildPass — single compute entry (binLights) that bins
    // point/spot lights into the coarse world-space light grid each frame.
    loadSlangCompute(m_lightGridBin, "LightGridBuildPass.cs.spv", "binLights");
    loadSlangCompute(m_surfelIntegrate, "SurfelIntegratePass.cs.spv", "csMain");
    loadSlangCompute(m_surfelGenerate, "SurfelGenerationPass.cs.spv", "csMain");
    // MainComposite — compute pass: one thread per pixel writes the composite
    // (albedo + inline decals + lighting) into the pogo attach bound as
    // gOutput. The renderer transitions the attach to GENERAL around the
    // dispatch and back to COLOR_ATTACHMENT_OPTIMAL afterwards.
    loadSlangCompute(m_mainComposite, "MainCompositePass.cs.spv", "csMain");
    loadSlangCompute(m_directLighting, "DirectLightingPass.cs.spv", "csMain");
    loadSlangCompute(m_directSpatialReuse, "DirectSpatialReusePass.cs.spv",
                     "csMain");
    loadSlangCompute(m_directAtrous, "DirectAtrousPass.cs.spv", "csMain");
    {
      // Particle pass (amnesia/slang/ParticlePass).
      auto p_vert = RIProgram::loadShaderStage(apResources->GetFileSearcher(),
                                               "Particle.vert.spv");
      auto p_frag = RIProgram::loadShaderStage(apResources->GetFileSearcher(),
                                               "Particle.frag.spv");
      std::array<RIProgram::ModuleStage, 2> stages = {
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, p_vert,
                                 "vsMain"},
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, p_frag,
                                 "psMain"}};
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
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, t_vert,
                                 "vsMain"},
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, t_frag,
                                 "psMain"}};
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
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, d_vert,
                                 "vsMain"},
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, d_frag,
                                 "psMain"}};
      m_decal.initialize(&RI.device, stages, externalLayouts);
    }
    {
      auto w_vert = RIProgram::loadShaderStage(apResources->GetFileSearcher(),
                                               "Water.vert.spv");
      auto w_frag = RIProgram::loadShaderStage(apResources->GetFileSearcher(),
                                               "Water.frag.spv");
      std::array<RIProgram::ModuleStage, 2> stages = {
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, w_vert,
                                 "vsMain"},
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, w_frag,
                                 "psMain"}};
      m_water.initialize(&RI.device, stages, externalLayouts);
    }

    RISegmentAllocDesc indirectDesc = {};
    indirectDesc.numSegments = RI_NUMBER_FRAMES_FLIGHT;
    indirectDesc.elementStride = sizeof(VkDrawIndirectCommand);
    indirectDesc.maxElements = kObjectSlotCapacity;
    m_indirectSegment = RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS>(&indirectDesc);
    m_indirectDrawBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, indirectDesc.maxElements, sizeof(VkDrawIndirectCommand),
        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    // VkTraceRaysIndirectCommandKHR — 3 × uint32: {width, height=1, depth=1}.
    // Width is patched each frame via vkCmdCopyBuffer from gSurfelCounter[kSurfelCounterRequestedRay].
    // SHADER_DEVICE_ADDRESS_BIT is required by vkCmdTraceRaysIndirectKHR.
    m_surfelRTIndirectBuf = detail::CreateBindlessSlotBuffer(
        &RI.device, 3u, sizeof(uint32_t),
        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);

    for (uint32_t i = 0; i < RI.swapchain.imageCount; ++i) {
      RITextureDesc desc = {};
      desc.type = RI_TEXTURE_2D;
      desc.format = RI_FORMAT_RGBA16_SFLOAT;
      desc.width = 4096u;
      desc.height = 4096u;
      // TRANSFER_DST: this atlas is seeded once via vkCmdClearColorImage.
      desc.usage = RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_SHADER_RESOURCE |
                   RI_USAGE_TRANSFER_DST;
      m_surfelDepthTexture[i] = RITexture::create(&RI.device, desc);

      RITextureViewDesc viewDesc = {};
      viewDesc.viewType = RI_VIEWTYPE_SHADER_RESOURCE_2D;
      viewDesc.format = RI_FORMAT_RGBA16_SFLOAT;
      viewDesc.mipNum = 1;
      viewDesc.layerNum = 1;
      m_surfelDepthView[i] =
          RITextureView::create(&RI.device, &m_surfelDepthTexture[i], viewDesc);
    }
  }
}

void cViewport::HybridViewportState::Update(RIBootstrap::FrameContext *cntx,
                                            cVector2l size) {
  if (size.x <= 0 || size.y <= 0) {
    return;
  }
  const uint32_t renderW = (uint32_t)size.x;
  const uint32_t renderH = (uint32_t)size.y;
  if (width == renderW && height == renderH &&
      targetWidth == (uint32_t)size.x && targetHeight == (uint32_t)size.y) {
    return;
  }

  *this = {}; // defer the old resources, reset to empty (see operator=)

  width = renderW;
  height = renderH;
  targetWidth = (uint32_t)size.x; // the BackBuffer crop window
  targetHeight = (uint32_t)size.y;

  for (uint32_t i = 0; i < RI.swapchain.imageCount; i++) {
    // Overscan HDR color target: the compute composite writes it as a
    // storage image, the forward raster passes attach it, cScene's pogo
    // feed blits its centered authored window out (TRANSFER_SRC).
    CreateViewportColorTexture(
        &RI.device, renderW, renderH, RIBootstrap::PogoColorFormat,
        RI_USAGE_COLOR_ATTACHMENT | RI_USAGE_SHADER_RESOURCE |
            RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_TRANSFER_SRC |
            RI_USAGE_TRANSFER_DST,
        &renderTarget[i], &renderTargetView[i],
        "HybridViewportState.renderTarget");

    // SAMPLED lets surfel_generate / surfel_raytrace bind the depth as
    // `sampler2D depthMap` after the gbuffer pass flips it to
    // SHADER_READ_ONLY.
    // DEPTH|STENCIL view: the Z passes only touch the depth aspect, but
    // cLuxEffectRenderer's outline pass binds this same view as a stencil
    // attachment (mark silhouette -> NOTEQUAL composite), so the view must
    // carry the stencil aspect. For sampling the depth aspect (soft particles)
    // we add a separate depth-only SRV below — a combined view can't be sampled.
    CreateViewportAttachmentTexture(
        &RI.device, renderW, renderH, RIBootstrap::DepthFormat,
        RI_USAGE_DEPTH_STENCIL_ATTACHMENT | RI_USAGE_SHADER_RESOURCE,
        RI_VIEWTYPE_DEPTH_STENCIL_ATTACHMENT,
        &depthTextures[i], &depthView[i], "HybridViewportState.depth");

    // Second view of the SAME depth image, depth-aspect only (SHADER_RESOURCE
    // view → RITextureView::create selects DEPTH_BIT, dropping the stencil bit
    // that makes the combined depthView above unsampleable). The particle pass
    // binds this as gSceneDepth for the soft-particle depth fade.
    {
      RITextureViewDesc dsv = {};
      dsv.viewType = RI_VIEWTYPE_SHADER_RESOURCE_2D;
      dsv.format = RIBootstrap::DepthFormat;
      dsv.mipNum = 1;
      dsv.layerNum = 1;
      RITextureView v =
          RITextureView::create(&RI.device, depthTextures[i].Get(), dsv);
      depthSampleView[i] = RISharedPointer<RITextureView>(&RI.device, v);
    }

    CreateViewportAttachmentTexture(
        &RI.device, renderW, renderH, RIBootstrap::VisibilityFormat,
        RI_USAGE_COLOR_ATTACHMENT | RI_USAGE_SHADER_RESOURCE,
        RI_VIEWTYPE_SHADER_RESOURCE_2D, &visibilityTexture[i], &visibilityView[i],
        "HybridViewportState.visibility");

    // Surfel-generation output — storage write (surfel_generate), sampled
    // by the composite, cleared per frame (TRANSFER_DST).
    CreateViewportAttachmentTexture(
        &RI.device, renderW, renderH, RIBootstrap::PogoColorFormat,
        RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_SHADER_RESOURCE |
            RI_USAGE_TRANSFER_DST,
        RI_VIEWTYPE_SHADER_RESOURCE_2D, &surfelResultTexture[i],
        &surfelResultView[i], "HybridViewportState.surfelResult");

    // Stage B packed visibility — RT pipeline storage write, sampled by the
    // surfel update / generation / direct passes.
    CreateViewportAttachmentTexture(
        &RI.device, renderW, renderH, RIBootstrap::VisibilityFormat,
        RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_SHADER_RESOURCE,
        RI_VIEWTYPE_SHADER_RESOURCE_2D, &packedHitInfoTexture[i],
        &packedHitInfoView[i], "HybridViewportState.packedHitInfo");

    // Screen-space velocity — gbuffer MRT #2, sampled by temporal passes.
    CreateViewportAttachmentTexture(
        &RI.device, renderW, renderH, RIBootstrap::VelocityFormat,
        RI_USAGE_COLOR_ATTACHMENT | RI_USAGE_SHADER_RESOURCE,
        RI_VIEWTYPE_SHADER_RESOURCE_2D, &velocityTexture[i], &velocityView[i],
        "HybridViewportState.velocity");

    // Decal accumulators — Mul/MulX2 into decalMul, Add into decalAdd; the
    // composite applies albedo = albedo*decalMul + decalAdd before lighting.
    // RGBA16F (PogoColorFormat) so the linear factors don't band; COLOR_ATTACHMENT
    // for the raster, SHADER_RESOURCE for the composite read.
    CreateViewportAttachmentTexture(
        &RI.device, renderW, renderH, RIBootstrap::PogoColorFormat,
        RI_USAGE_COLOR_ATTACHMENT | RI_USAGE_SHADER_RESOURCE,
        RI_VIEWTYPE_SHADER_RESOURCE_2D, &decalMulTexture[i], &decalMulView[i],
        "HybridViewportState.decalMul");
    CreateViewportAttachmentTexture(
        &RI.device, renderW, renderH, RIBootstrap::PogoColorFormat,
        RI_USAGE_COLOR_ATTACHMENT | RI_USAGE_SHADER_RESOURCE,
        RI_VIEWTYPE_SHADER_RESOURCE_2D, &decalAddTexture[i], &decalAddView[i],
        "HybridViewportState.decalAdd");
  }

  // Direct-lighting accumulation ping-pong + parallel surface-key textures —
  // STORAGE (compute write) + SAMPLED (history reproject + composite read) +
  // TRANSFER_DST (first-use clear). Kept in GENERAL; toggled per frame.
  for (uint32_t i = 0; i < 2; i++) {
    CreateViewportAttachmentTexture(
        &RI.device, renderW, renderH, RIBootstrap::PogoColorFormat,
        RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_SHADER_RESOURCE |
            RI_USAGE_TRANSFER_DST,
        RI_VIEWTYPE_SHADER_RESOURCE_2D, &directLightingTexture[i],
        &directLightingView[i], "HybridViewportState.directLighting");
    CreateViewportAttachmentTexture(
        &RI.device, renderW, renderH, RIBootstrap::PogoColorFormat,
        RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_SHADER_RESOURCE |
            RI_USAGE_TRANSFER_DST,
        RI_VIEWTYPE_SHADER_RESOURCE_2D, &directKeyTexture[i], &directKeyView[i],
        "HybridViewportState.directKey");
    CreateViewportAttachmentTexture(
        &RI.device, renderW, renderH, RIBootstrap::PogoColorFormat,
        RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_SHADER_RESOURCE |
            RI_USAGE_TRANSFER_DST,
        RI_VIEWTYPE_SHADER_RESOURCE_2D, &directAtrousTexture[i],
        &directAtrousView[i], "HybridViewportState.directAtrous");
    // ReSTIR reservoir history ping-pong (RGBA32F: asfloat(lightIndex), W, M).
    CreateViewportAttachmentTexture(
        &RI.device, renderW, renderH, RI_FORMAT_RGBA32_SFLOAT,
        RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_SHADER_RESOURCE |
            RI_USAGE_TRANSFER_DST,
        RI_VIEWTYPE_SHADER_RESOURCE_2D, &reservoirTexture[i], &reservoirView[i],
        "HybridViewportState.reservoir");
  }
  // Intra-frame reservoir hand-off (temporal pass → spatial pass), RGBA32F.
  CreateViewportAttachmentTexture(
      &RI.device, renderW, renderH, RI_FORMAT_RGBA32_SFLOAT,
      RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_SHADER_RESOURCE |
          RI_USAGE_TRANSFER_DST,
      RI_VIEWTYPE_SHADER_RESOURCE_2D, &reservoirTemporalTexture,
      &reservoirTemporalView, "HybridViewportState.reservoirTemporal");

  // Recreation invalidated every history: re-arm the one-time direct-lighting
  // init/clear and re-seed prev-camera = current on the next Draw.
  directLightingIndex = 0;
  directLightingInit = false;
  hasPrevCamera = false;
}

cViewport::HybridViewportState::~HybridViewportState() {
  for (uint32_t i = 0; i < RI_MAX_SWAPCHAIN_IMAGES; i++) {
    RI.graphicsDefer.push(renderTarget[i]);
    RI.graphicsDefer.push(depthTextures[i]);
    RI.graphicsDefer.push(visibilityTexture[i]);
    RI.graphicsDefer.push(surfelResultTexture[i]);
    RI.graphicsDefer.push(packedHitInfoTexture[i]);
    RI.graphicsDefer.push(velocityTexture[i]);
    RI.graphicsDefer.push(decalMulTexture[i]);
    RI.graphicsDefer.push(decalAddTexture[i]);

    RI.graphicsDefer.push(renderTargetView[i]);
    RI.graphicsDefer.push(depthView[i]);
    RI.graphicsDefer.push(depthSampleView[i]);
    RI.graphicsDefer.push(visibilityView[i]);
    RI.graphicsDefer.push(surfelResultView[i]);
    RI.graphicsDefer.push(packedHitInfoView[i]);
    RI.graphicsDefer.push(velocityView[i]);
    RI.graphicsDefer.push(decalMulView[i]);
    RI.graphicsDefer.push(decalAddView[i]);
  }
  for (uint32_t i = 0; i < 2; i++) {
    RI.graphicsDefer.push(directLightingTexture[i]);
    RI.graphicsDefer.push(directKeyTexture[i]);
    RI.graphicsDefer.push(directAtrousTexture[i]);
    RI.graphicsDefer.push(reservoirTexture[i]);

    RI.graphicsDefer.push(directLightingView[i]);
    RI.graphicsDefer.push(directKeyView[i]);
    RI.graphicsDefer.push(directAtrousView[i]);
    RI.graphicsDefer.push(reservoirView[i]);
  }
  RI.graphicsDefer.push(reservoirTemporalTexture);
  RI.graphicsDefer.push(reservoirTemporalView);
}

// Defer our current resources (~HybridViewportState defers each shared handle to
// the graphics freelist), then move-construct rhs's shared handles into us (a
// pointer steal — no refcount churn, no dispose). The resize path uses this as
// `*this = {}` to reset in place.
cViewport::HybridViewportState &
cViewport::HybridViewportState::operator=(HybridViewportState &&rhs) noexcept {
  if (this != &rhs) {
    this->~HybridViewportState();
    new (this) HybridViewportState(std::move(rhs));
  }
  return *this;
}

// Append the per-world light/fog SSBO bindings (set kWorldSet) to a pass's
// descriptor-binding vector. Every pass that reads lights/fog calls this before
// its bindDescriptors; RIProgram routes them to kWorldSet by reflection and
// caches the resulting set — stable world buffers hash to a cache hit, so the
// descriptor set is written once and reused until a re-bake swaps a buffer.
// Names a pass doesn't reflect are skipped, so binding all four everywhere is
// safe. The buffers are always >= 1 element after cWorld::PrepareFrame's bake
// (which runs first in Draw), so a reflected binding is never left unbound.
static void appendWorldLightFog(std::vector<RIProgram::DescriptorBinding> &bnd,
                                cWorld *apWorld) {
  if (!apWorld)
    return;
  auto add = [&](const char *name, RIBuffer *buf, uint32_t cnt, size_t stride) {
    if (!buf)
      return;
    bnd.emplace_back(name, RIDescriptor::storageBuffer(
                               &RI.device, buf, 0,
                               std::max<uint32_t>(cnt, 1u) * stride));
  };
  add("gPointLights", apWorld->GetPointLightBuffer(),
      apWorld->GetPointLightCount(), sizeof(PointLight));
  add("gSpotLights", apWorld->GetSpotLightBuffer(),
      apWorld->GetSpotLightCount(), sizeof(SpotLight));
  add("gAreaLights", apWorld->GetAreaLightBuffer(),
      apWorld->GetAreaLightCount(), sizeof(RectLight));
  add("gFogAreas", apWorld->GetFogAreaBuffer(), apWorld->GetFogAreaCount(),
      sizeof(FogAreaParams));
}

void cHybridRenderer::Draw(RIBootstrap::FrameContext *cntx, cViewport *viewport,
                           float afFrameTime, cFrustum *apFrustum,
                           cWorld *apWorld, cRenderSettings *apSettings,
                           bool abSendFrameBufferToPostEffects) {

  const cVector2l vTargetSize = viewport->GetTargetSize();
  if (vTargetSize.x <= 0 || vTargetSize.y <= 0) {
    return;
  }
  const uint32_t authoredWidth = (uint32_t)vTargetSize.x;
  const uint32_t authoredHeight = (uint32_t)vTargetSize.y;

  cViewport::HybridViewportState *pState =
      viewport->PrepareToRender<cViewport::HybridViewportState>(cntx);
  if (pState == nullptr || pState->width == 0) {
    return;
  }
  cViewport::HybridViewportState &state = *pState;
  const uint32_t renderWidth = state.width; // overscan applied by Update
  const uint32_t renderHeight = state.height;

  ml::float4x4 mainFrustumViewInvMat = apFrustum->GetViewMat();
  mainFrustumViewInvMat.Invert();
  const ml::float4x4 mainFrustumViewMat = apFrustum->GetViewMat();
  ml::float4x4 mainFrustumProjMat = apFrustum->GetProjectionMat();
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
    // Frustum-cull the raster render list: the visibility/gbuffer + translucent
    // passes only need what the camera sees, and culling keeps the per-frame
    // translucent/particle Update* work bounded. Whole-map RT geometry (shadows/
    // GI need everything, including behind the camera) is no longer sourced here
    // — cWorld::PrepareFrame walks its own renderables unculled to build the TLAS.
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

    // The TLAS (now owned by cWorld) can keep referencing the BLAS device
    // addresses of geometry freed on a map transition until it's rebuilt. No
    // per-frame pinning is needed for that: a cVertexBuffer owns its BLAS by
    // value and defers it (RISharedPointer on RI.graphicsDefer) on rebuild and in
    // its destructor, so any BLAS the TLAS can still reference outlives the
    // in-flight window even after its owning renderable is destroyed.
  }

  // --------------------------------------------------------------------
  // Per-frame prepare for every VISIBLE translucent renderable (particles +
  // meshes + billboards + beams). UpdateGraphicsForFrame/ForViewport recompute
  // dynamic geometry (billboard facing, beam stretch, emitter step) and mark the
  // VB dirty; SubmitToGPU then allocates/uploads dirty streams for the raster
  // particle + mesh passes. The render list is frustum-culled above, so only the
  // on-screen set pays this cost. BLAS builds for ray-traced meshes happen in
  // cWorld::PrepareFrame (TLAS owner), not here.
  //
  // Must run BEFORE any vkCmdBeginRendering so the uploader's barriers don't
  // collide with a dynamic-rendering scope.
  for (iRenderable *pObj :
       m_rendererList.GetRenderableItems(eRenderListType_Translucent)) {
    if (!pObj)
      continue;
    pObj->UpdateGraphicsForFrame(afFrameTime);
    pObj->UpdateGraphicsForViewport(apFrustum, afFrameTime);
    cVertexBuffer *pVB = pObj->GetVertexBuffer();
    if (pVB) {
      auto *vbri = static_cast<cVertexBuffer *>(pVB);
      vbri->SubmitToGPU(&RI.blasSubmit.cmds[0], &RI.device, cntx);
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

    cVertexBuffer *pVB = pObj->GetVertexBuffer();
    if (pVB) {
      auto *vbri = static_cast<cVertexBuffer *>(pVB);
      // Decals are never TLAS instances — upload streams, no BLAS.
      vbri->SubmitToGPU(&RI.blasSubmit.cmds[0], &RI.device, cntx);
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
  if (!state.hasPrevCamera) {
    std::memcpy(state.prevViewMat, mainFrustumViewMat.a,
                sizeof(state.prevViewMat));
    std::memcpy(state.prevProjMat, mainFrustumProjMat.a,
                sizeof(state.prevProjMat));
    state.hasPrevCamera = true;
  }
  std::memcpy(perFrame.prevViewMat, state.prevViewMat,
              sizeof(perFrame.prevViewMat));
  std::memcpy(perFrame.prevProjMat, state.prevProjMat,
              sizeof(perFrame.prevProjMat));
  std::memcpy(state.prevViewMat, mainFrustumViewMat.a,
              sizeof(state.prevViewMat));
  std::memcpy(state.prevProjMat, mainFrustumProjMat.a,
              sizeof(state.prevProjMat));
  // viewProjMat = proj * view (column-major); fill via direct ml composition
  // when needed. Leaving as identity-stub for now — first pass writes only
  // visibility; lighting in the FS reads viewMat/invViewMat which are correct.
  perFrame.viewportSize[0] = (float)renderWidth;
  perFrame.viewportSize[1] = (float)renderHeight;
  perFrame.viewTexel[0] = renderWidth ? 1.0f / (float)renderWidth : 0.0f;
  perFrame.viewTexel[1] = renderHeight ? 1.0f / (float)renderHeight : 0.0f;
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
  // World fog — copy the per-world settings into the per-frame UBO so Fog.slang's
  // world-fog block activates. Mirrors cWorld::BuildFogParams colour handling
  // (sRGB->linear, alpha kept linear). Leaving worldFogLength at 0 (the default
  // zero-init) disables world fog, matching the shader's `worldFogLength > 0` guard.
  if (apWorld->GetFogActive()) {
    const cColor fc = apWorld->GetFogColor();
    perFrame.worldFogStart = apWorld->GetFogStart();
    perFrame.worldFogLength = apWorld->GetFogEnd() - apWorld->GetFogStart();
    perFrame.fogFalloffExp = apWorld->GetFogFalloffExp();
    perFrame.worldFogColor = float4{sRGBToLinear(fc.r), sRGBToLinear(fc.g),
                                    sRGBToLinear(fc.b), fc.a};
    perFrame.oneMinusFogAlpha = 1.0f - fc.a;
  }

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
    // Guard band: widen the ray cone by (1+2f) to match the widened projMat
    // above, so the RT primary rays + velocity cover the overscan frame.
    const float uScale = focalLength * tanHalfFov * aspect;
    const float vScale = focalLength * tanHalfFov;

    perFrame.posW = posW;
    perFrame.cameraU = {uScale * rightW.x, uScale * rightW.y,
                        uScale * rightW.z};
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
  // Lights are no longer pulled from the per-frame render list — cWorld owns the
  // per-world light buffers (rebuilt once per frame by cWorld::PrepareFrame,
  // driven from cScene before the viewport loop).
  RISegmentReq indirectReq = {};
  const bool indirectOk =
      m_indirectSegment.request(RI.frameIndex, solids.size(), &indirectReq);
  assert(indirectOk);
  auto *indirectDst = reinterpret_cast<VkDrawIndirectCommand *>(
      static_cast<uint8_t *>(m_indirectDrawBuffer.mappedAddress) +
      (size_t)indirectReq.elementOffset * sizeof(VkDrawIndirectCommand));
  uint32_t writtenDraws = 0;

  // The world's per-frame GPU memory (light/fog/decal buffers, TLAS, bindless
  // object/material slots) is published once per frame by cWorld::PrepareFrame,
  // driven by cScene before the viewport loop — not here. Read the per-world
  // counts it produced into this viewport's SceneConstants.
  perFrame.pointLightCount = apWorld->GetPointLightCount();
  perFrame.spotLightCount = apWorld->GetSpotLightCount();
  perFrame.areaLightCount = apWorld->GetAreaLightCount();
  perFrame.fogAreaCount = apWorld->GetFogAreaCount();
  perFrame.decalCount = apWorld->GetDecalCount();

  for (iRenderable *pObject : solids) {
    cVertexBuffer *pVB = pObject->GetVertexBuffer();
    if (!pVB)
      continue;

    // Object slot for the indirect draw's firstInstance. cWorld::PrepareFrame
    // already submitted this object (same cookie), built its BLAS, and uploaded
    // its geometry for the TLAS this frame, so this is an idempotent cache hit
    // returning the same slot. Skips on material/pool exhaustion.
    const uint32_t slot =
        apWorld->SubmitRenderableObject(pObject, cntx, apFrustum);
    if (slot == UINT32_MAX)
      continue;

    if (writtenDraws < indirectReq.numElements) {
      indirectDst[writtenDraws++] = VkDrawIndirectCommand{
          /*vertexCount   =*/(uint32_t)pVB->GetIndexNum(),
          /*instanceCount =*/1u,
          /*firstVertex   =*/0u,
          /*firstInstance =*/slot,
      };
    }
  }

  // state.packedHitInfoView / m_surfelDepthView and the
  // freshly built TLAS now live on set 1 and are pushed per-dispatch via
  // RIProgram::bindDescriptors below (see the m_surfelVBuffer / m_surfelRT
  // / m_surfelIntegrate / m_surfelGenerate / m_mainComposite call sites).
  // Set 1 is allocated from a frame-rotated pool, so each frame's writes
  // land on an idle descriptor set.

  std::vector<RIProgram::DescriptorBinding> bindings;
  bindings.reserve(16);
  // Surfel image / TLAS bindings are pushed inline below. Note: the storage
  // images (`gPackedHitInfo` / `gSurfelDepthMap`) and the matching sampled view
  // (`gSurfelDepth`) all use GENERAL layout — the depth image stays GENERAL
  // across the frame, which satisfies both storage and sampled access.
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
    RITextureBarrier attachmentBarriers[3] = {
        {state.visibilityTexture[RI.swapchainIndex].Get(),
         RI_RESOURCE_STATE_UNDEFINED, RI_RESOURCE_STATE_RENDER_TARGET},
        {state.depthTextures[RI.swapchainIndex].Get(), RI_RESOURCE_STATE_UNDEFINED,
         RI_RESOURCE_STATE_DEPTH_WRITE, RI_STAGE_NONE, RI_STAGE_NONE,
         RI_BARRIER_ASPECT_DEPTH},
        // Velocity MRT — same UNDEFINED→COLOR transition as the visibility
        // target (loadOp=CLEAR, so prior contents don't matter).
        {state.velocityTexture[RI.swapchainIndex].Get(), RI_RESOURCE_STATE_UNDEFINED,
         RI_RESOURCE_STATE_RENDER_TARGET}};
    RI.primary.cmds[0].vk_d3d12_textureBarriers<3>(3, attachmentBarriers);
  }

  // MRT color targets, both cleared to all-zero. Visibility: psMain writes .w=1
  // (valid hit sentinel) or zero on sky/miss pixels (clear value). Velocity:
  // static/uncovered pixels read zero motion. (uint vs float clear is
  // bit-identical at zero.)
  RIRenderingAttachment gbufferColorAttachments[2] = {};
  gbufferColorAttachments[0].view = *state.visibilityView[RI.swapchainIndex];
  gbufferColorAttachments[0].loadOp = RI_ATTACHMENT_LOAD_OP_CLEAR;
  gbufferColorAttachments[0].storeOp = RI_ATTACHMENT_STORE_OP_STORE;
  gbufferColorAttachments[1].view = *state.velocityView[RI.swapchainIndex];
  gbufferColorAttachments[1].loadOp = RI_ATTACHMENT_LOAD_OP_CLEAR;
  gbufferColorAttachments[1].storeOp = RI_ATTACHMENT_STORE_OP_STORE;

  // MRT owns the per-frame depth clear.
  RIRenderingAttachment depthAttachment = {};
  depthAttachment.view = *state.depthView[RI.swapchainIndex];
  depthAttachment.loadOp = RI_ATTACHMENT_LOAD_OP_CLEAR;
  depthAttachment.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
  depthAttachment.clearValue.depth = 1.0f;

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
    RIGpuScope _gsSurfelPrepare(&RI.profiler, &RI.primary.cmds[0], "SurfelPrepare");
    m_surfelPrepare.bindComputePipeline(&RI.device, &RI.primary.cmds[0], kHash,
                                        "SurfelPreparePass.cs", &computeCreate);
    m_surfelPrepare.bindBindlessDescriptorSet(&RI.primary.cmds[0],
                                              &RI.globalset->m_bindlessSet, 0,
                                              VK_PIPELINE_BIND_POINT_COMPUTE);
    RI.primary.cmds[0].dispatch(&RI.device, 1u, 1u, 1u);
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
    RI.primary.cmds[0].copyBuffer(
        &RI.device, &RI.globalset->m_surfelValidBuffer, 0,
        &RI.globalset->m_surfelDirtyIndexBuffer, 0,
        (RIDeviceSize)kTotalSurfelLimit * sizeof(uint32_t));
  }
  {
    RI.primary.cmds[0].vk_d3d12_memoryBarrier(
        {RI_RESOURCE_STATE_STORAGE_WRITE | RI_RESOURCE_STATE_COPY_DST,
         RI_RESOURCE_STATE_UNORDERED_ACCESS, RI_STAGE_COMPUTE | RI_STAGE_COPY,
         RI_STAGE_COMPUTE});
  }
  // Cell + ref-counter clears are now done by SurfelPreparePass +
  // SurfelUpdatePass on the Slang side. No standalone clear dispatch.
  {
    RI.primary.cmds[0].vk_d3d12_memoryBarrier(
        {RI_RESOURCE_STATE_STORAGE_WRITE, RI_RESOURCE_STATE_UNORDERED_ACCESS,
         RI_STAGE_COMPUTE, RI_STAGE_COMPUTE});
  }
  // Stage B (packedHitInfo) is now produced by the raster G-buffer +
  // SurfelPomBary compute pass later in the frame. SurfelUpdate (Stage D)
  // reads cached geometry anchors (gSurfelGeometryBuffer), not gPackedHitInfo,
  // so it does not need the V-buffer this early.

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
    RIGpuScope _gsSurfelCollect(&RI.profiler, &RI.primary.cmds[0], "SurfelUpdateCollect");
    m_surfelUpdateCollect.bindComputePipeline(
        &RI.device, &RI.primary.cmds[0], kHash,
        "SurfelUpdatePass.cs:collectCellInfo", &computeCreate);
    m_surfelUpdateCollect.bindBindlessDescriptorSet(
        &RI.primary.cmds[0], &RI.globalset->m_bindlessSet, 0,
        VK_PIPELINE_BIND_POINT_COMPUTE);

    std::vector<RIProgram::DescriptorBinding> bnd;
    bnd.reserve(1);
    {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("gPerFrame");
      RI.UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
      bnd.push_back(b);
    }
    m_surfelUpdateCollect.bindDescriptors(&RI.device, &RI.primary.cmds[0],
                                          RI.frameIndex, bnd.data(), bnd.size(),
                                          VK_PIPELINE_BIND_POINT_COMPUTE);

    RI.primary.cmds[0].dispatch(&RI.device, (kTotalSurfelLimit + 63u) / 64u,
                                1u, 1u);
  }
  // RAW: accumulate reads cellInfo.surfelCount (collect-written) and reads
  // surfelCounter (collect-incremented). Accumulate's own writes are synced by
  // the next barrier — dstAccess=READ is sufficient and avoids an L2 flush of
  // every other SSBO collect touched (valid/free/recycle/rayResult/refCounter).
  {
    RI.primary.cmds[0].vk_d3d12_memoryBarrier(
        {RI_RESOURCE_STATE_STORAGE_WRITE, RI_RESOURCE_STATE_STORAGE_READ,
         RI_STAGE_COMPUTE, RI_STAGE_COMPUTE});
  }
  {
    VkComputePipelineCreateInfo computeCreate = {
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    RIGpuScope _gsSurfelAccum(&RI.profiler, &RI.primary.cmds[0], "SurfelUpdateAccumulate");
    m_surfelUpdateAccumulate.bindComputePipeline(
        &RI.device, &RI.primary.cmds[0], kHash,
        "SurfelUpdatePass.cs:accumulateCellInfo", &computeCreate);
    m_surfelUpdateAccumulate.bindBindlessDescriptorSet(
        &RI.primary.cmds[0], &RI.globalset->m_bindlessSet, 0,
        VK_PIPELINE_BIND_POINT_COMPUTE);
    const uint32_t groups = (kCellDimension + 3u) / 4u;
    RI.primary.cmds[0].dispatch(&RI.device, groups, groups, groups);
  }
  // RAW: scatter reads cellInfo.cellToSurfelBufferOffset (accumulate-written)
  // and RMWs cellInfo.surfelCount (accumulate zeroed it). Scatter's writes are
  // synced by the next barrier. Atomic RMWs only need the prior write visible
  // for the read half — dstAccess=READ suffices.
  {
    RI.primary.cmds[0].vk_d3d12_memoryBarrier(
        {RI_RESOURCE_STATE_STORAGE_WRITE, RI_RESOURCE_STATE_STORAGE_READ,
         RI_STAGE_COMPUTE, RI_STAGE_COMPUTE});
  }
  {
    VkComputePipelineCreateInfo computeCreate = {
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    RIGpuScope _gsSurfelScatter(&RI.profiler, &RI.primary.cmds[0], "SurfelUpdateScatter");
    m_surfelUpdateScatter.bindComputePipeline(
        &RI.device, &RI.primary.cmds[0], kHash,
        "SurfelUpdatePass.cs:updateCellToSurfelBuffer", &computeCreate);
    m_surfelUpdateScatter.bindBindlessDescriptorSet(
        &RI.primary.cmds[0], &RI.globalset->m_bindlessSet, 0,
        VK_PIPELINE_BIND_POINT_COMPUTE);

    std::vector<RIProgram::DescriptorBinding> bnd;
    bnd.reserve(1);
    {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("gPerFrame");
      RI.UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
      bnd.push_back(b);
    }
    m_surfelUpdateScatter.bindDescriptors(&RI.device, &RI.primary.cmds[0],
                                          RI.frameIndex, bnd.data(), bnd.size(),
                                          VK_PIPELINE_BIND_POINT_COMPUTE);

    RI.primary.cmds[0].dispatch(&RI.device, (kTotalSurfelLimit + 63u) / 64u,
                                1u, 1u);
  }
  {
    RI.primary.cmds[0].vk_d3d12_memoryBarrier(
        {RI_RESOURCE_STATE_STORAGE_WRITE, RI_RESOURCE_STATE_STORAGE_READ,
         RI_STAGE_COMPUTE,
         RI_STAGE_COMPUTE | RI_STAGE_RAY_TRACING | RI_STAGE_FRAGMENT});
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
    RIGpuScope _gsLightGridBin(&RI.profiler, &RI.primary.cmds[0], "LightGridBin");
    m_lightGridBin.bindComputePipeline(&RI.device, &RI.primary.cmds[0], kHash,
                                       "LightGridBuildPass.cs:binLights",
                                       &computeCreate);
    m_lightGridBin.bindBindlessDescriptorSet(&RI.primary.cmds[0],
                                             &RI.globalset->m_bindlessSet, 0,
                                             VK_PIPELINE_BIND_POINT_COMPUTE);
    std::vector<RIProgram::DescriptorBinding> bnd;
    bnd.reserve(1);
    {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("gPerFrame");
      RI.UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
      bnd.push_back(b);
    }
    appendWorldLightFog(bnd, apWorld);
    m_lightGridBin.bindDescriptors(&RI.device, &RI.primary.cmds[0],
                                   RI.frameIndex, bnd.data(), bnd.size(),
                                   VK_PIPELINE_BIND_POINT_COMPUTE);
    // One thread per grid cell (the shader early-outs past
    // kLightGridCellCount).
    RI.primary.cmds[0].dispatch(&RI.device, (kLightGridCellCount + 63u) / 64u,
                                1u, 1u);
  }

  {
    // binLights writes are read by two consumers later this frame: the surfel
    // ray-trace NEE (ray tracing) and the MainCompositePass direct-lighting
    // cull (fragment — SurfelShade.evalAnalyticLight walks the per-cell light
    // list). Both stages must be in dst or the fragment reads see an empty grid
    // and drop every point/spot light. Compute kept in dst for safety.
    RI.primary.cmds[0].vk_d3d12_memoryBarrier(
        {RI_RESOURCE_STATE_STORAGE_WRITE, RI_RESOURCE_STATE_STORAGE_READ,
         RI_STAGE_COMPUTE,
         RI_STAGE_RAY_TRACING | RI_STAGE_COMPUTE | RI_STAGE_FRAGMENT});
  }

  // One-shot UNDEFINED -> GENERAL transition for the surfel depth atlas the
  // first time each swapchain image appears. Subsequent frames skip this —
  // the atlas stays in GENERAL for life so integrate's EMA-blend reads keep
  // working and cross-frame sync goes through the engine frame fence.
  if (!m_surfelRTIndirectInit) {
    // Seed the fixed height=1 and depth=1 words once. Width (offset 0) is
    // overwritten each frame from the surfel counter via vkCmdCopyBuffer.
    vkCmdFillBuffer(RI.primary.cmds[0].vk.cmd, m_surfelRTIndirectBuf.vk.buffer,
                    4u, 4u, 1u);  // height = 1 at offset 4
    vkCmdFillBuffer(RI.primary.cmds[0].vk.cmd, m_surfelRTIndirectBuf.vk.buffer,
                    8u, 4u, 1u);  // depth  = 1 at offset 8
    m_surfelRTIndirectInit = true;
  }

  if (!m_surfelAtlasesInitialized[RI.swapchainIndex]) {
    // First touch of this swapchain image's atlas: UNDEFINED -> GENERAL and
    // *seed its contents*. SurfelIntegratePass accumulates into it via an EMA
    // read-modify-write (gSurfelDepthMap), so the starting value must be
    // defined — UNDEFINED discards it, leaving uninitialized / NaN reads that
    // poison the EMA and the generation pass's Chebyshev weight. The depth
    // atlas stores (E[z], E[z^2]) per octahedral texel; seed E[z^2] high so
    // the generation pass's weight
    //   variance / (variance + (dist - mean)^2),  variance = E[z^2] - E[z]^2
    // starts ~1 (no visibility suppression) and tightens only as real
    // first-bounce depths converge. Clearing to 0 instead would make variance=0
    // => weight 0 => every surfel contribution zeroed until the atlas fills in
    // (~hundreds of frames), i.e. a multi-second indirect black-out on enable.
    RITextureBarrier toClear[1] = {{&m_surfelDepthTexture[RI.swapchainIndex],
                                    RI_RESOURCE_STATE_UNDEFINED,
                                    RI_RESOURCE_STATE_CLEAR_STORAGE}};
    RI.primary.cmds[0].vk_d3d12_textureBarriers<1>(1, toClear);

    // (E[z], E[z^2] seeded high (half-float safe), 0, 0)
    const float depthClear[4] = {0.0f, 60000.0f, 0.0f, 0.0f};
    RI.primary.cmds[0].clearStorageImage(
        &RI.device, &m_surfelDepthTexture[RI.swapchainIndex], depthClear);

    // Clear (transfer) -> integrate's storage RW + ray-trace / generation reads
    // (sampled). Same GENERAL layout, availability/visibility only.
    RITextureBarrier toShader[1] = {
        {&m_surfelDepthTexture[RI.swapchainIndex],
         RI_RESOURCE_STATE_CLEAR_STORAGE,
         RI_RESOURCE_STATE_UNORDERED_ACCESS | RI_RESOURCE_STATE_SHADER_RESOURCE,
         RI_STAGE_NONE, RI_STAGE_COMPUTE | RI_STAGE_RAY_TRACING}};
    RI.primary.cmds[0].vk_d3d12_textureBarriers<1>(1, toShader);
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
  // gMaxRayCount = 64; the actual requested count lives in
  // gSurfelCounter[kSurfelCounterRequestedRay] and is copied into the
  // indirect args buffer so only the requested rays are launched.
  // ----------------------------------------------------------------------
  if (apWorld->GetTlas() != nullptr) {
    // Copy the requested ray count (4 bytes) from the surfel counter buffer
    // into m_surfelRTIndirectBuf.width (offset 0). Height and depth were
    // seeded to 1 once at m_surfelRTIndirectInit time above.
    {
      VkBufferCopy region = {};
      region.srcOffset = static_cast<VkDeviceSize>(kSurfelCounterRequestedRay) * sizeof(uint32_t);
      region.dstOffset = 0u;
      region.size      = sizeof(uint32_t);
      vkCmdCopyBuffer(RI.primary.cmds[0].vk.cmd,
                      RI.globalset->m_surfelCounterBuffer.vk.buffer,
                      m_surfelRTIndirectBuf.vk.buffer, 1u, &region);
    }
    // TRANSFER_WRITE → INDIRECT_COMMAND_READ before the RT dispatch reads the args.
    RI.primary.cmds[0].vk_d3d12_memoryBarrier(
        {RI_RESOURCE_STATE_COPY_DST, RI_RESOURCE_STATE_INDIRECT_ARGUMENT,
         RI_STAGE_COPY, RI_STAGE_DRAW_INDIRECT | RI_STAGE_RAY_TRACING});

    VkRayTracingPipelineCreateInfoKHR rtCreate = {
        VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
    rtCreate.maxPipelineRayRecursionDepth = 1;
    const hash_t kRtHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    RIGpuScope _gsSurfelRT(&RI.profiler, &RI.primary.cmds[0], "SurfelRT");
    m_surfelRT.bindRayTracingPipeline(&RI.device, &RI.primary.cmds[0], kRtHash,
                                      "SurfelRayTrace.rt", &rtCreate);
    m_surfelRT.bindBindlessDescriptorSet(
        &RI.primary.cmds[0], &RI.globalset->m_bindlessSet, 0,
        VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR);

    std::vector<RIProgram::DescriptorBinding> rtBnd;
    rtBnd.reserve(2);
    {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("gPerFrame");
      RI.UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
      rtBnd.push_back(b);
    }
    rtBnd.emplace_back("gRtAccel",
                       RIDescriptor::accelerationStructure(&RI.device, apWorld->GetTlas()));
    appendWorldLightFog(rtBnd, apWorld);
    m_surfelRT.bindDescriptors(&RI.device, &RI.primary.cmds[0], RI.frameIndex,
                               rtBnd.data(), rtBnd.size(),
                               VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR);

    const VkDeviceAddress indirectAddr = m_surfelRTIndirectBuf.GetDeviceHandle(&RI.device);
    m_surfelRT.traceRaysIndirect(&RI.primary.cmds[0], kRtHash, indirectAddr);
  }
  {
    RI.primary.cmds[0].vk_d3d12_memoryBarrier(
        {RI_RESOURCE_STATE_STORAGE_WRITE, RI_RESOURCE_STATE_STORAGE_READ,
         RI_STAGE_RAY_TRACING, RI_STAGE_COMPUTE | RI_STAGE_FRAGMENT});
  }

  RIBeginRenderingDesc gbufferBeginDesc = {};
  gbufferBeginDesc.renderArea.width = (int16_t)renderWidth;
  gbufferBeginDesc.renderArea.height = (int16_t)renderHeight;
  gbufferBeginDesc.colorCount = 2;
  gbufferBeginDesc.colors = gbufferColorAttachments;
  gbufferBeginDesc.depthStencil = &depthAttachment;
  RIGpuScope _gsGBuffer(&RI.profiler, &RI.primary.cmds[0], "GBuffer");
  RI.primary.cmds[0].vk_d3d12_beginRendering(&RI.device, gbufferBeginDesc);

  RIViewport vkViewport = {};
  vkViewport.x = 0.0f;
  vkViewport.y = (float)renderHeight;
  vkViewport.width = (float)renderWidth;
  vkViewport.height = -(float)renderHeight;
  vkViewport.depthMin = 0.0f;
  vkViewport.depthMax = 1.0f;
  RIRect scissor = {};
  scissor.width = (int16_t)renderWidth;
  scissor.height = (int16_t)renderHeight;
  RI.primary.cmds[0].setViewport(&RI.device, vkViewport);
  RI.primary.cmds[0].setScissor(&RI.device, scissor);

  if (writtenDraws > 0) {
    GBufferMRTPipelineDesc pipelineDesc(RIBootstrap::VisibilityFormat,
                                        RIBootstrap::VelocityFormat,
                                        RIBootstrap::DepthFormat);
    m_gbuffer.bindPipeline(&RI.device, &RI.primary.cmds[0], pipelineDesc.hash,
                           "SurfelGBuffer.3d", &pipelineDesc.createInfo);
    m_gbuffer.bindBindlessDescriptorSet(&RI.primary.cmds[0],
                                        &RI.globalset->m_bindlessSet, 0);
    m_gbuffer.bindDescriptors(&RI.device, &RI.primary.cmds[0], RI.frameIndex,
                              bindings.data(), bindings.size());
    RI.primary.cmds[0].drawIndirect(
        &RI.device, &m_indirectDrawBuffer,
        (VkDeviceSize)indirectReq.elementOffset * sizeof(VkDrawIndirectCommand),
        writtenDraws, (uint32_t)sizeof(VkDrawIndirectCommand));
  }

  RI.primary.cmds[0].vk_d3d12_endRendering(&RI.device);

  // Gbuffer output -> SHADER_READ_ONLY for the surfel-generation compute
  // pass (and any later fragment consumer). Includes depth, which the
  // gbuffer left in DEPTH_STENCIL_ATTACHMENT_OPTIMAL. The surfel result
  // image transitions UNDEFINED -> GENERAL for its first compute write.
  // packedHitInfoTexture transitions UNDEFINED -> STORAGE_WRITE for the
  // POM compute pass that follows immediately.
  {
    RITextureBarrier toRead[5] = {};
    // Visibility -> SHADER_READ for the fragment + compute consumers.
    toRead[0] = {state.visibilityTexture[RI.swapchainIndex].Get(),
                 RI_RESOURCE_STATE_RENDER_TARGET,
                 RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_NONE,
                 RI_STAGE_FRAGMENT | RI_STAGE_COMPUTE};

    // Depth -> SHADER_READ_ONLY for the compute pass.
    toRead[1] = {state.depthTextures[RI.swapchainIndex].Get(),
                 RI_RESOURCE_STATE_DEPTH_WRITE,
                 RI_RESOURCE_STATE_SHADER_RESOURCE,
                 RI_STAGE_NONE,
                 RI_STAGE_COMPUTE,
                 RI_BARRIER_ASPECT_DEPTH};

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
    toRead[2] = {state.surfelResultTexture[RI.swapchainIndex].Get(),
                 RI_RESOURCE_STATE_UNDEFINED, RI_RESOURCE_STATE_CLEAR_STORAGE};

    // Velocity (gbuffer MRT) -> SHADER_READ for the direct-lighting pass.
    toRead[3] = {state.velocityTexture[RI.swapchainIndex].Get(),
                 RI_RESOURCE_STATE_RENDER_TARGET,
                 RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_NONE,
                 RI_STAGE_COMPUTE};

    // packedHitInfo: UNDEFINED -> STORAGE_WRITE for the POM compute pass.
    // Previously written by the Stage B RT V-buffer; now produced by
    // SurfelPomBary.cs immediately after this barrier.
    toRead[4] = {state.packedHitInfoTexture[RI.swapchainIndex].Get(),
                 RI_RESOURCE_STATE_UNDEFINED, RI_RESOURCE_STATE_STORAGE_WRITE,
                 RI_STAGE_NONE, RI_STAGE_COMPUTE};

    RI.primary.cmds[0].vk_d3d12_textureBarriers<5>(5, toRead);
  }

  // Clear the surfel-result image to zero indirect (see toRead[2] above), then
  // make the clear visible to surfel_generation_pass's storage write.
  {
    // (0,0,0,1): zero radiance, opaque alpha
    const float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    RI.primary.cmds[0].clearStorageImage(
        &RI.device, state.surfelResultTexture[RI.swapchainIndex].Get(), clearColor);

    RI.primary.cmds[0].vk_d3d12_textureBarrier(
        {state.surfelResultTexture[RI.swapchainIndex].Get(),
         RI_RESOURCE_STATE_CLEAR_STORAGE, RI_RESOURCE_STATE_UNORDERED_ACCESS,
         RI_STAGE_NONE, RI_STAGE_COMPUTE});
  }

  // ----------------------------------------------------------------------
  // Stage B — POM barycentric correction (replaces the old RT V-buffer).
  //
  // Copies visibilityTexture (raw raster hit, gPackedHitInfoRaster) into
  // packedHitInfoTexture (gPackedHitInfo) and applies the parallax-occlusion
  // barycentric perturbation for height-mapped diffuse surfaces. Water/glass
  // refraction is not handled here; those pixels carry the raster surface
  // hit in gPackedHitInfo until a future sparse refraction RT pass lands.
  // ----------------------------------------------------------------------
  {
    RIGpuScope _gsSurfelPomBary(&RI.profiler, &RI.primary.cmds[0], "SurfelPomBary");
    VkComputePipelineCreateInfo ci = {
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    const hash_t kPomHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    m_surfelPomBary.bindComputePipeline(&RI.device, &RI.primary.cmds[0],
                                        kPomHash, "SurfelPomBary.cs", &ci);
    m_surfelPomBary.bindBindlessDescriptorSet(
        &RI.primary.cmds[0], &RI.globalset->m_bindlessSet, 0,
        VK_PIPELINE_BIND_POINT_COMPUTE);

    std::vector<RIProgram::DescriptorBinding> pomBnd;
    pomBnd.reserve(3);
    {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("gPerFrame");
      RI.UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
      pomBnd.push_back(b);
    }
    pomBnd.emplace_back(
        "gPackedHitInfoRaster",
        RIDescriptor::sampledImage(&RI.device,
                                   state.visibilityView[RI.swapchainIndex].Get(),
                                   RI_RESOURCE_STATE_SHADER_RESOURCE));
    pomBnd.emplace_back(
        "gPackedHitInfo",
        RIDescriptor::storageImage(&RI.device,
                                   state.packedHitInfoView[RI.swapchainIndex].Get()));

    m_surfelPomBary.bindDescriptors(&RI.device, &RI.primary.cmds[0],
                                    RI.frameIndex, pomBnd.data(), pomBnd.size(),
                                    VK_PIPELINE_BIND_POINT_COMPUTE);
    RI.primary.cmds[0].dispatch(&RI.device, (renderWidth + 15u) / 16u,
                                (renderHeight + 15u) / 16u, 1u);
  }
  {
    // packedHitInfo storage write -> shader read for integrate/generate/
    // direct-lighting/composite passes. Layout stays GENERAL.
    RI.primary.cmds[0].vk_d3d12_memoryBarrier(
        {RI_RESOURCE_STATE_STORAGE_WRITE, RI_RESOURCE_STATE_STORAGE_READ,
         RI_STAGE_COMPUTE,
         RI_STAGE_COMPUTE | RI_STAGE_FRAGMENT | RI_STAGE_RAY_TRACING});
  }

  // ----------------------------------------------------------------------
  // Surfel integrate + generation.
  //   integrate: per valid surfel, MSME-blend the per-frame raytraced radiance
  //              (gSurfelRayResultBuffer) into surfel.radiance.
  //   generate:  per pixel, walk the visibility cell's surfel list, write the
  //              indirect term into state.surfelResultTexture (sampled by the
  //              composite), and spawn / recycle surfels by coverage.
  // ----------------------------------------------------------------------
  {
    VkComputePipelineCreateInfo computeCreate = {
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    RIGpuScope _gsSurfelIntegrate(&RI.profiler, &RI.primary.cmds[0], "SurfelIntegrate");
    m_surfelIntegrate.bindComputePipeline(&RI.device, &RI.primary.cmds[0],
                                          kHash, "SurfelIntegratePass.cs",
                                          &computeCreate);
    m_surfelIntegrate.bindBindlessDescriptorSet(&RI.primary.cmds[0],
                                                &RI.globalset->m_bindlessSet, 0,
                                                VK_PIPELINE_BIND_POINT_COMPUTE);

    // gSurfelDepthSampler is bindless (set 0). The image views and the
    // CB are gone — params come from Constants.h. gPerFrame and the depth
    // atlas (RW + sampled views of the same image) push into set 1.
    std::vector<RIProgram::DescriptorBinding> bnd;
    bnd.reserve(3);
    {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("gPerFrame");
      RI.UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
      bnd.push_back(b);
    }
    bnd.emplace_back("gSurfelDepthMap",
                     RIDescriptor::storageImage(&RI.device,
                                                &m_surfelDepthView[RI.swapchainIndex]));
    bnd.emplace_back("gSurfelDepth",
                     RIDescriptor::sampledImage(&RI.device,
                                                &m_surfelDepthView[RI.swapchainIndex],
                                                RI_RESOURCE_STATE_GENERAL));
    m_surfelIntegrate.bindDescriptors(&RI.device, &RI.primary.cmds[0],
                                      RI.frameIndex, bnd.data(), bnd.size(),
                                      VK_PIPELINE_BIND_POINT_COMPUTE);
    RI.primary.cmds[0].dispatch(&RI.device, (kTotalSurfelLimit + 31u) / 32u,
                                1u, 1u);
  }
  {
    // SHADER_RESOURCE: with USE_SURFEL_DEPTH the generation pass samples the
    // depth atlas (gSurfelDepth) that integrate just wrote via a storage image;
    // the sampled read needs the write made visible to it, not just storage
    // reads.
    RI.primary.cmds[0].vk_d3d12_memoryBarrier(
        {RI_RESOURCE_STATE_STORAGE_WRITE,
         RI_RESOURCE_STATE_UNORDERED_ACCESS | RI_RESOURCE_STATE_SHADER_RESOURCE,
         RI_STAGE_COMPUTE, RI_STAGE_COMPUTE});
  }
  {
    VkComputePipelineCreateInfo computeCreate = {
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    RIGpuScope _gsSurfelGenerate(&RI.profiler, &RI.primary.cmds[0], "SurfelGenerate");
    m_surfelGenerate.bindComputePipeline(&RI.device, &RI.primary.cmds[0], kHash,
                                         "SurfelGenerationPass.cs",
                                         &computeCreate);
    m_surfelGenerate.bindBindlessDescriptorSet(&RI.primary.cmds[0],
                                               &RI.globalset->m_bindlessSet, 0,
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
    bnd.emplace_back("gPackedHitInfo",
                     RIDescriptor::storageImage(
                         &RI.device, state.packedHitInfoView[RI.swapchainIndex].Get()));
    bnd.emplace_back("gSurfelDepth",
                     RIDescriptor::sampledImage(&RI.device,
                                                &m_surfelDepthView[RI.swapchainIndex],
                                                RI_RESOURCE_STATE_GENERAL));
    {
      bnd.emplace_back("gOutput",
                       RIDescriptor::storageImage(
                           &RI.device,
                           state.surfelResultView[RI.swapchainIndex].Get()));
    }
    m_surfelGenerate.bindDescriptors(&RI.device, &RI.primary.cmds[0],
                                     RI.frameIndex, bnd.data(), bnd.size(),
                                     VK_PIPELINE_BIND_POINT_COMPUTE);

    static_assert(sizeof(OverlayPushConstants) == 4);
    const OverlayPushConstants push{ m_overlayMode };
    vkCmdPushConstants(RI.primary.cmds[0].vk.cmd, m_surfelGenerate.getPipelineLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);

    const uint32_t fullW = renderWidth;
    const uint32_t fullH = renderHeight;
    RI.primary.cmds[0].dispatch(&RI.device, (fullW + 15u) / 16u,
                                (fullH + 15u) / 16u, 1u);
  }

  // --------------------------------------------------------------------
  // DirectAtrousPass — SVGF-lite edge-aware à-trous spatial denoise.
  // DirectLightingPass + DirectSpatialReusePass were moved before SurfelRayTrace
  // above so gDirectLighting[dlCur] is ready for surfel bounce NEE screen-space
  // reuse. This block only runs the atrous denoise on that already-resolved buffer.
  // --------------------------------------------------------------------
  // The image the composite samples for direct lighting: the raw accumulation
  // when à-trous is disabled, else the final à-trous iteration's output.
  RITextureView *directResultView = nullptr;
  {
    const uint32_t dlCur = state.directLightingIndex;
    const uint32_t dlPrev = dlCur ^ 1u;

    if (!state.directLightingInit) {
      // First use: the colour + key ping-pong textures UNDEFINED -> GENERAL +
      // cleared so the history reads are defined; they stay GENERAL thereafter.
      RITextureBarrier toGen[9] = {
          {state.directLightingTexture[0].Get(), RI_RESOURCE_STATE_UNDEFINED,
           RI_RESOURCE_STATE_CLEAR_STORAGE},
          {state.directLightingTexture[1].Get(), RI_RESOURCE_STATE_UNDEFINED,
           RI_RESOURCE_STATE_CLEAR_STORAGE},
          {state.directKeyTexture[0].Get(), RI_RESOURCE_STATE_UNDEFINED,
           RI_RESOURCE_STATE_CLEAR_STORAGE},
          {state.directKeyTexture[1].Get(), RI_RESOURCE_STATE_UNDEFINED,
           RI_RESOURCE_STATE_CLEAR_STORAGE},
          {state.directAtrousTexture[0].Get(), RI_RESOURCE_STATE_UNDEFINED,
           RI_RESOURCE_STATE_CLEAR_STORAGE},
          {state.directAtrousTexture[1].Get(), RI_RESOURCE_STATE_UNDEFINED,
           RI_RESOURCE_STATE_CLEAR_STORAGE},
          {state.reservoirTexture[0].Get(), RI_RESOURCE_STATE_UNDEFINED,
           RI_RESOURCE_STATE_CLEAR_STORAGE},
          {state.reservoirTexture[1].Get(), RI_RESOURCE_STATE_UNDEFINED,
           RI_RESOURCE_STATE_CLEAR_STORAGE},
          {state.reservoirTemporalTexture.Get(), RI_RESOURCE_STATE_UNDEFINED,
           RI_RESOURCE_STATE_CLEAR_STORAGE}};
      RI.primary.cmds[0].vk_d3d12_textureBarriers<9>(9, toGen);

      const float clr[4] = {0.0f, 0.0f, 0.0f, 0.0f};
      for (uint32_t i = 0; i < 9; ++i)
        RI.primary.cmds[0].clearStorageImage(&RI.device, toGen[i].texture, clr);

      RI.primary.cmds[0].vk_d3d12_memoryBarrier(
          {RI_RESOURCE_STATE_CLEAR_STORAGE,
           RI_RESOURCE_STATE_SHADER_RESOURCE | RI_RESOURCE_STATE_STORAGE_WRITE,
           RI_STAGE_NONE, RI_STAGE_COMPUTE});
      state.directLightingInit = true;
    } else {
      // Make last frame's writes to the ping-pong textures visible (history
      // sampled-read + current write-after-read/write). Both stay GENERAL.
      RI.primary.cmds[0].vk_d3d12_memoryBarrier(
          {RI_RESOURCE_STATE_STORAGE_WRITE | RI_RESOURCE_STATE_SHADER_RESOURCE,
           RI_RESOURCE_STATE_SHADER_RESOURCE | RI_RESOURCE_STATE_STORAGE_WRITE,
           RI_STAGE_COMPUTE, RI_STAGE_COMPUTE});
    }

    VkComputePipelineCreateInfo ci = {
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    RIGpuScope _gsDirectLighting(&RI.profiler, &RI.primary.cmds[0], "DirectLighting");
    m_directLighting.bindComputePipeline(&RI.device, &RI.primary.cmds[0], kHash,
                                         "DirectLightingPass.cs", &ci);
    m_directLighting.bindBindlessDescriptorSet(&RI.primary.cmds[0],
                                               &RI.globalset->m_bindlessSet, 0,
                                               VK_PIPELINE_BIND_POINT_COMPUTE);

    std::vector<RIProgram::DescriptorBinding> bnd;
    bnd.reserve(8);
    {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("gPerFrame");
      RI.UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
      bnd.push_back(b);
    }
    // Temporal pass traces no rays — only builds + reprojects reservoirs.
    bnd.emplace_back("gPackedHitInfo",
                     RIDescriptor::storageImage(
                         &RI.device, state.packedHitInfoView[RI.swapchainIndex].Get()));
    bnd.emplace_back("gPackedHitInfoRaster",
                     RIDescriptor::sampledImage(
                         &RI.device, state.visibilityView[RI.swapchainIndex].Get(),
                         RI_RESOURCE_STATE_SHADER_RESOURCE));
    bnd.emplace_back("gVelocity",
                     RIDescriptor::sampledImage(
                         &RI.device, state.velocityView[RI.swapchainIndex].Get(),
                         RI_RESOURCE_STATE_SHADER_RESOURCE));
    bnd.emplace_back("gReservoirHistory",
                     RIDescriptor::sampledImage(&RI.device,
                                                state.reservoirView[dlPrev].Get(),
                                                RI_RESOURCE_STATE_GENERAL));
    bnd.emplace_back("gDirectKeyHistory",
                     RIDescriptor::sampledImage(&RI.device,
                                                state.directKeyView[dlPrev].Get(),
                                                RI_RESOURCE_STATE_GENERAL));
    bnd.emplace_back("gReservoirOut",
                     RIDescriptor::storageImage(&RI.device,
                                                state.reservoirTemporalView.Get()));
    bnd.emplace_back("gDirectKeyOut",
                     RIDescriptor::storageImage(&RI.device,
                                                state.directKeyView[dlCur].Get()));

    appendWorldLightFog(bnd, apWorld);
    m_directLighting.bindDescriptors(&RI.device, &RI.primary.cmds[0],
                                     RI.frameIndex, bnd.data(), bnd.size(),
                                     VK_PIPELINE_BIND_POINT_COMPUTE);
    RI.primary.cmds[0].dispatch(&RI.device, (renderWidth + 15u) / 16u,
                                (renderHeight + 15u) / 16u, 1u);

    // Temporal reservoir + current key writes -> spatial pass sampled reads.
    RI.primary.cmds[0].vk_d3d12_memoryBarrier(
        {RI_RESOURCE_STATE_STORAGE_WRITE, RI_RESOURCE_STATE_SHADER_RESOURCE,
         RI_STAGE_COMPUTE, RI_STAGE_COMPUTE});

    // ----------------------------------------------------------------
    // DirectSpatialReusePass — ReSTIR DI spatial reuse + resolve. Merges a few
    // same-surface neighbours' reservoirs, then traces ONE soft shadow ray for
    // the chosen light to demodulated irradiance. Writes reservoir[dlCur] (next
    // frame's temporal history) and directLighting[dlCur] (the à-trous input).
    // ----------------------------------------------------------------
    {
      RIGpuScope _gsDirectSpatialReuse(&RI.profiler, &RI.primary.cmds[0], "DirectSpatialReuse");
      m_directSpatialReuse.bindComputePipeline(
          &RI.device, &RI.primary.cmds[0], kHash, "DirectSpatialReusePass.cs",
          &ci);
      m_directSpatialReuse.bindBindlessDescriptorSet(
          &RI.primary.cmds[0], &RI.globalset->m_bindlessSet, 0,
          VK_PIPELINE_BIND_POINT_COMPUTE);

      std::vector<RIProgram::DescriptorBinding> sb;
      sb.reserve(8);
      {
        RIProgram::DescriptorBinding b;
        b.handle = DescriptorBindingID::Create("gPerFrame");
        RI.UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
        sb.push_back(b);
      }
      sb.emplace_back("gPackedHitInfo",
                      RIDescriptor::storageImage(
                          &RI.device, state.packedHitInfoView[RI.swapchainIndex].Get()));
      sb.emplace_back("gRtAccel",
                      RIDescriptor::accelerationStructure(
                          &RI.device, apWorld->GetTlas())); // resolve shadow ray
      sb.emplace_back("gPackedHitInfoRaster",
                      RIDescriptor::sampledImage(
                          &RI.device, state.visibilityView[RI.swapchainIndex].Get(),
                          RI_RESOURCE_STATE_SHADER_RESOURCE));
      sb.emplace_back("gReservoirIn",
                      RIDescriptor::sampledImage(&RI.device,
                                                 state.reservoirTemporalView.Get(),
                                                 RI_RESOURCE_STATE_GENERAL));
      sb.emplace_back("gDirectKey",
                      RIDescriptor::sampledImage(&RI.device,
                                                 state.directKeyView[dlCur].Get(),
                                                 RI_RESOURCE_STATE_GENERAL));
      sb.emplace_back("gVelocity",
                      RIDescriptor::sampledImage(
                          &RI.device, state.velocityView[RI.swapchainIndex].Get(),
                          RI_RESOURCE_STATE_SHADER_RESOURCE));
      sb.emplace_back("gDirectHistory",
                      RIDescriptor::sampledImage(&RI.device,
                                                 state.directLightingView[dlPrev].Get(),
                                                 RI_RESOURCE_STATE_GENERAL));
      sb.emplace_back("gDirectKeyHistory",
                      RIDescriptor::sampledImage(&RI.device,
                                                 state.directKeyView[dlPrev].Get(),
                                                 RI_RESOURCE_STATE_GENERAL));
      sb.emplace_back("gReservoirOut",
                      RIDescriptor::storageImage(&RI.device,
                                                 state.reservoirView[dlCur].Get()));
      sb.emplace_back("gDirectLighting",
                      RIDescriptor::storageImage(
                          &RI.device, state.directLightingView[dlCur].Get()));

      appendWorldLightFog(sb, apWorld);
      m_directSpatialReuse.bindDescriptors(&RI.device, &RI.primary.cmds[0],
                                           RI.frameIndex, sb.data(), sb.size(),
                                           VK_PIPELINE_BIND_POINT_COMPUTE);
      RI.primary.cmds[0].dispatch(&RI.device, (renderWidth + 15u) / 16u,
                                  (renderHeight + 15u) / 16u, 1u);
    }

    // Resolved direct + final reservoir writes -> à-trous / composite reads
    // (stays GENERAL).
    RI.primary.cmds[0].vk_d3d12_memoryBarrier(
        {RI_RESOURCE_STATE_STORAGE_WRITE, RI_RESOURCE_STATE_SHADER_RESOURCE,
         RI_STAGE_COMPUTE, RI_STAGE_COMPUTE});

    // ----------------------------------------------------------------
    // DirectAtrousPass — SVGF-lite edge-aware à-trous spatial denoise. The
    // spatial half of the direct denoiser: share the 1-spp estimate across
    // same-surface neighbours, tap spacing doubling each iteration. Iteration 0
    // reads the accumulation (directLighting[dlCur]); later iterations
    // ping-pong the directAtrous scratch. All textures stay GENERAL.
    // ----------------------------------------------------------------
    directResultView = state.directLightingView[dlCur].Get();
    RIGpuScope _gsDirectAtrous(&RI.profiler, &RI.primary.cmds[0], "DirectAtrous");
    for (int it = 0; it < kAtrousIterations; ++it) {
      RITextureView *inView = (it == 0)
                               ? state.directLightingView[dlCur].Get()
                               : state.directAtrousView[(it - 1) & 1].Get();
      const uint32_t outIdx = static_cast<uint32_t>(it) & 1u;
      RITextureView *outView = state.directAtrousView[outIdx].Get();

      m_directAtrous.bindComputePipeline(&RI.device, &RI.primary.cmds[0], kHash,
                                         "DirectAtrousPass.cs", &ci);
      m_directAtrous.bindBindlessDescriptorSet(&RI.primary.cmds[0],
                                               &RI.globalset->m_bindlessSet, 0,
                                               VK_PIPELINE_BIND_POINT_COMPUTE);

      // Per-iteration tap spacing (1, 2, 4 …). Padded to 16 bytes (std140 UBO).
      struct AtrousParamsHost {
        uint32_t stepSize;
        uint32_t pad[3];
      } ap = {};
      ap.stepSize = 1u << it;

      std::vector<RIProgram::DescriptorBinding> ab;
      ab.reserve(4);
      {
        RIProgram::DescriptorBinding b;
        b.handle = DescriptorBindingID::Create("gAtrous");
        RI.UpdateFrameUBO(&b.descriptor, &ap, sizeof(ap));
        ab.push_back(b);
      }
      ab.emplace_back("gAtrousIn",
                      RIDescriptor::sampledImage(&RI.device, inView,
                                                 RI_RESOURCE_STATE_GENERAL));
      ab.emplace_back("gDirectKey",
                      RIDescriptor::sampledImage(&RI.device,
                                                 state.directKeyView[dlCur].Get(),
                                                 RI_RESOURCE_STATE_GENERAL));
      ab.emplace_back("gAtrousOut",
                      RIDescriptor::storageImage(&RI.device, outView));

      m_directAtrous.bindDescriptors(&RI.device, &RI.primary.cmds[0],
                                     RI.frameIndex, ab.data(), ab.size(),
                                     VK_PIPELINE_BIND_POINT_COMPUTE);
      RI.primary.cmds[0].dispatch(&RI.device, (renderWidth + 15u) / 16u,
                                  (renderHeight + 15u) / 16u, 1u);

      // This iteration's write -> next iteration's / composite's sampled read.
      RI.primary.cmds[0].vk_d3d12_memoryBarrier(
          {RI_RESOURCE_STATE_STORAGE_WRITE, RI_RESOURCE_STATE_SHADER_RESOURCE,
           RI_STAGE_COMPUTE, RI_STAGE_COMPUTE});
      directResultView = outView;
    }
  }

  // --------------------------------------------------------------------
  // MainCompositePass — compute pass. Reads gIndirectLighting
  // (state.surfelResultView, from SurfelGenerationPass above) + gPackedHitInfo
  // / gPackedHitInfoRaster / TLAS / gPerFrame, and writes the composited color
  // into the viewport render target. The forward passes draw on top of it; the
  // tail crop-blits it into the viewport backbuffer, which Scene.cpp's
  // post-effect chain + swapchain tail blit consume.
  // --------------------------------------------------------------------

  // Composite + forward passes render into the OVERSCAN render target (guard
  // band, single image — the main draw never ping-pongs); cropped 1:1 center
  // into the authored-size viewport backbuffer at the end of Draw.

  // Barrier: make the surfel cache + gIndirectLighting visible to the COMPUTE
  // SurfelGI composite, and put the render target into GENERAL for the storage
  // write. (The RT V-buffer pass (2956) and the raster visibility buffer (3016)
  // were already barriered to the COMPUTE stage upstream.)
  {
    // SHADER_RESOURCE for the gIndirectLighting image sample; STORAGE_READ so
    // the isWater branch's gatherSurfelIndirect can read the surfel-cache SSBOs
    // (gSurfelBuffer / gCellInfoBuffer / gCellToSurfelBuffer), written by the
    // surfel compute passes earlier this frame.
    RIMemoryBarrier mem = {RI_RESOURCE_STATE_STORAGE_WRITE,
                           RI_RESOURCE_STATE_SHADER_RESOURCE |
                               RI_RESOURCE_STATE_STORAGE_READ,
                           RI_STAGE_COMPUTE, RI_STAGE_COMPUTE};

    RITextureBarrier imageBarriers[2] = {
        // gIndirectLighting GENERAL -> SHADER_READ_ONLY, now consumed by
        // compute.
        {state.surfelResultTexture[RI.swapchainIndex].Get(),
         RI_RESOURCE_STATE_STORAGE_WRITE, RI_RESOURCE_STATE_SHADER_RESOURCE,
         RI_STAGE_COMPUTE, RI_STAGE_COMPUTE},
        // Pogo attach -> GENERAL for the compute storage write. Discard prior
        // contents (UNDEFINED): the dispatch writes every pixel, matching the
        // old fragment pass's LOAD_OP_DONT_CARE. This also covers the
        // first-frame init for the attach half.
        {state.renderTarget[RI.swapchainIndex].Get(), RI_RESOURCE_STATE_UNDEFINED,
         RI_RESOURCE_STATE_STORAGE_WRITE, RI_STAGE_NONE, RI_STAGE_COMPUTE}};

    RI.primary.cmds[0].vk_d3d12_resourceBarrier<1, 0, 2>(1, &mem, 0, NULL, 2,
                                                         imageBarriers);
  }

  // Depth flip shared by the decal pre-pass (below) and the particle /
  // translucent passes further down: depth arrives in SHADER_READ_ONLY from
  // surfel-generate and flips once to DEPTH_READ_ONLY for any depth-tested
  // pass.
  bool depthFlippedForReadOnly = false;
  auto flipDepthToReadOnly = [&]() {
    if (depthFlippedForReadOnly)
      return;
    RI.primary.cmds[0].vk_d3d12_textureBarrier(
        {state.depthTextures[RI.swapchainIndex].Get(),
         RI_RESOURCE_STATE_SHADER_RESOURCE, RI_RESOURCE_STATE_DEPTH_READ,
         RI_STAGE_FRAGMENT | RI_STAGE_COMPUTE, RI_STAGE_NONE,
         RI_BARRIER_ASPECT_DEPTH});
    depthFlippedForReadOnly = true;
  };

  // --------------------------------------------------------------------
  // Decal pre-pass — rasterize Type="Decal" meshes (dirt_floor / moist_wall /
  // pool_wine / trails, …) into two per-viewport accumulators BEFORE the
  // composite, which then applies albedo = albedo*decalMul + decalAdd ahead of
  // lighting + fog so the decals are lit + fogged like the surface they sit on
  // (the V-buffer renderer has no albedo G-buffer to write them into the way the
  // original deferred engine did). These decals carry no alpha — transparency is
  // the blend-mode identity (white for Mul/MulX2, black for Add) — so a single
  // premultiplied-over buffer turned their no-op areas opaque. Two accumulators
  // reproduce the real blend: Mul/MulX2 multiply into decalMul (cleared white),
  // Add adds into decalAdd (cleared black). Depth-tested ≤ against the gbuffer
  // depth (no write); both accumulators are CLEARED every frame so the composite
  // never samples stale contents. (Linear×linear multiply equals the legacy
  // gamma multiply via the power law; Add is a close linear approximation.)
  // --------------------------------------------------------------------
  {
    RIGpuScope _gsDecal(&RI.profiler, &RI.primary.cmds[0], "Decal");
    std::vector<iRenderable *> mulDecals; // Mul / MulX2 -> decalMul
    std::vector<iRenderable *> addDecals; // Add        -> decalAdd
    for (iRenderable *pObj :
         m_rendererList.GetRenderableItems(eRenderListType_Decal)) {
      if (!pObj)
        continue;
      cMaterial *pMat = pObj->GetMaterial();
      if (!pMat)
        continue;
      cVertexBuffer *pVB = pObj->GetVertexBuffer();
      if (!pVB || pVB->GetIndexNum() <= 0)
        continue;
      switch (pMat->GetBlendMode()) {
      case eMaterialBlendMode_Mul:
      case eMaterialBlendMode_MulX2:
        mulDecals.push_back(pObj);
        break;
      case eMaterialBlendMode_Add:
        addDecals.push_back(pObj);
        break;
      default:
        break; // Type="Decal" content is Mul/MulX2/Add only; skip others.
      }
    }

    auto decalBlend = [](eMaterialBlendMode m) -> DecalPipelineDesc::BlendMode {
      switch (m) {
      case eMaterialBlendMode_MulX2:
        return DecalPipelineDesc::BLEND_MULX2;
      case eMaterialBlendMode_Add:
        return DecalPipelineDesc::BLEND_ADD;
      default:
        return DecalPipelineDesc::BLEND_MUL;
      }
    };

    flipDepthToReadOnly();

    // One accumulator pass: clear `tex` to the blend identity, depth-test the
    // family's decals against the gbuffer depth (read-only), and blend each with
    // its material blend mode. Always run (even empty) so the texture is cleared
    // and ends in SHADER_RESOURCE for the composite.
    auto renderDecalAccumulator = [&](RITexture *tex, const RITextureView &view,
                                      const float clearRGBA[4],
                                      const std::vector<iRenderable *> &list) {
      RI.primary.cmds[0].vk_d3d12_textureBarrier(
          {tex, RI_RESOURCE_STATE_UNDEFINED, RI_RESOURCE_STATE_RENDER_TARGET,
           RI_STAGE_NONE, RI_STAGE_FRAGMENT});

      RIRenderingAttachment color = {};
      color.view = view;
      color.loadOp = RI_ATTACHMENT_LOAD_OP_CLEAR;
      color.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
      color.clearValue.color[0] = clearRGBA[0];
      color.clearValue.color[1] = clearRGBA[1];
      color.clearValue.color[2] = clearRGBA[2];
      color.clearValue.color[3] = clearRGBA[3];

      RIRenderingAttachment depth = {};
      depth.view = *state.depthView[RI.swapchainIndex];
      depth.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
      depth.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
      depth.readOnly = true;

      RIBeginRenderingDesc beginDesc = {};
      beginDesc.renderArea.width = (int16_t)renderWidth;
      beginDesc.renderArea.height = (int16_t)renderHeight;
      beginDesc.colorCount = 1;
      beginDesc.colors = &color;
      beginDesc.depthStencil = &depth;
      RI.primary.cmds[0].vk_d3d12_beginRendering(&RI.device, beginDesc);

      RIViewport vp = {};
      vp.x = 0.0f;
      vp.y = (float)renderHeight;
      vp.width = (float)renderWidth;
      vp.height = -(float)renderHeight;
      vp.depthMin = 0.0f;
      vp.depthMax = 1.0f;
      RIRect sc = {};
      sc.width = (int16_t)renderWidth;
      sc.height = (int16_t)renderHeight;
      RI.primary.cmds[0].setViewport(&RI.device, vp);
      RI.primary.cmds[0].setScissor(&RI.device, sc);

      if (!list.empty()) {
        m_decal.bindBindlessDescriptorSet(&RI.primary.cmds[0],
                                          &RI.globalset->m_bindlessSet, 0);
        {
          // VS reads gPerFrame (view/proj) + gSceneObjects; FS emits the linear
          // decal colour. No fog/light buffers — the composite lights and fogs.
          RIProgram::DescriptorBinding b;
          b.handle = DescriptorBindingID::Create("gPerFrame");
          RI.UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
          m_decal.bindDescriptors(&RI.device, &RI.primary.cmds[0], RI.frameIndex,
                                  &b, 1);
        }

        for (iRenderable *pObj : list) {
          cVertexBuffer *pVB = pObj->GetVertexBuffer();
          cMaterial *pMat = pObj->GetMaterial();
          const int indexCount = pVB->GetIndexNum();

          uint32_t materialId =
              RI.globalset->submitMaterial(cntx, pMat, (uint32_t)RI.frameIndex)
                  .materialId;
          if (materialId == UINT32_MAX) {
            Warning("Material Slot exhausted (decal)");
            continue;
          }

          ObjectSubmitDesc d; // decals fold into albedo; lit by the composite
          d.modelMatrix = pObj->GetModelMatrix(apFrustum);
          d.uvMatrix = pMat->GetUvMatrix();
          d.materialId = materialId;
          d.dissolveAmount = pObj->GetCoverageAmount();
          d.renderFlags = pObj->GetRenderFlags();

          const uint32_t slot = RI.globalset->submitObject(
              pObj->GetUniqueCookie(), (uint32_t)RI.frameIndex,
              static_cast<cVertexBuffer *>(pVB), d);
          if (slot == UINT32_MAX) {
            Warning("bindless pool exhausted (decal)");
            continue;
          }

          uint32_t vtxMask = 0;
          if (!detail::BindVertexStreams(&RI.primary.cmds[0], pVB, "decal",
                                         &vtxMask))
            continue;

          DecalPipelineDesc pipelineDesc(RIBootstrap::PogoColorFormat,
                                         RIBootstrap::DepthFormat,
                                         decalBlend(pMat->GetBlendMode()),
                                         vtxMask);
          m_decal.bindPipeline(&RI.device, &RI.primary.cmds[0], pipelineDesc.hash,
                               "Decal", &pipelineDesc.createInfo);

          RI.primary.cmds[0].drawIndexed(&RI.device, (uint32_t)indexCount, 1u, 0u,
                                         0, slot);
        }
      }

      RI.primary.cmds[0].vk_d3d12_endRendering(&RI.device);

      // COLOR_ATTACHMENT -> SHADER_RESOURCE for the composite read.
      RI.primary.cmds[0].vk_d3d12_textureBarrier(
          {tex, RI_RESOURCE_STATE_RENDER_TARGET, RI_RESOURCE_STATE_SHADER_RESOURCE,
           RI_STAGE_FRAGMENT, RI_STAGE_COMPUTE});
    };

    const float kIdentityMul[4] = {1.0f, 1.0f, 1.0f, 1.0f}; // ×1
    const float kIdentityAdd[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // +0
    renderDecalAccumulator(state.decalMulTexture[RI.swapchainIndex].Get(),
                           *state.decalMulView[RI.swapchainIndex], kIdentityMul,
                           mulDecals);
    renderDecalAccumulator(state.decalAddTexture[RI.swapchainIndex].Get(),
                           *state.decalAddView[RI.swapchainIndex], kIdentityAdd,
                           addDecals);
  }

  // Surfel-GI compute pass — one thread per pixel writes the composite into the
  // pogo attach bound as gOutput (storage image, GENERAL).
  {
    VkComputePipelineCreateInfo surfelCreate = {
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    RIGpuScope _gsMainComposite(&RI.profiler, &RI.primary.cmds[0], "MainComposite");
    m_mainComposite.bindComputePipeline(&RI.device, &RI.primary.cmds[0], kHash,
                                        "MainCompositePass.cs", &surfelCreate);
    m_mainComposite.bindBindlessDescriptorSet(&RI.primary.cmds[0],
                                              &RI.globalset->m_bindlessSet, 0,
                                              VK_PIPELINE_BIND_POINT_COMPUTE);

    std::vector<RIProgram::DescriptorBinding> bnd;
    bnd.reserve(10);
    {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("gPerFrame");
      RI.UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
      bnd.push_back(b);
    }
    bnd.emplace_back("gPackedHitInfo",
                     RIDescriptor::storageImage(
                         &RI.device, state.packedHitInfoView[RI.swapchainIndex].Get()));
    bnd.emplace_back("gRtAccel",
                     RIDescriptor::accelerationStructure(&RI.device, apWorld->GetTlas()));
    {
      bnd.emplace_back("gIndirectLighting",
                       RIDescriptor::sampledImage(
                           &RI.device,
                           state.surfelResultView[RI.swapchainIndex].Get()));
    }
    {
      // Rasterized V-buffer fallback — SurfelGBuffer writes
      // RI.visibilityTexture earlier this frame and the toRead[] barriers
      // upstream already transitioned it to SHADER_READ_ONLY_OPTIMAL (visible
      // to COMPUTE).
      bnd.emplace_back("gPackedHitInfoRaster",
                       RIDescriptor::sampledImage(
                           &RI.device,
                           state.visibilityView[RI.swapchainIndex].Get()));
    }
    // gDirectLighting — this frame's direct irradiance the composite multiplies
    // albedo into: the SVGF-lite à-trous output (or the raw accumulation if the
    // filter is disabled), sampled, GENERAL.
    bnd.emplace_back("gDirectLighting",
                     RIDescriptor::sampledImage(&RI.device, directResultView,
                                                RI_RESOURCE_STATE_GENERAL));
    // gOutput — the viewport render target bound as a storage image (GENERAL).
    bnd.emplace_back("gOutput",
                     RIDescriptor::storageImage(
                         &RI.device, state.renderTargetView[RI.swapchainIndex].Get()));

    // Decal accumulators from the pre-pass above (already in SHADER_RESOURCE):
    // the composite applies albedo = albedo*gDecalMul + gDecalAdd before lighting.
    bnd.emplace_back("gDecalMul",
                     RIDescriptor::sampledImage(
                         &RI.device, state.decalMulView[RI.swapchainIndex].Get()));
    bnd.emplace_back("gDecalAdd",
                     RIDescriptor::sampledImage(
                         &RI.device, state.decalAddView[RI.swapchainIndex].Get()));

    // Per-world static decal buffers (set kWorldDecalSet), baked once by
    // cWorld::Compile. RIProgram reflects them as set 2 and binds a rotated set.
    // Worlds compiled under the hybrid renderer always have valid (>=1 element)
    // buffers, so set 2 is never left unbound.
    if (RIBuffer *decalBuf = apWorld->GetDecalBuffer()) {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("gDecals");
      b.descriptor = RIDescriptor::storageBuffer(
          &RI.device, decalBuf, 0,
          std::max<size_t>(apWorld->GetDecalCount(), 1) * sizeof(GpuDecal));
      bnd.push_back(b);
    }
    if (RIBuffer *idxBuf = apWorld->GetDecalObjectIndexBuffer()) {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("gObjectDecalIndices");
      b.descriptor = RIDescriptor::storageBuffer(
          &RI.device, idxBuf, 0,
          std::max<size_t>(apWorld->GetDecalObjectIndices().size(), 1) *
              sizeof(uint32_t));
      bnd.push_back(b);
    }

    appendWorldLightFog(bnd, apWorld);
    m_mainComposite.bindDescriptors(&RI.device, &RI.primary.cmds[0],
                                    RI.frameIndex, bnd.data(), bnd.size(),
                                    VK_PIPELINE_BIND_POINT_COMPUTE);

    static_assert(sizeof(OverlayPushConstants) == 4);
    const OverlayPushConstants push{ m_overlayMode };
    vkCmdPushConstants(RI.primary.cmds[0].vk.cmd, m_mainComposite.getPipelineLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);

    RI.primary.cmds[0].dispatch(&RI.device, (renderWidth + 15u) / 16u,
                                (renderHeight + 15u) / 16u, 1u);
  }

  // Toggle the direct-lighting ping-pong: this frame's write becomes next
  // frame's history.
  state.directLightingIndex ^= 1u;

  // Render target: GENERAL (compute write) -> COLOR_ATTACHMENT_OPTIMAL so the
  // downstream raster passes find the layout they expect.
  {
    RI.primary.cmds[0].vk_d3d12_textureBarrier(
        {state.renderTarget[RI.swapchainIndex].Get(),
         RI_RESOURCE_STATE_STORAGE_WRITE, RI_RESOURCE_STATE_RENDER_TARGET,
         RI_STAGE_COMPUTE});
  }

  // Single render target — no toggle: the main draw never ping-pongs. The
  // downstream raster passes flip it COLOR -> SHADER_READ as they go (each
  // pass transitions in before drawing and back out after); the tail blits
  // it into the viewport backbuffer, where the post-effect chain + tail blit
  // in cScene::Render consume it.
  {
    RI.primary.cmds[0].vk_d3d12_textureBarrier(RI_PogoShaderBarrier(
        state.renderTarget[RI.swapchainIndex].Get(), /*initial=*/false));
  }

  // (depthFlippedForReadOnly + flipDepthToReadOnly are defined above, before
  // the decal pre-pass, and shared with the particle / translucent passes
  // below.)

  // (The Type="Decal" mesh decals are rasterized in the decal pre-pass above,
  // before the composite, into the decal-overlay target — not here. The
  // composite folds that overlay onto the base albedo so they are lit + fogged.)

  // --------------------------------------------------------------------
  // Water pass — raster the water surface over the refracted background that
  // the GI composite already shaded (SurfelVBuffer.rt's water refraction
  // clobbered the primary hit). Two draws per mesh: MUL (tint + refraction
  // exposure) then ADD (inline-RT lit reflection × Fresnel). Reuses the
  // translucent 5-stream layout + TranslucentMeshPipelineDesc state (depth ≤,
  // no write); the m_water program supplies the shaders (Water.vert/frag).
  // Pogo-read-half barriers as the other translucent sub-passes.
  // --------------------------------------------------------------------
  {
    RIGpuScope _gsWater(&RI.profiler, &RI.primary.cmds[0], "Water");
    std::vector<iRenderable *> waters;
    for (iRenderable *pObj :
         m_rendererList.GetRenderableItems(eRenderListType_Translucent)) {
      if (!pObj)
        continue;
      cMaterial *pMat = pObj->GetMaterial();
      if (!pMat || pMat->GetMaterialID() != MaterialID::Water)
        continue;
      cVertexBuffer *pVB = pObj->GetVertexBuffer();
      if (!pVB || pVB->GetIndexNum() <= 0)
        continue;
      waters.push_back(pObj);
    }

    if (!waters.empty()) {
      flipDepthToReadOnly();

      VkImage pogoReadImage = state.renderTarget[RI.swapchainIndex]->vk.image;
      VkImageView pogoReadView =
          state.renderTargetView[RI.swapchainIndex]->vk.image;

      RI.primary.cmds[0].vk_d3d12_textureBarrier(RI_PogoAttachmentBarrier(
          state.renderTarget[RI.swapchainIndex].Get(), /*initial=*/false));

      RITextureView colorView = {};
      colorView.vk.image = pogoReadView;
      RIRenderingAttachment color = {};
      color.view = colorView;
      color.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
      color.storeOp = RI_ATTACHMENT_STORE_OP_STORE;

      RIRenderingAttachment depth = {};
      depth.view = *state.depthView[RI.swapchainIndex];
      depth.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
      depth.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
      depth.readOnly = true;

      RIBeginRenderingDesc beginDesc = {};
      beginDesc.renderArea.width = (int16_t)renderWidth;
      beginDesc.renderArea.height = (int16_t)renderHeight;
      beginDesc.colorCount = 1;
      beginDesc.colors = &color;
      beginDesc.depthStencil = &depth;
      RI.primary.cmds[0].vk_d3d12_beginRendering(&RI.device, beginDesc);

      RIViewport vp = {};
      vp.x = 0.0f;
      vp.y = (float)renderHeight;
      vp.width = (float)renderWidth;
      vp.height = -(float)renderHeight;
      vp.depthMin = 0.0f;
      vp.depthMax = 1.0f;
      RIRect sc = {};
      sc.width = (int16_t)renderWidth;
      sc.height = (int16_t)renderHeight;
      RI.primary.cmds[0].setViewport(&RI.device, vp);
      RI.primary.cmds[0].setScissor(&RI.device, sc);

      m_water.bindBindlessDescriptorSet(&RI.primary.cmds[0],
                                        &RI.globalset->m_bindlessSet, 0);
      {
        // set 1: gPerFrame + gRtAccel (binding 36). The water frag does inline
        // RayQuery reflection (traceReflectionHit), so the TLAS must be bound —
        // the raster pipeline doesn't get it for free like the compute/RT
        // passes.
        std::vector<RIProgram::DescriptorBinding> wbnd;
        {
          RIProgram::DescriptorBinding b;
          b.handle = DescriptorBindingID::Create("gPerFrame");
          RI.UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
          wbnd.push_back(b);
        }
        wbnd.emplace_back("gRtAccel",
                          RIDescriptor::accelerationStructure(&RI.device, apWorld->GetTlas()));
        appendWorldLightFog(wbnd, apWorld);
        // The water frag's lit reflection shades the RT hit via gatherSurfelIndirect
        // (shadeReflectionHit), which samples the set-1 surfel depth atlas. The
        // surfel storage buffers ride set 0 (m_bindlessSet, bound above), but
        // gSurfelDepth is set 1 and must be bound here or the pipeline reports it
        // as statically-used-but-never-updated (VUID-vkCmdDrawIndexed-None-08114).
        wbnd.emplace_back("gSurfelDepth",
                          RIDescriptor::sampledImage(
                              &RI.device, &m_surfelDepthView[RI.swapchainIndex],
                              RI_RESOURCE_STATE_GENERAL));
        m_water.bindDescriptors(&RI.device, &RI.primary.cmds[0], RI.frameIndex,
                                wbnd.data(), (uint32_t)wbnd.size());
      }

      struct WaterPush {
        uint32_t pass;
        uint32_t p0, p1, p2;
      };

      for (iRenderable *pObj : waters) {
        cVertexBuffer *pVB = pObj->GetVertexBuffer();
        cMaterial *pMat = pObj->GetMaterial();
        const int indexCount = pVB->GetIndexNum();

        auto mat = RI.globalset->submitMaterial(cntx, pMat, (uint32_t)RI.frameIndex);
        if (mat.materialId == UINT32_MAX) {
          Warning("Material Slot exhausted (water)");
          continue;
        }

        ObjectSubmitDesc d;
        d.modelMatrix = pObj->GetModelMatrix(apFrustum);
        d.uvMatrix = pMat->GetUvMatrix();
        d.materialId =
            mat.materialId; // water ids fall in the water range of materialID
        d.dissolveAmount = pObj->GetCoverageAmount();
        d.renderFlags = pObj->GetRenderFlags();

        const uint32_t slot = RI.globalset->submitObject(
            pObj->GetUniqueCookie(), (uint32_t)RI.frameIndex,
            static_cast<cVertexBuffer *>(pVB), d);
        if (slot == UINT32_MAX) {
          Warning("bindless pool exhausted (water)");
          continue;
        }

        uint32_t vtxMask = 0;
        if (!detail::BindVertexStreams(&RI.primary.cmds[0], pVB, "water",
                                       &vtxMask))
          continue;

        // Two draws into the pogo: tint (MUL) then lit reflection (ADD). Salt
        // the pipeline hash so it doesn't collide with the translucent
        // program's cache (same TranslucentMeshPipelineDesc state, different
        // program/shaders).
        const TranslucentMeshPipelineDesc::BlendMode modes[2] = {
            TranslucentMeshPipelineDesc::BLEND_MUL,
            TranslucentMeshPipelineDesc::BLEND_ADD};
        for (uint32_t pass = 0; pass < 2u; ++pass) {
          TranslucentMeshPipelineDesc pd(RIBootstrap::PogoColorFormat,
                                         RIBootstrap::DepthFormat, modes[pass],
                                         vtxMask);
          const hash_t waterHash = hash_u32(pd.hash, 0x57415445u /*'WATE'*/);
          m_water.bindPipeline(&RI.device, &RI.primary.cmds[0], waterHash,
                               "Water", &pd.createInfo);
          WaterPush push = {pass, 0u, 0u, 0u};
          RI.primary.cmds[0].vk_d3d12_setPushConstants(&RI.device, m_water, 0,
                                                       sizeof(push), &push);
          RI.primary.cmds[0].drawIndexed(&RI.device, (uint32_t)indexCount, 1u,
                                         0u, 0, slot);
        }
      }

      RI.primary.cmds[0].vk_d3d12_endRendering(&RI.device);

      {
        RI.primary.cmds[0].vk_d3d12_textureBarrier(RI_PogoShaderBarrier(
            state.renderTarget[RI.swapchainIndex].Get(), /*initial=*/false));
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
  //   - RI.globalset->m_objectSlots / m_objectBuffer (per-renderable OBJECT slot)
  //   - m_opaque*Handles (BDA fan-out: particles/meshes overload
  //                       position/uv0/color/index with their own VB addresses)
  //   - RI.globalset->m_materialBindless / m_materialBuffer (material slot)
  //
  // Sync: (a) swapchain stays COLOR_ATTACHMENT_OPTIMAL from the composite
  //           (load to preserve it); (b) flipDepthToReadOnly() moves depth back
  //           to DEPTH_READ_ONLY_OPTIMAL (shared with the decal pass).
  // --------------------------------------------------------------------
  {
    // Collect particle emitters from the translucent list once so we can
    // skip the whole pass (and its barriers/begin-rendering) when empty.
    RIGpuScope _gsParticle(&RI.profiler, &RI.primary.cmds[0], "Particle");
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
      // anything that samples it, e.g. the menu/inventory screen capture)
      // carries particles too. Flip that half COLOR_ATTACHMENT for the draw,
      // then back to SHADER_READ after, so the tail blit below can sample it.
      VkImage pogoReadImage = state.renderTarget[RI.swapchainIndex]->vk.image;
      VkImageView pogoReadView =
          state.renderTargetView[RI.swapchainIndex]->vk.image;
      const RI_Format_e particleTargetFormat = RIBootstrap::PogoColorFormat;
      {
        RI.primary.cmds[0].vk_d3d12_textureBarrier(RI_PogoAttachmentBarrier(
            state.renderTarget[RI.swapchainIndex].Get(), /*initial=*/false));
      }

      RITextureView colorView = {};
      colorView.vk.image = pogoReadView;
      RIRenderingAttachment color = {};
      color.view = colorView;
      color.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
      color.storeOp = RI_ATTACHMENT_STORE_OP_STORE;

      RIRenderingAttachment depth = {};
      depth.view = *state.depthView[RI.swapchainIndex];
      depth.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
      depth.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
      depth.readOnly = true;

      RIBeginRenderingDesc beginDesc = {};
      beginDesc.renderArea.width = (int16_t)renderWidth;
      beginDesc.renderArea.height = (int16_t)renderHeight;
      beginDesc.colorCount = 1;
      beginDesc.colors = &color;
      beginDesc.depthStencil = &depth;
      RI.primary.cmds[0].vk_d3d12_beginRendering(&RI.device, beginDesc);

      RIViewport vp = {};
      vp.x = 0.0f;
      vp.y = (float)renderHeight;
      vp.width = (float)renderWidth;
      vp.height = -(float)renderHeight;
      vp.depthMin = 0.0f;
      vp.depthMax = 1.0f;
      RIRect sc = {};
      sc.width = (int16_t)renderWidth;
      sc.height = (int16_t)renderHeight;
      RI.primary.cmds[0].setViewport(&RI.device, vp);
      RI.primary.cmds[0].setScissor(&RI.device, sc);

      m_particle.bindBindlessDescriptorSet(&RI.primary.cmds[0],
                                           &RI.globalset->m_bindlessSet, 0);

      std::vector<RIProgram::DescriptorBinding> particleBindings;
      particleBindings.reserve(2);
      {
        RIProgram::DescriptorBinding b;
        b.handle = DescriptorBindingID::Create("gPerFrame");
        RI.UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
        particleBindings.push_back(b);
      }
      // Soft particles: scene depth (opaque geometry) for the per-pixel fade.
      // Runs after flipDepthToReadOnly() above, so the image is already in
      // DEPTH_READ_ONLY_OPTIMAL — the same layout this sampled descriptor
      // declares, and the depth attachment is read-only, so the feedback loop
      // is legal with no extra barrier.
      particleBindings.emplace_back(
          "gSceneDepth",
          RIDescriptor::sampledImage(&RI.device,
                                     state.depthSampleView[RI.swapchainIndex].Get(),
                                     RI_RESOURCE_STATE_DEPTH_READ));
      appendWorldLightFog(particleBindings, apWorld);
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
        cMaterial *pMat = pEmitter->GetMaterial();
        if (!pMat)
          continue;
        // Per-frame scratch geometry (no persistent emitter VB): build this
        // frame's camera-facing quads into the shared RI.translucentVtx/Idx
        // segments — same single producer as the wireframe/simple panes.
        auto geom = pEmitter->BuildScratchGeometry(apFrustum, afFrameTime,
                                                   /*withUv=*/true);
        if (!geom.valid)
          continue;
        const int indexCount = (int)geom.indexCount;

        uint32_t materialId =
            RI.globalset->submitMaterial(cntx, pMat, (uint32_t)RI.frameIndex)
                .materialId;
        if (materialId == UINT32_MAX) {
          Warning("Material Slot exhausted (particle)");
          continue;
        }

        ObjectSubmitDesc d; // particle: identity uv, no dissolve/illum
        d.modelMatrix = pEmitter->GetModelMatrix(apFrustum);
        d.materialId = materialId;

        // The particle VS pulls pos/uv0/color/index via BDA from the slot's
        // UniformObject handles. Point them at the per-viewport translucent
        // scratch segments (base address + byte offset); normal/tangent are never
        // read for a particle slot, so 0. submitObject folds these into the
        // payload — refreshed every frame since the scratch offsets change.
        {
          const uint64_t vtxBase =
              RI.translucentVtxBuffer->GetDeviceHandle(&RI.device);
          d.streamHandles.pos   = vtxBase + geom.posByteOffset;
          d.streamHandles.color = vtxBase + geom.colByteOffset;
          d.streamHandles.uv0   = vtxBase + geom.uvByteOffset;
          d.streamHandles.index =
              RI.translucentIdxBuffer->GetDeviceHandle(&RI.device) +
              geom.idxByteOffset;
          d.streamHandles.set = true;
        }

        // Particles share the object-slot pool with opaque solids; the payload
        // submit also bumps the slot generation when the slot is (re)assigned —
        // so a surfel still anchored to the slot's previous opaque occupant
        // self-invalidates before it dereferences this slot's smaller streams.
        const uint32_t slot = RI.globalset->submitObject(pEmitter->GetUniqueCookie(),
                                                    (uint32_t)RI.frameIndex,
                                                    nullptr, d, kSubmitData);
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
        RI.primary.cmds[0].vk_d3d12_setPushConstants(&RI.device, m_particle, 0,
                                                     sizeof(push), &push);

        RI.primary.cmds[0].draw(&RI.device, (uint32_t)indexCount, 1u, 0u, slot);
      }

      RI.primary.cmds[0].vk_d3d12_endRendering(&RI.device);

      // pogo "read" half back to SHADER_READ_ONLY so the tail blit can sample
      // it.
      {
        RI.primary.cmds[0].vk_d3d12_textureBarrier(RI_PogoShaderBarrier(
            state.renderTarget[RI.swapchainIndex].Get(), /*initial=*/false));
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
    RIGpuScope _gsTranslucent(&RI.profiler, &RI.primary.cmds[0], "TranslucentMesh");
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
      if (pMat->GetMaterialID() == MaterialID::Water)
        continue;
      cVertexBuffer *pVB = pObj->GetVertexBuffer();
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

      VkImage pogoReadImage = state.renderTarget[RI.swapchainIndex]->vk.image;
      VkImageView pogoReadView =
          state.renderTargetView[RI.swapchainIndex]->vk.image;
      const RI_Format_e meshTargetFormat = RIBootstrap::PogoColorFormat;

      // SHADER_READ_ONLY → COLOR_ATTACHMENT_OPTIMAL. If the particle pass
      // ran above, that block left the pogo half in SHADER_READ_ONLY (for
      // a tail blit that never got to run); if it didn't, the visibility
      // composite + post-effect chain also left it in SHADER_READ_ONLY. The
      // barrier helper handles either source state.
      {
        RI.primary.cmds[0].vk_d3d12_textureBarrier(RI_PogoAttachmentBarrier(
            state.renderTarget[RI.swapchainIndex].Get(), /*initial=*/false));
      }

      RITextureView colorView = {};
      colorView.vk.image = pogoReadView;
      RIRenderingAttachment color = {};
      color.view = colorView;
      color.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
      color.storeOp = RI_ATTACHMENT_STORE_OP_STORE;

      RIRenderingAttachment depth = {};
      depth.view = *state.depthView[RI.swapchainIndex];
      depth.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
      depth.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
      depth.readOnly = true;

      RIBeginRenderingDesc beginDesc = {};
      beginDesc.renderArea.width = (int16_t)renderWidth;
      beginDesc.renderArea.height = (int16_t)renderHeight;
      beginDesc.colorCount = 1;
      beginDesc.colors = &color;
      beginDesc.depthStencil = &depth;
      RI.primary.cmds[0].vk_d3d12_beginRendering(&RI.device, beginDesc);

      RIViewport vp = {};
      vp.x = 0.0f;
      vp.y = (float)renderHeight;
      vp.width = (float)renderWidth;
      vp.height = -(float)renderHeight;
      vp.depthMin = 0.0f;
      vp.depthMax = 1.0f;
      RIRect sc = {};
      sc.width = (int16_t)renderWidth;
      sc.height = (int16_t)renderHeight;
      RI.primary.cmds[0].setViewport(&RI.device, vp);
      RI.primary.cmds[0].setScissor(&RI.device, sc);

      m_translucentMesh.bindBindlessDescriptorSet(&RI.primary.cmds[0],
                                                  &RI.globalset->m_bindlessSet, 0);

      std::vector<RIProgram::DescriptorBinding> meshBindings;
      meshBindings.reserve(1);
      {
        RIProgram::DescriptorBinding b;
        b.handle = DescriptorBindingID::Create("gPerFrame");
        RI.UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
        meshBindings.push_back(b);
      }
      appendWorldLightFog(meshBindings, apWorld);
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
        float sceneAlpha;
        uint32_t options;
        uint32_t _pad;
      };
      constexpr uint32_t kTransOptUseIllumination = 1u << 0;

      for (iRenderable *pObj : meshes) {
        cVertexBuffer *pVB = pObj->GetVertexBuffer();
        cMaterial *pMat = pObj->GetMaterial();
        // Filter above already rejected null pVB / pMat and 0-index VBs.
        const int indexCount = pVB->GetIndexNum();

        uint32_t materialId =
            RI.globalset->submitMaterial(cntx, pMat, (uint32_t)RI.frameIndex)
                .materialId;
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
        d.renderFlags = pObj->GetRenderFlags();

        // Stable slot keyed on the renderable's unique cookie. submitObject
        // bumps the slot generation on (re)assignment so a surfel anchored to a
        // previous opaque occupant self-invalidates before dereferencing the
        // wrong VB/IB.
        const uint32_t slot = RI.globalset->submitObject(
            pObj->GetUniqueCookie(), (uint32_t)RI.frameIndex,
            static_cast<cVertexBuffer *>(pVB), d);
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
        TranslucentMeshPipelineDesc pipelineDesc(
            meshTargetFormat, RIBootstrap::DepthFormat, mode, vtxMask);
        m_translucentMesh.bindPipeline(&RI.device, &RI.primary.cmds[0],
                                       pipelineDesc.hash, "TranslucentMesh",
                                       &pipelineDesc.createInfo);

        // Fog (world + per-area) is applied per-pixel in Translucent.frag.slang
        // by walking gFogAreas. sceneAlpha stays 1.0 for the no-extra-alpha
        // common path.
        const float sceneAlpha = 1.0f;
        PushBlock push = {(uint32_t)mode, sceneAlpha, 0u, 0u};
        RI.primary.cmds[0].vk_d3d12_setPushConstants(&RI.device, m_translucentMesh,
                                                     0, sizeof(push), &push);

        RI.primary.cmds[0].drawIndexed(&RI.device, (uint32_t)indexCount, 1u,
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
          RI.primary.cmds[0].vk_d3d12_setPushConstants(
              &RI.device, m_translucentMesh, 0, sizeof(pushIllum), &pushIllum);
          // Vertex / index buffers stay bound from the main draw above —
          // same renderable, just a second pipeline + push-constant set.
          RI.primary.cmds[0].drawIndexed(&RI.device, (uint32_t)indexCount, 1u,
                                         0u, 0, slot);
        }
      }

      RI.primary.cmds[0].vk_d3d12_endRendering(&RI.device);
      RI.primary.cmds[0].vk_d3d12_textureBarrier(RI_PogoShaderBarrier(state.renderTarget[RI.swapchainIndex].Get(), /*initial=*/false));
    }
  }

  // Restore depth to DEPTH_ATTACHMENT_OPTIMAL before yielding the command
  // buffer: RI_VK_FillDepthAttachment hardcodes that layout, and depth ends
  // here in either SHADER_READ_ONLY_OPTIMAL (surfel-only) or
  // DEPTH_READ_ONLY_OPTIMAL (flipDepthToReadOnly ran for particle/decal).
  {
    const uint32_t beforeState = depthFlippedForReadOnly
                                     ? RI_RESOURCE_STATE_DEPTH_READ
                                     : RI_RESOURCE_STATE_SHADER_RESOURCE;
    const uint32_t beforeStages = depthFlippedForReadOnly
                                      ? RI_STAGE_NONE
                                      : (RI_STAGE_FRAGMENT | RI_STAGE_COMPUTE);

    RI.primary.cmds[0].vk_d3d12_textureBarrier(
        {state.depthTextures[RI.swapchainIndex].Get(), beforeState,
         RI_RESOURCE_STATE_DEPTH_WRITE, beforeStages, RI_STAGE_NONE,
         RI_BARRIER_ASPECT_DEPTH});
  }

  // DebugDraw overlay (editor panes: grid / gizmos / icons, enqueued by the
  // viewport's OnPreWorldDraw callbacks): draw into the finished scene in
  // the render target against the scene depth, so cScene's pogo feed carries
  // the overlay along. The render target sits in SHADER_READ_ONLY (left by
  // the post-composite flip / last forward pass); flip it to COLOR for the
  // overlay pass and back to SHADER_READ after — Draw's contract is to leave
  // the finished frame in the render target, SHADER_READ (the BackBuffer).
  DebugDraw *debugDraw = mpGraphics->GetDebugDraw();
  const bool debugOverlayDrawn = debugDraw && debugDraw->HasRequests();
  if (debugOverlayDrawn) {
    RI.primary.cmds[0].vk_d3d12_textureBarrier(
        {state.renderTarget[RI.swapchainIndex].Get(),
         RI_RESOURCE_STATE_SHADER_RESOURCE,
         RI_RESOURCE_STATE_RENDER_TARGET_READ, RI_STAGE_FRAGMENT});

    {
      RITextureView colorView = *state.renderTargetView[RI.swapchainIndex];
      RIRenderingAttachment color = {};
      color.view = colorView;
      color.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
      color.storeOp = RI_ATTACHMENT_STORE_OP_STORE;

      // Scene depth, restored to DEPTH_ATTACHMENT_OPTIMAL just above; the
      // overlay pipelines test against it but never write. LOAD, no clear.
      RIRenderingAttachment depth = {};
      depth.view = *state.depthView[RI.swapchainIndex];
      depth.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
      depth.storeOp = RI_ATTACHMENT_STORE_OP_STORE;

      RIBeginRenderingDesc beginDesc = {};
      beginDesc.renderArea.width = (int16_t)renderWidth;
      beginDesc.renderArea.height = (int16_t)renderHeight;
      beginDesc.colorCount = 1;
      beginDesc.colors = &color;
      beginDesc.depthStencil = &depth;
      RI.primary.cmds[0].vk_d3d12_beginRendering(&RI.device, beginDesc);

      debugDraw->flush(cntx, &RI.primary.cmds[0], apFrustum, renderWidth,
                       renderHeight, RIBootstrap::PogoColorFormat);

      RI.primary.cmds[0].vk_d3d12_endRendering(&RI.device);
    }
    RI.primary.cmds[0].vk_d3d12_textureBarrier(RI_PogoShaderBarrier(
        state.renderTarget[RI.swapchainIndex].Get(), /*initial=*/false));
  }

  // Commit every bindless handle / slot-generation write made this frame. The
  // uploader records into its own transfer cmd buffer, flushed as a fenced
  // pre-pass the primary submit waits on (RIBootstrap), so this single tail
  // call lands all copies ahead of every primary read regardless of recording
  // order.
  RI.globalset->flushMirrors(&RI.device);
}

cHybridRenderer::~cHybridRenderer() {
  // The ray-tracing TLAS + its storage/instance buffers now live on cWorld and
  // are disposed with the world.
  // The global managed set is engine-lifetime; ShutdownGlobalManagedSets()
  // (cGraphics teardown) destroys it after the device is idle.
  // Fallback vertex streams are RIBootstrap globals (process-lifetime, like
  // RI.nulVertexBuffer); not freed here.
  for (uint32_t i = 0; i < RI_MAX_SWAPCHAIN_IMAGES; ++i) {
    m_surfelDepthView[i].dispose(&RI.device);
    m_surfelDepthTexture[i].dispose(&RI.device);
    m_surfelDepthTexture[i] = {};
  }
  m_surfelRTIndirectBuf.dispose(&RI.device);
  m_surfelRTIndirectBuf = {};
}

} // namespace hpl
