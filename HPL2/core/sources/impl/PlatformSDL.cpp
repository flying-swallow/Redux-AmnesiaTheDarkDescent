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

#include "system/Platform.h"

#include "system/String.h"

#include "system/LowLevelSystem.h"

#include <SDL3/SDL.h>

#include "impl/TimerSDL.h"
#include "impl/ThreadSDL.h"
#include "impl/MutexSDL.h"

#include <set>
#include <algorithm>

namespace hpl {

	// SDL3 addresses displays by SDL_DisplayID rather than a contiguous index;
	// translate the engine's display index through the live display list.
	static SDL_DisplayID GetDisplayIDByIndex(int alDisplay)
	{
		SDL_DisplayID result = 0;
		int count = 0;
		SDL_DisplayID *displays = SDL_GetDisplays(&count);
		if (displays)
		{
			if (alDisplay >= 0 && alDisplay < count)
				result = displays[alDisplay];
			else if (count > 0)
				result = displays[0];
			SDL_free(displays);
		}
		return result;
	}

	//////////////////////////////////////////////////////////////////////////
	// APPLICATION
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	unsigned long cPlatform::GetApplicationTime()
	{
		// SDL3's SDL_GetTicks() returns Uint64 milliseconds since init.
		return (unsigned long)SDL_GetTicks();
	}

	//-----------------------------------------------------------------------

	void cPlatform::Sleep ( const unsigned int alMillisecs )
	{
		SDL_Delay ( alMillisecs );
	}

	void cPlatform::CopyTextToClipboard(const tWString &asText)
	{
        tString tstr = cString::S16BitToUTF8(asText);
        SDL_SetClipboardText(tstr.c_str());
	}

	//-----------------------------------------------------------------------

	tWString cPlatform::LoadTextFromClipboard()
	{
        tWString tstr;
        if (SDL_HasClipboardText()) {
            // Gets utf8 encoded text
            char * clip = SDL_GetClipboardText();
            if (clip) {
                tstr = cString::UTF8ToWChar(clip);
                SDL_free(clip);
            }
        }
		return tstr;
	}

	void cPlatform::GetDisplayResolution(int alDisplay, int& alWidth, int& alHeight)
	{
		const SDL_DisplayMode *desktop = SDL_GetDesktopDisplayMode(GetDisplayIDByIndex(alDisplay));
		if (desktop)
		{
			alWidth = desktop->w;
			alHeight = desktop->h;
		}
		else
		{
			alWidth = 1024;
			alHeight = 768;
		}
	}

	//-----------------------------------------------------------------------
	void cPlatform::GetAvailableVideoModes(tVideoModeVec& avDestVidModes, int alMinBpp, int alMinRefreshRate)
	{
        int ndisplays = 0;
        SDL_DisplayID *displays = SDL_GetDisplays(&ndisplays);
        if (!displays) return;

        std::set<cVideoMode, VideoComp> uniqVideoModes;

        for (int d=0; d<ndisplays; ++d)
        {
            SDL_DisplayID did = displays[d];
            const SDL_DisplayMode *desktop = SDL_GetDesktopDisplayMode(did);
            if (!desktop) continue;

            int nmodes = 0;
            SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(did, &nmodes);
            if (modes)
            {
                for (int m = 0; m < nmodes; ++m)
                {
                    SDL_DisplayMode *mode = modes[m];

                    if (SDL_BITSPERPIXEL(desktop->format) != (int)SDL_BITSPERPIXEL(mode->format))
                    {
                        continue;
                    }
                    cVideoMode vidMode(
                                       d,
                                       cVector2l(mode->w, mode->h),
                                       SDL_BITSPERPIXEL(mode->format),
                                       1
                                       );
                    uniqVideoModes.insert(vidMode);
                }
                SDL_free(modes);
            }
            // Add fullscreen desktop mode
            uniqVideoModes.insert(cVideoMode(d, cVector2l(0,0), SDL_BITSPERPIXEL(desktop->format), 1));
        }
        SDL_free(displays);

        avDestVidModes.assign(uniqVideoModes.begin(), uniqVideoModes.end());
	}

    tWString cPlatform::GetDisplayName(int alDisplay)
    {
        return cString::To16Char(SDL_GetDisplayName(GetDisplayIDByIndex(alDisplay)));
    }

#ifndef HPL_MINIMAL
	//-----------------------------------------------------------------------

	iTimer * cPlatform::CreateTimer()
	{
		return hplNew(cTimerSDL, () );
	}


	//////////////////////////////////////////////////////////////////////////
	// THREADING
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------
	iThread* cPlatform::CreateThread(iThreadClass* apThreadClass)
	{
		iThread* pThread = hplNew(cThreadSDL, ());
		pThread->SetThreadClass(apThreadClass);

		return pThread;
	}

	//-----------------------------------------------------------------------

	iMutex* cPlatform::CreateMutEx()
	{
		return hplNew(cMutexSDL, ());
	}
#endif
}
