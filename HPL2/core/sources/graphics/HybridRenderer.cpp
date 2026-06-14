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
#include "scene/Viewport.h"
#include "graphics/Renderable.h"
#include "graphics/VertexBuffer.h"
#include "graphics/VertexBuffer.h"
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
#include <span>
#include <unordered_set>
#include <vector>

namespace hpl {

namespace detail {

static inline bool BindVertexStreams(struct RICmd *cmd, cVertexBuffer *pVB,
                                     const char *passLabel,
                                     uint32_t *outPresentMask) {
  auto *vbri = static_cast<cVertexBuffer *>(pVB);
  auto bufOf = [&](eVertexBufferElement type) -> RIBuffer * {
    const auto *element = vbri->GetElement(type);
    return (element && element->buffer) ? element->buffer.get() : nullptr;
  };
  // Position + index are the only truly required streams — without geometry
  // there's nothing to draw.
  RIBuffer *pos = bufOf(eVertexBufferElement_Position);
  const auto &idxRI = vbri->GetIndexRIBuffer();
  RIBuffer *idx = idxRI ? idxRI.get() : nullptr;
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
  RIBuffer *uv  = bufOf(eVertexBufferElement_Texture0);
  uint32_t mask = eVertexElementFlag_Position; // required, present per check above
  if (nrm) mask |= eVertexElementFlag_Normal;
  if (tan) mask |= eVertexElementFlag_Texture1;
  if (col) mask |= eVertexElementFlag_Color0;
  if (uv)  mask |= eVertexElementFlag_Texture0;
  if (outPresentMask)
    *outPresentMask = mask;
  RIBuffer *vertBufs[5] = {
      pos,
      nrm ? nrm : &RI.fallbackNormalVertex,
      tan ? tan : &RI.fallbackTangentVertex,
      col ? col : &RI.fallbackColorVertex,
      uv  ? uv  : &RI.fallbackUv0Vertex,
  };
  cmd->bindVertexBuffers<5>(0, 5, vertBufs);
  cmd->bindIndexBuffer(&RI.renderer, idx, 0, RI_INDEX_TYPE_32);
  return true;
}

} // namespace detail

// A renderable needs a BLAS only if it can become a TLAS instance — i.e. it's a
// mesh. Particles/billboards/beams/ropes/decals are never ray-traced (the TLAS
// gather skips them), so their per-frame BLAS build is dead work.
static bool renderableNeedsBlas(iRenderable *apObject) {
  return apObject && apObject->GetRenderType() == eRenderableType_SubMesh;
}

// ---------------------------------------------------------------------------
// Explicit per-backend binding tables (replace spirv_reflect). One block here
// for every RIProgram this file initializes. Vulkan keys by {set,binding} (the
// shader's [vk::binding(slot,set)]); Metal keys by the per-stage flat
// [[buffer/texture/sampler(N)]] index read off the generated .metal entry
// point. Only PROGRAM-MANAGED descriptors (the resources each program pushes
// via bindDescriptors) appear here — the set-0 bindless globals (gTextures2D[],
// gSceneObjects, gPerFrame's siblings, gOpaque*Handles, gDiffuseMaterials,
// gPointLights, ... ) are EXTERNAL (m_global.m_bindlessSet, bound via
// bindExternalSet) and are deliberately omitted / skipped.
//
// Program-managed set/binding reference:
//   gPerFrame        set 1 binding 0   UNIFORM_BUFFER  (PerFrame/resource.slang)
//   gPackedHitInfo   set 1 binding 31  STORAGE_IMAGE   (bindless.slang, RWTexture2D<uint4>)
//   gSurfelDepthMap  set 1 binding 33  STORAGE_IMAGE   (bindless.slang, RWTexture2D<float2>)
//   gSurfelDepth     set 1 binding 34  SAMPLED_IMAGE   (bindless.slang, Texture2D<float2>)
//   gRtAccel         set 1 binding 36  ACCEL           (bindless.slang)
//   (per-pass set 2 textures / CBs are declared in each pass's own .cs.slang)
// ---------------------------------------------------------------------------

// Combined ray-tracing stage mask (the RT programs apply one table across all
// four entry points: raygen / miss / closesthit / anyhit).
static constexpr uint32_t kRtStages =
    RI_SHADER_STAGE_RAYGEN | RI_SHADER_STAGE_MISS |
    RI_SHADER_STAGE_CLOSEST_HIT | RI_SHADER_STAGE_ANY_HIT;
static constexpr uint16_t kMtlNone = RIProgram::RI_MTL_NONE;

// ----- SurfelGBuffer.3d (m_gbuffer): gPerFrame (VS+FS, Metal buffer 34) -----
static const RIProgram::RIProgramBinding kGBuffer[] = {
    {"gPerFrame", RI_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
     RI_SHADER_STAGE_VERTEX | RI_SHADER_STAGE_FRAGMENT, {1, 0}, {}, {34}}};

// ----- Particle (m_particle): gPerFrame VS=buffer34 / FS=buffer35 + fs PC -----
static const RIProgram::RIProgramBinding kParticle[] = {
    {"gPerFrame", RI_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, RI_SHADER_STAGE_VERTEX,   {1, 0}, {}, {34}},
    {"gPerFrame", RI_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, RI_SHADER_STAGE_FRAGMENT, {1, 0}, {}, {35}}};

// ----- Translucent (m_translucentMesh): gPerFrame VS=34 / FS=35 + fs PC -----
static const RIProgram::RIProgramBinding kTranslucent[] = {
    {"gPerFrame", RI_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, RI_SHADER_STAGE_VERTEX,   {1, 0}, {}, {34}},
    {"gPerFrame", RI_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, RI_SHADER_STAGE_FRAGMENT, {1, 0}, {}, {35}}};

// ----- Decal (m_decal): gPerFrame VS only (frag has only the PC) -----
static const RIProgram::RIProgramBinding kDecal[] = {
    {"gPerFrame", RI_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, RI_SHADER_STAGE_VERTEX, {1, 0}, {}, {34}}};

// ----- Water (m_water): gPerFrame VS=34 / FS=35 + fs PC -----
static const RIProgram::RIProgramBinding kWater[] = {
    {"gPerFrame", RI_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, RI_SHADER_STAGE_VERTEX,   {1, 0}, {}, {34}},
    {"gPerFrame", RI_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, RI_SHADER_STAGE_FRAGMENT, {1, 0}, {}, {35}}};

// ----- SurfelPreparePass: external set only (no program-managed bindings) -----

// ----- SurfelUpdatePass (collect / accumulate / scatter): gPerFrame -----
// Metal: gPerFrame buffer(1); set-0 globals are in the bindless arg buffer.
static const RIProgram::RIProgramBinding kSurfelUpdateCs[] = {
    {"gPerFrame", RI_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, RI_SHADER_STAGE_COMPUTE, {1, 0}, {}, {0}}};

// ----- LightGridBuildPass (m_lightGridBin): gPerFrame -----
// Per-set Metal argument buffers: set 0 (bindless) at buffer(0), set 1
// (program) at buffer(1). Set-1 [[id]] == mtl.index here (gPerFrame id 0).
static const RIProgram::RIProgramBinding kLightGridCs[] = {
    {"gPerFrame", RI_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, RI_SHADER_STAGE_COMPUTE, {1, 0}, {}, {0}}};

// ----- SurfelIntegratePass (m_surfelIntegrate) -----
// Set-1 arg buffer (buffer(1)); [[id]] == mtl.index: gPerFrame 0,
// gSurfelDepthMap 1, gSurfelDepth 2. gSurfelDepthSampler is bindless (Set0 id 35).
static const RIProgram::RIProgramBinding kSurfelIntegrateCs[] = {
    {"gPerFrame", RI_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, RI_SHADER_STAGE_COMPUTE, {1, 0}, {}, {0}},
    {"gSurfelDepthMap", RI_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, RI_SHADER_STAGE_COMPUTE, {1, kBindingSurfelDepthMap}, {}, {1}},
    {"gSurfelDepth", RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, RI_SHADER_STAGE_COMPUTE, {1, kBindingSurfelDepthSampled}, {}, {2}}};

// ----- SurfelGenerationPass (m_surfelGenerate) -----
// Set-1 arg buffer (buffer(1), [[id]]==mtl.index: gPerFrame 0, gPackedHitInfo 1,
// gSurfelDepth 2); Set-2 (buffer(2)): gOutput id 0. Sampler is bindless (Set0 id 35).
static const RIProgram::RIProgramBinding kSurfelGenerateCs[] = {
    {"gPerFrame", RI_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, RI_SHADER_STAGE_COMPUTE, {1, 0}, {}, {0}},
    {"gPackedHitInfo", RI_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, RI_SHADER_STAGE_COMPUTE, {1, kBindingPackedHitInfo}, {}, {1}},
    {"gSurfelDepth", RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, RI_SHADER_STAGE_COMPUTE, {1, kBindingSurfelDepthSampled}, {}, {2}},
    {"gOutput", RI_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, RI_SHADER_STAGE_COMPUTE, {2, 0}, {}, {0}}};

// ----- DirectLightingPass (m_directLighting) -----
static const RIProgram::RIProgramBinding kDirectLightingCs[] = {
    {"gPerFrame", RI_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, RI_SHADER_STAGE_COMPUTE, {1, 0}, {}, {34}},
    {"gPackedHitInfo", RI_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, RI_SHADER_STAGE_COMPUTE, {1, kBindingPackedHitInfo}, {}, {6}},
    {"gPackedHitInfoRaster", RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, RI_SHADER_STAGE_COMPUTE, {2, 0}, {}, {0}},
    {"gVelocity", RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, RI_SHADER_STAGE_COMPUTE, {2, 1}, {}, {1}},
    {"gReservoirHistory", RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, RI_SHADER_STAGE_COMPUTE, {2, 2}, {}, {2}},
    {"gReservoirOut", RI_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, RI_SHADER_STAGE_COMPUTE, {2, 3}, {}, {3}},
    {"gDirectKeyHistory", RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, RI_SHADER_STAGE_COMPUTE, {2, 4}, {}, {4}},
    {"gDirectKeyOut", RI_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, RI_SHADER_STAGE_COMPUTE, {2, 5}, {}, {5}}};

// ----- DirectSpatialReusePass (m_directSpatialReuse) -----
static const RIProgram::RIProgramBinding kDirectSpatialCs[] = {
    {"gPerFrame", RI_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, RI_SHADER_STAGE_COMPUTE, {1, 0}, {}, {34}},
    {"gPackedHitInfo", RI_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, RI_SHADER_STAGE_COMPUTE, {1, kBindingPackedHitInfo}, {}, {8}},
    {"gRtAccel", RI_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE, 1, RI_SHADER_STAGE_COMPUTE, {1, kBindingTlas}, {}, {33}},
    {"gPackedHitInfoRaster", RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, RI_SHADER_STAGE_COMPUTE, {2, 0}, {}, {0}},
    {"gReservoirIn", RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, RI_SHADER_STAGE_COMPUTE, {2, 1}, {}, {1}},
    {"gDirectKey", RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, RI_SHADER_STAGE_COMPUTE, {2, 2}, {}, {2}},
    {"gReservoirOut", RI_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, RI_SHADER_STAGE_COMPUTE, {2, 3}, {}, {3}},
    {"gDirectLighting", RI_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, RI_SHADER_STAGE_COMPUTE, {2, 4}, {}, {4}},
    {"gVelocity", RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, RI_SHADER_STAGE_COMPUTE, {2, 5}, {}, {5}},
    {"gDirectHistory", RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, RI_SHADER_STAGE_COMPUTE, {2, 6}, {}, {6}},
    {"gDirectKeyHistory", RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, RI_SHADER_STAGE_COMPUTE, {2, 7}, {}, {7}}};

// ----- DirectAtrousPass (m_directAtrous) -----
static const RIProgram::RIProgramBinding kDirectAtrousCs[] = {
    {"gAtrousIn", RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, RI_SHADER_STAGE_COMPUTE, {2, 0}, {}, {0}},
    {"gDirectKey", RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, RI_SHADER_STAGE_COMPUTE, {2, 1}, {}, {1}},
    {"gAtrousOut", RI_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, RI_SHADER_STAGE_COMPUTE, {2, 2}, {}, {2}},
    {"gAtrous", RI_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, RI_SHADER_STAGE_COMPUTE, {2, 3}, {}, {0}}};

// ----- MainCompositePass (m_mainComposite). gRtAccel is VK-only — slangc
// dropped it from the MSL kernel (RI_MTL_NONE skips it on Metal). -----
static const RIProgram::RIProgramBinding kMainCompositeCs[] = {
    {"gPerFrame", RI_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, RI_SHADER_STAGE_COMPUTE, {1, 0}, {}, {34}},
    {"gPackedHitInfo", RI_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, RI_SHADER_STAGE_COMPUTE, {1, kBindingPackedHitInfo}, {}, {4}},
    {"gRtAccel", RI_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE, 1, RI_SHADER_STAGE_COMPUTE, {1, kBindingTlas}, {}, {kMtlNone}},
    {"gIndirectLighting", RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, RI_SHADER_STAGE_COMPUTE, {2, 0}, {}, {0}},
    {"gPackedHitInfoRaster", RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, RI_SHADER_STAGE_COMPUTE, {2, 1}, {}, {2}},
    {"gOutput", RI_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, RI_SHADER_STAGE_COMPUTE, {2, 2}, {}, {3}},
    {"gDirectLighting", RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, RI_SHADER_STAGE_COMPUTE, {2, 3}, {}, {1}}};

// ----- SurfelVBuffer (m_surfelVBuffer, RT): VK-only (no RT MSL yet) -----
static const RIProgram::RIProgramBinding kSurfelVBuffer[] = {
    {"gPerFrame", RI_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, kRtStages, {1, 0}, {}, {kMtlNone}},
    {"gPackedHitInfo", RI_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, kRtStages, {1, kBindingPackedHitInfo}, {}, {kMtlNone}},
    {"gRtAccel", RI_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE, 1, kRtStages, {1, kBindingTlas}, {}, {kMtlNone}}};

// ----- SurfelRayTrace (m_surfelRT, RT): VK-only -----
static const RIProgram::RIProgramBinding kSurfelRT[] = {
    {"gPerFrame", RI_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, kRtStages, {1, 0}, {}, {0}},
    {"gRtAccel", RI_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE, 1, kRtStages, {1, kBindingTlas}, {}, {1}}};

cHybridRenderer::cHybridRenderer(cGraphics *apGraphics, cResources *apResources)
    : iRenderer("Hybrid", apGraphics, apResources) {
  {
    // Build the global bindless descriptor set (set 0) and create + seed every
    // buffer bound to it. All set-0 state now lives in m_global.
    m_global.initialize(&RI.device, mpResources);

    // Set 0 is the global bindless set, owned by m_global and shared across
    // every program below as an external set (bound via bindExternalSet).
    RIBindlessDescriptorSet *const externalSets[] = {&m_global.m_bindlessSet};
    {
      // Gbuffer pass: one .spv, two entry points (vsMain / psMain).
      auto gbuffer_bin = RIProgram::loadShaderStage(
          apResources->GetFileSearcher(), "SurfelGBuffer.3d.spv");
      std::array<RIProgram::ModuleStage, 2> stages = {
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, gbuffer_bin,
                                 "vsMain"},
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, gbuffer_bin,
                                 "psMain"}};
      RIProgram::RIProgramDescriptor desc = {};
      desc.stages = stages;
      desc.bindings = kGBuffer;
      desc.externalSets = externalSets;
      m_gbuffer.initialize(&RI.device, desc);
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
      // RT pipeline: VK-only bindings (no RT MSL generated yet — mtl RI_MTL_NONE).
      RIProgram::RIProgramDescriptor desc = {};
      desc.stages = stages;
      desc.bindings = kSurfelVBuffer;
      desc.externalSets = externalSets;
      m_surfelVBuffer.initialize(&RI.device, desc);
    }
    // Compute load that passes the Slang entry-point name + the unified,
    // combined-stage binding table through to the descriptor.
    auto loadSlangCompute =
        [&](RIProgram &prog, const char *name, const char *entryPoint,
            std::span<const RIProgram::RIProgramBinding> bindings = {}) {
          auto bin =
              RIProgram::loadShaderStage(apResources->GetFileSearcher(), name);
          std::array<RIProgram::ModuleStage, 1> stages = {RIProgram::ModuleStage{
              RIProgram::PROGRAM_STAGE_COMPUTE, bin, entryPoint}};
          RIProgram::RIProgramDescriptor desc = {};
          desc.stages = stages;
          desc.bindings = bindings;
          desc.externalSets = externalSets;
          prog.initialize(&RI.device, desc);
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
        RIProgram::RIProgramDescriptor desc = {};
        desc.stages = stages;
        desc.bindings = kSurfelUpdateCs;
        desc.externalSets = externalSets;
        prog.initialize(&RI.device, desc);
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
      // RT pipeline: VK-only bindings (no RT MSL generated yet — mtl RI_MTL_NONE).
      RIProgram::RIProgramDescriptor desc = {};
      desc.stages = stages;
      desc.bindings = kSurfelRT;
      desc.externalSets = externalSets;
      m_surfelRT.initialize(&RI.device, desc);
    }
    // LightGridBuildPass — single compute entry (binLights) that bins point/spot
    // lights into the coarse world-space light grid each frame.
    loadSlangCompute(m_lightGridBin, "LightGridBuildPass.cs.spv", "binLights",
                     kLightGridCs);
    loadSlangCompute(m_surfelIntegrate, "SurfelIntegratePass.cs.spv", "csMain",
                     kSurfelIntegrateCs);
    loadSlangCompute(m_surfelGenerate, "SurfelGenerationPass.cs.spv", "csMain",
                     kSurfelGenerateCs);
    // MainComposite — compute pass: one thread per pixel writes the composite
    // (albedo + inline decals + lighting) into the pogo attach bound as gOutput.
    // The renderer transitions the attach to GENERAL around the dispatch and back
    // to COLOR_ATTACHMENT_OPTIMAL afterwards.
    loadSlangCompute(m_mainComposite, "MainCompositePass.cs.spv", "csMain",
                     kMainCompositeCs);
    loadSlangCompute(m_directLighting, "DirectLightingPass.cs.spv", "csMain",
                     kDirectLightingCs);
    loadSlangCompute(m_directSpatialReuse, "DirectSpatialReusePass.cs.spv",
                     "csMain", kDirectSpatialCs);
    loadSlangCompute(m_directAtrous, "DirectAtrousPass.cs.spv", "csMain",
                     kDirectAtrousCs);
    {
      // Particle pass (amnesia/slang/ParticlePass).
      auto p_vert = RIProgram::loadShaderStage(apResources->GetFileSearcher(),
                                               "Particle.vert.spv");
      auto p_frag = RIProgram::loadShaderStage(apResources->GetFileSearcher(),
                                               "Particle.frag.spv");
      std::array<RIProgram::ModuleStage, 2> stages = {
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, p_vert, "vsMain"},
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, p_frag, "psMain"}};
      RIProgram::RIProgramDescriptor desc = {};
      desc.stages = stages;
      desc.bindings = kParticle;
      desc.externalSets = externalSets;
      // ParticlePushConstants (PushBlock: blendMode + sceneAlpha), fragment-only.
      desc.pushConstantSize = 8;
      desc.pushConstantStages = RI_SHADER_STAGE_FRAGMENT;
      m_particle.initialize(&RI.device, desc);
    }
    {
      // Translucent mesh pass (amnesia/slang/TranslucentPass). Shares
      // externalSets with m_particle so the same bindless set / per-frame
      // UBO bindings light up.
      auto t_vert = RIProgram::loadShaderStage(apResources->GetFileSearcher(),
                                               "Translucent.vert.spv");
      auto t_frag = RIProgram::loadShaderStage(apResources->GetFileSearcher(),
                                               "Translucent.frag.spv");
      std::array<RIProgram::ModuleStage, 2> stages = {
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, t_vert, "vsMain"},
          RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, t_frag, "psMain"}};
      RIProgram::RIProgramDescriptor desc = {};
      desc.stages = stages;
      desc.bindings = kTranslucent;
      desc.externalSets = externalSets;
      // TranslucentPushConstants (PushBlock: blendMode/sceneAlpha/options/pad),
      // fragment-only.
      desc.pushConstantSize = 16;
      desc.pushConstantStages = RI_SHADER_STAGE_FRAGMENT;
      m_translucentMesh.initialize(&RI.device, desc);
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
      RIProgram::RIProgramDescriptor desc = {};
      desc.stages = stages;
      desc.bindings = kDecal;
      desc.externalSets = externalSets;
      // DecalPushConstants, fragment-only.
      desc.pushConstantSize = 4;
      desc.pushConstantStages = RI_SHADER_STAGE_FRAGMENT;
      m_decal.initialize(&RI.device, desc);
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
      RIProgram::RIProgramDescriptor desc = {};
      desc.stages = stages;
      desc.bindings = kWater;
      desc.externalSets = externalSets;
      // WaterPC (pass + 3 pad words), fragment-only.
      desc.pushConstantSize = 16;
      desc.pushConstantStages = RI_SHADER_STAGE_FRAGMENT;
      m_water.initialize(&RI.device, desc);
    }

    RISegmentAllocDesc indirectDesc = {};
    indirectDesc.numSegments = RI_NUMBER_FRAMES_FLIGHT;
    indirectDesc.elementStride = sizeof(RIDrawIndirectCommand);
    indirectDesc.maxElements = kObjectSlotCapacity;
    m_indirectSegment = RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS>(&indirectDesc);
    m_indirectDrawBuffer = detail::CreateBindlessSlotBuffer(
        &RI.device, indirectDesc.maxElements, sizeof(RIDrawIndirectCommand),
        RI_BUFFER_USAGE_INDIRECT | RI_BUFFER_USAGE_TRANSFER_DST);

    // (The per-viewport frame textures — surfel result, packed hit info,
    // velocity, direct-lighting history — live on
    // cViewport::HybridViewportState; see its Update below.)

    // (Per-bounce refraction/reflection V-buffers removed: water refraction now
    // clobbers the primary gPackedHitInfo like glass; water reflection is drawn
    // in the raster water pass.)

    // Surfel-ray depth atlas — RGBA16F storing the (E[z], E[z^2]) Chebyshev
    // pair per octahedral tile texel for each surfel (only .rg are populated;
    // .ba left zero). 4096x4096 footprint; the integrate shader's tile-index
    // math is `surfelIndex % (W/6), surfelIndex / (W/6)`. STORAGE for integrate
    // writes, SAMPLED for integrate's own readback.
    for (uint32_t i = 0; i < RI.swapchain.imageCount; ++i) {
      // TRANSFER_DST: this atlas is seeded once via RICmd::clearStorageImage.
      RITextureDesc atlasDesc = {};
      atlasDesc.type = RI_TEXTURE_2D;
      atlasDesc.format = RI_FORMAT_RGBA16_SFLOAT;
      atlasDesc.width = 4096u;
      atlasDesc.height = 4096u;
      atlasDesc.depth = 1u;
      atlasDesc.mipNum = 1;
      atlasDesc.layerNum = 1;
      atlasDesc.usage = RI_USAGE_SHADER_RESOURCE_STORAGE |
                        RI_USAGE_SHADER_RESOURCE | RI_USAGE_TRANSFER_DST;
      m_surfelDepthTexture[i] = RITexture::create(&RI.device, atlasDesc);

      RITextureViewDesc atlasView = {};
      atlasView.viewType = RI_VIEWTYPE_SHADER_RESOURCE_2D;
      atlasView.format = RI_FORMAT_RGBA16_SFLOAT;
      atlasView.mipNum = 1;
      atlasView.layerNum = 1;
      m_surfelDepthView[i] =
          RITextureView::create(&RI.device, &m_surfelDepthTexture[i], atlasView);
    }

  }
}

