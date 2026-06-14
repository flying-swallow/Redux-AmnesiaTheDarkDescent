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

#include "impl/KeyboardSDL.h"

#include <SDL3/SDL.h>

#include "impl/LowLevelInputSDL.h"
#include "system/String.h"

#ifdef _WIN32
#include <conio.h>
#endif

namespace hpl {
	
	//////////////////////////////////////////////////////////////////////////
	// CONSTRUCTORS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------
	
	cKeyboardSDL::cKeyboardSDL(cLowLevelInputSDL *apLowLevelInputSDL) : iKeyboard("SDL Portable Keyboard")
	{
		mpLowLevelInputSDL = apLowLevelInputSDL;

		mvKeyArray.resize(eKey_LastEnum);
		ClearKeyList();
	}
	
	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PUBLIC METHODS
	//////////////////////////////////////////////////////////////////////////

	void cKeyboardSDL::Update()
	{
		mlstKeysPressed.clear();
		mlstKeysReleased.clear();
		std::list<SDL_Event>::iterator it = mpLowLevelInputSDL->mlstEvents.begin();
        for(; it != mpLowLevelInputSDL->mlstEvents.end(); ++it)
		{
			SDL_Event *pEvent = &(*it);

			if(pEvent->type == SDL_EVENT_KEY_DOWN || pEvent->type == SDL_EVENT_KEY_UP)
			{
                eKey key = SDLToKey(pEvent->key.key);

                mvKeyArray[key] = pEvent->key.down;
                int sdl_mod = pEvent->key.mod;

                if(pEvent->key.down)
                {
                    AddKeyToList(sdl_mod, key, 0, mlstKeysPressed);
                }
                else
                {
                    AddKeyToList(sdl_mod, key, 0, mlstKeysReleased);
                }
            }
            else if(pEvent->type == SDL_EVENT_TEXT_INPUT)
            {
                tWString tstr = cString::UTF8ToWChar(pEvent->text.text);
                for (size_t i=0,l=tstr.size(); i<l; ++i) {
                    AddKeyToList(SDL_GetModState(), eKey_None, (int)tstr[i], mlstKeysPressed);
                }
            }
        }
	}

	//-----------------------------------------------------------------------

    bool cKeyboardSDL::KeyIsDown(eKey aKey)
	{
		return mvKeyArray[aKey];
	}

	//-----------------------------------------------------------------------


	cKeyPress cKeyboardSDL::GetKey()
	{
		cKeyPress key = mlstKeysPressed.front();
		mlstKeysPressed.pop_front();
		return key;
	}

	//-----------------------------------------------------------------------

	bool cKeyboardSDL::KeyIsPressed()
	{
		return mlstKeysPressed.empty()==false;
	}

	//-----------------------------------------------------------------------

	/*
	int cKeyboardSDL::GetModifier()
	{
		return eKeyModifier_None;
	}
	*/

	//-----------------------------------------------------------------------

	cKeyPress cKeyboardSDL::GetReleasedKey()
	{
		cKeyPress key = mlstKeysReleased.front();
		mlstKeysReleased.pop_front();
		return key;
	}

	//-----------------------------------------------------------------------

	bool cKeyboardSDL::KeyIsReleased()
	{
		return mlstKeysReleased.empty()==false;
	}
	
	//-----------------------------------------------------------------------

	/////////////////////////////////////////////////////////////////////////
	// PRIVATE METHODS
	/////////////////////////////////////////////////////////////////////////
	
	//-----------------------------------------------------------------------
	
