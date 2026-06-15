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

#include "EditorIndex.h"

#include "EditorBaseClasses.h"
#include "EditorThumbnailBuilder.h"

#include <tinyxml2.h>
#include "resources/XmlHelper.h"

#include <algorithm>

//-------------------------------------------------------------------

static tString gsIndexVersion = "3";

//-------------------------------------------------------------------

/////////////////////////////////////////////////////////////////////
// INDEX CLASS
/////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------

iEditorObjectIndex::iEditorObjectIndex(iEditorBase* apEditor, 
									   const tWString& asBaseFolder, 
									   const tWString& asIndexExt, 
									   const tString& asIndexTypeName, 
									   const tString& asEntryTypeName,
									   bool abCreateSubDirs)
{
	mpEditor = apEditor;
	msBaseFolderFullPath = cString::ReplaceCharToW(asBaseFolder, _W("\\"), _W("/"));
	msIndexExt = asIndexExt;
	msIndexTypeName = asIndexTypeName;
	msEntryTypeName = asEntryTypeName;

	mpRootDir = NULL;
	
	int lLastSlashPos = cString::GetLastStringPosW(msBaseFolderFullPath, _W("/"));
	if(lLastSlashPos==msBaseFolderFullPath.size()-1)
	{
		msBaseFolderFullPath = cString::SubW(msBaseFolderFullPath, 0, lLastSlashPos);

		lLastSlashPos = cString::GetLastStringPosW(msBaseFolderFullPath, _W("/"));
	}

	msBaseFolder = cString::SubW(msBaseFolderFullPath, lLastSlashPos+1);
	
	msFilename = mpEditor->GetTempDir() + msBaseFolder + _W(".") + msIndexExt;

	mbUpdated = false;
	mbRefreshThumbnails = false;

	mbCreateSubCategories = abCreateSubDirs;
}

//-------------------------------------------------------------------

iEditorObjectIndex::~iEditorObjectIndex()
{
	if(mpRootDir)
		hplDelete(mpRootDir);
}

//-------------------------------------------------------------------

void iEditorObjectIndex::Create()
{
	if(mpRootDir)
		hplDelete(mpRootDir);

	mpRootDir = CreateDir();

	Refresh();
}

//-------------------------------------------------------------------

bool iEditorObjectIndex::Refresh()
{
	mpEditor->ClearTempEntities();
	mpEditor->GetThumbnailBuilder()->PreBuild();

	tinyxml2::XMLDocument xmlDoc;
	if(hpl::LoadXmlFile(xmlDoc, msFilename) && xmlDoc.RootElement()!=NULL)
	{
		tinyxml2::XMLElement* pDoc = xmlDoc.RootElement();
		if(GetAttributeString(pDoc, "Version")!=gsIndexVersion)
		{
			mbUpdated = true;
			if(mpRootDir)
				mpRootDir->CreateFromDir(NULL, _W(""), mbCreateSubCategories);
		}
		else
		{
			tinyxml2::XMLElement* pXmlRootDir = pDoc->FirstChildElement("RootDir");

			if(mpRootDir && pXmlRootDir)
				mpRootDir->Refresh(pXmlRootDir, mbCreateSubCategories);
		}
	}
	else
	{
		mbUpdated = true;
		if(mpRootDir)
			mpRootDir->CreateFromDir(NULL, _W(""), mbCreateSubCategories);
	}

	if(mbUpdated)
	{
		mbUpdated=false;
		Save(&xmlDoc);
	}

	/////////////////////////
	// Reload thumbnail directory
	if(mbRefreshThumbnails)
	{
		mbRefreshThumbnails = false;
		mpEditor->GetEngine()->GetResources()->AddResourceDir(mpEditor->GetThumbnailDir(), false);
		
		mpEditor->GetThumbnailBuilder()->CleanUp();
	}

	mpEditor->GetThumbnailBuilder()->PostBuild();


	return true;
}

//-------------------------------------------------------------------

void iEditorObjectIndex::AddFilesInDir(tWStringList& asFileList, const tWString& asFolder, const tWString& asLocalPath, bool abAddFilesInSubDirs)
{
	/////////////////////////////////////////////////////////////
	// Scan given dir for files using the filters 
	tWStringList lstFiles;
	for(int i=0;i<(int)mvFileFilters.size();++i)
	{
		tWString sFilter = mvFileFilters[i];
		cPlatform::FindFilesInDir(lstFiles, asFolder, mvFileFilters[i]); 
	}
	lstFiles.sort();

	/////////////////////////////////////////////////////////////
	// Add to file list
	tWStringListIt it = lstFiles.begin();
	for(;it!=lstFiles.end();++it)
	{
		tWString sFile = *it;
		asFileList.push_back(sFile);
	}
}

//-------------------------------------------------------------------

