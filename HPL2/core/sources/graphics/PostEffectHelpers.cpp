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
                                 uint32_t height, VkFormat format,
                                 VkImageUsageFlags additionalUsage,
                                 const char *debugName) {
    out.width  = width;
    out.height = height;
    out.valid  = false;

#if (DEVICE_IMPL_VULKAN)
    VkImageCreateInfo imageInfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType   = VK_IMAGE_TYPE_2D;
    imageInfo.format      = format;
    imageInfo.extent      = {width, height, 1};
    imageInfo.mipLevels   = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples     = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling      = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | additionalUsage;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo memReqs = {};
    memReqs.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    VK_WrapResult(vmaCreateImage(RI.device.vk.vmaAllocator, &imageInfo,
                                 &memReqs, &out.texture.vk.image,
                                 &out.texture.vk.allocation, nullptr));

    VkImageViewCreateInfo viewInfo = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image    = out.texture.vk.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format   = format;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    out.descriptor = RIDescriptor_s{};
    out.descriptor.flags |= RI_VK_DESC_OWN_IMAGE_VIEW;
    out.descriptor.texture = &out.texture;
    out.descriptor.vk.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    out.descriptor.vk.image.imageLayout =
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VK_WrapResult(vkCreateImageView(RI.device.vk.device, &viewInfo, nullptr,
                                    &out.descriptor.vk.image.imageView));
    out.descriptor.finalize(&RI.device);

    if (debugName && vkSetDebugUtilsObjectNameEXT) {
        VkDebugUtilsObjectNameInfoEXT name = {
            VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
        name.objectType   = VK_OBJECT_TYPE_IMAGE;
        name.objectHandle = reinterpret_cast<uint64_t>(out.texture.vk.image);
        name.pObjectName  = debugName;
        vkSetDebugUtilsObjectNameEXT(RI.device.vk.device, &name);
    }

    out.valid = true;
#endif
}

void DestroyPostEffectColorTarget(PostEffectColorTarget &target) {
#if (DEVICE_IMPL_VULKAN)
    if (!target.valid)
        return;
    if (target.descriptor.vk.image.imageView != VK_NULL_HANDLE) {
        vkDestroyImageView(RI.device.vk.device,
                           target.descriptor.vk.image.imageView, nullptr);
    }
    if (target.texture.vk.image != VK_NULL_HANDLE) {
        vmaDestroyImage(RI.device.vk.vmaAllocator, target.texture.vk.image,
                        target.texture.vk.allocation);
    }
    target.descriptor = RIDescriptor_s{};
    target.texture    = RITexture_s{};
    target.valid      = false;
    target.width = target.height = 0;
#endif
}

void InitPostEffectPipelineState(PostEffectPipelineState &state,
                                 VkFormat colorFormat, bool alphaBlend) {
    state.vertexInput = {
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};

    state.inputAssembly = {
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    state.inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    state.rasterization = {
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    state.rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    state.rasterization.cullMode    = VK_CULL_MODE_NONE;
    state.rasterization.lineWidth   = 1.0f;

    state.viewportState = {
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    state.viewportState.viewportCount = 1;
    state.viewportState.scissorCount  = 1;

    state.multisample = {
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    state.multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    state.depthStencil = {
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};

    state.blendAttachment = {};
    state.blendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (alphaBlend) {
        state.blendAttachment.blendEnable = VK_TRUE;
        state.blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        state.blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
        state.blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        state.blendAttachment.dstColorBlendFactor =
            VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        state.blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        state.blendAttachment.dstAlphaBlendFactor =
            VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    }
    state.colorBlend = {
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    state.colorBlend.attachmentCount = 1;
    state.colorBlend.pAttachments    = &state.blendAttachment;

    state.dynamicStates[0] = VK_DYNAMIC_STATE_VIEWPORT;
    state.dynamicStates[1] = VK_DYNAMIC_STATE_SCISSOR;
    state.dynamicState = {
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    state.dynamicState.dynamicStateCount = 2;
    state.dynamicState.pDynamicStates    = state.dynamicStates;

    state.colorFormat = colorFormat;
    state.pipelineRendering = {
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    state.pipelineRendering.colorAttachmentCount    = 1;
    state.pipelineRendering.pColorAttachmentFormats = &state.colorFormat;

    state.createInfo = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    state.createInfo.pNext               = &state.pipelineRendering;
    state.createInfo.pVertexInputState   = &state.vertexInput;
    state.createInfo.pInputAssemblyState = &state.inputAssembly;
    state.createInfo.pRasterizationState = &state.rasterization;
    state.createInfo.pViewportState      = &state.viewportState;
    state.createInfo.pMultisampleState   = &state.multisample;
    state.createInfo.pDepthStencilState  = &state.depthStencil;
    state.createInfo.pColorBlendState    = &state.colorBlend;
    state.createInfo.pDynamicState       = &state.dynamicState;
}

} // namespace hpl
