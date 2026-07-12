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

#include "EditorFileWatcher.h"

#include "system/FileWatcher.h"

//----------------------------------------------------------------------------

static const char* ActionName(eFileWatchAction aAction)
{
	switch(aAction)
	{
	case eFileWatchAction_Added:	return "added";
	case eFileWatchAction_Removed:	return "removed";
	case eFileWatchAction_Modified:
	default:						return "modified";
	}
}

//////////////////////////////////////////////////////////////////////////
// CONSTRUCTORS
//////////////////////////////////////////////////////////////////////////

//----------------------------------------------------------------------------

cEditorFileWatcher::cEditorFileWatcher()
{
	mfDebounce  = 0.15f;	// hold-stable window before applying a reload
	mbEnabled   = true;
	mbWatchDeps = true;		// full dependency chain by default
	mbVerbose   = false;

	mpOSWatcher = cPlatform::CreateFileWatcher();
	if(mpOSWatcher == NULL)
		Error("[FileWatch] could not create an OS file watcher - live reload disabled\n");
	else
		Log("[FileWatch] OS file watcher ready\n");
}

//----------------------------------------------------------------------------

cEditorFileWatcher::~cEditorFileWatcher()
{
	Clear();
	if(mpOSWatcher)
		hplDelete(mpOSWatcher);
}

//////////////////////////////////////////////////////////////////////////
// PRIVATE HELPERS
//////////////////////////////////////////////////////////////////////////

//----------------------------------------------------------------------------

void cEditorFileWatcher::AddDirRef(const tWString& asFullPath)
{
	tWString sDir = cString::GetFilePathW(asFullPath);
	if(sDir.empty())
		return;

	int& lRef = mmapWatchedDirs[sDir];
	if(lRef == 0)
	{
		// First watched file in this directory - start the OS watch. Even if the
		// watch fails we keep the refcount so bookkeeping stays consistent.
		if(mpOSWatcher && mpOSWatcher->AddDirectory(sDir) && mbVerbose)
			Log("[FileWatch] watching dir %ls\n", sDir.c_str());
	}
	++lRef;
}

//----------------------------------------------------------------------------

void cEditorFileWatcher::RemoveDirRef(const tWString& asFullPath)
{
	tWString sDir = cString::GetFilePathW(asFullPath);
	if(sDir.empty())
		return;

	std::map<tWString,int>::iterator itR = mmapWatchedDirs.find(sDir);
	if(itR != mmapWatchedDirs.end())
	{
		if(--itR->second <= 0)
		{
			// Last watched file left this directory - drop the OS watch.
			if(mpOSWatcher)
				mpOSWatcher->RemoveDirectory(sDir);
			if(mbVerbose)
				Log("[FileWatch] unwatching dir %ls\n", sDir.c_str());

			mmapWatchedDirs.erase(itR);
		}
	}
}

//////////////////////////////////////////////////////////////////////////
// PUBLIC METHODS
//////////////////////////////////////////////////////////////////////////

//----------------------------------------------------------------------------

void cEditorFileWatcher::RegisterEntity(int alEntityID, const tWString& asFullPath)
{
	if(asFullPath.empty())
		return;

	std::map<tWString, cWatchedFile>::iterator it = mmapWatched.find(asFullPath);
	if(it==mmapWatched.end())
	{
		// First entity to watch this file - create the entry and start watching
		// its parent directory.
		cWatchedFile file;
		file.msFullPath = asFullPath;
		file.msetEntityIDs.insert(alEntityID);
		mmapWatched[asFullPath] = file;

		AddDirRef(asFullPath);
	}
	else
	{
		it->second.msetEntityIDs.insert(alEntityID);
	}

	mmapEntityFiles[alEntityID].insert(asFullPath);
}

//----------------------------------------------------------------------------

