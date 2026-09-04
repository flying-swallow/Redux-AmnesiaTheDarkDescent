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
#include "graphics/Graphics.h"
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
#include "scene/LightArea.h"
#include "scene/LightSpot.h"
#include "scene/ParticleEmitter.h"
#include "scene/RenderableSet.h"
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
  cGraphics* pGraphics = Interface<cGraphics>::Get();
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
      nrm ? nrm : &pGraphics->fallbackNormalVertex,
      tan ? tan : &pGraphics->fallbackTangentVertex,
      col ? col : &pGraphics->fallbackColorVertex,
      uv ? uv : &pGraphics->fallbackUv0Vertex,
  };
  cmd->bindVertexBuffers<5>(0, 5, vertBufs);
  cmd->bindIndexBuffer(&pGraphics->device, idx, 0, RI_INDEX_TYPE_32);
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
        mpGraphics->globalset->m_bindlessSet.vk.m_bindlessSetLayout};
    {
      // Gbuffer pass: one .spv, two entry points (vsMain / psMain).
      auto gbuffer_bin = RIProgram::loadShaderStage(
          apResources->GetFileSearcher(), "VBufferRaster.3d.spv");
      std::array<RIProgram::ModuleStage, 2> stages = {
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, gbuffer_bin,
                                 "vsMain"},
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, gbuffer_bin,
                                 "psMain"}};
      m_gbuffer.initialize(&mpGraphics->device, stages, externalLayouts, "Hybrid.gbuffer");
    }

    auto loadComputeProgram = [&](RIProgram &prog, const char *name) {
      auto bin =
          RIProgram::loadShaderStage(apResources->GetFileSearcher(), name);
      std::array<RIProgram::ModuleStage, 1> stages = {
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_COMPUTE, bin}};
      prog.initialize(&mpGraphics->device, stages, externalLayouts, name);
    };
    // Compute load that passes the Slang entry-point name through to
    // ModuleStage.
    auto loadSlangCompute = [&](RIProgram &prog, const char *name,
                                const char *entryPoint) {
      auto bin =
          RIProgram::loadShaderStage(apResources->GetFileSearcher(), name);
      std::array<RIProgram::ModuleStage, 1> stages = {RIProgram::ModuleStage{
          RIProgram::PROGRAM_STAGE_COMPUTE, bin, entryPoint}};
      prog.initialize(&mpGraphics->device, stages, externalLayouts, name);
    };
    // VBufferPomBary — compute pass that copies the raster V-buffer into
    // packedHitInfoTexture and applies parallax-occlusion barycentric
    // correction for height-mapped diffuse surfaces.
    loadSlangCompute(m_vBufferPomBary, "VBufferPomBary.cs.spv", "csMain");
    // PathTracePass — per-pixel reference path tracer. One .spv, four entry
    // points (rayGen / ptMiss / ptCloseHit / ptAnyHit). Shadow rays use inline
    // RayQuery, so no second hit group is needed (SBT stays single-ray-type).
    {
      auto pt_bin = RIProgram::loadShaderStage(apResources->GetFileSearcher(),
                                               "PathTracePass.rt.spv");
      std::array<RIProgram::ModuleStage, 4> stages = {
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_RAYGEN, pt_bin,
                                 "rayGen"},
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_MISS, pt_bin,
                                 "ptMiss"},
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_CLOSEST_HIT, pt_bin,
                                 "ptCloseHit"},
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_ANY_HIT, pt_bin,
                                 "ptAnyHit"}};
      m_pathTrace.initialize(&mpGraphics->device, stages, externalLayouts, "Hybrid.pathTrace");
    }
    // LightGridBuildPass — single compute entry (binLights) that bins
    // point/spot lights into the coarse world-space light grid each frame.
    loadSlangCompute(m_lightGrid, "LightGridBuildPass.cs.spv", "binLights");
    // Composite — compute pass: one thread per pixel writes the composite
    // (albedo + inline decals + lighting) into the pogo attach bound as
    // gOutput. The renderer transitions the attach to GENERAL around the
    // dispatch and back to COLOR_ATTACHMENT_OPTIMAL afterwards.
    loadSlangCompute(m_composite, "MainCompositePass.cs.spv", "csMain");
    loadSlangCompute(m_directLighting, "DirectLightingPass.cs.spv", "csMain");
    loadSlangCompute(m_directSpatialReuse, "DirectSpatialReusePass.cs.spv",
                     "csMain");
    loadSlangCompute(m_nrdPack, "NrdPack.cs.spv", "csMain");
    {
      // Particle pass (amnesia/slang/Particle).
      auto p_vert = RIProgram::loadShaderStage(apResources->GetFileSearcher(),
                                               "Particle.vert.spv");
      auto p_frag = RIProgram::loadShaderStage(apResources->GetFileSearcher(),
                                               "Particle.frag.spv");
      std::array<RIProgram::ModuleStage, 2> stages = {
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, p_vert,
                                 "vsMain"},
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, p_frag,
                                 "psMain"}};
      m_particle.initialize(&mpGraphics->device, stages, externalLayouts, "Hybrid.particle");
    }
    {
      // Translucent mesh pass (amnesia/slang/Translucent). Shares
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
      m_translucentMesh.initialize(&mpGraphics->device, stages, externalLayouts, "Hybrid.translucentMesh");
    }
    {
      // Decal pass (amnesia/slang/Decal). Reuses the translucent 5-stream
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
      m_decal.initialize(&mpGraphics->device, stages, externalLayouts, "Hybrid.decal");
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
      m_water.initialize(&mpGraphics->device, stages, externalLayouts, "Hybrid.water");
    }

    RISegmentAllocDesc indirectDesc = {};
    indirectDesc.numSegments = RI_NUMBER_FRAMES_FLIGHT;
    indirectDesc.elementStride = sizeof(VkDrawIndirectCommand);
    indirectDesc.maxElements = kObjectSlotCapacity;
    m_indirectSegment = RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS>(&indirectDesc);
    m_indirectDrawBuffer = detail::CreateBindlessSlotBuffer(
        &mpGraphics->device, indirectDesc.maxElements, sizeof(VkDrawIndirectCommand),
        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
  }
}

void cViewport::HybridViewportState::Update(cGraphics::FrameContext *cntx,
                                            cVector2l size) {
  cGraphics* pGraphics = Interface<cGraphics>::Get();
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

  for (uint32_t i = 0; i < pGraphics->swapchain->imageCount; i++) {
    // Overscan HDR color target: the compute composite writes it as a
    // storage image, the forward raster passes attach it, cScene's pogo
    // feed blits its centered authored window out (TRANSFER_SRC).
    CreateViewportColorTexture(
        &pGraphics->device, renderW, renderH, cGraphics::PogoColorFormat,
        RI_USAGE_COLOR_ATTACHMENT | RI_USAGE_SHADER_RESOURCE |
            RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_TRANSFER_SRC |
            RI_USAGE_TRANSFER_DST,
        &renderTarget[i], &renderTargetView[i],
        "HybridViewportState.renderTarget");

    // SAMPLED lets the compute passes bind the depth as `sampler2D depthMap`
    // after the gbuffer pass flips it to SHADER_READ_ONLY.
    // DEPTH|STENCIL view: the Z passes only touch the depth aspect, but
    // cLuxEffectRenderer's outline pass binds this same view as a stencil
    // attachment (mark silhouette -> NOTEQUAL composite), so the view must
    // carry the stencil aspect. For sampling the depth aspect (soft particles)
    // we add a separate depth-only SRV below — a combined view can't be
    // sampled.
    CreateViewportAttachmentTexture(
        &pGraphics->device, renderW, renderH, cGraphics::DepthFormat,
        RI_USAGE_DEPTH_STENCIL_ATTACHMENT | RI_USAGE_SHADER_RESOURCE,
        RI_VIEWTYPE_DEPTH_STENCIL_ATTACHMENT, &depthTextures[i], &depthView[i],
        "HybridViewportState.depth");

    // Second view of the SAME depth image, depth-aspect only (SHADER_RESOURCE
    // view → RITextureView::create selects DEPTH_BIT, dropping the stencil bit
    // that makes the combined depthView above unsampleable). The particle pass
    // binds this as gSceneDepth for the soft-particle depth fade.
    {
      RITextureViewDesc dsv = {};
      dsv.viewType = RI_VIEWTYPE_SHADER_RESOURCE_2D;
      dsv.format = cGraphics::DepthFormat;
      dsv.mipNum = 1;
      dsv.layerNum = 1;
      RITextureView v =
          RITextureView::create(&pGraphics->device, depthTextures[i].Get(), dsv);
      depthSampleView[i] = RISharedPointer<RITextureView>(&pGraphics->device, v);
    }

    CreateViewportAttachmentTexture(
        &pGraphics->device, renderW, renderH, cGraphics::VisibilityFormat,
        RI_USAGE_COLOR_ATTACHMENT | RI_USAGE_SHADER_RESOURCE,
        RI_VIEWTYPE_SHADER_RESOURCE_2D, &visibilityTexture[i],
        &visibilityView[i], "HybridViewportState.visibility");

    // Packed visibility — RT pipeline storage write, sampled by the
    // path-tracing / direct / composite passes.
    CreateViewportAttachmentTexture(
        &pGraphics->device, renderW, renderH, cGraphics::VisibilityFormat,
        RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_SHADER_RESOURCE,
        RI_VIEWTYPE_SHADER_RESOURCE_2D, &packedHitInfoTexture[i],
        &packedHitInfoView[i], "HybridViewportState.packedHitInfo");

    // Screen-space velocity — gbuffer MRT #2, sampled by temporal passes.
    CreateViewportAttachmentTexture(
        &pGraphics->device, renderW, renderH, cGraphics::VelocityFormat,
        RI_USAGE_COLOR_ATTACHMENT | RI_USAGE_SHADER_RESOURCE,
        RI_VIEWTYPE_SHADER_RESOURCE_2D, &velocityTexture[i], &velocityView[i],
        "HybridViewportState.velocity");

    // Decal accumulators — Mul/MulX2 into decalMul, Add into decalAdd; the
    // composite applies albedo = albedo*decalMul + decalAdd before lighting.
    // RGBA16F (PogoColorFormat) so the linear factors don't band;
    // COLOR_ATTACHMENT for the raster, SHADER_RESOURCE for the composite read.
    CreateViewportAttachmentTexture(
        &pGraphics->device, renderW, renderH, cGraphics::PogoColorFormat,
        RI_USAGE_COLOR_ATTACHMENT | RI_USAGE_SHADER_RESOURCE,
        RI_VIEWTYPE_SHADER_RESOURCE_2D, &decalMulTexture[i], &decalMulView[i],
        "HybridViewportState.decalMul");
    CreateViewportAttachmentTexture(
        &pGraphics->device, renderW, renderH, cGraphics::PogoColorFormat,
        RI_USAGE_COLOR_ATTACHMENT | RI_USAGE_SHADER_RESOURCE,
        RI_VIEWTYPE_SHADER_RESOURCE_2D, &decalAddTexture[i], &decalAddView[i],
        "HybridViewportState.decalAdd");
  }

  // Still ping-ponged across frames: the ReSTIR DI surface key and reservoir.
  // DirectLightingPass reprojects last frame's reservoir and rejects on last
  // frame's key, so both need a history slot. STORAGE (compute write) +
  // SAMPLED (history reproject) + TRANSFER_DST (first-use clear); kept in
  // GENERAL, toggled per frame by directLightingIndex.
  for (uint32_t i = 0; i < 2; i++) {
    CreateViewportAttachmentTexture(
        &pGraphics->device, renderW, renderH, cGraphics::PogoColorFormat,
        RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_SHADER_RESOURCE |
            RI_USAGE_TRANSFER_DST,
        RI_VIEWTYPE_SHADER_RESOURCE_2D, &directKeyTexture[i], &directKeyView[i],
        "HybridViewportState.directKey");
    // ReSTIR reservoir history ping-pong (RGBA32F: asfloat(lightIndex), W, M).
    CreateViewportAttachmentTexture(
        &pGraphics->device, renderW, renderH, RI_FORMAT_RGBA32_SFLOAT,
        RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_SHADER_RESOURCE |
            RI_USAGE_TRANSFER_DST,
        RI_VIEWTYPE_SHADER_RESOURCE_2D, &reservoirTexture[i], &reservoirView[i],
        "HybridViewportState.reservoir");
  }

  // Everything below is written and consumed within a single frame — NRD owns
  // all denoiser history internally — so one slot each, no ping-pong.
  //
  // ReSTIR DI's resolved direct irradiance, and the path tracer's two lighting
  // channels plus the two halves of its surface key.
  CreateViewportAttachmentTexture(
      &pGraphics->device, renderW, renderH, cGraphics::PogoColorFormat,
      RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_SHADER_RESOURCE |
          RI_USAGE_TRANSFER_DST,
      RI_VIEWTYPE_SHADER_RESOURCE_2D, &directLightingTexture,
      &directLightingView, "HybridViewportState.directLighting");
  CreateViewportAttachmentTexture(
      &pGraphics->device, renderW, renderH, cGraphics::PogoColorFormat,
      RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_SHADER_RESOURCE |
          RI_USAGE_TRANSFER_DST,
      RI_VIEWTYPE_SHADER_RESOURCE_2D, &indirectRadianceTexture,
      &indirectRadianceView, "HybridViewportState.indirectRadiance");
  CreateViewportAttachmentTexture(
      &pGraphics->device, renderW, renderH, cGraphics::PogoColorFormat,
      RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_SHADER_RESOURCE |
          RI_USAGE_TRANSFER_DST,
      RI_VIEWTYPE_SHADER_RESOURCE_2D, &indirectSpecularTexture,
      &indirectSpecularView, "HybridViewportState.indirectSpecular");
  CreateViewportAttachmentTexture(
      &pGraphics->device, renderW, renderH, cGraphics::PogoColorFormat,
      RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_SHADER_RESOURCE |
          RI_USAGE_TRANSFER_DST,
      RI_VIEWTYPE_SHADER_RESOURCE_2D, &indirectKeyTexture, &indirectKeyView,
      "HybridViewportState.indirectKey");
  // Second half of the indirect surface key: primary-hit GGX alpha in .x,
  // diffuse primary hit distance in metres in .y, specular primary hit distance
  // in .z; both are 0 for no hit/skipped lobe, and .w is reserved.
  CreateViewportAttachmentTexture(
      &pGraphics->device, renderW, renderH, cGraphics::PogoColorFormat,
      RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_SHADER_RESOURCE |
          RI_USAGE_TRANSFER_DST,
      RI_VIEWTYPE_SHADER_RESOURCE_2D, &indirectKeyExtraTexture,
      &indirectKeyExtraView, "HybridViewportState.indirectKeyExtra");

  // NRD frontend inputs. The normal target uses the exact configured
  // R10G10B10A2_UNORM NRD encoding; the radiance targets carry YCoCg +
  // normalized hit distance in RGBA16F; viewZ is a linear R32F guide.
  CreateViewportAttachmentTexture(
      &pGraphics->device, renderW, renderH, RI_FORMAT_R10_G10_B10_A2_UNORM,
      RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_SHADER_RESOURCE |
          RI_USAGE_TRANSFER_DST,
      RI_VIEWTYPE_SHADER_RESOURCE_2D, &nrdNormalRoughnessTexture,
      &nrdNormalRoughnessView, "HybridViewportState.nrdNormalRoughness");
  CreateViewportAttachmentTexture(
      &pGraphics->device, renderW, renderH, RI_FORMAT_R32_SFLOAT,
      RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_SHADER_RESOURCE |
          RI_USAGE_TRANSFER_DST,
      RI_VIEWTYPE_SHADER_RESOURCE_2D, &nrdViewZTexture, &nrdViewZView,
      "HybridViewportState.nrdViewZ");
  CreateViewportAttachmentTexture(
      &pGraphics->device, renderW, renderH, cGraphics::PogoColorFormat,
      RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_SHADER_RESOURCE |
          RI_USAGE_TRANSFER_DST,
      RI_VIEWTYPE_SHADER_RESOURCE_2D, &nrdDiffuseRadianceHitDistTexture,
      &nrdDiffuseRadianceHitDistView,
      "HybridViewportState.nrdDiffuseRadianceHitDist");
  CreateViewportAttachmentTexture(
      &pGraphics->device, renderW, renderH, cGraphics::PogoColorFormat,
      RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_SHADER_RESOURCE |
          RI_USAGE_TRANSFER_DST,
      RI_VIEWTYPE_SHADER_RESOURCE_2D, &nrdSpecularRadianceHitDistTexture,
      &nrdSpecularRadianceHitDistView,
      "HybridViewportState.nrdSpecularRadianceHitDist");
  // NRD declares IN_MV as an output in temporal stabilization. Keep this
  // RG16F copy private to NRD; the shared velocity attachment remains a
  // read-only input for the rest of the frame.
  CreateViewportAttachmentTexture(
      &pGraphics->device, renderW, renderH, cGraphics::VelocityFormat,
      RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_SHADER_RESOURCE |
          RI_USAGE_TRANSFER_DST,
      RI_VIEWTYPE_SHADER_RESOURCE_2D, &nrdMotionVectorsTexture,
      &nrdMotionVectorsView, "HybridViewportState.nrdMotionVectors");

  // The denoiser is per-viewport: NRD sizes its history and pools to one
  // extent, so sharing one instance across differently-sized viewports would
  // thrash both. Recreating on resize would also be wasteful, hence OnResize.
  if (!nrd)
    nrd = std::make_shared<NrdIntegration>(pGraphics);
  nrd->OnResize(renderW, renderH);
  // Resource recreation invalidates every temporal history.
  indirectHistoryReset = true;
  nrdInputInShaderResource = false;

  // Intra-frame reservoir hand-off (temporal pass → spatial pass), RGBA32F.
  CreateViewportAttachmentTexture(
      &pGraphics->device, renderW, renderH, RI_FORMAT_RGBA32_SFLOAT,
      RI_USAGE_SHADER_RESOURCE_STORAGE | RI_USAGE_SHADER_RESOURCE |
          RI_USAGE_TRANSFER_DST,
      RI_VIEWTYPE_SHADER_RESOURCE_2D, &reservoirTemporalTexture,
      &reservoirTemporalView, "HybridViewportState.reservoirTemporal");

  // Recreation invalidated every history: re-arm the one-time direct- and
  // indirect-lighting init/clear and re-seed prev-camera = current on the
  // next Draw.
  directLightingIndex = 0;
  directLightingInit = false;
  indirectLightingInit = false;
  hasPrevCamera = false;
}

