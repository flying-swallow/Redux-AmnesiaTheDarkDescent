
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
  static constexpr RI_Format_e VisibilityFormat = RI_FORMAT_R32_UINT;
  static constexpr RI_Format_e DepthFormat = RI_FORMAT_D32_SFLOAT;
  // Packed unit-vector normal — see compress_unit_vec / decompress_unit_vec.
  // Sampled by surfel_generation_pass as `usampler2D` and unpacked back to a
  // vec3; the surfel shaders' decompress_unit_vec(uint) expects R32_UINT.
  static constexpr RI_Format_e NormalFormat = RI_FORMAT_R32_UINT;

  explicit RIBootstrap() {

  }
  struct FrameContext {
    struct RIScratchAlloc_s uboScratchAlloc;
    std::vector<std::shared_ptr<HPLTexture>> textureLink; // keep track of textures that are used in this frame
    std::vector<std::shared_ptr<RIBuffer_s>> bufferLink; 
    std::vector<RIFree> freelist;
  };

  RIRenderer_s renderer;
  RIDevice_s device;
	RIProgram gui;

  // 1x1 white texture used as the default texture binding when no real
  // texture is available.
  struct RITexture_s whiteTexture2D;
  struct RIDescriptor_s whiteTexture2DBinding;

  // Zero-filled vertex buffer bound into vertex input slots that don't have
  // a real stream — the vertex fetcher reads zeros for those attributes.
  struct RIBuffer_s nulVertexBuffer;

  RISwapchain_s<RI_MAX_SWAPCHAIN_IMAGES> swapchain;
	struct RITextureView_s swapchainView[RI_MAX_SWAPCHAIN_IMAGES];
	struct RITexture_s depthTextures[RI_MAX_SWAPCHAIN_IMAGES];
	struct RITextureView_s depthView[RI_MAX_SWAPCHAIN_IMAGES];
	struct RI_PogoBuffer pogoBuffer[RI_MAX_SWAPCHAIN_IMAGES];

  struct RITexture_s visibilityTexture[RI_MAX_SWAPCHAIN_IMAGES];
  struct RITextureView_s visibilityView[RI_MAX_SWAPCHAIN_IMAGES];

  struct RITexture_s normalTexture[RI_MAX_SWAPCHAIN_IMAGES];
  struct RITextureView_s normalView[RI_MAX_SWAPCHAIN_IMAGES];

	RICommandRingBuffer_s<RI_COMMAND_RING_POOL_COUNT, RI_COMMAND_RING_CMD_PER_POOL> graphicsCmdRing;
	struct RICommandRingElement_s primary;

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