void iEditorObjectIndex::Save(tinyxml2::XMLDocument* apDoc)
{
	apDoc->Clear();
	tinyxml2::XMLElement* pRoot = apDoc->NewElement(msIndexTypeName.c_str());
	apDoc->InsertEndChild(pRoot);
	SetAttributeString(pRoot, "Version", gsIndexVersion);

	if(mpRootDir)
	{
		tinyxml2::XMLElement* pElement = pRoot->GetDocument()->NewElement("RootDir");
		pRoot->InsertEndChild(pElement);

		mpRootDir->Save(pElement, mbCreateSubCategories);
	}

	hpl::SaveXmlFile(*apDoc, msFilename);
}

//-------------------------------------------------------------------

/////////////////////////////////////////////////////////////////////
// INDEX DIR
/////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------

iEditorObjectIndexDir::iEditorObjectIndexDir(iEditorObjectIndex* apIndex, iEditorObjectIndexDir* apParent)
{
	mpIndex = apIndex;
	mpParentDir = apParent;

	mbExpanded = true;
}

//----------------------------------------------------------

iEditorObjectIndexDir::~iEditorObjectIndexDir()
{
	Clear();
}

//----------------------------------------------------------

void iEditorObjectIndexDir::Clear()
{
	STLMapDeleteAll(mmapEntries);
	STLMapDeleteAll(mmapSubDirs);
}

bool iEditorObjectIndexDir::CreateFromDir(iEditorObjectIndexDir* apParentDir, const tWString& asRelPath, bool abCreateSubDirs)
{
	if(cString::GetFirstStringPosW(asRelPath, _W("."))==0)
		return false;

	mpParentDir = apParentDir;
	Clear();	

	msDirName = asRelPath;

    tWStringList lstFiles;
	const tWStringVec& vFilters = mpIndex->GetFileFilters();

	tWString sDir = GetFullPath();
	mpIndex->GetEditor()->GetEngine()->GetResources()->AddResourceDir(sDir, false);

	mpIndex->AddFilesInDir(lstFiles, sDir, _W(""), false);

	tWStringListIt itFiles = lstFiles.begin();
	for(;itFiles!=lstFiles.end();++itFiles)
	{
		iEditorObjectIndexEntry* pEntry = CreateEntry();
		if(pEntry->CreateFromFile(*itFiles))
			mmapEntries.insert(std::pair<tString, iEditorObjectIndexEntry*>(pEntry->GetEntryName(), pEntry));
		else
			hplDelete(pEntry);
	}

	if(abCreateSubDirs)
	{
		tWStringList lstDirs;

		cPlatform::FindFoldersInDir(lstDirs, sDir, false, false);

		tWStringListIt itDirs = lstDirs.begin();
		for(;itDirs!=lstDirs.end();++itDirs)
		{
			tWString sRelPath = *itDirs;
			iEditorObjectIndexDir* pDir = mpIndex->CreateDir();
			if(pDir->CreateFromDir(this, sRelPath, true))
				mmapSubDirs.insert(std::pair<tWString,iEditorObjectIndexDir*>(sRelPath,pDir));
			else
				hplDelete(pDir);
		}
	}

	return true;
}

//-------------------------------------------------------------------

const tWString& iEditorObjectIndexDir::GetRelPath()
{
	if(msRelPath==_W(""))
	{
		tWString sTemp = (mpParentDir?mpParentDir->GetRelPath():_W(""));

		msRelPath = (sTemp==_W("")?sTemp:sTemp+_W("/")) + msDirName;
	}

	return msRelPath;
}

//-------------------------------------------------------------------

const tWString& iEditorObjectIndexDir::GetFullPath()
{
	if(msFullPath==_W(""))
		msFullPath = mpIndex->GetBaseFolderFullPath() + _W("/") + GetRelPath();

	return msFullPath;
}

//-------------------------------------------------------------------

