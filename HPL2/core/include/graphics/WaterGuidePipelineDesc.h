#ifndef HPL_WATER_GUIDE_PIPELINE_DESC_H
#define HPL_WATER_GUIDE_PIPELINE_DESC_H

#include "graphics/RITypes.h"   // RI_Format_e

#include "graphics/RIPreamble.h"
#include "graphics/TranslucentMeshPipelineDesc.h"

#include "system/Hasher.h"      // hash_t

#include <cstdint>

namespace hpl {

// Pipeline descriptor for the water reflection-guide raster pass. The guide
// writes three unblended MRTs and borrows the translucent mesh coverage state
// so its vertex layout, rasterization, depth test, dynamic viewport/scissor,
// and multisampling match the final water draw. Copy and move are deleted on
// both descriptors because createInfo keeps every other pXxxState pointer in
// the base's owned sub-structs while the guide-specific pointers use its own
// descriptor storage.
struct WaterGuidePipelineDesc {
  TranslucentMeshPipelineDesc base; // owns borrowed coverage state
  VkFormat colorFormats[3];
  VkPipelineRenderingCreateInfo pipelineRendering;
  VkPipelineColorBlendAttachmentState blendAttachments[3];
  VkPipelineColorBlendStateCreateInfo colorBlendState;
  VkGraphicsPipelineCreateInfo createInfo;
  hash_t hash;

  WaterGuidePipelineDesc(RI_Format_e positionViewZFormat,
                         RI_Format_e normalWeightFormat,
                         RI_Format_e velocityFormat,
                         RI_Format_e depthFormat,
                         uint32_t vertexPresentMask);

  WaterGuidePipelineDesc(const WaterGuidePipelineDesc &) = delete;
  WaterGuidePipelineDesc &operator=(const WaterGuidePipelineDesc &) = delete;
  WaterGuidePipelineDesc(WaterGuidePipelineDesc &&) = delete;
  WaterGuidePipelineDesc &operator=(WaterGuidePipelineDesc &&) = delete;
};

} // namespace hpl

#endif
