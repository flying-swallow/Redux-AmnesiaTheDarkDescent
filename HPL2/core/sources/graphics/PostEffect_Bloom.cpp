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

// Explicit per-backend binding tables for the two Bloom programs. These
// post-effects are not bindless, so every [vk::binding] is program-managed.
// vk = {name, set, binding, RIDescriptorType_e, count}; mtl = {name, kind,
// index} read off the generated [[fragment]] signature in
// build-mtl/.../posteffect_bloom_{blur,add}.frag.metal.
//
// posteffect_bloom_blur.frag.metal:
//   sourceInput [[texture(0)]], pc [[buffer(0)]], inputSampler [[sampler(0)]]
constexpr RIProgram::RIProgramBinding kBlur[] = {
    {"inputSampler", RI_DESCRIPTOR_TYPE_SAMPLER, 1, RI_SHADER_STAGE_FRAGMENT, {0, 0}, {}, {0}},
    {"sourceInput", RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, RI_SHADER_STAGE_FRAGMENT, {0, 1}, {}, {0}},
};

// posteffect_bloom_add.frag.metal:
//   sourceInput [[texture(0)]], inputSampler [[sampler(0)]],
//   blurInput [[texture(1)]], pc [[buffer(0)]]
constexpr RIProgram::RIProgramBinding kAdd[] = {
    {"inputSampler", RI_DESCRIPTOR_TYPE_SAMPLER, 1, RI_SHADER_STAGE_FRAGMENT, {0, 0}, {}, {0}},
    {"sourceInput", RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, RI_SHADER_STAGE_FRAGMENT, {0, 1}, {}, {0}},
    {"blurInput", RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, RI_SHADER_STAGE_FRAGMENT, {0, 2}, {}, {1}},
};

