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

#include "LuxTypes.h"

#include "graphics/Image.h"
#include "graphics/Texture.h"
#include "graphics/RIBootstrap.h"
#include "graphics/RITypes.h"

#include <memory>

namespace hpl {
	class cGui;
	class cGuiGfxElement;
}

// Build a screen-sized color attachment that is also sampleable as a 2D texture
// (and a TRANSFER_DST for the capture copy), wrapped in a standalone Image and
// wired up for cGui::CreateGfxTexture / RIProgram::bindDescriptors. Shared by the
// capture (R16G16B16A16_SFLOAT) and the per-state effect (R8G8B8A8_UNORM).
hpl::SharedResourceHandle<hpl::Image> LuxCreateScreenRenderTarget(
		uint32_t width, uint32_t height, enum RI_Format_e format, const char *debugName);

//////////////////////////////////////////////////////////////////////////
// Shared "primary capture" of the gameplay scene, owned at game scope (as a
// global module) and reused by every menu backdrop — inventory, journal and
// the escape menu. There is ONE captured frame, and the per-state effect
// (cLuxScreenEffect) reads it.
//
// The capture is a straight vkCmdCopyImage of the gameplay viewport's RETAINED
// pogo "read" half — the clean, post-processed scene with no GUI (the same
// image the renderer's tail blit writes to the swapchain). The pogo is retained
// while the gameplay viewport is hidden behind a menu, so it still holds the
// last real gameplay frame, and the renderer leaves it SHADER_RESOURCE, so it
// is unambiguous to read.
//
// The copy is recorded on the frame's PRIMARY command buffer in OnPostRender.
// This is a global module, so its OnPostRender runs before the active menu
// state's cLuxScreenEffect::OnPostRender (see cUpdater::RunMessage) — the copy
// is recorded before the effect samples it, on the same command buffer, in the
// same frame. Both source and destination are R16G16B16A16_SFLOAT.
//
// GetScreenGfx() draws the sharp copy; cLuxScreenEffect reads PrimaryDescriptor()
// to build its desaturated/blurred backdrop.
//////////////////////////////////////////////////////////////////////////

class cLuxScreenCapture : public iLuxUpdateable
{
public:
	cLuxScreenCapture();
	~cLuxScreenCapture();

	cLuxScreenCapture(const cLuxScreenCapture&) = delete;
	cLuxScreenCapture& operator=(const cLuxScreenCapture&) = delete;

	// iLuxUpdateable: records the deferred copy on the primary command buffer.
	void OnPostRender(float afFrameTime) override;

	// Flag a capture to run on the next OnPostRender.
	void RequestCapture();

	// True once the requested capture has landed in m_primaryColor.
	bool IsCaptured() const { return mbCaptured; }

	// The sharp backdrop quad (an unmodified copy of the captured frame).
	hpl::cGuiGfxElement* GetScreenGfx() { return mpScreenGfx; }

	// The captured color, for the per-state effect passes (cLuxScreenEffect).
	// Produced on demand; an empty descriptor (.view == nullptr) means no
	// capture texture exists yet.
	RIDescriptor PrimaryDescriptor();
	cVector2l GetSize() const { return cVector2l(mlWidth, mlHeight); }

	// Release the gfx and texture.
	void Destroy();

private:
	void EnsureTextures();

	hpl::cGui            *mpGui       = nullptr;
	hpl::cGuiGfxElement  *mpScreenGfx = nullptr;

	hpl::SharedResourceHandle<hpl::Image> m_primaryImage;
	uint32_t mlWidth  = 0;
	uint32_t mlHeight = 0;

	bool mbPending            = false;
	bool mbCaptured           = false; // the REQUESTED capture has landed
	bool mbPrimaryEverWritten = false; // m_primaryColor has valid prior contents
};

#endif // LUX_SCREEN_CAPTURE_H
