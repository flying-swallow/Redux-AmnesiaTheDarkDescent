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

#include "graphics/PostEffect_ColorConvTex.h"

#include "graphics/Graphics.h"
#include "graphics/Texture.h"
#include "graphics/PostEffectHelpers.h"
#include "graphics/Graphics.h"
#include "graphics/RIProgramHelpers.h"
#include "math/Math.h"
#include "resources/Resources.h"
#include "resources/TextureManager.h"
#include "system/Hasher.h"
#include "system/LowLevelSystem.h"

namespace hpl {

namespace {
struct ColorConvPushConstants {
    float alphaFade;
};
} // namespace

cPostEffectType_ColorConvTex::cPostEffectType_ColorConvTex(
    cGraphics *apGraphics, cResources *apResources)
    : iPostEffectType("ColorConvTex", apGraphics, apResources) {
    LoadSlangGraphics(&mpGraphics->device, m_program, apResources,
                      "posteffect_fullscreen.vert.spv",
                      "posteffect_color_conv.frag.spv");
}

cPostEffectType_ColorConvTex::~cPostEffectType_ColorConvTex() {}

iPostEffect *
cPostEffectType_ColorConvTex::CreatePostEffect(iPostEffectParams *apParams) {
    // cGraphics::CreatePostEffect invokes SetParams on the returned effect
    // — don't do it here too. Calling SetParams twice during init would
    // destroy + reload the LUT before the resource uploader has flushed
    // the initial upload, tripping a validation layer assertion.
    (void)apParams;
    return hplNew(cPostEffect_ColorConvTex, (mpGraphics, mpResources, this));
}

//-----------------------------------------------------------------------

cPostEffect_ColorConvTex::cPostEffect_ColorConvTex(cGraphics *apGraphics,
                                                   cResources *apResources,
                                                   iPostEffectType *apType)
    : iPostEffect(apGraphics, apResources, apType),
      mpColorConvTex(nullptr),
      mpSpecificType(static_cast<cPostEffectType_ColorConvTex *>(mpType)) {}

cPostEffect_ColorConvTex::~cPostEffect_ColorConvTex() {
    if (mpColorConvTex) {
        mpResources->GetTextureManager()->Destroy(mpColorConvTex);
        mpColorConvTex = nullptr;
    }
}

void cPostEffect_ColorConvTex::OnSetParams() {
    if (mParams.msTextureFile.empty())
        return;

    if (mpColorConvTex) {
        mpResources->GetTextureManager()->Destroy(mpColorConvTex);
        mpColorConvTex = nullptr;
    }
    mpColorConvTex = mpResources->GetTextureManager()->Create1DImage(
        mParams.msTextureFile, false).Release();
}

void cPostEffect_ColorConvTex::RenderEffect(const PostEffectRenderCtx &ctx) {
    // Without a valid LUT there's nothing useful to render — let the
    // composite skip our pogo half by drawing a passthrough blit. The
    // simplest passthrough is to just copy the input straight into the
    // output via vkCmdBlitImage (the formats match: both pogo halves share
    // cGraphics::PogoColorFormat). This keeps the composite chain
    // consistent even when the LUT failed to load.
    if (!mpColorConvTex || !mpColorConvTex->GetTexture() ||
        mpColorConvTex->GetTexture()->view.isEmpty()) {
        return;
    }

    VkCommandBuffer cmd = ctx.cmd->vk.cmd;

    RITextureView outView = {};
    outView.vk.image = ctx.outputView;
    RIRenderingAttachment color = {};
    color.view    = outView;
    color.loadOp  = RI_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.storeOp = RI_ATTACHMENT_STORE_OP_STORE;

    RIBeginRenderingDesc beginDesc = {};
    beginDesc.renderArea.width  = (int16_t)ctx.width;
    beginDesc.renderArea.height = (int16_t)ctx.height;
    beginDesc.colorCount = 1;
    beginDesc.colors     = &color;
    ctx.cmd->vk_d3d12_beginRendering(&mpGraphics->device, beginDesc);

    VkViewport viewport = {0.0f,
                           0.0f,
                           static_cast<float>(ctx.width),
                           static_cast<float>(ctx.height),
                           0.0f,
                           1.0f};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor = {{0, 0}, {ctx.width, ctx.height}};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    PostEffectPipelineState state{};
    InitPostEffectPipelineState(state, cGraphics::PogoColorFormat, false);

    const hash_t pipelineHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    mpSpecificType->m_program.bindPipeline(&mpGraphics->device, ctx.cmd, pipelineHash,
                                           "PostEffect_ColorConvTex",
                                           &state.createInfo);

    auto samplerDesc = mpGraphics->resolve_filter_descriptor(
        eTextureWrap_ClampToEdge, eTextureWrap_ClampToEdge,
        eTextureWrap_ClampToEdge, eTextureFilter_Bilinear);

    RIProgram::DescriptorBinding bindings[3] = {};
    bindings[0].descriptor = *samplerDesc;
    bindings[0].handle     = DescriptorBindingID::Create("inputSampler");
    bindings[1].descriptor = ctx.inputSrv;
    bindings[1].handle     = DescriptorBindingID::Create("sourceInput");
    bindings[2].descriptor = mpColorConvTex->GetTexture()->descriptor();
    bindings[2].handle     = DescriptorBindingID::Create("colorConv");
    mpSpecificType->m_program.bindDescriptors(&mpGraphics->device, ctx.cmd,
                                              ctx.frameIndex, bindings, 3);

    ColorConvPushConstants pc{};
    pc.alphaFade = cMath::Max(mParams.mfFadeAlpha, 0.0f);
    vkCmdPushConstants(cmd, mpSpecificType->m_program.getPipelineLayout(),
                       VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

    vkCmdDraw(cmd, 3, 1, 0, 0);
    ctx.cmd->vk_d3d12_endRendering(&mpGraphics->device);
}

} // namespace hpl
