#ifndef HPL_RENDERER_HYBRID_H
#define HPL_RENDERER_HYBRID_H

#include "graphics/BindlessPool.h"
#include "graphics/HPLGraphicsConfig.h"
#include "graphics/Material.h"
#include "graphics/RenderList2.h"
#include "graphics/Renderer.h"
#include "graphics/RISegmentAlloc.h"
#include "graphics/RITypes.h"
#include "graphics/RIRenderer.h"
#include "system/Hasher.h"
#include "system/Event.h"
#include <array>
#include <vector>

#include "Constants.h"
#include "SceneTypes.slang"
#include "SurfelGI/SurfelTypes.slang"

namespace hpl {

class Image;

class cHybridRenderer : public iRenderer {
public:
  cHybridRenderer(cGraphics *apGraphics, cResources *apResources);
  ~cHybridRenderer();

  // kObjectSlotCapacity / kTextureSlotCapacity / kMaterialSlotCapacity and
  // kTotalSurfelLimit / kRayBudget come from amnesia/glsl/forward_shared.h.

  virtual void Draw(RIBootstrap::FrameContext *cntx, cViewport *viewport,
                    float afFrameTime, cFrustum *apFrustum, cWorld *apWorld,
                    cRenderSettings *apSettings,
                    bool abSendFrameBufferToPostEffects) override;

  virtual bool LoadData() override { return true; };
  virtual void DestroyData() override {};
  virtual void CopyToFrameBuffer() override {};
  virtual void SetupRenderList() override {};
  virtual void RenderObjects() override {};

private:
  // Resolve `mat` to its slot in m_opaqueMaterialBuffer. Allocates a slot on
  // first sight, resolves and uploads texture indices when the material's
  // generation differs from the cached one. Returns UINT32_MAX when the
  // material pool is exhausted.
  uint32_t resolveMaterial(RIBootstrap::FrameContext *cntx,cMaterial *mat, uint32_t frameIndex);

  // Map an Image* to its bindless slot in textures_2d[]. Used by
  // resolveMaterial() and the light upload loops (spot falloff / gobo,
  // point/spot attenuation maps). Returns kInvalidTextureIndex when the
  // image is null or the bindless pool is exhausted.
  uint32_t resolveTextureSlot(RIBootstrap::FrameContext *cntx, Image *img, uint32_t frameIndex);

  // Same as resolveTextureSlot() but writes into the textures_cube[] bindless
  // array at kBindingTexturesCube. The 2D and cube pools cannot share slot
  // ids because they index different descriptor arrays. Currently fed only
  // by per-frame point-light gobo cube uploads.
  uint32_t resolveCubeTextureSlot(RIBootstrap::FrameContext *cntx, Image *img, uint32_t frameIndex);

  cRenderList2 m_rendererList;

  // Object-slot cache. Each slot stores an EventHandler bound to the owning
  // vertex buffer's destroy event, so the slot is retired for surfel cleanup
  // when its geometry is destroyed (see the solids loop in Draw()).
  LRUCacheState<EventHandler<>> m_diffuseBindless;

  struct RIBuffer_s m_diffuseObjectBuffer;

  // Per-frame light SSBOs. Device-local; refilled each frame through
  // RI.uploader (see Draw()). The m_*Scratch arrays are CPU staging,
  // reserved once at init so the per-frame fill doesn't reallocate.
  struct RIBuffer_s m_pointLightBuffer = {};
  std::array<PointLight, kPointSlotLightCapacity> m_pointLightScratch;
  struct RIBuffer_s m_spotLightBuffer = {};
  std::array<SpotLight, kSpotSlotLightCapacity> m_spotLightScratch;
  struct RIBuffer_s m_boxLightBuffer = {};
  std::array<BoxLight, kBoxSlotLightCapacity> m_boxLightScratch;

  struct RIBuffer_s m_opaquePositionHandles;
  struct RIBuffer_s m_opaqueTangentHandles;
  struct RIBuffer_s m_opaqueNormalHandles;
  struct RIBuffer_s m_opaqueUv0Handles;
  struct RIBuffer_s m_opaqueColorHandles;
  struct RIBuffer_s m_opaqueIndexHandles;