cViewport::HybridViewportState::~HybridViewportState() {
  cGraphics* pGraphics = Interface<cGraphics>::Get();
  for (uint32_t i = 0; i < RI_MAX_SWAPCHAIN_IMAGES; i++) {
    pGraphics->graphicsDefer.push(renderTarget[i]);
    pGraphics->graphicsDefer.push(depthTextures[i]);
    pGraphics->graphicsDefer.push(visibilityTexture[i]);
    pGraphics->graphicsDefer.push(packedHitInfoTexture[i]);
    pGraphics->graphicsDefer.push(velocityTexture[i]);
    pGraphics->graphicsDefer.push(decalMulTexture[i]);
    pGraphics->graphicsDefer.push(decalAddTexture[i]);

    pGraphics->graphicsDefer.push(renderTargetView[i]);
    pGraphics->graphicsDefer.push(depthView[i]);
    pGraphics->graphicsDefer.push(depthSampleView[i]);
    pGraphics->graphicsDefer.push(visibilityView[i]);
    pGraphics->graphicsDefer.push(packedHitInfoView[i]);
    pGraphics->graphicsDefer.push(velocityView[i]);
    pGraphics->graphicsDefer.push(decalMulView[i]);
    pGraphics->graphicsDefer.push(decalAddView[i]);
  }
  // Ping-ponged pair: the ReSTIR key and reservoir.
  for (uint32_t i = 0; i < 2; i++) {
    pGraphics->graphicsDefer.push(directKeyTexture[i]);
    pGraphics->graphicsDefer.push(reservoirTexture[i]);
    pGraphics->graphicsDefer.push(directKeyView[i]);
    pGraphics->graphicsDefer.push(reservoirView[i]);
  }
  // NRD owns Vulkan pipelines and its own texture pool, and it disposes them
  // eagerly rather than through graphicsDefer. Destroying it here would call
  // vkDestroyPipeline on pipelines the in-flight command buffers still
  // reference (the viewport dies while frames are outstanding, unlike the
  // renderer, which is torn down after the device is idle). So hand ownership
  // to the deferral: the lambda runs once the GPU has passed this frame.
  if (nrd)
    pGraphics->graphicsDefer.push(
        std::function<void()>([keep = std::move(nrd)]() mutable { keep.reset(); }));

  // Single-slot, intra-frame resources.
  pGraphics->graphicsDefer.push(directLightingTexture);
  pGraphics->graphicsDefer.push(indirectRadianceTexture);
  pGraphics->graphicsDefer.push(indirectSpecularTexture);
  pGraphics->graphicsDefer.push(indirectKeyTexture);
  pGraphics->graphicsDefer.push(indirectKeyExtraTexture);
  pGraphics->graphicsDefer.push(nrdNormalRoughnessTexture);
  pGraphics->graphicsDefer.push(nrdViewZTexture);
  pGraphics->graphicsDefer.push(nrdDiffuseRadianceHitDistTexture);
  pGraphics->graphicsDefer.push(nrdSpecularRadianceHitDistTexture);
  pGraphics->graphicsDefer.push(nrdMotionVectorsTexture);

  pGraphics->graphicsDefer.push(directLightingView);
  pGraphics->graphicsDefer.push(indirectRadianceView);
  pGraphics->graphicsDefer.push(indirectSpecularView);
  pGraphics->graphicsDefer.push(indirectKeyView);
  pGraphics->graphicsDefer.push(indirectKeyExtraView);
  pGraphics->graphicsDefer.push(nrdNormalRoughnessView);
  pGraphics->graphicsDefer.push(nrdViewZView);
  pGraphics->graphicsDefer.push(nrdDiffuseRadianceHitDistView);
  pGraphics->graphicsDefer.push(nrdSpecularRadianceHitDistView);
  pGraphics->graphicsDefer.push(nrdMotionVectorsView);
  pGraphics->graphicsDefer.push(reservoirTemporalTexture);
  pGraphics->graphicsDefer.push(reservoirTemporalView);
}

// Defer our current resources (~HybridViewportState defers each shared handle
// to the graphics freelist), then move-construct rhs's shared handles into us
// (a pointer steal — no refcount churn, no dispose). The resize path uses this
// as
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
    if (!buf) {
      // Null before the first PrepareFrame bake. Push an empty descriptor
      // flagged optional rather than nothing: an empty entry is skipped by the
      // write/hash exactly as before, but it tells the debug unwritten-binding
      // check the omission is intentional.
      bnd.emplace_back(name, RIDescriptor(), 0, true);
      return;
    }
    bnd.emplace_back(
        name, RIDescriptor::storageBuffer(
                  &Interface<cGraphics>::Get()->device, buf, 0, std::max<uint32_t>(cnt, 1u) * stride));
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

