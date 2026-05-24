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

#include "LuxPostEffects.h"

#include "LuxMapHandler.h"

//////////////////////////////////////////////////////////////////////////
// VARIABLES
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

#define kVar_afAlpha			0
#define kVar_afT				1
#define kVar_avScreenSize		2
#define kVar_afAmpT				3
#define kVar_afWaveAlpha		4
#define kVar_afZoomAlpha		5

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// INSANITY
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

cLuxPostEffect_Insanity::cLuxPostEffect_Insanity(cGraphics *apGraphics, cResources *apResources) : iLuxPostEffect(apGraphics, apResources)
{
	//////////////////////////////
	// Create program
	cParserVarContainer vars;
	vars.Add("UseUv");
	mpProgram = mpGraphics->CreateGpuProgramFromShaders("LuxInsanity","deferred_base_vtx.glsl", "posteffect_insanity_frag.glsl", &vars);
	if(mpProgram)
	{
		mpProgram->GetVariableAsId("afAlpha",kVar_afAlpha);
		mpProgram->GetVariableAsId("afT",kVar_afT);
		mpProgram->GetVariableAsId("avScreenSize",kVar_avScreenSize);
		mpProgram->GetVariableAsId("afAmpT",kVar_afAmpT);
		mpProgram->GetVariableAsId("afWaveAlpha",kVar_afWaveAlpha);
		mpProgram->GetVariableAsId("afZoomAlpha",kVar_afZoomAlpha);
	}


	//////////////////////////////
	// Textures
	mvAmpMaps.resize(3);
	
	for(size_t i=0; i<mvAmpMaps.size(); ++i)
		mvAmpMaps[i] = mpResources->GetTextureManager()->Create2DImage("posteffect_insanity_ampmap"+cString::ToString((int)i), false);

	mpZoomMap = mpResources->GetTextureManager()->Create2DImage("posteffect_insanity_zoom.jpg", false);

	//////////////////////////////
	// Init vars
	mfT =0;
	mfAnimCount =0;
	mfWaveAlpha = 0.0f;
	mfZoomAlpha = 0.0f;
	mfWaveSpeed =0.0f;
}

//-----------------------------------------------------------------------

cLuxPostEffect_Insanity::~cLuxPostEffect_Insanity()
{

}

//-----------------------------------------------------------------------

void cLuxPostEffect_Insanity::Update(float afTimeStep)
{
	mfT += afTimeStep * mfWaveSpeed;
	
	mfAnimCount += afTimeStep * 0.15f;

	float fMaxAnim = (float)mvAmpMaps.size();
	if(mfAnimCount >= fMaxAnim) mfAnimCount = mfAnimCount-fMaxAnim;
}

//-----------------------------------------------------------------------


void cLuxPostEffect_Insanity::RenderEffect(const hpl::PostEffectRenderCtx &ctx)
{
	// Vulkan-bindless port — the cLuxPostEffect_Insanity shader has not
	// yet been ported to Slang. Until that lands, the effect is a no-op
	// (composite will toggle the pogo buffer after this call without us
	// having written anything, so the buffer halves swap roles but the
	// "just-written" half still carries the prior effect's output —
	// which is what a passthrough would do).
	(void)ctx;
}


//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// POST EFFECT HANDLER
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

cLuxPostEffectHandler::cLuxPostEffectHandler() : iLuxUpdateable("LuxPostEffectHandler")
{
	cGraphics *pGraphics = gpBase->mpEngine->GetGraphics();
	cResources *pResources = gpBase->mpEngine->GetResources();

	///////////////////////
	// Create post effects
	mpInsanity = hplNew(cLuxPostEffect_Insanity, (pGraphics, pResources) );
	AddEffect(mpInsanity, 25);
	mpInsanity->SetActive(true);
}

//-----------------------------------------------------------------------

cLuxPostEffectHandler::~cLuxPostEffectHandler()
{
	STLDeleteAll(mvPostEffects);
}

//-----------------------------------------------------------------------

void cLuxPostEffectHandler::OnStart()
{

}

//-----------------------------------------------------------------------

void cLuxPostEffectHandler::Update(float afTimeStep)
{
	for(size_t i=0; i<mvPostEffects.size(); ++i)
	{
		iLuxPostEffect *pPostEffect = mvPostEffects[i];

        if(pPostEffect->IsActive()) pPostEffect->Update(afTimeStep);
	}
}

//-----------------------------------------------------------------------

void cLuxPostEffectHandler::Reset()
{

}

//-----------------------------------------------------------------------

void cLuxPostEffectHandler::LoadMainConfig()
{
	cConfigFile *pMainCfg = gpBase->mpMainConfig;

	mpInsanity->SetDisabled(pMainCfg->GetBool("Graphics", "PostEffectInsanity", true)==false);

}

//-----------------------------------------------------------------------

void cLuxPostEffectHandler::SaveMainConfig()
{
	cConfigFile *pMainCfg = gpBase->mpMainConfig;

	pMainCfg->SetBool("Graphics", "PostEffectInsanity", mpInsanity->IsDisabled()==false);
}

//-----------------------------------------------------------------------

void cLuxPostEffectHandler::AddEffect(iLuxPostEffect *apPostEffect, int alPrio)
{
	mvPostEffects.push_back(apPostEffect);
	apPostEffect->SetActive(false);
	gpBase->mpMapHandler->GetViewport()->GetPostEffectComposite()->AddPostEffect(apPostEffect, alPrio);
}

//-----------------------------------------------------------------------
