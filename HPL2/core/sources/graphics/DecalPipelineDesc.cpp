#include "graphics/DecalPipelineDesc.h"

#include "graphics/RIVK.h"     // RIFormatToVK
#include "system/Hasher.h"     // hash_u32 / HASH_INITIAL_VALUE
#include "system/Types.h"      // ARRAY_COUNT

namespace hpl {

DecalPipelineDesc::DecalPipelineDesc(RI_Format_e colorFormat,
                                     RI_Format_e depthFormat, BlendMode mode) {
  // Strides match VertexBuffer_RI (and TranslucentMeshPipelineDesc):
  // position/tangent/color stored as float4 (16 B), normal as float3 (12 B),
  // texcoord as float3 with only .xy consumed (stride 12 B, R32G32 reads the
  // first two floats).
  vertexBindings[0] = {0, 16, VK_VERTEX_INPUT_RATE_VERTEX}; // position
  vertexBindings[1] = {1, 12, VK_VERTEX_INPUT_RATE_VERTEX}; // normal (unused)
  vertexBindings[2] = {2, 16, VK_VERTEX_INPUT_RATE_VERTEX}; // tangent (unused)
  vertexBindings[3] = {3, 16, VK_VERTEX_INPUT_RATE_VERTEX}; // color
  vertexBindings[4] = {4, 12, VK_VERTEX_INPUT_RATE_VERTEX}; // texcoord
  vertexAttributes[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT,    0}; // POSITION
  vertexAttributes[1] = {1, 1, VK_FORMAT_R32G32B32_SFLOAT,    0}; // NORMAL
  vertexAttributes[2] = {2, 2, VK_FORMAT_R32G32B32A32_SFLOAT, 0}; // TANGENT
  vertexAttributes[3] = {3, 3, VK_FORMAT_R32G32B32A32_SFLOAT, 0}; // COLOR
  vertexAttributes[4] = {4, 4, VK_FORMAT_R32G32_SFLOAT,       0}; // TEXCOORD0
  vertexInputState = {
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vertexInputState.vertexBindingDescriptionCount = ARRAY_COUNT(vertexBindings);
  vertexInputState.pVertexBindingDescriptions = vertexBindings;
  vertexInputState.vertexAttributeDescriptionCount = ARRAY_COUNT(vertexAttributes);
  vertexInputState.pVertexAttributeDescriptions = vertexAttributes;

  inputAssemblyState = {
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  inputAssemblyState.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  rasterizationState = {
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rasterizationState.polygonMode = VK_POLYGON_MODE_FILL;
  // NONE: decals are thin clipped meshes with arbitrary winding — draw both
  // faces so they never vanish. frontFace stays CLOCKWISE to match the
  // GBuffer/translucent passes under the same Y-flipped viewport (irrelevant
  // while culling is off, but kept consistent if it's later tightened).
  rasterizationState.cullMode = VK_CULL_MODE_NONE;
  rasterizationState.frontFace = VK_FRONT_FACE_CLOCKWISE;
  rasterizationState.lineWidth = 1.0f;

  dynamicStates[0] = VK_DYNAMIC_STATE_VIEWPORT;
  dynamicStates[1] = VK_DYNAMIC_STATE_SCISSOR;
  dynamicState = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamicState.dynamicStateCount = ARRAY_COUNT(dynamicStates);
  dynamicState.pDynamicStates = dynamicStates;

  colorFormats[0] = RIFormatToVK(colorFormat);
  pipelineRendering = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  pipelineRendering.colorAttachmentCount = 1;
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
  depthStencilState.depthWriteEnable = VK_FALSE;
  depthStencilState.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
  depthStencilState.minDepthBounds = 0.0f;
  depthStencilState.maxDepthBounds = 1.0f;

  blendAttachment = {};
  blendAttachment.blendEnable = VK_TRUE;
  // RGB only — decals must not write scene alpha into the post-lighting pogo
  // buffer (matches the reference renderer's decal blend).
  blendAttachment.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
      VK_COLOR_COMPONENT_B_BIT;
  blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
  blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
  switch (mode) {
  case BLEND_ADD:
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    break;
  case BLEND_MUL:
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    break;
  case BLEND_MULX2:
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
    blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_DST_ALPHA;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    break;
  case BLEND_ALPHA:
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendAttachment.dstColorBlendFactor =
        VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendAttachment.dstAlphaBlendFactor =
        VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    break;
  case BLEND_PREMUL_ALPHA:
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstColorBlendFactor =
        VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstAlphaBlendFactor =
        VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    break;
  default:
    break;
  }
  colorBlendState = {
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  colorBlendState.attachmentCount = 1;
  colorBlendState.pAttachments = &blendAttachment;

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

  // Pipeline cache lives on the program (RIProgram::PipelineSlot); m_decal is
  // its own program so (colorFormat, depthFormat, mode) collisions with other
  // programs are impossible.
  hash = hash_u32(HASH_INITIAL_VALUE, (uint32_t)colorFormat);
  hash = hash_u32(hash, (uint32_t)depthFormat);
  hash = hash_u32(hash, (uint32_t)mode);
}

} // namespace hpl
