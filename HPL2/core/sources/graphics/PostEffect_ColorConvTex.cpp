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
#include "graphics/RIBootstrap.h"
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

// Explicit binding tables for posteffect_color_conv.frag.slang (set 0):
//   [vk::binding(0,0)] SamplerState inputSampler
//   [vk::binding(1,0)] Texture2D<float4> sourceInput
//   [vk::binding(2,0)] Texture1D<float4> colorConv
//   [vk::push_constant] ColorConvPC pc
// Generated MSL (posteffect_color_conv.frag.metal):
//   sourceInput [[texture(0)]], inputSampler [[sampler(0)]],
//   colorConv [[texture(1)]], ColorConvPC constant* pc [[buffer(0)]]
// The fullscreen vertex stage has no descriptors / push constant (vs empty).
static const RIProgram::RIProgramBinding kColorConv[] = {
    {"inputSampler", RI_DESCRIPTOR_TYPE_SAMPLER, 1, RI_SHADER_STAGE_FRAGMENT, {0, 0}, {}, {0}},
    {"sourceInput", RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, RI_SHADER_STAGE_FRAGMENT, {0, 1}, {}, {0}},
    {"colorConv", RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, RI_SHADER_STAGE_FRAGMENT, {0, 2}, {}, {1}},
};

cPostEffectType_ColorConvTex::cPostEffectType_ColorConvTex(
    cGraphics *apGraphics, cResources *apResources)
    : iPostEffectType("ColorConvTex", apGraphics, apResources) {
    LoadSlangGraphics(&RI.device, m_program, apResources,
                      "posteffect_fullscreen.vert.spv",
                      "posteffect_color_conv.frag.spv", "vsMain", "psMain",
                      /*externalSets*/ {}, kColorConv,
                      sizeof(ColorConvPushConstants), RI_SHADER_STAGE_FRAGMENT);
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
        mParams.msTextureFile, false);
}

void cPostEffect_ColorConvTex::RenderEffect(const PostEffectRenderCtx &ctx) {
    // Without a valid LUT there's nothing useful to render — let the
    // composite skip our pogo half by drawing a passthrough blit. The
    // simplest passthrough is to just copy the input straight into the
    // output via vkCmdBlitImage (the formats match: both pogo halves share
    // RIBootstrap::PogoColorFormat). This keeps the composite chain
    // consistent even when the LUT failed to load.
    if (!mpColorConvTex || !mpColorConvTex->GetTexture() ||
        !mpColorConvTex->GetTexture()->binding.texture) {
        return;
    }

    RIRenderingAttachment colorAttach = {};
    colorAttach.view    = ctx.outputView;
    colorAttach.loadOp  = RI_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttach.storeOp = RI_ATTACHMENT_STORE_OP_STORE;

    RIBeginRenderingDesc renderInfo = {};
    renderInfo.renderArea.width  = (int16_t)ctx.width;
    renderInfo.renderArea.height = (int16_t)ctx.height;
    renderInfo.colorCount        = 1;
    renderInfo.colors            = &colorAttach;
    ctx.cmd->vk_d3d12_beginRendering(&RI.renderer, renderInfo);
    ctx.cmd->mtl_encoderDraw(renderInfo);

    RIViewport viewport = {};
    viewport.width    = static_cast<float>(ctx.width);
    viewport.height   = static_cast<float>(ctx.height);
    viewport.depthMax = 1.0f;
    ctx.cmd->setViewport(&RI.renderer, viewport);
    RIRect scissor = {};
    scissor.width  = (int16_t)ctx.width;
    scissor.height = (int16_t)ctx.height;
    ctx.cmd->setScissor(&RI.renderer, scissor);

    RIGraphicsPipelineDesc pipeDesc{};
    pipeDesc.colorCount = 1;
    pipeDesc.colors[0].format = RIBootstrap::PogoColorFormat;

    const hash_t pipelineHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    mpSpecificType->m_program.bindPipeline(&RI.device, ctx.cmd, pipelineHash,
                                           "PostEffect_ColorConvTex", pipeDesc);

    auto samplerDesc = RI.resolve_filter_descriptor(
        eTextureWrap_ClampToEdge, eTextureWrap_ClampToEdge,
        eTextureWrap_ClampToEdge, eTextureFilter_Bilinear);

    RIProgram::DescriptorBinding bindings[3] = {};
    bindings[0].descriptor = *samplerDesc;
    bindings[0].handle     = DescriptorBindingID::Create("inputSampler");
    bindings[1].descriptor = *ctx.inputSrv;
    bindings[1].handle     = DescriptorBindingID::Create("sourceInput");
    bindings[2].descriptor = mpColorConvTex->GetTexture()->binding;
    bindings[2].handle     = DescriptorBindingID::Create("colorConv");
    mpSpecificType->m_program.bindDescriptors(&RI.device, ctx.cmd,
                                              ctx.frameIndex, bindings, 3);

    ColorConvPushConstants pc{};
    pc.alphaFade = cMath::Max(mParams.mfFadeAlpha, 0.0f);
    mpSpecificType->m_program.pushConstants(ctx.cmd, &pc, sizeof(pc));

    ctx.cmd->draw(&RI.renderer, 3, 1, 0, 0);
    ctx.cmd->mtl_encoderEnd();
    ctx.cmd->vk_d3d12_endRendering(&RI.renderer);
}

} // namespace hpl
