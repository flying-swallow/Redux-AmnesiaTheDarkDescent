/*
 * Copyright © 2009-2020 Frictional Games
 * Copyright 2023 Michael Pollind
 *
 * This file is part of Amnesia: The Dark Descent.
 *
 * Amnesia: The Dark Descent is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * Amnesia: The Dark Descent is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Amnesia: The Dark Descent.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "graphics/PostEffect_Bloom.h"

#include "graphics/Graphics.h"
#include "graphics/PostEffectHelpers.h"
#include "graphics/RIBootstrap.h"
#include "graphics/RIProgramHelpers.h"
#include "system/Hasher.h"

#include <algorithm>

namespace hpl {

namespace {
struct BloomBlurPushConstants {
    float blurDir[2];
    float _pad[2];
};

struct BloomAddPushConstants {
    float rgbToIntensity[3];
    float _pad;
};

} // namespace

cPostEffectType_Bloom::cPostEffectType_Bloom(cGraphics *apGraphics,
                                             cResources *apResources)
    : iPostEffectType("Bloom", apGraphics, apResources) {
    LoadSlangGraphics(&RI.device, m_blurProgram, apResources,
                      "posteffect_fullscreen.vert.spv",
                      "posteffect_bloom_blur.frag.spv");
    LoadSlangGraphics(&RI.device, m_addProgram, apResources,
                      "posteffect_fullscreen.vert.spv",
                      "posteffect_bloom_add.frag.spv");
}

cPostEffectType_Bloom::~cPostEffectType_Bloom() {}

iPostEffect *
cPostEffectType_Bloom::CreatePostEffect(iPostEffectParams *apParams) {
    // cGraphics::CreatePostEffect already calls SetParams on the returned
    // effect — leave initialisation to that single site.
    (void)apParams;
    return hplNew(cPostEffect_Bloom, (mpGraphics, mpResources, this));
}

//-----------------------------------------------------------------------

cPostEffect_Bloom::cPostEffect_Bloom(cGraphics *apGraphics,
                                     cResources *apResources,
                                     iPostEffectType *apType)
    : iPostEffect(apGraphics, apResources, apType),
      mpBloomType(static_cast<cPostEffectType_Bloom *>(mpType)) {}

cPostEffect_Bloom::~cPostEffect_Bloom() {
    DestroyPostEffectColorTarget(m_blur[0]);
    DestroyPostEffectColorTarget(m_blur[1]);
}

void cPostEffect_Bloom::RenderEffect(const PostEffectRenderCtx &ctx) {
    VkCommandBuffer cmd = ctx.cmd->vk.cmd;

    const uint32_t blurW = std::max(ctx.width / 4u, 1u);
    const uint32_t blurH = std::max(ctx.height / 4u, 1u);

    // (Re)allocate the quarter-res blur scratch when the viewport changes
    // or on first use.
    if (!m_blurInitialized || m_blur[0].width != blurW ||
        m_blur[0].height != blurH) {
        DestroyPostEffectColorTarget(m_blur[0]);
        DestroyPostEffectColorTarget(m_blur[1]);
        CreatePostEffectColorTarget(m_blur[0], blurW, blurH,
                                    RIBootstrap::PogoColorFormatVk, 0u,
                                    "PostEffect_Bloom.blur0");
        CreatePostEffectColorTarget(m_blur[1], blurW, blurH,
                                    RIBootstrap::PogoColorFormatVk, 0u,
                                    "PostEffect_Bloom.blur1");
        // First-time transitions out of UNDEFINED — set the "rest state":
        // blur[0] = SHADER_READ_ONLY (will flip to ATTACH on first H blur),
        // blur[1] = COLOR_ATTACHMENT  (will flip to READ on first H blur).
        RITextureBarrier blur0Init(&m_blur[0].texture,
                         RI_RESOURCE_STATE_UNDEFINED, RI_RESOURCE_STATE_SHADER_RESOURCE,
                         RI_STAGE_NONE, RI_STAGE_FRAGMENT);
        ctx.cmd->vk_d3d12_textureBarrier(blur0Init);
        RITextureBarrier blur1Init(&m_blur[1].texture,
                         RI_RESOURCE_STATE_UNDEFINED, RI_RESOURCE_STATE_RENDER_TARGET,
                         RI_STAGE_NONE, RI_STAGE_NONE);
        ctx.cmd->vk_d3d12_textureBarrier(blur1Init);
        m_blurInitialized = true;
    }

    auto samplerDesc = RI.resolve_filter_descriptor(
        eTextureWrap_ClampToEdge, eTextureWrap_ClampToEdge,
        eTextureWrap_ClampToEdge, eTextureFilter_Bilinear);

    PostEffectPipelineState blurState{};
    InitPostEffectPipelineState(blurState, RIBootstrap::PogoColorFormatVk, false);
    const hash_t kBlurHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);

    VkViewport blurViewport = {0.0f,
                               0.0f,
                               static_cast<float>(blurW),
                               static_cast<float>(blurH),
                               0.0f,
                               1.0f};
    VkRect2D blurScissor = {{0, 0}, {blurW, blurH}};

    auto bindBlurInput = [&](const RIDescriptor &inputDesc) {
        RIProgram::DescriptorBinding bindings[2] = {};
        bindings[0].descriptor = *samplerDesc;
        bindings[0].handle     = DescriptorBindingID::Create("inputSampler");
        bindings[1].descriptor = inputDesc;
        bindings[1].handle     = DescriptorBindingID::Create("sourceInput");
        mpBloomType->m_blurProgram.bindDescriptors(
            &RI.device, ctx.cmd, ctx.frameIndex, bindings, 2);
    };

    auto blurPass = [&](VkImageView destView, RITexture *destTexture,
                        RITexture *prevDestTexture, const RIDescriptor &inputDesc,
                        float dirX, float dirY) {
        // Layout flip: dest SHADER_READ → COLOR_ATTACH ; prev dest
        // (which we'll sample next time) COLOR_ATTACH → SHADER_READ.
        RITextureBarrier destToTarget(destTexture,
                         RI_RESOURCE_STATE_SHADER_RESOURCE, RI_RESOURCE_STATE_RENDER_TARGET,
                         RI_STAGE_FRAGMENT, RI_STAGE_NONE);
        ctx.cmd->vk_d3d12_textureBarrier(destToTarget);
        RITextureBarrier prevToSampled(prevDestTexture,
                         RI_RESOURCE_STATE_RENDER_TARGET, RI_RESOURCE_STATE_SHADER_RESOURCE,
                         RI_STAGE_NONE, RI_STAGE_FRAGMENT);
        ctx.cmd->vk_d3d12_textureBarrier(prevToSampled);

        RITextureView destViewRi = {};
        destViewRi.vk.image = destView;

        RIRenderingAttachment color = {};
        color.view    = destViewRi;
        color.loadOp  = RI_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.storeOp = RI_ATTACHMENT_STORE_OP_STORE;

        RIBeginRenderingDesc beginDesc = {};
        beginDesc.renderArea.width  = (int16_t)blurW;
        beginDesc.renderArea.height = (int16_t)blurH;
        beginDesc.colorCount = 1;
        beginDesc.colors     = &color;
        ctx.cmd->vk_d3d12_beginRendering(&RI.renderer, beginDesc);

        vkCmdSetViewport(cmd, 0, 1, &blurViewport);
        vkCmdSetScissor(cmd, 0, 1, &blurScissor);

        mpBloomType->m_blurProgram.bindPipeline(&RI.device, ctx.cmd, kBlurHash,
                                                "PostEffect_Bloom.blur",
                                                &blurState.createInfo);
        bindBlurInput(inputDesc);

        BloomBlurPushConstants pc{};
        pc.blurDir[0] = dirX;
        pc.blurDir[1] = dirY;
        vkCmdPushConstants(cmd, mpBloomType->m_blurProgram.getPipelineLayout(),
                           VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

        vkCmdDraw(cmd, 3, 1, 0, 0);
        ctx.cmd->vk_d3d12_endRendering(&RI.renderer);
    };

    // Iterative blur: first iter samples the pogo input, subsequent
    // iterations sample blur[1].
    for (int iter = 0; iter < std::max(mParams.mlBlurIterations, 1); ++iter) {
        const RIDescriptor &firstInput =
            (iter == 0) ? *ctx.inputSrv : m_blur[1].descriptor;

        // Pass H: dest = blur[0], read = firstInput, prevDest = blur[1].
        blurPass(m_blur[0].descriptor.vk.image.imageView,
                 &m_blur[0].texture, &m_blur[1].texture,
                 firstInput, mParams.mfBlurSize, 0.0f);

        // Pass V: dest = blur[1], read = blur[0], prevDest = blur[0].
        blurPass(m_blur[1].descriptor.vk.image.imageView,
                 &m_blur[1].texture, &m_blur[0].texture,
                 m_blur[0].descriptor, 0.0f, mParams.mfBlurSize);
    }

    // Flip blur[1] COLOR_ATTACH → SHADER_READ for the add pass to sample.
    RITextureBarrier blur1ToSampled(&m_blur[1].texture,
                     RI_RESOURCE_STATE_RENDER_TARGET, RI_RESOURCE_STATE_SHADER_RESOURCE,
                     RI_STAGE_NONE, RI_STAGE_FRAGMENT);
    ctx.cmd->vk_d3d12_textureBarrier(blur1ToSampled);

    // ----- Add pass: sample source + blur, write pogo output -----
    RITextureView outView = {};
    outView.vk.image = ctx.outputView;

    RIRenderingAttachment outColor = {};
    outColor.view    = outView;
    outColor.loadOp  = RI_ATTACHMENT_LOAD_OP_DONT_CARE;
    outColor.storeOp = RI_ATTACHMENT_STORE_OP_STORE;

    RIBeginRenderingDesc outBeginDesc = {};
    outBeginDesc.renderArea.width  = (int16_t)ctx.width;
    outBeginDesc.renderArea.height = (int16_t)ctx.height;
    outBeginDesc.colorCount = 1;
    outBeginDesc.colors     = &outColor;
    ctx.cmd->vk_d3d12_beginRendering(&RI.renderer, outBeginDesc);

    VkViewport viewport = {0.0f,
                           0.0f,
                           static_cast<float>(ctx.width),
                           static_cast<float>(ctx.height),
                           0.0f,
                           1.0f};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor = {{0, 0}, {ctx.width, ctx.height}};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    PostEffectPipelineState addState{};
    InitPostEffectPipelineState(addState, RIBootstrap::PogoColorFormatVk, false);
    const hash_t kAddHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/1u);
    mpBloomType->m_addProgram.bindPipeline(&RI.device, ctx.cmd, kAddHash,
                                           "PostEffect_Bloom.add",
                                           &addState.createInfo);

    {
        RIProgram::DescriptorBinding bindings[3] = {};
        bindings[0].descriptor = *samplerDesc;
        bindings[0].handle     = DescriptorBindingID::Create("inputSampler");
        bindings[1].descriptor = *ctx.inputSrv;
        bindings[1].handle     = DescriptorBindingID::Create("sourceInput");
        bindings[2].descriptor = m_blur[1].descriptor;
        bindings[2].handle     = DescriptorBindingID::Create("blurInput");
        mpBloomType->m_addProgram.bindDescriptors(
            &RI.device, ctx.cmd, ctx.frameIndex, bindings, 3);
    }

    BloomAddPushConstants apc{};
    apc.rgbToIntensity[0] = mParams.mvRgbToIntensity.x;
    apc.rgbToIntensity[1] = mParams.mvRgbToIntensity.y;
    apc.rgbToIntensity[2] = mParams.mvRgbToIntensity.z;
    vkCmdPushConstants(cmd, mpBloomType->m_addProgram.getPipelineLayout(),
                       VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(apc), &apc);

    vkCmdDraw(cmd, 3, 1, 0, 0);
    ctx.cmd->vk_d3d12_endRendering(&RI.renderer);

    // Restore the rest state: blur[1] back to COLOR_ATTACH for the next
    // call's swap-and-write pass. blur[0] is already in SHADER_READ_ONLY.
    RITextureBarrier blur1Restore(&m_blur[1].texture,
                     RI_RESOURCE_STATE_SHADER_RESOURCE, RI_RESOURCE_STATE_RENDER_TARGET,
                     RI_STAGE_FRAGMENT, RI_STAGE_NONE);
    ctx.cmd->vk_d3d12_textureBarrier(blur1Restore);
}

} // namespace hpl
