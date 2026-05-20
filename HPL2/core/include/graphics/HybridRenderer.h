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

class Image;

class cHybridRenderer : public iRenderer {
public:
  cHybridRenderer(cGraphics *apGraphics, cResources *apResources);
  ~cHybridRenderer();

  // OBJECT_SLOT_CAPACITY / TEXTURE_SLOT_CAPACITY / MATERIAL_SLOT_CAPACITY and
  // SURFEL_MAX_CAPACITY / MAX_RAY_COUNT come from amnesia/glsl/forward_shared.h.

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
  // point/spot attenuation maps). Returns INVALID_TEXTURE_INDEX when the
  // image is null or the bindless pool is exhausted.
  uint32_t resolveTextureSlot(RIBootstrap::FrameContext *cntx, Image *img, uint32_t frameIndex);

  // Same as resolveTextureSlot() but writes into the textures_cube[] bindless
  // array at BINDING_TEXTURES_CUBE. The 2D and cube pools cannot share slot
  // ids because they index different descriptor arrays. Currently fed only
  // by per-frame point-light gobo cube uploads.
  uint32_t resolveCubeTextureSlot(RIBootstrap::FrameContext *cntx, Image *img, uint32_t frameIndex);

  cRenderList2 m_rendererList;

  LRUCache m_diffuseBindless;

  struct RIBuffer_s m_diffuseObjectBuffer;

  // Per-frame light SSBOs. Device-local; refilled each frame through
  // RI.uploader (see Draw()). The m_*Scratch arrays are CPU staging,
  // reserved once at init so the per-frame fill doesn't reallocate.
  struct RIBuffer_s m_pointLightBuffer = {};
  std::array<PointLight, POINT_SLOT_LIGHT_CAPACITY> m_pointLightScratch;
  struct RIBuffer_s m_spotLightBuffer = {};
  std::array<SpotLight, SPOT_SLOT_LIGHT_CAPACITY> m_spotLightScratch;
  struct RIBuffer_s m_boxLightBuffer = {};
  std::array<BoxLight, BOX_SLOT_LIGHT_CAPACITY> m_boxLightScratch;

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

  // prepare gbuffer
	RIProgram m_gbuffer;

  // surfel prepare
	RIProgram m_surfelPrepare;
	RIProgram m_surfelGenerate;
	RIProgram m_surfelUpdate;
	RIProgram m_cellInfoUpdate;
	RIProgram m_cellToSurfelUpdate;
	RIProgram m_surfelRaytrace;
	RIProgram m_surfelIntegrate;

	// Final visibility-buffer composite — port of AmnesiaTheDarkDescent's
	// visibility_emit_shade_pass.frag.fsl. Fullscreen triangle that
	// reconstructs (objectID, primID) from the gbuffer, runs Hawkins
	// barycentric, samples diffuse + applies point-light direct + surfel
	// indirect, and writes the swapchain image.
	RIProgram m_visibilityShade;

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
};

} // namespace hpl

#endif
