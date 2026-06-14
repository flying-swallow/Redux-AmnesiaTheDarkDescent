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

// Explicit per-backend binding tables for the two ImageTrail programs. These
// post-effects are not bindless, so every [vk::binding] is program-managed.
// vk = {name, set, binding, RIDescriptorType_e, count}; mtl = {name, kind,
// index} read off the generated [[fragment]] signature in
// build-mtl/.../posteffect_{image_trail,blit}.frag.metal.
//
// posteffect_image_trail.frag.metal:
//   sourceInput [[texture(0)]], inputSampler [[sampler(0)]], pc [[buffer(0)]]
constexpr RIProgram::RIProgramBinding kUpdate[] = {
    {"inputSampler", RI_DESCRIPTOR_TYPE_SAMPLER, 1, RI_SHADER_STAGE_FRAGMENT, {0, 0}, {}, {0}},
    {"sourceInput", RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, RI_SHADER_STAGE_FRAGMENT, {0, 1}, {}, {0}},
};

// posteffect_blit.frag.metal (no push constant):
//   sourceInput [[texture(0)]], inputSampler [[sampler(0)]]
constexpr RIProgram::RIProgramBinding kBlit[] = {
    {"inputSampler", RI_DESCRIPTOR_TYPE_SAMPLER, 1, RI_SHADER_STAGE_FRAGMENT, {0, 0}, {}, {0}},
    {"sourceInput", RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, RI_SHADER_STAGE_FRAGMENT, {0, 1}, {}, {0}},
};
} // namespace

cPostEffectType_ImageTrail::cPostEffectType_ImageTrail(cGraphics *apGraphics,
                                                       cResources *apResources)
    : iPostEffectType("ImageTrail", apGraphics, apResources) {
    LoadSlangGraphics(&RI.device, m_updateProgram, apResources,
                      "posteffect_fullscreen.vert.spv",
                      "posteffect_image_trail.frag.spv", "vsMain", "psMain", {},
                      kUpdate, sizeof(ImageTrailPushConstants),
                      RI_SHADER_STAGE_FRAGMENT);
    LoadSlangGraphics(&RI.device, m_blitProgram, apResources,
                      "posteffect_fullscreen.vert.spv",
                      "posteffect_blit.frag.spv", "vsMain", "psMain", {},
                      kBlit);
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
void EmitAccumBarrier(RICmd *cmd, RITexture *texture,
                      enum RIResourceState_e before, uint32_t beforeStages,
                      enum RIResourceState_e after, uint32_t afterStages) {
    RITextureBarrier barrier = {};
    barrier.texture = texture;
    barrier.before = before;
    barrier.beforeStages = beforeStages;
    barrier.after = after;
    barrier.afterStages = afterStages;
    cmd->vk_d3d12_textureBarrier(barrier);
}
} // namespace

