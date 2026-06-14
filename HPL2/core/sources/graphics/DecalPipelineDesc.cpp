#include "graphics/DecalPipelineDesc.h"

#include "graphics/GraphicsTypes.h" // eVertexElementFlag_*
#include "system/Hasher.h"     // hash_u32 / HASH_INITIAL_VALUE

namespace hpl {

DecalPipelineDesc::DecalPipelineDesc(RI_Format_e colorFormat,
                                     RI_Format_e depthFormat, BlendMode mode,
                                     uint32_t vertexPresentMask) {
  desc = {};
  desc.topology = RI_TOPOLOGY_TRIANGLE_LIST;
  desc.fillMode = RI_FILL_SOLID;
  // NONE: decals are thin clipped meshes with arbitrary winding — draw both
  // faces so they never vanish.
  desc.cullMode = RI_CULL_MODE_NONE;
  desc.frontCounterClockwise = false; // VK_FRONT_FACE_CLOCKWISE

  desc.depthTestEnable = true;
  desc.depthWriteEnable = false;
  desc.depthCompareOp = RI_COMPARE_LESS_EQUAL;
  desc.depthStencilFormat = depthFormat;

  // Same 5-stream layout as the translucent mesh pass.
  desc.vertexStreamCount = 5;
  desc.vertexStreams[0] = {0, 16, false}; // position (always present)
  desc.vertexStreams[1] = {1, (vertexPresentMask & eVertexElementFlag_Normal)   ? 12u : 0u, false};
  desc.vertexStreams[2] = {2, (vertexPresentMask & eVertexElementFlag_Texture1) ? 16u : 0u, false};
  desc.vertexStreams[3] = {3, (vertexPresentMask & eVertexElementFlag_Color0)   ? 16u : 0u, false};
  desc.vertexStreams[4] = {4, (vertexPresentMask & eVertexElementFlag_Texture0) ? 12u : 0u, false};
  desc.vertexAttributeCount = 5;
  desc.vertexAttributes[0] = {0, 0, RI_FORMAT_RGB32_SFLOAT,  0}; // POSITION
  desc.vertexAttributes[1] = {1, 1, RI_FORMAT_RGB32_SFLOAT,  0}; // NORMAL
  desc.vertexAttributes[2] = {2, 2, RI_FORMAT_RGBA32_SFLOAT, 0}; // TANGENT
  desc.vertexAttributes[3] = {3, 3, RI_FORMAT_RGBA32_SFLOAT, 0}; // COLOR
  desc.vertexAttributes[4] = {4, 4, RI_FORMAT_RG32_SFLOAT,   0}; // TEXCOORD0

  desc.colorCount = 1;
  RIColorAttachmentDesc &c = desc.colors[0];
  c.format = colorFormat;
  c.blendEnabled = true;
  c.colorBlendOp = RI_BLEND_OP_ADD;
  c.alphaBlendOp = RI_BLEND_OP_ADD;
  // RGB only — decals must not write scene alpha into the post-lighting pogo.
  c.writeMask = RI_COLOR_WRITE_RGB;
  switch (mode) {
  case BLEND_ADD:
    c.srcColor = RI_BLEND_ONE; c.dstColor = RI_BLEND_ONE;
    c.srcAlpha = RI_BLEND_ONE; c.dstAlpha = RI_BLEND_ONE;
    break;
  case BLEND_MUL:
    c.srcColor = RI_BLEND_ZERO; c.dstColor = RI_BLEND_SRC_COLOR;
    c.srcAlpha = RI_BLEND_ZERO; c.dstAlpha = RI_BLEND_SRC_ALPHA;
    break;
  case BLEND_MULX2:
    c.srcColor = RI_BLEND_DST_COLOR; c.dstColor = RI_BLEND_SRC_COLOR;
    c.srcAlpha = RI_BLEND_DST_ALPHA; c.dstAlpha = RI_BLEND_SRC_ALPHA;
    break;
  case BLEND_ALPHA:
  case BLEND_PREMUL_ALPHA:
    c.srcColor = RI_BLEND_ONE; c.dstColor = RI_BLEND_ONE_MINUS_SRC_ALPHA;
    c.srcAlpha = RI_BLEND_ONE; c.dstAlpha = RI_BLEND_ONE_MINUS_SRC_ALPHA;
    break;
  default:
    break;
  }

  hash = hash_u32(HASH_INITIAL_VALUE, (uint32_t)colorFormat);
  hash = hash_u32(hash, (uint32_t)depthFormat);
  hash = hash_u32(hash, (uint32_t)mode);
  hash = hash_u32(hash, vertexPresentMask);
}

} // namespace hpl
