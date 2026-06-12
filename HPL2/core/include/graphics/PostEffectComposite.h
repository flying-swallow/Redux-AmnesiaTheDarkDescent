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

#ifndef HPL_POSTEFFECT_COMPOSITE_H
#define HPL_POSTEFFECT_COMPOSITE_H

#include "graphics/RIPogoBuffer.h"
#include "graphics/RITypes.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace hpl {

class cGraphics;
class iPostEffect;

class cPostEffectComposite {
public:
    cPostEffectComposite(cGraphics *apGraphics);
    ~cPostEffectComposite();

    // Run the active effects in priority order, ping-ponging through
    // the supplied pogo buffer. After Render returns, the most recently
    // written half is in SHADER_READ_ONLY_OPTIMAL and is reachable via
    // RI_PogoBufferShaderResource(pogo); the other half is in
    // COLOR_ATTACHMENT_OPTIMAL and reachable via RI_PogoBufferAttachment.
    //
    // When HasActiveEffects() is false this is a no-op — the caller can
    // skip the call entirely or invoke it and pay only the early return.
    void Render(float afFrameTime, struct RICmd *cmd,
                struct RI_PogoBuffer *pogo, uint32_t width, uint32_t height,
                uint32_t frameIndex);

    // Highest priority is rendered first.
    void AddPostEffect(iPostEffect *apPostEffect, int alPrio);
    inline int GetPostEffectNum() const {
        return static_cast<int>(m_postEffects.size());
    }
    // alIdx is the effect's insertion index, not its position in the
    // priority-sorted list.
    inline iPostEffect *GetPostEffect(int alIdx) const {
        for (const auto &entry : m_postEffects) {
            if (entry._id == static_cast<size_t>(alIdx))
                return entry._effect;
        }
        return nullptr;
    }

    bool HasActiveEffects();

    float GetCurrentFrameTime() { return mfCurrentFrameTime; }

private:
    struct PostEffectEntry {
        size_t       _id;     // insertion sequence (for GetPostEffect index)
        int          _prio;   // priority; highest rendered first
        iPostEffect *_effect;
    };

    cGraphics *mpGraphics;

    // Kept sorted by priority (highest first); _id preserves insertion order.
    std::vector<PostEffectEntry> m_postEffects;

    float mfCurrentFrameTime = 0.0f;
};

}; // namespace hpl
#endif // HPL_POSTEFFECT_COMPOSITE_H
