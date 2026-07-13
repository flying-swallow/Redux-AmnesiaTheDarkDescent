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

#include "PrefabManager.h"

#include "EditorBaseClasses.h"

#include <tinyxml2.h>
#include "resources/XmlHelper.h"

//----------------------------------------------------------------------------

// Prefab identity: the engine resolves entity files by bare filename, so that
// IS the prefab's identity — full paths and bare names land on the same
// resource. Doubles as the resource's keying "full path" in the manager map.
static tString PrefabKey(const tString& asEntFile)
{
	return cString::ToLowerCase(cString::GetFileName(asEntFile));
}

// mkdir -p for a file's parent folders (kept local, mirroring the copy in
// LevelEditor.cpp / the MCP commands TU — the duplication keeps the TUs
// independent and is already an accepted pattern in this codebase).
static void CreatePrefabParentFolders(const tWString& asFilePath)
{
	tWString sDir = cString::GetFilePathW(asFilePath);
	tWString sSep = _W("/\\");
	tWString sAccum = (sDir.empty()==false && sDir[0]==_W('/')) ? _W("/") : _W("");
	tWStringVec vSteps;
	cString::GetStringVecW(sDir, vSteps, &sSep);
	for(size_t i=0;i<vSteps.size();++i)
	{
		sAccum += vSteps[i] + _W("/");
		if(cPlatform::FolderExists(sAccum)==false)
			cPlatform::CreateFolder(sAccum);
	}
}

//////////////////////////////////////////////////////////////////////////////
// PREFAB RESOURCE
//////////////////////////////////////////////////////////////////////////////

//----------------------------------------------------------------------------

cPrefabResource::~cPrefabResource()
{
	if(mpDoc)
		delete mpDoc;
}

//////////////////////////////////////////////////////////////////////////////
// CONSTRUCTORS
//////////////////////////////////////////////////////////////////////////////

//----------------------------------------------------------------------------

cPrefabManager::cPrefabManager(iEditorBase* apEditor)
	: iResourceManager(apEditor->GetEngine()->GetResources()->GetFileSearcher(),
					   apEditor->GetEngine()->GetResources()->GetLowLevel(),
					   apEditor->GetEngine()->GetResources()->GetLowLevelSystem())
{
	mpEditor = apEditor;
}

//----------------------------------------------------------------------------

cPrefabManager::~cPrefabManager()
{
	// Release the pending pins while the manager is still intact — each release
	// frees its now-unreferenced resource through the normal FreeResource path.
	// (The world, and with it every wrapper-held handle, is destroyed before the
	// manager — see ~iEditorBase — so DestroyAll should find an empty map; it
	// stays as the backstop for anything left behind.)
	mmapPendingPins.clear();
	DestroyAll();
}

//////////////////////////////////////////////////////////////////////////////
// PUBLIC METHODS
//////////////////////////////////////////////////////////////////////////////

//----------------------------------------------------------------------------

SharedResourceHandle<cPrefabResource> cPrefabManager::CreatePrefab(const tString& asEntFile)
{
	tString sKey = PrefabKey(asEntFile);
	if(sKey.empty())
		return {};

	cPrefabResource* pRes = GetPrefab(sKey);
	if(pRes==NULL)
	{
		pRes = hplNew(cPrefabResource,(sKey));
		AddResource(pRes, false);
	}

	// Keep the best-known FULL path for disk flushes: a path-bearing name always
	// wins over a previously stored bare filename; a bare name only fills a blank.
	if(cString::GetFileName(asEntFile)!=asEntFile || pRes->msDiskPath.empty())
		pRes->msDiskPath = asEntFile;

	return AcquireResource<cPrefabResource>(this, pRes);
}

//----------------------------------------------------------------------------

tinyxml2::XMLElement* cPrefabManager::GetPendingRoot(const tString& asEntFile)
{
	cPrefabResource* pRes = GetPrefab(asEntFile);
	if(pRes==NULL || pRes->mpDoc==NULL)
		return NULL;
	return pRes->mpDoc->RootElement();
}

//----------------------------------------------------------------------------

