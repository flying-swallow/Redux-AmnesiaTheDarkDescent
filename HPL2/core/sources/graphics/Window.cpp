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

#include "graphics/Window.h"

#include "system/LowLevelSystem.h"
#include "system/Platform.h"
#include "engine/Engine.h"
#include "engine/Interface.h"

#include <assert.h>

#include "SDL2/SDL.h"
#include "SDL2/SDL_syswm.h"

namespace hpl {

	//-----------------------------------------------------------------------

	cWindow::cWindow()
	{
		mpWindow = NULL;
		mlDisplay = 0;
		mbGrab = false;
	}

	cWindow::~cWindow()
	{
		if (mpWindow)
			SDL_DestroyWindow(mpWindow);
	}

	//-----------------------------------------------------------------------

	bool cWindow::Init(const cVector2l& avSize, int alDisplay, bool abFullscreen, const tString& asCaption)
	{
		mlDisplay = alDisplay;

		// Local scratch for the requested / fallback window size only — the
		// window does not cache its size (query it live via GetSize).
		cVector2l vRequestedSize = avSize;

		unsigned int mlFlags = SDL_WINDOW_VULKAN;
		if (avSize.x == 0 && avSize.y == 0) {
			vRequestedSize = cVector2l(800, 600);
			mlFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
		}
		else if (abFullscreen)
			mlFlags |= SDL_WINDOW_FULLSCREEN;

		Log(" Setting video mode: %d x %d\n", avSize.x, avSize.y);
		mpWindow = SDL_CreateWindow(asCaption.c_str(), SDL_WINDOWPOS_CENTERED_DISPLAY(mlDisplay), SDL_WINDOWPOS_CENTERED_DISPLAY(mlDisplay), vRequestedSize.x, vRequestedSize.y, mlFlags);

		if (mpWindow == NULL)
		{
			Error("Could not set display mode setting a lower one! %s\n", SDL_GetError());
			vRequestedSize = cVector2l(640, 480);
			mpWindow = SDL_CreateWindow(asCaption.c_str(), SDL_WINDOWPOS_CENTERED_DISPLAY(mlDisplay), SDL_WINDOWPOS_CENTERED_DISPLAY(mlDisplay), vRequestedSize.x, vRequestedSize.y, mlFlags);

			if (mpWindow == NULL)
			{
				FatalError("Unable to initialize display! %s\n", SDL_GetError());
				return false;
			}
			else
				cPlatform::CreateMessageBox(_W("Warning!"), _W("Could not set displaymode and 640x480 is used instead!\n"));
		}

		if (mbGrab)
			SetGrab(true);

		// Turn off cursor as default
		SDL_ShowCursor(SDL_DISABLE);

		return true;
	}

	//-----------------------------------------------------------------------

	struct RIWindowHandle cWindow::GetHandle()
	{
		struct RIWindowHandle handle = {};
		SDL_SysWMinfo wmi = {};
		SDL_VERSION(&wmi.version);
		handle.type = RI_WINDOW_UNKNOWN;
		if (SDL_GetWindowWMInfo(mpWindow, &wmi)) {
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

	//-----------------------------------------------------------------------

	void cWindow::SetCaption(const tString& asName)
	{
		if (mpWindow)
			SDL_SetWindowTitle(mpWindow, asName.c_str());
	}

	//-----------------------------------------------------------------------

	void cWindow::SetSize(const cVector2l& avSize, bool abFullscreen)
	{
		if (mpWindow == NULL)
			return;

		if (abFullscreen)
		{
			// Point exclusive fullscreen at the requested resolution, then engage it.
			SDL_DisplayMode mode;
			if (SDL_GetWindowDisplayMode(mpWindow, &mode) == 0)
			{
				mode.w = avSize.x;
				mode.h = avSize.y;
				SDL_SetWindowDisplayMode(mpWindow, &mode);
			}
			SDL_SetWindowFullscreen(mpWindow, SDL_WINDOW_FULLSCREEN);
		}
		else
		{
			SDL_SetWindowFullscreen(mpWindow, 0);
			SDL_SetWindowSize(mpWindow, avSize.x, avSize.y);
			SDL_SetWindowPosition(mpWindow, SDL_WINDOWPOS_CENTERED_DISPLAY(mlDisplay),
								   SDL_WINDOWPOS_CENTERED_DISPLAY(mlDisplay));
		}
		// No cached size — the OS window is the single source of truth for the
		// window/screen size. BeginActiveSet reconciles the swapchain to it.
	}

	cVector2l cWindow::GetSize()
	{
		if (mpWindow == NULL)
			return cVector2l(0, 0);
		int w = 0, h = 0;
		SDL_GetWindowSize(mpWindow, &w, &h);
		return cVector2l(w, h);
	}

	cVector2f cWindow::GetSizeF()
	{
		const cVector2l vSize = GetSize();
		return cVector2f((float)vSize.x, (float)vSize.y);
	}

	bool cWindow::IsMinimized()
	{
		if (mpWindow == NULL)
			return false;
		return (SDL_GetWindowFlags(mpWindow) & SDL_WINDOW_MINIMIZED) != 0;
	}

	//-----------------------------------------------------------------------

	void cWindow::ConnectToEventBus()
	{
		m_sdlEventHandler = Event<const SDL_Event&>::Handler(
			[this](const SDL_Event& aEvent) { OnSDLEvent(aEvent); });
		if (cEngine* pEngine = Interface<cEngine>::Get())
			m_sdlEventHandler.Connect(pEngine->OnSDLEvent());
	}

	//-----------------------------------------------------------------------

	void cWindow::OnSDLEvent(const SDL_Event& aEvent)
	{
		if (mpWindow == NULL || aEvent.type != SDL_WINDOWEVENT)
			return;
		// Only react to this window's own events (multi-window ready).
		if (aEvent.window.windowID != SDL_GetWindowID(mpWindow))
			return;
		if (aEvent.window.event != SDL_WINDOWEVENT_SIZE_CHANGED &&
			aEvent.window.event != SDL_WINDOWEVENT_RESIZED)
			return;

		// Read the size live (the OS is the source of truth) and coalesce SDL's
		// duplicate SIZE_CHANGED/RESIZED pairs so consumers relayout once.
		const cVector2l vSize = GetSize();
		if (vSize.x <= 0 || vSize.y <= 0 || vSize == m_lastNotifiedSize)
			return;
		m_lastNotifiedSize = vSize;
		m_onScreenSizeChanged.Signal(vSize);
	}

	//-----------------------------------------------------------------------

	bool cWindow::HasInputFocus()
	{
		return (SDL_GetWindowFlags(mpWindow) & SDL_WINDOW_INPUT_FOCUS) != 0;
	}

	bool cWindow::HasMouseFocus()
	{
		return (SDL_GetWindowFlags(mpWindow) & SDL_WINDOW_MOUSE_FOCUS) != 0;
	}

	bool cWindow::IsVisible()
	{
		return (SDL_GetWindowFlags(mpWindow) & SDL_WINDOW_SHOWN) != 0;
	}

	//-----------------------------------------------------------------------

	void cWindow::SetGrab(bool abX)
	{
		mbGrab = abX;
		if (mpWindow) {
			SDL_SetWindowGrab(mpWindow, abX ? SDL_TRUE : SDL_FALSE);
		}
	}

	void cWindow::SetRelativeMouse(bool abX)
	{
		SDL_SetRelativeMouseMode(abX ? SDL_TRUE : SDL_FALSE);
	}

	//-----------------------------------------------------------------------

} // namespace hpl
