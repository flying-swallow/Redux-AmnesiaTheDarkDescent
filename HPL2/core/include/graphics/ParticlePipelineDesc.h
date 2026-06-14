#ifndef HPL_PARTICLE_PIPELINE_DESC_H
#define HPL_PARTICLE_PIPELINE_DESC_H

#include "graphics/RIProgram.h" // RIGraphicsPipelineDesc
#include "graphics/RITypes.h"   // RI_Format_e
#include "system/Hasher.h"      // hash_t

#include <cstdint>

namespace hpl {

// Backend-neutral pipeline state for the particle (translucent) pass. One
// instance per blend mode — the blend factors come from the legacy
// translucencyBlendTable mapping. Depth test on, depth write off (particles
// sort against opaque without occluding each other); cull NONE (billboards face
// either way); no vertex input (VS pulls via BDA from opaque*Handles[]).
struct ParticlePipelineDesc {
  RIGraphicsPipelineDesc desc;
  hash_t hash;

  enum BlendMode : uint32_t {
    BLEND_ADD = 0,
    BLEND_MUL = 1,
    BLEND_MULX2 = 2,
    BLEND_ALPHA = 3,
    BLEND_PREMUL_ALPHA = 4,
    BLEND_LAST_ENUM = 5,
  };

  ParticlePipelineDesc(RI_Format_e swapchainFormat, RI_Format_e depthFormat,
                       BlendMode mode);
};

} // namespace hpl

#endif
