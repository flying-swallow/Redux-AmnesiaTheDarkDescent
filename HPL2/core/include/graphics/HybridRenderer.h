#ifndef HPL_RENDERER_HYBRID_H
#define HPL_RENDERER_HYBRID_H

#include "graphics/GlobalManagedSets.h"
#include "graphics/HPLGraphicsConfig.h"
#include "graphics/Material.h"
#include "graphics/RenderList.h"
#include "graphics/Renderer.h"
#include "graphics/RISegmentAlloc.h"
#include "graphics/RITypes.h"
#include <array>

#include "Constants.h"

namespace hpl {

class Image;
class cVertexBuffer;

class cHybridRenderer : public iRenderer {
public:
  cHybridRenderer(cGraphics *apGraphics, cResources *apResources);
  ~cHybridRenderer();

  // Per-viewport internals live in HybridViewportState (pure data — see
  // scene/Viewport.h), held by cViewport; this renderer owns its
  // creation/sizing (file-local helper in HybridRenderer.cpp).

  // kObjectSlotCapacity / kTextureSlotCapacity / kMaterialCapacity and
  // kTotalSurfelLimit / kRayBudget come from amnesia/glsl/forward_shared.h.

  virtual void Draw(RIBootstrap::FrameContext *cntx, cViewport *viewport,
                    float afFrameTime, cFrustum *apFrustum, cWorld *apWorld,
                    cRenderSettings *apSettings,
                    bool abSendFrameBufferToPostEffects) override;

  virtual bool LoadData() override { return true; };
  virtual void DestroyData() override {};

  void SetOverlay(int alOverlay) { m_overlayMode = (uint32_t)alOverlay; }

private:
  cRenderList2 m_rendererList;

  // The object-slot cache now lives in m_global (GlobalManagedSets::
  // m_objectSlots); submitObject() owns slot allocation + the slot-generation
  // bump (on new occupant / index-count change / VB destroy), so the renderer no
  // longer holds the cache, the per-slot hooks, or a geometry-dirty array.

  // Default-value fallback vertex streams for renderables that omit an optional
  // stream now live globally in RIBootstrap (RI.fallback*Vertex), created once
  // at init; detail::BindVertexStreams binds them in the raster passes.

  RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS> m_indirectSegment;
  struct RIBuffer m_indirectDrawBuffer;

  // The ray-tracing TLAS (storage + instance buffer) is owned by cWorld and built
  // in cWorld::PrepareFrame; each RT pass binds it via apWorld->GetTlas().

  // (The per-viewport frame textures — surfel result, packed hit info,
  // velocity, direct-lighting history + ping-pong index/init, prev-frame
  // camera — live on cViewport::HybridViewportState; their Update/Dispose
  // are defined in HybridRenderer.cpp.)

  // Per-bounce V-buffers written by SurfelVBuffer.rt.slang's closeHit when
  // the primary surface is refractive / reflective. Same format + dims as
  // the viewport state's packedHitInfoTexture; cleared to uint4(0) host-side each frame so
  // consumers detect "no bounce here" via the valid bit in .w. .w high
  // bits also stamp the source instanceID (the glass / mirror that bent
  // the ray) so consumers can look up its DiffuseMaterial for blending.

  // prepare gbuffer
	RIProgram m_gbuffer;

  // Surfel-GI compute / ray-tracing programs. Filled by stage B–F of the
  // SurfelGI port. Until Stage F lands, the visibility_shade composite
  // reads vec3(0) indirect — direct lighting still renders correctly.

  // Stage B: POM barycentric correction compute pass.
  // Copies visibilityTexture (raw raster hit) → packedHitInfoTexture and
  // perturbs barycentrics on height-mapped diffuse surfaces so downstream
  // getVertexData() reconstructs the parallax-occluded point.
  // Water/glass refraction pixels are handled by a follow-up sparse RT pass.
  RIProgram m_surfelPomBary;

  // Stage D: surfel prepare / cell-clear / update chain. Each .comp maps to
  // one of the SurfelGI reference's csMain entry points (the reference is
  // a single .slang file with three [shader("compute")] entries; we split
  // each into its own SPIR-V module because GLSL is one-entry-per-file).
  // The reservation+refcounter clears are factored out into auxiliary
  // dispatches that the reference folds into the front of its prepare/
  // accumulate passes; explicit dispatches mirror the data dependency.
  RIProgram m_surfelPrepare;
  RIProgram m_surfelUpdateCollect;
  RIProgram m_surfelUpdateAccumulate;
  RIProgram m_surfelUpdateScatter;
  RIProgram m_lightGridBin;

