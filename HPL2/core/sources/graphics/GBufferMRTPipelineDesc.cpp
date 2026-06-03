#include "graphics/GBufferMRTPipelineDesc.h"

#include "graphics/RIVK.h"     // RIFormatToVK
#include "system/Hasher.h"     // hash_u32 / HASH_INITIAL_VALUE
#include "system/Types.h"      // ARRAY_COUNT

namespace hpl {

GBufferMRTPipelineDesc::GBufferMRTPipelineDesc(RI_Format_e visibilityFormat,
                                               RI_Format_e velocityFormat,
                                               RI_Format_e depthFormat) {
  // VS pulls all per-vertex data via buffer_reference from set 0 SSBOs,
  // so the pipeline declares zero vertex input bindings and attributes.
  vertexInputState = {
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vertexInputState.vertexBindingDescriptionCount = 0;
  vertexInputState.pVertexBindingDescriptions = nullptr;
  vertexInputState.vertexAttributeDescriptionCount = 0;
  vertexInputState.pVertexAttributeDescriptions = nullptr;

  inputAssemblyState = {
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  inputAssemblyState.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  rasterizationState = {
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rasterizationState.polygonMode = VK_POLYGON_MODE_FILL;
  rasterizationState.cullMode = VK_CULL_MODE_BACK_BIT;
  rasterizationState.frontFace = VK_FRONT_FACE_CLOCKWISE;
  rasterizationState.lineWidth = 1.0f;

  dynamicStates[0] = VK_DYNAMIC_STATE_VIEWPORT;
  dynamicStates[1] = VK_DYNAMIC_STATE_SCISSOR;
  dynamicState = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamicState.dynamicStateCount = ARRAY_COUNT(dynamicStates);
  dynamicState.pDynamicStates = dynamicStates;

  // Two MRTs — SV_TARGET0 packed TriangleHit (uint4), SV_TARGET1 velocity (RG16F).
  colorFormats[0] = RIFormatToVK(visibilityFormat);
  colorFormats[1] = RIFormatToVK(velocityFormat);
  pipelineRendering = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  pipelineRendering.colorAttachmentCount = 2;
  pipelineRendering.pColorAttachmentFormats = colorFormats;
  pipelineRendering.depthAttachmentFormat = RIFormatToVK(depthFormat);

  viewportState = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewportState.viewportCount = 1;
  viewportState.scissorCount = 1;

  multisampleState = {
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisampleState.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  depthStencilState = {
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  depthStencilState.depthTestEnable = VK_TRUE;
  depthStencilState.depthWriteEnable = VK_TRUE;
  depthStencilState.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
  depthStencilState.minDepthBounds = 0.0f;
  depthStencilState.maxDepthBounds = 1.0f;

  // Both targets write raw (blendEnable VK_FALSE) — the uint visibility target
  // can't blend, and velocity wants the exact value. Factors stay identity.
  const VkPipelineColorBlendAttachmentState noBlend = {
      VK_FALSE,        VK_BLEND_FACTOR_ONE,     VK_BLEND_FACTOR_ZERO,
      VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE,     VK_BLEND_FACTOR_ZERO,
      VK_BLEND_OP_ADD,
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
  blendAttachments[0] = noBlend;
  blendAttachments[1] = noBlend;
  colorBlendState = {
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  colorBlendState.attachmentCount = 2;
  colorBlendState.pAttachments = blendAttachments;

  createInfo = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  createInfo.pNext = &pipelineRendering;
  createInfo.pVertexInputState = &vertexInputState;
  createInfo.pInputAssemblyState = &inputAssemblyState;
  createInfo.pRasterizationState = &rasterizationState;
  createInfo.pDynamicState = &dynamicState;
  createInfo.pViewportState = &viewportState;
  createInfo.pMultisampleState = &multisampleState;
  createInfo.pDepthStencilState = &depthStencilState;
  createInfo.pColorBlendState = &colorBlendState;

  hash = hash_u32(HASH_INITIAL_VALUE, visibilityFormat);
  hash = hash_u32(hash, velocityFormat);
  hash = hash_u32(hash, depthFormat);
}

} // namespace hpl
