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

#ifndef HPL_WINDOW_H
#define HPL_WINDOW_H

#include "graphics/RISwapchain.h"
#include "math/MathTypes.h"
#include "system/SystemTypes.h"
#include "system/Event.h"

struct SDL_Window;
// Forward-declared so the window can hold an SDL-event-bus handler without
// pulling SDL into this widely-included header (Window.cpp includes the full
// SDL). SDL_Event lives at global scope.
union SDL_Event;

namespace hpl {

	//------------------------------------------
	// The OS window (SDL-backed). Owns the SDL_Window and everything that is
	// a property of it: lifecycle, size/fullscreen, caption, focus/visibility,
	// cursor grab, and the native handle the Vulkan surface is created from.
	// It is the single source of truth for the window/screen size and is
	// reachable everywhere via Interface<cWindow>::Get(); the swapchain simply
	// follows it. The window keeps no cached size — GetSize() queries the OS
	// live each call.
	//------------------------------------------

	class cWindow
	{
	public:
		cWindow();
		~cWindow();

		// Creates the OS window. Not called in headless setups — every method
		// null-guards so an un-Init'd cWindow is inert.
		bool Init(const cVector2l& avSize, int alDisplay, bool abFullscreen, const tString& asCaption);

		// Native handle for Vulkan surface creation (RISwapchainDesc.source).
		struct RIWindowHandle GetHandle();

		void SetCaption(const tString& asName);

		// Resize / re-mode the window (settings path): applies the size +
		// fullscreen to the OS window. The OS reflects the new size immediately;
		// cGraphics::BeginActiveSet reconciles the swapchain by polling GetSize().
		void SetSize(const cVector2l& avSize, bool abFullscreen);
		// The window's actual current size, queried live from the OS.
		cVector2l GetSize();
		// Float form of GetSize(), for aspect / GUI math.
		cVector2f GetSizeF();
		// True when the window is minimized/iconified. A minimized window
		// cannot present, so the frame loop skips swapchain recreate + acquire
		// while it is (SDL may still report a nonzero window size, so a size
		// check alone is not enough).
		bool IsMinimized();

		bool HasInputFocus();
		bool HasMouseFocus();
		bool IsVisible();

		void SetGrab(bool abX);
		void SetRelativeMouse(bool abX);

		// Subscribe to cEngine's OS-event bus. From then on the window filters
		// events for its own SDL_Window and re-broadcasts a typed resize on
		// OnScreenSizeChanged(). Called once, after cEngine is Interface-
		// registered (see Engine.cpp). Per-window, so this extends to multiple
		// windows: each cWindow only reacts to its own window's events.
		void ConnectToEventBus();

		// Fired when this window's size actually changes (deduped against SDL's
		// duplicate resize events). Payload is the new window size in pixels.
		// Consumers that snapshot the size subscribe here to re-sync layout.
		Event<const cVector2l&>& OnScreenSizeChanged() { return m_onScreenSizeChanged; }

	private:
		// Bus handler: filters for mpWindow and re-broadcasts resizes.
		void OnSDLEvent(const SDL_Event& aEvent);

		SDL_Window* mpWindow;
		int mlDisplay;
		bool mbGrab;

		Event<const cVector2l&> m_onScreenSizeChanged;
		Event<const SDL_Event&>::Handler m_sdlEventHandler;
		// Last size we broadcast, to coalesce SDL's duplicate resize events.
		cVector2l m_lastNotifiedSize = cVector2l(0, 0);
	};
};
#endif // HPL_WINDOW_H
