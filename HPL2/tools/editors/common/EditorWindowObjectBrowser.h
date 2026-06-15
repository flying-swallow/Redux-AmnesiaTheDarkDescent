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

#ifndef HPLEDITOR_EDITOR_WINDOW_OBJECT_BROWSER_H
#define HPLEDITOR_EDITOR_WINDOW_OBJECT_BROWSER_H

#include "EditorWindow.h"
#include "EditorIndex.h"

#include "gui/WidgetNodeTree.h"
#include "gui/WidgetMeshObjectList.h"

#include <memory>

namespace hpl {
	class Image;
}

namespace tinyxml2 { class XMLElement; }

//----------------------------------------------------------

class iEditorBase;

class iEditorObjectIndex;
class iEditorObjectIndexEntry;

class cEditorWindowObjectBrowser;

//----------------------------------------------------------

////////////////////////////////////////////////////////////
// BASE INDEX ENTRY FOR MESH OBJECTS
////////////////////////////////////////////////////////////

//----------------------------------------------------------

class iEditorObjectIndexEntryMeshObject : public iEditorObjectIndexEntry
{
public:
	iEditorObjectIndexEntryMeshObject(iEditorObjectIndexDir* apDir);

	bool CreateFromFile(const tWString& asFilename);
	bool CreateFromXmlElement(tinyxml2::XMLElement* apElement);

	const cVector3f& GetBVMin() { return mvBVMin; }
	const cVector3f& GetBVMax() { return mvBVMax; }
	int GetTriangleCount() { return mlTriangleCount; }

	virtual tWString& GetMeshFileName();

	void Save(tinyxml2::XMLElement* apElement);

	virtual cMeshEntity* CreateTempEntity(cWorld* apWorld);

	// The entry OWNS its GPU-resident thumbnail — delivered by the builder's
	// callback, released when the entry dies (GPU frees are deferred).
	Image* GetThumbnailImage() { return mpThumbnailImage.get(); }
protected:
	void BuildThumbnail();

	cVector3f mvBVMin;
	cVector3f mvBVMax;
	int mlTriangleCount;

	tString msTempFileName;

	tWString msMeshFileName;

	std::shared_ptr<Image> mpThumbnailImage;
};


//----------------------------------------------------------

////////////////////////////////////////////////////////////
// THUMBNAIL GRID ITEM
////////////////////////////////////////////////////////////

//----------------------------------------------------------

class cWidgetMeshObjectBrowserItem : public iWidgetMeshObjectItem
{
public:
	cWidgetMeshObjectBrowserItem(const tWString& asName, iEditorObjectIndexEntry* apObject, iEditorBase* apEditor);
	~cWidgetMeshObjectBrowserItem();

	void LoadThumbnail();
	void DisposeThumbnail();

protected:
	iEditorObjectIndexEntry* mpObject;
	iEditorBase* mpEditor;
};

//----------------------------------------------------------

////////////////////////////////////////////////////////////
// OBJECT BROWSER WINDOW
////////////////////////////////////////////////////////////

//----------------------------------------------------------

class cEditorWindowObjectBrowser : public iEditModeObjectCreatorWindow
{
public:
	cEditorWindowObjectBrowser(iEditorEditMode* apEditMode, const tWStringVec& avBaseDirs, bool abAddCategoryHeaders);
	~cEditorWindowObjectBrowser();

	virtual iEditorObjectIndexEntryMeshObject* GetSelectedObject();
	cMeshEntity* GetPreviewEntity() { return mpPreviewEntity; }

	void Reset();

	cMesh* GetSelectedMesh();
protected:
	void OnInitLayout();

	bool ObjectSets_OnChange(iWidget* apWidget, const cGuiMessageData& aData);
	kGuiCallbackDeclarationEnd(ObjectSets_OnChange);

	bool ObjectList_OnChangeSelection(iWidget* apWidget, const cGuiMessageData& aData);
	kGuiCallbackDeclarationEnd(ObjectList_OnChangeSelection);

	bool Input_OnFilterTextChanged(iWidget* apWidget, const cGuiMessageData& aData);
	kGuiCallbackDeclarationEnd(Input_OnFilterTextChanged);

	bool Refresh_OnPressed(iWidget* apWidget, const cGuiMessageData& aData);
	kGuiCallbackDeclarationEnd(Refresh_OnPressed);

	void BuildObjectSetList();
	void BuildObjectSetListHelper(const tWString& asFolder, int alLevel, cWidgetTreeNode* apNode);

	void BuildObjectList();
	void ClearObjectList();
	virtual void UpdateObjectList();

	void UpdateObjectInfo();
	void AddEntriesInDirToList(iEditorObjectIndexDir* apDir,std::vector<iEditorObjectIndexEntryMeshObject*>& avEntries);

	iEditorObjectIndex* CreateIndex(cWidgetTreeNode* apNode);
	virtual iEditorObjectIndex* CreateSpecificIndex(iEditorBase* apEditorBase, const tWString& asFolder)=0;

	virtual cMeshEntity* CreatePreviewEntity(iEditorObjectIndexEntryMeshObject* apEntry)=0;

	////////////////////////////////////////////////////
	// Data
	tWStringVec mvBaseDirs;

	// Layout stuff
	cWidgetFrame* mpDirectoryFrame;
	cWidgetNodeTree* mpDirectoryTree;
	cWidgetTreeNode* mpRootDirectory;

	cWidgetTextBox* mpObjectFilter;
	cWidgetLabel* mpObjectFilterTempText;

	cWidgetFrame* mpObjectSelectGroup;
	cWidgetMeshObjectList* mpObjectList;

	tWStringVec mvCategoryStrings;

	bool mbAddCategoryHeaders;

	cMeshEntity* mpPreviewEntity;

	std::map<tWString, iEditorObjectIndex*> mmapObjectIndices;
	iEditorObjectIndex* mpCurrentIndex;
	std::vector<iEditorObjectIndexEntryMeshObject*> mvCurrentListedEntries;

	tIntVec mvDirIndices;
};

//----------------------------------------------------------

#endif // HPLEDITOR_EDITOR_WINDOW_OBJECT_BROWSER_H
