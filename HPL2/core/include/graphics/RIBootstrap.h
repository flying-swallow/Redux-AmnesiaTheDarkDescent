
#ifndef HPL_GRAPHICS_BACKEND_H
#define HPL_GRAPHICS_BACKEND_H

#include "graphics/GraphicsTypes.h"
#include "graphics/RISegmentAlloc.h"
#include "graphics/RITypes.h"
#include <array>

#include <graphics/RIResourceUploader.h>
#include <graphics/RIScratchAlloc.h>
#include <graphics/RIProgram.h>
#include <graphics/RINulTexture.h>
#include <graphics/RIPogoBuffer.h>
#include <memory>

namespace hpl {
struct HPLTexture;

//bootstrap implementation
struct RIBootstrap {
public:
  explicit RIBootstrap() {

  }
  struct FrameContext {
    struct RIScratchAlloc_s uboScratchAlloc;
    struct RIDescriptor_s colorAttachment;
    std::vector<std::shared_ptr<HPLTexture>> textureLink; // keep track of textures that are used in this frame
    std::vector<RIFree> freelist;
  };

  RIRenderer_s renderer;
  RIDevice_s device;
  RISwapchain_s swapchain;
	RIProgram gui;

  struct RINulTexture whiteTexture2D;

  RI_Format_e depthFormat;
	struct RIDescriptor_s colorAttachment[RI_MAX_SWAPCHAIN_IMAGES];
	struct RITexture_s depthTextures[RI_MAX_SWAPCHAIN_IMAGES];
	struct RITextureView_s depthView[RI_MAX_SWAPCHAIN_IMAGES];
	struct RI_PogoBuffer pogoBuffer[RI_MAX_SWAPCHAIN_IMAGES];

	struct RICommandRingBuffer_s graphicsCmdRing;
	struct RICommandRingElement_s primary;

  struct RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS> guiVertexAlloc;
  RIBuffer_s guiVertexBuffer; 
  struct RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS> guiIndexAlloc;
  RIBuffer_s guiIndexBuffer;

  std::array<FrameContext, RI_NUMBER_FRAMES_FLIGHT> frameSets;
	std::array<RIDescriptor_s, 1024> cachedFilters; 
  uint32_t swapchainIndex;
  uint64_t frameIndex = 0;

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
