#include "graphics/GBufferMRTPipelineDesc.h"

#include "system/Hasher.h"     // hash_u32 / HASH_INITIAL_VALUE

namespace hpl {

GBufferMRTPipelineDesc::GBufferMRTPipelineDesc(RI_Format_e visibilityFormat,
                                               RI_Format_e velocityFormat,
                                               RI_Format_e depthFormat) {
  // VS pulls all per-vertex data via buffer_reference from set 0 SSBOs, so the
  // pipeline declares zero vertex input bindings/attributes.
  desc = {};
  desc.topology = RI_TOPOLOGY_TRIANGLE_LIST;
  desc.fillMode = RI_FILL_SOLID;
  desc.cullMode = RI_CULL_MODE_BACK;
  desc.frontCounterClockwise = false; // VK_FRONT_FACE_CLOCKWISE

  desc.depthTestEnable = true;
  desc.depthWriteEnable = true;
  desc.depthCompareOp = RI_COMPARE_LESS_EQUAL;
  desc.depthStencilFormat = depthFormat;

  // Two MRTs — SV_TARGET0 packed TriangleHit (uint4), SV_TARGET1 velocity
  // (RG16F). Both write raw (no blend): the uint visibility target can't blend
  // and velocity wants the exact value.
  desc.colorCount = 2;
  desc.colors[0].format = visibilityFormat;
  desc.colors[0].blendEnabled = false;
  desc.colors[0].writeMask = RI_COLOR_WRITE_RGBA;
  desc.colors[1].format = velocityFormat;
  desc.colors[1].blendEnabled = false;
  desc.colors[1].writeMask = RI_COLOR_WRITE_RGBA;

  hash = hash_u32(HASH_INITIAL_VALUE, visibilityFormat);
  hash = hash_u32(hash, velocityFormat);
  hash = hash_u32(hash, depthFormat);
}

} // namespace hpl