  // Stage E: path-tracer RT pipeline. One ray per pending SurfelRayResult
  // slot; the rgen drives an iterative trace loop (no recursive TraceRay),
  // closest-hit handles material shading + next-event-estimation using an
  // inline ray-query for shadow visibility.
  RIProgram m_surfelRT;
  // Indirect args for vkCmdTraceRaysIndirectKHR: VkTraceRaysIndirectCommandKHR
  // {width, height=1, depth=1}. Width is copied from gSurfelCounter[kSurfelCounterRequestedRay]
  // each frame so the RT dispatch scales to actual ray demand.
  struct RIBuffer m_surfelRTIndirectBuf;

  // Stage F: integrate + generation. The integrate pass folds ray results
  // into per-surfel radiance via MSME; the generation pass walks the
  // visibility buffer per pixel, scores cell coverage, spawns / recycles
  // surfels, and writes the indirect-lighting term into
  // the viewport state's surfelResultTexture which visibility_shade.frag samples.
  RIProgram m_surfelIntegrate;
  RIProgram m_surfelGenerate;

  // Final composite (Slang MainCompositePass.cs.slang). Reads
  // gPackedHitInfo + gIndirectLighting + gDirectLighting and writes gOutput.
  RIProgram m_mainComposite;

  // Direct-lighting pass (DirectLightingPass.cs.slang): soft-shadowed analytic
  // direct lighting, temporally accumulated via the velocity texture. Writes the
  // ping-pong direct texture the composite then samples.
  RIProgram m_directLighting;
  // ReSTIR DI spatial reuse + resolve (DirectSpatialReusePass.cs).
  RIProgram m_directSpatialReuse;
  // SVGF-lite à-trous spatial denoise for the direct pass (DirectAtrousPass.cs).
  RIProgram m_directAtrous;


	// Particle (translucent) pass — port of legacy RendererDeferred's
	// translucency_particle.{vert,frag}.fsl. Reuses the opaque object/material
	// bindless pools (one OBJECT slot + one MATERIAL slot per emitter per
	// frame); cHybridRenderer writes the particle VB's BDA into the same
	// opaque*Handles[] arrays so the VS can pull pos/uv/color via instanceId.
	// Hardware blend state varies per blend mode — one pipeline per mode is
	// stamped on demand via the program's PipelineSlot cache.
	RIProgram m_particle;

	// Non-particle translucent meshes (glass, lamp glass, decals tagged
	// translucent, etc.). Renders in its own pass after the particle pass into
	// the same pogo "read" half, depth read-only. One pipeline per
	// eMaterialBlendMode (Add/Mul/MulX2/Alpha/PremulAlpha) is stamped on demand
	// via the program's PipelineSlot cache, mirroring m_particle. Refraction
	// and cube-map reflection materials are filtered out at the call site —
	// those need a screen-color copy + cube-map binding the renderer doesn't
	// have yet.
	RIProgram m_translucentMesh;

	// Decal overlay pass. Thin clipped meshes (blood / scorch / impact marks,
	// posters) drawn after the translucent mesh pass into the same pogo "read"
	// half, depth read-only. Reuses the translucent 5-stream fixed-function
	// vertex layout, the m_diffuseBindless / m_objectBuffer object pool,
	// and resolveMaterial (DiffuseMaterial slot — only tex[0] diffuse is used).
	// One pipeline per eMaterialBlendMode is stamped via the program's
	// PipelineSlot cache. Port of decal.frag.fsl / decal.vert.fsl.
	RIProgram m_decal;
	RIProgram m_water;

	// Surfel-ray depth map — RG16F atlas storing (depth, depth^2) per
	// octahedral tile texel for each surfel. Written by surfel_integrate.comp;
	// sampled by the surfel shading passes for visibility-aware GI gather.
	// Lives at VK_IMAGE_LAYOUT_GENERAL across all surfel passes so the same
	// view can be bound as both storage image (writes) and sampled image.
	struct RITexture     m_surfelDepthTexture[RI_MAX_SWAPCHAIN_IMAGES] = {};
	struct RITextureView m_surfelDepthView[RI_MAX_SWAPCHAIN_IMAGES]    = {};

	// One-shot UNDEFINED -> GENERAL transition tracker for the two surfel
	// atlases. Per-swapchain-image because each backing image needs the
	// transition once on its first appearance; after that the atlas data
	// must persist (integrate EMA-blends with the prior frame's values).
	std::array<bool, RI_MAX_SWAPCHAIN_IMAGES> m_surfelAtlasesInitialized = {};
  bool m_surfelRTIndirectInit = false;

	struct OverlayPushConstants { uint32_t overlayMode; };
	uint32_t m_overlayMode = kDefaultOverlayMode;
};

} // namespace hpl

#endif