// --------------------------------------------------------------------
// cViewport::HybridViewportState — the viewport holds the state, this
// backend refines it: Update receives the target size (GetTargetSize) and
// grows it by the guard band internally — every target renders the overscan
// frame; GetBackBuffer exposes the centered authored window, which cScene
// feeds into the viewport pogo in the post-processing step. Replacement/
// resize hand the old targets to the frame freelist (drained once the
// pipeline is done with them) — no stall.
// --------------------------------------------------------------------

void cViewport::HybridViewportState::Update(RIBootstrap::FrameContext *cntx,
                                            cVector2l size) {
  if (size.x <= 0 || size.y <= 0) {
    return;
  }
  const uint32_t renderW = overscanExtent((uint32_t)size.x);
  const uint32_t renderH = overscanExtent((uint32_t)size.y);
  if (width == renderW && height == renderH &&
      targetWidth == (uint32_t)size.x && targetHeight == (uint32_t)size.y) {
    return;
  }

  Dispose(cntx);

  width = renderW;
  height = renderH;
  targetWidth = (uint32_t)size.x;   // the BackBuffer crop window
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
        &renderTarget[i], &renderTargetColorView[i], &renderTargetDescriptor[i],
        "HybridViewportState.renderTarget");

    // SAMPLED lets surfel_generate / surfel_raytrace bind the depth as
    // `sampler2D depthMap` after the gbuffer pass flips it to
    // SHADER_READ_ONLY.
    // DEPTH|STENCIL view: the Z passes only touch the depth aspect, but
    // cLuxEffectRenderer's outline pass binds this same view as a stencil
    // attachment (mark silhouette -> NOTEQUAL composite), so the view must
    // carry the stencil aspect. The depth aspect is never sampled (surfel
    // passes sample their own depth atlas), so one combined view suffices.
    CreateViewportAttachmentTexture(
        &RI.device, renderW, renderH, RIBootstrap::DepthFormat,
        RI_USAGE_DEPTH_STENCIL_ATTACHMENT | RI_USAGE_SHADER_RESOURCE,
        RI_VIEWTYPE_DEPTH_STENCIL_ATTACHMENT,
        &depthTextures[i], &depthView[i], "HybridViewportState.depth");

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