void cHybridRenderer::Draw(cGraphics::FrameContext *cntx, cViewport *viewport,
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

  // NOTE: HybridViewportState::Update creates and sizes state.nrd alongside the
  // packed-input textures, so it is non-null and correctly sized here.
  //
  // NOTE: cameraJitter is deliberately left at zero in NrdIntegration because
  // perFrame.jitterX/Y are hard-zeroed below ("TAA not yet wired up"). Whoever
  // lands TAA must plumb the jitter into CommonSettings at the same time.

  ml::float4x4 mainFrustumViewInvMat = apFrustum->GetViewMat();
  mainFrustumViewInvMat.Invert();
  const ml::float4x4 mainFrustumViewMat = apFrustum->GetViewMat();
  ml::float4x4 mainFrustumProjMat = apFrustum->GetProjectionMat();
  ml::float4x4 mainFrustumProjInvMat = mainFrustumProjMat;
  mainFrustumProjInvMat.Invert();
  {
    m_rendererList.BeginAndReset(afFrameTime, apFrustum);
    auto *dynamicContainer =
        apWorld->GetRenderableSet(eWorldContainerType_Dynamic);
    auto *staticContainer =
        apWorld->GetRenderableSet(eWorldContainerType_Static);
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
    // translucent/particle Update* work bounded. Whole-map RT geometry
    // (shadows/ GI need everything, including behind the camera) is no longer
    // sourced here — cWorld::PrepareFrame walks its own renderables unculled to
    // build the TLAS.
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
    // value and defers it (RISharedPointer on Interface<cGraphics>::Get()->graphicsDefer) on rebuild and
    // in its destructor, so any BLAS the TLAS can still reference outlives the
    // in-flight window even after its owning renderable is destroyed.
  }

  // --------------------------------------------------------------------
  // Per-frame prepare for every VISIBLE translucent renderable (particles +
  // meshes + billboards + beams). UpdateGraphicsForFrame/ForViewport recompute
  // dynamic geometry (billboard facing, beam stretch, emitter step) and mark
  // the VB dirty; SubmitToGPU then allocates/uploads dirty streams for the
  // raster particle + mesh passes. The render list is frustum-culled above, so
  // only the on-screen set pays this cost. BLAS builds for ray-traced meshes
  // happen in cWorld::PrepareFrame (TLAS owner), not here.
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
      vbri->SubmitToGPU(&mpGraphics->blasSubmit.cmds[0], &mpGraphics->device, cntx);
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
      vbri->SubmitToGPU(&mpGraphics->blasSubmit.cmds[0], &mpGraphics->device, cntx);
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
  perFrame.totalFrames = mpGraphics->frameIndex;
  perFrame.cameraFov = apFrustum->GetFOV();
  perFrame.fireflyClampThreshold = 10.0f;
  perFrame.zNear = apFrustum->GetNearPlane();
  perFrame.zFar = apFrustum->GetFarPlane();
  perFrame.allLightsCastShadows = mpGraphics->allLightsCastShadows ? 1u : 0u;
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
  // World fog — copy the per-world settings into the per-frame UBO so
  // Fog.slang's world-fog block activates. Mirrors cWorld::BuildFogParams
  // colour handling (sRGB->linear, alpha kept linear). Leaving worldFogLength
  // at 0 (the default zero-init) disables world fog, matching the shader's
  // `worldFogLength > 0` guard.
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
    // above, so Scene.slang's pinhole ray basis stays in sync with the raster
    // projection that covers the overscan frame. Primary hits and velocity
    // themselves come from VBufferRaster.3d, not from a traced primary ray.
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
  // Lights are no longer pulled from the per-frame render list — cWorld owns
  // the per-world light buffers (rebuilt once per frame by
  // cWorld::PrepareFrame, driven from cScene before the viewport loop).
  RISegmentReq indirectReq = {};
  const bool indirectOk =
      m_indirectSegment.request(mpGraphics->frameIndex, solids.size(), &indirectReq);
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

  // state.packedHitInfoView and the
  // freshly built TLAS now live on set 1 and are pushed per-dispatch via
  // RIProgram::bindDescriptors below (see the m_vBufferPomBary / m_pathTrace
  // / m_composite call sites).
  // Set 1 is allocated from a frame-rotated pool, so each frame's writes
  // land on an idle descriptor set.

  std::vector<RIProgram::DescriptorBinding> bindings;
  bindings.reserve(16);
  // Per-pass image / TLAS bindings are pushed inline below. Note: the storage
  // image `gPackedHitInfo` uses GENERAL layout, which satisfies both storage
  // and sampled access.
  {
    RIProgram::DescriptorBinding b;
    b.handle = DescriptorBindingID::Create("gPerFrame");
    mpGraphics->UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
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
        {state.visibilityTexture[mpGraphics->swapchainIndex].Get(),
         RI_RESOURCE_STATE_UNDEFINED, RI_RESOURCE_STATE_RENDER_TARGET},
        {state.depthTextures[mpGraphics->swapchainIndex].Get(),
         RI_RESOURCE_STATE_UNDEFINED, RI_RESOURCE_STATE_DEPTH_WRITE,
         RI_STAGE_NONE, RI_STAGE_NONE, RI_BARRIER_ASPECT_DEPTH},
        // Velocity MRT — same UNDEFINED→COLOR transition as the visibility
        // target (loadOp=CLEAR, so prior contents don't matter).
        {state.velocityTexture[mpGraphics->swapchainIndex].Get(),
         RI_RESOURCE_STATE_UNDEFINED, RI_RESOURCE_STATE_RENDER_TARGET}};
    mpGraphics->primary.cmds[0].vk_d3d12_textureBarriers<3>(3, attachmentBarriers);
  }

  // MRT color targets, both cleared to all-zero. Visibility: psMain writes .w=1
  // (valid hit sentinel) or zero on sky/miss pixels (clear value). Velocity:
  // static/uncovered pixels read zero motion. (uint vs float clear is
  // bit-identical at zero.)
  RIRenderingAttachment gbufferColorAttachments[2] = {};
  gbufferColorAttachments[0].view = *state.visibilityView[mpGraphics->swapchainIndex];
  gbufferColorAttachments[0].loadOp = RI_ATTACHMENT_LOAD_OP_CLEAR;
  gbufferColorAttachments[0].storeOp = RI_ATTACHMENT_STORE_OP_STORE;
  gbufferColorAttachments[1].view = *state.velocityView[mpGraphics->swapchainIndex];
  gbufferColorAttachments[1].loadOp = RI_ATTACHMENT_LOAD_OP_CLEAR;
  gbufferColorAttachments[1].storeOp = RI_ATTACHMENT_STORE_OP_STORE;

  // MRT owns the per-frame depth clear.
  RIRenderingAttachment depthAttachment = {};
  depthAttachment.view = *state.depthView[mpGraphics->swapchainIndex];
  depthAttachment.loadOp = RI_ATTACHMENT_LOAD_OP_CLEAR;
  depthAttachment.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
  depthAttachment.clearValue.depth = 1.0f;

  // ----------------------------------------------------------------------
  // World-space light grid build (feeds the path tracer's NEE importance
  // sampling + the Composite direct cull). Per-cell gather: one thread
  // per grid cell walks the light list and writes that cell's count + list. The
  // light SSBOs were uploaded + barriered to SHADER_READ earlier this frame, so
  // binLights reads them directly. No per-cell count clear is needed — every
  // cell's count is written unconditionally by its thread. Runs before any
  // consumer of the grid (direct lighting, path trace, composite).
  // ----------------------------------------------------------------------
  {
    VkComputePipelineCreateInfo computeCreate = {
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    RIGpuScope _gsLightGrid(&mpGraphics->profiler, &mpGraphics->primary.cmds[0],
                               "LightGrid");
    m_lightGrid.bindComputePipeline(&mpGraphics->device, &mpGraphics->primary.cmds[0], kHash,
                                       "LightGrid.cs:binLights",
                                       &computeCreate);
    m_lightGrid.bindBindlessDescriptorSet(&mpGraphics->primary.cmds[0],
                                             &mpGraphics->globalset->m_bindlessSet, 0,
                                             VK_PIPELINE_BIND_POINT_COMPUTE);
    std::vector<RIProgram::DescriptorBinding> bnd;
    bnd.reserve(1);
    {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("gPerFrame");
      mpGraphics->UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
      bnd.push_back(b);
    }
    appendWorldLightFog(bnd, apWorld);
    m_lightGrid.bindDescriptors(&mpGraphics->device, &mpGraphics->primary.cmds[0],
                                   mpGraphics->frameIndex, bnd.data(), bnd.size(),
                                   VK_PIPELINE_BIND_POINT_COMPUTE);
    // One thread per grid cell (the shader early-outs past
    // kLightGridCellCount).
    mpGraphics->primary.cmds[0].dispatch(&mpGraphics->device, (kLightGridCellCount + 63u) / 64u,
                                1u, 1u);
  }

  {
    // binLights writes are read by several consumers later this frame: the
    // path tracer's NEE (ray tracing), the direct-lighting pass (compute) and
    // the MainCompositePass direct cull (fragment — evalAnalyticLight walks the
    // per-cell light list). Every stage must be in dst or the fragment reads
    // see an empty grid and drop every point/spot light.
    mpGraphics->primary.cmds[0].vk_d3d12_memoryBarrier(
        {RI_RESOURCE_STATE_STORAGE_WRITE, RI_RESOURCE_STATE_STORAGE_READ,
         RI_STAGE_COMPUTE,
         RI_STAGE_RAY_TRACING | RI_STAGE_COMPUTE | RI_STAGE_FRAGMENT});
  }

  RIBeginRenderingDesc gbufferBeginDesc = {};
  gbufferBeginDesc.renderArea.width = (int16_t)renderWidth;
  gbufferBeginDesc.renderArea.height = (int16_t)renderHeight;
  gbufferBeginDesc.colorCount = 2;
  gbufferBeginDesc.colors = gbufferColorAttachments;
  gbufferBeginDesc.depthStencil = &depthAttachment;
  {
    RIGpuScope _gsGBuffer(&mpGraphics->profiler, &mpGraphics->primary.cmds[0], "GBuffer");
    mpGraphics->primary.cmds[0].vk_d3d12_beginRendering(&mpGraphics->device, gbufferBeginDesc);

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
    mpGraphics->primary.cmds[0].setViewport(&mpGraphics->device, vkViewport);
    mpGraphics->primary.cmds[0].setScissor(&mpGraphics->device, scissor);

    if (writtenDraws > 0) {
      GBufferMRTPipelineDesc pipelineDesc(cGraphics::VisibilityFormat,
                                          cGraphics::VelocityFormat,
                                          cGraphics::DepthFormat);
      m_gbuffer.bindPipeline(&mpGraphics->device, &mpGraphics->primary.cmds[0], pipelineDesc.hash,
                             "VBufferRaster.3d", &pipelineDesc.createInfo);
      m_gbuffer.bindBindlessDescriptorSet(&mpGraphics->primary.cmds[0],
                                          &mpGraphics->globalset->m_bindlessSet, 0);
      m_gbuffer.bindDescriptors(&mpGraphics->device, &mpGraphics->primary.cmds[0], mpGraphics->frameIndex,
                                bindings.data(), bindings.size());
      mpGraphics->primary.cmds[0].drawIndirect(&mpGraphics->device, &m_indirectDrawBuffer,
                                      (VkDeviceSize)indirectReq.elementOffset *
                                          sizeof(VkDrawIndirectCommand),
                                      writtenDraws,
                                      (uint32_t)sizeof(VkDrawIndirectCommand));
    }

    mpGraphics->primary.cmds[0].vk_d3d12_endRendering(&mpGraphics->device);
  }

  // Gbuffer output -> SHADER_READ_ONLY for the downstream compute
  // passes (and any later fragment consumer). Includes depth, which the
  // gbuffer left in DEPTH_STENCIL_ATTACHMENT_OPTIMAL.
  // packedHitInfoTexture transitions UNDEFINED -> STORAGE_WRITE for the
  // POM compute pass that follows immediately.
  {
    RITextureBarrier toRead[4] = {};
    // Visibility -> SHADER_READ for the fragment + compute consumers.
    toRead[0] = {state.visibilityTexture[mpGraphics->swapchainIndex].Get(),
                 RI_RESOURCE_STATE_RENDER_TARGET,
                 RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_NONE,
                 RI_STAGE_FRAGMENT | RI_STAGE_COMPUTE};

    // Depth -> SHADER_READ_ONLY for the compute pass.
    toRead[1] = {state.depthTextures[mpGraphics->swapchainIndex].Get(),
                 RI_RESOURCE_STATE_DEPTH_WRITE,
                 RI_RESOURCE_STATE_SHADER_RESOURCE,
                 RI_STAGE_NONE,
                 RI_STAGE_COMPUTE,
                 RI_BARRIER_ASPECT_DEPTH};

    // Velocity (gbuffer MRT) -> SHADER_READ for the direct-lighting pass.
    toRead[2] = {state.velocityTexture[mpGraphics->swapchainIndex].Get(),
                 RI_RESOURCE_STATE_RENDER_TARGET,
                 RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_NONE,
                 RI_STAGE_COMPUTE};

    // packedHitInfo: UNDEFINED -> STORAGE_WRITE for the POM compute pass.
    // Previously written by the Stage B RT V-buffer; now produced by
    // VBufferPomBary.cs immediately after this barrier.
    toRead[3] = {state.packedHitInfoTexture[mpGraphics->swapchainIndex].Get(),
                 RI_RESOURCE_STATE_UNDEFINED, RI_RESOURCE_STATE_STORAGE_WRITE,
                 RI_STAGE_NONE, RI_STAGE_COMPUTE};

    mpGraphics->primary.cmds[0].vk_d3d12_textureBarriers<4>(4, toRead);
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
    RIGpuScope _gsVBufferPomBary(&mpGraphics->profiler, &mpGraphics->primary.cmds[0],
                                "VBufferPomBary");
    VkComputePipelineCreateInfo ci = {
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    const hash_t kPomHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    m_vBufferPomBary.bindComputePipeline(&mpGraphics->device, &mpGraphics->primary.cmds[0],
                                        kPomHash, "VBufferPomBary.cs", &ci);
    m_vBufferPomBary.bindBindlessDescriptorSet(&mpGraphics->primary.cmds[0],
                                              &mpGraphics->globalset->m_bindlessSet, 0,
                                              VK_PIPELINE_BIND_POINT_COMPUTE);

    std::vector<RIProgram::DescriptorBinding> pomBnd;
    pomBnd.reserve(3);
    {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("gPerFrame");
      mpGraphics->UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
      pomBnd.push_back(b);
    }
    pomBnd.emplace_back("gPackedHitInfoRaster",
                        RIDescriptor::sampledImage(
                            &mpGraphics->device,
                            state.visibilityView[mpGraphics->swapchainIndex].Get(),
                            RI_RESOURCE_STATE_SHADER_RESOURCE));
    pomBnd.emplace_back(
        "gPackedHitInfo",
        RIDescriptor::storageImage(
            &mpGraphics->device, state.packedHitInfoView[mpGraphics->swapchainIndex].Get()));

    m_vBufferPomBary.bindDescriptors(&mpGraphics->device, &mpGraphics->primary.cmds[0],
                                    mpGraphics->frameIndex, pomBnd.data(), pomBnd.size(),
                                    VK_PIPELINE_BIND_POINT_COMPUTE);
    mpGraphics->primary.cmds[0].dispatch(&mpGraphics->device, (renderWidth + 15u) / 16u,
                                (renderHeight + 15u) / 16u, 1u);
  }
  {
    // packedHitInfo storage write -> shader read for integrate/generate/
    // direct-lighting/composite passes. Layout stays GENERAL.
    mpGraphics->primary.cmds[0].vk_d3d12_memoryBarrier(
        {RI_RESOURCE_STATE_STORAGE_WRITE, RI_RESOURCE_STATE_STORAGE_READ,
         RI_STAGE_COMPUTE,
         RI_STAGE_COMPUTE | RI_STAGE_FRAGMENT | RI_STAGE_RAY_TRACING});
  }

  // --------------------------------------------------------------------
  // ReSTIR DI chain — DirectLighting -> DirectSpatialReuse, run HERE, before
  // the path tracer. This is variance REDUCTION, not denoising: it stabilizes
  // light selection and resolves one shadow ray per pixel. The resolved,
  // albedo-demodulated irradiance is handed to NrdPack, which sums it into
  // REBLUR's diffuse channel so one denoiser filters direct and indirect
  // together. Its output is NOT fed back into the path tracer.
  // --------------------------------------------------------------------
  // The resolved direct irradiance NrdPack sums into the diffuse channel.
  RITextureView *directResultView = nullptr;
  {
    const uint32_t dlCur = state.directLightingIndex;
    const uint32_t dlPrev = dlCur ^ 1u;

    if (!state.directLightingInit) {
      // First use: the direct target plus the ping-ponged key/reservoir
      // textures UNDEFINED -> GENERAL + cleared so the history reads are
      // defined; they stay GENERAL thereafter.
      RITextureBarrier toGen[6] = {
          {state.directLightingTexture.Get(), RI_RESOURCE_STATE_UNDEFINED,
           RI_RESOURCE_STATE_CLEAR_STORAGE},
          {state.directKeyTexture[0].Get(), RI_RESOURCE_STATE_UNDEFINED,
           RI_RESOURCE_STATE_CLEAR_STORAGE},
          {state.directKeyTexture[1].Get(), RI_RESOURCE_STATE_UNDEFINED,
           RI_RESOURCE_STATE_CLEAR_STORAGE},
          {state.reservoirTexture[0].Get(), RI_RESOURCE_STATE_UNDEFINED,
           RI_RESOURCE_STATE_CLEAR_STORAGE},
          {state.reservoirTexture[1].Get(), RI_RESOURCE_STATE_UNDEFINED,
           RI_RESOURCE_STATE_CLEAR_STORAGE},
          {state.reservoirTemporalTexture.Get(), RI_RESOURCE_STATE_UNDEFINED,
           RI_RESOURCE_STATE_CLEAR_STORAGE}};
      mpGraphics->primary.cmds[0].vk_d3d12_textureBarriers<6>(6, toGen);

      const float clr[4] = {0.0f, 0.0f, 0.0f, 0.0f};
      for (uint32_t i = 0; i < 6; ++i)
        mpGraphics->primary.cmds[0].clearStorageImage(&mpGraphics->device, toGen[i].texture, clr);

      mpGraphics->primary.cmds[0].vk_d3d12_memoryBarrier(
          {RI_RESOURCE_STATE_CLEAR_STORAGE,
           RI_RESOURCE_STATE_SHADER_RESOURCE | RI_RESOURCE_STATE_STORAGE_WRITE,
           RI_STAGE_NONE, RI_STAGE_COMPUTE});
      state.directLightingInit = true;
    } else {
      // Make last frame's writes to the ping-pong textures visible (history
      // sampled-read + current write-after-read/write). Both stay GENERAL.
      mpGraphics->primary.cmds[0].vk_d3d12_memoryBarrier(
          {RI_RESOURCE_STATE_STORAGE_WRITE | RI_RESOURCE_STATE_SHADER_RESOURCE,
           RI_RESOURCE_STATE_SHADER_RESOURCE | RI_RESOURCE_STATE_STORAGE_WRITE,
           RI_STAGE_COMPUTE, RI_STAGE_COMPUTE});
    }

    VkComputePipelineCreateInfo ci = {
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    {
      RIGpuScope _gsDirectLighting(&mpGraphics->profiler, &mpGraphics->primary.cmds[0],
                                   "DirectLighting");
      m_directLighting.bindComputePipeline(&mpGraphics->device, &mpGraphics->primary.cmds[0],
                                           kHash, "DirectLightingPass.cs", &ci);
      m_directLighting.bindBindlessDescriptorSet(
          &mpGraphics->primary.cmds[0], &mpGraphics->globalset->m_bindlessSet, 0,
          VK_PIPELINE_BIND_POINT_COMPUTE);

      std::vector<RIProgram::DescriptorBinding> bnd;
      bnd.reserve(8);
      {
        RIProgram::DescriptorBinding b;
        b.handle = DescriptorBindingID::Create("gPerFrame");
        mpGraphics->UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
        bnd.push_back(b);
      }
      // Temporal pass traces no rays — only builds + reprojects reservoirs.
      bnd.emplace_back(
          "gPackedHitInfo",
          RIDescriptor::storageImage(
              &mpGraphics->device, state.packedHitInfoView[mpGraphics->swapchainIndex].Get()));
      bnd.emplace_back("gVelocity",
                       RIDescriptor::sampledImage(
                           &mpGraphics->device,
                           state.velocityView[mpGraphics->swapchainIndex].Get(),
                           RI_RESOURCE_STATE_SHADER_RESOURCE));
      bnd.emplace_back("gReservoirHistory",
                       RIDescriptor::sampledImage(
                           &mpGraphics->device, state.reservoirView[dlPrev].Get(),
                           RI_RESOURCE_STATE_GENERAL));
      bnd.emplace_back("gDirectKeyHistory",
                       RIDescriptor::sampledImage(
                           &mpGraphics->device, state.directKeyView[dlPrev].Get(),
                           RI_RESOURCE_STATE_GENERAL));
      bnd.emplace_back("gReservoirOut",
                       RIDescriptor::storageImage(
                           &mpGraphics->device, state.reservoirTemporalView.Get()));
      bnd.emplace_back("gDirectKeyOut",
                       RIDescriptor::storageImage(
                           &mpGraphics->device, state.directKeyView[dlCur].Get()));

      appendWorldLightFog(bnd, apWorld);
      m_directLighting.bindDescriptors(&mpGraphics->device, &mpGraphics->primary.cmds[0],
                                       mpGraphics->frameIndex, bnd.data(), bnd.size(),
                                       VK_PIPELINE_BIND_POINT_COMPUTE);
      mpGraphics->primary.cmds[0].dispatch(&mpGraphics->device, (renderWidth + 15u) / 16u,
                                  (renderHeight + 15u) / 16u, 1u);
    }

    // Temporal reservoir + current key writes -> spatial pass sampled reads.
    mpGraphics->primary.cmds[0].vk_d3d12_memoryBarrier(
        {RI_RESOURCE_STATE_STORAGE_WRITE, RI_RESOURCE_STATE_SHADER_RESOURCE,
         RI_STAGE_COMPUTE, RI_STAGE_COMPUTE});

    // ----------------------------------------------------------------
    // DirectSpatialReusePass — ReSTIR DI spatial reuse + resolve. Merges a few
    // same-surface neighbours' reservoirs, then traces ONE soft shadow ray for
    // the chosen light to demodulated irradiance. Writes reservoir[dlCur] (next
    // frame's temporal history) and directLighting (NrdPack's direct input).
    // ----------------------------------------------------------------
    {
      RIGpuScope _gsDirectSpatialReuse(&mpGraphics->profiler, &mpGraphics->primary.cmds[0],
                                       "DirectSpatialReuse");
      m_directSpatialReuse.bindComputePipeline(
          &mpGraphics->device, &mpGraphics->primary.cmds[0], kHash, "DirectSpatialReusePass.cs",
          &ci);
      m_directSpatialReuse.bindBindlessDescriptorSet(
          &mpGraphics->primary.cmds[0], &mpGraphics->globalset->m_bindlessSet, 0,
          VK_PIPELINE_BIND_POINT_COMPUTE);

      std::vector<RIProgram::DescriptorBinding> sb;
      sb.reserve(8);
      {
        RIProgram::DescriptorBinding b;
        b.handle = DescriptorBindingID::Create("gPerFrame");
        mpGraphics->UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
        sb.push_back(b);
      }
      sb.emplace_back(
          "gPackedHitInfo",
          RIDescriptor::storageImage(
              &mpGraphics->device, state.packedHitInfoView[mpGraphics->swapchainIndex].Get()));
      // optional: the TLAS is null until the first build, and this call site
      // (unlike the path tracer's) isn't guarded on it.
      sb.emplace_back(
          "gRtAccel",
          RIDescriptor::accelerationStructure(
              &mpGraphics->device, apWorld->GetTlas()), // resolve shadow ray
          0, true);
      sb.emplace_back("gReservoirIn",
                      RIDescriptor::sampledImage(
                          &mpGraphics->device, state.reservoirTemporalView.Get(),
                          RI_RESOURCE_STATE_GENERAL));
      sb.emplace_back("gDirectKey",
                      RIDescriptor::sampledImage(
                          &mpGraphics->device, state.directKeyView[dlCur].Get(),
                          RI_RESOURCE_STATE_GENERAL));
      sb.emplace_back("gReservoirOut",
                      RIDescriptor::storageImage(
                          &mpGraphics->device, state.reservoirView[dlCur].Get()));
      sb.emplace_back("gDirectLighting",
                      RIDescriptor::storageImage(
                          &mpGraphics->device, state.directLightingView.Get()));

      appendWorldLightFog(sb, apWorld);
      m_directSpatialReuse.bindDescriptors(&mpGraphics->device, &mpGraphics->primary.cmds[0],
                                           mpGraphics->frameIndex, sb.data(), sb.size(),
                                           VK_PIPELINE_BIND_POINT_COMPUTE);
      mpGraphics->primary.cmds[0].dispatch(&mpGraphics->device, (renderWidth + 15u) / 16u,
                                  (renderHeight + 15u) / 16u, 1u);
    }

    // Resolved direct + final reservoir writes -> NrdPack / next-frame reads
    // (stays GENERAL).
    mpGraphics->primary.cmds[0].vk_d3d12_memoryBarrier(
        {RI_RESOURCE_STATE_STORAGE_WRITE, RI_RESOURCE_STATE_SHADER_RESOURCE,
         RI_STAGE_COMPUTE, RI_STAGE_COMPUTE});

    // The resolved direct irradiance goes straight to NrdPack, which sums it
    // into REBLUR's diffuse channel. There is no spatial filter here any more:
    // REBLUR owns all denoising for that channel.
    directResultView = state.directLightingView.Get();
  }

  // What the composite samples: REBLUR's two denoised outputs. One per lobe —
  // diffuse (albedo-demodulated, carrying direct + indirect) and specular
  // (undemodulated). Declared at Draw scope; the composite below binds them.
  RITextureView *indirectResultView = nullptr;
  RITextureView *indirectSpecularResultView = nullptr;

  if (!state.indirectLightingInit) {
    // First use: UNDEFINED -> GENERAL + cleared so every read is defined even
    // on a frame with no TLAS; they stay GENERAL thereafter.
    RITextureBarrier toGen[9] = {
        {state.indirectRadianceTexture.Get(), RI_RESOURCE_STATE_UNDEFINED,
         RI_RESOURCE_STATE_CLEAR_STORAGE},
        {state.indirectSpecularTexture.Get(), RI_RESOURCE_STATE_UNDEFINED,
         RI_RESOURCE_STATE_CLEAR_STORAGE},
        {state.indirectKeyTexture.Get(), RI_RESOURCE_STATE_UNDEFINED,
         RI_RESOURCE_STATE_CLEAR_STORAGE},
        {state.indirectKeyExtraTexture.Get(), RI_RESOURCE_STATE_UNDEFINED,
         RI_RESOURCE_STATE_CLEAR_STORAGE},
        {state.nrdNormalRoughnessTexture.Get(), RI_RESOURCE_STATE_UNDEFINED,
         RI_RESOURCE_STATE_CLEAR_STORAGE},
        {state.nrdViewZTexture.Get(), RI_RESOURCE_STATE_UNDEFINED,
         RI_RESOURCE_STATE_CLEAR_STORAGE},
        {state.nrdDiffuseRadianceHitDistTexture.Get(),
         RI_RESOURCE_STATE_UNDEFINED, RI_RESOURCE_STATE_CLEAR_STORAGE},
        {state.nrdSpecularRadianceHitDistTexture.Get(),
         RI_RESOURCE_STATE_UNDEFINED, RI_RESOURCE_STATE_CLEAR_STORAGE},
        {state.nrdMotionVectorsTexture.Get(), RI_RESOURCE_STATE_UNDEFINED,
         RI_RESOURCE_STATE_CLEAR_STORAGE}};
    mpGraphics->primary.cmds[0].vk_d3d12_textureBarriers<9>(9, toGen);

    const float clr[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    for (uint32_t i = 0; i < 9; ++i)
      mpGraphics->primary.cmds[0].clearStorageImage(&mpGraphics->device, toGen[i].texture, clr);

    mpGraphics->primary.cmds[0].vk_d3d12_memoryBarrier(
        {RI_RESOURCE_STATE_CLEAR_STORAGE,
         RI_RESOURCE_STATE_SHADER_RESOURCE | RI_RESOURCE_STATE_STORAGE_WRITE,
         RI_STAGE_NONE, RI_STAGE_COMPUTE | RI_STAGE_RAY_TRACING});
    state.indirectLightingInit = true;
    state.indirectHistoryReset = false;
  } else {
    if (state.indirectHistoryReset) {
      // The engine-side textures hold no history any more — they are rewritten
      // every frame — so there is nothing here to clear. All temporal state
      // lives inside NRD, which discards it via CLEAR_AND_RESTART on the next
      // Denoise call.
      state.nrd->ResetHistory();
      state.indirectHistoryReset = false;
    }
    // Make last frame's writes visible to this frame's RT write / pack read.
    // All stay GENERAL.
    mpGraphics->primary.cmds[0].vk_d3d12_memoryBarrier(
        {RI_RESOURCE_STATE_STORAGE_WRITE | RI_RESOURCE_STATE_SHADER_RESOURCE,
         RI_RESOURCE_STATE_SHADER_RESOURCE | RI_RESOURCE_STATE_STORAGE_WRITE,
         RI_STAGE_COMPUTE | RI_STAGE_RAY_TRACING,
         RI_STAGE_COMPUTE | RI_STAGE_RAY_TRACING});
  }

  if (apWorld->GetTlas() != nullptr) {
    // ----------------------------------------------------------------
    // PathTracePass — per-pixel reference path tracer. Rooted at the primary
    // V-buffer hit; writes two indirect channels (albedo-demodulated diffuse +
    // undemodulated specular) and the surface key both denoise chains reject
    // on (viewZ/normal, plus GGX alpha and primary hit distance in the key extra). Needs this
    // frame's POM-corrected gPackedHitInfo, hence its position after
    // VBufferPomBary.
    // ----------------------------------------------------------------
    RIGpuScope _gsPathTrace(&mpGraphics->profiler, &mpGraphics->primary.cmds[0],
                            "PathTrace");
    VkRayTracingPipelineCreateInfoKHR ptCreate = {
        VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
    ptCreate.maxPipelineRayRecursionDepth = 1;
    const hash_t kPtHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    m_pathTrace.bindRayTracingPipeline(&mpGraphics->device, &mpGraphics->primary.cmds[0],
                                       kPtHash, "PathTracePass.rt", &ptCreate);
    m_pathTrace.bindBindlessDescriptorSet(
        &mpGraphics->primary.cmds[0], &mpGraphics->globalset->m_bindlessSet, 0,
        VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR);

    std::vector<RIProgram::DescriptorBinding> ptBnd;
    ptBnd.reserve(8);
    {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("gPerFrame");
      mpGraphics->UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
      ptBnd.push_back(b);
    }
    ptBnd.emplace_back(
        "gPackedHitInfo",
        RIDescriptor::storageImage(
            &mpGraphics->device, state.packedHitInfoView[mpGraphics->swapchainIndex].Get()));
    ptBnd.emplace_back("gRtAccel", RIDescriptor::accelerationStructure(
                                       &mpGraphics->device, apWorld->GetTlas()));
    // Two radiance channels: diffuse is albedo-demodulated (the composite
    // re-applies albedo), specular is left undemodulated.
    ptBnd.emplace_back("gIndirectDiffuse",
                       RIDescriptor::storageImage(
                           &mpGraphics->device, state.indirectRadianceView.Get()));
    ptBnd.emplace_back("gIndirectSpecular",
                       RIDescriptor::storageImage(
                           &mpGraphics->device, state.indirectSpecularView.Get()));
    ptBnd.emplace_back("gIndirectKeyOut",
                       RIDescriptor::storageImage(
                           &mpGraphics->device, state.indirectKeyView.Get()));
    // Key extra: primary-hit GGX alpha in .x, diffuse primary hit distance in .y,
    // specular primary hit distance in .z, and .w reserved.
    ptBnd.emplace_back("gIndirectKeyExtra",
                       RIDescriptor::storageImage(
                           &mpGraphics->device, state.indirectKeyExtraView.Get()));
    appendWorldLightFog(ptBnd, apWorld);
    m_pathTrace.bindDescriptors(&mpGraphics->device, &mpGraphics->primary.cmds[0],
                                mpGraphics->frameIndex, ptBnd.data(), ptBnd.size(),
                                VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR);
    m_pathTrace.traceRays(&mpGraphics->primary.cmds[0], kPtHash, renderWidth,
                          renderHeight, 1u);
  }
  // PathTracePass storage writes -> sampled read by the denoiser below.
  mpGraphics->primary.cmds[0].vk_d3d12_memoryBarrier(
      {RI_RESOURCE_STATE_STORAGE_WRITE, RI_RESOURCE_STATE_SHADER_RESOURCE,
       RI_STAGE_RAY_TRACING, RI_STAGE_COMPUTE});

  // ----------------------------------------------------------------------
  // Denoise — NrdPack repacks this frame's lighting into NRD's layouts, then
  // one REBLUR_DIFFUSE_SPECULAR instance filters both lobes. 1 spp is far too
  // noisy to composite raw. This runs even with no TLAS (the path tracer is
  // skipped, but the pack targets and NRD's history must still advance).
  //
  // The diffuse channel carries the ReSTIR DI direct term as well as the
  // indirect bounce: REBLUR splits by LOBE at the primary vertex, not by
  // direct/indirect, because its specular path needs roughness-driven
  // reprojection and virtual history.
  // ----------------------------------------------------------------------
  {
    VkComputePipelineCreateInfo ci = {
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);

    {
      // Repack the path tracer's engine-native key/radiance/velocity buffers
      // into the layouts required by NRD. The pack targets are read-only after
      // this pass, except for NRD's private IN_MV, which stabilization writes;
      // transition only the slot that was previously consumed by NRD.
      if (state.nrdInputInShaderResource) {
        RITextureBarrier toStorage[5] = {
            {state.nrdNormalRoughnessTexture.Get(),
             RI_RESOURCE_STATE_SHADER_RESOURCE,
             RI_RESOURCE_STATE_STORAGE_WRITE, RI_STAGE_COMPUTE, RI_STAGE_COMPUTE},
            {state.nrdViewZTexture.Get(), RI_RESOURCE_STATE_SHADER_RESOURCE,
             RI_RESOURCE_STATE_STORAGE_WRITE, RI_STAGE_COMPUTE, RI_STAGE_COMPUTE},
            {state.nrdDiffuseRadianceHitDistTexture.Get(),
             RI_RESOURCE_STATE_SHADER_RESOURCE,
             RI_RESOURCE_STATE_STORAGE_WRITE, RI_STAGE_COMPUTE, RI_STAGE_COMPUTE},
            {state.nrdSpecularRadianceHitDistTexture.Get(),
             RI_RESOURCE_STATE_SHADER_RESOURCE,
             RI_RESOURCE_STATE_STORAGE_WRITE, RI_STAGE_COMPUTE, RI_STAGE_COMPUTE}};
        toStorage[4] = {
            state.nrdMotionVectorsTexture.Get(),
            RI_RESOURCE_STATE_SHADER_RESOURCE | RI_RESOURCE_STATE_STORAGE_WRITE,
            RI_RESOURCE_STATE_STORAGE_WRITE, RI_STAGE_COMPUTE, RI_STAGE_COMPUTE};
        mpGraphics->primary.cmds[0].vk_d3d12_textureBarriers<5>(5, toStorage);
      }

      {
        RIGpuScope _gsNrdPack(&mpGraphics->profiler, &mpGraphics->primary.cmds[0],
                              "NRD.Pack");
        m_nrdPack.bindComputePipeline(&mpGraphics->device,
                                      &mpGraphics->primary.cmds[0], kHash,
                                      "NrdPack.cs", &ci);
        m_nrdPack.bindBindlessDescriptorSet(
            &mpGraphics->primary.cmds[0], &mpGraphics->globalset->m_bindlessSet,
            0, VK_PIPELINE_BIND_POINT_COMPUTE);

        std::vector<RIProgram::DescriptorBinding> nb;
        nb.reserve(12);
        {
          RIProgram::DescriptorBinding b;
          b.handle = DescriptorBindingID::Create("gPerFrame");
          mpGraphics->UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
          nb.push_back(b);
        }
        nb.emplace_back("gIndirectKey",
                        RIDescriptor::sampledImage(
                            &mpGraphics->device, state.indirectKeyView.Get(),
                            RI_RESOURCE_STATE_GENERAL));
        nb.emplace_back("gIndirectKeyExtra",
                        RIDescriptor::sampledImage(
                            &mpGraphics->device,
                            state.indirectKeyExtraView.Get(),
                            RI_RESOURCE_STATE_GENERAL));
        nb.emplace_back("gIndirectDiffuse",
                        RIDescriptor::sampledImage(
                            &mpGraphics->device,
                            state.indirectRadianceView.Get(),
                            RI_RESOURCE_STATE_GENERAL));
        nb.emplace_back("gIndirectSpecular",
                        RIDescriptor::sampledImage(
                            &mpGraphics->device,
                            state.indirectSpecularView.Get(),
                            RI_RESOURCE_STATE_GENERAL));
        nb.emplace_back("gVelocity",
                        RIDescriptor::sampledImage(
                            &mpGraphics->device,
                            state.velocityView[mpGraphics->swapchainIndex].Get(),
                            RI_RESOURCE_STATE_SHADER_RESOURCE));
        // ReSTIR DI's resolved direct irradiance. NrdPack sums it into the
        // diffuse channel so one REBLUR instance denoises direct + indirect
        // together; REBLUR splits by lobe, not by direct/indirect.
        nb.emplace_back("gDirectLighting",
                        RIDescriptor::sampledImage(
                            &mpGraphics->device, directResultView,
                            RI_RESOURCE_STATE_GENERAL));
        nb.emplace_back("gNrdNormalRoughness",
                        RIDescriptor::storageImage(
                            &mpGraphics->device,
                            state.nrdNormalRoughnessView.Get()));
        nb.emplace_back("gNrdViewZ",
                        RIDescriptor::storageImage(
                            &mpGraphics->device, state.nrdViewZView.Get()));
        nb.emplace_back("gNrdDiffuseRadianceHitDist",
                        RIDescriptor::storageImage(
                            &mpGraphics->device,
                            state.nrdDiffuseRadianceHitDistView.Get()));
        nb.emplace_back("gNrdSpecularRadianceHitDist",
                        RIDescriptor::storageImage(
                            &mpGraphics->device,
                            state.nrdSpecularRadianceHitDistView.Get()));
        nb.emplace_back("gNrdMotionVectors",
                        RIDescriptor::storageImage(
                            &mpGraphics->device,
                            state.nrdMotionVectorsView.Get()));

        m_nrdPack.bindDescriptors(&mpGraphics->device, &mpGraphics->primary.cmds[0],
                                  mpGraphics->frameIndex, nb.data(), nb.size(),
                                  VK_PIPELINE_BIND_POINT_COMPUTE);
        mpGraphics->primary.cmds[0].dispatch(
            &mpGraphics->device, (renderWidth + 15u) / 16u,
            (renderHeight + 15u) / 16u, 1u);
      }

      RITextureBarrier toSampled[5] = {
          {state.nrdNormalRoughnessTexture.Get(),
           RI_RESOURCE_STATE_STORAGE_WRITE, RI_RESOURCE_STATE_SHADER_RESOURCE,
           RI_STAGE_COMPUTE, RI_STAGE_COMPUTE},
          {state.nrdViewZTexture.Get(), RI_RESOURCE_STATE_STORAGE_WRITE,
           RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_COMPUTE, RI_STAGE_COMPUTE},
          {state.nrdDiffuseRadianceHitDistTexture.Get(),
           RI_RESOURCE_STATE_STORAGE_WRITE, RI_RESOURCE_STATE_SHADER_RESOURCE,
           RI_STAGE_COMPUTE, RI_STAGE_COMPUTE},
          {state.nrdSpecularRadianceHitDistTexture.Get(),
           RI_RESOURCE_STATE_STORAGE_WRITE, RI_RESOURCE_STATE_SHADER_RESOURCE,
           RI_STAGE_COMPUTE, RI_STAGE_COMPUTE},
          // IN_MV is sampled by most NRD dispatches and written by temporal
          // stabilization, so leave it in GENERAL with both accesses enabled.
          {state.nrdMotionVectorsTexture.Get(),
           RI_RESOURCE_STATE_STORAGE_WRITE,
           RI_RESOURCE_STATE_SHADER_RESOURCE | RI_RESOURCE_STATE_STORAGE_WRITE,
           RI_STAGE_COMPUTE, RI_STAGE_COMPUTE}};
      mpGraphics->primary.cmds[0].vk_d3d12_textureBarriers<5>(5, toSampled);
      state.nrdInputInShaderResource = true;

      NrdFrameData nrdFrame = {};
      // perFrame.prev* are the previous camera matrices copied from the
      // viewport state before it is advanced for the next frame.
      std::memcpy(nrdFrame.viewToClipMatrix, perFrame.projMat,
                  sizeof(nrdFrame.viewToClipMatrix));
      std::memcpy(nrdFrame.viewToClipMatrixPrev, perFrame.prevProjMat,
                  sizeof(nrdFrame.viewToClipMatrixPrev));
      std::memcpy(nrdFrame.worldToViewMatrix, perFrame.viewMat,
                  sizeof(nrdFrame.worldToViewMatrix));
      std::memcpy(nrdFrame.worldToViewMatrixPrev, perFrame.prevViewMat,
                  sizeof(nrdFrame.worldToViewMatrixPrev));
      nrdFrame.frameIndex = perFrame.totalFrames;
      // Past the far plane a pixel is sky; NrdPack already writes NRD_INF into
      // viewZ there, and this keeps the two consistent.
      nrdFrame.denoisingRange = apFrustum->GetFarPlane();
      nrdFrame.timeDeltaMs = afFrameTime * 1000.0f;

      NrdDenoiseInputs nrdInputs = {};
      nrdInputs.normalRoughness = state.nrdNormalRoughnessView.Get();
      nrdInputs.viewZ = state.nrdViewZView.Get();
      nrdInputs.motionVectors =
          state.nrdMotionVectorsView.Get();
      nrdInputs.diffuseRadianceHitDistance =
          state.nrdDiffuseRadianceHitDistView.Get();
      nrdInputs.specularRadianceHitDistance =
          state.nrdSpecularRadianceHitDistView.Get();

      NrdDenoiseOutputs nrdOutputs = {};
      {
        RIGpuScope _gsNrdDenoise(&mpGraphics->profiler,
                                 &mpGraphics->primary.cmds[0], "NRD.Denoise");
        nrdOutputs = state.nrd->Denoise(&mpGraphics->primary.cmds[0], nrdFrame,
                                   nrdInputs);
      }
      indirectResultView = nrdOutputs.diffuseRadianceHitDistance;
      indirectSpecularResultView = nrdOutputs.specularRadianceHitDistance;
    }
  }

  // --------------------------------------------------------------------
  // Composite — compute pass. Reads REBLUR's two denoised outputs
  // (gIndirectLighting = the diffuse channel, carrying direct + indirect;
  // gIndirectSpecular = the GGX lobe) + gPackedHitInfo / TLAS / gPerFrame, and
  // writes the composited color into the viewport render target. The forward passes draw on top of it; the
  // tail crop-blits it into the viewport backbuffer, which Scene.cpp's
  // post-effect chain + swapchain tail blit consume.
  // --------------------------------------------------------------------

  // Composite + forward passes render into the OVERSCAN render target (guard
  // band, single image — the main draw never ping-pongs); cropped 1:1 center
  // into the authored-size viewport backbuffer at the end of Draw.

  // Barrier: make the lighting results visible to the COMPUTE composite, and
  // put the render target into GENERAL for the storage write. (The raster
  // visibility buffer and the VBufferPomBary output were already barriered to
  // the COMPUTE stage upstream.)
  {
    // SHADER_RESOURCE for the two denoised lighting image
    // samples; STORAGE_READ for the SSBOs the composite walks (light grid,
    // object / material pools) written earlier this frame.
    RIMemoryBarrier mem = {RI_RESOURCE_STATE_STORAGE_WRITE,
                           RI_RESOURCE_STATE_SHADER_RESOURCE |
                               RI_RESOURCE_STATE_STORAGE_READ,
                           RI_STAGE_COMPUTE, RI_STAGE_COMPUTE};

    RITextureBarrier imageBarriers[1] = {
        // Pogo attach -> GENERAL for the compute storage write. Discard prior
        // contents (UNDEFINED): the dispatch writes every pixel, matching the
        // old fragment pass's LOAD_OP_DONT_CARE. This also covers the
        // first-frame init for the attach half.
        {state.renderTarget[mpGraphics->swapchainIndex].Get(),
         RI_RESOURCE_STATE_UNDEFINED, RI_RESOURCE_STATE_STORAGE_WRITE,
         RI_STAGE_NONE, RI_STAGE_COMPUTE}};

    mpGraphics->primary.cmds[0].vk_d3d12_resourceBarrier<1, 0, 1>(1, &mem, 0, NULL, 1,
                                                         imageBarriers);
  }

  // Depth flip shared by the decal pre-pass (below) and the particle /
  // translucent passes further down: depth arrives in SHADER_READ_ONLY from
  // the gbuffer barrier and flips once to DEPTH_READ_ONLY for any depth-tested
  // pass.
  bool depthFlippedForReadOnly = false;
  auto flipDepthToReadOnly = [&]() {
    if (depthFlippedForReadOnly)
      return;
    mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(
        {state.depthTextures[mpGraphics->swapchainIndex].Get(),
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
  // (the V-buffer renderer has no albedo G-buffer to write them into the way
  // the original deferred engine did). These decals carry no alpha —
  // transparency is the blend-mode identity (white for Mul/MulX2, black for
  // Add) — so a single premultiplied-over buffer turned their no-op areas
  // opaque. Two accumulators reproduce the real blend: Mul/MulX2 multiply into
  // decalMul (cleared white), Add adds into decalAdd (cleared black).
  // Depth-tested ≤ against the gbuffer depth (no write); both accumulators are
  // CLEARED every frame so the composite never samples stale contents.
  // (Linear×linear multiply equals the legacy gamma multiply via the power law;
  // Add is a close linear approximation.)
  // --------------------------------------------------------------------
  {
    RIGpuScope _gsDecal(&mpGraphics->profiler, &mpGraphics->primary.cmds[0], "Decal");
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
    // family's decals against the gbuffer depth (read-only), and blend each
    // with its material blend mode. Always run (even empty) so the texture is
    // cleared and ends in SHADER_RESOURCE for the composite.
    auto renderDecalAccumulator = [&](RITexture *tex, const RITextureView &view,
                                      const float clearRGBA[4],
                                      const std::vector<iRenderable *> &list) {
      // Dst stage hint deliberately NONE: RENDER_TARGET must sync against
      // COLOR_ATTACHMENT_OUTPUT (derived from the state), not FRAGMENT_SHADER —
      // an explicit FRAGMENT hint here pairs COLOR_ATTACHMENT_WRITE access with
      // a stage that doesn't support it (VUID-VkImageMemoryBarrier2-dstAccessMask-03911).
      mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(
          {tex, RI_RESOURCE_STATE_UNDEFINED, RI_RESOURCE_STATE_RENDER_TARGET,
           RI_STAGE_NONE, RI_STAGE_NONE});

      RIRenderingAttachment color = {};
      color.view = view;
      color.loadOp = RI_ATTACHMENT_LOAD_OP_CLEAR;
      color.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
      color.clearValue.color[0] = clearRGBA[0];
      color.clearValue.color[1] = clearRGBA[1];
      color.clearValue.color[2] = clearRGBA[2];
      color.clearValue.color[3] = clearRGBA[3];

      RIRenderingAttachment depth = {};
      depth.view = *state.depthView[mpGraphics->swapchainIndex];
      depth.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
      depth.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
      depth.readOnly = true;

      RIBeginRenderingDesc beginDesc = {};
      beginDesc.renderArea.width = (int16_t)renderWidth;
      beginDesc.renderArea.height = (int16_t)renderHeight;
      beginDesc.colorCount = 1;
      beginDesc.colors = &color;
      beginDesc.depthStencil = &depth;
      mpGraphics->primary.cmds[0].vk_d3d12_beginRendering(&mpGraphics->device, beginDesc);

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
      mpGraphics->primary.cmds[0].setViewport(&mpGraphics->device, vp);
      mpGraphics->primary.cmds[0].setScissor(&mpGraphics->device, sc);

      if (!list.empty()) {
        m_decal.bindBindlessDescriptorSet(&mpGraphics->primary.cmds[0],
                                          &mpGraphics->globalset->m_bindlessSet, 0);
        {
          // VS reads gPerFrame (view/proj) + gSceneObjects; FS emits the linear
          // decal colour. No fog/light buffers — the composite lights and fogs.
          RIProgram::DescriptorBinding b;
          b.handle = DescriptorBindingID::Create("gPerFrame");
          mpGraphics->UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
          m_decal.bindDescriptors(&mpGraphics->device, &mpGraphics->primary.cmds[0],
                                  mpGraphics->frameIndex, &b, 1);
        }

        for (iRenderable *pObj : list) {
          cVertexBuffer *pVB = pObj->GetVertexBuffer();
          cMaterial *pMat = pObj->GetMaterial();
          const int indexCount = pVB->GetIndexNum();

          uint32_t materialId =
              mpGraphics->globalset->submitMaterial(cntx, pMat, (uint32_t)mpGraphics->frameIndex)
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

          const uint32_t slot = mpGraphics->globalset->submitObject(
              pObj->GetUniqueCookie(), (uint32_t)mpGraphics->frameIndex,
              static_cast<cVertexBuffer *>(pVB), d);
          if (slot == UINT32_MAX) {
            Warning("bindless pool exhausted (decal)");
            continue;
          }

          uint32_t vtxMask = 0;
          if (!detail::BindVertexStreams(&mpGraphics->primary.cmds[0], pVB, "decal",
                                         &vtxMask))
            continue;

          DecalPipelineDesc pipelineDesc(
              cGraphics::PogoColorFormat, cGraphics::DepthFormat,
              decalBlend(pMat->GetBlendMode()), vtxMask);
          m_decal.bindPipeline(&mpGraphics->device, &mpGraphics->primary.cmds[0],
                               pipelineDesc.hash, "Decal",
                               &pipelineDesc.createInfo);

          mpGraphics->primary.cmds[0].drawIndexed(&mpGraphics->device, (uint32_t)indexCount, 1u,
                                         0u, 0, slot);
        }
      }

      mpGraphics->primary.cmds[0].vk_d3d12_endRendering(&mpGraphics->device);

      // COLOR_ATTACHMENT -> SHADER_RESOURCE for the composite read.
      mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(
          {tex, RI_RESOURCE_STATE_RENDER_TARGET,
           RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT,
           RI_STAGE_COMPUTE});
    };

    const float kIdentityMul[4] = {1.0f, 1.0f, 1.0f, 1.0f}; // ×1
    const float kIdentityAdd[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // +0
    renderDecalAccumulator(state.decalMulTexture[mpGraphics->swapchainIndex].Get(),
                           *state.decalMulView[mpGraphics->swapchainIndex], kIdentityMul,
                           mulDecals);
    renderDecalAccumulator(state.decalAddTexture[mpGraphics->swapchainIndex].Get(),
                           *state.decalAddView[mpGraphics->swapchainIndex], kIdentityAdd,
                           addDecals);
  }

  // Composite compute pass — one thread per pixel writes the composite into the
  // pogo attach bound as gOutput (storage image, GENERAL).
  {
    VkComputePipelineCreateInfo compositeCreate = {
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    RIGpuScope _gsComposite(&mpGraphics->profiler, &mpGraphics->primary.cmds[0],
                                "Composite");
    m_composite.bindComputePipeline(&mpGraphics->device, &mpGraphics->primary.cmds[0], kHash,
                                        "Composite.cs", &compositeCreate);
    m_composite.bindBindlessDescriptorSet(&mpGraphics->primary.cmds[0],
                                              &mpGraphics->globalset->m_bindlessSet, 0,
                                              VK_PIPELINE_BIND_POINT_COMPUTE);

    std::vector<RIProgram::DescriptorBinding> bnd;
    bnd.reserve(11);
    {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("gPerFrame");
      mpGraphics->UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
      bnd.push_back(b);
    }
    bnd.emplace_back(
        "gPackedHitInfo",
        RIDescriptor::storageImage(
            &mpGraphics->device, state.packedHitInfoView[mpGraphics->swapchainIndex].Get()));
    // optional: the TLAS is null until the first build, and this call site
    // isn't guarded on it (the composite shader may or may not reflect it).
    bnd.emplace_back("gRtAccel",
                     RIDescriptor::accelerationStructure(&mpGraphics->device,
                                                         apWorld->GetTlas()),
                     0, true);
    // gIndirectLighting — REBLUR's OUT_DIFF_RADIANCE_HITDIST: the denoised,
    // albedo-demodulated diffuse channel, carrying the ReSTIR DI direct term
    // summed with the path tracer's indirect bounce (NrdPack does that sum).
    // gIndirectSpecular is OUT_SPEC_RADIANCE_HITDIST, undemodulated and added
    // on top as-is. Both are YCoCg-packed; the shader unpacks them. NRD leaves
    // its outputs in GENERAL. There is no separate direct input any more.
    bnd.emplace_back("gIndirectLighting",
                     RIDescriptor::sampledImage(&mpGraphics->device,
                                                indirectResultView,
                                                RI_RESOURCE_STATE_GENERAL));
    bnd.emplace_back("gIndirectSpecular",
                     RIDescriptor::sampledImage(&mpGraphics->device,
                                                indirectSpecularResultView,
                                                RI_RESOURCE_STATE_GENERAL));
    // gOutput — the viewport render target bound as a storage image (GENERAL).
    bnd.emplace_back(
        "gOutput",
        RIDescriptor::storageImage(
            &mpGraphics->device, state.renderTargetView[mpGraphics->swapchainIndex].Get()));

    // Decal accumulators from the pre-pass above (already in SHADER_RESOURCE):
    // the composite applies albedo = albedo*gDecalMul + gDecalAdd before
    // lighting.
    bnd.emplace_back(
        "gDecalMul",
        RIDescriptor::sampledImage(
            &mpGraphics->device, state.decalMulView[mpGraphics->swapchainIndex].Get()));
    bnd.emplace_back(
        "gDecalAdd",
        RIDescriptor::sampledImage(
            &mpGraphics->device, state.decalAddView[mpGraphics->swapchainIndex].Get()));

    // Per-world static decal buffers (set kWorldDecalSet), baked once by
    // cWorld::Compile. RIProgram reflects them as set 2 and binds a rotated
    // set. Worlds compiled under the hybrid renderer always have valid (>=1
    // element) buffers, so set 2 is never left unbound.
    if (RIBuffer *decalBuf = apWorld->GetDecalBuffer()) {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("gDecals");
      b.descriptor = RIDescriptor::storageBuffer(
          &mpGraphics->device, decalBuf, 0,
          std::max<size_t>(apWorld->GetDecalCount(), 1) * sizeof(GpuDecal));
      bnd.push_back(b);
    } else {
      // optional: absent until the world is compiled.
      bnd.emplace_back("gDecals", RIDescriptor(), 0, true);
    }
    if (RIBuffer *idxBuf = apWorld->GetDecalObjectIndexBuffer()) {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("gObjectDecalIndices");
      b.descriptor = RIDescriptor::storageBuffer(
          &mpGraphics->device, idxBuf, 0,
          std::max<size_t>(apWorld->GetDecalObjectIndices().size(), 1) *
              sizeof(uint32_t));
      bnd.push_back(b);
    } else {
      // optional: absent until the world is compiled.
      bnd.emplace_back("gObjectDecalIndices", RIDescriptor(), 0, true);
    }

    appendWorldLightFog(bnd, apWorld);
    m_composite.bindDescriptors(&mpGraphics->device, &mpGraphics->primary.cmds[0],
                                    mpGraphics->frameIndex, bnd.data(), bnd.size(),
                                    VK_PIPELINE_BIND_POINT_COMPUTE);

    static_assert(sizeof(OverlayPushConstants) == 4);
    const OverlayPushConstants push{m_overlayMode};
    vkCmdPushConstants(mpGraphics->primary.cmds[0].vk.cmd,
                       m_composite.getPipelineLayout(),
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);

    mpGraphics->primary.cmds[0].dispatch(&mpGraphics->device, (renderWidth + 15u) / 16u,
                                (renderHeight + 15u) / 16u, 1u);
  }

  // Toggle the ReSTIR key/reservoir ping-pong: this frame's writes become next
  // frame's history. Nothing else ping-pongs — NRD keeps its own history.
  state.directLightingIndex ^= 1u;

  // Render target: GENERAL (compute write) -> COLOR_ATTACHMENT_OPTIMAL so the
  // downstream raster passes find the layout they expect.
  {
    mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(
        {state.renderTarget[mpGraphics->swapchainIndex].Get(),
         RI_RESOURCE_STATE_STORAGE_WRITE, RI_RESOURCE_STATE_RENDER_TARGET,
         RI_STAGE_COMPUTE});
  }

  // Single render target — no toggle: the main draw never ping-pongs. The
  // downstream raster passes flip it COLOR -> SHADER_READ as they go (each
  // pass transitions in before drawing and back out after); the tail blits
  // it into the viewport backbuffer, where the post-effect chain + tail blit
  // in cScene::Render consume it.
  {
    mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(RI_PogoShaderBarrier(
        state.renderTarget[mpGraphics->swapchainIndex].Get(), /*initial=*/false));
  }

  // (depthFlippedForReadOnly + flipDepthToReadOnly are defined above, before
  // the decal pre-pass, and shared with the particle / translucent passes
  // below.)

  // (The Type="Decal" mesh decals are rasterized in the decal pre-pass above,
  // before the composite, into the decal-overlay target — not here. The
  // composite folds that overlay onto the base albedo so they are lit +
  // fogged.)

  // --------------------------------------------------------------------
  // Water pass — raster the water surface over the background the GI
  // composite already shaded. That background is NOT refracted: no refraction
  // pass runs, so the primary hit under the water is the plain rasterized
  // front surface. Two draws per mesh: MUL (tint + refraction
  // exposure) then ADD (inline-RT lit reflection × Fresnel). The pair composes
  // as a nested over, so this pass is order-dependent: water meshes are sorted
  // back-to-front here explicitly instead of inheriting the shared translucent
  // sort. The per-object MUL-then-ADD interleaving is deliberate and required —
  // hoisting all the MULs ahead of all the ADDs would stop a far surface's
  // reflection and fog being attenuated through the near surface in front of
  // it. Reuses the translucent 5-stream layout + TranslucentMeshPipelineDesc
  // state (depth ≤, no write); the m_water program supplies the shaders
  // (Water.vert/frag).
  // Pogo-read-half barriers as the other translucent sub-passes.
  // --------------------------------------------------------------------
  {
    RIGpuScope _gsWater(&mpGraphics->profiler, &mpGraphics->primary.cmds[0], "Water");
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

    // View space is -Z-forward here (the water fragment shader negates it), so
    // ascending view-space Z is farthest first, i.e. back-to-front. Stable so
    // meshes at equal depth keep the render list's relative order.
    std::stable_sort(waters.begin(), waters.end(),
                     [](const iRenderable *a, const iRenderable *b) {
                       return a->GetViewSpaceZ() < b->GetViewSpaceZ();
                     });

    if (!waters.empty()) {
      flipDepthToReadOnly();

      VkImage pogoReadImage = state.renderTarget[mpGraphics->swapchainIndex]->vk.image;
      VkImageView pogoReadView =
          state.renderTargetView[mpGraphics->swapchainIndex]->vk.image;

      mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(RI_PogoAttachmentBarrier(
          state.renderTarget[mpGraphics->swapchainIndex].Get(), /*initial=*/false));

      RITextureView colorView = {};
      colorView.vk.image = pogoReadView;
      RIRenderingAttachment color = {};
      color.view = colorView;
      color.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
      color.storeOp = RI_ATTACHMENT_STORE_OP_STORE;

      RIRenderingAttachment depth = {};
      depth.view = *state.depthView[mpGraphics->swapchainIndex];
      depth.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
      depth.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
      depth.readOnly = true;

      RIBeginRenderingDesc beginDesc = {};
      beginDesc.renderArea.width = (int16_t)renderWidth;
      beginDesc.renderArea.height = (int16_t)renderHeight;
      beginDesc.colorCount = 1;
      beginDesc.colors = &color;
      beginDesc.depthStencil = &depth;
      mpGraphics->primary.cmds[0].vk_d3d12_beginRendering(&mpGraphics->device, beginDesc);

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
      mpGraphics->primary.cmds[0].setViewport(&mpGraphics->device, vp);
      mpGraphics->primary.cmds[0].setScissor(&mpGraphics->device, sc);

      m_water.bindBindlessDescriptorSet(&mpGraphics->primary.cmds[0],
                                        &mpGraphics->globalset->m_bindlessSet, 0);
      {
        // set 1: gPerFrame + gRtAccel (binding 36) + the world light/fog
        // buffers. The water frag does inline RayQuery reflection
        // (traceReflectionHit) and then re-traces one indirect bounce at the
        // reflection hit (traceIndirectAtHit), shading both with NEE
        // (evalAnalyticLight -> light grid + shadow rays), so it needs the TLAS
        // AND the lights — the raster pipeline doesn't get either for free like
        // the compute/RT passes.
        std::vector<RIProgram::DescriptorBinding> wbnd;
        {
          RIProgram::DescriptorBinding b;
          b.handle = DescriptorBindingID::Create("gPerFrame");
          mpGraphics->UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
          wbnd.push_back(b);
        }
        // optional: the TLAS is null until the first build.
        wbnd.emplace_back("gRtAccel",
                          RIDescriptor::accelerationStructure(
                              &mpGraphics->device, apWorld->GetTlas()),
                          0, true);
        appendWorldLightFog(wbnd, apWorld);
        m_water.bindDescriptors(&mpGraphics->device, &mpGraphics->primary.cmds[0], mpGraphics->frameIndex,
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

        auto mat =
            mpGraphics->globalset->submitMaterial(cntx, pMat, (uint32_t)mpGraphics->frameIndex);
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

        const uint32_t slot = mpGraphics->globalset->submitObject(
            pObj->GetUniqueCookie(), (uint32_t)mpGraphics->frameIndex,
            static_cast<cVertexBuffer *>(pVB), d);
        if (slot == UINT32_MAX) {
          Warning("bindless pool exhausted (water)");
          continue;
        }

        uint32_t vtxMask = 0;
        if (!detail::BindVertexStreams(&mpGraphics->primary.cmds[0], pVB, "water",
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
          TranslucentMeshPipelineDesc pd(cGraphics::PogoColorFormat,
                                         cGraphics::DepthFormat, modes[pass],
                                         vtxMask);
          const hash_t waterHash = hash_u32(pd.hash, 0x57415445u /*'WATE'*/);
          m_water.bindPipeline(&mpGraphics->device, &mpGraphics->primary.cmds[0], waterHash,
                               "Water", &pd.createInfo);
          WaterPush push = {pass, 0u, 0u, 0u};
          mpGraphics->primary.cmds[0].vk_d3d12_setPushConstants(&mpGraphics->device, m_water, 0,
                                                       sizeof(push), &push);
          mpGraphics->primary.cmds[0].drawIndexed(&mpGraphics->device, (uint32_t)indexCount, 1u,
                                         0u, 0, slot);
        }
      }

      mpGraphics->primary.cmds[0].vk_d3d12_endRendering(&mpGraphics->device);

      {
        mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(RI_PogoShaderBarrier(
            state.renderTarget[mpGraphics->swapchainIndex].Get(), /*initial=*/false));
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
  //   - Interface<cGraphics>::Get()->globalset->m_objectSlots / m_objectBuffer (per-renderable OBJECT
  //   slot)
  //   - m_opaque*Handles (BDA fan-out: particles/meshes overload
  //                       position/uv0/color/index with their own VB addresses)
  //   - Interface<cGraphics>::Get()->globalset->m_materialBindless / m_materialBuffer (material slot)
  //
  // Sync: (a) swapchain stays COLOR_ATTACHMENT_OPTIMAL from the composite
  //           (load to preserve it); (b) flipDepthToReadOnly() moves depth back
  //           to DEPTH_READ_ONLY_OPTIMAL (shared with the decal pass).
  // --------------------------------------------------------------------
  {
    // Collect particle emitters from the translucent list once so we can
    // skip the whole pass (and its barriers/begin-rendering) when empty.
    RIGpuScope _gsParticle(&mpGraphics->profiler, &mpGraphics->primary.cmds[0], "Particle");
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
      VkImage pogoReadImage = state.renderTarget[mpGraphics->swapchainIndex]->vk.image;
      VkImageView pogoReadView =
          state.renderTargetView[mpGraphics->swapchainIndex]->vk.image;
      const RI_Format_e particleTargetFormat = cGraphics::PogoColorFormat;
      {
        mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(RI_PogoAttachmentBarrier(
            state.renderTarget[mpGraphics->swapchainIndex].Get(), /*initial=*/false));
      }

      RITextureView colorView = {};
      colorView.vk.image = pogoReadView;
      RIRenderingAttachment color = {};
      color.view = colorView;
      color.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
      color.storeOp = RI_ATTACHMENT_STORE_OP_STORE;

      RIRenderingAttachment depth = {};
      depth.view = *state.depthView[mpGraphics->swapchainIndex];
      depth.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
      depth.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
      depth.readOnly = true;

      RIBeginRenderingDesc beginDesc = {};
      beginDesc.renderArea.width = (int16_t)renderWidth;
      beginDesc.renderArea.height = (int16_t)renderHeight;
      beginDesc.colorCount = 1;
      beginDesc.colors = &color;
      beginDesc.depthStencil = &depth;
      mpGraphics->primary.cmds[0].vk_d3d12_beginRendering(&mpGraphics->device, beginDesc);

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
      mpGraphics->primary.cmds[0].setViewport(&mpGraphics->device, vp);
      mpGraphics->primary.cmds[0].setScissor(&mpGraphics->device, sc);

      m_particle.bindBindlessDescriptorSet(&mpGraphics->primary.cmds[0],
                                           &mpGraphics->globalset->m_bindlessSet, 0);

      std::vector<RIProgram::DescriptorBinding> particleBindings;
      particleBindings.reserve(2);
      {
        RIProgram::DescriptorBinding b;
        b.handle = DescriptorBindingID::Create("gPerFrame");
        mpGraphics->UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
        particleBindings.push_back(b);
      }
      // Soft particles: scene depth (opaque geometry) for the per-pixel fade.
      // Runs after flipDepthToReadOnly() above, so the image is already in
      // DEPTH_READ_ONLY_OPTIMAL — the same layout this sampled descriptor
      // declares, and the depth attachment is read-only, so the feedback loop
      // is legal with no extra barrier.
      particleBindings.emplace_back(
          "gSceneDepth",
          RIDescriptor::sampledImage(
              &mpGraphics->device, state.depthSampleView[mpGraphics->swapchainIndex].Get(),
              RI_RESOURCE_STATE_DEPTH_READ));
      appendWorldLightFog(particleBindings, apWorld);
      m_particle.bindDescriptors(&mpGraphics->device, &mpGraphics->primary.cmds[0], mpGraphics->frameIndex,
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
        // frame's camera-facing quads into the shared Interface<cGraphics>::Get()->translucentVtx/Idx
        // segments — same single producer as the wireframe/simple panes.
        auto geom = pEmitter->BuildScratchGeometry(apFrustum, afFrameTime,
                                                   /*withUv=*/true);
        if (!geom.valid)
          continue;
        const int indexCount = (int)geom.indexCount;

        uint32_t materialId =
            mpGraphics->globalset->submitMaterial(cntx, pMat, (uint32_t)mpGraphics->frameIndex)
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
        // scratch segments (base address + byte offset); normal/tangent are
        // never read for a particle slot, so 0. submitObject folds these into
        // the payload — refreshed every frame since the scratch offsets change.
        {
          const uint64_t vtxBase =
              mpGraphics->translucentVtxBuffer->GetDeviceHandle(&mpGraphics->device);
          d.streamHandles.pos = vtxBase + geom.posByteOffset;
          d.streamHandles.color = vtxBase + geom.colByteOffset;
          d.streamHandles.uv0 = vtxBase + geom.uvByteOffset;
          d.streamHandles.index =
              mpGraphics->translucentIdxBuffer->GetDeviceHandle(&mpGraphics->device) +
              geom.idxByteOffset;
          d.streamHandles.set = true;
        }

        // Particles share the object-slot pool with opaque solids; the payload
        // submit also bumps the slot generation when the slot is (re)assigned —
        // so a consumer still anchored to the slot's previous opaque occupant
        // self-invalidates before it dereferences this slot's smaller streams.
        const uint32_t slot = mpGraphics->globalset->submitObject(
            pEmitter->GetUniqueCookie(), (uint32_t)mpGraphics->frameIndex, nullptr, d,
            kSubmitData);
        if (slot == UINT32_MAX) {
          Warning("bindless pool exhausted (particle)");
          continue;
        }

        const ParticlePipelineDesc::BlendMode mode =
            remapBlend(pMat->GetBlendMode());
        ParticlePipelineDesc pipelineDesc(particleTargetFormat,
                                          cGraphics::DepthFormat, mode);
        m_particle.bindPipeline(&mpGraphics->device, &mpGraphics->primary.cmds[0],
                                pipelineDesc.hash, "Particle",
                                &pipelineDesc.createInfo);

        // Fog (world + per-area) is applied per-pixel in Particle.frag.slang
        // by walking gFogAreas. sceneAlpha is now the unmodified per-object
        // scalar (1.0 by default) — kept in the push block for parity with the
        // mesh path and any future per-object alpha gates.
        const float sceneAlpha = 1.0f;
        PushBlock push = {(uint32_t)mode, sceneAlpha};
        mpGraphics->primary.cmds[0].vk_d3d12_setPushConstants(&mpGraphics->device, m_particle, 0,
                                                     sizeof(push), &push);

        mpGraphics->primary.cmds[0].draw(&mpGraphics->device, (uint32_t)indexCount, 1u, 0u, slot);
      }

      mpGraphics->primary.cmds[0].vk_d3d12_endRendering(&mpGraphics->device);

      // pogo "read" half back to SHADER_READ_ONLY so the tail blit can sample
      // it.
      {
        mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(RI_PogoShaderBarrier(
            state.renderTarget[mpGraphics->swapchainIndex].Get(), /*initial=*/false));
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
  // Refraction is currently NOT implemented: the legacy screen-copy
  // bend-behind distortion was dropped, and the RT V-buffer pass meant to
  // replace it (bending the primary ray at refractive surfaces) was retired
  // before it ever ran. So refractive translucents just alpha-blend over the
  // unrefracted composite. Water is filtered out of the mesh collection
  // below; it has its own raster pass (m_water) over that same unrefracted
  // background.
  // --------------------------------------------------------------------
  {
    RIGpuScope _gsTranslucent(&mpGraphics->profiler, &mpGraphics->primary.cmds[0],
                              "TranslucentMesh");
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
      // standard blend pipeline (the background it blends over is the
      // unrefracted composite — no refraction pass runs); HasWorldReflection
      // is harmless (the legacy-only planar reflection buffer is unused).
      // Water is skipped here because it has its own raster sub-pass
      // (m_water, above) with the MUL + ADD draw pair.
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

      VkImage pogoReadImage = state.renderTarget[mpGraphics->swapchainIndex]->vk.image;
      VkImageView pogoReadView =
          state.renderTargetView[mpGraphics->swapchainIndex]->vk.image;
      const RI_Format_e meshTargetFormat = cGraphics::PogoColorFormat;

      // SHADER_READ_ONLY → COLOR_ATTACHMENT_OPTIMAL. If the particle pass
      // ran above, that block left the pogo half in SHADER_READ_ONLY (for
      // a tail blit that never got to run); if it didn't, the visibility
      // composite + post-effect chain also left it in SHADER_READ_ONLY. The
      // barrier helper handles either source state.
      {
        mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(RI_PogoAttachmentBarrier(
            state.renderTarget[mpGraphics->swapchainIndex].Get(), /*initial=*/false));
      }

      RITextureView colorView = {};
      colorView.vk.image = pogoReadView;
      RIRenderingAttachment color = {};
      color.view = colorView;
      color.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
      color.storeOp = RI_ATTACHMENT_STORE_OP_STORE;

      RIRenderingAttachment depth = {};
      depth.view = *state.depthView[mpGraphics->swapchainIndex];
      depth.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
      depth.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
      depth.readOnly = true;

      RIBeginRenderingDesc beginDesc = {};
      beginDesc.renderArea.width = (int16_t)renderWidth;
      beginDesc.renderArea.height = (int16_t)renderHeight;
      beginDesc.colorCount = 1;
      beginDesc.colors = &color;
      beginDesc.depthStencil = &depth;
      mpGraphics->primary.cmds[0].vk_d3d12_beginRendering(&mpGraphics->device, beginDesc);

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
      mpGraphics->primary.cmds[0].setViewport(&mpGraphics->device, vp);
      mpGraphics->primary.cmds[0].setScissor(&mpGraphics->device, sc);

      m_translucentMesh.bindBindlessDescriptorSet(
          &mpGraphics->primary.cmds[0], &mpGraphics->globalset->m_bindlessSet, 0);

      std::vector<RIProgram::DescriptorBinding> meshBindings;
      meshBindings.reserve(1);
      {
        RIProgram::DescriptorBinding b;
        b.handle = DescriptorBindingID::Create("gPerFrame");
        mpGraphics->UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
        meshBindings.push_back(b);
      }
      appendWorldLightFog(meshBindings, apWorld);
      m_translucentMesh.bindDescriptors(&mpGraphics->device, &mpGraphics->primary.cmds[0],
                                        mpGraphics->frameIndex, meshBindings.data(),
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
            mpGraphics->globalset->submitMaterial(cntx, pMat, (uint32_t)mpGraphics->frameIndex)
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
        // bumps the slot generation on (re)assignment so a consumer anchored to a
        // previous opaque occupant self-invalidates before dereferencing the
        // wrong VB/IB.
        const uint32_t slot = mpGraphics->globalset->submitObject(
            pObj->GetUniqueCookie(), (uint32_t)mpGraphics->frameIndex,
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
        if (!detail::BindVertexStreams(&mpGraphics->primary.cmds[0], pVB, "translucent",
                                       &vtxMask))
          continue;

        const TranslucentMeshPipelineDesc::BlendMode mode =
            remapBlend(pMat->GetBlendMode());
        TranslucentMeshPipelineDesc pipelineDesc(
            meshTargetFormat, cGraphics::DepthFormat, mode, vtxMask);
        m_translucentMesh.bindPipeline(&mpGraphics->device, &mpGraphics->primary.cmds[0],
                                       pipelineDesc.hash, "TranslucentMesh",
                                       &pipelineDesc.createInfo);

        // Fog (world + per-area) is applied per-pixel in Translucent.frag.slang
        // by walking gFogAreas. sceneAlpha stays 1.0 for the no-extra-alpha
        // common path.
        const float sceneAlpha = 1.0f;
        PushBlock push = {(uint32_t)mode, sceneAlpha, 0u, 0u};
        mpGraphics->primary.cmds[0].vk_d3d12_setPushConstants(
            &mpGraphics->device, m_translucentMesh, 0, sizeof(push), &push);

        mpGraphics->primary.cmds[0].drawIndexed(&mpGraphics->device, (uint32_t)indexCount, 1u, 0u,
                                       0, slot);

        // Second draw for the cube-map Fresnel + rim contribution.
        // Reference gates this on `cubeMap && !isRefraction`
        // (RendererDeferred.cpp:4660) because its refraction path consumed
        // the screen copy the cube-map draw would have overwritten. Nothing
        // here refracts (no refraction pass exists), so there is no such
        // conflict and the cube-map second draw stays gated only on whether
        // the material carries a cube map.
        if (pMat->GetImage(eMaterialTexture_CubeMap)) {
          TranslucentMeshPipelineDesc addDesc(
              meshTargetFormat, cGraphics::DepthFormat,
              TranslucentMeshPipelineDesc::BLEND_ADD, vtxMask);
          m_translucentMesh.bindPipeline(&mpGraphics->device, &mpGraphics->primary.cmds[0],
                                         addDesc.hash, "TranslucentMeshIllum",
                                         &addDesc.createInfo);
          PushBlock pushIllum = {
              (uint32_t)TranslucentMeshPipelineDesc::BLEND_ADD, sceneAlpha,
              kTransOptUseIllumination, 0u};
          mpGraphics->primary.cmds[0].vk_d3d12_setPushConstants(
              &mpGraphics->device, m_translucentMesh, 0, sizeof(pushIllum), &pushIllum);
          // Vertex / index buffers stay bound from the main draw above —
          // same renderable, just a second pipeline + push-constant set.
          mpGraphics->primary.cmds[0].drawIndexed(&mpGraphics->device, (uint32_t)indexCount, 1u,
                                         0u, 0, slot);
        }
      }

      mpGraphics->primary.cmds[0].vk_d3d12_endRendering(&mpGraphics->device);
      mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(RI_PogoShaderBarrier(
          state.renderTarget[mpGraphics->swapchainIndex].Get(), /*initial=*/false));
    }
  }

  // Restore depth to DEPTH_ATTACHMENT_OPTIMAL before yielding the command
  // buffer: RI_VK_FillDepthAttachment hardcodes that layout, and depth ends
  // here in either SHADER_READ_ONLY_OPTIMAL (compute-only) or
  // DEPTH_READ_ONLY_OPTIMAL (flipDepthToReadOnly ran for particle/decal).
  {
    const uint32_t beforeState = depthFlippedForReadOnly
                                     ? RI_RESOURCE_STATE_DEPTH_READ
                                     : RI_RESOURCE_STATE_SHADER_RESOURCE;
    const uint32_t beforeStages = depthFlippedForReadOnly
                                      ? RI_STAGE_NONE
                                      : (RI_STAGE_FRAGMENT | RI_STAGE_COMPUTE);

    mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(
        {state.depthTextures[mpGraphics->swapchainIndex].Get(), beforeState,
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
    mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(
        {state.renderTarget[mpGraphics->swapchainIndex].Get(),
         RI_RESOURCE_STATE_SHADER_RESOURCE,
         RI_RESOURCE_STATE_RENDER_TARGET_READ, RI_STAGE_FRAGMENT});

    {
      RITextureView colorView = *state.renderTargetView[mpGraphics->swapchainIndex];
      RIRenderingAttachment color = {};
      color.view = colorView;
      color.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
      color.storeOp = RI_ATTACHMENT_STORE_OP_STORE;

      // Scene depth, restored to DEPTH_ATTACHMENT_OPTIMAL just above; the
      // overlay pipelines test against it but never write. LOAD, no clear.
      RIRenderingAttachment depth = {};
      depth.view = *state.depthView[mpGraphics->swapchainIndex];
      depth.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
      depth.storeOp = RI_ATTACHMENT_STORE_OP_STORE;

      RIBeginRenderingDesc beginDesc = {};
      beginDesc.renderArea.width = (int16_t)renderWidth;
      beginDesc.renderArea.height = (int16_t)renderHeight;
      beginDesc.colorCount = 1;
      beginDesc.colors = &color;
      beginDesc.depthStencil = &depth;
      mpGraphics->primary.cmds[0].vk_d3d12_beginRendering(&mpGraphics->device, beginDesc);

      debugDraw->flush(cntx, &mpGraphics->primary.cmds[0], apFrustum, renderWidth,
                       renderHeight, cGraphics::PogoColorFormat);

      mpGraphics->primary.cmds[0].vk_d3d12_endRendering(&mpGraphics->device);
    }
    mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(RI_PogoShaderBarrier(
        state.renderTarget[mpGraphics->swapchainIndex].Get(), /*initial=*/false));
  }

  // Commit every bindless handle / slot-generation write made this frame. The
  // uploader records into its own transfer cmd buffer, flushed as a fenced
  // pre-pass the primary submit waits on (cGraphics), so this single tail
  // call lands all copies ahead of every primary read regardless of recording
  // order.
  mpGraphics->globalset->flushMirrors(&mpGraphics->device);
}

cHybridRenderer::~cHybridRenderer() {
  // The ray-tracing TLAS + its storage/instance buffers now live on cWorld and
  // are disposed with the world.
  // The global managed set is engine-lifetime; ShutdownGlobalManagedSets()
  // (cGraphics teardown) destroys it after the device is idle.
  // Fallback vertex streams are cGraphics members (process-lifetime, like
  // Interface<cGraphics>::Get()->nulVertexBuffer); not freed here.
  // Every pipeline program owned by the renderer. dispose() frees the pipelines,
  // pipeline layout, descriptor-set layouts, and (for m_pathTrace) the ray-tracing
  // SBT buffer — the last of which is a VMA allocation that otherwise trips the
  // vmaDestroyAllocator leak assert at device teardown. Safe on an unused program.
  RIProgram *programs[] = {
      &m_gbuffer,        &m_vBufferPomBary,
      &m_lightGrid,      &m_composite,           &m_directLighting,
      &m_directSpatialReuse, &m_nrdPack,
      &m_particle,
      &m_translucentMesh, &m_decal,               &m_water,
      &m_pathTrace,
  };
  for (RIProgram *p : programs)
    p->dispose(&mpGraphics->device);

  // Per-frame indirect-draw args buffer (INDIRECT | TRANSFER_DST).
  m_indirectDrawBuffer.dispose(&mpGraphics->device);
  m_indirectDrawBuffer = {};
}

} // namespace hpl
