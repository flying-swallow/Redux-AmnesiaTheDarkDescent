#include "graphics/PostEffectHelpers.h"

#include "graphics/RIBootstrap.h"
#include "graphics/RIRenderer.h"
#include "graphics/RIVK.h"

#include <cstring>

#if (DEVICE_IMPL_VULKAN)
#include <vk_mem_alloc.h>
#endif

namespace hpl {

void CreatePostEffectColorTarget(PostEffectColorTarget &out, uint32_t width,
                                 uint32_t height, uint32_t format,
                                 uint32_t additionalUsage,
                                 const char *debugName) {
    out.width  = width;
    out.height = height;
    out.valid  = false;

    RITextureDesc texDesc = {};
    texDesc.type = RI_TEXTURE_2D;
    texDesc.format = format;
    texDesc.width = width;
    texDesc.height = height;
    texDesc.depth = 1;
    texDesc.mipNum = 1;
    texDesc.layerNum = 1;
    texDesc.usage =
        RI_USAGE_SHADER_RESOURCE | RI_USAGE_COLOR_ATTACHMENT | additionalUsage;
    out.texture = RITexture::create(&RI.device, texDesc);
    if (out.texture.isEmpty(&RI.device))
        return;

    RITextureViewDesc viewDesc = {};
    viewDesc.viewType = RI_VIEWTYPE_SHADER_RESOURCE_2D;
    viewDesc.format = format;
    viewDesc.mipNum = 1;
    viewDesc.layerNum = 1;
    out.view = RITextureView::create(&RI.device, &out.texture, viewDesc);

    out.descriptor = RIDescriptor::sampledImage(&RI.device, &out.view, hash_random());
    out.descriptor.texture = &out.texture;

    if (debugName)
        out.texture.setDebugObjectName(&RI.device, debugName);

    out.valid = true;
}

void DestroyPostEffectColorTarget(PostEffectColorTarget &target) {
    if (!target.valid)
        return;
    // descriptor is a pure payload that owns nothing — no dispose needed.
    target.view.dispose(&RI.device);
    target.texture.dispose(&RI.device);
    target.descriptor = RIDescriptor{};
    target.view       = RITextureView{};
    target.texture    = RITexture{};
    target.valid      = false;
    target.width = target.height = 0;
}

} // namespace hpl