  // Per-object-slot reuse generation (sized kObjectSlotCapacity). Bumped to a
  // fresh monotonic value each time m_diffuseBindless (re)assigns a slot to a
  // different object; surfels capture it at spawn so a reused slot
  // self-invalidates the stale anchor in collectCellInfo. m_nextSlotGeneration
  // is the host-side source of new values so we never read back the
  // write-combined mapped buffer.
  struct RIBuffer_s m_bindlessSlotGenerationBuffer;
  uint32_t m_nextSlotGeneration = 0;

  // === SurfelGI resources (set=0 bindings 10..19, 27..28, 31..33) ===
  // Layout mirrors the reference SurfelGI implementation at
  //   /mnt/c/Users/m_pol/Documents/projects/SurfelGI/RenderPasses/Surfel/
  // Sizes come from forward_shared.h: kTotalSurfelLimit = 150K surfels,
  // kRayBudget = 9.6M ray slots, kCellCount = 250^3 = 15.625M cells.
  //
  // The counter is a flat uint[kSurfelCounterSlotCount] buffer indexed by
  // SURFEL_COUNTER_* macros (Valid/Dirty/Free/Cell/RequestedRay/MissBounce).
  // gSurfelGeometryBuffer caches the uint4 TriangleHit from primary-ray
  // visibility, so surfels can be refreshed each frame from the cached hit
  // without re-tracing. The cellInfo + cellToSurfel pair encodes a sparse
  // surfel-per-cell index list scattered into a 125-cell neighborhood.
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

  // Per-surfel copy of its anchor slot's generation, captured at spawn and
  // compared against m_bindlessSlotGenerationBuffer in collectCellInfo.
  struct RIBuffer_s m_surfelSlotGenerationBuffer;

  RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS> m_indirectSegment;
  struct RIBuffer_s m_indirectDrawBuffer;

  // Bindless material wiring.
  LRUCache m_materialBindless;
  struct RIBuffer_s m_opaqueMaterialBuffer;
  struct RIDescriptor_s *m_materialSampler = nullptr;

  RIBindlessDescriptorSet m_bindlessSet;
  LRUCache m_textureBindless;
  // Separate LRU for cube textures. Slot ids index textures_cube[] (set 0,
  // binding 1) and must not be confused with textures_2d[] ids.
  LRUCache m_textureCubeBindless;

  RIBuffer_s m_tlasStorage = {};
  struct RIAccelStructure_s m_tlas = {};
  struct RIBuffer_s m_tlasInstanceBuffer = {};
  uint32_t m_tlasCapacity = 0;
  uint32_t m_tlasStorageCapacity = 0;

  // Surfel-generation output — full-res HDR storage image written by
  // surfel_generation_pass.comp (set=3, binding=1). One per swapchain image.
  struct RITexture_s     m_surfelResultTexture[RI_MAX_SWAPCHAIN_IMAGES] = {};
  struct RITextureView_s m_surfelResultView[RI_MAX_SWAPCHAIN_IMAGES] = {};
  uint32_t m_surfelResultWidth = 0;
  uint32_t m_surfelResultHeight = 0;

  // Stage B packed visibility — RGBA32UI storage image written by the
  // surfel_vbuffer RT pipeline, sampled by the Stage D / F surfel update
  // and generation passes. One per swapchain image; per-frame layout is
  // UNDEFINED → GENERAL → SHADER_READ_ONLY, like the gbuffer outputs.
  struct RITexture_s     m_packedHitInfoTexture[RI_MAX_SWAPCHAIN_IMAGES] = {};
  struct RITextureView_s m_packedHitInfoView[RI_MAX_SWAPCHAIN_IMAGES] = {};

  // prepare gbuffer
	RIProgram m_gbuffer;

  // Surfel-GI compute / ray-tracing programs. Filled by stage B–F of the
  // SurfelGI port. Until Stage F lands, the visibility_shade composite
  // reads vec3(0) indirect — direct lighting still renders correctly.

  // Stage B: primary-ray VBuffer. RT pipeline (rgen + chit + ahit + miss)
  // that writes a packed uvec4 TriangleHit into m_packedHitInfoTexture.
  // Same hit-pack convention as the Slang reference's TriangleHit.pack(),
  // so Stage D update / Stage F generation+composite can consume the
  // resulting visibility data.
  RIProgram m_surfelVBuffer;

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

