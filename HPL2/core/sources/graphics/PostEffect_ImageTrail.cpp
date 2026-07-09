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
#include "graphics/Graphics.h"
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
    LoadSlangGraphics(&mpGraphics->device, m_updateProgram, apResources,
                      "posteffect_fullscreen.vert.spv",
                      "posteffect_image_trail.frag.spv");
    LoadSlangGraphics(&mpGraphics->device, m_blitProgram, apResources,
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


void cPostEffect_ImageTrail::RenderEffect(const PostEffectRenderCtx &ctx) {
    VkCommandBuffer cmd = ctx.cmd->vk.cmd;

    // Lazy-allocate / resize the accumulator. Width / height come from the
    // pogo dimensions (= swapchain dimensions in the current bootstrap).
    if (!m_accum.valid || m_accum.width != ctx.width ||
        m_accum.height != ctx.height) {
        DestroyPostEffectColorTarget(m_accum);
        CreatePostEffectColorTarget(m_accum, ctx.width, ctx.height,
                                    cGraphics::PogoColorFormat, RI_USAGE_NONE,
                                    "PostEffect_ImageTrail.accum");
        mbClearAccum = true;
    }

    const RI_Format_e imageTrailFormat = cGraphics::PogoColorFormat;

    VkViewport viewport = { 0.0f, 0.0f, static_cast<float>(ctx.width), static_cast<float>(ctx.height), 0.0f, 1.0f };
    VkRect2D scissor = { {0, 0}, {ctx.width, ctx.height} };
    auto samplerDesc = mpGraphics->resolve_filter_descriptor(eTextureWrap_ClampToEdge, eTextureWrap_ClampToEdge, eTextureWrap_ClampToEdge, eTextureFilter_Bilinear);

    // ----- Pass 1: blend new frame into accum (with alpha) -----
    // Layout: SHADER_READ (or UNDEFINED first time) → COLOR_ATTACH
    {
        // The blend pass both reads and writes the attachment, hence
        // RENDER_TARGET_READ (color read|write) rather than write-only.
        RITextureBarrier accumToTarget(
            &m_accum.texture,
            mbClearAccum ? RI_RESOURCE_STATE_UNDEFINED
                         : RI_RESOURCE_STATE_SHADER_RESOURCE,
            RI_RESOURCE_STATE_RENDER_TARGET_READ,
            mbClearAccum ? RI_STAGE_NONE : RI_STAGE_FRAGMENT, RI_STAGE_NONE);
        ctx.cmd->vk_d3d12_textureBarrier(accumToTarget);

        RIRenderingAttachment color = {};
        color.view =  m_accum.view;
        color.loadOp = mbClearAccum ? RI_ATTACHMENT_LOAD_OP_CLEAR
                                    : RI_ATTACHMENT_LOAD_OP_LOAD;
        color.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color[0] = 0.0f;
        color.clearValue.color[1] = 0.0f;
        color.clearValue.color[2] = 0.0f;
        color.clearValue.color[3] = 1.0f;

        RIBeginRenderingDesc beginDesc = {};
        beginDesc.renderArea.width = (int16_t)ctx.width;
        beginDesc.renderArea.height = (int16_t)ctx.height;
        beginDesc.colorCount = 1;
        beginDesc.colors = &color;
        ctx.cmd->vk_d3d12_beginRendering(&mpGraphics->device, beginDesc);

        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        PostEffectPipelineState blendState{};
        InitPostEffectPipelineState(blendState, imageTrailFormat,
            /*alphaBlend=*/true);

        const hash_t blendHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/1u);
        mpImageTrailType->m_updateProgram.bindPipeline(
            &mpGraphics->device, ctx.cmd, blendHash, "PostEffect_ImageTrail.update",
            &blendState.createInfo);



        {
            RIProgram::DescriptorBinding bindings[2] = {};
            bindings[0].descriptor = *samplerDesc;
            bindings[0].handle = DescriptorBindingID::Create("inputSampler");
            bindings[1].descriptor = ctx.inputSrv;
            bindings[1].handle = DescriptorBindingID::Create("sourceInput");
            mpImageTrailType->m_updateProgram.bindDescriptors(
                &mpGraphics->device, ctx.cmd, ctx.frameIndex, bindings, 2);
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
        ctx.cmd->vk_d3d12_endRendering(&mpGraphics->device);

        mbClearAccum = false;

        // Accum has been written; flip it to SHADER_READ_ONLY so pass 2 can sample.
        RITextureBarrier accumToSampled(
            &m_accum.texture,
            RI_RESOURCE_STATE_RENDER_TARGET, RI_RESOURCE_STATE_SHADER_RESOURCE,
            RI_STAGE_NONE, RI_STAGE_FRAGMENT);
        ctx.cmd->vk_d3d12_textureBarrier(accumToSampled);
    }

    {
        // ----- Pass 2: blit accum into pogo output -----
        RITextureView outView = {};
        outView.vk.image = ctx.outputView;
        RIRenderingAttachment outColor = {};
        outColor.view = outView;
        outColor.loadOp = RI_ATTACHMENT_LOAD_OP_DONT_CARE;
        outColor.storeOp = RI_ATTACHMENT_STORE_OP_STORE;

        RIBeginRenderingDesc outBeginDesc = {};
        outBeginDesc.renderArea.width = (int16_t)ctx.width;
        outBeginDesc.renderArea.height = (int16_t)ctx.height;
        outBeginDesc.colorCount = 1;
        outBeginDesc.colors = &outColor;
        ctx.cmd->vk_d3d12_beginRendering(&mpGraphics->device, outBeginDesc);

        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        PostEffectPipelineState blitState{};
        InitPostEffectPipelineState(blitState, imageTrailFormat,
            /*alphaBlend=*/false);

        const hash_t blitHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/2u);
        mpImageTrailType->m_blitProgram.bindPipeline(
            &mpGraphics->device, ctx.cmd, blitHash, "PostEffect_ImageTrail.blit",
            &blitState.createInfo);

        {
            RIProgram::DescriptorBinding bindings[2] = {};
            bindings[0].descriptor = *samplerDesc;
            bindings[0].handle = DescriptorBindingID::Create("inputSampler");
            bindings[1].descriptor = m_accum.descriptor();
            bindings[1].handle = DescriptorBindingID::Create("sourceInput");
            mpImageTrailType->m_blitProgram.bindDescriptors(
                &mpGraphics->device, ctx.cmd, ctx.frameIndex, bindings, 2);
        }

        vkCmdDraw(cmd, 3, 1, 0, 0);
        ctx.cmd->vk_d3d12_endRendering(&mpGraphics->device);
    }
}

} // namespace hpl