	eKey cKeyboardSDL::SDLToKey(int alKey)
	{
		switch(alKey)
		{
			case	SDLK_BACKSPACE: return eKey_BackSpace;
			case	SDLK_TAB:return  eKey_Tab;
			case 	SDLK_CLEAR: return eKey_Clear;
			case 	SDLK_RETURN: return eKey_Return;
			case 	SDLK_PAUSE: return eKey_Pause;
			case 	SDLK_ESCAPE: return eKey_Escape;
			case 	SDLK_SPACE: return eKey_Space;
			case 	SDLK_EXCLAIM: return eKey_Exclaim;
			case 	SDLK_DBLAPOSTROPHE: return eKey_QuoteDouble;
			case 	SDLK_HASH: return eKey_Hash;
			case 	SDLK_DOLLAR: return eKey_Dollar;
			case 	SDLK_AMPERSAND: return eKey_Ampersand;
			case 	SDLK_APOSTROPHE: return eKey_Quote;
			case 	SDLK_LEFTPAREN: return eKey_LeftParen;
			case 	SDLK_RIGHTPAREN: return eKey_RightParen;
			case 	SDLK_ASTERISK: return eKey_Asterisk;
			case 	SDLK_PLUS: return eKey_Plus;
			case 	SDLK_COMMA: return eKey_Comma;
			case 	SDLK_MINUS: return eKey_Minus;
			case 	SDLK_PERIOD: return eKey_Period;
			case 	SDLK_SLASH: return eKey_Slash;
			case 	SDLK_0: return eKey_0;
			case 	SDLK_1: return eKey_1;
			case 	SDLK_2: return eKey_2;
			case 	SDLK_3: return eKey_3;
			case 	SDLK_4: return eKey_4;
			case 	SDLK_5: return eKey_5;
			case 	SDLK_6: return eKey_6;
			case 	SDLK_7: return eKey_7;
			case 	SDLK_8: return eKey_8;
			case 	SDLK_9: return eKey_9;
			case 	SDLK_COLON: return eKey_Colon;
			case 	SDLK_SEMICOLON: return eKey_SemiColon;
			case 	SDLK_LESS: return eKey_Less;
			case 	SDLK_EQUALS: return eKey_Equals;
			case 	SDLK_GREATER: return eKey_Greater;
			case 	SDLK_QUESTION: return eKey_Question;
			case 	SDLK_AT: return eKey_At;
			case 	SDLK_LEFTBRACKET: return eKey_LeftBracket;
			case 	SDLK_BACKSLASH: return eKey_BackSlash;
			case 	SDLK_RIGHTBRACKET: return eKey_RightBracket;
			case 	SDLK_CARET: return eKey_Caret;
			case 	SDLK_UNDERSCORE: return eKey_Underscore;
			case 	SDLK_GRAVE: return eKey_BackSlash;
			case 	SDLK_A: return eKey_A;
			case 	SDLK_B: return eKey_B;
			case 	SDLK_C: return eKey_C;
			case 	SDLK_D: return eKey_D;
			case 	SDLK_E: return eKey_E;
			case 	SDLK_F: return eKey_F;
			case 	SDLK_G: return eKey_G;
			case 	SDLK_H: return eKey_H;
			case 	SDLK_I: return eKey_I;
			case 	SDLK_J: return eKey_J;
			case 	SDLK_K: return eKey_K;
			case 	SDLK_L: return eKey_L;
			case 	SDLK_M: return eKey_M;
			case 	SDLK_N: return eKey_N;
			case 	SDLK_O: return eKey_O;
			case 	SDLK_P: return eKey_P;
			case 	SDLK_Q: return eKey_Q;
			case 	SDLK_R: return eKey_R;
			case 	SDLK_S: return eKey_S;
			case 	SDLK_T: return eKey_T;
			case 	SDLK_U: return eKey_U;
			case 	SDLK_V: return eKey_V;
			case 	SDLK_W: return eKey_W;
			case 	SDLK_X: return eKey_X;
			case 	SDLK_Y: return eKey_Y;
			case 	SDLK_Z: return eKey_Z;
			case 	SDLK_DELETE: return eKey_Delete;
			case 	SDLK_KP_0: return eKey_KP_0;
			case 	SDLK_KP_1: return eKey_KP_1;
			case 	SDLK_KP_2: return eKey_KP_2;
			case 	SDLK_KP_3: return eKey_KP_3;
			case 	SDLK_KP_4: return eKey_KP_4;
			case 	SDLK_KP_5: return eKey_KP_5;
			case 	SDLK_KP_6: return eKey_KP_6;
			case 	SDLK_KP_7: return eKey_KP_7;
			case 	SDLK_KP_8: return eKey_KP_8;
			case 	SDLK_KP_9: return eKey_KP_9;
			case 	SDLK_KP_PERIOD: return eKey_KP_Period;
			case 	SDLK_KP_DIVIDE: return eKey_KP_Divide;
			case 	SDLK_KP_MULTIPLY: return eKey_KP_Multiply;
			case 	SDLK_KP_MINUS: return eKey_KP_Minus;
			case 	SDLK_KP_PLUS: return eKey_KP_Plus;
			case 	SDLK_KP_ENTER: return eKey_KP_Enter;
			case 	SDLK_KP_EQUALS: return eKey_KP_Equals;
			case 	SDLK_UP: return eKey_Up;
			case 	SDLK_DOWN: return eKey_Down;
			case 	SDLK_RIGHT: return eKey_Right;
			case 	SDLK_LEFT: return eKey_Left;
			case 	SDLK_INSERT: return eKey_Insert;
			case 	SDLK_HOME: return eKey_Home;
			case 	SDLK_END: return eKey_End;
			case 	SDLK_PAGEUP: return eKey_PageUp;
			case 	SDLK_PAGEDOWN: return eKey_PageDown;
			case 	SDLK_F1: return eKey_F1;
			case 	SDLK_F2: return eKey_F2;
			case 	SDLK_F3: return eKey_F3;
			case 	SDLK_F4: return eKey_F4;
			case 	SDLK_F5: return eKey_F5;
			case 	SDLK_F6: return eKey_F6;
			case 	SDLK_F7: return eKey_F7;
			case 	SDLK_F8: return eKey_F8;
			case 	SDLK_F9: return eKey_F9;
			case 	SDLK_F10: return eKey_F10;
			case 	SDLK_F11: return eKey_F11;
			case 	SDLK_F12: return eKey_F12;
			case 	SDLK_F13: return eKey_F13;
			case 	SDLK_F14: return eKey_F14;
			case 	SDLK_F15: return eKey_F15;
            case    SDLK_NUMLOCKCLEAR: return eKey_NumLock;
			case 	SDLK_SCROLLLOCK: return eKey_ScrollLock;
			case 	SDLK_LGUI: return eKey_LeftSuper;
			case 	SDLK_RGUI: return eKey_RightSuper;
            case    SDLK_PRINTSCREEN: return eKey_Print;
			case 	SDLK_CAPSLOCK: return eKey_CapsLock;
			case 	SDLK_RSHIFT: return eKey_RightShift;
			case 	SDLK_LSHIFT: return eKey_LeftShift;
			case 	SDLK_RCTRL: return eKey_RightCtrl;
			case 	SDLK_LCTRL: return eKey_LeftCtrl;
			case 	SDLK_RALT: return eKey_RightAlt;
			case 	SDLK_LALT: return eKey_LeftAlt;
			case 	SDLK_MODE: return eKey_Mode;
			case 	SDLK_HELP: return eKey_Help;
			case 	SDLK_SYSREQ: return eKey_SysReq;
			case 	SDLK_MENU: return eKey_Menu;
			case 	SDLK_POWER: return eKey_Power;
		}

		return eKey_None;
	}
	
	//-----------------------------------------------------------------------

	void cKeyboardSDL::ClearKeyList()
	{
		mvKeyArray.assign(mvKeyArray.size(),false);
	}

	//-----------------------------------------------------------------------

	void cKeyboardSDL::AddKeyToList(int alSDLMod, eKey aKey, int alUnicode, std::list<cKeyPress>& alstKeys)
	{
		int mod =0;

		if(alSDLMod & SDL_KMOD_CTRL)		mod |= eKeyModifier_Ctrl;
		if(alSDLMod & SDL_KMOD_SHIFT)		mod |= eKeyModifier_Shift;
		if(alSDLMod & SDL_KMOD_ALT)			mod |= eKeyModifier_Alt;
		
		alstKeys.push_back(cKeyPress(aKey,alUnicode,mod));
		
		//if(mlstKeysPressed.size()>MAX_KEY_PRESSES) mlstKeysPressed.pop_front();
	}

	//-----------------------------------------------------------------------

}
