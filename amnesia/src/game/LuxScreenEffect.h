/*
 * Copyright © 2009-2020 Frictional Games
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

#ifndef LUX_SCREEN_EFFECT_H
#define LUX_SCREEN_EFFECT_H

#include "graphics/Image.h"
#include "graphics/Texture.h"
#include "graphics/RIBootstrap.h"
#include "graphics/RIProgram.h"
#include "graphics/RITypes.h"

#include <memory>

namespace hpl {
	class cGui;
	class cGuiGfxElement;
}

//////////////////////////////////////////////////////////////////////////
// Per-state backdrop effect for the inventory / journal / escape menu. The
// shared cLuxScreenCapture (gpBase->GetScreenCapture()) holds ONE captured copy
// of the clean scene; this object reads that copy and produces the per-state
// "post" backdrop quad:
//   GetScreenGfx()   — forwards the shared sharp copy (drawn while fading in).
//   GetScreenBgGfx() — the desaturated/darkened (inventory/journal) or blurred
//                      (escape menu) version, drawn at full alpha once shown.
//
// The effect pass runs on the frame's primary command buffer in OnPostRender,
// one frame after the shared capture lands (capture happens in the shared
// module's OnPostBufferSwap, which precedes this OnPostRender on the next
// frame). IsCaptured() reflects the effect having been applied, so both quads
// are valid before either is drawn.
//////////////////////////////////////////////////////////////////////////

class cLuxScreenEffect
{
public:
	cLuxScreenEffect() = default;
	~cLuxScreenEffect() = default;

	cLuxScreenEffect(const cLuxScreenEffect&) = delete;
	cLuxScreenEffect& operator=(const cLuxScreenEffect&) = delete;

	enum class Effect { DesaturateDarken, Blur };

	// Load the (slang-compiled) post-effect program. Call once at construction.
	void Init(hpl::cGui *apGui, Effect effect = Effect::DesaturateDarken);

	// Allocate the per-state effect target(s) and the GUI gfx element, and
	// request the shared capture.
	void CreateTextures();

	// Trigger the shared capture and flag the effect to apply once it lands.
	void RequestCapture();

	// Apply the effect on the primary command buffer if the shared capture is
	// ready and the effect is pending.
	void OnPostRender();

	// Release the GUI gfx and target(s).
	void Destroy();

	hpl::cGuiGfxElement* GetScreenGfx();                 // shared sharp copy
	hpl::cGuiGfxElement* GetScreenBgGfx() { return mpScreenBgGfx; }
	bool IsCaptured() const { return mbApplied; }

private:
	hpl::cGui            *mpGui         = nullptr;
	hpl::cGuiGfxElement  *mpScreenBgGfx = nullptr;

	// Render targets, each a standalone Image owning its cTexture. Held as
	// reference-counted handles so per-frame keep-alive pins can defer the GPU
	// free. m_screenScratchImage is the blur ping-pong (Blur effect only).
	hpl::SharedResourceHandle<hpl::Image> m_screenBgImage;
	hpl::SharedResourceHandle<hpl::Image> m_screenScratchImage;

	hpl::RIProgram m_postProgram; // desaturate/darken or blur post-effect
	Effect mEffect = Effect::DesaturateDarken;

	bool mbApplyPending = false;
	bool mbApplied      = false;
};

#endif // LUX_SCREEN_EFFECT_H
