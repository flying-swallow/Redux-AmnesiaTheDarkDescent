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

#include "../common/StdAfx.h"
using namespace hpl;

#include "ModelEditor.h"
#include "BuildID_ModelEditor.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

int hplMain(const tString& asCommandLine)
{
	//cMemoryManager::SetLogCreation(true);

	cModelEditor* pEditor = hplNew(cModelEditor, ());

	////////////////////////////////////////////////////////////////////
	// Optional launch args: "<ent-file> [--mcp-port N] [--mcp-token T]".
	// The LevelEditor's "Edit in Model Editor" button passes these so we open
	// the file on startup and can push live-reload notifications back on save.
	{
		tStringVec vArgs;
		tString sSep = " ";
		cString::GetStringVec(asCommandLine, vArgs, &sSep);

		tString sFile;
		int lPort = 0;
		tString sToken;
		for(size_t i=0; i<vArgs.size(); ++i)
		{
			const tString& sArg = vArgs[i];
			if(sArg.empty())
				continue;
			if(sArg=="--mcp-port" && i+1<vArgs.size())
				lPort = cString::ToInt(vArgs[++i].c_str(), 0);
			else if(sArg=="--mcp-token" && i+1<vArgs.size())
				sToken = vArgs[++i];
			else if(sArg.size()>=2 && sArg[0]=='-' && sArg[1]=='-')
				continue; // unknown flag, ignore
			else if(sFile.empty())
				sFile = sArg; // first bare token = file to open
		}

		if(sFile.empty()==false)
			pEditor->SetStartupFile(sFile);
		if(lPort>0)
			pEditor->SetReloadNotifyEndpoint(lPort, sToken);
	}

	pEditor->Init(NULL, "ModelEditor", GetBuildID_ModelEditor());

	pEditor->OpenStartupFile();

    pEditor->Run();

	hplDelete(pEditor);

	cMemoryManager::LogResults();

	return 0;
}
