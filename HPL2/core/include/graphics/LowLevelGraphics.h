/*
 * Copyright © 2009-2020 Frictional Games
 *
 * This file is part of Amnesia: The Dark Descent.
 *
 * Amnesia: The Dark Descent is free software: you can redistribute it and/or
 modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * Amnesia: The Dark Descent is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Amnesia: The Dark Descent.  If not, see
 <https://www.gnu.org/licenses/>.
 */

#ifndef HPL_LOWLEVELGRAPHICS_H
#define HPL_LOWLEVELGRAPHICS_H

#include "graphics/RIBootstrap.h"
#include "graphics/GraphicsTypes.h"
#include "math/MathTypes.h"
#include "system/SystemTypes.h"

#include "graphics/RISwapchain.h"

namespace hpl {

	class iFontData;
	class cVertexBuffer;
	class cBitmap;
	class iMutex;

	//----------------------------------------

	class iLowLevelGraphics {
	public:
		virtual ~iLowLevelGraphics() {}

		virtual struct RIWindowHandle GetWindowHandle() = 0;

		virtual bool Init(int alWidth, int alHeight, int alDisplay, int alBpp, int abFullscreen, const tString& asWindowCaption, const cVector2l& avWindowPos) = 0;

		virtual void ShowCursor(bool abX) = 0; // Show the cursor or not. Default is false
		virtual void SetWindowGrab(bool abX) = 0;
		virtual void SetRelativeMouse(bool abX) = 0;
		virtual void SetWindowCaption(const tString& asName) = 0;
		virtual bool GetWindowMouseFocus() = 0;
		virtual bool GetWindowInputFocus() = 0;
		virtual bool GetWindowIsVisible() = 0;
		virtual bool GetFullscreenModeActive() = 0; // Get fullscreen mode

		// Get Size of screen
		virtual cVector2f GetScreenSizeFloat() = 0;
		virtual const cVector2l& GetScreenSizeInt() = 0;

		/////////////////////////////////////////////////////
		/////////////// DATA CREATION ///////////////////////
		/////////////////////////////////////////////////////

		virtual iFontData* CreateFontData(const tString& asName) = 0;
		virtual cVertexBuffer* CreateVertexBuffer(eVertexBufferType aType, eVertexBufferDrawType aDrawType, eVertexBufferUsageType aUsageType, int alReserveVtxSize = 0, int alReserveIdxSize = 0) = 0;

		static void SetForceShaderModel3And4Off(bool abX) { mbForceShaderModel3And4Off = abX; }
		static bool GetForceShaderModel3And4Off() { return mbForceShaderModel3And4Off; }
	protected:
		static bool mbForceShaderModel3And4Off;
	};
}; // namespace hpl
#endif // HPL_LOWLEVELGRAPHICS_H
