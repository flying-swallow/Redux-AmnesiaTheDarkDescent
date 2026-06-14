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

#include "impl/GamepadSDL2.h"

#include "impl/LowLevelInputSDL.h"

#include "math/Math.h"
#include "system/LowLevelSystem.h"
#include "system/Platform.h"

#include <limits>
#include <float.h>

namespace hpl {

	//////////////////////////////////////////////////////////////////////////
	// CONSTRUCTORS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	float cGamepadSDL2::mfInvAxisMax = 1.0f/32767.5f;
	//float cGamepadSDL2::mfDeadZoneRadius = 6553.5f*cGamepadSDL2::mfInvAxisMax;
	float cGamepadSDL2::mfDeadZoneRadius = 0.333f;
	float cGamepadSDL2::mfDeadZoneRadiusSqr = cGamepadSDL2::mfDeadZoneRadius*cGamepadSDL2::mfDeadZoneRadius;

	//-----------------------------------------------------------------------

	cGamepadSDL2::cGamepadSDL2(cLowLevelInputSDL *apLowLevelInputSDL, int alIndex) : iGamepad("SDL Portable Gamepad", alIndex)
	{
		mpLowLevelInputSDL = apLowLevelInputSDL;

		// SDL3 opens gamepads by instance id, not by device index; map the
		// requested index through the current gamepad list.
		SDL_JoystickID gamepadId = 0;
		{
			int lNumGamepads = 0;
			SDL_JoystickID *pGamepads = SDL_GetGamepads(&lNumGamepads);
			if(pGamepads)
			{
				if(mlIndex >= 0 && mlIndex < lNumGamepads)
					gamepadId = pGamepads[mlIndex];
				SDL_free(pGamepads);
			}
		}

		mpHandle = gamepadId ? SDL_OpenGamepad(gamepadId) : NULL;
		mlLastTimeActive = -1;
		mpHaptic = NULL;

		if(mpHandle)
		{
			SDL_Joystick *joy = SDL_GetGamepadJoystick(mpHandle);

			mlInstance = SDL_GetJoystickID(joy);

			msGamepadName = tString(SDL_GetGamepadName(mpHandle));

			mvButtonArray.assign(SDL_GAMEPAD_BUTTON_COUNT, false);

			mvAxisArray.assign(SDL_GAMEPAD_AXIS_COUNT, 0.0f);

			mpHaptic = SDL_OpenHapticFromJoystick(joy);

			if(mpHaptic && !SDL_InitHapticRumble(mpHaptic))
			{
				SDL_CloseHaptic(mpHaptic);
				mpHaptic = NULL;
			}
		}
	}

	cGamepadSDL2::~cGamepadSDL2()
	{
		if(mpHaptic)
		{
			SDL_StopHapticRumble(mpHaptic);
			SDL_CloseHaptic(mpHaptic);
		}
		if(mpHandle) SDL_CloseGamepad(mpHandle);
	}


	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PUBLIC METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	int cGamepadSDL2::GetNumButtons()
	{
		return (int)mvButtonArray.size();
	}

	int cGamepadSDL2::GetNumAxes()
	{
		return (int)mvAxisArray.size();
	}

	int cGamepadSDL2::GetNumHats()
	{
		return 0;
	}

	int cGamepadSDL2::GetNumBalls()
	{
		return 0;
	}

	//-----------------------------------------------------------------------

	void cGamepadSDL2::ClearState()
	{
		mvButtonArray.assign(mvButtonArray.size(), false);
		mvAxisArray.assign(mvAxisArray.size(), 0.0f);
	}

	//-----------------------------------------------------------------------

	float cGamepadSDL2::GetTimeSinceLastActive()
	{
		if(mlLastTimeActive == -1) return FLT_MAX;
		return (float)(cPlatform::GetApplicationTime() - mlLastTimeActive) / 1000.0f;
	}

	//-----------------------------------------------------------------------

