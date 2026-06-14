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

#ifndef HPL_LOWLEVELGRAPHICS_SDL_H
#define HPL_LOWLEVELGRAPHICS_SDL_H

#include <SDL3/SDL.h>

#include "graphics/LowLevelGraphics.h"
#include "math/MathTypes.h"


namespace hpl {

	class cLowLevelGraphicsSDL : public iLowLevelGraphics
	{
	public:
		cLowLevelGraphicsSDL();
		~cLowLevelGraphicsSDL();

		struct RIWindowHandle GetWindowHandle() override;

		bool Init(int alWidth, int alHeight, int alDisplay, int alBpp, int abFullscreen, const tString& asWindowCaption, const cVector2l& avWindowPos);

		void ShowCursor(bool abX);
		void SetWindowGrab(bool abX);
		void SetRelativeMouse(bool abX);
		void SetWindowCaption(const tString& asName);
		bool GetWindowMouseFocus();
		bool GetWindowInputFocus();
		bool GetWindowIsVisible();
		bool GetFullscreenModeActive() { return mbFullscreen; }
		cVector2f GetScreenSizeFloat();
		const cVector2l& GetScreenSizeInt();

		/////////////////////////////////////////////////////
		/////////////// DATA CREATION //////////////////////
		/////////////////////////////////////////////////////

		iFontData* CreateFontData(const tString& asName);

		cVertexBuffer* CreateVertexBuffer(eVertexBufferType aType, eVertexBufferDrawType aDrawType, eVertexBufferUsageType aUsageType, int alReserveVtxSize = 0, int alReserveIdxSize = 0);

		/////////////////////////////////////////////////////
		/////////// IMPLEMENTION SPECIFICS /////////////////
		/////////////////////////////////////////////////////

	private:
		cVector2l mvScreenSize;
		int mlDisplay;
		int mlBpp;
		bool mbFullscreen;
		bool mbInitHasBeenRun;

		//////////////////////////////////////
		//Gamma — original ramp saved at init, restored on shutdown
		//(SDL_SetGammaRamp). The user gamma setting moved to cLuxConfigHandler
		//(consumed by the tonemap post-effect).
		Uint16 mvStartGammaArray[3][256];

		//////////////////////////////////////
		//SDL Variables
		SDL_Window* mpScreen;
		// Metal builds: the CAMetalLayer-backed view created from the window;
		// GetWindowHandle hands its layer to the Metal swapchain. (void* on all
		// platforms — SDL_MetalView is a typedef for void*.)
		SDL_MetalView mpMetalView = nullptr;
		bool mbGrab;
	};
};
#endif // HPL_LOWLEVELGRAPHICS_SDL_H
