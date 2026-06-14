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

#include "input/Gamepad.h"
#include "input/InputTypes.h"

namespace hpl
{
	
	//////////////////////////////////////////////////////////////////////////
	// CONSTRUCTORS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	iGamepad::iGamepad(tString asName, int alIndex) : iInputDevice(asName, eInputDeviceType_Gamepad)
	{
		mlIndex = alIndex;	
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PUBLIC METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	eGamepadAxisRange iGamepad::GetAxisRangeFromValue(float afX)
	{
		eGamepadAxisRange range = eGamepadAxisRange_Dead;
		if(afX>0.0f)
			range = eGamepadAxisRange_Positive;
		else if(afX<0.0f)
			range = eGamepadAxisRange_Negative;

		return range;
	}

	//-----------------------------------------------------------------------

	tString iGamepad::ButtonToString(eGamepadButton aButton)
	{
		switch(aButton)
		{
        case eGamepadButton_A: return "Button A";
        case eGamepadButton_B: return "Button B";
        case eGamepadButton_X: return "Button X";
        case eGamepadButton_Y: return "Button Y";
        case eGamepadButton_Back: return "Button Back";
        case eGamepadButton_Guide: return "Button Guide";
        case eGamepadButton_Start: return "Button Start";
        case eGamepadButton_LeftStick: return "Button LeftStick";
        case eGamepadButton_RightStick: return "Button RightStick";
        case eGamepadButton_LeftShoulder: return "Button LeftShoulder";
        case eGamepadButton_RightShoulder: return "Button RightShoulder";
        case eGamepadButton_DpadUp: return "Button DpadUp";
        case eGamepadButton_DpadDown: return "Button DpadDown";
        case eGamepadButton_DpadLeft: return "Button DpadLeft";
        case eGamepadButton_DpadRight: return "Button DpadRight";
		}
		
		return "Unknown";
	}

	//-----------------------------------------------------------------------

	eGamepadButton iGamepad::StringToButton(const tString& asButton)
	{
        if(asButton=="Button A") return eGamepadButton_A;
        if(asButton=="Button B") return eGamepadButton_B;
        if(asButton=="Button X") return eGamepadButton_X;
        if(asButton=="Button Y") return eGamepadButton_Y;
        if(asButton=="Button Back") return eGamepadButton_Back;
        if(asButton=="Button Guide") return eGamepadButton_Guide;
        if(asButton=="Button Start") return eGamepadButton_Start;
        if(asButton=="Button LeftStick") return eGamepadButton_LeftStick;
        if(asButton=="Button RightStick") return eGamepadButton_RightStick;
        if(asButton=="Button LeftShoulder") return eGamepadButton_LeftShoulder;
        if(asButton=="Button RightShoulder") return eGamepadButton_RightShoulder;
        if(asButton=="Button DpadUp") return eGamepadButton_DpadUp;
        if(asButton=="Button DpadDown") return eGamepadButton_DpadDown;
        if(asButton=="Button DpadLeft") return eGamepadButton_DpadLeft;
        if(asButton=="Button DpadRight") return eGamepadButton_DpadRight;
		
		return eGamepadButton_LastEnum;
	}

	//-----------------------------------------------------------------------

	tString iGamepad::AxisToString(eGamepadAxis aAxis)
	{
		switch(aAxis)
		{
        case eGamepadAxis_LeftX: return "Axis LeftX";
        case eGamepadAxis_LeftY: return "Axis LeftY";
        case eGamepadAxis_RightX: return "Axis RightX";
        case eGamepadAxis_RightY: return "Axis RightY";
        case eGamepadAxis_LeftTrigger: return "Axis LeftTrigger";
        case eGamepadAxis_RightTrigger: return "Axis RightTrigger";
		}
		return "None";
	}

	//-----------------------------------------------------------------------

	eGamepadAxis iGamepad::StringToAxis(const tString& asAxis)
	{
        if(asAxis=="Axis LeftX") return eGamepadAxis_LeftX;
        if(asAxis=="Axis LeftY") return eGamepadAxis_LeftY;
        if(asAxis=="Axis RightX") return eGamepadAxis_RightX;
        if(asAxis=="Axis RightY") return eGamepadAxis_RightY;
        if(asAxis=="Axis LeftTrigger") return eGamepadAxis_LeftTrigger;
        if(asAxis=="Axis RightTrigger") return eGamepadAxis_RightTrigger;
		return eGamepadAxis_LastEnum;
	}

	//-----------------------------------------------------------------------

	tString iGamepad::AxisRangeToString(eGamepadAxisRange aRange)
	{
		switch(aRange)
		{
		case eGamepadAxisRange_Positive: return "Positive";
		case eGamepadAxisRange_Negative: return "Negative";
		}

		return "None";
	}

	//-----------------------------------------------------------------------

	eGamepadAxisRange iGamepad::StringToAxisRange(const tString& asRange)
	{
		if(asRange=="Positive") return eGamepadAxisRange_Positive;
		if(asRange=="Negative") return eGamepadAxisRange_Negative;

		return eGamepadAxisRange_LastEnum;
	}

	//-----------------------------------------------------------------------

	tString iGamepad::HatToString(eGamepadHat aHat)
	{
		switch(aHat)
		{
		case eGamepadHat_0: return "Hat 0";
		case eGamepadHat_1: return "Hat 1";
		case eGamepadHat_2: return "Hat 2";
		case eGamepadHat_3: return "Hat 3";
		case eGamepadHat_4: return "Hat 4";
		case eGamepadHat_5: return "Hat 5";
		case eGamepadHat_6: return "Hat 6";
		case eGamepadHat_7: return "Hat 7";
		case eGamepadHat_8: return "Hat 8";
		case eGamepadHat_9: return "Hat 9";
		}

		return "None";
	}

	//-----------------------------------------------------------------------

	eGamepadHat iGamepad::StringToHat(const tString& asHat)
	{
		if(asHat=="Hat 0") return eGamepadHat_0;
		if(asHat=="Hat 1") return eGamepadHat_1;
		if(asHat=="Hat 2") return eGamepadHat_2;
		if(asHat=="Hat 3") return eGamepadHat_3;
		if(asHat=="Hat 4") return eGamepadHat_4;
		if(asHat=="Hat 5") return eGamepadHat_5;
		if(asHat=="Hat 6") return eGamepadHat_6;
		if(asHat=="Hat 7") return eGamepadHat_7;
		if(asHat=="Hat 8") return eGamepadHat_8;
		if(asHat=="Hat 9") return eGamepadHat_9;

		return eGamepadHat_LastEnum;
	}

	//-----------------------------------------------------------------------

	tString iGamepad::HatStateToString(eGamepadHatState aHatState)
	{
		switch(aHatState)
		{
		case eGamepadHatState_Centered: return "Centered";
		case eGamepadHatState_Up: return "Up";
		case eGamepadHatState_Right: return "Right";
		case eGamepadHatState_Down: return "Down";
		case eGamepadHatState_Left: return "Left";
		case eGamepadHatState_RightUp: return "RightUp";
		case eGamepadHatState_RightDown: return "RightDown";
		case eGamepadHatState_LeftUp: return "LeftUp";
		case eGamepadHatState_LeftDown: return "LeftDown";
		}

		return "None";
	}

	//-----------------------------------------------------------------------

	eGamepadHatState iGamepad::StringToHatState(const tString& asHatState)
	{
		if(asHatState=="Centered") return eGamepadHatState_Centered;
		if(asHatState=="Up") return eGamepadHatState_Up;
		if(asHatState=="Right") return eGamepadHatState_Right;
		if(asHatState=="Down") return eGamepadHatState_Down;
		if(asHatState=="Left") return eGamepadHatState_Left;
		if(asHatState=="RightUp") return eGamepadHatState_RightUp;
		if(asHatState=="RightDown") return eGamepadHatState_RightDown;
		if(asHatState=="LeftUp") return eGamepadHatState_LeftUp;
		if(asHatState=="LeftDown") return eGamepadHatState_LeftDown;

		return eGamepadHatState_LastEnum;
	}

	//-----------------------------------------------------------------------

	tString iGamepad::BallToString(eGamepadBall aBall)
	{
		switch(aBall)
		{
		case eGamepadBall_0: return "Trackball 0";
		}

		return "None";
	}

	//-----------------------------------------------------------------------

	eGamepadBall iGamepad::StringToBall(const tString& asBall)
	{
		if(asBall=="Trackball 0") return eGamepadBall_0;

		return eGamepadBall_LastEnum;
	}

	//-----------------------------------------------------------------------
}
