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

#include "EditorWindowObjectBrowser.h"
#include "EditorThumbnailBuilder.h"
#include "EditorGrid.h"

#include "EditorWorld.h"

#include <tinyxml2.h>
#include "resources/XmlHelper.h"

#include <algorithm>

//-------------------------------------------------------------------
//-------------------------------------------------------------------

iEditorObjectIndexEntryMeshObject::iEditorObjectIndexEntryMeshObject(iEditorObjectIndexDir* apDir) : iEditorObjectIndexEntry(apDir)
{
}

//-------------------------------------------------------------------

tWString& iEditorObjectIndexEntryMeshObject::GetMeshFileName()
{
	if(msMeshFileName==_W(""))
		msMeshFileName = GetFileNameRelPath();

	return msMeshFileName;
}

//-------------------------------------------------------------------

bool iEditorObjectIndexEntryMeshObject::CreateFromFile(const tWString& asFilename)
{
	if(iEditorObjectIndexEntry::CreateFromFile(asFilename)==false)
		return false;

	iEditorObjectIndex* pIndex = mpParentDir->GetIndex();

	cMeshEntity* pMeshEntity = CreateTempEntity(pIndex->GetEditor()->GetTempWorld());

	mlTriangleCount = pMeshEntity->GetMesh()->GetTriangleCount();

	mvBVMin = 999999999.0f;
	mvBVMax = -999999999.0f;
	for(int i=0;i<pMeshEntity->GetSubMeshEntityNum();++i)
	{
		cSubMeshEntity* pSubMeshEntity = pMeshEntity->GetSubMeshEntity(i);

		cBoundingVolume* pBV = pSubMeshEntity->GetBoundingVolume();
		cMath::ExpandAABB(mvBVMin, mvBVMax, pBV->GetMin(), pBV->GetMax());
	}

	return true;
}

//-------------------------------------------------------------------

bool iEditorObjectIndexEntryMeshObject::CreateFromXmlElement(tinyxml2::XMLElement* apElement)
{
	if(iEditorObjectIndexEntry::CreateFromXmlElement(apElement)==false)
		return false;

	mlTriangleCount = GetAttributeInt(apElement, "TriCount");
	mvBVMin = GetAttributeVector3f(apElement, "BVMin");
	mvBVMax = GetAttributeVector3f(apElement, "BVMax");

	return true;
}

//-------------------------------------------------------------------

void iEditorObjectIndexEntryMeshObject::Save(tinyxml2::XMLElement* apElement)
{
	iEditorObjectIndexEntry::Save(apElement);

	SetAttributeInt(apElement, "TriCount", mlTriangleCount);
	SetAttributeVector3f(apElement, "BVMin", mvBVMin);
	SetAttributeVector3f(apElement, "BVMax", mvBVMax);
}

//-------------------------------------------------------------------

void iEditorObjectIndexEntryMeshObject::BuildThumbnail()
{
	iEditorObjectIndex* pIndex = mpParentDir->GetIndex();
	iEditorBase* pEditor = pIndex->GetEditor();
	cEditorThumbnailBuilder* pTmbBuilder = pEditor->GetThumbnailBuilder();

	////////////////////////////////////////////////////////////////
	// Create thumbnail if it doesn't exist or the object was updated
	bool bIsUpdated = IsUpdated();
	if(bIsUpdated || mpThumbnailImage==NULL)
	{
		// Async: this only enqueues — the builder hands the finished Image
		// to the callback a frame later (ownership transfers to this entry)
		// and the grid cell picks it up via GetThumbnailImage(). The entity
		// goes in the builder's OWN world (isolated from the editor temp
		// world). Capturing `this` is safe: PreBuild flushes the queue at the
		// start of every index Refresh, before entries are deleted/recreated.
		cMeshEntity* pEnt = CreateTempEntity(pTmbBuilder->GetWorld());

		pTmbBuilder->BuildThumbnailFromMeshEntity(pEnt,
			[this](std::shared_ptr<Image> apImage)
			{
				mpThumbnailImage = std::move(apImage);
			});
	}
}

//-------------------------------------------------------------------

cMeshEntity* iEditorObjectIndexEntryMeshObject::CreateTempEntity(cWorld* apWorld)
{
	iEditorObjectIndex* pIndex = mpParentDir->GetIndex();
	iEditorBase* pEditor = pIndex->GetEditor();
	cResources* pResources = pEditor->GetEngine()->GetResources();

	tString sSourceFile = cString::To8Char(GetFileNameFullPath());

	cMesh* pMesh = pResources->GetMeshManager()->CreateMesh(sSourceFile);
    if(pMesh==NULL)
		return NULL;

	cMeshEntity* pMeshEntity = apWorld->CreateMeshEntity("TempObject_"+msFileName, pMesh);
	pMeshEntity->SetSourceFile(sSourceFile);
	pMeshEntity->SetActive(false);
	pMeshEntity->SetVisible(false);

	return pMeshEntity;
}