void EmitImageBarrier(RICmd *cmd, RITexture *texture,
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

cPostEffectType_Bloom::cPostEffectType_Bloom(cGraphics *apGraphics,
                                             cResources *apResources)
    : iPostEffectType("Bloom", apGraphics, apResources) {
    LoadSlangGraphics(&RI.device, m_blurProgram, apResources,
                      "posteffect_fullscreen.vert.spv",
                      "posteffect_bloom_blur.frag.spv", "vsMain", "psMain", {},
                      kBlur, sizeof(BloomBlurPushConstants),
                      RI_SHADER_STAGE_FRAGMENT);
    LoadSlangGraphics(&RI.device, m_addProgram, apResources,
                      "posteffect_fullscreen.vert.spv",
                      "posteffect_bloom_add.frag.spv", "vsMain", "psMain", {},
                      kAdd, sizeof(BloomAddPushConstants),
                      RI_SHADER_STAGE_FRAGMENT);
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
    const uint32_t blurW = std::max(ctx.width / 4u, 1u);
    const uint32_t blurH = std::max(ctx.height / 4u, 1u);

    // (Re)allocate the quarter-res blur scratch when the viewport changes
    // or on first use.
    if (!m_blurInitialized || m_blur[0].width != blurW ||
        m_blur[0].height != blurH) {
        DestroyPostEffectColorTarget(m_blur[0]);
        DestroyPostEffectColorTarget(m_blur[1]);
        CreatePostEffectColorTarget(m_blur[0], blurW, blurH,
                                    RIBootstrap::PogoColorFormat, RI_USAGE_NONE,
                                    "PostEffect_Bloom.blur0");
        CreatePostEffectColorTarget(m_blur[1], blurW, blurH,
                                    RIBootstrap::PogoColorFormat, RI_USAGE_NONE,
                                    "PostEffect_Bloom.blur1");
        // First-time transitions out of UNDEFINED — set the "rest state":
        // blur[0] = SHADER_READ_ONLY (will flip to ATTACH on first H blur),
        // blur[1] = COLOR_ATTACHMENT  (will flip to READ on first H blur).
        EmitImageBarrier(ctx.cmd, &m_blur[0].texture,
                         RI_RESOURCE_STATE_UNDEFINED, RI_STAGE_NONE,
                         RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT);
        EmitImageBarrier(ctx.cmd, &m_blur[1].texture,
                         RI_RESOURCE_STATE_UNDEFINED, RI_STAGE_NONE,
                         RI_RESOURCE_STATE_RENDER_TARGET, RI_STAGE_NONE);
        m_blurInitialized = true;
    }

    auto samplerDesc = RI.resolve_filter_descriptor(
        eTextureWrap_ClampToEdge, eTextureWrap_ClampToEdge,
        eTextureWrap_ClampToEdge, eTextureFilter_Bilinear);

    RIGraphicsPipelineDesc blurPipe{};
    blurPipe.colorCount = 1;
    blurPipe.colors[0].format = RIBootstrap::PogoColorFormat;
    const hash_t kBlurHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);

    RIViewport blurViewport = {};
    blurViewport.width    = static_cast<float>(blurW);
    blurViewport.height   = static_cast<float>(blurH);
    blurViewport.depthMax = 1.0f;
    RIRect blurScissor = {};
    blurScissor.width  = (int16_t)blurW;
    blurScissor.height = (int16_t)blurH;

    auto bindBlurInput = [&](const RIDescriptor &inputDesc) {
        RIProgram::DescriptorBinding bindings[2] = {};
        bindings[0].descriptor = *samplerDesc;
        bindings[0].handle     = DescriptorBindingID::Create("inputSampler");
        bindings[1].descriptor = inputDesc;
        bindings[1].handle     = DescriptorBindingID::Create("sourceInput");
        mpBloomType->m_blurProgram.bindDescriptors(
            &RI.device, ctx.cmd, ctx.frameIndex, bindings, 2);
    };

    auto blurPass = [&](RITextureView *destView, RITexture *destTexture,
                        RITexture *prevDestTexture, const RIDescriptor &inputDesc,
                        float dirX, float dirY) {
        // Layout flip: dest SHADER_READ → COLOR_ATTACH ; prev dest
        // (which we'll sample next time) COLOR_ATTACH → SHADER_READ.
        EmitImageBarrier(ctx.cmd, destTexture,
                         RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT,
                         RI_RESOURCE_STATE_RENDER_TARGET, RI_STAGE_NONE);
        EmitImageBarrier(ctx.cmd, prevDestTexture,
                         RI_RESOURCE_STATE_RENDER_TARGET, RI_STAGE_NONE,
                         RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT);

        RIRenderingAttachment attach = {};
        attach.view    = destView;
        attach.loadOp  = RI_ATTACHMENT_LOAD_OP_DONT_CARE;
        attach.storeOp = RI_ATTACHMENT_STORE_OP_STORE;

        RIBeginRenderingDesc render = {};
        render.renderArea.width  = (int16_t)blurW;
        render.renderArea.height = (int16_t)blurH;
        render.colorCount        = 1;
        render.colors            = &attach;
        ctx.cmd->vk_d3d12_beginRendering(&RI.renderer, render);
        ctx.cmd->mtl_encoderDraw(render);

        ctx.cmd->setViewport(&RI.renderer, blurViewport);
        ctx.cmd->setScissor(&RI.renderer, blurScissor);

        mpBloomType->m_blurProgram.bindPipeline(&RI.device, ctx.cmd, kBlurHash,
                                                "PostEffect_Bloom.blur",
                                                blurPipe);
        bindBlurInput(inputDesc);

        BloomBlurPushConstants pc{};
        pc.blurDir[0] = dirX;
        pc.blurDir[1] = dirY;
        mpBloomType->m_blurProgram.pushConstants(ctx.cmd, &pc, sizeof(pc));

        ctx.cmd->draw(&RI.renderer, 3, 1, 0, 0);
        ctx.cmd->mtl_encoderEnd();
        ctx.cmd->vk_d3d12_endRendering(&RI.renderer);
    };

    // Iterative blur: first iter samples the pogo input, subsequent
    // iterations sample blur[1].
    for (int iter = 0; iter < std::max(mParams.mlBlurIterations, 1); ++iter) {
        const RIDescriptor &firstInput =
            (iter == 0) ? *ctx.inputSrv : m_blur[1].descriptor;

        // Pass H: dest = blur[0], read = firstInput, prevDest = blur[1].
        blurPass(m_blur[0].descriptor.view,
                 &m_blur[0].texture, &m_blur[1].texture,
                 firstInput, mParams.mfBlurSize, 0.0f);

        // Pass V: dest = blur[1], read = blur[0], prevDest = blur[0].
        blurPass(m_blur[1].descriptor.view,
                 &m_blur[1].texture, &m_blur[0].texture,
                 m_blur[0].descriptor, 0.0f, mParams.mfBlurSize);
    }

    // Flip blur[1] COLOR_ATTACH → SHADER_READ for the add pass to sample.
    EmitImageBarrier(ctx.cmd, &m_blur[1].texture,
                     RI_RESOURCE_STATE_RENDER_TARGET, RI_STAGE_NONE,
                     RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT);

    // ----- Add pass: sample source + blur, write pogo output -----
    RIRenderingAttachment outAttach = {};
    outAttach.view    = ctx.outputView;
    outAttach.loadOp  = RI_ATTACHMENT_LOAD_OP_DONT_CARE;
    outAttach.storeOp = RI_ATTACHMENT_STORE_OP_STORE;

    RIBeginRenderingDesc outRender = {};
    outRender.renderArea.width  = (int16_t)ctx.width;
    outRender.renderArea.height = (int16_t)ctx.height;
    outRender.colorCount        = 1;
    outRender.colors            = &outAttach;
    ctx.cmd->vk_d3d12_beginRendering(&RI.renderer, outRender);
    ctx.cmd->mtl_encoderDraw(outRender);

    RIViewport viewport = {};
    viewport.width    = static_cast<float>(ctx.width);
    viewport.height   = static_cast<float>(ctx.height);
    viewport.depthMax = 1.0f;
    ctx.cmd->setViewport(&RI.renderer, viewport);
    RIRect scissor = {};
    scissor.width  = (int16_t)ctx.width;
    scissor.height = (int16_t)ctx.height;
    ctx.cmd->setScissor(&RI.renderer, scissor);

    RIGraphicsPipelineDesc addPipe{};
    addPipe.colorCount = 1;
    addPipe.colors[0].format = RIBootstrap::PogoColorFormat;
    const hash_t kAddHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/1u);
    mpBloomType->m_addProgram.bindPipeline(&RI.device, ctx.cmd, kAddHash,
                                           "PostEffect_Bloom.add", addPipe);

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
    mpBloomType->m_addProgram.pushConstants(ctx.cmd, &apc, sizeof(apc));

    ctx.cmd->draw(&RI.renderer, 3, 1, 0, 0);
    ctx.cmd->mtl_encoderEnd();
    ctx.cmd->vk_d3d12_endRendering(&RI.renderer);

    // Restore the rest state: blur[1] back to COLOR_ATTACH for the next
    // call's swap-and-write pass. blur[0] is already in SHADER_READ_ONLY.
    EmitImageBarrier(ctx.cmd, &m_blur[1].texture,
                     RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT,
                     RI_RESOURCE_STATE_RENDER_TARGET, RI_STAGE_NONE);
}

} // namespace hpl
