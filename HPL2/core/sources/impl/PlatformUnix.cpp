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

#include <sys/stat.h>
#include <dirent.h>
#include <sys/param.h>
#include <fstream>

#if USE_SDL2
#include "SDL2/SDL.h"
#else
#include "SDL/SDL.h"
#endif

#ifdef __linux__
#if !SDL_VERSION_ATLEAST(2,0,0)
#include <FL/fl_ask.H>
#endif

#include <sys/types.h>
#endif
#include <unistd.h>
#include <sys/time.h>

#include <algorithm>

#include "system/FileWatcher.h"
#ifdef __linux__
#include <sys/inotify.h>
#endif
#include <errno.h>
#include <map>

namespace hpl {

	//////////////////////////////////////////////////////////////////////////
	// FILE HANDLING
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	unsigned long cPlatform::GetFileSize(const tWString& asFileName)
	{
		struct stat statbuf;
		if (stat(cString::To8Char(asFileName).c_str(), &statbuf) == -1) {
			return 0;
		};
		return statbuf.st_size;
	}
	
	//-----------------------------------------------------------------------

	bool cPlatform::CopyFileToBuffer(const tWString& asFileName, void *apBuffer, unsigned long alSize)
	{
		FILE *pFile = OpenFile(asFileName, _W("r"));
		if (pFile==NULL) return false;
		fread(apBuffer, sizeof(char), alSize, pFile);
		
		fclose(pFile);
		return true;
	}

	//-----------------------------------------------------------------------

	bool cPlatform::FileExists(const tWString& asFileName)
	{
		struct stat statbuf;
		return (lstat(cString::To8Char(asFileName).c_str(), &statbuf) == 0);
	}

	//-----------------------------------------------------------------------

	void cPlatform::RemoveFile(const tWString& asFilePath)
	{
		int ret = unlink(cString::To8Char(asFilePath).c_str());
	}

	//-----------------------------------------------------------------------
#define COPY_BUFFSIZE 8192

	bool cPlatform::CloneFile(	const tWString& asSrcFileName,const tWString& asDestFileName,
		bool abFailIfExists)
	{
		std::ifstream IN (cString::To8Char(asSrcFileName).c_str(), std::ios::binary);
		std::ofstream OUT (cString::To8Char(asDestFileName).c_str(), std::ios::binary);
		OUT << IN.rdbuf();
		OUT.flush();
		return true;
	}

	//-----------------------------------------------------------------------

	bool cPlatform::CreateFolder(const tWString& asPath)
	{
		return mkdir(cString::To8Char(asPath).c_str(),0755) == 0;
	}
	
	//-----------------------------------------------------------------------

	bool cPlatform::RemoveFolder(const tWString& asPath, bool abDeleteAllFiles, bool abDeleteAllSubFolders)
	{
		////////////////////
		// Remove any files in the directory
		if(abDeleteAllFiles)
		{
			tWStringList lstFiles;
            FindFilesInDir(lstFiles,asPath,_W("*"), true);
			for(tWStringListIt it = lstFiles.begin(); it != lstFiles.end(); ++it)
			{
				tWString sFilePath = cString::SetFilePathW(*it, asPath);
				RemoveFile(sFilePath);
			}
		}
		
		////////////////////
		// Remove any sub folders in the directory
		if(abDeleteAllSubFolders)
		{
			tWStringList lstFolders;
			FindFoldersInDir(lstFolders, asPath,true, false);
			for(tWStringListIt it = lstFolders.begin(); it != lstFolders.end(); ++it)
			{
				tWString sFolderPath = cString::SetFilePathW(*it, asPath);
				RemoveFolder(sFolderPath, abDeleteAllFiles,abDeleteAllSubFolders);
			}
		}
		
		if(rmdir(cString::To8Char(asPath).c_str())!=0)
		{
//			wchar_t sTempString[2048];
//			FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM,0,GetLastError(),0,sTempString,2048,NULL);
//			Error("Could not remove folder: '%s': %s",cString::To8Char(sPath).c_str(), cString::To8Char(sTempString).c_str());
			return false;
		}
		return true;
	}

