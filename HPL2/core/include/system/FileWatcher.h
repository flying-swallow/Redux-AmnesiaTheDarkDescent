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

#ifndef HPL_FILE_WATCHER_H
#define HPL_FILE_WATCHER_H

#include "system/SystemTypes.h"

#include <vector>

namespace hpl {

	//////////////////////////////////////////////////////////////////////////
	// cFileWatchEvent
	//	One file-level change reported by an iFileWatcher backend. msPath is the
	//	absolute path of the changed file, reconstructed as (watched dir + name)
	//	so it matches the exact key the caller registered.
	//
	enum eFileWatchAction
	{
		eFileWatchAction_Added,
		eFileWatchAction_Modified,
		eFileWatchAction_Removed,
	};

	struct cFileWatchEvent
	{
		tWString			msPath;
		eFileWatchAction	mAction;
	};

	//////////////////////////////////////////////////////////////////////////
	// iFileWatcher
	//	OS-native directory watcher. Concrete backends: inotify (Linux),
	//	ReadDirectoryChangesW (Windows), FSEvents (macOS); create one with
	//	cPlatform::CreateFileWatcher(). Watched directories are non-recursive; the
	//	backend reports FILE-level typed changes (added/modified/removed) with the
	//	absolute path. Drained non-blocking from the caller's tick - no background
	//	thread, no locks.
	//
	class iFileWatcher
	{
	public:
		virtual ~iFileWatcher() {}

		// Begin/stop watching a directory (non-recursive). AddDirectory is
		// idempotent per path and returns false if the dir could not be watched.
		virtual bool AddDirectory(const tWString& asDir) = 0;
		virtual void RemoveDirectory(const tWString& asDir) = 0;

		// Non-blocking. Appends the file-level changes seen since the last call.
		// Returns immediately when idle.
		virtual void PollEvents(std::vector<cFileWatchEvent>& avOut) = 0;

		// Drop every watch.
		virtual void Clear() = 0;
	};

};

#endif // HPL_FILE_WATCHER_H
