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

#ifndef HPLEDITOR_LEVEL_EDITOR_WORLD_H
#define HPLEDITOR_LEVEL_EDITOR_WORLD_H

#include "../common/EditorWorld.h"
#include "LevelEditorStaticObjectCombo.h"

//---------------------------------------------------------------

namespace tinyxml2 { class XMLElement; class XMLDocument; }

//---------------------------------------------------------------

class cLevelEditorStaticObjectCombo;

//---------------------------------------------------------------

//---------------------------------------------------------------

///////////////////////////////////////////////////////////
// cLevelEditorEntityExtData
//	Extension Data for entities in LevelEditor
class cLevelEditorEntityExtData
{
public:
	cLevelEditorEntityExtData() { mlGroupID = 0; mlComboID = -1; }

	int mlGroupID;
	int mlComboID;
};

//---------------------------------------------------------------

///////////////////////////////////////////////////////////
// cLevelEditorWorld
//	Scene data with specifics to LevelEditor
class cLevelEditorWorld : public iEditorWorld
{
public:
	cLevelEditorWorld(iEditorBase* apEditor);
	~cLevelEditorWorld();

	tinyxml2::XMLElement* GetWorldDataElement(tinyxml2::XMLElement* apXmlDoc);
	tinyxml2::XMLElement* GetWorldObjectsElement(tinyxml2::XMLElement* apWorldDataElement);
	void LoadWorldObjects(tinyxml2::XMLElement* apWorldObjectsElement);

	tinyxml2::XMLElement* CreateWorldDataElement(tinyxml2::XMLElement* apXmlDoc);
	tinyxml2::XMLElement* CreateWorldObjectsElement(tinyxml2::XMLElement* apWorldDataElement);
	void SaveWorldObjects(tinyxml2::XMLElement* apWorldObjectsElement, tEntityWrapperList& alstEnts);

	void Reset();

	//////////////////////////////////////////
	// StaticObject Combos management
	int GetFreeComboID() { return mlComboID++; }
	cLevelEditorStaticObjectCombo* CreateStaticObjectCombo(int alID);

	bool AddStaticObjectCombo(cLevelEditorStaticObjectCombo* apCombo);
	void RemoveStaticObjectCombo(int alID);
	void ClearStaticObjectCombos();
	const tStaticObjectComboList& GetStaticObjectCombos() { return mlstStaticObjectCombos; }

	cLevelEditorStaticObjectCombo* GetStaticObjectCombo(int alComboId);
	int GetStaticObjectComboNum();

	////////////////////////////////////////////
	// Level Editor specific code
	void CreateDataCallback(iEntityWrapperData* apData);
	void CopyDataFromEntityCallback(iEntityWrapperData* apData, iEntityWrapper* apEnt);
	void CopyDataToEntityCallback(iEntityWrapperData* apData, iEntityWrapper* apEnt, int alCopyStep);
	void DestroyDataCallback(iEntityWrapperData* apData);

	void SaveDataCallback(iEntityWrapperData* apData, tinyxml2::XMLElement* apElement);
	void LoadDataCallback(iEntityWrapperData* apData, tinyxml2::XMLElement* apElement);

	void DestroyEntityWrapperCallback(iEntityWrapper* apEnt);

protected:

	int mlComboID;
	tStaticObjectComboList mlstStaticObjectCombos;
};

//---------------------------------------------------------------

#endif // HPLEDITOR_LEVEL_EDITOR_WORLD_H

