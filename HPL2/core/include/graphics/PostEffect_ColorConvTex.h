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

#ifndef HPL_POSTEFFECT_COLOR_CONV_TEX_H
#define HPL_POSTEFFECT_COLOR_CONV_TEX_H

#include "graphics/Image.h"
#include "graphics/PostEffect.h"
#include "graphics/RIProgram.h"
#include "system/SystemTypes.h"

namespace hpl {

class cPostEffectParams_ColorConvTex : public iPostEffectParams {
public:
    cPostEffectParams_ColorConvTex()
        : iPostEffectParams("ColorConvTex"), msTextureFile(""), mfFadeAlpha(1) {}

    kPostEffectParamsClassInit(cPostEffectParams_ColorConvTex)

    tString msTextureFile;
    float   mfFadeAlpha;
};

class cPostEffectType_ColorConvTex : public iPostEffectType {
    friend class cPostEffect_ColorConvTex;

public:
    cPostEffectType_ColorConvTex(cGraphics *apGraphics, cResources *apResources);
    virtual ~cPostEffectType_ColorConvTex();

    iPostEffect *CreatePostEffect(iPostEffectParams *apParams) override;

private:
    RIProgram m_program;
};

class cPostEffect_ColorConvTex : public iPostEffect {
public:
    cPostEffect_ColorConvTex(cGraphics *apGraphics, cResources *apResources,
                             iPostEffectType *apType);
    ~cPostEffect_ColorConvTex();

    void RenderEffect(const PostEffectRenderCtx &ctx) override;

private:
    void OnSetParams() override;
    iPostEffectParams *GetTypeSpecificParams() override { return &mParams; }

    // 1D LUT loaded through cTextureManager::Create1DImage in OnSetParams.
    Image *mpColorConvTex;

    cPostEffectType_ColorConvTex *mpSpecificType;
    cPostEffectParams_ColorConvTex mParams;
};

}; // namespace hpl
#endif
