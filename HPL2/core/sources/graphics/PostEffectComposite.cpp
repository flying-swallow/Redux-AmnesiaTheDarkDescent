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

#include "graphics/PostEffectComposite.h"

#include "graphics/PostEffect.h"
#include "graphics/RIBootstrap.h"
#include "graphics/RIPogoBuffer.h"
#include "system/LowLevelSystem.h"

namespace hpl {

cPostEffectComposite::cPostEffectComposite(cGraphics *apGraphics)
    : mpGraphics(apGraphics) {}

cPostEffectComposite::~cPostEffectComposite() {}

//-----------------------------------------------------------------------

void cPostEffectComposite::Render(float afFrameTime, struct RICmd_s *cmd,
                                  struct RI_PogoBuffer *pogo, uint32_t width,
                                  uint32_t height, uint32_t frameIndex) {
    mfCurrentFrameTime = afFrameTime;

    // Walk priority-ordered effects once to identify the last active one,
    // so RenderEffect can be told `isLastEffect` (used by effects that
    // would otherwise emit an extra barrier transition).
    iPostEffect *lastEffect = nullptr;
    for (auto it = m_mapPostEffects.begin(); it != m_mapPostEffects.end();
         ++it) {
        if (it->second->IsActive())
            lastEffect = it->second;
    }
    if (lastEffect == nullptr)
        return;

    // Each effect samples the pogo's "read" half and writes into the
    // pogo's "attach" half. RI_PogoBufferToggle flips the roles and
    // emits the COLOR_ATTACHMENT_OUTPUT ↔ FRAGMENT_SHADER barrier pair
    // before the next effect runs.
    for (auto it = m_mapPostEffects.begin(); it != m_mapPostEffects.end();
         ++it) {
        iPostEffect *effect = it->second;
        if (!effect->IsActive())
            continue;

        PostEffectRenderCtx ctx{};
        ctx.cmd          = cmd;
        ctx.inputSrv     = RI_PogoBufferShaderResource(pogo);
        ctx.outputImage  = pogo->textures[pogo->attachmentIndex].vk.image;
        ctx.outputView   = pogo->pogoAttachment[pogo->attachmentIndex]
                              .vk.image.imageView;
        ctx.width        = width;
        ctx.height       = height;
        ctx.frameIndex   = frameIndex;
        ctx.frameTime    = afFrameTime;
        ctx.isLastEffect = (effect == lastEffect);

        effect->RenderEffect(ctx);

        RI_PogoBufferToggle(&RI.device, pogo, cmd);
    }
}

//-----------------------------------------------------------------------

void cPostEffectComposite::AddPostEffect(iPostEffect *apPostEffect,
                                         int alPrio) {
    if (apPostEffect == NULL)
        return;

    m_mapPostEffects.insert(tPostEffectMap::value_type(alPrio, apPostEffect));
    mvPostEffects.push_back(apPostEffect);
}

bool cPostEffectComposite::HasActiveEffects() {
    for (auto *effect : mvPostEffects) {
        if (effect->IsActive())
            return true;
    }
    return false;
}

//-----------------------------------------------------------------------

} // namespace hpl
