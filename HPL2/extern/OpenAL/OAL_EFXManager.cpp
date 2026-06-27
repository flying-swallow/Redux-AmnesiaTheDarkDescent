#include "OpenAL/OAL_EFXManager.h"
#include "OpenAL/OAL_EffectSlot.h"
#include "OpenAL/OAL_Effect.h"
#include "OpenAL/OAL_Effect_Reverb.h"
#include "OpenAL/OAL_Filter.h"
#include "OpenAL/OAL_Device.h"


#include "system/MemoryManager.h"
#include "system/LowLevelSystem.h"


int SlotUpdaterThread(void* alUnusedArg);

extern cOAL_Device* gpDevice;

// Set EFX func pointers to null (maybe this should be in OAL_Device.cpp? )

// Effect Slots

LPALGENAUXILIARYEFFECTSLOTS oalw_alGenAuxiliaryEffectSlots = NULL;
LPALDELETEAUXILIARYEFFECTSLOTS oalw_alDeleteAuxiliaryEffectSlots = NULL;
LPALISAUXILIARYEFFECTSLOT oalw_alIsAuxiliaryEffectSlot = NULL;
LPALAUXILIARYEFFECTSLOTI oalw_alAuxiliaryEffectSloti = NULL;
LPALAUXILIARYEFFECTSLOTIV oalw_alAuxiliaryEffectSlotiv = NULL;
LPALAUXILIARYEFFECTSLOTF oalw_alAuxiliaryEffectSlotf = NULL;
LPALAUXILIARYEFFECTSLOTFV oalw_alAuxiliaryEffectSlotfv = NULL;
LPALGETAUXILIARYEFFECTSLOTI oalw_alGetAuxiliaryEffectSloti = NULL;
LPALGETAUXILIARYEFFECTSLOTIV oalw_alGetAuxiliaryEffectSlotiv = NULL;
LPALGETAUXILIARYEFFECTSLOTF oalw_alGetAuxiliaryEffectSlotf = NULL;
LPALGETAUXILIARYEFFECTSLOTFV oalw_alGetAuxiliaryEffectSlotfv = NULL;

// Effects

LPALGENEFFECTS oalw_alGenEffects = NULL;
LPALDELETEEFFECTS oalw_alDeleteEffects = NULL;
LPALISEFFECT oalw_alIsEffect = NULL;
LPALEFFECTI oalw_alEffecti = NULL;
LPALEFFECTIV oalw_alEffectiv = NULL;
LPALEFFECTF oalw_alEffectf = NULL;
LPALEFFECTFV oalw_alEffectfv = NULL;
LPALGETEFFECTI oalw_alGetEffecti = NULL;
LPALGETEFFECTIV oalw_alGetEffectiv = NULL;
LPALGETEFFECTF oalw_alGetEffectf = NULL;
LPALGETEFFECTFV oalw_alGetEffectfv = NULL;

// Filters

LPALGENFILTERS oalw_alGenFilters = NULL;
LPALDELETEFILTERS oalw_alDeleteFilters = NULL;
LPALISFILTER oalw_alIsFilter = NULL;
LPALFILTERI oalw_alFilteri = NULL;
LPALFILTERIV oalw_alFilteriv = NULL;
LPALFILTERF oalw_alFilterf = NULL;
LPALFILTERFV oalw_alFilterfv = NULL;
LPALGETFILTERI oalw_alGetFilteri = NULL;
LPALGETFILTERIV oalw_alGetFilteriv = NULL;
LPALGETFILTERF oalw_alGetFilterf = NULL;
LPALGETFILTERFV oalw_alGetFilterfv = NULL;

cOAL_EFXManager::cOAL_EFXManager() : mlNumSlots(0), mpvSlots(NULL), mplstEffectList(NULL), mplstFilterList(NULL)  
{
}

cOAL_EFXManager::~cOAL_EFXManager()
{
}