tinyxml2::XMLElement* cPrefabManager::EnsureDoc(const tString& asEntFile)
{
	// Caller pins the resource first (CreatePrefab); if it somehow does not exist,
	// there is nothing to materialize into.
	cPrefabResource* pRes = GetPrefab(asEntFile);
	if(pRes==NULL)
		return NULL;

	// A pending in-memory definition (scope-session edit or MCP update_prefab) wins,
	// dirty state untouched.
	if(pRes->mpDoc)
		return pRes->mpDoc->RootElement();

	// Materialize the on-disk definition into a resource-owned standalone doc. The
	// doc cResources hands back is tracked in its own list (freed via
	// DestroyXmlDocument), so DeepCopy into a fresh doc the resource can own and
	// delete directly, then release the loaded one. CLEAN (not dirty): bringing disk
	// into memory is not an edit and must not flush back on map save.
	cResources* pResources = mpEditor->GetEngine()->GetResources();
	tinyxml2::XMLElement* pDiskRoot = pResources->LoadXmlDocument(asEntFile);
	if(pDiskRoot==NULL)
		return NULL;

	tinyxml2::XMLDocument* pOwned = new tinyxml2::XMLDocument();
	pDiskRoot->GetDocument()->DeepCopy(pOwned);
	pResources->DestroyXmlDocument(pDiskRoot);

	pRes->mpDoc   = pOwned;			// resource owns it; freed in its dtor / SetPendingDoc
	pRes->mbDirty = false;
	return pOwned->RootElement();
}

//----------------------------------------------------------------------------

void cPrefabManager::SetPendingDoc(const tString& asEntFile, tinyxml2::XMLDocument* apDoc, bool abBroadcast)
{
	SharedResourceHandle<cPrefabResource> hRes = CreatePrefab(asEntFile);
	if(!hRes)
		return;
	cPrefabResource* pRes = hRes.Get();

	bool bHadDoc = (pRes->mpDoc!=NULL);
	if(pRes->mpDoc && pRes->mpDoc!=apDoc)
		delete pRes->mpDoc;			// free the definition we are replacing
	pRes->mpDoc   = apDoc;			// take ownership
	pRes->mbDirty = true;

	// Pin the resource while it carries a pending doc, so an instance-less
	// definition survives transient handles dropping to zero. (Assigning the
	// same resource over an existing pin is a no-op in SharedResourceHandle.)
	tString sKey = PrefabKey(asEntFile);
	mmapPendingPins[sKey] = hRes;

	// Notify this prefab's subscribers (each placed instance) so they rebuild from
	// the new definition. pRes is kept alive by hRes across the Signal.
	if(abBroadcast)
		pRes->OnModified().Signal(bHadDoc ? ePrefabEvent_Modified : ePrefabEvent_Added);
}

//----------------------------------------------------------------------------

void cPrefabManager::OnExternalFileChange(const tString& asEntFile, bool abBroadcast)
{
	cPrefabResource* pRes = GetPrefab(asEntFile);
	if(pRes==NULL || pRes->mpDoc==NULL)
		return;	// nothing shadows the disk file

	if(pRes->mbDirty)
	{
		// Unsaved MCP edit AND an external disk edit: keep the in-memory edit
		// (it is the only copy of that work) but make the conflict visible.
		Warning("Prefab '%s' has unsaved in-memory edits AND changed on disk — keeping the "
				"in-memory edit. update_prefab persist:true overwrites the disk version.\n",
				pRes->GetName().c_str());
		return;
	}

	// Persisted (or never-edited) doc: the disk version is now the truth —
	// drop the shadow so reloads re-read the file.
	delete pRes->mpDoc;
	pRes->mpDoc = NULL;

	// Notify subscribers (each placed instance) BEFORE releasing the pin: with the
	// shadow gone they re-read disk, and any instance holding a reference keeps pRes
	// alive across the Signal. Firing after the pin erase would touch a freed pRes.
	if(abBroadcast)
		pRes->OnModified().Signal(ePrefabEvent_Removed);

	// Releasing the pin may free the resource if no placed instance holds a
	// reference — pRes must not be touched past this point.
	tString sKey = PrefabKey(asEntFile);
	mmapPendingPins.erase(sKey);
	pRes = NULL;
}

//----------------------------------------------------------------------------

int cPrefabManager::Flush()
{
	int lWritten = 0;
	for(tResourceBaseMapIt it=m_mapResources.begin(); it!=m_mapResources.end(); ++it)
	{
		cPrefabResource* pRes = static_cast<cPrefabResource*>(it->second);
		if(pRes->mbDirty==false || pRes->mpDoc==NULL)
			continue;
		if(WritePrefabToDisk(*pRes))			// clears mbDirty on success
			++lWritten;
	}
	return lWritten;
}

//----------------------------------------------------------------------------

