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

#ifndef HPLEDITOR_ENTITY_WRAPPER_ENTITY_H
#define HPLEDITOR_ENTITY_WRAPPER_ENTITY_H

#include "../common/StdAfx.h"

using namespace hpl;

#include "EntityWrapper.h"
#include "PrefabManager.h"

namespace tinyxml2 { class XMLElement; }

//---------------------------------------------------------------

class cEditorWindowEntityEditBoxEntity;

//---------------------------------------------------------------

#define EntityPropIdStart 40

enum eEntityInt
{
	eEntityInt_FileIndex = EntityPropIdStart,

	eEntityInt_LastEnum,
};

enum eEntityStr
{
	eEntityStr_Filename = EntityPropIdStart,

	eEntityStr_LastEnum,
};

//---------------------------------------------------------------

class cEntityWrapperTypeEntity : public iEntityWrapperTypeUserDefinedEntity
{
public:
	cEntityWrapperTypeEntity(cEditorUserClassSubType*);

	tString ToString();

	bool IsAppropriateType(tinyxml2::XMLElement*);
	bool IsAppropriateDefaultType(tinyxml2::XMLElement*);

	const tString& GetUserTypeName();
	const tString& GetUserSubTypeName();

	iEditorVar* GetAffectsShadowsVar();
	iEditorVar* GetAffectsLightVar();
	iEditorVar* GetAffectsParticlesVar();

	iEditorVar* GetLinkedEditorSetupVar(const tWString&, eVariableType);

protected:
 	iEntityWrapperData* CreateSpecificData();

	// Resolve a .ent's EntityType/EntitySubType PENDING-FIRST through cPrefabResource
	// (single source of truth); the file-static cache below only accelerates the disk parse.
	bool ResolveEntFileTypes(const tString& asFilename, tString& asTypeOut, tString& asSubTypeOut);

	/////////////////////////
	// Data for making checks for appropriate type faster and nicer
	static tString msLastCheckedFile;
	static tString msLastCheckedType;
	static tString msLastCheckedSubType;
};

//---------------------------------------------------------------

class cEntityWrapperDataEntity : public iEntityWrapperDataUserDefinedEntity
{
public:
	cEntityWrapperDataEntity(iEntityWrapperType*);
	~cEntityWrapperDataEntity();

	bool Load(tinyxml2::XMLElement* apElement);

protected:
	iEntityWrapper* CreateSpecificEntity();
};

//---------------------------------------------------------------

class cEntityWrapperEntity : public iEntityWrapperUserDefinedEntity
{
public:
	cEntityWrapperEntity(iEntityWrapperData* apData);
	~cEntityWrapperEntity();

	bool GetProperty(int, int&);
	bool GetProperty(int, tString&);

	bool SetProperty(int, const int&);
	bool SetProperty(int, const tString&);

	void SetFileIndex(int alIdx) { mlFileIndex = alIdx; }
	int GetFileIndex() { return mlFileIndex; }

	void SetFilename(const tString& asFilename);

	bool GetTypeChanged() { return mbTypeChanged; }
	void SetTypeChanged(bool abX) { mbTypeChanged = abX; }

	cEditorWindowEntityEditBox* CreateEditBox(cEditorEditModeSelect* apEditMode);

	bool IsAffectedByDecal(bool abAffectsStaticObject, bool abAffectsPrimitive, bool abAffectsEntity);

	void UpdateEntity();

	iEditorAction* CreateSetPropertyActionString(int alPropID, const tString& asX);
protected:
	iEngineEntity* CreateSpecificEngineEntity();

	void OnSetActive(bool abX);
	void OnSetVar(const tWString& asName, const tWString& asValue);

	bool SetEntityType(iEntityWrapperType* apType);

	// Re-point mPrefabRef at the prefab resource for the current msFilename
	// (empty filename → empty handle). Idempotent; called wherever msFilename
	// is (re)assigned so the reference count tracks placed instances exactly.
	// Also re-points mPrefabModifiedHandler at the new resource's OnModified().
	void UpdatePrefabRef();

	// This instance's prefab definition changed (session Save, MCP update_prefab,
	// or a dropped external-edit shadow) — rebuild THIS engine entity from the
	// current definition (pending-doc-first, else disk). Quiet live reload: no
	// undo, no map-dirty.
	void OnPrefabModified(ePrefabEvent aEvent);

	///////////////////////////
	// Data
	std::vector<iLight*> mvLights;
	std::vector<cMatrixf> mvLightLocalTransforms;

	int mlFileIndex;
	bool mbAffectedByDecal;

	bool mbTypeChanged;

	// Instance reference to this entity's .ent prefab resource: a lifetime/count
	// handle (RAII-released in the destructor, so the prefab definition cannot
	// vanish while placed instances exist) PLUS a modification subscription — this
	// instance rebuilds itself when its prefab changes (see OnPrefabModified).
	SharedResourceHandle<cPrefabResource> mPrefabRef;

	// The resource mPrefabModifiedHandler is currently connected to (NULL = none).
	// Guards UpdatePrefabRef so it only re-points the handler when the resource
	// actually changes — a rebuild re-enters UpdatePrefabRef with the SAME resource,
	// and re-connecting mid-Signal would churn the handler being iterated.
	cPrefabResource* mpSubscribedPrefab = NULL;
	Event<ePrefabEvent>::Handler mPrefabModifiedHandler {
		[this](ePrefabEvent aEvent){ OnPrefabModified(aEvent); } };
};

//---------------------------------------------------------------

#endif // HPLEDITOR_ENTITY_WRAPPER_ENTITY_H
