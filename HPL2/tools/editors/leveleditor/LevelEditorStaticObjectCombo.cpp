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

#include "LevelEditorStaticObjectCombo.h"
#include "LevelEditorActions.h"
#include "LevelEditorWorld.h"

#include "../common/EntityWrapperStaticObject.h"

#include "resources/XmlHelper.h"
#include <tinyxml2.h>

#include <algorithm>

//-----------------------------------------------------------------------

/////////////////////////////////////////////////////////////////////////
// CONSTRUCTORS
/////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

cLevelEditorStaticObjectCombo::cLevelEditorStaticObjectCombo(cLevelEditorWorld* apWorld, int alComboID)
{
	mpWorld = apWorld;
	mlComboID = alComboID;

	// The legacy flat-color GPU program is gone — combo highlighting draws
	// through DebugDraw::DebugSolidFromVertexBuffer with mColor instead.
	SetColor(cMath::RandRectColor(cColor(0,1), cColor(1,1)));
}

cLevelEditorStaticObjectCombo::~cLevelEditorStaticObjectCombo()
{
}

//-----------------------------------------------------------------------

/////////////////////////////////////////////////////////////////////////
// PUBLIC METHODS
/////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

bool cLevelEditorStaticObjectCombo::AddObject(iEntityWrapper* apObj)
{
	if(IsValidObject(apObj)==false || HasObject(apObj))
		return false;

	mlstEntities.push_back(apObj);

	cLevelEditorEntityExtData* pData = (cLevelEditorEntityExtData*)apObj->GetEntityExtData();
	pData->mlComboID = GetID();

	return true;
}

bool cLevelEditorStaticObjectCombo::RemoveObject(iEntityWrapper* apObj)
{
	if(IsValidObject(apObj)==false || HasObject(apObj)==false)
		return false;

	mlstEntities.remove(apObj);

	cLevelEditorEntityExtData* pData = (cLevelEditorEntityExtData*)apObj->GetEntityExtData();
	pData->mlComboID = -1;

	return true;
}

//-----------------------------------------------------------------------

bool cLevelEditorStaticObjectCombo::HasObject(iEntityWrapper* apObj)
{
	if(IsValidObject(apObj)==false)
		return false;

	return find(mlstEntities.begin(), mlstEntities.end(), apObj) != mlstEntities.end();
}

//-----------------------------------------------------------------------

void cLevelEditorStaticObjectCombo::SetColor(const cColor& aCol)
{
	if(mColor==aCol)
		return;

	mColor = aCol;
}

//-----------------------------------------------------------------------

void cLevelEditorStaticObjectCombo::Draw(cEditorWindowViewport* apViewport, DebugDraw* apFunctions)
{
	tEntityWrapperListIt it = mlstEntities.begin();
	for(;it!=mlstEntities.end();++it)
	{
		iEntityWrapper* pEnt = *it;
		pEnt->DrawProgram(apViewport, apFunctions, NULL, mColor);
	}
}

//-----------------------------------------------------------------------

bool cLevelEditorStaticObjectCombo::Load(tinyxml2::XMLElement* apElement)
{
	mlComboID = GetAttributeInt(apElement, "ID", mlComboID);
	mColor = GetAttributeColor(apElement, "Color", mColor);

	//////////////////////////////////////////
	// Load combined object ids
	tString sObjIds = GetAttributeString(apElement, "ObjIds");
	tIntVec vObjIds;
	cString::GetIntVec(sObjIds, vObjIds);

	for(int i=0;i<(int)vObjIds.size();++i)
	{
		int lID = vObjIds[i];
		iEntityWrapper* pObj = mpWorld->GetEntity(lID);

		AddObject((cEntityWrapperStaticObject*)pObj);
	}
	
	return true;
}

bool cLevelEditorStaticObjectCombo::Save(tinyxml2::XMLElement* apElement)
{
	tinyxml2::XMLElement* pData = apElement->GetDocument()->NewElement("Combo");
	apElement->InsertEndChild(pData);
	SetAttributeInt(pData, "ID", mlComboID);
	SetAttributeColor(pData, "Color", mColor);

	//////////////////////////////////////////
	// Save combined object ids
	tString sObjIds;
	tEntityWrapperList::const_iterator itObjs = mlstEntities.begin();
	for(;itObjs!=mlstEntities.end();++itObjs)
	{
		iEntityWrapper* pEnt = *itObjs;
		sObjIds += cString::ToString(pEnt->GetID()) + " ";
	}
	// Cut space at the end of the string
	sObjIds = cString::Sub(sObjIds, 0, (int)sObjIds.length()-1);

	SetAttributeString(pData, "ObjIds", sObjIds);

	return true;
}

//-----------------------------------------------------------------------

iEditorAction* cLevelEditorStaticObjectCombo::CreateActionAddObject(iEntityWrapper* apObj)
{
	if(IsValidObject(apObj)==false ||
		HasObject((cEntityWrapperStaticObject*)apObj))
		return NULL;


	iEditorAction* pAction = hplNew(cLevelEditorActionAddObjectToCombo, (mpWorld, this,(cEntityWrapperStaticObject*)  apObj));

	if(pAction->IsValidAction()==false)
	{
		hplDelete(pAction);
		pAction = NULL;
	}

	return pAction;
}

//-----------------------------------------------------------------------

iEditorAction* cLevelEditorStaticObjectCombo::CreateActionRemoveObject(iEntityWrapper* apObj)
{
	if(IsValidObject(apObj)==false || 
		HasObject(apObj)==false)
		return NULL;


	iEditorAction* pAction = hplNew(cLevelEditorActionRemoveObjectFromCombo, (mpWorld, this, (cEntityWrapperStaticObject*) apObj));

	if(pAction->IsValidAction()==false)
	{
		hplDelete(pAction);
		pAction = NULL;
	}

	return pAction;
}

//-----------------------------------------------------------------------

iEditorAction* cLevelEditorStaticObjectCombo::CreateActionSetColor(const cColor& aCol)
{
    iEditorAction* pAction = hplNew(cLevelEditorActionComboSetColor, (mpWorld, mlComboID, aCol));

	if(pAction->IsValidAction()==false)
	{
		hplDelete(pAction);
		pAction = NULL;
	}

	return pAction;
}

//-----------------------------------------------------------------------

//-----------------------------------------------------------------------

/////////////////////////////////////////////////////////////////////////
// PROTECTED METHODS
/////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

bool cLevelEditorStaticObjectCombo::IsValidObject(iEntityWrapper* apObj)
{
	return apObj!=NULL &&
			(apObj->GetTypeID()==eEditorEntityType_StaticObject ||
			apObj->GetTypeID()==eEditorEntityType_Primitive);
}


//-----------------------------------------------------------------------


