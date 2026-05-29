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

#include "graphics/PostEffect_ImageTrail.h"

#include "graphics/Graphics.h"
#include "graphics/PostEffectHelpers.h"
#include "graphics/RIBootstrap.h"
#include "graphics/RIProgramHelpers.h"
#include "system/Hasher.h"

#include <cmath>

namespace hpl {

namespace {
struct ImageTrailPushConstants {
    float alpha;
};
} // namespace

cPostEffectType_ImageTrail::cPostEffectType_ImageTrail(cGraphics *apGraphics,
                                                       cResources *apResources)
    : iPostEffectType("ImageTrail", apGraphics, apResources) {
    LoadSlangGraphics(&RI.device, m_updateProgram, apResources,
                      "posteffect_fullscreen.vert.spv",
                      "posteffect_image_trail.frag.spv");
    LoadSlangGraphics(&RI.device, m_blitProgram, apResources,
                      "posteffect_fullscreen.vert.spv",
                      "posteffect_blit.frag.spv");
}

cPostEffectType_ImageTrail::~cPostEffectType_ImageTrail() {}

iPostEffect *
cPostEffectType_ImageTrail::CreatePostEffect(iPostEffectParams *apParams) {
    // cGraphics::CreatePostEffect already calls SetParams on the returned
    // effect — leave initialisation to that single site.
    (void)apParams;
    return hplNew(cPostEffect_ImageTrail, (mpGraphics, mpResources, this));
}

//-----------------------------------------------------------------------

cPostEffect_ImageTrail::cPostEffect_ImageTrail(cGraphics *apGraphics,
                                               cResources *apResources,
                                               iPostEffectType *apType)
    : iPostEffect(apGraphics, apResources, apType),
      mpImageTrailType(static_cast<cPostEffectType_ImageTrail *>(mpType)) {}

cPostEffect_ImageTrail::~cPostEffect_ImageTrail() {
    DestroyPostEffectColorTarget(m_accum);
}

void cPostEffect_ImageTrail::Reset() { mbClearAccum = true; }

void cPostEffect_ImageTrail::OnSetActive(bool abX) {
    if (!abX)
        Reset();
}

namespace {
void EmitAccumBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout,
                      VkImageLayout newLayout,
                      VkPipelineStageFlags2 srcStage,
                      VkAccessFlags2 srcAccess,
                      VkPipelineStageFlags2 dstStage,
                      VkAccessFlags2 dstAccess) {
    VkImageMemoryBarrier2 barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask  = srcStage;
    barrier.srcAccessMask = srcAccess;
    barrier.dstStageMask  = dstStage;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout     = oldLayout;
    barrier.newLayout     = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image         = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0,
                                VK_REMAINING_MIP_LEVELS, 0,
                                VK_REMAINING_ARRAY_LAYERS};
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers    = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);
}
} // namespace