	//-----------------------------------------------------------------------

	bool cPlatform::FolderExists(const tWString& asPath)
	{
		struct stat statbuf;
		if (stat(cString::To8Char(asPath).c_str(), &statbuf)!=0) {
            return false;
        } else {
            return S_ISDIR(statbuf.st_mode);
        }
	}

	//-----------------------------------------------------------------------

	tWString cPlatform::GetFullFilePath(const tWString& asFilePath)
	{
		char rpath[PATH_MAX];
		realpath(cString::To8Char(asFilePath).c_str(), rpath);
		tWString ret = cString::To16Char(tString(rpath)); 
		return ret;
	}
	
	//-----------------------------------------------------------------------
	
	FILE *cPlatform::OpenFile(const tWString& asFileName, const tWString asMode)
	{
		return fopen(cString::To8Char(asFileName).c_str(), cString::To8Char(asMode).c_str());
	}

	//-----------------------------------------------------------------------

	static cDate DateFromGMTime(struct tm* apClock)
	{
		cDate date;

		date.seconds = apClock->tm_sec;
		date.minutes = apClock->tm_min;
		date.hours = apClock->tm_hour;
		date.month_day = apClock->tm_mday;
		date.month = apClock->tm_mon;
		date.year = 1900 + apClock->tm_year;
		date.week_day = apClock->tm_wday;
		date.year_day = apClock->tm_yday;

		return date;
	}

	cDate cPlatform::FileModifiedDate(const tWString& asFilePath)
	{
		struct tm pClock;
		struct stat attrib;
		stat(cString::To8Char(asFilePath).c_str(), &attrib);
		
		gmtime_r(&(attrib.st_mtime), &pClock);	// Get the last modified time and put it into the time structure
		
		cDate date = DateFromGMTime(&pClock);
		
		return date;
	}

	//-----------------------------------------------------------------------

	cDate cPlatform::FileCreationDate(const tWString& asFilePath)
	{
		struct tm pClock;

		struct stat attrib;
		stat(cString::To8Char(asFilePath).c_str(), &attrib);

		gmtime_r(&(attrib.st_ctime), &pClock);	// Get the last modified time and put it into the time structure

		cDate date = DateFromGMTime(&pClock);

		return date;
	}

	//-----------------------------------------------------------------------
	static inline int patiMatch (const wchar_t *pattern, const wchar_t *string) {
		switch (pattern[0])
		{
			case _W('\0'):
				return !string[0];
				
			case _W('*') :
				return patiMatch(pattern+1, string) || (string[0] && patiMatch(pattern, string+1));
				
			case _W('?') :
				return string[0] && patiMatch(pattern+1, string+1);
				
			default  :
				return (towupper(pattern[0]) == towupper(string[0])) && patiMatch(pattern+1, string+1);
      }
	}
	void cPlatform::FindFilesInDir(tWStringList &alstStrings,const tWString& asDir, const tWString& asMask, bool abAddHidden)
	{
		// Empty dir would index asDir[-1] below (and opendir("") fails anyway).
		// MaterialEditor's dir handler can feed an empty path from its config.
		if(asDir.empty()) return;

		//Get the search string
		wchar_t sSpec[256];
		wchar_t end = asDir[asDir.size()-1];
		//The needed structs
		DIR *dirhandle;
		dirent *_entry;
		struct stat statbuff;
		tWString fileentry;
		
		if ((dirhandle = opendir(cString::To8Char(asDir).c_str()))==NULL) return;
		
		while ((_entry = readdir(dirhandle)) != NULL) {
			if (end==_W('/'))
				swprintf(sSpec,256,_W("%ls%s"),asDir.c_str(),_entry->d_name);
			else
				swprintf(sSpec,256,_W("%ls/%s"),asDir.c_str(),_entry->d_name);
			
			// skip unreadable
			if (stat(cString::To8Char(sSpec).c_str(),&statbuff) ==-1) continue;
			// skip directories
			if (S_ISDIR(statbuff.st_mode)) continue;
			
			fileentry.assign(cString::To16Char(_entry->d_name));
			
			if (!patiMatch(asMask.c_str(),fileentry.c_str())) continue;
			alstStrings.push_back(fileentry);
		}
		closedir(dirhandle);
		alstStrings.sort();
	}

