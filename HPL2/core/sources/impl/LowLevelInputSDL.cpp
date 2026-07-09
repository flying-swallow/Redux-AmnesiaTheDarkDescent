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

#include "impl/LowLevelInputSDL.h"

#include "impl/MouseSDL.h"
#include "impl/KeyboardSDL.h"
#include "impl/GamepadSDL.h"
#include "impl/GamepadSDL2.h"

#include "system/LowLevelSystem.h"
#include "graphics/Window.h"

#include "engine/Engine.h"
#include "engine/Interface.h"

#include "SDL2/SDL.h"

#if defined _WIN32 && !SDL_VERSION_ATLEAST(2,0,0)
#include <Windows.h>
#include <Dbt.h>
#endif

namespace hpl {

	//////////////////////////////////////////////////////////////////////////
	// CONSTRUCTORS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	cLowLevelInputSDL::cLowLevelInputSDL(cWindow *apWindow)
        : mpWindow(apWindow), mbQuitMessagePosted(false)
	{
		LockInput(true);
		RelativeMouse(false);
#if SDL_VERSION_ATLEAST(2, 0, 0)
        SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
#else
//		mlConnectedDevices = 0;
//		mlCheckDeviceChange = 0;
//		mbDirtyGamepads = true;
//
		SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE);
#endif
	}

	//-----------------------------------------------------------------------

	cLowLevelInputSDL::~cLowLevelInputSDL()
	{
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PUBLIC METHOD
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------
	
	void cLowLevelInputSDL::LockInput(bool abX)
	{
		mpWindow->SetGrab(abX);
	}

	void cLowLevelInputSDL::RelativeMouse(bool abX)
	{
		mpWindow->SetRelativeMouse(abX);
	}
	
	//-----------------------------------------------------------------------

	void cLowLevelInputSDL::ConnectToEventBus()
	{
		// Source device events from cEngine's OS-event bus instead of polling
		// SDL ourselves (cEngine now owns the single per-frame pump).
		mSdlEventHandler = Event<const SDL_Event&>::Handler(
			[this](const SDL_Event& aEvent) { OnSdlEvent(aEvent); });
		if (cEngine* pEngine = Interface<cEngine>::Get())
			mSdlEventHandler.Connect(pEngine->OnSDLEvent());
	}

	//-----------------------------------------------------------------------

	void cLowLevelInputSDL::OnSdlEvent(const SDL_Event& aEvent)
	{
		// Gamepad hotplug: flag a rescan. The event still falls through to
		// mlstEvents below so the gamepad device processes it too.
		if (aEvent.type == SDL_CONTROLLERDEVICEADDED)
			cEngine::SetDeviceWasPlugged();
		else if (aEvent.type == SDL_CONTROLLERDEVICEREMOVED)
			cEngine::SetDeviceWasRemoved();

#if defined (__APPLE__)
		// Cmd+Q quits; swallow it rather than forwarding as a keypress.
		if (aEvent.type == SDL_KEYDOWN &&
			aEvent.key.keysym.sym == SDLK_q && (aEvent.key.keysym.mod & KMOD_GUI))
		{
			mbQuitMessagePosted = true;
			return;
		}
#endif

		if (aEvent.type == SDL_QUIT)
			mbQuitMessagePosted = true;
		else
			mlstEvents.push_back(aEvent);
	}

	//-----------------------------------------------------------------------

	void cLowLevelInputSDL::BeginInputUpdate()
	{
		// The pump now lives in cEngine; the bus handler (OnSdlEvent) has
		// already filled mlstEvents for this frame. Nothing to do here.
	}

	//-----------------------------------------------------------------------

	void cLowLevelInputSDL::EndInputUpdate()
	{
		// Consume-once: the device loop in cInput::Update has read this frame's
		// events, so clear them. cEngine pumps once per rendered frame, so no
		// events arrive between logic ticks; a 0-tick frame keeps them until the
		// next tick consumes them.
		mlstEvents.clear();
	}

	//-----------------------------------------------------------------------

	void cLowLevelInputSDL::InitGamepadSupport()
	{
#if !SDL_VERSION_ATLEAST(2, 0, 0)
		SDL_InitSubSystem(SDL_INIT_JOYSTICK);
#endif
	}

	void cLowLevelInputSDL::DropGamepadSupport()
	{
#if !SDL_VERSION_ATLEAST(2, 0, 0)
		SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
#endif
	}

	int cLowLevelInputSDL::GetPluggedGamepadNum()
	{
#if USE_XINPUT
		return cGamepadXInput::GetNumConnected();
#else
		return SDL_NumJoysticks();
#endif
	}

	//-----------------------------------------------------------------------
	
	iMouse* cLowLevelInputSDL::CreateMouse()
	{
		return hplNew( cMouseSDL,(this));
	}
	
	//-----------------------------------------------------------------------
	
	iKeyboard* cLowLevelInputSDL::CreateKeyboard()
	{
		return hplNew( cKeyboardSDL,(this) );
	}

	//-----------------------------------------------------------------------

	iGamepad* cLowLevelInputSDL::CreateGamepad(int alIndex)
	{
#if USE_SDL2
		return hplNew( cGamepadSDL2, (this, alIndex) );
#else
		return hplNew( cGamepadSDL, (this, alIndex) );
#endif
	}
	
	//-----------------------------------------------------------------------

    bool cLowLevelInputSDL::isQuitMessagePosted()
    {
        return mbQuitMessagePosted;
    }
    
    void cLowLevelInputSDL::resetQuitMessagePosted()
    {
        mbQuitMessagePosted = false;
    }

	//-----------------------------------------------------------------------
    
}
