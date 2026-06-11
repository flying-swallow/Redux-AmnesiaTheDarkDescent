#ifndef HPL_POSTEFFECT_HELPERS_H
#define HPL_POSTEFFECT_HELPERS_H

#include "graphics/RITypes.h"

#if (DEVICE_IMPL_VULKAN)
#include <volk.h>
#endif

#include <cstdint>

namespace hpl {

// Color attachment + sampled scratch target owned by a single post-effect
// instance (e.g. Bloom's two quarter-res blur buffers, ImageTrail's
// accumulator). The descriptor is pre-finalized with type SAMPLED_IMAGE
// so it can be plugged into RIProgram::DescriptorBinding directly.
//
// Lifecycle: create via CreatePostEffectColorTarget, destroy via
// DestroyPostEffectColorTarget. The owner re-creates when the viewport
// dimensions change (compare against `width` / `height`).
struct PostEffectColorTarget {
    struct RITexture    texture {};
    struct RIDescriptor descriptor {};
    uint32_t width = 0;
    uint32_t height = 0;
    bool valid = false;
};

// Allocate `out` with the given dimensions / format and a usage of
// (SAMPLED | COLOR_ATTACHMENT | additionalUsage). additionalUsage covers
// uncommon needs (e.g. TRANSFER_SRC for the ImageTrail accumulator).
// The caller must destroy via DestroyPostEffectColorTarget before exit.
void CreatePostEffectColorTarget(PostEffectColorTarget &out, uint32_t width,
                                 uint32_t height, VkFormat format,
                                 VkImageUsageFlags additionalUsage,
                                 const char *debugName);

void DestroyPostEffectColorTarget(PostEffectColorTarget &target);

// Bundled pipeline-create state for a fullscreen post-effect pass. The
// owner stamps one of these on the stack, calls
// InitPostEffectPipelineState, then passes `state.createInfo` to
// RIProgram::bindPipeline. Inline state arrays must stay alive across
// the bindPipeline call; this struct guarantees that.
struct PostEffectPipelineState {
    VkPipelineVertexInputStateCreateInfo   vertexInput;
    VkPipelineInputAssemblyStateCreateInfo inputAssembly;
    VkPipelineRasterizationStateCreateInfo rasterization;
    VkPipelineViewportStateCreateInfo      viewportState;
    VkPipelineMultisampleStateCreateInfo   multisample;
    VkPipelineDepthStencilStateCreateInfo  depthStencil;
    VkPipelineColorBlendAttachmentState    blendAttachment;
    VkPipelineColorBlendStateCreateInfo    colorBlend;
    VkDynamicState                         dynamicStates[2];
    VkPipelineDynamicStateCreateInfo       dynamicState;
    VkFormat                               colorFormat;
    VkPipelineRenderingCreateInfo          pipelineRendering;
    VkGraphicsPipelineCreateInfo           createInfo;
};

// Fill `state` for a fullscreen post-effect: no vertex input, no
// depth/stencil, cull NONE, dynamic viewport+scissor, single color
// attachment at `colorFormat`. When `alphaBlend` is true the blend
// attachment is configured as (SRC_ALPHA, ONE_MINUS_SRC_ALPHA) for both
// color and alpha; otherwise blend is disabled.
void InitPostEffectPipelineState(PostEffectPipelineState &state,
                                 VkFormat colorFormat, bool alphaBlend);

} // namespace hpl

#endif
