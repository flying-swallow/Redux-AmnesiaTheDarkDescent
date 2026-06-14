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

#include "graphics/PostEffect_RadialBlur.h"

#include "graphics/Graphics.h"
#include "graphics/PostEffectHelpers.h"
#include "graphics/RIBootstrap.h"
#include "graphics/RIProgramHelpers.h"
#include "graphics/RIRenderer.h"
#include "system/Hasher.h"
#include "system/LowLevelSystem.h"

namespace hpl {

namespace {
struct RadialBlurPushConstants {
    float size;
    float blurStartDist;
    float screenDim[2];
};
} // namespace

// Explicit binding tables for posteffect_radial_blur.frag.slang (set 0):
//   [vk::binding(0,0)] SamplerState inputSampler
//   [vk::binding(1,0)] Texture2D<float4> sourceInput
//   [vk::push_constant] RadialBlurPC pc
// Generated MSL (posteffect_radial_blur.frag.metal):
//   RadialBlurPC constant* pc [[buffer(0)]],
//   sourceInput [[texture(0)]], inputSampler [[sampler(0)]]
// The fullscreen vertex stage has no descriptors / push constant (vs empty).
static const RIProgram::RIProgramBinding kRadialBlur[] = {
    {"inputSampler", RI_DESCRIPTOR_TYPE_SAMPLER, 1, RI_SHADER_STAGE_FRAGMENT, {0, 0}, {}, {0}},
    {"sourceInput", RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, RI_SHADER_STAGE_FRAGMENT, {0, 1}, {}, {0}},
};

cPostEffectType_RadialBlur::cPostEffectType_RadialBlur(cGraphics *apGraphics,
                                                       cResources *apResources)
    : iPostEffectType("RadialBlur", apGraphics, apResources) {
    LoadSlangGraphics(&RI.device, m_program, apResources,
                      "posteffect_fullscreen.vert.spv",
                      "posteffect_radial_blur.frag.spv", "vsMain", "psMain",
                      /*externalSets*/ {}, kRadialBlur,
                      sizeof(RadialBlurPushConstants), RI_SHADER_STAGE_FRAGMENT);
}

cPostEffectType_RadialBlur::~cPostEffectType_RadialBlur() {}

iPostEffect *
cPostEffectType_RadialBlur::CreatePostEffect(iPostEffectParams *apParams) {
    // cGraphics::CreatePostEffect already calls SetParams on the returned
    // effect — leave initialisation to that single site.
    (void)apParams;
    return hplNew(cPostEffect_RadialBlur, (mpGraphics, mpResources, this));
}

//-----------------------------------------------------------------------

cPostEffect_RadialBlur::cPostEffect_RadialBlur(cGraphics *apGraphics,
                                               cResources *apResources,
                                               iPostEffectType *apType)
    : iPostEffect(apGraphics, apResources, apType),
      mpRadialBlurType(static_cast<cPostEffectType_RadialBlur *>(mpType)) {}

cPostEffect_RadialBlur::~cPostEffect_RadialBlur() {}

void cPostEffect_RadialBlur::RenderEffect(const PostEffectRenderCtx &ctx) {
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
    mpRadialBlurType->m_program.bindPipeline(&RI.device, ctx.cmd, pipelineHash,
                                             "PostEffect_RadialBlur", pipeDesc);

    auto samplerDesc = RI.resolve_filter_descriptor(
        eTextureWrap_ClampToEdge, eTextureWrap_ClampToEdge,
        eTextureWrap_ClampToEdge, eTextureFilter_Bilinear);

    RIProgram::DescriptorBinding bindings[2] = {};
    bindings[0].descriptor = *samplerDesc;
    bindings[0].handle     = DescriptorBindingID::Create("inputSampler");
    bindings[1].descriptor = *ctx.inputSrv;
    bindings[1].handle     = DescriptorBindingID::Create("sourceInput");
    mpRadialBlurType->m_program.bindDescriptors(&RI.device, ctx.cmd,
                                                ctx.frameIndex, bindings, 2);

    RadialBlurPushConstants pc{};
    pc.size          = mParams.mfSize;
    pc.blurStartDist = mParams.mfBlurStartDist;
    pc.screenDim[0]  = static_cast<float>(ctx.width);
    pc.screenDim[1]  = static_cast<float>(ctx.height);
    mpRadialBlurType->m_program.pushConstants(ctx.cmd, &pc, sizeof(pc));

    ctx.cmd->draw(&RI.renderer, 3, 1, 0, 0);
    ctx.cmd->mtl_encoderEnd();
    ctx.cmd->vk_d3d12_endRendering(&RI.renderer);
}

} // namespace hpl
