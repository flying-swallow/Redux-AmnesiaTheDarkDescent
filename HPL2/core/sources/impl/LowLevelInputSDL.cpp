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
#include "impl/GamepadSDL2.h"

#include "system/LowLevelSystem.h"
#include "graphics/LowLevelGraphics.h"

#include "engine/Engine.h"

#include <SDL3/SDL.h>

namespace hpl {

	//////////////////////////////////////////////////////////////////////////
	// CONSTRUCTORS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	cLowLevelInputSDL::cLowLevelInputSDL(iLowLevelGraphics *apLowLevelGraphics)
        : mpLowLevelGraphics(apLowLevelGraphics), mbQuitMessagePosted(false)
	{
		LockInput(true);
		RelativeMouse(false);
        SDL_InitSubSystem(SDL_INIT_GAMEPAD);
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
		mpLowLevelGraphics->SetWindowGrab(abX);
	}

	void cLowLevelInputSDL::RelativeMouse(bool abX)
	{
		mpLowLevelGraphics->SetRelativeMouse(abX);
	}
	
	//-----------------------------------------------------------------------

	void cLowLevelInputSDL::BeginInputUpdate()
	{
		SDL_Event sdlEvent;

		mlstEvents.clear();
		while(SDL_PollEvent(&sdlEvent))
		{
            // built-in SDL gamepad hotplug code
            // this whole contract should be rewritten to allow clean adding/removing
            // of controllers, instead of brute force rescanning
            if (sdlEvent.type==SDL_EVENT_GAMEPAD_ADDED)
            {
                // sdlEvent.gdevice.which is the device #
                cEngine::SetDeviceWasPlugged();
            } else if (sdlEvent.type==SDL_EVENT_GAMEPAD_REMOVED)
            {
                // sdlEvent.gdevice.which is the instance # (not device #).
                // instance # increases as devices are plugged and unplugged.
                cEngine::SetDeviceWasRemoved();
            } else
#if defined (__APPLE__)
            if (sdlEvent.type==SDL_EVENT_KEY_DOWN)
            {
                if (sdlEvent.key.key == SDLK_Q && sdlEvent.key.mod & SDL_KMOD_GUI) {
                    mbQuitMessagePosted = true;
                } else {
                    mlstEvents.push_back(sdlEvent);
                }
            } else
#endif
            if (sdlEvent.type==SDL_EVENT_QUIT)
            {
                mbQuitMessagePosted = true;
            } else
				mlstEvents.push_back(sdlEvent);
		}
	}
	
	//-----------------------------------------------------------------------

	void cLowLevelInputSDL::EndInputUpdate()
	{
		
	}

	//-----------------------------------------------------------------------

	void cLowLevelInputSDL::InitGamepadSupport()
	{
		// SDL3 initialises the gamepad subsystem in the constructor.
	}

	void cLowLevelInputSDL::DropGamepadSupport()
	{
	}

	int cLowLevelInputSDL::GetPluggedGamepadNum()
	{
#if USE_XINPUT
		return cGamepadXInput::GetNumConnected();
#else
		int lNumJoysticks = 0;
		SDL_JoystickID *pIds = SDL_GetJoysticks(&lNumJoysticks);
		SDL_free(pIds);
		return lNumJoysticks;
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
		return hplNew( cGamepadSDL2, (this, alIndex) );
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