void cEditorFileWatcher::UnregisterEntity(int alEntityID)
{
	std::map<int, std::set<tWString> >::iterator itEnt = mmapEntityFiles.find(alEntityID);
	if(itEnt==mmapEntityFiles.end())
		return;

	std::set<tWString>& setFiles = itEnt->second;
	for(std::set<tWString>::iterator itFile=setFiles.begin(); itFile!=setFiles.end(); ++itFile)
	{
		std::map<tWString, cWatchedFile>::iterator itWatched = mmapWatched.find(*itFile);
		if(itWatched==mmapWatched.end())
			continue;

		itWatched->second.msetEntityIDs.erase(alEntityID);
		if(itWatched->second.msetEntityIDs.empty())
		{
			// No entity depends on this file any more - stop watching it.
			RemoveDirRef(*itFile);
			msetPending.erase(*itFile);
			mmapWatched.erase(itWatched);
		}
	}

	mmapEntityFiles.erase(itEnt);
}

//----------------------------------------------------------------------------

void cEditorFileWatcher::Poll(float afTimeStep)
{
	if(mbEnabled==false || mpOSWatcher==NULL)
		return;

	// 1) Drain OS file-level events.
	std::vector<cFileWatchEvent> vEvents;
	mpOSWatcher->PollEvents(vEvents);

	// Idle fast path: nothing changed and nothing settling. No per-file scan.
	if(vEvents.empty() && msetPending.empty())
		return;

	// 2) Map each event to a watched file by exact path and (re)start its
	//    debounce window. Events for files we don't watch (other files sharing a
	//    watched dir) are ignored. The OS event is the trigger, so there is no
	//    mtime check - even same-second saves are caught.
	for(size_t i=0; i<vEvents.size(); ++i)
	{
		std::map<tWString, cWatchedFile>::iterator itW = mmapWatched.find(vEvents[i].msPath);
		if(itW==mmapWatched.end())
			continue;

		if(mbVerbose)
			Log("[FileWatch] %s %ls\n", ActionName(vEvents[i].mAction),
				cString::GetFileNameW(vEvents[i].msPath).c_str());

		itW->second.mbPending = true;
		itW->second.mfQuietAccum = 0;
		msetPending.insert(vEvents[i].msPath);
	}

	// 3) Advance the debounce over the pending set only; collect settled files.
	//    A fresh event above reset mfQuietAccum, so bursts coalesce.
	std::set<int> setReloadIDs;

	std::set<tWString>::iterator itP = msetPending.begin();
	while(itP != msetPending.end())
	{
		std::map<tWString, cWatchedFile>::iterator itW = mmapWatched.find(*itP);
		if(itW==mmapWatched.end())			// unregistered while pending
		{
			msetPending.erase(itP++);
			continue;
		}

		cWatchedFile& file = itW->second;
		file.mfQuietAccum += afTimeStep;
		if(file.mfQuietAccum < mfDebounce)
		{
			++itP;
			continue;
		}

		// Stable for the whole debounce window - apply the reload.
		file.mbPending = false;
		file.mfQuietAccum = 0;

		for(std::set<int>::iterator itID=file.msetEntityIDs.begin(); itID!=file.msetEntityIDs.end(); ++itID)
			setReloadIDs.insert(*itID);

		msetPending.erase(itP++);
	}

	// 4) Announce. Subscribers reload; the reload re-registers watches (mutating
	//    the maps we just iterated), so it must happen after the loop.
	if(setReloadIDs.empty()==false)
	{
		Log("[FileWatch] change detected, reloading %d entit%s\n",
			(int)setReloadIDs.size(), setReloadIDs.size()==1 ? "y" : "ies");
		m_onReloadEntities.Signal(setReloadIDs);
	}
}

//----------------------------------------------------------------------------

void cEditorFileWatcher::Clear()
{
	if(mpOSWatcher)
		mpOSWatcher->Clear();

	mmapWatched.clear();
	mmapEntityFiles.clear();
	mmapWatchedDirs.clear();
	msetPending.clear();
}

//----------------------------------------------------------------------------