bool iEditorObjectIndexDir::Refresh(tinyxml2::XMLElement* apElement, bool abAddSubDirs)
{
	bool bMapEmpty = mmapEntries.empty();
	bool bUpdated = false;

	Clear();
	
	cResources* pRes = mpIndex->GetEditor()->GetEngine()->GetResources();
	tWStringList lstFiles;
	tWStringList lstDirs;

	msDirName = cString::To16Char(GetAttributeString(apElement, "RelPath"));

	const tWString& sFullPath = GetFullPath();

	pRes->AddResourceDir(sFullPath, false);

	mpIndex->AddFilesInDir(lstFiles, sFullPath, _W(""), false);

	tinyxml2::XMLElement* pEntries = apElement->FirstChildElement("Entries");
	tinyxml2::XMLElement* pDirs = apElement->FirstChildElement("SubDirs");

	if(pEntries)
	{
		for(tinyxml2::XMLElement* pXmlEntry = pEntries->FirstChildElement(); pXmlEntry != NULL; pXmlEntry = pXmlEntry->NextSiblingElement())
		{
			iEditorObjectIndexEntry* pEntry = CreateEntry();

			if(pEntry->CreateFromXmlElement(pXmlEntry))
			{
				tString sEntryName = pEntry->GetEntryName();
				bool bEntryIsUpdated = pEntry->IsUpdated();

				if(bEntryIsUpdated)
					bUpdated = true;

				if(bMapEmpty)
					mmapEntries[sEntryName] = pEntry;
				else
				{
					tIndexEntryMapIt itFoundEntry = mmapEntries.find(sEntryName);
					if(itFoundEntry != mmapEntries.end())
					{
						if(bEntryIsUpdated)
						{
							hplDelete(itFoundEntry->second);
							itFoundEntry->second = pEntry;
						}
						else
							hplDelete(pEntry);
					}
					else
						mmapEntries[sEntryName] = pEntry;
				}

				lstFiles.erase(find(lstFiles.begin(),lstFiles.end(), cString::To16Char(cString::GetFileName(pEntry->GetFileName()))));
			}
			else
			{
				hplDelete(pEntry);
			}
		}
	}

	////////////////////////////////////////////////////////////////
	// Create the entries not listed in the XML but present in disk
	if(lstFiles.empty()==false)
	{
		bUpdated = true;
		tWStringListIt itFiles = lstFiles.begin();
		for(;itFiles!=lstFiles.end();++itFiles)
		{
			iEditorObjectIndexEntry* pEntry = CreateEntry();
			if(pEntry->CreateFromFile(*itFiles))
			{
				mmapEntries[pEntry->GetEntryName()] = pEntry;
			}
			else
				hplDelete(pEntry);
		}
	}

	if(abAddSubDirs)
	{
		cPlatform::FindFoldersInDir(lstDirs, sFullPath, false, false);
		if(pDirs)
		{
			for(tinyxml2::XMLElement* pXmlDir = pDirs->FirstChildElement(); pXmlDir != NULL; pXmlDir = pXmlDir->NextSiblingElement())
			{
				iEditorObjectIndexDir* pDir = mpIndex->CreateDir(this);

				if(pDir->Refresh(pXmlDir, abAddSubDirs))
				{
					const tWString& sRelPath = pDir->GetDirName();
					mmapSubDirs.insert(std::pair<tWString,iEditorObjectIndexDir*>(sRelPath, pDir));
					tWStringListIt it = find(lstDirs.begin(),lstDirs.end(), sRelPath);
					if (it == lstDirs.end()) {
						Warning("Error finding Directory %ls in list for path %ls\n",sRelPath.c_str(), sFullPath.c_str());
					} else {
						lstDirs.erase(it);
					}
				}
				else
				{
					hplDelete(pDir);
				}
			}
		}
	

		if(lstDirs.empty()==false)
		{
			bUpdated = true;
			tWStringListIt itDirs = lstDirs.begin();
			for(;itDirs!=lstDirs.end();++itDirs)
			{
				iEditorObjectIndexDir* pDir = mpIndex->CreateDir();
				if(pDir->CreateFromDir(this, *itDirs, false))
				{
					mmapSubDirs.insert(std::pair<tWString, iEditorObjectIndexDir*>(*itDirs, pDir));
				}
				else
					hplDelete(pDir);
			}
		}
	}


	mpIndex->SetUpdated(bUpdated);

	

	return true;
}

//-------------------------------------------------------------------

iEditorObjectIndexEntry* iEditorObjectIndexDir::GetEntry(const tString& asIndex)
{
	tIndexEntryMapIt it = mmapEntries.find(asIndex);
	if(it!=mmapEntries.end())
		return it->second;
	
	return NULL;
}

//-------------------------------------------------------------------

void iEditorObjectIndexDir::Save(tinyxml2::XMLElement* apIndexElement, bool abSaveSubDirs)
{
	SetAttributeString(apIndexElement, "RelPath", cString::To8Char(msDirName));
	if(mmapEntries.empty()==false)
	{
		tinyxml2::XMLElement* pEntries = apIndexElement->GetDocument()->NewElement("Entries");
		apIndexElement->InsertEndChild(pEntries);
		tIndexEntryMapConstIt itEntries = mmapEntries.begin();
		for(;itEntries!=mmapEntries.end();++itEntries)
		{
			tinyxml2::XMLElement* pXmlEntry = pEntries->GetDocument()->NewElement("");
			pEntries->InsertEndChild(pXmlEntry);

			iEditorObjectIndexEntry* pEntry = itEntries->second;
			pEntry->Save(pXmlEntry);
		}
	}

	if(abSaveSubDirs && mmapSubDirs.empty()==false)
	{
		tinyxml2::XMLElement* pDirs = apIndexElement->GetDocument()->NewElement("SubDirs");
		apIndexElement->InsertEndChild(pDirs);
		tIndexDirMapIt itSubDirs = mmapSubDirs.begin();
		for(;itSubDirs!=mmapSubDirs.end();++itSubDirs)
		{
			iEditorObjectIndexDir* pDir = itSubDirs->second;

			tinyxml2::XMLElement* pXmlDir = pDirs->GetDocument()->NewElement("Dir");
			pDirs->InsertEndChild(pXmlDir);

			pDir->Save(pXmlDir, abSaveSubDirs);
		}
	}
}

