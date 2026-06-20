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

#include "resources/ResourceManager.h"

#include "system/String.h"
#include "system/Platform.h"

#include "resources/LowLevelResources.h"
#include "resources/FileSearcher.h"
#include "resources/ResourceBase.h"

#include "system/LowLevelSystem.h"

#include <algorithm>

namespace hpl {

	int iResourceManager::mlTabCount=0;

	//////////////////////////////////////////////////////////////////////////
	// CONSTRUCTORS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	iResourceManager::iResourceManager(cFileSearcher *apFileSearcher, 
										iLowLevelResources *apLowLevelResources,
										iLowLevelSystem *apLowLevelSystem)
	{
		mpFileSearcher = apFileSearcher;
		mpLowLevelResources = apLowLevelResources;
		mpLowLevelSystem = apLowLevelSystem;
	}

	//-----------------------------------------------------------------------

	// Default free: unregister + delete. Managers needing extra cleanup
	// (texture/image/sound) override this.
	void iResourceManager::FreeResource(iResourceBase* apResource)
	{
		RemoveResource(apResource);
		hplDelete(apResource);
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PUBLIC METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	iResourceBase* iResourceManager::GetResource(const tWString& asFullPath)
	{
		unsigned int lHash = cString::GetHashW(asFullPath);

		tResourceBaseMapIt it = m_mapResources.find(lHash);
		if(it == m_mapResources.end())return NULL;

        size_t lCount = m_mapResources.count(lHash);
		for(size_t i=0; i<lCount; ++i, ++it)
		{
			iResourceBase *pResource = it->second;

			if(pResource->GetFullPath() == asFullPath) return pResource;
		}

		return NULL;
	}

	//-----------------------------------------------------------------------

	cResourceBaseIterator iResourceManager::GetResourceBaseIterator()
	{
		return cResourceBaseIterator(&m_mapResources);
	}
	
	//-----------------------------------------------------------------------

	class cSortResources
	{
	public:
		bool operator()(iResourceBase* apResourceA, iResourceBase* apResourceB)
		{
			if(apResourceA->GetReferenceCount() != apResourceB->GetReferenceCount())
			{
				return apResourceA->GetReferenceCount() > apResourceB->GetReferenceCount();
			}
			
			return apResourceA->GetTime() > apResourceB->GetTime();
		}
	};

	//-----------------------------------------------------------------------

    void iResourceManager::DestroyUnused(int alMaxToKeep)
	{
		//Log("Start Num Of: %d\n",m_mapHandleResources.size());
		//Check if there are too many resources.
		if((int)m_mapResources.size() <= alMaxToKeep) return;

		//Add resources to a vector
		std::vector<iResourceBase*> vResources;
		vResources.reserve(m_mapResources.size());
		
		tResourceBaseMapIt it = m_mapResources.begin();
		for(;it != m_mapResources.end();++it)
		{
			vResources.push_back(it->second);
		}

		//Sort the sounds according to num of users and then time.
		std::sort(vResources.begin(), vResources.end(), cSortResources());
		
		//Log("-------------Num: %d-----------------\n",vResources.size());
		for(size_t i=alMaxToKeep; i<vResources.size(); ++i)
		{
			iResourceBase *pRes = vResources[i];
			//Log("%s count:%d time:%d\n",pRes->GetName().c_str(), 
			//							pRes->GetReferenceCount(), 
			//							pRes->GetTime());

			if(pRes->HasReferences()==false)
			{
				RemoveResource(pRes);
				hplDelete(pRes);
			}
		}
		//Log("--------------------------------------\n");
		//Log("End Num Of: %d\n",m_mapHandleResources.size());

	}
	
	//-----------------------------------------------------------------------
	
	// Reference counting lives in SharedResourceHandle now; this is the bridge for
	// callers still holding raw pointers. Drop one reference and free at zero.
	void iResourceManager::Destroy(iResourceBase* apResource)
	{
		if(apResource==NULL) return;

		apResource->DropReference();

		if(apResource->HasReferences()==false)
			FreeResource(apResource);
	}

	//-----------------------------------------------------------------------

	void iResourceManager::DestroyAll()
	{
		// Teardown: free every resource regardless of remaining references (any
		// surviving handles must already be gone by shutdown). The loop relies on
		// FreeResource unregistering the entry so the map shrinks each iteration.
		size_t lPrevSize = m_mapResources.size();
		while(m_mapResources.empty()==false)
		{
			FreeResource(m_mapResources.begin()->second);

			// Guard against a FreeResource override that fails to unregister, which
			// would otherwise spin forever on the same entry.
			if(m_mapResources.size() >= lPrevSize)
				break;
			lPrevSize = m_mapResources.size();
		}
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PROTECTED METHODS
	//////////////////////////////////////////////////////////////////////////
	
	//-----------------------------------------------------------------------

	void iResourceManager::BeginLoad(const tString& asFile)
	{
		mlTimeStart = cPlatform::GetApplicationTime();
		
		//Log("Begin resource: %s\n",asFile.c_str());

		mlTabCount++;
	}
	
	//-----------------------------------------------------------------------

	void iResourceManager::EndLoad()
	{
		mlTabCount--;
	}

	//-----------------------------------------------------------------------
	
	iResourceBase* iResourceManager::FindLoadedResource(const tString &asName, tWString &asFilePath,int *apEqualCount)
	{
		asFilePath = mpFileSearcher->GetFilePath(asName, apEqualCount);
		iResourceBase* pResource = GetResource(asFilePath);
		if(pResource!=NULL)
		{
			asFilePath = _W("");
		}

		return pResource;
	}

	//-----------------------------------------------------------------------
	
	tString iResourceManager::GetTabs()
	{
		tString sTabs ="";
		for(int i=0; i<mlTabCount; ++i) sTabs+="  ";
		return sTabs;
	}

	void iResourceManager::AddResource(iResourceBase* apResource, bool abLog, bool abAddToSet)
	{
		tString sName = cString::ToLowerCase(apResource->GetName());

		// Record the owning manager so a keep-alive minted from a raw pointer
		// (RetainResource) frees this resource through FreeResource, not delete.
		if(apResource) apResource->SetOwningManager(this);

		if(abAddToSet)
		{
			int lHash = cString::GetHashW(apResource->GetFullPath());
			m_mapResources.insert(tResourceBaseMap::value_type(lHash, apResource));
		}

		//Log("Adding %d, '%s' hash: %u\n",apResource,cString::To8Char(apResource->GetFullPath()).c_str(), lHash);
		
		if(abLog && iResourceBase::GetLogCreateAndDelete())
		{
			unsigned long lTime = cPlatform::GetApplicationTime() - mlTimeStart;
            Log("%sLoaded resource %s in %d ms\n",GetTabs().c_str(), apResource->GetName().c_str(),lTime);
			apResource->SetLogDestruction(true);
		}
		
		//Log("End resource: %s\n",apResource->GetName().c_str());
	}
	
	//-----------------------------------------------------------------------

	void iResourceManager::RemoveResource(iResourceBase* apResource)
	{
		if(apResource->HasReferences())
			Warning("Deleting resource '%s' that still has %d reference(s)\n",
					apResource->GetName().c_str(), apResource->GetReferenceCount());
		//Log("Removing resource name: '%s' path: '%s' ", apResource->GetName().c_str(), cString::To8Char(apResource->GetFullPath()).c_str());

		unsigned int lHash = cString::GetHashW(apResource->GetFullPath());

		tResourceBaseMapIt it = m_mapResources.find(lHash);
		if(it == m_mapResources.end())
		{
			//Log("%d was not removed! '%s' Hash: %u\n", apResource, cString::To8Char(apResource->GetFullPath()).c_str(),lHash);

			//Log("...not found!\n");
			return;
		}

		size_t lCount = m_mapResources.count(lHash);
		for(size_t i=0; i<lCount; ++i, ++it)
		{
			iResourceBase *pResource = it->second;

			if(pResource == apResource)
			{
				//Log("...done!\n");
				m_mapResources.erase(it);
				return;
			}
		}
 	}

	//-----------------------------------------------------------------------


}
