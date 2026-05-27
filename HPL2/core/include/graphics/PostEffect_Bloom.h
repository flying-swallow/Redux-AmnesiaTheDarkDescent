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

#ifndef HPL_POSTEFFECT_BLOOM_H
#define HPL_POSTEFFECT_BLOOM_H

#include "graphics/PostEffect.h"
#include "graphics/PostEffectHelpers.h"
#include "graphics/RIProgram.h"
#include "math/MathTypes.h"

namespace hpl {

class cPostEffectParams_Bloom : public iPostEffectParams {
public:
    cPostEffectParams_Bloom()
        : iPostEffectParams("Bloom"),
          mvRgbToIntensity(0.3f, 0.58f, 0.12f),
          mfBlurSize(1.0f),
          mlBlurIterations(2) {}

    kPostEffectParamsClassInit(cPostEffectParams_Bloom)

    cVector3f mvRgbToIntensity;
    float     mfBlurSize;
    int       mlBlurIterations;
};

class cPostEffectType_Bloom : public iPostEffectType {
    friend class cPostEffect_Bloom;

public:
    cPostEffectType_Bloom(cGraphics *apGraphics, cResources *apResources);
    virtual ~cPostEffectType_Bloom();

    iPostEffect *CreatePostEffect(iPostEffectParams *apParams) override;

private:
    // Two programs share the fullscreen vert. blur applies a 5-tap
    // separable kernel; add combines the source + blurred bright pass.
    RIProgram m_blurProgram;
    RIProgram m_addProgram;
};

class cPostEffect_Bloom : public iPostEffect {
public:
    cPostEffect_Bloom(cGraphics *apGraphics, cResources *apResources,
                      iPostEffectType *apType);
    ~cPostEffect_Bloom();

    void RenderEffect(const PostEffectRenderCtx &ctx) override;

private:
    void OnSetParams() override {}
    iPostEffectParams *GetTypeSpecificParams() override { return &mParams; }

    // Quarter-resolution ping-pong blur scratch targets owned by this
    // effect instance. Allocated lazily on first RenderEffect (and on
    // viewport resize).
    PostEffectColorTarget m_blur[2];
    bool m_blurInitialized = false;

    cPostEffectType_Bloom *mpBloomType;
    cPostEffectParams_Bloom mParams;
};

}; // namespace hpl
#endif
