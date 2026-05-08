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

	LogMsg("",eOAL_LogVerbose_Medium, eOAL_LogMsg_Info, "Initializing EFX Manager...\n" );

	// EFX entry points come straight from statically-linked openal-soft —
	// no alGetProcAddress dance, no per-symbol nullness check. If the device
	// doesn't actually advertise ALC_EXT_EFX, the alGen* below sets
	// AL_INVALID_OPERATION and we exit through the no-slots-created path.

	LogMsg("",eOAL_LogVerbose_Medium, eOAL_LogMsg_Info, "Calculating max slots...\n" );

	while ( mlNumSlots < alNumSlotsHint )
	{
		RUN_AL_FUNC(alGenAuxiliaryEffectSlots(1,&lTempSlot[mlNumSlots]));
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

	RUN_AL_FUNC(alDeleteAuxiliaryEffectSlots ( mlNumSlots, lTempSlot ));
	
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