bool cOAL_EFXManager::Initialize(int alNumSlotsHint, int alNumSends, bool abUseThreading, int alSlotUpdateFreq)
{
	DEF_FUNC_NAME(cOAL_EFXManager::Initialize);
	FUNC_USES_AL;
	
	ALuint lTempSlot[256];
    
	// Set up every EFX function pointer 

	LogMsg("",eOAL_LogVerbose_Medium, eOAL_LogMsg_Info, "Initializing EFX Manager...\n" );
	LogMsg("",eOAL_LogVerbose_Medium, eOAL_LogMsg_Info, "Initializing function pointers...\n" );



	// Slot funcs
	oalw_alGenAuxiliaryEffectSlots		=	(LPALGENAUXILIARYEFFECTSLOTS) alGetProcAddress ("alGenAuxiliaryEffectSlots");
	oalw_alDeleteAuxiliaryEffectSlots	=	(LPALDELETEAUXILIARYEFFECTSLOTS) alGetProcAddress ("alDeleteAuxiliaryEffectSlots");
	oalw_alIsAuxiliaryEffectSlot			=	(LPALISAUXILIARYEFFECTSLOT) alGetProcAddress ("alIsAuxiliaryEffectSlot");
	oalw_alAuxiliaryEffectSloti			=	(LPALAUXILIARYEFFECTSLOTI) alGetProcAddress ("alAuxiliaryEffectSloti");
	oalw_alAuxiliaryEffectSlotiv			=	(LPALAUXILIARYEFFECTSLOTIV) alGetProcAddress ("alAuxiliaryEffectSlotiv");
	oalw_alAuxiliaryEffectSlotf			=	(LPALAUXILIARYEFFECTSLOTF) alGetProcAddress ("alAuxiliaryEffectSlotf");
	oalw_alAuxiliaryEffectSlotfv			=	(LPALAUXILIARYEFFECTSLOTFV) alGetProcAddress ("alAuxiliaryEffectSlotfv");
	oalw_alGetAuxiliaryEffectSloti		=	(LPALGETAUXILIARYEFFECTSLOTI) alGetProcAddress ("alGetAuxiliaryEffectSloti");
	oalw_alGetAuxiliaryEffectSlotiv		=	(LPALGETAUXILIARYEFFECTSLOTIV) alGetProcAddress ("alGetAuxiliaryEffectSlotiv");
	oalw_alGetAuxiliaryEffectSlotf		=	(LPALGETAUXILIARYEFFECTSLOTF) alGetProcAddress ("alGetAuxiliaryEffectSlotf");
	oalw_alGetAuxiliaryEffectSlotfv		=	(LPALGETAUXILIARYEFFECTSLOTFV) alGetProcAddress ("alGetAuxiliaryEffectSlotfv");

	// Effect funcs
	oalw_alGenEffects	=	(LPALGENEFFECTS) alGetProcAddress ("alGenEffects");
	oalw_alDeleteEffects =	(LPALDELETEEFFECTS) alGetProcAddress ("alDeleteEffects");
	oalw_alIsEffect		=	(LPALISEFFECT) alGetProcAddress ("alIsEffect");
	oalw_alEffecti		=	(LPALEFFECTI) alGetProcAddress ("alEffecti");
	oalw_alEffectiv		=	(LPALEFFECTIV) alGetProcAddress ("alEffectiv");
	oalw_alEffectf		=	(LPALEFFECTF) alGetProcAddress ("alEffectf");
	oalw_alEffectfv		=	(LPALEFFECTFV) alGetProcAddress ("alEffectfv");
	oalw_alGetEffecti	=	(LPALGETEFFECTI) alGetProcAddress ("alGetEffecti");
	oalw_alGetEffectiv	=	(LPALGETEFFECTIV) alGetProcAddress ("alGetEffectiv");
	oalw_alGetEffectf	=	(LPALGETEFFECTF) alGetProcAddress ("alGetEffectf");
	oalw_alGetEffectfv	=	(LPALGETEFFECTFV) alGetProcAddress ("alGetEffectfv");
	
	// Filter funcs
	oalw_alGenFilters	= (LPALGENFILTERS) alGetProcAddress ("alGenFilters");
	oalw_alDeleteFilters = (LPALDELETEFILTERS) alGetProcAddress ("alDeleteFilters");
	oalw_alIsFilter		= (LPALISFILTER) alGetProcAddress ("alIsFilter");
	oalw_alFilteri		= (LPALFILTERI) alGetProcAddress ("alFilteri");
	oalw_alFilteriv		= (LPALFILTERIV) alGetProcAddress ("alFilteriv");
	oalw_alFilterf		= (LPALFILTERF) alGetProcAddress ("alFilterf");
	oalw_alFilterfv		= (LPALFILTERFV) alGetProcAddress ("alFilterfv");
	oalw_alGetFilteri	= (LPALGETFILTERI) alGetProcAddress ("alGetFilteri");
	oalw_alGetFilteriv	= (LPALGETFILTERIV) alGetProcAddress ("alGetFilteriv");
	oalw_alGetFilterf	= (LPALGETFILTERF) alGetProcAddress ("alGetFilterf");
	oalw_alGetFilterfv	= (LPALGETFILTERFV) alGetProcAddress ("alGetFilterfv");

	if (!(oalw_alGenAuxiliaryEffectSlots && oalw_alDeleteAuxiliaryEffectSlots && oalw_alIsAuxiliaryEffectSlot &&
		oalw_alAuxiliaryEffectSloti && oalw_alAuxiliaryEffectSlotiv && oalw_alAuxiliaryEffectSlotf && oalw_alAuxiliaryEffectSlotfv &&
		oalw_alGetAuxiliaryEffectSloti && oalw_alGetAuxiliaryEffectSlotiv && oalw_alGetAuxiliaryEffectSlotf && oalw_alGetAuxiliaryEffectSlotfv &&

		oalw_alGenEffects && oalw_alDeleteEffects && oalw_alIsEffect &&
		oalw_alEffecti && oalw_alEffectiv && oalw_alEffectf && oalw_alEffectfv && 
		oalw_alGetEffecti && oalw_alGetEffectiv && oalw_alGetEffectf && oalw_alGetEffectfv &&

		oalw_alGenFilters && oalw_alDeleteFilters && oalw_alIsFilter &&
		oalw_alFilteri && oalw_alFilteriv && oalw_alFilterf && oalw_alFilterfv &&
		oalw_alGetFilteri && oalw_alGetFilteriv && oalw_alGetFilterf && oalw_alGetFilterfv))
	{
		LogMsg("",eOAL_LogVerbose_Medium, eOAL_LogMsg_Error, "Failed initializing function pointers\n" );
		return false;
	}
	LogMsg("",eOAL_LogVerbose_Medium, eOAL_LogMsg_Info, "Done\n" );

	LogMsg("",eOAL_LogVerbose_Medium, eOAL_LogMsg_Info, "Calculating max slots...\n" );

	while ( mlNumSlots < alNumSlotsHint )
	{
		RUN_AL_FUNC(oalw_alGenAuxiliaryEffectSlots(1,&lTempSlot[mlNumSlots]));
		if (!AL_ERROR_OCCURED)
		{
			LogMsg("",eOAL_LogVerbose_High, eOAL_LogMsg_Info, "Effect Slot Object successfully created\n" );
			++mlNumSlots;
		}
		else
			break;
	}

	if ( mlNumSlots == 0 )
	{
		LogMsg("",eOAL_LogVerbose_Low, eOAL_LogMsg_Error, "Error creating Slots. Failed to initialize EFX.\n" );
		return false;
	}

	LogMsg("",eOAL_LogVerbose_Medium, eOAL_LogMsg_Info, "Created %d Low Level Effect Slots, %d hinted\n", mlNumSlots, alNumSlotsHint);

	RUN_AL_FUNC(oalw_alDeleteAuxiliaryEffectSlots ( mlNumSlots, lTempSlot ));
	
	mpvSlots = hplNew (tSlotVector ,());
	mpvSlots->reserve(mlNumSlots);

	LogMsg("",eOAL_LogVerbose_Medium, eOAL_LogMsg_Info, "Creating High Level Effect Slots\n" );


	for ( int i = 0; i < mlNumSlots; i++ )
	{
		cOAL_EffectSlot *pSlot = hplNew( cOAL_EffectSlot,(this, i) );
		mpvSlots->push_back(pSlot);
	}

	LogMsg("",eOAL_LogVerbose_Medium, eOAL_LogMsg_Info, "Done creating Effect Slots\n" );

	
	LogMsg("",eOAL_LogVerbose_Medium, eOAL_LogMsg_Info, "Creating Filter and Effect containers\n" );
	mplstEffectList = hplNew( tOALEffectList, () );
	mplstFilterList = hplNew( tOALFilterList, () );

	if (!mplstEffectList || !mplstFilterList)
	{
		LogMsg("",eOAL_LogVerbose_Low, eOAL_LogMsg_Error, "Error creating containers. Failed to initialize EFX.\n" );
		return false;
	}
	else
		LogMsg("",eOAL_LogVerbose_Medium, eOAL_LogMsg_Info, "Done creating containers\n" );

	mlNumSends = alNumSends;
	mbUsingThread = abUseThreading;

	// Launch updater thread
	if (mbUsingThread)
	{
		LogMsg("",eOAL_LogVerbose_Medium, eOAL_LogMsg_Info, "Launching Slot updater thread...\n" );

		mlThreadWaitTime = 1000/alSlotUpdateFreq;
        
#ifdef USE_SDL2
		mpUpdaterThread = SDL_CreateThread(SlotUpdaterThread, "SlotUpdater", NULL);
#else
		mpUpdaterThread = SDL_CreateThread(SlotUpdaterThread, NULL);
#endif
	}
	
	LogMsg("",eOAL_LogVerbose_Medium, eOAL_LogMsg_Info, "EFX succesfully initialized.\n" );

	return true;


}