void cViewport::HybridViewportState::Dispose(RIBootstrap::FrameContext *cntx) {
  for (uint32_t i = 0; i < RI_MAX_SWAPCHAIN_IMAGES; i++) {
    ReleaseViewportColorTexture(cntx->freelist, &renderTarget[i],
                                &renderTargetColorView[i], &renderTargetDescriptor[i]);
    ReleaseViewportAttachmentTexture(cntx->freelist, &depthTextures[i],
                                     &depthView[i]);
    ReleaseViewportAttachmentTexture(cntx->freelist, &visibilityTexture[i],
                                     &visibilityView[i]);
    ReleaseViewportAttachmentTexture(cntx->freelist, &surfelResultTexture[i],
                                     &surfelResultView[i]);
    ReleaseViewportAttachmentTexture(cntx->freelist, &packedHitInfoTexture[i],
                                     &packedHitInfoView[i]);
    ReleaseViewportAttachmentTexture(cntx->freelist, &velocityTexture[i],
                                     &velocityView[i]);
  }
  for (uint32_t i = 0; i < 2; i++) {
    ReleaseViewportAttachmentTexture(cntx->freelist, &directLightingTexture[i],
                                     &directLightingView[i]);
    ReleaseViewportAttachmentTexture(cntx->freelist, &directKeyTexture[i],
                                     &directKeyView[i]);
    ReleaseViewportAttachmentTexture(cntx->freelist, &directAtrousTexture[i],
                                     &directAtrousView[i]);
    ReleaseViewportAttachmentTexture(cntx->freelist, &reservoirTexture[i],
                                     &reservoirView[i]);
  }
  ReleaseViewportAttachmentTexture(cntx->freelist, &reservoirTemporalTexture,
                                   &reservoirTemporalView);
  width = height = 0;
  targetWidth = targetHeight = 0;
  directLightingIndex = 0;
  directLightingInit = false;
  hasPrevCamera = false;
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
  const uint32_t renderWidth = state.width;   // overscan applied by Update
  const uint32_t renderHeight = state.height;

  ml::float4x4 mainFrustumViewInvMat = apFrustum->GetViewMat();
  mainFrustumViewInvMat.Invert();
  const ml::float4x4 mainFrustumViewMat = apFrustum->GetViewMat();
  ml::float4x4 mainFrustumProjMat = apFrustum->GetProjectionMat();
  // Guard band: widen the FOV by the overscan factor so the cropped center keeps
  // the authored FOV. Scaling the projection's x/y focal terms (diagonal a[0],
  // a[5]) by 1/(1+2f) zooms out symmetrically about the centre. Done here so the
  // widened projection flows into perFrame.projMat, invProjMat, and state.prevProjMat
  // (velocity) — and cameraU/V are widened to match below. Every target renders
  // the overscan frame (Update applies the same factor), so this is
  // unconditional.
  {
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
    // No frustum culling: the render list rebuilds fresh every frame and the
    // TLAS instances are gathered from it — RT shadows/GI need whole-map
    // geometry, including everything behind the camera. (Replaces the [TEMP]
    // persistent-render-list mechanism, which accumulated dangling pointers
    // to destroyed renderables and missed never-yet-seen geometry.)
    rendering::WalkAndPrepareRenderList(dynamicContainer, apFrustum,
                                        prepareObjectHandler,
                                        eRenderableFlag_VisibleInNonReflection,
                                        /*abIgnoreFrustumCull=*/true);
    rendering::WalkAndPrepareRenderList(staticContainer, apFrustum,
                                        prepareObjectHandler,
                                        eRenderableFlag_VisibleInNonReflection,
                                        /*abIgnoreFrustumCull=*/true);
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
    // its storage, and the vertex/index buffers onto resourceLink,
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
                static_cast<cVertexBuffer *>(pObject->GetVertexBuffer());
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
  // dirty; SubmitToGPU then allocates/uploads dirty streams and BuildBlas
  // rebuilds the BLAS (both no-op on repeat via their generation checks). Done
  // once here so the TLAS build, particle pass, and mesh pass all consume
  // already-prepared buffers.
  //
  // Must run BEFORE any vkCmdBeginRendering so the uploader's barriers and
  // BLAS-build cmds don't collide with a dynamic-rendering scope.
  for (iRenderable *pObj :
       m_rendererList.GetRenderableItems(eRenderListType_Translucent)) {
    if (!pObj)
      continue;
    pObj->UpdateGraphicsForFrame(afFrameTime);
    pObj->UpdateGraphicsForViewport(apFrustum, afFrameTime);
    cVertexBuffer *pVB = pObj->GetVertexBuffer();
    if (pVB) {
      auto *vbri = static_cast<cVertexBuffer *>(pVB);
      // Particles/billboards/beams/ropes are translucent but never TLAS
      // instances — upload their streams for the raster pass, skip the BLAS.
      vbri->SubmitToGPU(&RI.blasSubmit.cmds[0], &RI.device, cntx);
      if (renderableNeedsBlas(pObj)) {
        vbri->BuildBlas(&RI.blasSubmit.cmds[0], &RI.device, cntx);
      }
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

    cVertexBuffer *pVB = pObj->GetVertexBuffer();
    if (pVB) {
      auto *vbri = static_cast<cVertexBuffer *>(pVB);
      // Decals are never TLAS instances — upload streams, no BLAS.
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
  // Previous-frame view/proj for motion vectors (gbuffer velocity target). On
  // the first frame use this frame's matrices so velocity reads ~zero, then
  // remember this frame's for the next.
  if (!state.hasPrevCamera) {
    std::memcpy(state.prevViewMat, mainFrustumViewMat.a, sizeof(state.prevViewMat));
    std::memcpy(state.prevProjMat, mainFrustumProjMat.a, sizeof(state.prevProjMat));
    state.hasPrevCamera = true;
  }
  std::memcpy(perFrame.prevViewMat, state.prevViewMat, sizeof(perFrame.prevViewMat));
  std::memcpy(perFrame.prevProjMat, state.prevProjMat, sizeof(perFrame.prevProjMat));
  std::memcpy(state.prevViewMat, mainFrustumViewMat.a, sizeof(state.prevViewMat));
  std::memcpy(state.prevProjMat, mainFrustumProjMat.a, sizeof(state.prevProjMat));
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
  RISegmentReq indirectReq = {};
  const bool indirectOk =
      m_indirectSegment.request(RI.frameIndex, solids.size(), &indirectReq);
  assert(indirectOk);
  auto *indirectDst = reinterpret_cast<RIDrawIndirectCommand *>(
      static_cast<uint8_t *>(m_indirectDrawBuffer.mappedAddress) +
      (size_t)indirectReq.elementOffset * sizeof(RIDrawIndirectCommand));
  uint32_t writtenDraws = 0;

  // TLAS instance accumulator (backend-neutral; serialized per backend via
  // RI_WriteAccelInstance at upload). Sized for the shadow caster set (every
  // shadow caster contributes at most one TLAS instance).
  std::vector<RIAccelInstanceDesc> tlasInstances;
  tlasInstances.reserve(solids.size());

  // Deduplicated BLAS set the instances index into. Metal references BLASes by
  // index (instancedAccelerationStructures); Vulkan ignores blasIndex (it bakes
  // the device address into the instance buffer). A small linear scan keyed on
  // the BLAS pointer assigns each instance its index.
  std::vector<RIAccelStructure *> tlasBlasList;
  auto blasIndexFor = [&](RIAccelStructure *b) -> uint32_t {
    for (uint32_t i = 0; i < (uint32_t)tlasBlasList.size(); ++i)
      if (tlasBlasList[i] == b)
        return i;
    tlasBlasList.push_back(b);
    return (uint32_t)(tlasBlasList.size() - 1);
  };

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
    pl.color[0] = hpl::sRGBToLinear(c.r);
    pl.color[1] = hpl::sRGBToLinear(c.g);
    pl.color[2] = hpl::sRGBToLinear(c.b);
    pl.intensity = pLight->GetIntensity();
    pl.radius = pLight->GetRadius();
    /*
    {
      const float maxC = std::max(pl.color[0], std::max(pl.color[1], pl.color[2]));
      const float reachSq = ((maxC * authored) / kLightRadianceFloor) - kPointLightSourceRadiusSq;
      const float calculatedReach = reachSq > 0.f ? std::sqrt(reachSq) : 0.f;
      pl.radius = calculatedReach;
    }
    */
    // Physical source radius drives the soft-shadow penumbra in the direct pass.
    // Authored per-light via cLight::SetSourceRadius; when a light authors none
    // (0), fall back to a fraction of its authored reach radius so it's softly
    // shadowed by default instead of hard. An explicit author value always wins.
    const float authoredSourceRadius = pLight->GetSourceRadius();
    pl.sourceRadius = authoredSourceRadius;
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
    RIResourceBufferTransaction trans = {};
    trans.target = m_global.m_pointLightBuffer;
    trans.size = uploadBytes;
    trans.offset = 0;
    // After the first frame the buffer was previously read as a storage
    // resource; tell the uploader to barrier from that to TRANSFER_WRITE
    // and back. On the very first frame the buffer is uninitialised, so
    // the src side of the barrier is a no-op against zero contents — safe.
    trans.currentState = RI_RESOURCE_STATE_UNORDERED_ACCESS;
    trans.currentStages = RI_STAGE_FRAGMENT | RI_STAGE_COMPUTE;
    trans.postState = RI_RESOURCE_STATE_UNORDERED_ACCESS;
    trans.postStages = RI_STAGE_FRAGMENT | RI_STAGE_COMPUTE;

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
    const cMatrixf &world = pLight->GetWorldMatrix();
    sl.direction[0] = -world.m[0][2];
    sl.direction[1] = -world.m[1][2];
    sl.direction[2] = -world.m[2][2];
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
    sl.color[0] = hpl::sRGBToLinear(c.r);
    sl.color[1] = hpl::sRGBToLinear(c.g);
    sl.color[2] = hpl::sRGBToLinear(c.b);
    sl.intensity = pLight->GetIntensity();
    sl.radius = pLight->GetRadius();
    sl.sourceRadius = pLight->GetSourceRadius();
    sl.goboTextureIndex = m_global.resolveTextureSlot(cntx, pLight->GetGoboImage(),
                                             (uint32_t)RI.frameIndex);
    sl.shadowEnabled = pLight->GetCastShadows() ? 1u : 0u;
    const ml::float4x4 vpF4 =
        cMath::ToFloatTranspose4x4(pSpot->GetViewProjMatrix());
    std::memcpy(sl.viewProjection, vpF4.a, sizeof(sl.viewProjection));
    m_global.m_spotLightScratch[num_spot_lights++] = sl;
  }
  perFrame.spotLightCount = static_cast<uint32_t>(num_spot_lights);

  if (num_spot_lights > 0) {
    const size_t uploadBytes = num_spot_lights * sizeof(SpotLight);
    RIResourceBufferTransaction trans = {};
    trans.target = m_global.m_spotLightBuffer;
    trans.size = uploadBytes;
    trans.offset = 0;
    trans.currentState = RI_RESOURCE_STATE_UNORDERED_ACCESS;
    trans.currentStages = RI_STAGE_FRAGMENT | RI_STAGE_COMPUTE;
    trans.postState = RI_RESOURCE_STATE_UNORDERED_ACCESS;
    trans.postStages = RI_STAGE_FRAGMENT | RI_STAGE_COMPUTE;
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
    fa.color   = float3{hpl::sRGBToLinear(c.r),
                        hpl::sRGBToLinear(c.g),
                        hpl::sRGBToLinear(c.b)};
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
    RIResourceBufferTransaction trans = {};
    trans.target = m_global.m_fogAreaBuffer;
    trans.size = uploadBytes;
    trans.offset = 0;
    trans.currentState = RI_RESOURCE_STATE_UNORDERED_ACCESS;
    trans.currentStages = RI_STAGE_FRAGMENT | RI_STAGE_COMPUTE;
    trans.postState = RI_RESOURCE_STATE_UNORDERED_ACCESS;
    trans.postStages = RI_STAGE_FRAGMENT | RI_STAGE_COMPUTE;
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
    RIResourceBufferTransaction trans = {};
    trans.target = m_global.m_decalBuffer;
    trans.size = uploadBytes;
    trans.offset = 0;
    trans.currentState = RI_RESOURCE_STATE_UNORDERED_ACCESS;
    trans.currentStages = RI_STAGE_FRAGMENT | RI_STAGE_COMPUTE;
    trans.postState = RI_RESOURCE_STATE_UNORDERED_ACCESS;
    trans.postStages = RI_STAGE_FRAGMENT | RI_STAGE_COMPUTE;
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
      RIResourceBufferTransaction trans = {};
      trans.target = m_global.m_objectDecalIndexBuffer;
      trans.size = uploadBytes;
      trans.offset = 0;
      trans.currentState = RI_RESOURCE_STATE_UNORDERED_ACCESS;
      trans.currentStages = RI_STAGE_FRAGMENT;
      trans.postState = RI_RESOURCE_STATE_UNORDERED_ACCESS;
      trans.postStages = RI_STAGE_FRAGMENT;
      RI_ResourceBeginCopyBuffer(&RI.device, &RI.uploader, &trans);
      std::memcpy(trans.mapped.data, pool.data(), uploadBytes);
      RI_ResourceEndCopyBuffer(&RI.device, &RI.uploader, &trans);
    }
  }

  for (iRenderable *pObject : solids) {
    cMatrixf *pMtx = pObject->GetModelMatrix(apFrustum);
    cVertexBuffer *pVB = pObject->GetVertexBuffer();
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
    auto *vb = static_cast<cVertexBuffer *>(pVB);
    // BuildBlas submits the VB geometry first (internal SubmitToGPU), so
    // submitObject below sees the post-realloc index count — an in-loop realloc
    // that changes the triangle count must bump the slot generation this same
    // frame (anchored surfels with a now-out-of-range primitiveIndex go stale
    // before the OOB deref).
    vb->BuildBlas(&RI.blasSubmit.cmds[0], &RI.device, cntx);
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

    if (writtenDraws < indirectReq.numElements) {
      indirectDst[writtenDraws++] = RIDrawIndirectCommand{
          /*vertexCount   =*/(uint32_t)pVB->GetIndexNum(),
          /*instanceCount =*/1u,
          /*firstVertex   =*/0u,
          /*firstInstance =*/slot,
      };
    }

    // BLAS was recorded by BuildBlas above into the same primary cmd buffer;
    // the accel-build→accel-build barrier below guarantees the TLAS read sees
    // the BLAS writes.
    auto blas = vb->accelStructure();
    if (blas && !blas->isEmpty(&RI.renderer)) {
      // RIAccelInstanceDesc::transform is row-major 3x4 (transform[row][col]),
      // translation at [r][3]. modelF4 holds column-major storage (GLSL mat4
      // reading in gbuffer.vert), so index it as [col*4 + row] row-by-row.
      RIAccelInstanceDesc inst;
      for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
          inst.transform[r][c] = modelF4.a[c * 4 + r];
        }
      }
      inst.instanceCustomIndex = slot;
      inst.mask = kRayMaskOpaque;
      inst.flags = RI_ACCEL_INSTANCE_TRIANGLE_FLIP_FACING;
      const ShaderMaterialData &solidDesc = pMat->Descriptor();
      const bool dissolveFlags =
          pMat->GetImage(eMaterialTexture_DissolveAlpha) ||
          (solidDesc.m_id == MaterialID::SolidDiffuse &&
           solidDesc.m_solid.m_alphaDissolveFilter);
      if (!pMat->GetImage(eMaterialTexture_Alpha) &&
          pObject->GetCoverageAmount() >= 1.0f && !dissolveFlags)
        inst.flags |= RI_ACCEL_INSTANCE_FORCE_OPAQUE;
      inst.blas = blas.get();
      inst.blasIndex = blasIndexFor(blas.get());
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
    cVertexBuffer *pVB = pObj->GetVertexBuffer();
    if (!pVB || pVB->GetIndexNum() <= 0)
      continue;

    // VB upload + BLAS build already happened in the consolidated translucent
    // prepare loop near the top of Draw(); just pick up the cached BLAS here.
    auto *vbri = static_cast<cVertexBuffer *>(pVB);
    auto blas = vbri->accelStructure();
    if (!blas || blas->isEmpty(&RI.renderer))
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
    RIAccelInstanceDesc inst;
    for (int r = 0; r < 3; ++r) {
      for (int c = 0; c < 4; ++c) {
        inst.transform[r][c] = modelF4.a[c * 4 + r];
      }
    }
    inst.instanceCustomIndex = slot;
    inst.mask = kRayMaskTranslucent;
    inst.flags = RI_ACCEL_INSTANCE_TRIANGLE_FLIP_FACING;
    inst.blas = blas.get();
    inst.blasIndex = blasIndexFor(blas.get());
    tlasInstances.push_back(inst);
  }

  // ---------- TLAS build ----------
  // Walks the BLAS instances accumulated above and emits one TLAS build into
  // the primary cmd buffer. Also runs once with zero instances when no TLAS
  // exists yet (empty editor world): every RT descriptor push below requires
  // a valid handle, and rays into an empty TLAS just miss.
  if (!tlasInstances.empty() || m_tlas.isEmpty(&RI.renderer)) {
    const uint32_t instanceCount = (uint32_t)tlasInstances.size();

    // Persist the deduplicated BLAS set so the Metal RT pass can make them
    // resident at dispatch (only on a fresh gather; an empty gather keeps the
    // previous TLAS + its BLAS set).
    if (instanceCount > 0)
      m_tlasBlasList = tlasBlasList;

    auto destroyBuffer = [](RIBuffer *b) {
      if (IsRIBufferValid(&RI.renderer, b)) {
        RI.GetActiveSet()->freelist.push_back(*b);
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
      if (IsRIBufferValid(&RI.renderer, &m_tlasInstanceBuffer)) {
        cntx->freelist.push_back(m_tlasInstanceBuffer);
        m_tlasInstanceBuffer = {};
      }
      // Device-local: the instance buffer is a transfer destination written
      // each frame via the resource uploader. A persistent host mapping would
      // race the GPU's TLAS read for the previous frame still in flight.
      RIBufferDesc instDesc = {};
      instDesc.size = (uint64_t)newCap * RI_AccelInstanceStride(&RI.renderer);
      instDesc.usage = RI_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPT |
                       RI_BUFFER_USAGE_DEVICE_ADDRESS | RI_BUFFER_USAGE_TRANSFER_DST;
      instDesc.location = RI_MEMORY_DEVICE;
      instDesc.alignment = 16;
      m_tlasInstanceBuffer = RIBuffer::create(&RI.device, instDesc);
      m_tlasCapacity = newCap;
    }

    // Stage the instance array through the resource uploader so frame N+1's
    // write doesn't clobber the buffer mid-build for frame N. The uploader
    // owns the previous-use ↔ TRANSFER_WRITE ↔ next-use barrier pair.
    if (instanceCount > 0) {
      const uint32_t instanceStride = RI_AccelInstanceStride(&RI.renderer);
      RIResourceBufferTransaction trans = {};
      trans.target = m_tlasInstanceBuffer;
      trans.size = (size_t)instanceCount * instanceStride;
      trans.offset = 0;
      trans.currentState = RI_RESOURCE_STATE_ACCEL_READ;
      trans.currentStages = RI_STAGE_ACCEL_BUILD;
      trans.postState = RI_RESOURCE_STATE_ACCEL_READ;
      trans.postStages = RI_STAGE_ACCEL_BUILD;
      RI_ResourceBeginCopyBuffer(&RI.device, &RI.uploader, &trans);
      // Serialize each instance into the backend's instance-buffer layout
      // (Vulkan: VkAccelerationStructureInstanceKHR; Metal:
      // MTL::AccelerationStructureUserIDInstanceDescriptor).
      for (uint32_t i = 0; i < instanceCount; ++i)
        RI_WriteAccelInstance(&RI.renderer,
                              (char *)trans.mapped.data + (size_t)i * instanceStride,
                              &tlasInstances[i]);
      RI_ResourceEndCopyBuffer(&RI.device, &RI.uploader, &trans);
    }

    // BLAS builds are recorded into RI.blasSubmit (a separate command buffer
    // submitted + semaphore-synced ahead of the primary in CloseAndSubmitActiveSet),
    // so they are guaranteed complete before this primary buffer's TLAS build runs.
    // No inline accel-build→accel-build barrier is needed here.

    // Size the TLAS for the worst-case instance count we've seen. Re-init when
    // the instance count exceeds what the current TLAS storage was sized for.
    RIAccelStructureDesc tlasDesc = {};
    tlasDesc.type = RI_ACCEL_STRUCTURE_TYPE_TOP_LEVEL;
    tlasDesc.flags = RI_ACCEL_BUILD_PREFER_FAST_TRACE;
    tlasDesc.geometryOrInstanceNum = instanceCount;

    uint64_t tlasStorageSize = 0;
    uint64_t tlasBuildScratch = 0;
    tlasDesc.getMemoryReqs(&RI.device, &tlasStorageSize, &tlasBuildScratch,
                           nullptr);

    if (m_tlas.isEmpty(&RI.renderer) ||
        (!IsRIBufferValid(&RI.renderer, &m_tlasStorage) || tlasStorageSize > m_tlasStorageCapacity)) {
      if (!m_tlas.isEmpty(&RI.renderer)) {
        cntx->freelist.push_back(m_tlas);
        cntx->freelist.push_back(m_tlasStorage);
        m_tlas = {};
      }
      RIBufferDesc storageDesc = {};
      storageDesc.size = tlasStorageSize;
      storageDesc.usage = RI_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE |
                          RI_BUFFER_USAGE_DEVICE_ADDRESS;
      storageDesc.location = RI_MEMORY_DEVICE;
      m_tlasStorage = RIBuffer::create(&RI.device, storageDesc);
      tlasDesc.storage = &m_tlasStorage;
      tlasDesc.storageOffset = 0;
      tlasDesc.storageSize = tlasStorageSize;
      if (m_tlas.init(&RI.device, &tlasDesc) != RI_SUCCESS) {
        // Leave m_tlas zeroed; skip the build this frame.
        m_tlas = {};
      }
      m_tlasStorageCapacity = tlasStorageSize;
    }

    if (!m_tlas.isEmpty(&RI.renderer)) {
      // Source TLAS build scratch from the per-frame accel pool. The pool
      // recycles its blocks across frames and uses the oversized one-shot
      // path for builds that exceed blockSize. RIBlockMem embeds an
      // RIBuffer, so we hand its address straight to the build desc.
      RIBufferScratchAllocReq scratchReq = RIAllocBufferFromScratchAlloc(
          &RI.device, &cntx->accelScratchAlloc, tlasBuildScratch);

      RIBuildTlasDesc build = {};
      build.dst = &m_tlas;
      build.src = nullptr;
      build.mode = RI_ACCEL_BUILD_MODE_BUILD;
      build.instanceNum = instanceCount;
      build.instanceBuffer = &m_tlasInstanceBuffer;
      build.instanceOffset = 0;
      build.scratchBuffer = &scratchReq.block.buffer;
      build.scratchOffset = scratchReq.bufferOffset;
      // Metal: the BLASes instances reference by blasIndex. Vulkan ignores these.
      build.instanceBlases = tlasBlasList.data();
      build.instanceBlasNum = (uint32_t)tlasBlasList.size();
      RI.primary.cmds[0].buildTlas(&RI.device, &build, 1);

      // Consumed by both the RT pipelines and fragment-stage ray queries —
      // one barrier covers both.
      RI.primary.cmds[0].vk_d3d12_memoryBarrier(
          {RI_RESOURCE_STATE_ACCEL_WRITE, RI_RESOURCE_STATE_ACCEL_READ,
           RI_STAGE_ACCEL_BUILD, RI_STAGE_FRAGMENT | RI_STAGE_RAY_TRACING});
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
  auto pushBinding = [&](const char *name, const RIDescriptor &desc,
                         uint32_t registerOffset = 0) {
    RIProgram::DescriptorBinding b;
    b.handle = DescriptorBindingID::Create(name);
    b.registerOffset = registerOffset;
    b.descriptor = desc;
    bindings.push_back(b);
  };

  // Per-dispatch helpers for the set-1 surfel image / TLAS pushes —
  // `gPackedHitInfo` / `gSurfelDepthMap` are
  // RWTexture2D (GENERAL layout, storage image), `gSurfelDepth` is the
  // sampled view of the same depth image (still GENERAL since the
  // image stays GENERAL across the frame and GENERAL satisfies both
  // storage + sampled access). `gRtAccel` is the freshly built TLAS.
  // Each helper appends to a local std::vector<DescriptorBinding> so
  // multiple shaders can mix-and-match the subset they need.
  auto pushSurfelStorageImage =
      [&](std::vector<RIProgram::DescriptorBinding> &v, const char *name,
          const RITextureView &view) {
        RIProgram::DescriptorBinding b;
        b.handle = DescriptorBindingID::Create(name);
        // Transient per-frame render-target view: no stable resource id yet, so
        // hash_random() (loses cross-frame set caching — see cookie follow-up).
        b.descriptor = RIDescriptor::storageImage(
            &RI.device, const_cast<RITextureView *>(&view), hash_random());
        v.push_back(b);
      };
  auto pushSurfelSampledImage =
      [&](std::vector<RIProgram::DescriptorBinding> &v, const char *name,
          const RITextureView &view) {
        RIProgram::DescriptorBinding b;
        b.handle = DescriptorBindingID::Create(name);
        // Sampled but bound GENERAL (image stays GENERAL across the frame).
        b.descriptor = RIDescriptor::sampledImage(
            &RI.device, const_cast<RITextureView *>(&view), hash_random(),
            RI_RESOURCE_STATE_GENERAL);
        v.push_back(b);
      };
  auto pushTlas = [&](std::vector<RIProgram::DescriptorBinding> &v) {
    RIProgram::DescriptorBinding b;
    b.handle = DescriptorBindingID::Create("gRtAccel");
    b.descriptor =
        RIDescriptor::accelerationStructure(&RI.device, &m_tlas, hash_random());
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
    RITextureBarrier attachmentBarriers[3] = {
        {&state.visibilityTexture[RI.swapchainIndex],
         RI_RESOURCE_STATE_UNDEFINED, RI_RESOURCE_STATE_RENDER_TARGET},
        {&state.depthTextures[RI.swapchainIndex], RI_RESOURCE_STATE_UNDEFINED,
         RI_RESOURCE_STATE_DEPTH_WRITE, RI_STAGE_NONE, RI_STAGE_NONE,
         RI_BARRIER_ASPECT_DEPTH},
        // Velocity MRT — same UNDEFINED→COLOR transition as the visibility
        // target (loadOp=CLEAR, so prior contents don't matter).
        {&state.velocityTexture[RI.swapchainIndex], RI_RESOURCE_STATE_UNDEFINED,
         RI_RESOURCE_STATE_RENDER_TARGET}};
    RI.primary.cmds[0].vk_d3d12_textureBarriers<3>(3, attachmentBarriers);
  }

  RIRenderingAttachment colorAttachment = {};
  colorAttachment.view = &state.visibilityView[RI.swapchainIndex];
  colorAttachment.loadOp = RI_ATTACHMENT_LOAD_OP_CLEAR;
  colorAttachment.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
  // Cleared to all-zero. psMain writes .w=0 for hit; the depth test at the
  // composite gates miss pixels independently, so no miss sentinel needed.

  // Velocity MRT (SV_TARGET1). Cleared to 0 → static/uncovered pixels read zero
  // motion.
  RIRenderingAttachment velocityAttachment = {};
  velocityAttachment.view = &state.velocityView[RI.swapchainIndex];
  velocityAttachment.loadOp = RI_ATTACHMENT_LOAD_OP_CLEAR;
  velocityAttachment.storeOp = RI_ATTACHMENT_STORE_OP_STORE;

  const RIRenderingAttachment gbufferColorAttachments[2] = {
      colorAttachment, velocityAttachment};

  // MRT owns the per-frame depth clear.
  RIRenderingAttachment depthAttachment = {};
  depthAttachment.view = &state.depthView[RI.swapchainIndex];
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
    const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    m_surfelPrepare.bindComputePipeline(&RI.device, &RI.primary.cmds[0],
                                        kHash, "SurfelPreparePass.cs",
                                        RIComputePipelineDesc{});
    m_surfelPrepare.bindExternalSet(
        &RI.primary.cmds[0], &m_global.m_bindlessSet, 0,
        RI_PIPELINE_BIND_COMPUTE);
    RI.primary.cmds[0].dispatch(&RI.renderer, 1u, 1u, 1u);
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
    // Metal needs the prepare-pass compute encoder closed before a blit opens,
    // and the blit closed before the next dispatch (both no-ops on Vulkan).
    RI.primary.cmds[0].mtl_encoderEnd();
    RI.primary.cmds[0].copyBuffer(&RI.renderer, 
        &m_global.m_surfelValidBuffer, 0, &m_global.m_surfelDirtyIndexBuffer, 0,
        (RIDeviceSize)kTotalSurfelLimit * sizeof(uint32_t));
    RI.primary.cmds[0].mtl_encoderEnd();
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
         RI_STAGE_COMPUTE, RI_STAGE_COMPUTE | RI_STAGE_RAY_TRACING});
  }

  // ----------------------------------------------------------------------
  // Stage B — primary-ray VBuffer.
  //
  // RT pipeline that traces one ray per swapchain pixel through m_tlas and
  // packs the closest-hit (instanceCustomIndex, primitiveID, attribs) into
  // state.packedHitInfoTexture. Stages D/F consume this image; for the current
  // frame the output goes unused so the dispatch's correctness needs to be
  // verified through validation-layer signals (no SBT/descriptor errors,
  // no VK_ERROR_DEVICE_LOST).
  // ----------------------------------------------------------------------
  {
    // Primary V-buffer: UNDEFINED -> GENERAL for the RT pipeline to store into.
    // (No bounce buffers to clear anymore — water/glass refraction clobbers this
    // primary image, water reflection is in the raster water pass.)
    RI.primary.cmds[0].vk_d3d12_textureBarrier(
        {&state.packedHitInfoTexture[RI.swapchainIndex],
         RI_RESOURCE_STATE_UNDEFINED, RI_RESOURCE_STATE_STORAGE_WRITE,
         RI_STAGE_NONE, RI_STAGE_RAY_TRACING});
  }

  if (!m_tlas.isEmpty(&RI.renderer)) {
    RIRayTracingPipelineDesc rtDesc;
    // closeHit fires one recursive TraceRay before recording the second hit,
    // so the pipeline needs depth=2 (raygen→chit = depth 1, the recursive
    // TraceRay lands at depth 2). depth=1 silently leaves hit pixels unwritten
    // — "holes" in gPackedHitInfo against the UNDEFINED initial contents.
    rtDesc.maxRecursionDepth = 2;
    const hash_t kVBufferHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    m_surfelVBuffer.bindRayTracingPipeline(&RI.device, &RI.primary.cmds[0],
                                           kVBufferHash, "SurfelVBuffer.rt",
                                           rtDesc);
    m_surfelVBuffer.bindExternalSet(
        &RI.primary.cmds[0], &m_global.m_bindlessSet, 0,
        RI_PIPELINE_BIND_RAY_TRACING);

    std::vector<RIProgram::DescriptorBinding> vbBindings;
    vbBindings.reserve(3);
    {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("gPerFrame");
      RI.UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
      vbBindings.push_back(b);
    }
    pushSurfelStorageImage(vbBindings, "gPackedHitInfo",
                           state.packedHitInfoView[RI.swapchainIndex]);
    pushTlas(vbBindings);

    m_surfelVBuffer.bindDescriptors(
        &RI.device, &RI.primary.cmds[0], RI.frameIndex, vbBindings.data(),
        vbBindings.size(), RI_PIPELINE_BIND_RAY_TRACING);

    m_surfelVBuffer.traceRays(&RI.primary.cmds[0], kVBufferHash,
                              renderWidth, renderHeight, 1u);
  }

  {
    // state.packedHitInfoTexture is written by the rgen + miss + chit triplet
    // above. Transition it to SHADER_READ_ONLY_OPTIMAL so consumers can
    // Downstream consumers (SurfelEvaluation / SurfelGeneration /
    // SurfelGIRender) read gPackedHitInfo via direct `[pixel]` storage
    // loads on the bindless RWTexture2D — layout must stay GENERAL
    // through the frame, so this is just a memory/execution sync, not
    // a layout transition. The bindless descriptor was written with
    // VK_IMAGE_LAYOUT_GENERAL at frame start.
    RI.primary.cmds[0].vk_d3d12_memoryBarrier(
        {RI_RESOURCE_STATE_STORAGE_WRITE, RI_RESOURCE_STATE_STORAGE_READ,
         RI_STAGE_RAY_TRACING,
         RI_STAGE_COMPUTE | RI_STAGE_FRAGMENT | RI_STAGE_RAY_TRACING});
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
    const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    m_surfelUpdateCollect.bindComputePipeline(
        &RI.device, &RI.primary.cmds[0], kHash, "SurfelUpdatePass.cs:collectCellInfo",
        RIComputePipelineDesc{});
    m_surfelUpdateCollect.bindExternalSet(
        &RI.primary.cmds[0], &m_global.m_bindlessSet, 0,
        RI_PIPELINE_BIND_COMPUTE);

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
        bnd.size(), RI_PIPELINE_BIND_COMPUTE);

    RI.primary.cmds[0].dispatch(&RI.renderer, (kTotalSurfelLimit + 31u) / 32u, 1u, 1u);
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
    const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    m_surfelUpdateAccumulate.bindComputePipeline(
        &RI.device, &RI.primary.cmds[0], kHash, "SurfelUpdatePass.cs:accumulateCellInfo",
        RIComputePipelineDesc{});
    m_surfelUpdateAccumulate.bindExternalSet(
        &RI.primary.cmds[0], &m_global.m_bindlessSet, 0,
        RI_PIPELINE_BIND_COMPUTE);
    const uint32_t groups = (kCellDimension + 3u) / 4u;
    RI.primary.cmds[0].dispatch(&RI.renderer, groups, groups, groups);
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
    const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    m_surfelUpdateScatter.bindComputePipeline(
        &RI.device, &RI.primary.cmds[0], kHash, "SurfelUpdatePass.cs:updateCellToSurfelBuffer",
        RIComputePipelineDesc{});
    m_surfelUpdateScatter.bindExternalSet(
        &RI.primary.cmds[0], &m_global.m_bindlessSet, 0,
        RI_PIPELINE_BIND_COMPUTE);

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
        bnd.size(), RI_PIPELINE_BIND_COMPUTE);

    RI.primary.cmds[0].dispatch(&RI.renderer, (kTotalSurfelLimit + 31u) / 32u, 1u, 1u);
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
    const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    m_lightGridBin.bindComputePipeline(&RI.device, &RI.primary.cmds[0], kHash,
                                       "LightGridBuildPass.cs:binLights",
                                       RIComputePipelineDesc{});
    m_lightGridBin.bindExternalSet(&RI.primary.cmds[0], &m_global.m_bindlessSet,
                                             0, RI_PIPELINE_BIND_COMPUTE);
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
                                   RI_PIPELINE_BIND_COMPUTE);
    // One thread per grid cell (the shader early-outs past kLightGridCellCount).
    RI.primary.cmds[0].dispatch(&RI.renderer, (kLightGridCellCount + 63u) / 64u, 1u, 1u);
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
    RITextureBarrier toClear[1] = {
        {&m_surfelDepthTexture[RI.swapchainIndex],
         RI_RESOURCE_STATE_UNDEFINED, RI_RESOURCE_STATE_CLEAR_STORAGE}};
    RI.primary.cmds[0].vk_d3d12_textureBarriers<1>(1, toClear);

    const float depthClear[4] = {0.0f,       // E[z]
                                 60000.0f,   // E[z^2] seeded high (half-float safe)
                                 0.0f, 0.0f};
    RI.primary.cmds[0].clearStorageImage(&RI.renderer, &m_surfelDepthTexture[RI.swapchainIndex],
                                         depthClear);

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
  // gMaxRayCount = 64; the dispatched width is kRayBudget = 9.6M, the rgen
  // early-outs past RequestedRay so unused slots cost nothing.
  // ----------------------------------------------------------------------
  if (!m_tlas.isEmpty(&RI.renderer)) {
    RIRayTracingPipelineDesc rtDesc;
    rtDesc.maxRecursionDepth = 1;
    const hash_t kRtHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    m_surfelRT.bindRayTracingPipeline(&RI.device, &RI.primary.cmds[0],
                                      kRtHash, "SurfelRayTrace.rt", rtDesc);
    m_surfelRT.bindExternalSet(
        &RI.primary.cmds[0], &m_global.m_bindlessSet, 0,
        RI_PIPELINE_BIND_RAY_TRACING);

    std::vector<RIProgram::DescriptorBinding> rtBnd;
    rtBnd.reserve(2);
    {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("gPerFrame");
      RI.UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
      rtBnd.push_back(b);
    }
    pushTlas(rtBnd);
    m_surfelRT.bindDescriptors(
        &RI.device, &RI.primary.cmds[0], RI.frameIndex, rtBnd.data(),
        rtBnd.size(), RI_PIPELINE_BIND_RAY_TRACING);

    // The TLAS is resident via its gRtAccel binding, but Metal also needs the
    // BLASes it references resident for argument-buffer traversal (no-op on VK).
    RI.primary.cmds[0].mtl_useAccelStructuresResident(
        m_tlasBlasList.data(), (uint32_t)m_tlasBlasList.size());

    m_surfelRT.traceRays(&RI.primary.cmds[0], kRtHash, kRayBudget, 1u, 1u);
  }
  {
    RI.primary.cmds[0].vk_d3d12_memoryBarrier(
        {RI_RESOURCE_STATE_STORAGE_WRITE, RI_RESOURCE_STATE_STORAGE_READ,
         RI_STAGE_RAY_TRACING, RI_STAGE_COMPUTE | RI_STAGE_FRAGMENT});
  }

  RIBeginRenderingDesc renderingInfo = {};
  renderingInfo.renderArea.width = (int16_t)renderWidth;
  renderingInfo.renderArea.height = (int16_t)renderHeight;
  renderingInfo.colorCount = 2;
  renderingInfo.colors = gbufferColorAttachments;
  renderingInfo.depthStencil = &depthAttachment;
  RI.primary.cmds[0].vk_d3d12_beginRendering(&RI.renderer, renderingInfo);
  RI.primary.cmds[0].mtl_encoderDraw(renderingInfo);

  RIViewport vkViewport = {};
  vkViewport.y = (float)renderHeight;
  vkViewport.width = (float)renderWidth;
  vkViewport.height = -(float)renderHeight;
  vkViewport.depthMax = 1.0f;
  RIRect scissor = {};
  scissor.width = (int16_t)renderWidth;
  scissor.height = (int16_t)renderHeight;
  RI.primary.cmds[0].setViewport(&RI.renderer, vkViewport);
  RI.primary.cmds[0].setScissor(&RI.renderer, scissor);

  if (writtenDraws > 0) {
    GBufferMRTPipelineDesc pipelineDesc(RIBootstrap::VisibilityFormat,
                                        RIBootstrap::VelocityFormat,
                                        RIBootstrap::DepthFormat);
    m_gbuffer.bindPipeline(&RI.device, &RI.primary.cmds[0], pipelineDesc.hash,
                           "SurfelGBuffer.3d", pipelineDesc.desc);
    m_gbuffer.bindExternalSet(&RI.primary.cmds[0], &m_global.m_bindlessSet, 0);
    m_gbuffer.bindDescriptors(&RI.device, &RI.primary.cmds[0], RI.frameIndex,
                              bindings.data(), bindings.size());
    RI.primary.cmds[0].drawIndirect(&RI.renderer, 
        &m_indirectDrawBuffer,
        (RIDeviceSize)indirectReq.elementOffset * sizeof(RIDrawIndirectCommand),
        writtenDraws, (uint32_t)sizeof(RIDrawIndirectCommand));
  }

  RI.primary.cmds[0].mtl_encoderEnd();

  RI.primary.cmds[0].vk_d3d12_endRendering(&RI.renderer);

  // Gbuffer output -> SHADER_READ_ONLY for the surfel-generation compute
  // pass (and any later fragment consumer). Includes depth, which the
  // gbuffer left in DEPTH_STENCIL_ATTACHMENT_OPTIMAL. The surfel result
  // image transitions UNDEFINED -> GENERAL for its first compute write.
  {
    RITextureBarrier toRead[4] = {};
    // Visibility -> SHADER_READ for the fragment + compute consumers.
    toRead[0] = {&state.visibilityTexture[RI.swapchainIndex],
                 RI_RESOURCE_STATE_RENDER_TARGET,
                 RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_NONE,
                 RI_STAGE_FRAGMENT | RI_STAGE_COMPUTE};

    // Depth -> SHADER_READ_ONLY for the compute pass.
    toRead[1] = {&state.depthTextures[RI.swapchainIndex],
                 RI_RESOURCE_STATE_DEPTH_WRITE,
                 RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_NONE,
                 RI_STAGE_COMPUTE, RI_BARRIER_ASPECT_DEPTH};

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
    toRead[2] = {&state.surfelResultTexture[RI.swapchainIndex],
                 RI_RESOURCE_STATE_UNDEFINED, RI_RESOURCE_STATE_CLEAR_STORAGE};

    // Velocity (gbuffer MRT) -> SHADER_READ for the direct-lighting pass.
    toRead[3] = {&state.velocityTexture[RI.swapchainIndex],
                 RI_RESOURCE_STATE_RENDER_TARGET,
                 RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_NONE,
                 RI_STAGE_COMPUTE};

    RI.primary.cmds[0].vk_d3d12_textureBarriers<4>(4, toRead);
  }

  // Clear the surfel-result image to zero indirect (see toRead[2] above), then
  // make the clear visible to surfel_generation_pass's storage write.
  {
    const float clearColor[4] = {0.0f, 0.0f, 0.0f,
                                 1.0f}; // (0,0,0,1): zero radiance, opaque alpha
    RI.primary.cmds[0].clearStorageImage(&RI.renderer, 
        &state.surfelResultTexture[RI.swapchainIndex], clearColor);

    RI.primary.cmds[0].vk_d3d12_textureBarrier(
        {&state.surfelResultTexture[RI.swapchainIndex],
         RI_RESOURCE_STATE_CLEAR_STORAGE, RI_RESOURCE_STATE_UNORDERED_ACCESS,
         RI_STAGE_NONE, RI_STAGE_COMPUTE});
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
    const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    m_surfelIntegrate.bindComputePipeline(&RI.device, &RI.primary.cmds[0],
                                          kHash, "SurfelIntegratePass.cs",
                                          RIComputePipelineDesc{});
    m_surfelIntegrate.bindExternalSet(
        &RI.primary.cmds[0], &m_global.m_bindlessSet, 0,
        RI_PIPELINE_BIND_COMPUTE);

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
    pushSurfelStorageImage(bnd, "gSurfelDepthMap",
                           m_surfelDepthView[RI.swapchainIndex]);
    pushSurfelSampledImage(bnd, "gSurfelDepth",
                           m_surfelDepthView[RI.swapchainIndex]);
    m_surfelIntegrate.bindDescriptors(
        &RI.device, &RI.primary.cmds[0], RI.frameIndex, bnd.data(),
        bnd.size(), RI_PIPELINE_BIND_COMPUTE);
    RI.primary.cmds[0].dispatch(&RI.renderer, (kTotalSurfelLimit + 31u) / 32u, 1u, 1u);
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
    const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    m_surfelGenerate.bindComputePipeline(&RI.device, &RI.primary.cmds[0],
                                         kHash, "SurfelGenerationPass.cs",
                                         RIComputePipelineDesc{});
    m_surfelGenerate.bindExternalSet(
        &RI.primary.cmds[0], &m_global.m_bindlessSet, 0,
        RI_PIPELINE_BIND_COMPUTE);

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
                           state.packedHitInfoView[RI.swapchainIndex]);
    pushSurfelSampledImage(bnd, "gSurfelDepth",
                           m_surfelDepthView[RI.swapchainIndex]);
    pushSurfelStorageImage(bnd, "gOutput",
                           state.surfelResultView[RI.swapchainIndex]);
    m_surfelGenerate.bindDescriptors(
        &RI.device, &RI.primary.cmds[0], RI.frameIndex, bnd.data(),
        bnd.size(), RI_PIPELINE_BIND_COMPUTE);

    const uint32_t fullW = renderWidth;
    const uint32_t fullH = renderHeight;
    RI.primary.cmds[0].dispatch(&RI.renderer, (fullW + 15u) / 16u, (fullH + 15u) / 16u, 1u);
  }

  // --------------------------------------------------------------------
  // Direct-lighting pass — soft-shadowed analytic direct lighting, temporally
  // accumulated through the velocity texture, into the ping-pong direct texture
  // the composite samples. The V-buffer (gPackedHitInfo), raster fallback, and
  // velocity are all ready by here.
  // --------------------------------------------------------------------
  // The image the composite samples for direct lighting: the raw accumulation
  // when à-trous is disabled, else the final à-trous iteration's output.
  const RITextureView *directResultView = nullptr;
  {
    const uint32_t dlCur  = state.directLightingIndex;
    const uint32_t dlPrev = dlCur ^ 1u;

    if (!state.directLightingInit) {
      // First use: the colour + key ping-pong textures UNDEFINED -> GENERAL +
      // cleared so the history reads are defined; they stay GENERAL thereafter.
      RITexture *dirImgs[9] = {
          &state.directLightingTexture[0], &state.directLightingTexture[1],
          &state.directKeyTexture[0],      &state.directKeyTexture[1],
          &state.directAtrousTexture[0],   &state.directAtrousTexture[1],
          &state.reservoirTexture[0],      &state.reservoirTexture[1],
          &state.reservoirTemporalTexture};
      RITextureBarrier toGen[9] = {
          {&state.directLightingTexture[0], RI_RESOURCE_STATE_UNDEFINED,
           RI_RESOURCE_STATE_CLEAR_STORAGE},
          {&state.directLightingTexture[1], RI_RESOURCE_STATE_UNDEFINED,
           RI_RESOURCE_STATE_CLEAR_STORAGE},
          {&state.directKeyTexture[0], RI_RESOURCE_STATE_UNDEFINED,
           RI_RESOURCE_STATE_CLEAR_STORAGE},
          {&state.directKeyTexture[1], RI_RESOURCE_STATE_UNDEFINED,
           RI_RESOURCE_STATE_CLEAR_STORAGE},
          {&state.directAtrousTexture[0], RI_RESOURCE_STATE_UNDEFINED,
           RI_RESOURCE_STATE_CLEAR_STORAGE},
          {&state.directAtrousTexture[1], RI_RESOURCE_STATE_UNDEFINED,
           RI_RESOURCE_STATE_CLEAR_STORAGE},
          {&state.reservoirTexture[0], RI_RESOURCE_STATE_UNDEFINED,
           RI_RESOURCE_STATE_CLEAR_STORAGE},
          {&state.reservoirTexture[1], RI_RESOURCE_STATE_UNDEFINED,
           RI_RESOURCE_STATE_CLEAR_STORAGE},
          {&state.reservoirTemporalTexture, RI_RESOURCE_STATE_UNDEFINED,
           RI_RESOURCE_STATE_CLEAR_STORAGE}};
      RI.primary.cmds[0].vk_d3d12_textureBarriers<9>(9, toGen);

      const float clr[4] = {0.0f, 0.0f, 0.0f, 0.0f};
      for (uint32_t i = 0; i < 9; ++i)
        RI.primary.cmds[0].clearStorageImage(&RI.renderer, dirImgs[i], clr);

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

    const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    m_directLighting.bindComputePipeline(&RI.device, &RI.primary.cmds[0], kHash,
                                         "DirectLightingPass.cs", RIComputePipelineDesc{});
    m_directLighting.bindExternalSet(
        &RI.primary.cmds[0], &m_global.m_bindlessSet, 0,
        RI_PIPELINE_BIND_COMPUTE);

    // `st` selects the image layout (GENERAL vs SHADER_READ_ONLY); Metal ignores
    // it and resolves the resource from the view. hash_random(): transient view.
    std::vector<RIProgram::DescriptorBinding> bnd;
    bnd.reserve(8);
    {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("gPerFrame");
      RI.UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
      bnd.push_back(b);
    }
    // Temporal pass traces no rays — only builds + reprojects reservoirs.
    pushSurfelStorageImage(bnd, "gPackedHitInfo",
                           state.packedHitInfoView[RI.swapchainIndex]);
    bnd.emplace_back("gPackedHitInfoRaster",
                     RIDescriptor::sampledImage(
                         &RI.device, &state.visibilityView[RI.swapchainIndex],
                         hash_random(), RI_RESOURCE_STATE_SHADER_RESOURCE));
    bnd.emplace_back("gVelocity",
                     RIDescriptor::sampledImage(
                         &RI.device, &state.velocityView[RI.swapchainIndex],
                         hash_random(), RI_RESOURCE_STATE_SHADER_RESOURCE));
    bnd.emplace_back("gReservoirHistory",
                     RIDescriptor::sampledImage(
                         &RI.device, &state.reservoirView[dlPrev],
                         hash_random(), RI_RESOURCE_STATE_UNORDERED_ACCESS));
    bnd.emplace_back("gDirectKeyHistory",
                     RIDescriptor::sampledImage(
                         &RI.device, &state.directKeyView[dlPrev],
                         hash_random(), RI_RESOURCE_STATE_UNORDERED_ACCESS));
    pushSurfelStorageImage(bnd, "gReservoirOut",
                           state.reservoirTemporalView);
    pushSurfelStorageImage(bnd, "gDirectKeyOut",
                           state.directKeyView[dlCur]);

    m_directLighting.bindDescriptors(&RI.device, &RI.primary.cmds[0],
                                     RI.frameIndex, bnd.data(), bnd.size(),
                                     RI_PIPELINE_BIND_COMPUTE);
    RI.primary.cmds[0].dispatch(&RI.renderer, (renderWidth + 15u) / 16u,
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
      m_directSpatialReuse.bindComputePipeline(
          &RI.device, &RI.primary.cmds[0], kHash, "DirectSpatialReusePass.cs",
          RIComputePipelineDesc{});
      m_directSpatialReuse.bindExternalSet(
          &RI.primary.cmds[0], &m_global.m_bindlessSet, 0,
          RI_PIPELINE_BIND_COMPUTE);

      std::vector<RIProgram::DescriptorBinding> sb;
      sb.reserve(8);
      {
        RIProgram::DescriptorBinding b;
        b.handle = DescriptorBindingID::Create("gPerFrame");
        RI.UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
        sb.push_back(b);
      }
      pushSurfelStorageImage(sb, "gPackedHitInfo",
                             state.packedHitInfoView[RI.swapchainIndex]);
      pushTlas(sb);  // resolve shadow ray
      sb.emplace_back("gPackedHitInfoRaster",
                      RIDescriptor::sampledImage(
                          &RI.device, &state.visibilityView[RI.swapchainIndex],
                          hash_random(), RI_RESOURCE_STATE_SHADER_RESOURCE));
      sb.emplace_back("gReservoirIn",
                      RIDescriptor::sampledImage(
                          &RI.device, &state.reservoirTemporalView,
                          hash_random(), RI_RESOURCE_STATE_UNORDERED_ACCESS));
      sb.emplace_back("gDirectKey",
                      RIDescriptor::sampledImage(
                          &RI.device, &state.directKeyView[dlCur],
                          hash_random(), RI_RESOURCE_STATE_UNORDERED_ACCESS));
      sb.emplace_back("gVelocity",
                      RIDescriptor::sampledImage(
                          &RI.device, &state.velocityView[RI.swapchainIndex],
                          hash_random(), RI_RESOURCE_STATE_SHADER_RESOURCE));
      sb.emplace_back("gDirectHistory",
                      RIDescriptor::sampledImage(
                          &RI.device, &state.directLightingView[dlPrev],
                          hash_random(), RI_RESOURCE_STATE_UNORDERED_ACCESS));
      sb.emplace_back("gDirectKeyHistory",
                      RIDescriptor::sampledImage(
                          &RI.device, &state.directKeyView[dlPrev],
                          hash_random(), RI_RESOURCE_STATE_UNORDERED_ACCESS));
      pushSurfelStorageImage(sb, "gReservoirOut",
                             state.reservoirView[dlCur]);
      pushSurfelStorageImage(sb, "gDirectLighting",
                             state.directLightingView[dlCur]);

      m_directSpatialReuse.bindDescriptors(&RI.device, &RI.primary.cmds[0],
                                           RI.frameIndex, sb.data(), sb.size(),
                                           RI_PIPELINE_BIND_COMPUTE);
      RI.primary.cmds[0].dispatch(&RI.renderer, (renderWidth + 15u) / 16u,
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
    // reads the accumulation (directLighting[dlCur]); later iterations ping-pong
    // the directAtrous scratch. All textures stay GENERAL.
    // ----------------------------------------------------------------
    directResultView = &state.directLightingView[dlCur];
    for (int it = 0; it < kAtrousIterations; ++it) {
      const RITextureView &inView = (it == 0)
          ? state.directLightingView[dlCur]
          : state.directAtrousView[(it - 1) & 1];
      const uint32_t outIdx  = static_cast<uint32_t>(it) & 1u;
      const RITextureView &outView = state.directAtrousView[outIdx];

      m_directAtrous.bindComputePipeline(&RI.device, &RI.primary.cmds[0], kHash,
                                         "DirectAtrousPass.cs", RIComputePipelineDesc{});
      m_directAtrous.bindExternalSet(
          &RI.primary.cmds[0], &m_global.m_bindlessSet, 0,
          RI_PIPELINE_BIND_COMPUTE);

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
      pushSurfelSampledImage(ab, "gAtrousIn", inView);
      pushSurfelSampledImage(ab, "gDirectKey",
                             state.directKeyView[dlCur]);
      pushSurfelStorageImage(ab, "gAtrousOut", outView);

      m_directAtrous.bindDescriptors(&RI.device, &RI.primary.cmds[0],
                                     RI.frameIndex, ab.data(), ab.size(),
                                     RI_PIPELINE_BIND_COMPUTE);
      RI.primary.cmds[0].dispatch(&RI.renderer, (renderWidth + 15u) / 16u,
                                  (renderHeight + 15u) / 16u, 1u);

      // This iteration's write -> next iteration's / composite's sampled read.
      RI.primary.cmds[0].vk_d3d12_memoryBarrier(
          {RI_RESOURCE_STATE_STORAGE_WRITE, RI_RESOURCE_STATE_SHADER_RESOURCE,
           RI_STAGE_COMPUTE, RI_STAGE_COMPUTE});
      directResultView = &state.directAtrousView[outIdx];
    }
  }

  // --------------------------------------------------------------------
  // MainCompositePass — compute pass. Reads gIndirectLighting
  // (state.surfelResultView, from SurfelGenerationPass above) + gPackedHitInfo /
  // gPackedHitInfoRaster / TLAS / gPerFrame, and writes the composited color
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
    RIMemoryBarrier mem = {
        RI_RESOURCE_STATE_STORAGE_WRITE,
        RI_RESOURCE_STATE_SHADER_RESOURCE | RI_RESOURCE_STATE_STORAGE_READ,
        RI_STAGE_COMPUTE, RI_STAGE_COMPUTE};

    RITextureBarrier imageBarriers[2] = {
        // gIndirectLighting GENERAL -> SHADER_READ_ONLY, now consumed by
        // compute.
        {&state.surfelResultTexture[RI.swapchainIndex],
         RI_RESOURCE_STATE_STORAGE_WRITE, RI_RESOURCE_STATE_SHADER_RESOURCE,
         RI_STAGE_COMPUTE, RI_STAGE_COMPUTE},
        // Pogo attach -> GENERAL for the compute storage write. Discard prior
        // contents (UNDEFINED): the dispatch writes every pixel, matching the
        // old fragment pass's LOAD_OP_DONT_CARE. This also covers the
        // first-frame init for the attach half.
        {&state.renderTarget[RI.swapchainIndex], RI_RESOURCE_STATE_UNDEFINED,
         RI_RESOURCE_STATE_STORAGE_WRITE, RI_STAGE_NONE, RI_STAGE_COMPUTE}};

    RI.primary.cmds[0].vk_d3d12_resourceBarrier<1, 0, 2>(1, &mem, 0, NULL, 2,
                                                imageBarriers);
  }

  // Depth flip shared by the decal pre-pass (below) and the particle /
  // translucent passes further down: depth arrives in SHADER_READ_ONLY from
  // surfel-generate and flips once to DEPTH_READ_ONLY for any depth-tested pass.
  bool depthFlippedForReadOnly = false;
  auto flipDepthToReadOnly = [&]() {
    if (depthFlippedForReadOnly)
      return;
    RI.primary.cmds[0].vk_d3d12_textureBarrier(
        {&state.depthTextures[RI.swapchainIndex],
         RI_RESOURCE_STATE_SHADER_RESOURCE, RI_RESOURCE_STATE_DEPTH_READ,
         RI_STAGE_FRAGMENT | RI_STAGE_COMPUTE, RI_STAGE_NONE,
         RI_BARRIER_ASPECT_DEPTH});
    depthFlippedForReadOnly = true;
  };


  // Surfel-GI compute pass — one thread per pixel writes the composite into the
  // pogo attach bound as gOutput (storage image, GENERAL).
  {
    const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    m_mainComposite.bindComputePipeline(&RI.device, &RI.primary.cmds[0], kHash,
                                         "MainCompositePass.cs", RIComputePipelineDesc{});
    m_mainComposite.bindExternalSet(
        &RI.primary.cmds[0], &m_global.m_bindlessSet, 0,
        RI_PIPELINE_BIND_COMPUTE);

    std::vector<RIProgram::DescriptorBinding> bnd;
    bnd.reserve(8);
    {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create("gPerFrame");
      RI.UpdateFrameUBO(&b.descriptor, &perFrame, sizeof(perFrame));
      bnd.push_back(b);
    }
    pushSurfelStorageImage(bnd, "gPackedHitInfo",
                           state.packedHitInfoView[RI.swapchainIndex]);
    pushTlas(bnd);
    // gIndirectLighting / gPackedHitInfoRaster — sampled, SHADER_READ_ONLY
    // (transitioned upstream), so not the GENERAL pushSurfelSampledImage helper.
    auto pushReadOnlySampled = [&](const char *name, const RITextureView &view) {
      RIProgram::DescriptorBinding b;
      b.handle = DescriptorBindingID::Create(name);
      b.descriptor = RIDescriptor::sampledImage(
          &RI.device, const_cast<RITextureView *>(&view), hash_random(),
          RI_RESOURCE_STATE_SHADER_RESOURCE);
      bnd.push_back(b);
    };
    pushReadOnlySampled("gIndirectLighting",
                        state.surfelResultView[RI.swapchainIndex]);
    // Rasterized V-buffer fallback — SurfelGBuffer writes RI.visibilityTexture
    // earlier this frame and the toRead[] barriers transitioned it to
    // SHADER_READ_ONLY_OPTIMAL (visible to COMPUTE).
    pushReadOnlySampled("gPackedHitInfoRaster",
                        state.visibilityView[RI.swapchainIndex]);
    // gDirectLighting — this frame's direct irradiance the composite multiplies
    // albedo into: the SVGF-lite à-trous output (or the raw accumulation if the
    // filter is disabled), sampled, GENERAL.
    pushSurfelSampledImage(bnd, "gDirectLighting", *directResultView);
    // gOutput — the viewport render target bound as a storage image (GENERAL).
    pushSurfelStorageImage(
        bnd, "gOutput",
        state.renderTargetColorView[RI.swapchainIndex]);

    m_mainComposite.bindDescriptors(
        &RI.device, &RI.primary.cmds[0], RI.frameIndex, bnd.data(),
        bnd.size(), RI_PIPELINE_BIND_COMPUTE);

    RI.primary.cmds[0].dispatch(&RI.renderer, (renderWidth + 15u) / 16u,
                                (renderHeight + 15u) / 16u, 1u);
  }

  // Toggle the direct-lighting ping-pong: this frame's write becomes next
  // frame's history.
  state.directLightingIndex ^= 1u;

  // Render target: GENERAL (compute write) -> COLOR_ATTACHMENT_OPTIMAL so the
  // downstream raster passes find the layout they expect.
  {
    RI.primary.cmds[0].vk_d3d12_textureBarrier(
        {&state.renderTarget[RI.swapchainIndex], RI_RESOURCE_STATE_STORAGE_WRITE,
         RI_RESOURCE_STATE_RENDER_TARGET, RI_STAGE_COMPUTE});
  }

  // Single render target — no toggle: the main draw never ping-pongs. The
  // downstream raster passes flip it COLOR -> SHADER_READ as they go (each
  // pass transitions in before drawing and back out after); the tail blits
  // it into the viewport backbuffer, where the post-effect chain + tail blit
  // in cScene::Render consume it.
  {
    RI.primary.cmds[0].vk_d3d12_textureBarrier(RI_PogoShaderBarrier(
        &state.renderTarget[RI.swapchainIndex], /*initial=*/false));
  }

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
      cVertexBuffer *pVB = pObj->GetVertexBuffer();
      if (!pVB || pVB->GetIndexNum() <= 0)
        continue;
      decals.push_back(pObj);
    }

    if (!decals.empty()) {
      flipDepthToReadOnly();

      // Color target stored as an RIDescriptor; wrap its view to feed the
      // backend-neutral attachment (same idiom as the overlay pass).
      RITextureView colorView =
          state.renderTargetColorView[RI.swapchainIndex];

      {
        RI.primary.cmds[0].vk_d3d12_textureBarrier(RI_PogoAttachmentBarrier(
            &state.renderTarget[RI.swapchainIndex], /*initial=*/false));
      }

      RIRenderingAttachment colorAttachment = {};
      colorAttachment.view = &colorView;
      colorAttachment.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
      colorAttachment.storeOp = RI_ATTACHMENT_STORE_OP_STORE;

      // Scene depth bound read-only (flipDepthToReadOnly ran above): tested,
      // never written.
      RIRenderingAttachment depthAttachment = {};
      depthAttachment.view = &state.depthView[RI.swapchainIndex];
      depthAttachment.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
      depthAttachment.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
      depthAttachment.readOnly = true;

      RIBeginRenderingDesc renderingInfo = {};
      renderingInfo.renderArea.width = (int16_t)renderWidth;
      renderingInfo.renderArea.height = (int16_t)renderHeight;
      renderingInfo.colorCount = 1;
      renderingInfo.colors = &colorAttachment;
      renderingInfo.depthStencil = &depthAttachment;
      RI.primary.cmds[0].vk_d3d12_beginRendering(&RI.renderer, renderingInfo);
      RI.primary.cmds[0].mtl_encoderDraw(renderingInfo);

      RIViewport vp = {};
      vp.y = (float)renderHeight;
      vp.width = (float)renderWidth;
      vp.height = -(float)renderHeight;
      vp.depthMax = 1.0f;
      RIRect sc = {};
      sc.width = (int16_t)renderWidth;
      sc.height = (int16_t)renderHeight;
      RI.primary.cmds[0].setViewport(&RI.renderer, vp);
      RI.primary.cmds[0].setScissor(&RI.renderer, sc);

      m_decal.bindExternalSet(&RI.primary.cmds[0], &m_global.m_bindlessSet, 0);
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
        cVertexBuffer *pVB = pObj->GetVertexBuffer();
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
            static_cast<cVertexBuffer *>(pVB), d);
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
                             "Decal", pipelineDesc.desc);

        // Decal.frag's per-blend-mode output conversion (display-space source
        // → linear) needs the mode — mirrors the particle pass push block.
        const uint32_t push = (uint32_t)mode;
        m_decal.pushConstants(&RI.primary.cmds[0], &push, sizeof(push));

        RI.primary.cmds[0].drawIndexed(&RI.renderer, (uint32_t)indexCount, 1u, 0u, 0, slot);
      }

      RI.primary.cmds[0].mtl_encoderEnd();

      RI.primary.cmds[0].vk_d3d12_endRendering(&RI.renderer);

      {
        RI.primary.cmds[0].vk_d3d12_textureBarrier(RI_PogoShaderBarrier(
            &state.renderTarget[RI.swapchainIndex], /*initial=*/false));
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
      cVertexBuffer *pVB = pObj->GetVertexBuffer();
      if (!pVB || pVB->GetIndexNum() <= 0)
        continue;
      waters.push_back(pObj);
    }

    if (!waters.empty()) {
      flipDepthToReadOnly();

      // Color target stored as an RIDescriptor; wrap its view to feed the
      // backend-neutral attachment (same idiom as the overlay pass).
      RITextureView colorView =
          state.renderTargetColorView[RI.swapchainIndex];

      {
        RI.primary.cmds[0].vk_d3d12_textureBarrier(RI_PogoAttachmentBarrier(
            &state.renderTarget[RI.swapchainIndex], /*initial=*/false));
      }

      RIRenderingAttachment colorAttachment = {};
      colorAttachment.view = &colorView;
      colorAttachment.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
      colorAttachment.storeOp = RI_ATTACHMENT_STORE_OP_STORE;

      // Scene depth bound read-only (flipDepthToReadOnly ran above): tested,
      // never written.
      RIRenderingAttachment depthAttachment = {};
      depthAttachment.view = &state.depthView[RI.swapchainIndex];
      depthAttachment.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
      depthAttachment.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
      depthAttachment.readOnly = true;

      RIBeginRenderingDesc renderingInfo = {};
      renderingInfo.renderArea.width = (int16_t)renderWidth;
      renderingInfo.renderArea.height = (int16_t)renderHeight;
      renderingInfo.colorCount = 1;
      renderingInfo.colors = &colorAttachment;
      renderingInfo.depthStencil = &depthAttachment;
      RI.primary.cmds[0].vk_d3d12_beginRendering(&RI.renderer, renderingInfo);
      RI.primary.cmds[0].mtl_encoderDraw(renderingInfo);

      RIViewport vp = {};
      vp.y = (float)renderHeight;
      vp.width = (float)renderWidth;
      vp.height = -(float)renderHeight;
      vp.depthMax = 1.0f;
      RIRect sc = {};
      sc.width = (int16_t)renderWidth;
      sc.height = (int16_t)renderHeight;
      RI.primary.cmds[0].setViewport(&RI.renderer, vp);
      RI.primary.cmds[0].setScissor(&RI.renderer, sc);

      m_water.bindExternalSet(&RI.primary.cmds[0], &m_global.m_bindlessSet, 0);
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
        cVertexBuffer *pVB = pObj->GetVertexBuffer();
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
            static_cast<cVertexBuffer *>(pVB), d);
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
                               pd.desc);
          WaterPush push = {pass, 0u, 0u, 0u};
          m_water.pushConstants(&RI.primary.cmds[0], &push, sizeof(push));
          RI.primary.cmds[0].drawIndexed(&RI.renderer, (uint32_t)indexCount, 1u, 0u, 0, slot);
        }
      }

      RI.primary.cmds[0].mtl_encoderEnd();

      RI.primary.cmds[0].vk_d3d12_endRendering(&RI.renderer);

      {
        RI.primary.cmds[0].vk_d3d12_textureBarrier(RI_PogoShaderBarrier(
            &state.renderTarget[RI.swapchainIndex], /*initial=*/false));
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
      // Color target stored as an RIDescriptor; wrap its view to feed the
      // backend-neutral attachment (same idiom as the overlay pass).
      RITextureView colorView =
          state.renderTargetColorView[RI.swapchainIndex];
      const RI_Format_e particleTargetFormat = RIBootstrap::PogoColorFormat;
      {
        RI.primary.cmds[0].vk_d3d12_textureBarrier(RI_PogoAttachmentBarrier(
            &state.renderTarget[RI.swapchainIndex], /*initial=*/false));
      }

      RIRenderingAttachment colorAttachment = {};
      colorAttachment.view = &colorView;
      colorAttachment.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
      colorAttachment.storeOp = RI_ATTACHMENT_STORE_OP_STORE;

      // Scene depth bound read-only (flipDepthToReadOnly ran above): tested,
      // never written.
      RIRenderingAttachment depthAttachment = {};
      depthAttachment.view = &state.depthView[RI.swapchainIndex];
      depthAttachment.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
      depthAttachment.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
      depthAttachment.readOnly = true;

      RIBeginRenderingDesc renderingInfo = {};
      renderingInfo.renderArea.width = (int16_t)renderWidth;
      renderingInfo.renderArea.height = (int16_t)renderHeight;
      renderingInfo.colorCount = 1;
      renderingInfo.colors = &colorAttachment;
      renderingInfo.depthStencil = &depthAttachment;
      RI.primary.cmds[0].vk_d3d12_beginRendering(&RI.renderer, renderingInfo);
      RI.primary.cmds[0].mtl_encoderDraw(renderingInfo);

      RIViewport vp = {};
      vp.y = (float)renderHeight;
      vp.width = (float)renderWidth;
      vp.height = -(float)renderHeight;
      vp.depthMax = 1.0f;
      RIRect sc = {};
      sc.width = (int16_t)renderWidth;
      sc.height = (int16_t)renderHeight;
      RI.primary.cmds[0].setViewport(&RI.renderer, vp);
      RI.primary.cmds[0].setScissor(&RI.renderer, sc);

      m_particle.bindExternalSet(&RI.primary.cmds[0], &m_global.m_bindlessSet,
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
        cMaterial *pMat = pEmitter->GetMaterial();
        if (!pMat)
          continue;
        // Per-frame scratch geometry (no persistent emitter VB): build this
        // frame's camera-facing quads into the shared RI.translucentVtx/Idx
        // segments — same single producer as the wireframe/simple panes.
        auto geom = pEmitter->BuildScratchGeometry(apFrustum, afFrameTime, /*withUv=*/true);
        if (!geom.valid)
          continue;
        const int indexCount = (int)geom.indexCount;

        uint32_t materialId =
            m_global.submitMaterial(cntx, pMat, (uint32_t)RI.frameIndex).materialId;
        if (materialId == UINT32_MAX) {
          Warning("Material Slot exhausted (particle)");
          continue;
        }

        ObjectSubmitDesc d;          // particle: identity uv, no dissolve/illum
        d.modelMatrix = pEmitter->GetModelMatrix(apFrustum);
        d.materialId = materialId;

        // Particles share the object-slot pool with opaque solids; the payload
        // submit also bumps the slot generation when the slot is (re)assigned —
        // so a surfel still anchored to the slot's previous opaque occupant
        // self-invalidates before it dereferences this slot's smaller streams.
        const uint32_t slot = m_global.submitObject(
            pEmitter->GetUniqueCookie(), (uint32_t)RI.frameIndex, nullptr, d,
            kSubmitData);
        if (slot == UINT32_MAX) {
          Warning("bindless pool exhausted (particle)");
          continue;
        }

        // The particle VS pulls pos/uv0/color/index via BDA; point this slot's
        // handles at the scratch segments (base address + byte offset). The
        // mirrors are public by design — direct fill-site writes, refreshed
        // every frame exactly like the VB path was. normal/tangent are never
        // read for a particle slot; zero them so a stale opaque BDA can't
        // linger on a reused slot.
        {
          const uint64_t vtxBase =
              RI.translucentVtxBuffer.GetDeviceHandle(&RI.device);
          m_global.m_opaquePositionMirror.write<RIDeviceSize>(
              slot, vtxBase + geom.posByteOffset);
          m_global.m_opaqueColorMirror.write<RIDeviceSize>(
              slot, vtxBase + geom.colByteOffset);
          m_global.m_opaqueUv0Mirror.write<RIDeviceSize>(
              slot, vtxBase + geom.uvByteOffset);
          m_global.m_opaqueIndexMirror.write<RIDeviceSize>(
              slot, RI.translucentIdxBuffer.GetDeviceHandle(&RI.device) +
                        geom.idxByteOffset);
          m_global.m_opaqueNormalMirror.write<RIDeviceSize>(slot, 0);
          m_global.m_opaqueTangentMirror.write<RIDeviceSize>(slot, 0);
        }

        const ParticlePipelineDesc::BlendMode mode =
            remapBlend(pMat->GetBlendMode());
        ParticlePipelineDesc pipelineDesc(particleTargetFormat,
                                          RIBootstrap::DepthFormat, mode);
        m_particle.bindPipeline(&RI.device, &RI.primary.cmds[0],
                                pipelineDesc.hash, "Particle",
                                pipelineDesc.desc);

        // Fog (world + per-area) is applied per-pixel in Particle.frag.slang
        // by walking gFogAreas. sceneAlpha is now the unmodified per-object
        // scalar (1.0 by default) — kept in the push block for parity with the
        // mesh path and any future per-object alpha gates.
        const float sceneAlpha = 1.0f;
        PushBlock push = {(uint32_t)mode, sceneAlpha};
        m_particle.pushConstants(&RI.primary.cmds[0], &push, sizeof(push));

        RI.primary.cmds[0].draw(&RI.renderer, (uint32_t)indexCount, 1u, 0u, slot);
      }

      RI.primary.cmds[0].mtl_encoderEnd();

      RI.primary.cmds[0].vk_d3d12_endRendering(&RI.renderer);

      // pogo "read" half back to SHADER_READ_ONLY so the tail blit can sample it.
      {
        RI.primary.cmds[0].vk_d3d12_textureBarrier(RI_PogoShaderBarrier(
            &state.renderTarget[RI.swapchainIndex], /*initial=*/false));
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

      // Color target stored as an RIDescriptor; wrap its view to feed the
      // backend-neutral attachment (same idiom as the overlay pass).
      RITextureView colorView =
          state.renderTargetColorView[RI.swapchainIndex];
      const RI_Format_e meshTargetFormat = RIBootstrap::PogoColorFormat;

      // SHADER_READ_ONLY → COLOR_ATTACHMENT_OPTIMAL. If the particle pass
      // ran above, that block left the pogo half in SHADER_READ_ONLY (for
      // a tail blit that never got to run); if it didn't, the visibility
      // composite + post-effect chain also left it in SHADER_READ_ONLY. The
      // barrier helper handles either source state.
      {
        RI.primary.cmds[0].vk_d3d12_textureBarrier(RI_PogoAttachmentBarrier(
            &state.renderTarget[RI.swapchainIndex], /*initial=*/false));
      }

      RIRenderingAttachment colorAttachment = {};
      colorAttachment.view = &colorView;
      colorAttachment.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
      colorAttachment.storeOp = RI_ATTACHMENT_STORE_OP_STORE;

      // Scene depth bound read-only (flipDepthToReadOnly ran above): tested,
      // never written.
      RIRenderingAttachment depthAttachment = {};
      depthAttachment.view = &state.depthView[RI.swapchainIndex];
      depthAttachment.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
      depthAttachment.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
      depthAttachment.readOnly = true;

      RIBeginRenderingDesc renderingInfo = {};
      renderingInfo.renderArea.width = (int16_t)renderWidth;
      renderingInfo.renderArea.height = (int16_t)renderHeight;
      renderingInfo.colorCount = 1;
      renderingInfo.colors = &colorAttachment;
      renderingInfo.depthStencil = &depthAttachment;
      RI.primary.cmds[0].vk_d3d12_beginRendering(&RI.renderer, renderingInfo);
      RI.primary.cmds[0].mtl_encoderDraw(renderingInfo);

      RIViewport vp = {};
      vp.y = (float)renderHeight;
      vp.width = (float)renderWidth;
      vp.height = -(float)renderHeight;
      vp.depthMax = 1.0f;
      RIRect sc = {};
      sc.width = (int16_t)renderWidth;
      sc.height = (int16_t)renderHeight;
      RI.primary.cmds[0].setViewport(&RI.renderer, vp);
      RI.primary.cmds[0].setScissor(&RI.renderer, sc);

      m_translucentMesh.bindExternalSet(&RI.primary.cmds[0],
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
        cVertexBuffer *pVB = pObj->GetVertexBuffer();
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
        TranslucentMeshPipelineDesc pipelineDesc(meshTargetFormat,
                                                 RIBootstrap::DepthFormat,
                                                 mode, vtxMask);
        m_translucentMesh.bindPipeline(&RI.device, &RI.primary.cmds[0],
                                       pipelineDesc.hash, "TranslucentMesh",
                                       pipelineDesc.desc);

        // Fog (world + per-area) is applied per-pixel in Translucent.frag.slang
        // by walking gFogAreas. sceneAlpha stays 1.0 for the no-extra-alpha
        // common path.
        const float sceneAlpha = 1.0f;
        PushBlock push = {(uint32_t)mode, sceneAlpha, 0u, 0u};
        m_translucentMesh.pushConstants(&RI.primary.cmds[0], &push, sizeof(push));

        RI.primary.cmds[0].drawIndexed(&RI.renderer, (uint32_t)indexCount, 1u, 0u, 0, slot);

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
                                         addDesc.desc);
          PushBlock pushIllum = {
              (uint32_t)TranslucentMeshPipelineDesc::BLEND_ADD, sceneAlpha,
              kTransOptUseIllumination, 0u};
          m_translucentMesh.pushConstants(&RI.primary.cmds[0], &pushIllum,
                                          sizeof(pushIllum));
          // Vertex / index buffers stay bound from the main draw above —
          // same renderable, just a second pipeline + push-constant set.
          RI.primary.cmds[0].drawIndexed(&RI.renderer, (uint32_t)indexCount, 1u, 0u, 0, slot);
        }
      }

      RI.primary.cmds[0].mtl_encoderEnd();

      RI.primary.cmds[0].vk_d3d12_endRendering(&RI.renderer);

      // pogo "read" half back to SHADER_READ_ONLY so the tail blit can sample it.
      {
        RI.primary.cmds[0].vk_d3d12_textureBarrier(RI_PogoShaderBarrier(
            &state.renderTarget[RI.swapchainIndex], /*initial=*/false));
      }
    }
  }


  // Restore depth to DEPTH_ATTACHMENT_OPTIMAL before yielding the command
  // buffer: RI_VK_FillDepthAttachment hardcodes that layout, and depth ends
  // here in either SHADER_READ_ONLY_OPTIMAL (surfel-only) or
  // DEPTH_READ_ONLY_OPTIMAL (flipDepthToReadOnly ran for particle/decal).
  {
    const uint32_t beforeState =
        depthFlippedForReadOnly ? RI_RESOURCE_STATE_DEPTH_READ
                                : RI_RESOURCE_STATE_SHADER_RESOURCE;
    const uint32_t beforeStages =
        depthFlippedForReadOnly ? RI_STAGE_NONE
                                : (RI_STAGE_FRAGMENT | RI_STAGE_COMPUTE);

    RI.primary.cmds[0].vk_d3d12_textureBarrier(
        {&state.depthTextures[RI.swapchainIndex], beforeState,
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
  const bool debugOverlayDrawn =
      debugDraw && debugDraw->HasRequests();
  if (debugOverlayDrawn) {
    {
      RI.primary.cmds[0].vk_d3d12_textureBarrier(
          {&state.renderTarget[RI.swapchainIndex],
           RI_RESOURCE_STATE_SHADER_RESOURCE,
           RI_RESOURCE_STATE_RENDER_TARGET_READ, RI_STAGE_FRAGMENT});
    }
    {
      // Color target stored as an RIDescriptor; wrap its view (same idiom as
      // ReleaseViewportColorTexture) to feed the backend-neutral attachment.
      RITextureView colorView =
          state.renderTargetColorView[RI.swapchainIndex];

      RIRenderingAttachment colorAttachment = {};
      colorAttachment.view = &colorView;
      colorAttachment.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
      colorAttachment.storeOp = RI_ATTACHMENT_STORE_OP_STORE;

      // Scene depth, restored to DEPTH_ATTACHMENT_OPTIMAL just above; the
      // overlay pipelines test against it but never write. LOAD, no clear.
      RIRenderingAttachment depthStencil = {};
      depthStencil.view = &state.depthView[RI.swapchainIndex];
      depthStencil.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
      depthStencil.storeOp = RI_ATTACHMENT_STORE_OP_STORE;

      RIBeginRenderingDesc renderingInfo = {};
      renderingInfo.renderArea.width = (int16_t)renderWidth;
      renderingInfo.renderArea.height = (int16_t)renderHeight;
      renderingInfo.colorCount = 1;
      renderingInfo.colors = &colorAttachment;
      renderingInfo.depthStencil = &depthStencil;
      RI.primary.cmds[0].vk_d3d12_beginRendering(&RI.renderer, renderingInfo);
      RI.primary.cmds[0].mtl_encoderDraw(renderingInfo);

      debugDraw->flush(cntx, &RI.primary.cmds[0], apFrustum, renderWidth,
                       renderHeight, RIBootstrap::PogoColorFormat);

      RI.primary.cmds[0].mtl_encoderEnd();

      RI.primary.cmds[0].vk_d3d12_endRendering(&RI.renderer);
    }
    {
      RI.primary.cmds[0].vk_d3d12_textureBarrier(RI_PogoShaderBarrier(
          &state.renderTarget[RI.swapchainIndex], /*initial=*/false));
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
  if (!m_tlas.isEmpty(&RI.renderer)) {
    m_tlas.dispose(&RI.device);
    m_tlas = {};
  }
  if (IsRIBufferValid(&RI.renderer, &m_tlasInstanceBuffer)) {
    m_tlasInstanceBuffer.dispose(&RI.device);
    m_tlasInstanceBuffer = {};
  }
  m_global.destroy(&RI.device);
  // Fallback vertex streams are RIBootstrap globals (process-lifetime, like
  // RI.nulVertexBuffer); not freed here.
  for (uint32_t i = 0; i < RI_MAX_SWAPCHAIN_IMAGES; ++i) {
    m_surfelDepthView[i].dispose(&RI.device);
    m_surfelDepthTexture[i].dispose(&RI.device);
    m_surfelDepthView[i] = {};
    m_surfelDepthTexture[i] = {};
  }
}

} // namespace hpl
