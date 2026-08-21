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

#ifndef HPLEDITOR_EDITOR_FILE_WATCHER_H
#define HPLEDITOR_EDITOR_FILE_WATCHER_H

#include "../common/StdAfx.h"

#include "system/Event.h"

using namespace hpl;

#include <map>
#include <set>

//----------------------------------------------------------------------

class iEditorWorld;

namespace hpl { class iFileWatcher; }

//----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// cWatchedFile
//	One on-disk file that the editor is watching, plus the set of scene
//	entities that depend on it. Path-keyed, so N entities sharing one file
//	collapse to a single watch entry.
//
class cWatchedFile
{
public:
	cWatchedFile() : mbPending(false), mfQuietAccum(0) {}

	tWString		msFullPath;			// resolved absolute path (key into mmapWatched)
	bool			mbPending;			// a change is being debounced
	float			mfQuietAccum;		// seconds since the last event for this file
	std::set<int>	msetEntityIDs;		// dependent wrapper IDs (world-scoped)
};

//----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// cEditorFileWatcher
//	Watches the files referenced by placed entities and, when one changes on
//	disk, announces the affected entities so the world can reload them live.
//	Detection is OS-native and event-driven: it watches the parent directories
//	of the registered files through cPlatform::CreateFileWatcher() (inotify /
//	ReadDirectoryChangesW / FSEvents) and receives FILE-level typed events; each
//	event's path is looked up directly in the watch registry. A short debounce
//	window coalesces mid-write / burst saves into a single reload. There is no
//	periodic full-file-list scan and no mtime polling.
//
//	Directory (not per-file) watching is deliberate: the editor's own save and
//	most tools write a temp file then rename over the target, replacing its
//	inode - a file-inode watch would miss that; a directory watch catches it
//	(as a create/rename event for the target's name).
//
//	Ownership is decoupled via hpl::Event: the watcher does NOT know the world,
//	it just Signals OnReloadEntities() and the world subscribes (same pattern as
//	cEngine::OnSDLEvent / cWindow::OnScreenSizeChanged).
//
class cEditorFileWatcher
{
public:
	cEditorFileWatcher();
	~cEditorFileWatcher();

	////////////////////////////////////////////////////////
	// Fired (on the main thread, from Poll) with the set of entity IDs whose
	// dependencies changed on disk and have settled. The world subscribes and
	// reloads them; other consumers may subscribe too.
	Event<const std::set<int>&>& OnReloadEntities() { return m_onReloadEntities; }

	////////////////////////////////////////////////////////
	// Registration (paths are resolved absolute paths)
	void RegisterEntity(int alEntityID, const tWString& asFullPath);
	void UnregisterEntity(int alEntityID);

	////////////////////////////////////////////////////////
	// Ticking
	void Poll(float afTimeStep);
	void Clear();

	////////////////////////////////////////////////////////
	// Settings
	void SetEnabled(bool abX)				{ mbEnabled = abX; }
	bool GetEnabled()						{ return mbEnabled; }
	void SetWatchDependencies(bool abX)		{ mbWatchDeps = abX; }
	bool GetWatchDependencies()				{ return mbWatchDeps; }
	// Stability window (seconds) a changed file must hold before it reloads,
	// so mid-write / burst saves coalesce into a single reload.
	void SetDebounce(float afX)				{ if(afX>=0) mfDebounce = afX; }
	float GetDebounce()						{ return mfDebounce; }
	// When set, logs every watched dir / drained event / debounce step under
	// the [FileWatch] prefix so it is visible what triggers a reload.
	void SetVerbose(bool abX)				{ mbVerbose = abX; }
	bool GetVerbose()						{ return mbVerbose; }

	////////////////////////////////////////////////////////
	// Read-only introspection (used by the MCP "list_associated_files" tool):
	// every watched file (absolute path) + the entities that depend on it.
	const std::map<tWString, cWatchedFile>& GetWatchedFiles() const { return mmapWatched; }

private:
	// Bump/drop the OS directory watch for the parent dir of a full file path.
	void AddDirRef(const tWString& asFullPath);
	void RemoveDirRef(const tWString& asFullPath);

	hpl::iFileWatcher*	mpOSWatcher;		// OS-native directory watcher (may be NULL)

	Event<const std::set<int>&>	m_onReloadEntities;	// announced when files settle

	std::map<tWString, cWatchedFile>	mmapWatched;		// key = full path
	std::map<int, std::set<tWString> >	mmapEntityFiles;	// reverse index for O(1) unregister

	std::map<tWString, int>				mmapWatchedDirs;	// watched dir -> #files under it
	std::set<tWString>					msetPending;		// paths currently in the debounce window

	float	mfDebounce;
	bool	mbEnabled;
	bool	mbWatchDeps;
	bool	mbVerbose;
};

//----------------------------------------------------------------------

#endif // HPLEDITOR_EDITOR_FILE_WATCHER_H
