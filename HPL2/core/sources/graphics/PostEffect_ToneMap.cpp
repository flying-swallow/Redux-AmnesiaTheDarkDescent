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

#include "graphics/PostEffect_ToneMap.h"

#include "graphics/Graphics.h"
#include "graphics/PostEffectHelpers.h"
#include "graphics/RIBootstrap.h"
#include "graphics/RIProgramHelpers.h"
#include "system/Hasher.h"

namespace hpl {

namespace {
struct ToneMapPushConstants {
    float exposure;
    float shadowLift;
    float gamma;
    float _pad;
};
} // namespace

cPostEffectType_ToneMap::cPostEffectType_ToneMap(cGraphics *apGraphics,
                                                 cResources *apResources)
    : iPostEffectType("ToneMap", apGraphics, apResources) {
    LoadSlangGraphics(&RI.device, m_program, apResources,
                      "posteffect_fullscreen.vert.spv",
                      "posteffect_tonemap.frag.spv");
}

cPostEffectType_ToneMap::~cPostEffectType_ToneMap() {}

iPostEffect *
cPostEffectType_ToneMap::CreatePostEffect(iPostEffectParams *apParams) {
    // cGraphics::CreatePostEffect already calls SetParams on the returned effect.
    (void)apParams;
    return hplNew(cPostEffect_ToneMap, (mpGraphics, mpResources, this));
}

//-----------------------------------------------------------------------

cPostEffect_ToneMap::cPostEffect_ToneMap(cGraphics *apGraphics,
                                         cResources *apResources,
                                         iPostEffectType *apType)
    : iPostEffect(apGraphics, apResources, apType),
      mpToneMapType(static_cast<cPostEffectType_ToneMap *>(mpType)) {}

cPostEffect_ToneMap::~cPostEffect_ToneMap() {}

void cPostEffect_ToneMap::RenderEffect(const PostEffectRenderCtx &ctx) {
    VkCommandBuffer cmd = ctx.cmd->vk.cmd;

    // Single fullscreen pass: sample the (bloom-composited) HDR pogo input, ACES
    // tonemap to linear [0,1], write the pogo output. The composite handles the
    // pogo toggle / barriers around this call.
    VkRenderingAttachmentInfo outAttach = {
        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    outAttach.imageView   = ctx.outputView;
    outAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    outAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    outAttach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo render = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    render.renderArea           = {{0, 0}, {ctx.width, ctx.height}};
    render.layerCount           = 1;
    render.colorAttachmentCount = 1;
    render.pColorAttachments    = &outAttach;

    vkCmdBeginRendering(cmd, &render);

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
    InitPostEffectPipelineState(state, RIBootstrap::PogoColorFormatVk, false);
    const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    mpToneMapType->m_program.bindPipeline(&RI.device, ctx.cmd, kHash,
                                          "PostEffect_ToneMap",
                                          &state.createInfo);

    RIDescriptor_s *samplerDesc = RI.resolve_filter_descriptor(
        eTextureWrap_ClampToEdge, eTextureWrap_ClampToEdge,
        eTextureWrap_ClampToEdge, eTextureFilter_Bilinear);
    {
        RIProgram::DescriptorBinding bindings[2] = {};
        bindings[0].descriptor = *samplerDesc;
        bindings[0].handle     = DescriptorBindingID::Create("inputSampler");
        bindings[1].descriptor = *ctx.inputSrv;
        bindings[1].handle     = DescriptorBindingID::Create("sourceInput");
        mpToneMapType->m_program.bindDescriptors(&RI.device, ctx.cmd,
                                                 ctx.frameIndex, bindings, 2);
    }

    ToneMapPushConstants pc{};
    pc.exposure = mParams.mfExposure;
    pc.shadowLift = mParams.mfShadowLift;
    // User display-gamma setting, applied in-shader as the final encode step
    // (replaces the deprecated SDL window-brightness ramp). Authored by the
    // game's cLuxConfigHandler and pushed in via the tonemap params.
    pc.gamma = mParams.mfGamma;
    vkCmdPushConstants(cmd, mpToneMapType->m_program.getPipelineLayout(),
                       VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

} // namespace hpl
