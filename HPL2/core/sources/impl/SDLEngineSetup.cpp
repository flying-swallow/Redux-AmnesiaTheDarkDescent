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

#include "impl/SDLEngineSetup.h"

#include "system/System.h"
#include "input/Input.h"
#include "graphics/Graphics.h"
#include "resources/Resources.h"
#include "scene/Scene.h"
#include "sound/Sound.h"
#include "physics/Physics.h"
#include "ai/AI.h"
#include "haptic/Haptic.h"

#include "impl/KeyboardSDL.h"
#include "impl/MouseSDL.h"
#include "impl/LowLevelGraphicsSDL.h"
#include "impl/LowLevelResourcesSDL.h"
#include "impl/LowLevelSystemSDL.h"
#include "impl/LowLevelInputSDL.h"
#include "impl/LowLevelSoundFmod.h"
#include "impl/LowLevelSoundOpenAL.h"
#include "impl/LowLevelPhysicsNewton.h"

#ifdef INCLUDE_HAPTIC 
	#include "impl/LowLevelHapticHaptX.h"
#endif

#include <SDL3/SDL.h>

namespace hpl {

	//////////////////////////////////////////////////////////////////////////
	// CONSTRUCTORS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	cSDLEngineSetup::cSDLEngineSetup(tFlag alHplSetupFlags)
	{
		SDL_SetHint(SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES, "0");
		if(alHplSetupFlags & (eHplSetup_Screen | eHplSetup_Video))
		{
			// SDL3 dropped the timer subsystem flag; the timer API is always
			// available.
			if(!SDL_Init( SDL_INIT_VIDEO )) {
				FatalError("Error Initializing Display: %s",SDL_GetError());
				exit(1);
			}
            SDL_DisableScreenSaver();
		}
		else
		{
			SDL_Init( 0 );
		}
		
		//////////////////////////
		// System
		mpLowLevelSystem = hplNew( cLowLevelSystemSDL, () );
		
		//////////////////////////
		// Graphics
		mpLowLevelGraphics = hplNew( cLowLevelGraphicsSDL,() );
		
		//////////////////////////
		// Input
		mpLowLevelInput = hplNew( cLowLevelInputSDL,(mpLowLevelGraphics) );
		
		//////////////////////////
		// Resources
		mpLowLevelResources = hplNew( cLowLevelResourcesSDL,(mpLowLevelGraphics) );
		
		//////////////////////////
		// Sound
		mpLowLevelSound	= hplNew( cLowLevelSoundOpenAL,() );
		
		//////////////////////////
		// Physics
		mpLowLevelPhysics = hplNew( cLowLevelPhysicsNewton,() );
		
		//////////////////////////
		// Haptic
#ifdef INCLUDE_HAPTIC 
		mpLowLevelHaptic = hplNew( cLowLevelHapticHaptX,() );
#else 
		mpLowLevelHaptic = NULL;
#endif
		
	}

	//-----------------------------------------------------------------------

	cSDLEngineSetup::~cSDLEngineSetup()
	{
		Log("- Deleting lowlevel stuff.\n");
		
		Log("  Physics\n");
		hplDelete(mpLowLevelPhysics);
		Log("  Sound\n");
		hplDelete(mpLowLevelSound);
		Log("  Input\n");
		hplDelete(mpLowLevelInput);
		Log("  Resources\n");
		hplDelete(mpLowLevelResources);
		Log("  System\n");
		hplDelete(mpLowLevelSystem);
		Log("  Graphics\n");
		hplDelete(mpLowLevelGraphics);
		Log("  Haptic\n");
#ifdef INCLUDE_HAPTIC 	
		hplDelete(mpLowLevelHaptic);
#endif

        SDL_EnableScreenSaver();
		SDL_Quit();
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PUBLIC METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------
	
	cScene* cSDLEngineSetup::CreateScene(cGraphics* apGraphics, cResources *apResources, cSound* apSound,
										cPhysics *apPhysics, cSystem *apSystem,cAI *apAI,cGui *apGui,
										cHaptic *apHaptic)
	{
		cScene *pScene = hplNew( cScene, (apGraphics,apResources, apSound,apPhysics, apSystem,apAI,apGui,apHaptic) );
		return pScene;
	}

	//-----------------------------------------------------------------------

	
	/**
	 * \todo Lowlevelresource and resource both use lowlevel graphics. Can this be fixed??
	 * \param apGraphics 
	 * \return 
	 */
	cResources* cSDLEngineSetup::CreateResources(cGraphics* apGraphics)
	{
		cResources *pResources = hplNew( cResources, (mpLowLevelResources,mpLowLevelGraphics) );
		return pResources;
	}
	
	//-----------------------------------------------------------------------

	cInput* cSDLEngineSetup::CreateInput(cGraphics* apGraphics)
	{
		cInput *pInput = hplNew( cInput, (mpLowLevelInput) );
		return pInput;
	}
	
	//-----------------------------------------------------------------------

	cSystem* cSDLEngineSetup::CreateSystem()
	{
		cSystem *pSystem = hplNew( cSystem, (mpLowLevelSystem) );
		return pSystem;
	}
	
	//-----------------------------------------------------------------------

	cGraphics* cSDLEngineSetup::CreateGraphics()
	{
		cGraphics *pGraphics = hplNew( cGraphics, (mpLowLevelGraphics,mpLowLevelResources) );
		return pGraphics;
	}
	//-----------------------------------------------------------------------
	
	cSound* cSDLEngineSetup::CreateSound()
	{
		cSound *pSound = hplNew( cSound, (mpLowLevelSound) );
		return pSound;
	}
	
	//-----------------------------------------------------------------------
	
	cPhysics* cSDLEngineSetup::CreatePhysics()
	{
		cPhysics *pPhysics = hplNew( cPhysics, (mpLowLevelPhysics) );
		return pPhysics;
	}

	//-----------------------------------------------------------------------

	cAI* cSDLEngineSetup::CreateAI()
	{
		cAI *pAI = hplNew( cAI,() );
		return pAI;
	}

	//-----------------------------------------------------------------------

	cHaptic* cSDLEngineSetup::CreateHaptic()
	{
		cHaptic *pHaptic = hplNew( cHaptic, (mpLowLevelHaptic) );
		return pHaptic;
	}

	//-----------------------------------------------------------------------

}