void cOAL_EFXManager::Destroy()
{
	tSlotVectorIt vSlotIterator;
	tOALEffectListIt lstEffectIterator;
	tOALFilterListIt lstFilterIterator;

	LogMsg("",eOAL_LogVerbose_Medium, eOAL_LogMsg_Command, "Destroying EFX Manager...\n" );

	if ( mbUsingThread )							
	{
		LogMsg("",eOAL_LogVerbose_Medium, eOAL_LogMsg_Info, "Stopping Slot updater...\n" );
		mbUsingThread = false;
		SDL_WaitThread ( mpUpdaterThread, 0 );
		mpUpdaterThread = NULL;
	}

	LogMsg("",eOAL_LogVerbose_Medium, eOAL_LogMsg_Info, "Destroying Effect Slots...\n" );

	if (mpvSlots)
	{
		for ( vSlotIterator = mpvSlots->begin(); vSlotIterator != mpvSlots->end(); ++vSlotIterator )
		{
			(*vSlotIterator)->Reset();
			hplDelete((*vSlotIterator));
		}
		mpvSlots->clear();
		hplDelete(mpvSlots);
		
		mpvSlots = NULL;
	}

	LogMsg("",eOAL_LogVerbose_Medium, eOAL_LogMsg_Info, "Destroying Effects...\n" );
	
	if (mplstEffectList)
	{
		for ( lstEffectIterator = mplstEffectList->begin(); lstEffectIterator != mplstEffectList->end(); ++lstEffectIterator )
		{
			hplDelete ((*lstEffectIterator));
		}
		mplstEffectList->clear();
		hplDelete(mplstEffectList);
		mplstEffectList = NULL;
	}

	LogMsg("",eOAL_LogVerbose_Medium, eOAL_LogMsg_Info, "Destroying Filters...\n" );

	if (mplstFilterList)
	{
		for ( lstFilterIterator = mplstFilterList->begin(); lstFilterIterator != mplstFilterList->end(); ++lstFilterIterator )
		{
			hplDelete ((*lstFilterIterator));
		}
		mplstFilterList->clear();
		hplDelete(mplstFilterList);
		mplstFilterList = NULL;
	}

	LogMsg("",eOAL_LogVerbose_Medium, eOAL_LogMsg_Info, "EFX Manager successfully destroyed\n" );

}

