#ifndef HPL_DECAL_PIPELINE_DESC_H
#define HPL_DECAL_PIPELINE_DESC_H

#include "graphics/RITypes.h"   // RI_Format_e

#if (DEVICE_IMPL_VULKAN)
#include <volk.h>
#endif

#include "system/Hasher.h"      // hash_t

#include <cstdint>

namespace hpl {

// Pipeline for the decal overlay pass. Structurally identical to
// TranslucentMeshPipelineDesc (same 5-stream fixed-function vertex layout,
// depth read-only ≤, hardware blend per BlendMode, Y-flipped viewport so the
// CLOCKWISE front face matches the GBuffer) with one divergence:
//   - cullMode is NONE: decals are thin clipped meshes that can be
//     single-sided with arbitrary winding, so both faces must draw to
//     guarantee visibility. (Tighten to BACK only if z-fighting appears.)
// The decal fragment shader emits raw diffuse × vertex colour and relies on
// the hardware blend state below, matching decal.frag.fsl.
struct DecalPipelineDesc {
  // Order + formats MUST match the input struct in Decal.vert.slang (which
  // reuses the translucent 5-stream layout — position / normal / tangent /
  // color / texcoord; normal + tangent are bound but ignored by the shader).
  VkVertexInputBindingDescription   vertexBindings[5];
  VkVertexInputAttributeDescription vertexAttributes[5];
  VkPipelineVertexInputStateCreateInfo vertexInputState;
  VkPipelineInputAssemblyStateCreateInfo inputAssemblyState;
  VkPipelineRasterizationStateCreateInfo rasterizationState;
  VkDynamicState dynamicStates[2];
  VkPipelineDynamicStateCreateInfo dynamicState;
  VkFormat colorFormats[1];
  VkPipelineRenderingCreateInfo pipelineRendering;
  VkPipelineViewportStateCreateInfo viewportState;
  VkPipelineMultisampleStateCreateInfo multisampleState;
  VkPipelineDepthStencilStateCreateInfo depthStencilState;
  VkPipelineColorBlendAttachmentState blendAttachment;
  VkPipelineColorBlendStateCreateInfo colorBlendState;
  VkGraphicsPipelineCreateInfo createInfo;
  hash_t hash;

  // Same enum layout as TranslucentMeshPipelineDesc::BlendMode so the
  // eMaterialBlendMode -> pipeline-variant remap is directly reusable.
  enum BlendMode : uint32_t {
    BLEND_ADD = 0,
    BLEND_MUL = 1,
    BLEND_MULX2 = 2,
    BLEND_ALPHA = 3,
    BLEND_PREMUL_ALPHA = 4,
    BLEND_LAST_ENUM = 5,
  };

  // vertexPresentMask: bitset of eVertexElementFlag_* for the optional streams
  // (Normal / Texture1 tangent / Color0 / Texture0) the renderable supplies.
  // Absent streams get their binding stride zeroed (single-vertex fallback);
  // the mask folds into `hash` so RIProgram caches one pipeline per combination.
  DecalPipelineDesc(RI_Format_e colorFormat, RI_Format_e depthFormat,
                    BlendMode mode, uint32_t vertexPresentMask);

  DecalPipelineDesc(const DecalPipelineDesc &) = delete;
  DecalPipelineDesc &operator=(const DecalPipelineDesc &) = delete;
};

} // namespace hpl

#endif
