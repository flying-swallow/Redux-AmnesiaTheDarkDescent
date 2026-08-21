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

#ifndef MODEL_EDITOR_WORLD_H
#define MODEL_EDITOR_WORLD_H

#include "../common/EditorWorld.h"

//--------------------------------------------------------------------

namespace tinyxml2 { class XMLElement; class XMLDocument; }

class cEntityWrapperTypeSubMesh;
class cEntityWrapperTypeBone;
class cAnimationEventWrapper;
class cAnimationWrapper;

class cEditorUserClassSubType;
class cEditorClassInstance;

//--------------------------------------------------------------------

typedef std::vector<cAnimationEventWrapper> tAnimEventWrapperVec;
typedef tAnimEventWrapperVec::iterator tAnimEventWrapperVecIt;

typedef std::vector<cAnimationWrapper> tAnimWrapperVec;
typedef tAnimWrapperVec::iterator tAnimWrapperVecIt;

typedef std::map<tWString, tWString> tVarValueMap;
typedef tVarValueMap::iterator		 tVarValueMapIt;

//--------------------------------------------------------------------

class cAnimationEventWrapper
{
public:
	cAnimationEventWrapper();

	int GetIndex() { return mlIndex; }

	void Load(tinyxml2::XMLElement* apElement);
	void Save(tinyxml2::XMLElement* apElement);

	void SetTime(float afX) { mfTime = afX; }
	float GetTime() { return mfTime; }

	void SetType(const tString& asX) { msType = asX; }
	const tString& GetType() { return msType; }

	void SetValue(const tString& asX) { msValue = asX; }
	const tString& GetValue() { return msValue; }

	bool IsValid();

protected:
	static int mlIndices;
	int mlIndex;
	float mfTime;
	tString msType;
	tString msValue;
};

//--------------------------------------------------------------------

class cAnimationWrapper
{
public:
	cAnimationWrapper();

	bool IsValid();

	void Load(tinyxml2::XMLElement* apElement);
	void Save(tinyxml2::XMLElement* apElement);

	void SetName(const tString& asName) { msName = asName; }
	const tString& GetName() { return msName; }

	void SetFile(const tString& asFile) { msFile = asFile; }
	const tString& GetFile() { return msFile; }

	void SetSpeed(float afX) { mfSpeed = afX; }
	float GetSpeed() { return mfSpeed; }

	void SetSpecialEventTime(float afX) { mfSpecialEventTime = afX; }
	float GetSpecialEventTime() { return mfSpecialEventTime; }

	tAnimEventWrapperVec& GetEvents() { return mvEvents; }
	cAnimationEventWrapper* GetEvent(int alIdx) { return &mvEvents[alIdx]; }
	int AddEvent();
	void RemoveEvent(int alIdx);
	void ClearEvents();

protected:
	tString msName;
	tString msFile;
	float mfSpeed;
	float mfSpecialEventTime;

	tAnimEventWrapperVec mvEvents;
};

//--------------------------------------------------------------------

class cModelEditorWorld : public iEditorWorld
{
public:
	cModelEditorWorld(iEditorBase* apEditor);
	// Frees mpClass (SetType(NULL) releases the current class instance) — needed
	// for transient headless use (MCP create_entity_file) and plugs a latent leak
	// in the ModelEditor itself.
	~cModelEditorWorld();

	void Reset();

	cEntityWrapperTypeSubMesh* GetSubMeshType() { return mpTypeSubMesh; }
	cEntityWrapperTypeBone*	   GetBoneType()	{ return mpTypeBone; }
	
	cMeshEntity* GetMesh();
	tString GetMeshFilename();

	///////////////////////////////////////////
	// Mesh
	void SetMeshFromElement(tinyxml2::XMLElement* apMeshElement, tinyxml2::XMLElement* apBonesElement);

	///////////////////////////////////////////
	// Animations
	void SetAnimations(const tAnimWrapperVec& avAnims);
	tAnimWrapperVec& GetAnimations() { return mvAnimations; }

	///////////////////////////////////////////
	// User defined variables
	void SetTempValues(tVarValueMap& amapX);
	void SetType(cEditorUserClassSubType* apType, bool abKeepValues=true);
	cEditorClassInstance* GetClass() { return mpClass; }

	tinyxml2::XMLElement* GetWorldDataElement(tinyxml2::XMLElement* apXmlDoc);
	void LoadWorldData(tinyxml2::XMLElement* apWorldDataElement);
	tinyxml2::XMLElement* GetWorldObjectsElement(tinyxml2::XMLElement* apWorldDataElement);

	tinyxml2::XMLElement* CreateWorldDataElement(tinyxml2::XMLElement* apXmlDoc);
	tinyxml2::XMLElement* CreateWorldObjectsElement(tinyxml2::XMLElement* apWorldDataElement);

	void SaveWorldData(tinyxml2::XMLElement*);

protected:

	bool CustomCategorySaver(tinyxml2::XMLElement*);
	bool CustomCategoryLoader(tinyxml2::XMLElement*, tinyxml2::XMLElement*);

	cEntityWrapperTypeSubMesh* mpTypeSubMesh;
	cEntityWrapperTypeBone*		mpTypeBone;

	///////////////////////
	// Model Animations
	tAnimWrapperVec mvAnimations;

	///////////////////////
	// User defined variables
	cEditorClassInstance* mpClass;
	tVarValueMap mmapTempValues;
};


#endif // MODEL_EDITOR_WORLD_H
