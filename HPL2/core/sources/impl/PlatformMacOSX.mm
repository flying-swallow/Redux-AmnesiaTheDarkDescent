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

//
//  PlatformMacOSX.m
//  Lux
//
//  Created by Edward Rudd on 6/27/09.
//  Copyright 2009 OutOfOrder.cc. All rights reserved.
//

#import <Cocoa/Cocoa.h>

#include "system/SystemTypes.h"
#include "system/Platform.h"
#include "system/String.h"
#include "system/LowLevelSystem.h"
#include "system/FileWatcher.h"

#include <CoreServices/CoreServices.h>
#include <set>
#include <vector>

namespace hpl {
	static NSString* getBundlePathForBundleID(NSString *bundleID)
	{
		OSStatus rc;
		CFURLRef url = NULL;
		NSString *path = nil;

		rc = LSFindApplicationForInfo(kLSUnknownCreator, (CFStringRef)bundleID, NULL, NULL, &url);

		if (rc == noErr) {
			path = [(NSURL *)url path];
		}

		if (url) CFRelease(url);

		return path;
	}

	static NSString* getGamePathForBundle(NSBundle *bundle)
	{
		NSString *dataPath = nil;

		NSString *startPath = [[bundle infoDictionary] objectForKey:@"StartFolder"];
		if ([startPath isEqualToString:@"PARENT"]) {
			dataPath = [bundle bundlePath];
			dataPath = [dataPath stringByDeletingLastPathComponent];
		} else { // default is resource
			dataPath = [bundle resourcePath];
		}

		return dataPath;
	}
	
	tString FindGameResources()
	{
		NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];

		NSFileManager *fm = [NSFileManager defaultManager];

		tString path = "";

		NSArray *bundleIDs = [NSArray arrayWithObjects:@"com.frictionalgames.Amnesia", @"com.frictionalgames.AmnesiaSteam", nil];
		for (NSString *bundleID in bundleIDs) {
			NSString *amnesiaBundle = amnesiaBundle = getBundlePathForBundleID(bundleID);

			NSString *gamePath = getGamePathForBundle([NSBundle bundleWithPath: amnesiaBundle]);
			if ([fm fileExistsAtPath:[gamePath stringByAppendingPathComponent:@"resources.cfg"]])
			{
				[fm changeCurrentDirectoryPath:gamePath];
				path = [gamePath UTF8String];
				break;
			}
		}

		if (path.empty()) {
			cPlatform::CreateMessageBox(eMsgBoxType_Error, _W("Failed to Find game resources"), _W("Could not locate the Amnesia game resources, is Amnesia: The Dark Descent installed?"));
		}