void cPostEffect_ImageTrail::RenderEffect(const PostEffectRenderCtx &ctx) {
    VkCommandBuffer cmd = ctx.cmd->vk.cmd;

    // Lazy-allocate / resize the accumulator. Width / height come from the
    // pogo dimensions (= swapchain dimensions in the current bootstrap).
    if (!m_accum.valid || m_accum.width != ctx.width ||
        m_accum.height != ctx.height) {
        DestroyPostEffectColorTarget(m_accum);
        CreatePostEffectColorTarget(m_accum, ctx.width, ctx.height,
                                    RIBootstrap::PogoColorFormatVk, 0u,
                                    "PostEffect_ImageTrail.accum");
        mbClearAccum = true;
    }

    const RI_Format_e imageTrailFormat = RIBootstrap::PogoColorFormat;
    const VkFormat imageTrailVkFormat = RIBootstrap::PogoColorFormatVk;

    VkViewport viewport = { 0.0f, 0.0f, static_cast<float>(ctx.width), static_cast<float>(ctx.height), 0.0f, 1.0f };
    VkRect2D scissor = { {0, 0}, {ctx.width, ctx.height} };
    RIDescriptor_s* samplerDesc = RI.resolve_filter_descriptor(eTextureWrap_ClampToEdge, eTextureWrap_ClampToEdge, eTextureWrap_ClampToEdge, eTextureFilter_Bilinear);

    // ----- Pass 1: blend new frame into accum (with alpha) -----
    // Layout: SHADER_READ (or UNDEFINED first time) → COLOR_ATTACH
    {
        EmitAccumBarrier(
            cmd, m_accum.texture.vk.image,
            mbClearAccum ? VK_IMAGE_LAYOUT_UNDEFINED
            : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            mbClearAccum ? VK_PIPELINE_STAGE_2_NONE
            : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            mbClearAccum ? VkAccessFlags2{} : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
            VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);

        VkRenderingAttachmentInfo accumAttach = {
            VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        accumAttach.imageView = m_accum.descriptor.vk.image.imageView;
        accumAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        accumAttach.loadOp = mbClearAccum ? VK_ATTACHMENT_LOAD_OP_CLEAR
            : VK_ATTACHMENT_LOAD_OP_LOAD;
        accumAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        accumAttach.clearValue.color = { {0.0f, 0.0f, 0.0f, 1.0f} };

        VkRenderingInfo accumRender = { VK_STRUCTURE_TYPE_RENDERING_INFO };
        accumRender.renderArea = { {0, 0}, {ctx.width, ctx.height} };
        accumRender.layerCount = 1;
        accumRender.colorAttachmentCount = 1;
        accumRender.pColorAttachments = &accumAttach;

        vkCmdBeginRendering(cmd, &accumRender);

        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        PostEffectPipelineState blendState{};
        InitPostEffectPipelineState(blendState, imageTrailVkFormat,
            /*alphaBlend=*/true);

        const hash_t blendHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/1u);
        mpImageTrailType->m_updateProgram.bindPipeline(
            &RI.device, ctx.cmd, blendHash, "PostEffect_ImageTrail.update",
            &blendState.createInfo);



        {
            RIProgram::DescriptorBinding bindings[2] = {};
            bindings[0].descriptor = *samplerDesc;
            bindings[0].handle = DescriptorBindingID::Create("inputSampler");
            bindings[1].descriptor = *ctx.inputSrv;
            bindings[1].handle = DescriptorBindingID::Create("sourceInput");
            mpImageTrailType->m_updateProgram.bindDescriptors(
                &RI.device, ctx.cmd, ctx.frameIndex, bindings, 2);
        }

        ImageTrailPushConstants pc{};
        if (mbClearAccum) {
            pc.alpha = 1.0f; // No history yet; entire output = current frame.
        }
        else if (ctx.frameTime > 0.0f) {
            // Match the legacy curve: alpha = exp(-fPow * 0.015).
            // *30 in the legacy comment is implicit through (1 / frameTime).
            float fPow = (1.0f / ctx.frameTime) * mParams.mfAmount;
            pc.alpha = std::exp(-fPow * 0.015f);
        }
        else {
            pc.alpha = mParams.mfAmount;
        }
        vkCmdPushConstants(cmd,
            mpImageTrailType->m_updateProgram.getPipelineLayout(),
            VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);

        mbClearAccum = false;

        // Accum has been written; flip it to SHADER_READ_ONLY so pass 2 can sample.
        EmitAccumBarrier(
            cmd, m_accum.texture.vk.image,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }

    {
        // ----- Pass 2: blit accum into pogo output -----
        VkRenderingAttachmentInfo outAttach = {
            VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        outAttach.imageView = ctx.outputView;
        outAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        outAttach.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        outAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

        VkRenderingInfo outRender = { VK_STRUCTURE_TYPE_RENDERING_INFO };
        outRender.renderArea = { {0, 0}, {ctx.width, ctx.height} };
        outRender.layerCount = 1;
        outRender.colorAttachmentCount = 1;
        outRender.pColorAttachments = &outAttach;

        vkCmdBeginRendering(cmd, &outRender);

        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        PostEffectPipelineState blitState{};
        InitPostEffectPipelineState(blitState, imageTrailVkFormat,
            /*alphaBlend=*/false);

        const hash_t blitHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/2u);
        mpImageTrailType->m_blitProgram.bindPipeline(
            &RI.device, ctx.cmd, blitHash, "PostEffect_ImageTrail.blit",
            &blitState.createInfo);

        {
            RIProgram::DescriptorBinding bindings[2] = {};
            bindings[0].descriptor = *samplerDesc;
            bindings[0].handle = DescriptorBindingID::Create("inputSampler");
            bindings[1].descriptor = m_accum.descriptor;
            bindings[1].handle = DescriptorBindingID::Create("sourceInput");
            mpImageTrailType->m_blitProgram.bindDescriptors(
                &RI.device, ctx.cmd, ctx.frameIndex, bindings, 2);
        }

        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
    }
}

} // namespace hpl
