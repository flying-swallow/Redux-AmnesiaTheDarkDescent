#include "graphics/WaterGuidePipelineDesc.h"

#include "graphics/RIVK.h"     // RIFormatToVK
#include "system/Hasher.h"     // hash_u32 / HASH_INITIAL_VALUE

namespace hpl {

WaterGuidePipelineDesc::WaterGuidePipelineDesc(
    RI_Format_e positionViewZFormat, RI_Format_e normalWeightFormat,
    RI_Format_e velocityFormat, RI_Format_e depthFormat,
    uint32_t vertexPresentMask)
    : base(positionViewZFormat, depthFormat,
           // The guide replaces base's color/rendering and blend pointers;
           // this color format and blend mode only satisfy the borrowed
           // translucent coverage-state constructor.
           TranslucentMeshPipelineDesc::BLEND_ADD, vertexPresentMask) {
  colorFormats[0] = RIFormatToVK(positionViewZFormat);
  colorFormats[1] = RIFormatToVK(normalWeightFormat);
  colorFormats[2] = RIFormatToVK(velocityFormat);
  pipelineRendering = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  pipelineRendering.colorAttachmentCount = 3;
  pipelineRendering.pColorAttachmentFormats = colorFormats;
  pipelineRendering.depthAttachmentFormat = RIFormatToVK(depthFormat);

  const VkPipelineColorBlendAttachmentState noBlend = {
      VK_FALSE,        VK_BLEND_FACTOR_ONE,     VK_BLEND_FACTOR_ZERO,
      VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE,     VK_BLEND_FACTOR_ZERO,
      VK_BLEND_OP_ADD,
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
  blendAttachments[0] = noBlend;
  blendAttachments[1] = noBlend;
  blendAttachments[2] = noBlend;
  colorBlendState = {
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  colorBlendState.attachmentCount = 3;
  colorBlendState.pAttachments = blendAttachments;

  // Copy the base chain so every pXxxState pointer remains directed at the
  // base's owned state. Only rendering and blending are guide-specific.
  createInfo = base.createInfo;
  createInfo.pNext = &pipelineRendering;
  createInfo.pColorBlendState = &colorBlendState;

  hash = hash_u32(HASH_INITIAL_VALUE, 0x57474944u /*'WGID'*/);
  hash = hash_u32(hash, (uint32_t)positionViewZFormat);
  hash = hash_u32(hash, (uint32_t)normalWeightFormat);
  hash = hash_u32(hash, (uint32_t)velocityFormat);
  hash = hash_u32(hash, (uint32_t)depthFormat);
  hash = hash_u32(hash, vertexPresentMask);
}

} // namespace hpl
