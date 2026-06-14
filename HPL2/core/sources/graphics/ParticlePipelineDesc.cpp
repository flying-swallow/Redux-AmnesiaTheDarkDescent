#include "graphics/ParticlePipelineDesc.h"

#include "system/Hasher.h"     // hash_u32 / HASH_INITIAL_VALUE

namespace hpl {

ParticlePipelineDesc::ParticlePipelineDesc(RI_Format_e swapchainFormat,
                                           RI_Format_e depthFormat,
                                           BlendMode mode) {
  desc = {};
  desc.topology = RI_TOPOLOGY_TRIANGLE_LIST;
  desc.fillMode = RI_FILL_SOLID;
  desc.cullMode = RI_CULL_MODE_NONE;
  desc.frontCounterClockwise = false; // VK_FRONT_FACE_CLOCKWISE

  desc.depthTestEnable = true;
  desc.depthWriteEnable = false;
  desc.depthCompareOp = RI_COMPARE_LESS_EQUAL;
  desc.depthStencilFormat = depthFormat;

  // Match the legacy translucencyBlendTable mapping. The FS applies per-pixel
  // color transforms that prepare its output for these hardware factors.
  desc.colorCount = 1;
  RIColorAttachmentDesc &c = desc.colors[0];
  c.format = swapchainFormat;
  c.blendEnabled = true;
  c.colorBlendOp = RI_BLEND_OP_ADD;
  c.alphaBlendOp = RI_BLEND_OP_ADD;
  c.writeMask = RI_COLOR_WRITE_RGBA;
  switch (mode) {
  case BLEND_ADD:
    c.srcColor = RI_BLEND_ONE; c.dstColor = RI_BLEND_ONE;
    c.srcAlpha = RI_BLEND_ONE; c.dstAlpha = RI_BLEND_ONE;
    break;
  case BLEND_MUL:
    c.srcColor = RI_BLEND_ZERO; c.dstColor = RI_BLEND_SRC_COLOR;
    c.srcAlpha = RI_BLEND_ZERO; c.dstAlpha = RI_BLEND_ONE;
    break;
  case BLEND_MULX2:
    c.srcColor = RI_BLEND_DST_COLOR; c.dstColor = RI_BLEND_SRC_COLOR;
    c.srcAlpha = RI_BLEND_ONE; c.dstAlpha = RI_BLEND_ONE;
    break;
  case BLEND_ALPHA:
  case BLEND_PREMUL_ALPHA:
    // Premultiplied: the shader outputs rgb·pow(α,k) and a = 1−pow(1−α,k) so
    // the powered weights approximate the legacy lerp in the linear HDR target.
    c.srcColor = RI_BLEND_ONE; c.dstColor = RI_BLEND_ONE_MINUS_SRC_ALPHA;
    c.srcAlpha = RI_BLEND_ONE; c.dstAlpha = RI_BLEND_ONE_MINUS_SRC_ALPHA;
    break;
  default:
    break;
  }

  hash = hash_u32(HASH_INITIAL_VALUE, swapchainFormat);
  hash = hash_u32(hash, depthFormat);
  hash = hash_u32(hash, (uint32_t)mode);
}

} // namespace hpl