  // Stage D-pre: free surfels anchored to geometry destroyed mid-level. The
  // handler queues retired bindless object slots fired by
  // VertexBuffer_RI::s_onGiSlotRetired; the clear pass marks matching surfels
  // dead before collectCellInfo re-resolves their (now dangling) cached hit
  // through gScene.getVertexData(). Complements resetSurfelState() (map loads).
  RIProgram m_surfelClearByInstance;
  // Bindless object slots whose backing geometry was destroyed. A VB's destroy
  // handler (owned by m_diffuseBindless's per-slot state) pushes the slot it
  // occupied; the clear pass drains this each frame. Single-threaded game, so a
  // plain vector — no lock. The handlers live in m_diffuseBindless (a member),
  // so they can't fire after this renderer (and this vector) are gone.
  std::vector<uint32_t> m_retiredGiSlots;

  // Stage E: path-tracer RT pipeline. One ray per pending SurfelRayResult
  // slot; the rgen drives an iterative trace loop (no recursive TraceRay),
  // closest-hit handles material shading + next-event-estimation using an
  // inline ray-query for shadow visibility.
  RIProgram m_surfelRT;

  // Stage F: integrate + generation. The integrate pass folds ray results
  // into per-surfel radiance via MSME; the generation pass walks the
  // visibility buffer per pixel, scores cell coverage, spawns / recycles
  // surfels, and writes the indirect-lighting term into
  // m_surfelResultTexture which visibility_shade.frag samples.
  RIProgram m_surfelIntegrate;
  RIProgram m_surfelGenerate;

  // Final composite (Slang SurfelGIRenderPass.cs.slang). Reads
  // gPackedHitInfo + gIndirectLighting (the integrate pass's output) and
  // writes gOutput = swapchain. Replaces the legacy visibility_shade
  // fullscreen pass once it's wired in fully.
  RIProgram m_surfelGIRender;

	// Particle (translucent) pass — port of legacy RendererDeferred's
	// translucency_particle.{vert,frag}.fsl. Reuses the opaque object/material
	// bindless pools (one OBJECT slot + one MATERIAL slot per emitter per
	// frame); cHybridRenderer writes the particle VB's BDA into the same
	// opaque*Handles[] arrays so the VS can pull pos/uv/color via instanceId.
	// Hardware blend state varies per blend mode — one pipeline per mode is
	// stamped on demand via the program's PipelineSlot cache.
	RIProgram m_particle;

	// Surfel-ray irradiance map sampled by surfel_raytrace.comp's ray-guiding
	// branch and written by surfel_integrate.comp. Lives at VK_IMAGE_LAYOUT_GENERAL
	// across all surfel passes so the same view can be bound as both
	// combined-image-sampler (for reads) and storage-image (for writes) within
	// the integrate dispatch.
	struct RITexture_s     m_surfelIrradianceTexture[RI_MAX_SWAPCHAIN_IMAGES] = {};
	struct RITextureView_s m_surfelIrradianceView[RI_MAX_SWAPCHAIN_IMAGES]    = {};

	// Surfel-ray depth map — RG16F atlas storing (depth, depth^2) per
	// octahedral tile texel for each surfel. Written by surfel_integrate.comp
	// alongside the irradiance atlas; sampled by future shading passes for
	// visibility-aware GI gather. Lives at GENERAL like the irradiance atlas.
	struct RITexture_s     m_surfelDepthTexture[RI_MAX_SWAPCHAIN_IMAGES] = {};
	struct RITextureView_s m_surfelDepthView[RI_MAX_SWAPCHAIN_IMAGES]    = {};

	// One-shot UNDEFINED -> GENERAL transition tracker for the two surfel
	// atlases. Per-swapchain-image because each backing image needs the
	// transition once on its first appearance; after that the atlas data
	// must persist (integrate EMA-blends with the prior frame's values).
	std::array<bool, RI_MAX_SWAPCHAIN_IMAGES> m_surfelAtlasesInitialized = {};

	// Tail blit pass that copies the final pogo "read" half into the
	// swapchain image. Lives between the post-effect composite (or
	// directly after the surfel-GI write, when the composite has no
	// active effects) and the particle/decal pass.
	RIProgram m_postEffectBlit;

	// One-shot UNDEFINED transition tracker for the pogo halves. Per
	// swapchain image, since each backing image needs the initial
	// transitions emitted once on its first appearance.
	std::array<bool, RI_MAX_SWAPCHAIN_IMAGES> m_pogoInitialized = {};
};

} // namespace hpl

#endif
