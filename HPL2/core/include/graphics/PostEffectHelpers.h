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
    struct RITexture     texture {};
    struct RITextureView view {};
    struct RIDescriptor  descriptor {};
    uint32_t width = 0;
    uint32_t height = 0;
    bool valid = false;
};

// Allocate `out` with the given dimensions / format and a usage of
// (SAMPLED | COLOR_ATTACHMENT | additionalUsage). additionalUsage covers
// uncommon needs (e.g. TRANSFER_SRC for the ImageTrail accumulator).
// The caller must destroy via DestroyPostEffectColorTarget before exit.
void CreatePostEffectColorTarget(PostEffectColorTarget &out, uint32_t width,
                                 uint32_t height, uint32_t format,
                                 uint32_t additionalUsage,
                                 const char *debugName);

void DestroyPostEffectColorTarget(PostEffectColorTarget &target);

// Fullscreen post-effect pipeline state is now expressed directly with the
// backend-neutral RIGraphicsPipelineDesc at each call site (single color
// attachment, no depth, optional alpha blend); see PostEffect_*.cpp.

} // namespace hpl

#endif
