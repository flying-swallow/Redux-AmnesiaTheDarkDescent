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

#include "graphics/RIRenderer.h"
#include "graphics/RISwapchain.h"
#include "graphics/RITypes.h"
#include <assert.h>
#include <stdlib.h>

#include "system/LowLevelSystem.h"
#include "system/Mutex.h"
#include "system/Platform.h"

#include "impl/LowLevelGraphicsSDL.h"
#include "impl/SDLFontData.h"
#include "impl/SDLTexture.h"

#include "graphics/VertexBuffer_RI.h"
#include "graphics/Bitmap.h"

#if USE_SDL2
#include "SDL2/SDL_syswm.h"
#else
#include "SDL/SDL_syswm.h"
#endif

#ifdef _WIN32
#include "impl/TaskKeyHook.h"
#endif

#ifndef _WIN32
#if defined __ppc__ || defined(__LP64__)
#define CALLBACK
#else
#define CALLBACK __attribute__((__stdcall__))
#endif
#endif

namespace hpl {

#ifdef HPL_OGL_THREADSAFE
	iMutex* gpLowlevelGfxMutex = NULL;

	cLowlevelGfxMutex::cLowlevelGfxMutex()
	{
		if (gpLowlevelGfxMutex)
			gpLowlevelGfxMutex->Lock();
	}

	cLowlevelGfxMutex::~cLowlevelGfxMutex()
	{
		if (gpLowlevelGfxMutex)
			gpLowlevelGfxMutex->Unlock();
	}
#endif

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// CONSTRUCTORS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	cLowLevelGraphicsSDL::cLowLevelGraphicsSDL()
	{
		mpScreen = 0;
		mbGrab = false;

#if defined(_WIN32) && !SDL_VERSION_ATLEAST(2, 0, 0)
		mhKeyTrapper = NULL;
#endif

		mbInitHasBeenRun = false;
	}

	cLowLevelGraphicsSDL::~cLowLevelGraphicsSDL()
	{
		if (mbInitHasBeenRun)
		{
#if !SDL_VERSION_ATLEAST(2, 0, 0)
			SDL_SetGammaRamp(mvStartGammaArray[0], mvStartGammaArray[1], mvStartGammaArray[2]);
#endif
		}

#if SDL_VERSION_ATLEAST(2, 0, 0)
		SDL_DestroyWindow(mpScreen);
#endif
	}

	struct RIWindowHandle_s cLowLevelGraphicsSDL::GetWindowHandle()
	{
		struct RIWindowHandle_s handle = {};
		SDL_SysWMinfo wmi = {};
		SDL_VERSION(&wmi.version);
		handle.type = RI_WINDOW_UNKNOWN;
		if (SDL_GetWindowWMInfo(mpScreen, &wmi)) {
			switch (wmi.subsystem) {
#if defined( SDL_VIDEO_DRIVER_X11 )
			case SDL_SYSWM_X11:
				handle.type = RI_WINDOW_X11;
				handle.x11.window = wmi.info.x11.window;
				handle.x11.dpy = wmi.info.x11.display;
				break;
#endif
#if defined( SDL_VIDEO_DRIVER_WAYLAND )
			case SDL_SYSWM_WAYLAND:
				handle.type = RI_WINDOW_WAYLAND;
				handle.wayland.display = wmi.info.wl.display;
				handle.wayland.surface = wmi.info.wl.surface;
				break;
#endif
#if defined( SDL_VIDEO_DRIVER_WINDOWS )
			case SDL_SYSWM_WINDOWS:
				handle.type = RI_WINDOW_WIN32;
				handle.windows.hwnd = wmi.info.win.window;
				break;
#endif
			default:
				assert(false);
				break;
			}
		}
		return handle;
	}

