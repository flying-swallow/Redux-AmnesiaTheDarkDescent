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

#include "graphics/VertexBuffer.h"
#include "graphics/Bitmap.h"

// SDL3 removed SDL_syswm.h; native window handles come from the window's
// property set (see GetWindowHandle). SDL3 itself is included via the header.

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

#if DEVICE_IMPL_MTL
		// Per SDL docs, destroy the Metal view before the window.
		if (mpMetalView)
			SDL_Metal_DestroyView(mpMetalView);
#endif
		SDL_DestroyWindow(mpScreen);
	}

	struct RIWindowHandle cLowLevelGraphicsSDL::GetWindowHandle()
	{
		struct RIWindowHandle handle = {};
		handle.type = RI_WINDOW_UNKNOWN;

		// SDL3 removed SDL_syswm.h; native window handles now come from the
		// window's property set. The active video driver tells us which union
		// member is populated. SDL_VIDEO_DRIVER_* are SDL-internal build-config
		// macros not exposed by the public headers, so dispatch on the runtime
		// driver string. The SDL_PROP_WINDOW_* keys are always declared.
		SDL_PropertiesID props = SDL_GetWindowProperties(mpScreen);
		const char *driver = SDL_GetCurrentVideoDriver();
		if (driver == NULL) {
			assert(false);
			return handle;
		}

		if (SDL_strcmp(driver, "x11") == 0) {
			handle.type = RI_WINDOW_X11;
			handle.x11.dpy = SDL_GetPointerProperty(
				props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, NULL);
			handle.x11.window = (uint64_t)SDL_GetNumberProperty(
				props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
			return handle;
		}
		if (SDL_strcmp(driver, "wayland") == 0) {
			handle.type = RI_WINDOW_WAYLAND;
			handle.wayland.display = SDL_GetPointerProperty(
				props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, NULL);
			handle.wayland.surface = SDL_GetPointerProperty(
				props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, NULL);
			return handle;
		}
		if (SDL_strcmp(driver, "windows") == 0) {
			handle.type = RI_WINDOW_WIN32;
			handle.windows.hwnd = SDL_GetPointerProperty(
				props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
			return handle;
		}
#if DEVICE_IMPL_MTL
		if (SDL_strcmp(driver, "cocoa") == 0) {
			// The Metal view (created in Init) owns the CAMetalLayer; SDL3 hands
			// it back directly with no Objective-C. The swapchain casts it to
			// metal-cpp's CA::MetalLayer*.
			handle.type = RI_WINDOW_METAL;
			handle.metal.caMetalLayer = SDL_Metal_GetLayer(mpMetalView);
			return handle;
		}
#endif

		Error("Unsupported SDL video driver '%s' for native window handle\n",
			  driver);
		assert(false);
		return handle;
	}

	bool cLowLevelGraphicsSDL::Init(int alWidth, int alHeight, int alDisplay, int alBpp, int abFullscreen, const tString& asWindowCaption, const cVector2l& avWindowPos)
	{
		mvScreenSize.x = alWidth;
		mvScreenSize.y = alHeight;
		mlDisplay = alDisplay;
		mlBpp = alBpp;
		mbFullscreen = abFullscreen;

		// The window must be created for the active backend: a Metal-usable
		// view on the Metal backend, a Vulkan-surface-usable window otherwise.
#if DEVICE_IMPL_MTL
		SDL_WindowFlags mlFlags = SDL_WINDOW_METAL;
#else
		SDL_WindowFlags mlFlags = SDL_WINDOW_VULKAN;
#endif
		// SDL3 removed SDL_WINDOW_FULLSCREEN_DESKTOP; a fullscreen window with no
		// explicit display mode (the default) is borderless-desktop fullscreen.
		if (alWidth == 0 && alHeight == 0) {
			mvScreenSize = cVector2l(800, 600);
			mlFlags |= SDL_WINDOW_FULLSCREEN;
		}
		else if (abFullscreen)
			mlFlags |= SDL_WINDOW_FULLSCREEN;

		// SDL3 no longer implicitly activates a new window, so the engine's
		// "wait until the window has input focus" loop would otherwise spin
		// forever. Ask SDL to activate the window when shown/raised.
		SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "1");
		SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_RAISED, "1");

		Log(" Setting video mode: %d x %d - %d bpp\n", alWidth, alHeight, alBpp);
		// SDL3 SDL_CreateWindow no longer takes a position; set it afterwards.
		mpScreen = SDL_CreateWindow(asWindowCaption.c_str(), mvScreenSize.x, mvScreenSize.y, mlFlags);

		if (mpScreen == NULL)
		{
			// Try disabling FSAA
			Error("Could not set display mode setting a lower one! %s\n", SDL_GetError());
			mvScreenSize = cVector2l(640, 480);
			mpScreen = SDL_CreateWindow(asWindowCaption.c_str(), mvScreenSize.x, mvScreenSize.y, mlFlags);

			if (mpScreen == NULL)
			{
				FatalError("Unable to initialize display! %s\n", SDL_GetError());
				return false;
			}
			else
				cPlatform::CreateMessageBox(_W("Warning!"), _W("Could not set displaymode and 640x480 is used instead!\n"));
		}

		SDL_SetWindowPosition(mpScreen, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
		// Ensure the window is mapped and focused: Vulkan windows on some
		// compositors (notably Wayland) aren't mapped until explicitly shown,
		// and a new window won't gain input focus without an explicit raise.
		SDL_ShowWindow(mpScreen);
		SDL_RaiseWindow(mpScreen);

#if DEVICE_IMPL_MTL
		// Create the CAMetalLayer-backed view the Metal swapchain draws into.
		// SDL owns the view (destroyed in the dtor before SDL_DestroyWindow);
		// GetWindowHandle exposes its layer via SDL_Metal_GetLayer.
		mpMetalView = SDL_Metal_CreateView(mpScreen);
		if (mpMetalView == NULL)
			FatalError("Unable to create SDL Metal view! %s\n", SDL_GetError());
#endif

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
		// SDL3: SDL_ShowCursor()/SDL_HideCursor() take no argument.
		if (abX)
			SDL_ShowCursor();
		else
			SDL_HideCursor();
	}

	void cLowLevelGraphicsSDL::SetWindowGrab(bool abX)
	{
		mbGrab = abX;
		if (mpScreen) {
			// SDL3: per-window grab, plain bool (SDL_TRUE/SDL_FALSE removed).
			SDL_SetWindowMouseGrab(mpScreen, abX);
		}
	}

	void cLowLevelGraphicsSDL::SetRelativeMouse(bool abX)
	{
		// SDL3: relative mouse mode is per-window.
		if (mpScreen)
			SDL_SetWindowRelativeMouseMode(mpScreen, abX);
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
		// SDL3 removed SDL_WINDOW_SHOWN; a window is visible when not hidden.
		return (SDL_GetWindowFlags(mpScreen) & SDL_WINDOW_HIDDEN) == 0;
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

	cVertexBuffer* cLowLevelGraphicsSDL::CreateVertexBuffer(eVertexBufferType aType, eVertexBufferDrawType aDrawType, eVertexBufferUsageType aUsageType, int alReserveVtxSize, int alReserveIdxSize)
	{
		return new cVertexBuffer(this, aType, aDrawType, aUsageType, alReserveVtxSize, alReserveIdxSize);
	}
} // namespace hpl
