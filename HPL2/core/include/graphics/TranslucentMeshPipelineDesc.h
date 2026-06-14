#ifndef HPL_TRANSLUCENT_MESH_PIPELINE_DESC_H
#define HPL_TRANSLUCENT_MESH_PIPELINE_DESC_H

#include "graphics/RIProgram.h" // RIGraphicsPipelineDesc
#include "graphics/RITypes.h"   // RI_Format_e
#include "system/Hasher.h"      // hash_t

#include <cstdint>

namespace hpl {

// Backend-neutral pipeline state for the non-particle translucent (mesh) pass.
// Same as the particle desc (depth read-only ≤, hardware blend per BlendMode,
// clockwise front face under the Y-flipped viewport) except cull is BACK
// (meshes have orientation). Unlike the opaque/particle pipelines this one uses
// fixed-function vertex bindings (see Translucent.vert.slang).
struct TranslucentMeshPipelineDesc {
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

  // vertexPresentMask is a bitset of eVertexElementFlag_* naming the optional
  // streams the renderable supplies; absent streams get their binding stride
  // zeroed (single-vertex fallback) and the mask folds into `hash`.
  TranslucentMeshPipelineDesc(RI_Format_e colorFormat, RI_Format_e depthFormat,
                              BlendMode mode, uint32_t vertexPresentMask);
};

} // namespace hpl

#endif
