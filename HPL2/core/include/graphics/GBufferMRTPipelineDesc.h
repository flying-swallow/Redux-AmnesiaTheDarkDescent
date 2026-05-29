#ifndef HPL_GBUFFER_MRT_PIPELINE_DESC_H
#define HPL_GBUFFER_MRT_PIPELINE_DESC_H

#include "graphics/RITypes.h"   // RI_Format_e

#if (DEVICE_IMPL_VULKAN)
#include <volk.h>
#endif

#include "system/Hasher.h"      // hash_t

namespace hpl {

// Holder for the static portion of the "SurfelGBuffer.3d"
// VkGraphicsPipelineCreateInfo. Owns every sub-struct so the pointer
// chain stays valid as long as the holder lives. Non-copyable /
// non-movable - the pNext / pXxxState pointers would dangle.
struct GBufferMRTPipelineDesc {
  VkPipelineVertexInputStateCreateInfo vertexInputState;
  VkPipelineInputAssemblyStateCreateInfo inputAssemblyState;
  VkPipelineRasterizationStateCreateInfo rasterizationState;
  VkDynamicState dynamicStates[2];
  VkPipelineDynamicStateCreateInfo dynamicState;
  VkFormat colorFormat;
  VkPipelineRenderingCreateInfo pipelineRendering;
  VkPipelineViewportStateCreateInfo viewportState;
  VkPipelineMultisampleStateCreateInfo multisampleState;
  VkPipelineDepthStencilStateCreateInfo depthStencilState;
  VkPipelineColorBlendAttachmentState blendAttachment;
  VkPipelineColorBlendStateCreateInfo colorBlendState;
  VkGraphicsPipelineCreateInfo createInfo;
  hash_t hash;

  GBufferMRTPipelineDesc(RI_Format_e visibilityFormat, RI_Format_e depthFormat);

  GBufferMRTPipelineDesc(const GBufferMRTPipelineDesc &) = delete;
  GBufferMRTPipelineDesc &operator=(const GBufferMRTPipelineDesc &) = delete;
};

} // namespace hpl

#endif
