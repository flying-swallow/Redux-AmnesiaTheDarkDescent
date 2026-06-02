
#ifndef HPL_GRAPHICS_BACKEND_H
#define HPL_GRAPHICS_BACKEND_H

#include "graphics/GraphicsTypes.h"
#include "graphics/HPLGraphicsConfig.h"
#include "graphics/RISegmentAlloc.h"
#include "graphics/RITypes.h"
#include <array>

#include <graphics/RIResourceUploader.h>
#include <graphics/RIScratchAlloc.h>
#include <graphics/RIProgram.h>
#include <graphics/RIPogoBuffer.h>
#include <memory>

namespace hpl {
struct HPLTexture;


//bootstrap implementation
struct RIBootstrap {
public:
  // Raster G-buffer output — packed TriangleHit (uint4), same layout the RT
  // V-buffer writes into m_packedHitInfoTexture. Decoded with unpackHit() in
  // visibility_shade.frag and the surfel passes (see surfel_vbuffer.rgen for
  // the canonical pack convention).
  static constexpr RI_Format_e VisibilityFormat = RI_FORMAT_RGBA32_UINT;
  static constexpr RI_Format_e DepthFormat = RI_FORMAT_D32_SFLOAT;

  // HDR format for the pogo ping-pong buffer and every post-effect
  // intermediate target / pipeline color attachment that feeds it. The
  // SurfelGI composite + bloom keep linear values >1 here (the swapchain is
  // 16-bit linear scRGB); all these targets must share one format or Vulkan
  // raises an attachment-format mismatch — so this is the single source.
  // `PogoColorFormat` (RI) and `PogoColorFormatVk` (Vulkan) are the same
  // format in the two type vocabularies the call sites use; keep in lockstep.
  static constexpr RI_Format_e PogoColorFormat = RI_FORMAT_RGBA16_SFLOAT;
#if (DEVICE_IMPL_VULKAN)
  static constexpr VkFormat PogoColorFormatVk = VK_FORMAT_R16G16B16A16_SFLOAT;
#endif


  explicit RIBootstrap() {

  }
  struct FrameContext {
    struct RIScratchAlloc_s uboScratchAlloc;
    struct RIScratchAlloc_s accelScratchAlloc;
    std::vector<std::shared_ptr<HPLTexture>> textureLink; // keep track of textures that are used in this frame
    std::vector<std::shared_ptr<RIBuffer_s>> bufferLink; 
    std::vector<std::shared_ptr<RIAccelStructure_s>> accelLink;
    std::vector<RIFree> freelist;
  };

  RIRenderer_s renderer;
  RIDevice_s device;
	RIProgram gui;
	// Fullscreen passthrough (posteffect_fullscreen.vert + posteffect_blit.frag)
	// used to blit the renderer's pogo "read" half to the swapchain. Lives here
	// so cScene can present after applying the viewport's post-effect composite.
	RIProgram postEffectBlit;

  // 1x1 white texture used as the default texture binding when no real
  // texture is available.
  struct RITexture_s whiteTexture2D;
  struct RIDescriptor_s whiteTexture2DBinding;

  // Zero-filled vertex buffer bound into vertex input slots that don't have
  // a real stream — the vertex fetcher reads zeros for those attributes.
  struct RIBuffer_s nulVertexBuffer;

  // Default-value fallback vertex streams bound when a renderable omits an
  // optional stream in the fixed-function raster passes (translucent / water /
  // decal): normal = +Z, tangent = +X with +handedness, color = white, uv = 0.
  // Each holds a single vertex — the pipeline zeroes the binding stride for an
  // absent stream (see TranslucentMeshPipelineDesc / DecalPipelineDesc), so the
  // one element feeds every vertex. Filled once at init (see Graphics.cpp);
  // consumed by detail::BindVertexStreams in HybridRenderer.cpp.
  struct RIBuffer_s fallbackNormalVertex;
  struct RIBuffer_s fallbackTangentVertex;
  struct RIBuffer_s fallbackColorVertex;
  struct RIBuffer_s fallbackUv0Vertex;

  RISwapchain_s<RI_MAX_SWAPCHAIN_IMAGES> swapchain;
	struct RITextureView_s swapchainView[RI_MAX_SWAPCHAIN_IMAGES];
	struct RITexture_s depthTextures[RI_MAX_SWAPCHAIN_IMAGES];
	struct RITextureView_s depthView[RI_MAX_SWAPCHAIN_IMAGES];
	struct RI_PogoBuffer pogoBuffer[RI_MAX_SWAPCHAIN_IMAGES];

  struct RITexture_s visibilityTexture[RI_MAX_SWAPCHAIN_IMAGES];
  struct RITextureView_s visibilityView[RI_MAX_SWAPCHAIN_IMAGES];

	RICommandRingBuffer_s<RI_COMMAND_RING_POOL_COUNT, RI_COMMAND_RING_CMD_PER_POOL> graphicsCmdRing;
	struct RICommandRingElement_s primary;
	// Dedicated command buffer for BLAS builds, submitted (signalling its own
	// semaphore) ahead of `primary` so the primary's TLAS build is guaranteed to
	// see fully-built BLAS without an inline accel-build barrier. Second element
	// from graphicsCmdRing (its own cmd/fence/semaphore, shares primary's pool).
	struct RICommandRingElement_s blasSubmit;

  struct RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS> guiVertexAlloc;
  RIBuffer_s guiVertexBuffer; 
  struct RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS> guiIndexAlloc;
  RIBuffer_s guiIndexBuffer;

  std::array<FrameContext, RI_NUMBER_FRAMES_FLIGHT> frameSets;
	std::array<RIDescriptor_s, 1024> cachedFilters; 
  uint32_t swapchainIndex;
  uint32_t frameIndex = 0;

  struct RIResourceUploader_s uploader = {};

  void IncrementFrame();
  RIDescriptor_s *resolve_filter_descriptor(eTextureWrap wrapS, eTextureWrap wrapT, eTextureWrap wrapR, eTextureFilter filter);
  FrameContext *GetActiveSet() { return &frameSets[frameIndex % RI_NUMBER_FRAMES_FLIGHT]; }

  void UpdateFrameUBO(RIDescriptor_s* descriptor, void* data, size_t size);
  void CloseAndSubmitActiveSet();
  void BeginActiveSet();
  void Dispose();

};
extern struct RIBootstrap RI; 

}; // namespace hpl

#endif