	//-----------------------------------------------------------------------
	void cPlatform::FindFoldersInDir(tWStringList &alstStrings,const tWString& asDir, bool abAddHidden, bool abAddUpFolder)
	{
		//Get the search string
		char sSpec[256];
		tString sDir8 = cString::To8Char(asDir);
		char end = sDir8[sDir8.size()-1];
		
		if (end != '/') {
			sDir8 += "/";
		}
		
		//The needed structs
		DIR *dirhandle;
		dirent *_entry;
		struct stat statbuff;
		tWString fileentry;
		
		if ((dirhandle = opendir(cString::To8Char(asDir).c_str()))==NULL) return;
		
		while ((_entry = readdir(dirhandle)) != NULL) {
			snprintf(sSpec,256,"%s%s",sDir8.c_str(),_entry->d_name);
			
			// skip unreadable
			if (stat(sSpec,&statbuff) ==-1) continue;
			// skip non-directories
			if (!S_ISDIR(statbuff.st_mode)) continue;
			
			// add updir
			if (!abAddUpFolder && _entry->d_name[0] == '.' && _entry->d_name[1] == '.' && _entry->d_name[2] == '\0') continue;
			// add hidden
			if (!abAddHidden && _entry->d_name[0] == '.') continue;
			// Skip self
			if (_entry->d_name[0] == '.' && _entry->d_name[1]=='\0') continue;

			fileentry.assign(cString::To16Char(_entry->d_name));

			alstStrings.push_back(fileentry);
		}
		closedir(dirhandle);
		alstStrings.sort();
	}

	//-----------------------------------------------------------------------
#ifdef __linux__
    tString cPlatform::GetDataDir()
    {
			char buff[MAXPATHLEN];
			getcwd(buff, MAXPATHLEN);
			return tString(buff);
    }
#endif

	//-----------------------------------------------------------------------