//-------------------------------------------------------------------

////////////////////////////////////////////////////////////////////
// THUMBNAIL GRID ITEM
////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------

cWidgetMeshObjectBrowserItem::cWidgetMeshObjectBrowserItem(const tWString& asName, iEditorObjectIndexEntry* apObject, iEditorBase* apEditor) : iWidgetMeshObjectItem(asName)
{
	mpObject = apObject;
	mpEditor = apEditor;
}

//-------------------------------------------------------------------

cWidgetMeshObjectBrowserItem::~cWidgetMeshObjectBrowserItem()
{
	DisposeThumbnail();
	mpObject = NULL;
	mpEditor = NULL;
}

//-------------------------------------------------------------------

void cWidgetMeshObjectBrowserItem::LoadThumbnail()
{
	if (mpObject == NULL) return;

	// GPU-resident path: the entry's thumbnail is built asynchronously (enqueued
	// at index creation, see iEditorObjectIndexEntry::CreateFrom*). Read the
	// entry's Image once it has arrived; until then leave mpThumbnail NULL so
	// the grid shows the gray placeholder and re-checks next draw.
	Image* pImage = ((iEditorObjectIndexEntryMeshObject*)mpObject)->GetThumbnailImage();
	if (pImage)
	{
		cGui* pGui = mpEditor->GetEngine()->GetGui();
		// autoDestroy=false: the entry owns the Image; we only wrap it.
		mpThumbnail = pGui->CreateGfxTexture(pImage, false, eGuiMaterial_Diffuse);
	}
}

//-------------------------------------------------------------------

void cWidgetMeshObjectBrowserItem::DisposeThumbnail()
{
	if (mpThumbnail && mpEditor)
		mpEditor->GetEngine()->GetGui()->DestroyGfx(mpThumbnail);

	mpThumbnail = NULL;
}

//-------------------------------------------------------------------

////////////////////////////////////////////////////////////////////
// CONSTRUCTORS
////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------

cEditorWindowObjectBrowser::cEditorWindowObjectBrowser(iEditorEditMode* apEditMode,
													   const tWStringVec& avBaseDirs,
													   bool abAddCategoryHeaders) : iEditModeObjectCreatorWindow(apEditMode)

{
	mvBaseDirs = avBaseDirs;
	mbAddCategoryHeaders = abAddCategoryHeaders;

	mpPreviewEntity = NULL;
	mpCurrentIndex = NULL;
}

cEditorWindowObjectBrowser::~cEditorWindowObjectBrowser()
{
	mvCurrentListedEntries.clear();

	if(mpPreviewEntity)
		mpEditor->GetEditorWorld()->GetWorld()->DestroyMeshEntity(mpPreviewEntity);

	STLMapDeleteAll(mmapObjectIndices);
}

//-------------------------------------------------------------------

////////////////////////////////////////////////////////////////////
// PUBLIC METHODS
////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------

iEditorObjectIndexEntryMeshObject* cEditorWindowObjectBrowser::GetSelectedObject()
{
	int lIndex = mpObjectList->GetSelectedItemIdx();

	if(lIndex==-1 || lIndex >(int)mvCurrentListedEntries.size())
		return NULL;

	return mvCurrentListedEntries[lIndex];
}

//-------------------------------------------------------------------

void cEditorWindowObjectBrowser::Reset()
{
	UpdateObjectInfo();
}

//-------------------------------------------------------------------

////////////////////////////////////////////////////////////////////
// PROTECTED METHODS
////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------

bool cEditorWindowObjectBrowser::ObjectSets_OnChange(iWidget* apWidget, const cGuiMessageData& aData)
{
	BuildObjectList();
	return true;
}
kGuiCallbackDeclaredFuncEnd(cEditorWindowObjectBrowser,ObjectSets_OnChange);

//-------------------------------------------------------------------

bool cEditorWindowObjectBrowser::ObjectList_OnChangeSelection(iWidget* apWidget, const cGuiMessageData& aData)
{
	UpdateObjectInfo();
	return true;
}
kGuiCallbackDeclaredFuncEnd(cEditorWindowObjectBrowser, ObjectList_OnChangeSelection);

//-------------------------------------------------------------------