//-------------------------------------------------------------------

/////////////////////////////////////////////////////////////////////
// INDEX ENTRY
/////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------

iEditorObjectIndexEntry::iEditorObjectIndexEntry(iEditorObjectIndexDir* apParentDir)
{
	mpParentDir = apParentDir;
	
	mbRemoved = false;
	mbUpdated = false;
}

//-------------------------------------------------------------------

bool iEditorObjectIndexEntry::CreateFromFile(const tWString& asFilename)
{
	msFileName = cString::To8Char(asFilename);
	if(CheckFileExists()==false)
		return false;

	BuildEntryName(msEntryName);
	msDateModified = cPlatform::FileModifiedDate(GetFileNameFullPath()).ToString();

	BuildThumbnail();

	return true;
}

//-------------------------------------------------------------------

bool iEditorObjectIndexEntry::CreateFromXmlElement(tinyxml2::XMLElement* apElement)
{
	msFileName = GetAttributeString(apElement, "Filename");
	msEntryName = GetAttributeString(apElement, "EntryName");
	msDateModified = GetAttributeString(apElement, "DateModified");

	if(CheckFileExists()==false)
		return false;

	if(CheckNeedsUpdate())
	{
		mbUpdated = true;

		return CreateFromFile(cString::To16Char(msFileName));
	}

	BuildThumbnail();

	return true;
}

//-------------------------------------------------------------------

bool iEditorObjectIndexEntry::CheckFileExists()
{
	return cPlatform::FileExists(GetFileNameFullPath());
}

//-------------------------------------------------------------------

bool iEditorObjectIndexEntry::CheckNeedsUpdate()
{
	return (msDateModified != cPlatform::FileModifiedDate(GetFileNameFullPath()).ToString());
}

//-------------------------------------------------------------------

tWString iEditorObjectIndexEntry::GetFileNameRelPath()
{
	tWString sRelPath = mpParentDir->GetRelPath();
	tWString sFilenameRelPath = cString::AddSlashAtEndW(sRelPath) + cString::To16Char(msFileName);

	return sFilenameRelPath;
}

//-------------------------------------------------------------------

tWString iEditorObjectIndexEntry::GetFileNameFullPath()
{
	tWString sFilenameFullPath = cString::AddSlashAtEndW(mpParentDir->GetFullPath()) + cString::To16Char(msFileName);

	return sFilenameFullPath;
}

//-------------------------------------------------------------------

const tWString& iEditorObjectIndexEntry::GetThumbnailFilename()
{
	if(msThumbnailFilename==_W(""))
	{
		cEditorThumbnailBuilder* pTmbBuilder = mpParentDir->GetIndex()->GetEditor()->GetThumbnailBuilder();
		msThumbnailFilename = pTmbBuilder->GetThumbnailNameFromFileW(GetFileNameFullPath());
	}

	return msThumbnailFilename;
}

//-------------------------------------------------------------------

void iEditorObjectIndexEntry::Save(tinyxml2::XMLElement* apElement)
{
	apElement->SetValue(mpParentDir->GetIndex()->GetEntryTypeName().c_str());

	SetAttributeString(apElement, "Filename", msFileName);
	SetAttributeString(apElement, "EntryName", msEntryName);
	SetAttributeString(apElement, "DateModified", msDateModified);
}

//-------------------------------------------------------------------

void iEditorObjectIndexEntry::BuildEntryName(tString& asEntryName)
{
	asEntryName = msFileName;
	int lDotPos = cString::GetLastStringPos(asEntryName, ".");
	asEntryName = cString::Sub(asEntryName,0,lDotPos);
	asEntryName = cString::GetFileName(asEntryName);
	
	/*
	int lBaseFolderLength = 0;
	iEditorObjectIndex* pIndex = mpParentDir->GetIndex();

	if(cString::GetFirstStringPos(msFileName, cString::To8Char(pIndex->GetBaseFolder()))==0 )
		lBaseFolderLength = (int)pIndex->GetBaseFolder().size() + 1;

	int lLastDotPosition = cString::GetLastStringPos(msFileName, ".");
	asEntryName = cString::Sub(msFileName,lBaseFolderLength, lLastDotPosition-lBaseFolderLength);*/
}
