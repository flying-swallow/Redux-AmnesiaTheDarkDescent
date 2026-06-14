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

// Explicit binding tables for posteffect_tonemap.frag.slang (set 0):
//   [vk::binding(0,0)] SamplerState inputSampler
//   [vk::binding(1,0)] Texture2D<float4> sourceInput
//   [vk::push_constant] ToneMapPC pc
// Generated MSL (posteffect_tonemap.frag.metal):
//   sourceInput [[texture(0)]], inputSampler [[sampler(0)]],
//   ToneMapPC constant* pc [[buffer(0)]]
// The fullscreen vertex stage has no descriptors / push constant (vs empty).
static const RIProgram::RIProgramBinding kToneMap[] = {
    {"inputSampler", RI_DESCRIPTOR_TYPE_SAMPLER, 1, RI_SHADER_STAGE_FRAGMENT, {0, 0}, {}, {0}},
    {"sourceInput", RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, RI_SHADER_STAGE_FRAGMENT, {0, 1}, {}, {0}},
};

cPostEffectType_ToneMap::cPostEffectType_ToneMap(cGraphics *apGraphics,
                                                 cResources *apResources)
    : iPostEffectType("ToneMap", apGraphics, apResources) {
    LoadSlangGraphics(&RI.device, m_program, apResources,
                      "posteffect_fullscreen.vert.spv",
                      "posteffect_tonemap.frag.spv", "vsMain", "psMain",
                      /*externalSets*/ {}, kToneMap,
                      sizeof(ToneMapPushConstants), RI_SHADER_STAGE_FRAGMENT);
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
    // Single fullscreen pass: sample the (bloom-composited) HDR pogo input, ACES
    // tonemap to linear [0,1], write the pogo output. The composite handles the
    // pogo toggle / barriers around this call.
    RIRenderingAttachment outAttach = {};
    outAttach.view    = ctx.outputView;
    outAttach.loadOp  = RI_ATTACHMENT_LOAD_OP_DONT_CARE;
    outAttach.storeOp = RI_ATTACHMENT_STORE_OP_STORE;

    RIBeginRenderingDesc render = {};
    render.renderArea.width  = (int16_t)ctx.width;
    render.renderArea.height = (int16_t)ctx.height;
    render.colorCount        = 1;
    render.colors            = &outAttach;
    ctx.cmd->vk_d3d12_beginRendering(&RI.renderer, render);
    ctx.cmd->mtl_encoderDraw(render);

    RIViewport viewport = {};
    viewport.width    = static_cast<float>(ctx.width);
    viewport.height   = static_cast<float>(ctx.height);
    viewport.depthMax = 1.0f;
    ctx.cmd->setViewport(&RI.renderer, viewport);
    RIRect scissor = {};
    scissor.width  = (int16_t)ctx.width;
    scissor.height = (int16_t)ctx.height;
    ctx.cmd->setScissor(&RI.renderer, scissor);

    // Fullscreen pass: single color attachment at the pogo HDR format, no
    // depth, no blend. Backend-neutral pipeline descriptor (defaults are
    // triangle-list / cull-none / blend-disabled, matching the old
    // InitPostEffectPipelineState(..., alphaBlend=false)).
    RIGraphicsPipelineDesc pipeDesc{};
    pipeDesc.colorCount = 1;
    pipeDesc.colors[0].format = RIBootstrap::PogoColorFormat;
    const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, /*variant=*/0u);
    mpToneMapType->m_program.bindPipeline(&RI.device, ctx.cmd, kHash,
                                          "PostEffect_ToneMap", pipeDesc);

    auto samplerDesc = RI.resolve_filter_descriptor(
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
    mpToneMapType->m_program.pushConstants(ctx.cmd, &pc, sizeof(pc));

    ctx.cmd->draw(&RI.renderer, 3, 1, 0, 0);
    ctx.cmd->mtl_encoderEnd();
    ctx.cmd->vk_d3d12_endRendering(&RI.renderer);
}

} // namespace hpl