		[pool drain];
		return path;
	}

	tString cPlatform::GetDataDir()
	{
		tString sTemp;
		NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];

		NSBundle *bundle = [NSBundle mainBundle];
		NSString *dataPath = getGamePathForBundle(bundle);

		sTemp = [dataPath UTF8String];

		[pool drain];
		return sTemp;
	}

    void OSXAlertBox(eMsgBoxType eType, tString caption, tString message)
    {
        NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
        NSAlert *alert = [NSAlert alertWithMessageText:[NSString stringWithFormat:@"%s", caption.c_str()]
                                         defaultButton:nil // will default to localized OK
                                       alternateButton:nil
                                           otherButton:nil
                             informativeTextWithFormat:@"%s", message.c_str()];
        switch (eType) {
            case eMsgBoxType_Error:
                [alert setAlertStyle:NSCriticalAlertStyle];
                break;
            case eMsgBoxType_Warning:
                [alert setAlertStyle:NSWarningAlertStyle];
                break;
            case eMsgBoxType_Info:
            case eMsgBoxType_Default:
                [alert setAlertStyle:NSInformationalAlertStyle];
        }
        [alert runModal];
        [pool drain];
    }
    
    void OSXLaunchURL(tString url)
    {
        NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
        NSWorkspace *ws = [NSWorkspace sharedWorkspace];
        [ws openURL:[NSURL URLWithString:[NSString stringWithCString:url.c_str() encoding:NSUTF8StringEncoding]]];
        [pool drain];
    }
    
    bool OSXOpenFile(tString path)
    {
        bool ret = false;
        NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
        NSWorkspace *ws = [NSWorkspace sharedWorkspace];
        ret = [ws openFile:[NSString stringWithUTF8String: path.c_str()]] == YES;
        [pool drain];
        return ret;
    }
    
    bool OSXRunProgram(tString path, tString args)
    {
        bool ret = false;
        NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
        NSWorkspace *ws = [NSWorkspace sharedWorkspace];
        NSString *tPath = [NSString stringWithCString:path.c_str() encoding:NSUTF8StringEncoding];
        NSError *err;
        NSLog(@"Type of %@ is %@",tPath, [ws typeOfFile:[tPath stringByStandardizingPath] error:&err]);
        ret = [ws launchApplication:tPath] == YES;
        [pool drain];
        return ret;
    }

	//////////////////////////////////////////////////////////////////////////
	// cFileWatcherFSEvents - macOS FSEvents backend for iFileWatcher.
	//	An FSEventStream over the watched dirs (with kFSEventStreamCreateFlagFile-
	//	Events) gives file-level typed changes. To honour the no-thread model the
	//	stream is scheduled on the current run loop and pumped SYNCHRONOUSLY inside
	//	PollEvents (CFRunLoopRunInMode with a 0 timeout), so the callback runs on
	//	the calling thread and no locking is needed. NOTE: unbuilt/unverified in
	//	this setup (PlatformMacOSX.mm is not in the current premake build).
	//////////////////////////////////////////////////////////////////////////

	class cFileWatcherFSEvents : public iFileWatcher
	{
	public:
		cFileWatcherFSEvents() : mStream(NULL) {}
		~cFileWatcherFSEvents() { Clear(); }

		bool AddDirectory(const tWString& asDir)
		{
			if(msetDirs.insert(asDir).second)
				RebuildStream();	// stream watch paths are fixed at create time
			return true;
		}

		void RemoveDirectory(const tWString& asDir)
		{
			if(msetDirs.erase(asDir) > 0)
				RebuildStream();
		}

		void PollEvents(std::vector<cFileWatchEvent>& avOut)
		{
			if(mStream == NULL)
				return;

			// Fire any pending FSEvents callbacks on this thread without blocking.
			CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true);

			avOut.insert(avOut.end(), mvPending.begin(), mvPending.end());
			mvPending.clear();
		}

		void Clear()
		{
			DestroyStream();
			msetDirs.clear();
			mvPending.clear();
		}

	private:
		static void Callback(ConstFSEventStreamRef /*stream*/, void* apInfo, size_t alNumEvents,
			void* apEventPaths, const FSEventStreamEventFlags aFlags[], const FSEventStreamEventId[])
		{
			cFileWatcherFSEvents* pSelf = (cFileWatcherFSEvents*)apInfo;
			const char** pPaths = (const char**)apEventPaths;

			for(size_t i=0; i<alNumEvents; ++i)
			{
				tWString sPath = cString::To16Char(tString(pPaths[i]));
				FSEventStreamEventFlags f = aFlags[i];

				// A single coalesced event may carry several flags.
				if(f & (kFSEventStreamEventFlagItemCreated | kFSEventStreamEventFlagItemRenamed))
					pSelf->Push(sPath, eFileWatchAction_Added);
				if(f & kFSEventStreamEventFlagItemModified)
					pSelf->Push(sPath, eFileWatchAction_Modified);
				if(f & kFSEventStreamEventFlagItemRemoved)
					pSelf->Push(sPath, eFileWatchAction_Removed);
			}
		}

		void Push(const tWString& asPath, eFileWatchAction aAction)
		{
			cFileWatchEvent ev;
			ev.msPath = asPath;
			ev.mAction = aAction;
			mvPending.push_back(ev);
		}

		void RebuildStream()
		{
			DestroyStream();
			if(msetDirs.empty())
				return;

			CFMutableArrayRef paths = CFArrayCreateMutable(NULL, (CFIndex)msetDirs.size(), &kCFTypeArrayCallBacks);
			for(std::set<tWString>::iterator it=msetDirs.begin(); it!=msetDirs.end(); ++it)
			{
				CFStringRef s = CFStringCreateWithCString(NULL, cString::To8Char(*it).c_str(), kCFStringEncodingUTF8);
				CFArrayAppendValue(paths, s);
				CFRelease(s);
			}

			FSEventStreamContext ctx = { 0, this, NULL, NULL, NULL };
			mStream = FSEventStreamCreate(NULL, &Callback, &ctx, paths,
				kFSEventStreamEventIdSinceNow, 0.05,
				kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer);
			CFRelease(paths);

			if(mStream)
			{
				FSEventStreamScheduleWithRunLoop(mStream, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
				FSEventStreamStart(mStream);
			}
		}

		void DestroyStream()
		{
			if(mStream)
			{
				FSEventStreamStop(mStream);
				FSEventStreamInvalidate(mStream);
				FSEventStreamRelease(mStream);
				mStream = NULL;
			}
		}

		FSEventStreamRef				mStream;
		std::set<tWString>				msetDirs;
		std::vector<cFileWatchEvent>	mvPending;
	};

	iFileWatcher* cPlatform::CreateFileWatcher()
	{
		return hplNew(cFileWatcherFSEvents, ());
	}

}
