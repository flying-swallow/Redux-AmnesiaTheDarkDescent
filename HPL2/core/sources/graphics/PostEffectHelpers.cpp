#include "graphics/PostEffectHelpers.h"

#include "graphics/Graphics.h"
#include "graphics/RISharedPointer.h"
#include "graphics/RIRenderer.h"
#include "graphics/RIVK.h"

#include <cstring>

#if (DEVICE_IMPL_VULKAN)
#include <vk_mem_alloc.h>
#endif

namespace hpl {

void CreatePostEffectColorTarget(PostEffectColorTarget &out, uint32_t width,
                                 uint32_t height, enum RI_Format_e format,
                                 uint32_t additionalUsage,
                                 const char *debugName) {
    cGraphics* pGraphics = Interface<cGraphics>::Get();
    out.width  = width;
    out.height = height;
    out.valid  = false;

    RITextureDesc desc = {};
    desc.type   = RI_TEXTURE_2D;
    desc.format = format;
    desc.width  = width;
    desc.height = height;
    desc.usage  = RI_USAGE_SHADER_RESOURCE | RI_USAGE_COLOR_ATTACHMENT |
                  additionalUsage;
    out.texture = RITexture::create(&pGraphics->device, desc);
    if (out.texture.isEmpty())
        return;

    RITextureViewDesc viewDesc = {};
    viewDesc.viewType = RI_VIEWTYPE_SHADER_RESOURCE_2D;
    viewDesc.format   = format;
    viewDesc.mipNum   = 1;
    viewDesc.layerNum = 1;
    out.view = RITextureView::create(&pGraphics->device, &out.texture, viewDesc);

#if (DEVICE_IMPL_VULKAN)
    if (debugName && vkSetDebugUtilsObjectNameEXT) {
        VkDebugUtilsObjectNameInfoEXT name = {
            VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
        name.objectType   = VK_OBJECT_TYPE_IMAGE;
        name.objectHandle = reinterpret_cast<uint64_t>(out.texture.vk.image);
        name.pObjectName  = debugName;
        vkSetDebugUtilsObjectNameEXT(pGraphics->device.vk.device, &name);
    }
#endif

    out.valid = true;
}

void DestroyPostEffectColorTarget(PostEffectColorTarget &target) {
    cGraphics* pGraphics = Interface<cGraphics>::Get();
#if (DEVICE_IMPL_VULKAN)
    if (!target.valid)
        return;
    pGraphics->graphicsDefer.push(
        RISharedPointer<RITextureView>(&pGraphics->device, target.view));
    pGraphics->graphicsDefer.push(
        RISharedPointer<RITexture>(&pGraphics->device, target.texture));
    target.view    = RITextureView{};
    target.texture = RITexture{};
    target.valid   = false;
    target.width = target.height = 0;
#endif
}

void InitPostEffectPipelineState(PostEffectPipelineState &state,
                                 enum RI_Format_e colorFormat, bool alphaBlend) {
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

    state.colorFormat = RIFormatToVK(colorFormat);
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
