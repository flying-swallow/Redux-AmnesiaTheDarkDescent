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

#include "forward_shared.h"

#include <array>
#include <vector>

namespace hpl {


class cHybridRenderer : public iRenderer {
public:
  cHybridRenderer(cGraphics *apGraphics, cResources *apResources);
  ~cHybridRenderer();

  // OBJECT_SLOT_CAPACITY / TEXTURE_SLOT_CAPACITY / MATERIAL_SLOT_CAPACITY and
  // SURFEL_MAX_CAPACTIY / MAX_RAY_COUNT come from amnesia/glsl/forward_shared.h.

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

  cRenderList2 m_rendererList;

  LRUCache m_diffuseBindless;

  struct RIBuffer_s m_diffuseObjectBuffer;

  // Per-frame point-light SSBO. Device-local; refilled each frame through
  // RI.uploader (see Draw()). m_pointLightScratch is the CPU staging vector,
  // reserved once at init so the per-frame fill doesn't reallocate.
  struct RIBuffer_s m_pointLightBuffer = {};
  std::array<PointLight, LIGHT_SLOT_CAPACITY> m_pointLightScratch;
  struct RIBuffer_s m_opaquePositionHandles;
  struct RIBuffer_s m_opaqueTangentHandles;
  struct RIBuffer_s m_opaqueNormalHandles;
  struct RIBuffer_s m_opaqueUv0Handles;
  struct RIBuffer_s m_opaqueColorHandles;
  struct RIBuffer_s m_opaqueIndexHandles;

	// Surfel Resources
  struct RIBuffer_s m_surfelCounterBuffer;
  struct RIBuffer_s m_surfelBuffer;
  struct RIBuffer_s m_surfelAliveBuffer;
  struct RIBuffer_s m_surfelDeadBuffer;
  struct RIBuffer_s m_surfelDirtyBuffer;
  struct RIBuffer_s m_surfelRecycleBuffer;
  struct RIBuffer_s m_surfelRayBuffer;

  // Cell-grid resources (set=0 bindings 17..19). Static infrastructure shared
  // by every surfel/cell compute pass; never resized at runtime.
  struct RIBuffer_s m_cellInfoBuffer;
  struct RIBuffer_s m_cellCounterBuffer;
  struct RIBuffer_s m_cellToSurfelBuffer;


  RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS> m_indirectSegment;
  struct RIBuffer_s m_indirectDrawBuffer;

  // Bindless material wiring.
  LRUCache m_materialBindless;
  struct RIBuffer_s m_opaqueMaterialBuffer;
  struct RIDescriptor_s *m_materialSampler = nullptr;

  RIBindlessDescriptorSet m_bindlessSet;
  LRUCache m_textureBindless;

  std::shared_ptr<RIBuffer_s> m_tlasStorage;
  struct RIAccelStructure_s m_tlas = {};
  struct RIBuffer_s m_tlasInstanceBuffer = {};
  uint32_t m_tlasCapacity = 0;

  // Surfel-generation output — full-res HDR storage image written by
  // surfel_generation_pass.comp (set=3, binding=1). One per swapchain image.
  struct RITexture_s     m_surfelResultTexture[RI_MAX_SWAPCHAIN_IMAGES] = {};
  struct RITextureView_s m_surfelResultView[RI_MAX_SWAPCHAIN_IMAGES] = {};
  uint32_t m_surfelResultWidth = 0;
  uint32_t m_surfelResultHeight = 0;

  // prepare gbuffer
	RIProgram m_gbuffer;

  // surfel prepare
	RIProgram m_surfelPrepare;
	RIProgram m_surfelGenerate;
	RIProgram m_surfelUpdate;
	RIProgram m_cellInfoUpdate;
	RIProgram m_surfelRaytrace;

	// Surfel-ray irradiance map sampled by surfel_raytrace.comp's ray-guiding
	// branch. Filled in by a future pass; until then it stays zero-cleared,
	// which keeps the cosine-weighted branch active.
	struct RITexture_s     m_surfelIrradianceTexture[RI_MAX_SWAPCHAIN_IMAGES] = {};
	struct RITextureView_s m_surfelIrradianceView[RI_MAX_SWAPCHAIN_IMAGES]    = {};
};

} // namespace hpl

#endif