	void cGamepadSDL2::Update()
	{
		cGamepadInputData inputUpdate;

		mlstInputUpdates.clear();
		mlstButtonsPressed.clear();
		mlstButtonsReleased.clear();

		bool bDeviceRemoved = false;
		int lFlushed = 0;

		std::list<SDL_Event>::iterator it = mpLowLevelInputSDL->mlstEvents.begin();
		for(; it != mpLowLevelInputSDL->mlstEvents.end(); ++it)
		{
			SDL_Event *pEvent = &(*it);

			switch (pEvent->type) {
			case SDL_EVENT_GAMEPAD_REMOVED:
				if(mlInstance == pEvent->gdevice.which)
				{
					bDeviceRemoved = true;
				}
				break;
			case SDL_EVENT_GAMEPAD_AXIS_MOTION:
				if (mlInstance == pEvent->gaxis.which) {
					eGamepadAxis axis = SDLToAxis(pEvent->gaxis.axis);
					float fAxisValue = SDLToAxisValue(pEvent->gaxis.value);

					if(fAxisValue!=mvAxisArray[axis])
					{
						inputUpdate = cGamepadInputData(mlIndex, eGamepadInputType_Axis, axis, fAxisValue);

						mlstAxisChanges.push_back(inputUpdate);
						mlstInputUpdates.push_back(inputUpdate);
					}

					if(pEvent->gaxis.value == 0 || pEvent->gaxis.value == 16384) lFlushed++;

					mvAxisArray[axis] = fAxisValue;
				}
				break;
			case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
			case SDL_EVENT_GAMEPAD_BUTTON_UP:
				if (mlInstance == pEvent->gbutton.which) {
					eGamepadButton button = SDLToButton(pEvent->gbutton.button);
					inputUpdate = cGamepadInputData(mlIndex, eGamepadInputType_Button, button, 0.0f);

					bool bPressed = pEvent->gbutton.down;

					if(mvButtonArray[button] != bPressed) 
					{
						if(bPressed)
						{
							inputUpdate.mfInputValue = 1.0f;
							mlstButtonsPressed.push_back(inputUpdate);
						}
						else
						{
							inputUpdate.mfInputValue = 0.0f;
							mlstButtonsReleased.push_back(inputUpdate);
						}
						mlstInputUpdates.push_back(inputUpdate);
						mvButtonArray[button] = bPressed;         
					}

					if(bPressed == false) lFlushed++;
				}
				break;
			}
		}

		if(mvAxisArray.size() >= 6)
		{
			//////////////
			// Contrain dead zone
			UpdateAxis(0, mvAxisArray[0],
				1, mvAxisArray[1]);
			UpdateAxis(2, mvAxisArray[2],
				3, mvAxisArray[3]);
			UpdateAxis(4, mvAxisArray[4]);
			UpdateAxis(5, mvAxisArray[5]);
		}

		if(bDeviceRemoved || lFlushed >= (SDL_GAMEPAD_BUTTON_COUNT + SDL_GAMEPAD_AXIS_COUNT)) // if flushed is 21 then all buttons have been reset which means that the controller has been disconnected in some way
		{
			mlstInputUpdates.clear();
			mlstButtonsPressed.clear();
			mlstButtonsReleased.clear();

			mvButtonArray.assign(mvButtonArray.size(), false);
			mvAxisArray.assign(mvAxisArray.size(), 0.0f);
		}

		////////////////
		// Keep track of when the gamepad was last used
		if(mlstInputUpdates.empty() == false)
		{
			bool bValidInput = false;

			for(std::list<cGamepadInputData>::iterator begin = mlstInputUpdates.begin(), end = mlstInputUpdates.end();
				begin != end; ++begin)
			{
				cGamepadInputData input = *begin;

				if(input.mInputType != eGamepadInputType_Axis ||
					cMath::Abs(input.mfInputValue) > mfDeadZoneRadius * 0.5)
				{
					bValidInput = true;
					break;
				}
			}

			if(bValidInput) mlLastTimeActive = cPlatform::GetApplicationTime();
		}
	}

	//-----------------------------------------------------------------------

	void cGamepadSDL2::UpdateAxis(int alAxis, float afVal)
	{
		if(cMath::Abs(afVal) < mfDeadZoneRadius)
			afVal = 0.0f;

		mvAxisArray[alAxis] = afVal;
	}

	//-----------------------------------------------------------------------

	void cGamepadSDL2::UpdateAxis(int alAxis0, float afVal0, int alAxis1, float afVal1)
	{
		////////////
		// Update two axis at the same time, for thumb sticks
		float fLength = afVal0 * afVal0 + afVal1 * afVal1;

		if(fLength < mfDeadZoneRadiusSqr)
		{
			afVal0 = 0.0f;
			afVal1 = 0.0f;
		}

		mvAxisArray[alAxis0] = afVal0;
		mvAxisArray[alAxis1] = afVal1;
	}

	//-----------------------------------------------------------------------

	bool cGamepadSDL2::HasInputUpdates()
	{
		return mlstInputUpdates.empty()==false;
	}

	cGamepadInputData cGamepadSDL2::GetInputUpdate()
	{
		cGamepadInputData input = mlstInputUpdates.front();
		mlstInputUpdates.pop_front();

		switch(input.mInputType)
		{
		case eGamepadInputType_Button:
			{
				if(input.mfInputValue==0.0f)
					mlstButtonsReleased.remove(input);
				else
					mlstButtonsPressed.remove(input);
			}
			break;
		case eGamepadInputType_Axis:
			mlstAxisChanges.remove(input);
			break;
		}

		return input;
	}