	tWString cPlatform::GetWorkingDir()
	{
		char buff[MAXPATHLEN];
		getcwd(buff, MAXPATHLEN);
		return cString::To16Char(tString(buff));
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// DIALOG
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------
#ifdef __APPLE__
	void OSXAlertBox(eMsgBoxType eType, tString caption, tString message);
#endif

	void cPlatform::CreateMessageBoxBase(eMsgBoxType eType, const wchar_t* asCaption, const wchar_t* fmt, va_list ap)
	{
		wchar_t text[2048];

		if (fmt == NULL)
			return;	
		vswprintf(text, 2047, fmt, ap);

		tWString sMess = _W("");

		sMess += text;

#if defined(__APPLE__) && (HPL_MINIMAL || !SDL_VERSION_ATLEAST(2,0,0))
		OSXAlertBox(eType, cString::To8Char(asCaption), cString::To8Char(sMess));
#elif SDL_VERSION_ATLEAST(2,0,0)
		Uint32 type = SDL_MESSAGEBOX_WARNING;
		switch (eType) {
			case eMsgBoxType_Info: type = SDL_MESSAGEBOX_INFORMATION; break;
			case eMsgBoxType_Error: type = SDL_MESSAGEBOX_ERROR; break;
			case eMsgBoxType_Warning: type = SDL_MESSAGEBOX_WARNING; break;
		}
		tString caption = cString::To8Char(asCaption);
		tString message = cString::To8Char(sMess);
		SDL_MessageBoxButtonData buttons[] = {
			{
				SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT,
				1,
				"Dismiss"
			}
		};
		SDL_MessageBoxData data = {
			type,
			NULL,
			caption.c_str(),
			message.c_str(),
			1,
			buttons,
			NULL
		};
		int pressed = -1;
		SDL_ShowMessageBox(&data, &pressed);
#else
		// Linux/X11 implementation
		SDL_GrabMode cur = SDL_WM_GrabInput(SDL_GRAB_QUERY);
		if (cur == SDL_GRAB_ON) {
			SDL_WM_GrabInput(SDL_GRAB_OFF);
		}
		fl_alert("%s", cString::To8Char(asCaption + tWString(_W("\n\n")) + sMess).c_str());
		if (cur == SDL_GRAB_ON) {
			SDL_WM_GrabInput(SDL_GRAB_ON);
		}
#endif
	}


	
	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// SYSTEM STATS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	ePlatform cPlatform::GetPlatform()
	{
#if defined(__linux__)
		return ePlatform_Linux;
#elif defined(__APPLE__)
        return ePlatform_Mac;
#else
#error Platfom Unsupported
#endif
	}

	//-----------------------------------------------------------------------
#if defined(__linux__) && defined(__LP64__)
	tString cPlatform::msName = "Linux x86_64";
#elif defined(__linux__)
    tString cPlatform::msName = "Linux x86";
#elif defined(__APPLE__) && (defined(__PPC__) || defined(__ppc__))
    tString cPlatform::msName = "Mac OS X PowerPC";
#elif defined(__APPLE__) && defined(__LP64__)
	tString cPlatform::msName = "Mac OS X x86_64";
#elif defined(__APPLE__)
	tString cPlatform::msName = "Mac OS X x86";
#else
#error Platform Unsupported
#endif

	//-----------------------------------------------------------------------

	cDate cPlatform::GetDate()
	{
		time_t lTime;
		time(&lTime);

		struct tm* pClock;
		pClock = localtime(&lTime);

		return DateFromGMTime(pClock);		
	}

	//-----------------------------------------------------------------------

	tWString cPlatform::GetSystemSpecialPath(eSystemPath aPathType)
	{
		switch (aPathType)
		{
			case eSystemPath_Personal: {
				const char *home = getenv("HOME");
				tWString sDir = cString::To16Char(tString(home));
				if (cString::GetLastCharW(sDir) != _W("/")) {
					sDir += _W("/");
				}
				return sDir;
			}
			default:
				return _W("");
		}
	}

	//-----------------------------------------------------------------------

	unsigned long cPlatform::GetSystemAvailableDrives()
	{
		/* @THOMAS Mac and Linux do not have an equivilent of Drives everything is underone tree */
		return 0x0;
	}

	//////////////////////////////////////////////////////////////////////////
	// SYSTEM COMMANDS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------
	void OSXLaunchURL(tString url);
	void cPlatform::OpenBrowserWindow ( const tWString& asURL )
	{
#if defined(__APPLE__)
		OSXLaunchURL(cString::To8Char(asURL));
#else
		pid_t pID = fork();
		if (pID == 0) {// child
			execlp("xdg-open", "xdg-open", cString::To8Char(asURL).c_str(), (char *)0);
			exit(1);
		} else if (pID < 0) { // Failed
			Error("Could not Open URL %ls\n", asURL.c_str());
		} else { // Parent
			Log("Opened URL %ls\n", asURL.c_str());
		}
#endif
	}

    bool OSXOpenFile(tString path);
	bool cPlatform::OpenFileOnShell( const tWString& asPath )
	{
#if defined(__APPLE__)
        return OSXOpenFile(cString::To8Char(asPath));
#else
		pid_t pID = fork();
		if (pID == 0) {// child
			execlp("/bin/bash", "xdg-open", "xdg-open", cString::To8Char(asPath).c_str(), (char *)0);
			exit(1);
		} else if (pID < 0) { // Failed
			Error("Could not Open File %ls\n", asPath.c_str());
			return false;
		} else { // Parent
			Log("Opened File %ls\n", asPath.c_str());
			return true;
		}
#endif
	}

	bool OSXRunProgram(tString path, tString args);
	bool cPlatform::RunProgram( const tWString& asPath, const tWString& asParams )
	{
        bool ret = true;
#if defined(__APPLE__)
		ret = OSXRunProgram(cString::To8Char(asPath), cString::To8Char(asParams));
#else
		pid_t pID = fork();
		if (pID == 0) {// child
			tStringVec tvArgs;
			tString sSepp = " ";
			cString::GetStringVec( cString::To8Char(asParams), tvArgs, &sSepp);

			char **argv = (char **)hplMalloc(sizeof(char *) * (tvArgs.size()+2));
			if (argv) {
				tString sTemp = cString::To8Char(asPath);
				argv[0] = const_cast <char *> (sTemp.c_str());
				size_t ai=1;
				for (size_t i = 0; i < tvArgs.size(); ++i,++ai) {
					argv[ai] = const_cast <char*> (tvArgs[i].c_str());
				}
				argv[ai] = (char *)0;

				execvp(sTemp.c_str(), argv);
			}
			exit(1);
		} else if (pID < 0) { // Failed
			Error("Could not Launch program %ls\n", asPath.c_str());
            ret = false;
		} else { // Parent
			Log("Launched Program %ls\n", asPath.c_str());
		}
#endif
        return ret;
	}

	//-----------------------------------------------------------------------
	//-----------------------------------------------------------------------
	//-----------------------------------------------------------------------

#ifdef __linux__

	//////////////////////////////////////////////////////////////////////////
	// cFileWatcherInotify - Linux inotify backend for iFileWatcher.
	//	Watches directories (not individual files) so that atomic save-and-rename
	//	(write temp -> rename over target), which replaces the target's inode, is
	//	still seen (as IN_MOVED_TO on the target name). Reports file-level typed
	//	events; the path is rebuilt as watched-dir + event name.
	//////////////////////////////////////////////////////////////////////////

	class cFileWatcherInotify : public iFileWatcher
	{
	public:
		cFileWatcherInotify()
		{
			mlFd = inotify_init1(IN_NONBLOCK);
			if(mlFd < 0)
				Error("inotify_init1 failed (errno %d) - editor file watching disabled\n", errno);
		}

		~cFileWatcherInotify()
		{
			Clear();
			if(mlFd >= 0)
				close(mlFd);
		}

		bool AddDirectory(const tWString& asDir)
		{
			if(mlFd < 0)
				return false;
			if(mmapDirToWd.find(asDir) != mmapDirToWd.end())
				return true;	// already watched

			int lWd = inotify_add_watch(mlFd, cString::To8Char(asDir).c_str(),
				IN_CLOSE_WRITE | IN_MODIFY | IN_MOVED_TO | IN_MOVED_FROM | IN_CREATE | IN_DELETE |
				IN_MOVE_SELF | IN_DELETE_SELF | IN_ONLYDIR);
			if(lWd < 0)
			{
				Warning("inotify_add_watch failed for '%ls' (errno %d)\n", asDir.c_str(), errno);
				return false;
			}

			mmapDirToWd[asDir] = lWd;
			mmapWdToDir[lWd]   = asDir;
			return true;
		}

		void RemoveDirectory(const tWString& asDir)
		{
			std::map<tWString,int>::iterator it = mmapDirToWd.find(asDir);
			if(it == mmapDirToWd.end())
				return;

			if(mlFd >= 0)
				inotify_rm_watch(mlFd, it->second);
			mmapWdToDir.erase(it->second);
			mmapDirToWd.erase(it);
		}

		void PollEvents(std::vector<cFileWatchEvent>& avOut)
		{
			if(mlFd < 0)
				return;

			bool bOverflow = false;

			// Drain the inotify fd non-blocking. The buffer must be aligned for
			// struct inotify_event and large enough for at least one event.
			char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
			for(;;)
			{
				ssize_t lLen = read(mlFd, buf, sizeof(buf));
				if(lLen <= 0)	// EAGAIN once the queue is empty
					break;

				for(char* p = buf; p < buf + lLen; )
				{
					struct inotify_event* pEv = (struct inotify_event*)p;
					p += sizeof(struct inotify_event) + pEv->len;	// advance before any continue

					if(pEv->mask & IN_Q_OVERFLOW)
					{
						bOverflow = true;
						continue;
					}

					std::map<int,tWString>::iterator it = mmapWdToDir.find(pEv->wd);
					if(it == mmapWdToDir.end())
						continue;

					// The watched directory itself went away - drop the stale
					// watch. It gets re-added on the next RegisterEntity.
					if(pEv->mask & (IN_IGNORED | IN_MOVE_SELF | IN_DELETE_SELF))
					{
						mmapDirToWd.erase(it->second);
						mmapWdToDir.erase(it);
						continue;
					}

					// We watch non-recursively; ignore subdirectory changes and
					// events on the dir itself (no name).
					if((pEv->mask & IN_ISDIR) || pEv->len == 0)
						continue;

					// Rebuild the absolute path. The watched dir string keeps its
					// trailing separator (from cString::GetFilePathW), so join
					// WITHOUT adding one - this yields exactly the caller's key.
					cFileWatchEvent ev;
					ev.msPath = it->second + cString::To16Char(pEv->name);

					if(pEv->mask & (IN_CREATE | IN_MOVED_TO))
						ev.mAction = eFileWatchAction_Added;
					else if(pEv->mask & (IN_DELETE | IN_MOVED_FROM))
						ev.mAction = eFileWatchAction_Removed;
					else	// IN_CLOSE_WRITE | IN_MODIFY
						ev.mAction = eFileWatchAction_Modified;

					avOut.push_back(ev);
				}
			}

			// The kernel dropped events - emit a Modified for each watched dir path
			// so the consumer re-checks. Unknown paths are ignored by the consumer.
			if(bOverflow)
			{
				for(std::map<tWString,int>::iterator it=mmapDirToWd.begin(); it!=mmapDirToWd.end(); ++it)
				{
					cFileWatchEvent ev;
					ev.msPath = it->first;
					ev.mAction = eFileWatchAction_Modified;
					avOut.push_back(ev);
				}
			}
		}

		void Clear()
		{
			if(mlFd >= 0)
			{
				for(std::map<tWString,int>::iterator it=mmapDirToWd.begin(); it!=mmapDirToWd.end(); ++it)
					inotify_rm_watch(mlFd, it->second);
			}
			mmapDirToWd.clear();
			mmapWdToDir.clear();
		}

	private:
		int mlFd;
		std::map<tWString,int> mmapDirToWd;
		std::map<int,tWString> mmapWdToDir;
	};

	//-----------------------------------------------------------------------

	iFileWatcher* cPlatform::CreateFileWatcher()
	{
		return hplNew(cFileWatcherInotify, ());
	}

#else

	// Unix without inotify (e.g. BSD): no-op watcher - editor hot-reload is off.
	class cFileWatcherNull : public iFileWatcher
	{
	public:
		bool AddDirectory(const tWString&)					{ return false; }
		void RemoveDirectory(const tWString&)				{}
		void PollEvents(std::vector<cFileWatchEvent>&)		{}
		void Clear()										{}
	};

	iFileWatcher* cPlatform::CreateFileWatcher()
	{
		return hplNew(cFileWatcherNull, ());
	}

#endif // __linux__

}

