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

#ifndef LUX_SCREEN_CAPTURE_H
#define LUX_SCREEN_CAPTURE_H

#include "graphics/Image.h"
#include "graphics/HPLTexture.h"
#include "graphics/RIBootstrap.h"
#include "graphics/RIProgram.h"
#include "graphics/RITypes.h"

#include <memory>

namespace hpl {
	class cGui;
	class cGuiGfxElement;
}

//////////////////////////////////////////////////////////////////////////
// Screen-snapshot pipeline shared by the inventory and journal screens.
//
// Both states draw a frozen copy of the rendered scene behind their GUI:
//   GetScreenGfx()   — an unmodified copy of the captured frame, drawn while
//                      the state fades in.
//   GetScreenBgGfx() — a post-effect (desaturated/darkened) version of that
//                      copy, drawn at full alpha once the fade completes.
//
// The GPU work (blit swapchain -> color texture, then the fullscreen
// post-effect) must run while a command buffer is recording and after the
// scene has rendered, so RequestCapture() only flags the work and OnPostRender()
// performs it on the next post-render hook. See LuxScreenCapture.cpp.
//////////////////////////////////////////////////////////////////////////

class cLuxScreenCapture
{
public:
	cLuxScreenCapture() = default;
	~cLuxScreenCapture() = default;

	cLuxScreenCapture(const cLuxScreenCapture&) = delete;
	cLuxScreenCapture& operator=(const cLuxScreenCapture&) = delete;

	// Post-effect applied to the captured frame for the "blurred backdrop"
	// quad (GetScreenBgGfx). DesaturateDarken is a single fullscreen pass
	// (inventory/journal); Blur is a separable gaussian ping-pong (escape menu).
	enum class Effect { DesaturateDarken, Blur };

	// Load the (slang-compiled) post-effect program. Call once at construction.
	void Init(hpl::cGui *apGui, Effect effect = Effect::DesaturateDarken);

	// Allocate the screen-sized textures and their GUI gfx elements.
	void CreateTextures();

	// Flag a capture to run on the next OnPostRender().
	void RequestCapture();

	// Perform the deferred capture if one is pending and a frame is recording.
	void OnPostRender();

	// Release the GUI gfx and textures.
	void Destroy();

	hpl::cGuiGfxElement* GetScreenGfx()   { return mpScreenGfx; }
	hpl::cGuiGfxElement* GetScreenBgGfx() { return mpScreenBgGfx; }
	bool IsCaptured() const               { return mbCaptured; }

private:
	hpl::cGui            *mpGui        = nullptr;
	hpl::cGuiGfxElement  *mpScreenGfx   = nullptr;
	hpl::cGuiGfxElement  *mpScreenBgGfx = nullptr;

	std::shared_ptr<hpl::HPLTexture> m_screenColor;
	std::shared_ptr<hpl::HPLTexture> m_screenBgColor;
	std::shared_ptr<hpl::HPLTexture> m_screenScratch; // Blur ping-pong only.
	std::shared_ptr<hpl::Image>      m_screenImage;
	std::shared_ptr<hpl::Image>      m_screenBgImage;

	hpl::RIProgram m_copyProgram; // pogo scene-color -> m_screenColor passthrough
	hpl::RIProgram m_postProgram; // desaturate/darken or blur post-effect
	Effect mEffect = Effect::DesaturateDarken;

	bool mbPending  = false;
	bool mbCaptured = false;
};

#endif // LUX_SCREEN_CAPTURE_H