	//-----------------------------------------------------------------------

	bool cGamepadSDL2::ButtonIsDown(eGamepadButton aButton)
	{
		if ((size_t)aButton >= mvButtonArray.size()) return false;

		return mvButtonArray[aButton];
	}

	//-----------------------------------------------------------------------


	cGamepadInputData cGamepadSDL2::GetButton()
	{
		cGamepadInputData button = mlstButtonsPressed.front();
		mlstButtonsPressed.pop_front();
		mlstInputUpdates.remove(button);

		return button;
	}

	//-----------------------------------------------------------------------

	bool cGamepadSDL2::ButtonIsPressed()
	{
		return mlstButtonsPressed.empty()==false;
	}

	//-----------------------------------------------------------------------

	cGamepadInputData cGamepadSDL2::GetReleasedButton()
	{
		cGamepadInputData button = mlstButtonsReleased.front();
		mlstButtonsReleased.pop_front();
		mlstInputUpdates.remove(button);

		return button;
	}

	//-----------------------------------------------------------------------

	bool cGamepadSDL2::ButtonIsReleased()
	{
		return mlstButtonsReleased.empty()==false;
	}

	//-----------------------------------------------------------------------

	float cGamepadSDL2::GetAxisValue(eGamepadAxis aAxis)
	{
		if ((size_t)aAxis >= mvAxisArray.size()) return 0;

		return mvAxisArray[aAxis];
	}

	float cGamepadSDL2::GetAxisDeadZoneRadiusValue()
	{
		return mfDeadZoneRadius;
	}

	void cGamepadSDL2::SetAxisDeadZoneRadiusValue(float afValue)
	{
		mfDeadZoneRadius = afValue;
	}

	cGamepadInputData cGamepadSDL2::GetUpdatedAxis()
	{
		cGamepadInputData axis = mlstAxisChanges.front();
		mlstAxisChanges.pop_front();
		mlstInputUpdates.remove(axis);

		return axis;
	}

	bool cGamepadSDL2::AxesUpdated()
	{
		return mlstAxisChanges.empty()==false;
	}

	//-----------------------------------------------------------------------

	eGamepadHatState cGamepadSDL2::GetHatCurrentState(eGamepadHat aHat)
	{
		return eGamepadHatState_Centered;
	}

	bool cGamepadSDL2::HatIsInState(eGamepadHat aHat, eGamepadHatState aState)
	{
		return (GetHatCurrentState(aHat)&aState)!=0;
	}

	bool cGamepadSDL2::HatsChanged()
	{
		return false;
	}

	cGamepadInputData cGamepadSDL2::GetHatState()
	{
		return cGamepadInputData();
	}

	//-----------------------------------------------------------------------

	cVector2l cGamepadSDL2::GetBallAbsPos(eGamepadBall aBall)
	{
		return cVector2l(0,0);
	}

	cVector2l cGamepadSDL2::GetBallRelPos(eGamepadBall aBall)
	{
		return cVector2l(0,0);
	}

	//-----------------------------------------------------------------------

	void cGamepadSDL2::SetRumble(float afValue, int alMillisec)
	{
		if(mpHaptic)
		{
			if(afValue > 0) SDL_PlayHapticRumble(mpHaptic, afValue, alMillisec);
			else			SDL_StopHapticRumble(mpHaptic);
		}
	}

	//-----------------------------------------------------------------------

	/////////////////////////////////////////////////////////////////////////
	// PRIVATE METHODS
	/////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	eGamepadButton cGamepadSDL2::SDLToButton(Uint8 alButton)
	{
		return static_cast<eGamepadButton>(alButton);
	}

	eGamepadAxis	cGamepadSDL2::SDLToAxis(Uint8 alAxis)
	{
		return static_cast<eGamepadAxis>(alAxis);
	}

	float cGamepadSDL2::SDLToAxisValue(Sint16 alAxisValue)
	{
		return cMath::Clamp((float)alAxisValue*mfInvAxisMax, -1.0f, 1.0f);
	}

	eGamepadHat cGamepadSDL2::SDLToHat(Uint8 alHat)
	{
		return static_cast<eGamepadHat>(alHat);
	}

	eGamepadHatState cGamepadSDL2::SDLToHatState(Uint8 alHatState)
	{
		return static_cast<eGamepadHatState>(alHatState);
	}

	eGamepadBall cGamepadSDL2::SDLToBall(Uint8 alBall)
	{
		return static_cast<eGamepadBall>(alBall);
	}

	//-----------------------------------------------------------------------

}