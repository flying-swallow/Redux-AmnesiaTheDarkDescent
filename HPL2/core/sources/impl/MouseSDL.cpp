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

#include "impl/MouseSDL.h"

#include <SDL3/SDL.h>

#include "graphics/LowLevelGraphics.h"
#include "impl/LowLevelInputSDL.h"
#include "math/Math.h"

#include "system/LowLevelSystem.h"
#include "system/Platform.h"

namespace hpl {

	//////////////////////////////////////////////////////////////////////////
	// CONSTRUCTORS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	cMouseSDL::cMouseSDL(cLowLevelInputSDL *apLowLevelInputSDL) : iMouse("SDL Portable Mouse")
	{
		mvMButtonArray.resize(eMouseButton_LastEnum);
		mvMButtonArray.assign(mvMButtonArray.size(),false);

		mpLowLevelInputSDL = apLowLevelInputSDL;

		mvMouseRelPos = cVector2l(0,0);
		mvMouseAbsPos = cVector2l(0,0);

		mbWheelUpMoved = false;
		mbWheelDownMoved = false;

		mbFirstTime = true;
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PUBLIC METHODS
	//////////////////////////////////////////////////////////////////////////
	
	//-----------------------------------------------------------------------

	void cMouseSDL::Update()
	{
		///////////////////////////////////////
		//On first update make sure to clear all previous events.
		if(mbFirstTime)
		{
			//Clear all relative movement
			for(int i=0; i<10; ++i)
			{
				SDL_PumpEvents();
				float lX,lY;
				SDL_GetRelativeMouseState(&lX, &lY);
			}
			mbFirstTime = false;
		}
		
		iLowLevelGraphics *pLowLevelGfx = mpLowLevelInputSDL->GetLowLevelGraphics();
		//mvMouseRelPos = cVector2f(0,0);
		mbWheelUpMoved = false;
		mbWheelDownMoved = false;

		std::list<SDL_Event>::iterator it = mpLowLevelInputSDL->mlstEvents.begin();
		for(; it != mpLowLevelInputSDL->mlstEvents.end(); ++it)
		{
			SDL_Event *pEvent = &(*it);

			if(	pEvent->type != SDL_EVENT_MOUSE_MOTION &&
				pEvent->type != SDL_EVENT_MOUSE_BUTTON_DOWN &&
                pEvent->type != SDL_EVENT_MOUSE_WHEEL &&
				pEvent->type != SDL_EVENT_MOUSE_BUTTON_UP)
			{
				continue;
			}

			if(pEvent->type == SDL_EVENT_MOUSE_MOTION)
			{
				// SDL3 reports mouse coordinates as floats; the engine works
				// in integer pixels.
				mvMouseAbsPos = cVector2l((int)pEvent->motion.x,(int)pEvent->motion.y);
			}
            else if(pEvent->type == SDL_EVENT_MOUSE_WHEEL)
            {
                if (pEvent->wheel.y > 0) {
                    mvMButtonArray[eMouseButton_WheelUp] = true;
                    mbWheelUpMoved = true;
                } else {
                    mvMButtonArray[eMouseButton_WheelDown] = true;
                    mbWheelDownMoved = true;
                }
                break;
            }
			else
			{
				bool bButtonIsDown = pEvent->type==SDL_EVENT_MOUSE_BUTTON_DOWN;

				switch(pEvent->button.button)
				{
					case SDL_BUTTON_LEFT: mvMButtonArray[eMouseButton_Left] = bButtonIsDown;break;
					case SDL_BUTTON_MIDDLE: mvMButtonArray[eMouseButton_Middle] = bButtonIsDown;break;
					case SDL_BUTTON_RIGHT: mvMButtonArray[eMouseButton_Right] = bButtonIsDown;break;
					case SDL_BUTTON_X1: mvMButtonArray[eMouseButton_Button6] = bButtonIsDown;break;
					case SDL_BUTTON_X2: mvMButtonArray[eMouseButton_Button7] = bButtonIsDown;break;
				}
			}
		}

		if(mbWheelDownMoved)	mvMButtonArray[eMouseButton_WheelDown] = true;
		else					mvMButtonArray[eMouseButton_WheelDown] = false;
		if(mbWheelUpMoved)		mvMButtonArray[eMouseButton_WheelUp] = true;
		else					mvMButtonArray[eMouseButton_WheelUp] = false;
		
		float lX,lY;
		SDL_GetRelativeMouseState(&lX, &lY);
		mvMouseRelPos = cVector2l((int)lX,(int)lY);

		
	}
	
	//-----------------------------------------------------------------------
	
	bool cMouseSDL::ButtonIsDown(eMouseButton mButton)
	{
		return mvMButtonArray[mButton];
	}

	//-----------------------------------------------------------------------

	cVector2l cMouseSDL::GetAbsPosition()
	{
		return mvMouseAbsPos;
	}
	
	//-----------------------------------------------------------------------

	cVector2l cMouseSDL::GetRelPosition()
	{
		cVector2l vPos = mvMouseRelPos;
		mvMouseRelPos = cVector2l(0,0);
		
		return vPos;
	}

	//-----------------------------------------------------------------------
	
	/////////////////////////////////////////////////////////////////////////
	// PRIVATE METHODS
	/////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------
	
	//-----------------------------------------------------------------------

}
