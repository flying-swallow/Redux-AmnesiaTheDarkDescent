#ifndef HPL_GBUFFER_MRT_PIPELINE_DESC_H
#define HPL_GBUFFER_MRT_PIPELINE_DESC_H

#include "graphics/RIProgram.h" // RIGraphicsPipelineDesc
#include "graphics/RITypes.h"   // RI_Format_e
#include "system/Hasher.h"      // hash_t

namespace hpl {

// Backend-neutral static pipeline state for the "SurfelGBuffer.3d" pass. Built
// once from the attachment formats; bound via the neutral
// RIProgram::bindPipeline(..., desc) overload (which caches by `hash`).
struct GBufferMRTPipelineDesc {
  RIGraphicsPipelineDesc desc;
  hash_t hash;

  GBufferMRTPipelineDesc(RI_Format_e visibilityFormat, RI_Format_e velocityFormat,
                         RI_Format_e depthFormat);
};

} // namespace hpl

#endif