	bool cLowLevelGraphicsSDL::Init(int alWidth, int alHeight, int alDisplay, int alBpp, int abFullscreen, const tString& asWindowCaption, const cVector2l& avWindowPos)
	{
		mvScreenSize.x = alWidth;
		mvScreenSize.y = alHeight;
		mlDisplay = alDisplay;
		mlBpp = alBpp;
		mbFullscreen = abFullscreen;

		unsigned int mlFlags = SDL_WINDOW_VULKAN;
		if (alWidth == 0 && alHeight == 0) {
			mvScreenSize = cVector2l(800, 600);
			mlFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
		}
		else if (abFullscreen)
			mlFlags |= SDL_WINDOW_FULLSCREEN;

		Log(" Setting video mode: %d x %d - %d bpp\n", alWidth, alHeight, alBpp);
		mpScreen = SDL_CreateWindow(asWindowCaption.c_str(), SDL_WINDOWPOS_CENTERED_DISPLAY(mlDisplay), SDL_WINDOWPOS_CENTERED_DISPLAY(mlDisplay), mvScreenSize.x, mvScreenSize.y, mlFlags);

		if (mpScreen == NULL)
		{
			// Try disabling FSAA
			Error("Could not set display mode setting a lower one! %s\n", SDL_GetError());
			mvScreenSize = cVector2l(640, 480);
			mpScreen = SDL_CreateWindow(asWindowCaption.c_str(), SDL_WINDOWPOS_CENTERED_DISPLAY(mlDisplay), SDL_WINDOWPOS_CENTERED_DISPLAY(mlDisplay), mvScreenSize.x, mvScreenSize.y, mlFlags);

			if (mpScreen == NULL)
			{
				FatalError("Unable to initialize display! %s\n", SDL_GetError());
				return false;
			}
			else
				cPlatform::CreateMessageBox(_W("Warning!"), _W("Could not set displaymode and 640x480 is used instead!\n"));
		}

		// Update with the screen size ACTUALLY obtained
		int w, h;
		SDL_GetWindowSize(mpScreen, &w, &h);
		mvScreenSize = cVector2l(w, h);

		if (mbGrab)
			SetWindowGrab(true);

		// Turn off cursor as default
		ShowCursor(false);

		mbInitHasBeenRun = true;
		return true;
	}

	void cLowLevelGraphicsSDL::ShowCursor(bool abX)
	{
		if (abX)
			SDL_ShowCursor(SDL_ENABLE);
		else
			SDL_ShowCursor(SDL_DISABLE);
	}

	void cLowLevelGraphicsSDL::SetWindowGrab(bool abX)
	{
		mbGrab = abX;
#if SDL_VERSION_ATLEAST(2, 0, 0)
		if (mpScreen) {
			SDL_SetWindowGrab(mpScreen, abX ? SDL_TRUE : SDL_FALSE);
		}
#else
		SDL_WM_GrabInput(abX ? SDL_GRAB_ON : SDL_GRAB_OFF);
#endif
	}

	void cLowLevelGraphicsSDL::SetRelativeMouse(bool abX)
	{
#if SDL_VERSION_ATLEAST(2, 0, 0)
		SDL_SetRelativeMouseMode(abX ? SDL_TRUE : SDL_FALSE);
#endif
	}

	void cLowLevelGraphicsSDL::SetWindowCaption(const tString& asName)
	{
#if SDL_VERSION_ATLEAST(2, 0, 0)
		SDL_SetWindowTitle(mpScreen, asName.c_str());
#else
		SDL_WM_SetCaption(asName.c_str(), "");
#endif
	}

	bool cLowLevelGraphicsSDL::GetWindowMouseFocus()
	{
#if SDL_VERSION_ATLEAST(2, 0, 0)
		return (SDL_GetWindowFlags(mpScreen) & SDL_WINDOW_MOUSE_FOCUS) != 0;
#else
		return (SDL_GetAppState() & SDL_APPMOUSEFOCUS) != 0;
#endif
	}

	bool cLowLevelGraphicsSDL::GetWindowInputFocus()
	{
#if SDL_VERSION_ATLEAST(2, 0, 0)
		return (SDL_GetWindowFlags(mpScreen) & SDL_WINDOW_INPUT_FOCUS) != 0;
#else
		return (SDL_GetAppState() & SDL_APPINPUTFOCUS) != 0;
#endif
	}

	bool cLowLevelGraphicsSDL::GetWindowIsVisible()
	{
#if SDL_VERSION_ATLEAST(2, 0, 0)
		return (SDL_GetWindowFlags(mpScreen) & SDL_WINDOW_SHOWN) != 0;
#else
		return (SDL_GetAppState() & SDL_APPACTIVE) != 0;
#endif
	}

	cVector2f cLowLevelGraphicsSDL::GetScreenSizeFloat()
	{
		return cVector2f((float)mvScreenSize.x, (float)mvScreenSize.y);
	}

	const cVector2l& cLowLevelGraphicsSDL::GetScreenSizeInt()
	{
		return mvScreenSize;
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// DATA CREATION
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	iFontData* cLowLevelGraphicsSDL::CreateFontData(const tString& asName)
	{
		return hplNew(cSDLFontData, (asName, this));
	}

	iTexture* cLowLevelGraphicsSDL::CreateTexture(const tString& asName, eTextureType aType, eTextureUsage aUsage)
	{
		cSDLTexture* pTexture = hplNew(cSDLTexture, (asName, aType, aUsage, this));
		return pTexture;
	}

	iVertexBuffer* cLowLevelGraphicsSDL::CreateVertexBuffer(eVertexBufferType aType, eVertexBufferDrawType aDrawType, eVertexBufferUsageType aUsageType, int alReserveVtxSize, int alReserveIdxSize)
	{
		return new VertexBuffer_RI(this, aType, aDrawType, aUsageType, alReserveVtxSize, alReserveIdxSize);
	}
} // namespace hpl