bool cEditorWindowObjectBrowser::Input_OnFilterTextChanged(iWidget* apWidget, const cGuiMessageData& aData)
{
	const tWString& sText = mpObjectFilter->GetText();
	mpObjectFilterTempText->SetVisible(sText.empty());
	mpObjectList->SetFilter(sText);
	mpObjectList->UpdateProperties();
	return true;
}
kGuiCallbackDeclaredFuncEnd(cEditorWindowObjectBrowser, Input_OnFilterTextChanged);

//-------------------------------------------------------------------

bool cEditorWindowObjectBrowser::Refresh_OnPressed(iWidget* apWidget, const cGuiMessageData& aData)
{
	if(mpCurrentIndex && mpCurrentIndex->Refresh())
		UpdateObjectList();

	return true;
}
kGuiCallbackDeclaredFuncEnd(cEditorWindowObjectBrowser, Refresh_OnPressed);

//-------------------------------------------------------------------

void cEditorWindowObjectBrowser::OnInitLayout()
{
	//////////////////////
	// Set up layout
    mpBGFrame->SetSize(cVector2f(200,700));

	///////////////////////////////////////////////////
	// Directory tree
	mpDirectoryFrame = mpSet->CreateWidgetFrame(cVector3f(5, 8, 0.1f), cVector2f(190, 205), true, mpBGFrame, false, true);
	mpDirectoryFrame->SetBackgroundBgfx(eGuiSkinGfx_ListBoxBackground);
	mpDirectoryFrame->SetDrawBackground(true);

	mpDirectoryTree = mpSet->CreateWidgetNodeTree(190, mpDirectoryFrame);
	mpDirectoryTree->AddCallback(eGuiMessage_SelectionChange, this, kGuiCallback(ObjectSets_OnChange));
	mpRootDirectory = mpDirectoryTree->AddTreeNode(_W("All"));
	mpRootDirectory->SetName(_W("_ALL_"));
	mpRootDirectory->SetExtended(true);

	///////////////////////////////////////////////////
	// Filter
	mpObjectFilter = mpSet->CreateWidgetTextBox(cVector3f(3, 217, 0.1f), cVector2f(194, 0), _W(""), mpBGFrame);
	mpObjectFilter->AddCallback(eGuiMessage_TextChange, this, kGuiCallback(Input_OnFilterTextChanged));

	mpObjectFilterTempText = mpSet->CreateWidgetLabel(cVector3f(8, 220, 0.2f), cVector2f(194, 0), _W("Type a filter string here..."), mpBGFrame);
	mpObjectFilterTempText->SetColorMul(cColor(1, 1, 1, 0.3f));

	///////////////////////////////////////////////////
	// Object grid
	mpObjectSelectGroup = mpSet->CreateWidgetFrame(cVector3f(5, 243, 0.1f), cVector2f(190, 260), true, mpBGFrame, false, true);
	mpObjectSelectGroup->SetBackGroundColor(cColor(0.35f, 0.35f, 0.35f, 1));
	mpObjectSelectGroup->SetDrawBackground(true);

	mpObjectList = mpSet->CreateWidgetMeshObjectList(0, cVector2f(172, 172), mpObjectSelectGroup);
	mpObjectList->AddCallback(eGuiMessage_SelectionChange, this, kGuiCallback(ObjectList_OnChangeSelection));

	BuildObjectSetList();
}

//-------------------------------------------------------------------

void cEditorWindowObjectBrowser::BuildObjectSetList()
{
	mpRootDirectory->ClearTreeNodes();

	for(int i=0;i<(int)mvBaseDirs.size();++i)
        BuildObjectSetListHelper(mvBaseDirs[i],0,mpRootDirectory);
}

//-------------------------------------------------------------------

void cEditorWindowObjectBrowser::BuildObjectSetListHelper(const tWString& asFolder, int alLevel, cWidgetTreeNode* apNode)
{
	tWStringList lstObjectDirs;

	cPlatform::FindFoldersInDir(lstObjectDirs, asFolder, false);

	tWStringListIt it = lstObjectDirs.begin();

	for(;it!=lstObjectDirs.end();++it)
	{
		tWString sDir = cString::AddSlashAtEndW(asFolder) + *it;

		cWidgetTreeNode* pNode = apNode->AddTreeNode(*it);
		pNode->SetName(sDir);
		BuildObjectSetListHelper(sDir, alLevel+1, pNode);
	}
}

//-------------------------------------------------------------------

void cEditorWindowObjectBrowser::BuildObjectList()
{
	cWidgetTreeNode* pNode = mpDirectoryTree->GetSelectedNode();

	if(pNode && pNode != mpRootDirectory) mpCurrentIndex = CreateIndex(pNode);
	else mpCurrentIndex = NULL;

	UpdateObjectList();
}

//-------------------------------------------------------------------