//////////////////////////////////////////////////////////////

cOAL_Filter* cOAL_EFXManager::CreateFilter()
{
    cOAL_Filter* pFilter = hplNew (cOAL_Filter,());

	if(pFilter && pFilter->GetStatus())
	{
		mplstFilterList->push_back(pFilter);
		return pFilter;
	}
	else
	{
		hplDelete ( pFilter );
		pFilter = NULL;
		
		return NULL;
	}
}

////////////////////////////////////////////////////////////

cOAL_Effect_Reverb* cOAL_EFXManager::CreateReverbEffect()
{
	cOAL_Effect_Reverb* pEffect = hplNew (cOAL_Effect_Reverb, () );

	if (pEffect && pEffect->GetStatus())
	{
		mplstEffectList->push_back(pEffect);
		return pEffect;
	}
	else
	{
        hplDelete (pEffect);
		pEffect = NULL;
		
		return NULL;
	}
	
}

////////////////////////////////////////////////////////////

void cOAL_EFXManager::DestroyFilter ( cOAL_Filter* apFilter )
{
	if (apFilter == NULL)
		return;

	mplstFilterList->remove(apFilter);
	hplDelete (apFilter);
}

////////////////////////////////////////////////////////////

void cOAL_EFXManager::DestroyEffect ( cOAL_Effect *apEffect )
{
	if (apEffect == NULL)
		return;

	mplstEffectList->remove(apEffect);
	hplDelete (apEffect);
}

////////////////////////////////////////////////////////////

int cOAL_EFXManager::UseEffect ( cOAL_Effect *apEffect )
{
	if (apEffect == NULL)
		return -1;

	cOAL_EffectSlot* pSlot = NULL;
	for (int i = 0; i < mlNumSlots; ++i)
	{
		pSlot = (*mpvSlots)[i];
		if (pSlot->IsFree())
		{
			if (pSlot->AttachEffect(apEffect))
            	return i;
			else
			{
				pSlot->Reset();
			}
		}
	}
	return -1;
}

void cOAL_EFXManager::UpdateSlots()
{
	cOAL_EffectSlot* pSlot;
	for (int i = 0; i < mlNumSlots; ++i)
	{
		pSlot = (*mpvSlots)[i];
		pSlot->Lock();
		pSlot->Update();
		pSlot->Unlock();
	}
}

inline int cOAL_EFXManager::GetThreadWaitTime()
{
	return mlThreadWaitTime;
}

int SlotUpdaterThread ( void* alUnusedArg )
{
	cOAL_EFXManager* pEFXManager = gpDevice->GetEFXManager();
	
	int lWaitTime = pEFXManager->GetThreadWaitTime();

	while(pEFXManager->IsThreadAlive())
	{
		//	While the thread lives, perform the update
        pEFXManager->UpdateSlots();
		//	And rest a bit
		SDL_Delay ( lWaitTime );			
	}
	return 0;
}

