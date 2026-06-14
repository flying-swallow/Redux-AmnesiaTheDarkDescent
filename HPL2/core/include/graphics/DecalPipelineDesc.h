#ifndef HPL_DECAL_PIPELINE_DESC_H
#define HPL_DECAL_PIPELINE_DESC_H

#include "graphics/RIProgram.h" // RIGraphicsPipelineDesc
#include "graphics/RITypes.h"   // RI_Format_e
#include "system/Hasher.h"      // hash_t

#include <cstdint>

namespace hpl {

// Backend-neutral pipeline state for the decal overlay pass. Structurally
// identical to TranslucentMeshPipelineDesc (same 5-stream vertex layout, depth
// read-only ≤, hardware blend per BlendMode) except cull is NONE (thin clipped
// meshes must draw both faces) and the write mask is RGB only (decals must not
// write scene alpha into the post-lighting pogo buffer).
struct DecalPipelineDesc {
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

  // vertexPresentMask: bitset of eVertexElementFlag_* for the optional streams
  // the renderable supplies; absent streams get their stride zeroed and the
  // mask folds into `hash`.
  DecalPipelineDesc(RI_Format_e colorFormat, RI_Format_e depthFormat,
                    BlendMode mode, uint32_t vertexPresentMask);
};

} // namespace hpl

#endif