void cEditorWindowObjectBrowser::ClearObjectList()
{
	mvCurrentListedEntries.clear();
	mpObjectList->ClearItems();
	UpdateObjectInfo();
}

//-------------------------------------------------------------------

void cEditorWindowObjectBrowser::UpdateObjectList()
{
	ClearObjectList();
	if (mpCurrentIndex == NULL)
	{
		// "All" / a folder with no index of its own: list every immediate
		// child folder's index.
		tWidgetTreeNodeVec& vChildNodes = mpRootDirectory->GetChildNodes();

		for (int i = 0; i < (int)vChildNodes.size(); ++i)
		{
			cWidgetTreeNode* pNode = vChildNodes[i];
			if (pNode == NULL)
				continue;

			iEditorObjectIndex* pIndex = CreateIndex(pNode);
			if(pIndex)
				AddEntriesInDirToList(pIndex->GetRootDir(), mvCurrentListedEntries);
		}
	}
	else AddEntriesInDirToList(mpCurrentIndex->GetRootDir(), mvCurrentListedEntries);

	mpObjectList->UpdateProperties();
}

//-------------------------------------------------------------------

void cEditorWindowObjectBrowser::UpdateObjectInfo()
{
	iEditorObjectIndexEntryMeshObject* pObj = GetSelectedObject();

	cWorld* pWorld = mpEditor->GetEditorWorld()->GetWorld();

	if(mpPreviewEntity)
		pWorld->DestroyMeshEntity(mpPreviewEntity);
	mpPreviewEntity = pObj?pObj->CreateTempEntity(pWorld):NULL;
}

//-------------------------------------------------------------------

iEditorObjectIndex* cEditorWindowObjectBrowser::CreateIndex(cWidgetTreeNode* apNode)
{
	if (apNode == mpRootDirectory) return NULL;

	// Rebuild the folder path relative to the base dir from the node chain.
	tWString sFullFolder = apNode->GetText();
	cWidgetTreeNode* pParentNode = apNode->GetParentNode();
	while (pParentNode)
	{
		if (pParentNode == mpRootDirectory) break;
		sFullFolder = cString::AddSlashAtEndW(pParentNode->GetText()) + sFullFolder;
		pParentNode = pParentNode->GetParentNode();
	}

	///////////////////////////////////////////////////////////
	// First try to return an already created index
	std::map<tWString, iEditorObjectIndex*>::iterator it = mmapObjectIndices.find(sFullFolder);
    if(it!=mmapObjectIndices.end())
	{
		iEditorObjectIndex* pIndex = it->second;
		// Should the index be refreshed here?
		pIndex->Refresh();
		return pIndex;
	}

	mpEditor->ShowLoadingWindow(_W("Loading"), _W("Loading"));

	iEditorObjectIndex* pIndex = NULL;
	for(int i=0;i<(int)mvBaseDirs.size();++i)
	{
		tWString sFolder = cString::AddSlashAtEndW(mvBaseDirs[i]) + sFullFolder;
		if(cPlatform::FolderExists(sFolder)==false)
			continue;

		pIndex = CreateSpecificIndex(mpEditor, sFolder);
		if(pIndex)
		{
			mmapObjectIndices[sFullFolder] = pIndex;
			pIndex->Create();

			break;
		}
	}

	return pIndex;
}

//-------------------------------------------------------------------

void cEditorWindowObjectBrowser::AddEntriesInDirToList(iEditorObjectIndexDir* apDir, std::vector<iEditorObjectIndexEntryMeshObject*>& avEntries)
{
	const tIndexEntryMap& mapEntries = apDir->GetEntries();
	tIndexEntryMapConstIt itEntries = mapEntries.begin();
	for(;itEntries!=mapEntries.end();++itEntries)
	{
		iEditorObjectIndexEntry* pEntry = itEntries->second;

		cWidgetMeshObjectBrowserItem* pItem =
			hplNew(cWidgetMeshObjectBrowserItem,
				(cString::To16Char(pEntry->GetEntryName()),
				pEntry,
				mpEditor));

		pItem->SetFullPath(pEntry->GetFileNameFullPath());

		mpObjectList->AddItem(pItem);
		avEntries.push_back((iEditorObjectIndexEntryMeshObject*)pEntry);
	}

	tIndexDirMap& mapSubDirs = (tIndexDirMap&)apDir->GetSubDirs();
	tIndexDirMapIt itSubDirs = mapSubDirs.begin();
	for(;itSubDirs!=mapSubDirs.end();++itSubDirs)
	{
		iEditorObjectIndexDir* pSubDir = itSubDirs->second;

		AddEntriesInDirToList(pSubDir, avEntries);
	}
}

//-------------------------------------------------------------------