bool cPrefabManager::FlushOne(const tString& asEntFile)
{
	cPrefabResource* pRes = GetPrefab(asEntFile);
	if(pRes==NULL || pRes->mbDirty==false || pRes->mpDoc==NULL)
		return false;
	return WritePrefabToDisk(*pRes);
}

//----------------------------------------------------------------------------

int cPrefabManager::DirtyCount()
{
	int lCount = 0;
	for(tResourceBaseMapIt it=m_mapResources.begin(); it!=m_mapResources.end(); ++it)
	{
		cPrefabResource* pRes = static_cast<cPrefabResource*>(it->second);
		if(pRes->mbDirty && pRes->mpDoc)
			++lCount;
	}
	return lCount;
}

//----------------------------------------------------------------------------

void cPrefabManager::DiscardAllPending()
{
	// No broadcast by design: this only runs while the map/world is being torn
	// down or replaced (new/load/reset) — reloading entities mid-teardown would
	// be wasteful at best. Docs are dropped first; releasing the pins afterwards
	// frees any resource with no instance references left.
	for(tResourceBaseMapIt it=m_mapResources.begin(); it!=m_mapResources.end(); ++it)
	{
		cPrefabResource* pRes = static_cast<cPrefabResource*>(it->second);
		if(pRes->mpDoc)
		{
			delete pRes->mpDoc;
			pRes->mpDoc = NULL;
		}
		pRes->mbDirty = false;
	}
	mmapPendingPins.clear();
}

//----------------------------------------------------------------------------

void cPrefabManager::ListPrefabs(std::vector<cPrefabInfo>& avOut)
{
	for(tResourceBaseMapIt it=m_mapResources.begin(); it!=m_mapResources.end(); ++it)
	{
		cPrefabResource* pRes = static_cast<cPrefabResource*>(it->second);

		// Wrapper handles and the manager's own pending pin are the only
		// reference holders, so instances = refcount minus the pin.
		bool bPinned = mmapPendingPins.find(pRes->GetName())!=mmapPendingPins.end();
		int lInstances = (int)pRes->GetReferenceCount() - (bPinned ? 1 : 0);

		cPrefabInfo info;
		info.msEntFile   = pRes->msDiskPath.empty() ? pRes->GetName() : pRes->msDiskPath;
		info.mlInstances = lInstances;
		info.mbPending   = (pRes->mpDoc!=NULL);
		info.mbDirty     = pRes->mbDirty;
		avOut.push_back(info);
	}
}

//////////////////////////////////////////////////////////////////////////////
// PRIVATE METHODS
//////////////////////////////////////////////////////////////////////////////

//----------------------------------------------------------------------------

cPrefabResource* cPrefabManager::GetPrefab(const tString& asEntFile)
{
	// Straight hash lookup by the bare-filename key. NOT FindLoadedResource:
	// that resolves through FileSearcher paths, which a pathless pending
	// definition does not have.
	return static_cast<cPrefabResource*>(GetResource(cString::To16Char(PrefabKey(asEntFile))));
}

//----------------------------------------------------------------------------

bool cPrefabManager::WritePrefabToDisk(cPrefabResource& aRes)
{
	if(aRes.mpDoc==NULL)
		return false;

	// If only a bare filename was ever seen, resolve the full path through the
	// resource index — an unresolvable, never-written prefab cannot be saved.
	tString sPath = aRes.msDiskPath.empty() ? aRes.GetName() : aRes.msDiskPath;
	if(cString::GetFileName(sPath)==sPath)
	{
		tWString wFull = mpFileSearcher->GetFilePath(sPath);
		if(wFull.empty())
		{
			Error("Cannot write prefab '%s': no full path known and the name does not "
				  "resolve in the resource index\n", sPath.c_str());
			return false;
		}
		sPath = cString::To8Char(wFull);
		aRes.msDiskPath = sPath;
	}

	tWString wPath = cString::To16Char(sPath);
	CreatePrefabParentFolders(wPath);
	if(hpl::SaveXmlFile(*aRes.mpDoc, wPath)==false)
	{
		Error("Could not write prefab file '%s'\n", sPath.c_str());
		return false;						// stays dirty for the next save
	}

	// Re-index the written file's directory so it resolves in the FileSearcher
	// (mirrors cLevelEditor::RefreshResourceIndex's single-dir branch).
	tWString wDir = cString::GetFilePathW(wPath);
	if(cPlatform::FolderExists(wDir))
		mpEditor->GetEngine()->GetResources()->AddResourceDir(wDir, true);

	aRes.mbDirty = false;
	return true;
}

//----------------------------------------------------------------------------