void cPostEffect_ImageTrail::RenderEffect(const PostEffectRenderCtx &ctx) {
    // Lazy-allocate / resize the accumulator. Width / height come from the
    // pogo dimensions (= swapchain dimensions in the current bootstrap).
    if (!m_accum.valid || m_accum.width != ctx.width ||
        m_accum.height != ctx.height) {
        DestroyPostEffectColorTarget(m_accum);
        CreatePostEffectColorTarget(m_accum, ctx.width, ctx.height,
                                    RIBootstrap::PogoColorFormat, RI_USAGE_NONE,
                                    "PostEffect_ImageTrail.accum");
        mbClearAccum = true;
    }

    const RI_Format_e imageTrailFormat = RIBootstrap::PogoColorFormat;

    RIViewport viewport = {};
    viewport.width    = static_cast<float>(ctx.width);
    viewport.height   = static_cast<float>(ctx.height);
    viewport.depthMax = 1.0f;
    RIRect scissor = {};
    scissor.width  = (int16_t)ctx.width;
    scissor.height = (int16_t)ctx.height;
    auto samplerDesc = RI.resolve_filter_descriptor(eTextureWrap_ClampToEdge, eTextureWrap_ClampToEdge, eTextureWrap_ClampToEdge, eTextureFilter_Bilinear);

    // ----- Pass 1: blend new frame into accum (with alpha) -----
    // Layout: SHADER_READ (or UNDEFINED first time) → COLOR_ATTACH
    {
        // The blend pass both reads and writes the attachment, hence
        // RENDER_TARGET_READ (color read|write) rather than write-only.
        EmitAccumBarrier(
            ctx.cmd, &m_accum.texture,
            mbClearAccum ? RI_RESOURCE_STATE_UNDEFINED
                         : RI_RESOURCE_STATE_SHADER_RESOURCE,
            mbClearAccum ? RI_STAGE_NONE : RI_STAGE_FRAGMENT,
            RI_RESOURCE_STATE_RENDER_TARGET_READ, RI_STAGE_NONE);

        RIRenderingAttachment accumAttach = {};
        accumAttach.view = m_accum.descriptor.view;
        accumAttach.loadOp = mbClearAccum ? RI_ATTACHMENT_LOAD_OP_CLEAR
            : RI_ATTACHMENT_LOAD_OP_LOAD;
        accumAttach.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
        accumAttach.clearValue.color[3] = 1.0f;

        RIBeginRenderingDesc accumRender = {};
        accumRender.renderArea.width  = (int16_t)ctx.width;
        accumRender.renderArea.height = (int16_t)ctx.height;
        accumRender.colorCount = 1;
        accumRender.colors = &accumAttach;
        ctx.cmd->vk_d3d12_beginRendering(&RI.renderer, accumRender);
        ctx.cmd->mtl_encoderDraw(accumRender);

        ctx.cmd->setViewport(&RI.renderer, viewport);
        ctx.cmd->setScissor(&RI.renderer, scissor);

        RIGraphicsPipelineDesc blendState{};
        blendState.colorCount = 1;
        blendState.colors[0].format = imageTrailFormat;
        blendState.colors[0].blendEnabled = true;
        blendState.colors[0].srcColor = RI_BLEND_SRC_ALPHA;
        blendState.colors[0].dstColor = RI_BLEND_ONE_MINUS_SRC_ALPHA;
        blendState.colors[0].colorBlendOp = RI_BLEND_OP_ADD;
        blendState.colors[0].srcAlpha = RI_BLEND_SRC_ALPHA;
        blendState.colors[0].dstAlpha = RI_BLEND_ONE_MINUS_SRC_ALPHA;
        blendState.colors[0].alphaBlendOp = RI_BLEND_OP_ADD;

        const hash_t blendHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/1u);
        mpImageTrailType->m_updateProgram.bindPipeline(
            &RI.device, ctx.cmd, blendHash, "PostEffect_ImageTrail.update",
            blendState);



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
        mpImageTrailType->m_updateProgram.pushConstants(ctx.cmd, &pc, sizeof(pc));

        ctx.cmd->draw(&RI.renderer, 3, 1, 0, 0);
        ctx.cmd->mtl_encoderEnd();
        ctx.cmd->vk_d3d12_endRendering(&RI.renderer);

        mbClearAccum = false;

        // Accum has been written; flip it to SHADER_READ_ONLY so pass 2 can sample.
        EmitAccumBarrier(
            ctx.cmd, &m_accum.texture,
            RI_RESOURCE_STATE_RENDER_TARGET, RI_STAGE_NONE,
            RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT);
    }

    {
        // ----- Pass 2: blit accum into pogo output -----
        RIRenderingAttachment outAttach = {};
        outAttach.view = ctx.outputView;
        outAttach.loadOp = RI_ATTACHMENT_LOAD_OP_DONT_CARE;
        outAttach.storeOp = RI_ATTACHMENT_STORE_OP_STORE;

        RIBeginRenderingDesc outRender = {};
        outRender.renderArea.width  = (int16_t)ctx.width;
        outRender.renderArea.height = (int16_t)ctx.height;
        outRender.colorCount = 1;
        outRender.colors = &outAttach;
        ctx.cmd->vk_d3d12_beginRendering(&RI.renderer, outRender);
        ctx.cmd->mtl_encoderDraw(outRender);

        ctx.cmd->setViewport(&RI.renderer, viewport);
        ctx.cmd->setScissor(&RI.renderer, scissor);

        RIGraphicsPipelineDesc blitState{};
        blitState.colorCount = 1;
        blitState.colors[0].format = imageTrailFormat; // blend disabled (default)

        const hash_t blitHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/2u);
        mpImageTrailType->m_blitProgram.bindPipeline(
            &RI.device, ctx.cmd, blitHash, "PostEffect_ImageTrail.blit",
            blitState);

        {
            RIProgram::DescriptorBinding bindings[2] = {};
            bindings[0].descriptor = *samplerDesc;
            bindings[0].handle = DescriptorBindingID::Create("inputSampler");
            bindings[1].descriptor = m_accum.descriptor;
            bindings[1].handle = DescriptorBindingID::Create("sourceInput");
            mpImageTrailType->m_blitProgram.bindDescriptors(
                &RI.device, ctx.cmd, ctx.frameIndex, bindings, 2);
        }

        ctx.cmd->draw(&RI.renderer, 3, 1, 0, 0);
        ctx.cmd->mtl_encoderEnd();
        ctx.cmd->vk_d3d12_endRendering(&RI.renderer);
    }
}

} // namespace hpl
