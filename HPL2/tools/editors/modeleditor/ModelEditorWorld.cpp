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

#include "ModelEditorWorld.h"

#include "../common/EditorBaseClasses.h"
#include "../common/EntityWrapperFactory.h"

#include "../common/EntityWrapperBodyShape.h"
#include "../common/EntityWrapperBody.h"
#include "../common/EntityWrapperJointBall.h"
#include "../common/EntityWrapperJointHinge.h"
#include "../common/EntityWrapperJointScrew.h"
#include "../common/EntityWrapperJointSlider.h"
#include "../common/EntityWrapperLightPoint.h"
#include "../common/EntityWrapperLightSpot.h"
#include "../common/EntityWrapperBillboard.h"
#include "../common/EntityWrapperParticleSystem.h"
#include "../common/EntityWrapperSound.h"
#include "../common/EntityWrapperSubMesh.h"
#include "../common/EntityWrapperBone.h"

#include "../common/EditorHelper.h"

#include "../common/EditorUserClassDefinitionManager.h"

#include <tinyxml2.h>
#include "resources/XmlHelper.h"

#include <algorithm>

int cAnimationEventWrapper::mlIndices = 0;
//--------------------------------------------------------------------

cAnimationEventWrapper::cAnimationEventWrapper()
{
	mlIndex = mlIndices++;
}

void cAnimationEventWrapper::Load(tinyxml2::XMLElement* apElement)
{
	SetTime(GetAttributeFloat(apElement, "Time"));
	SetType(GetAttributeString(apElement, "Type"));
	SetValue(GetAttributeString(apElement, "Value"));
}

void cAnimationEventWrapper::Save(tinyxml2::XMLElement* apElement)
{
	SetAttributeFloat(apElement, "Time",GetTime());
	SetAttributeString(apElement, "Type",GetType());
	SetAttributeString(apElement, "Value",GetValue());
}

bool cAnimationEventWrapper::IsValid()
{
	if(msType=="") return false;
	if(msValue=="" && msType!="Step") return false;

	return true;
}

//--------------------------------------------------------------------

cAnimationWrapper::cAnimationWrapper()
{
	mfSpeed = 1;
}

//--------------------------------------------------------------------

bool cAnimationWrapper::IsValid()
{
	if(msFile!="" && msName!="")
	{
		for(int i=0;i<(int)mvEvents.size();++i)
		{
			cAnimationEventWrapper& event = mvEvents[i];
			if(event.IsValid()==false)
				return false;
		}
	}
	else
		return false;
	
	return true;
}

void cAnimationWrapper::Load(tinyxml2::XMLElement* apElement)
{
	SetName(GetAttributeString(apElement, "Name"));
	SetFile(GetAttributeString(apElement, "File"));
	SetSpeed(GetAttributeFloat(apElement, "Speed"));
	SetSpecialEventTime(GetAttributeFloat(apElement, "SpecialEventTime"));

	for(tinyxml2::XMLElement* pXmlEvent = apElement->FirstChildElement(); pXmlEvent != NULL; pXmlEvent = pXmlEvent->NextSiblingElement())
	{
		cAnimationEventWrapper event;
		event.Load(pXmlEvent);
		mvEvents.push_back(event);
	}
}

void cAnimationWrapper::Save(tinyxml2::XMLElement* apElement)
{
	SetAttributeString(apElement, "Name",GetName());
	SetAttributeString(apElement, "File",GetFile());
	SetAttributeFloat(apElement, "Speed",GetSpeed());
	SetAttributeFloat(apElement, "SpecialEventTime",GetSpecialEventTime());
	for(int i=0;i<(int)mvEvents.size();++i)
	{
		cAnimationEventWrapper* pEvent = &mvEvents[i];
		tinyxml2::XMLElement* pXmlEvent = apElement->GetDocument()->NewElement("Event");
		apElement->InsertEndChild(pXmlEvent);
		pEvent->Save(pXmlEvent);
	}
}

//--------------------------------------------------------------------

int cAnimationWrapper::AddEvent()
{
	mvEvents.push_back(cAnimationEventWrapper());
	return (int)mvEvents.size();
}

void cAnimationWrapper::RemoveEvent(int alIdx)
{
	cAnimationEventWrapper* event = &mvEvents[alIdx];
	std::vector<cAnimationEventWrapper> vTemp;
	for(int i=0;i<(int)mvEvents.size();++i)
		if(mvEvents[i].GetIndex()!=event->GetIndex())
			vTemp.push_back(mvEvents[i]);

	mvEvents = vTemp;
}
//--------------------------------------------------------------------

void cAnimationWrapper::ClearEvents()
{
	mvEvents.clear();
}


//------------------------------------------------------------------

///////////////////////////////////////////////////////////////////
// CONSTRUCTORS
///////////////////////////////////////////////////////////////////

//------------------------------------------------------------------

cModelEditorWorld::cModelEditorWorld(iEditorBase* apEditor) : iEditorWorld(apEditor, "Entity")
{
	AddEntityCategory("Entities", -1);
	AddEntityCategory("Mesh", eEditorEntityType_SubMesh);
	AddEntityCategory("Bones", eEditorEntityType_Bone);
	AddEntityCategory("Shapes", eEditorEntityType_BodyShape);
	AddEntityCategory("Bodies", eEditorEntityType_Body);
	AddEntityCategory("Joints", eEditorEntityType_Joint);
	AddEntityCategory("Animations", -1);
	
	mpTypeSubMesh = hplNew(cEntityWrapperTypeSubMesh,());
	mpTypeBone	  = hplNew(cEntityWrapperTypeBone,());
	AddEntityType(mpTypeSubMesh);
	AddEntityType(mpTypeBone);

	mpClass = NULL;
}

//------------------------------------------------------------------

cModelEditorWorld::~cModelEditorWorld()
{
	SetType(NULL, false);
}

//------------------------------------------------------------------

///////////////////////////////////////////////////////////////////
// PUBLIC METHODS
///////////////////////////////////////////////////////////////////

//------------------------------------------------------------------

void cModelEditorWorld::Reset()
{
	mpTypeSubMesh->ClearMesh();

	mvAnimations.clear();
	
	/////////////////////////////////////////
	// Reset user defined variable values
	mmapTempValues.clear();
	cEditorUserClassDefinition* pDef = mpEditor->GetClassDefinitionManager()->GetDefinition(eUserClassDefinition_Entity);
	SetType(pDef->GetType(0)->GetSubType(0), false);

	iEditorWorld::Reset();
}

//------------------------------------------------------------------

cMeshEntity* cModelEditorWorld::GetMesh()
{
	return mpTypeSubMesh->GetMesh();
}

tString cModelEditorWorld::GetMeshFilename()
{
	return mpTypeSubMesh->GetMeshFilename();
}

void cModelEditorWorld::SetMeshFromElement(tinyxml2::XMLElement* apMeshElement, tinyxml2::XMLElement* apBonesElement)
{
	tEntityDataVec vSubMeshData, vBoneData;

	////////////////////////////////////////////////////////////
	// Get submesh data from the .ent file
	for(tinyxml2::XMLElement* pSubMesh = apMeshElement->FirstChildElement(); pSubMesh != NULL; pSubMesh = pSubMesh->NextSiblingElement())
	{
		iEntityWrapperData* pData = mpTypeSubMesh->CreateData();
		pData->Load(pSubMesh);

		vSubMeshData.push_back(pData);
	}

	////////////////////////////////////////////////////////////
	// Get bone data from the .ent file
	if(apBonesElement)
	{
		for(tinyxml2::XMLElement* pBone = apBonesElement->FirstChildElement(); pBone != NULL; pBone = pBone->NextSiblingElement())
		{
			iEntityWrapperData* pData = mpTypeBone->CreateData();
			pData->Load(pBone);

			vBoneData.push_back(pData);
		}
	}

	////////////////////////////////////////////////////////////
	// Load mesh using the submesh and bone data loaded above
	// A comparison will take place and updates to data will come if necessary
	mpTypeSubMesh->SetMesh(GetAttributeString(apMeshElement, "Filename"), true,
							vSubMeshData, tIntList(), vBoneData, tIntList());
}

//------------------------------------------------------------------

void cModelEditorWorld::SetAnimations(const tAnimWrapperVec& avAnims)
{
	IncModifications();
	mvAnimations = avAnims;
}

//------------------------------------------------------------------

void cModelEditorWorld::SetType(cEditorUserClassSubType* apType, bool abKeepValues)
{
	if(mpClass && mpClass->GetClass()==apType)
		return;

	if(mpClass)
	{
		if(abKeepValues)
			mpClass->SaveValuesToMap(mmapTempValues);

		hplDelete(mpClass);
		mpClass = NULL;
	}

	if(apType)
	{
		mpClass = apType->CreateInstance(eEditorVarCategory_Type);
		if(abKeepValues)
			mpClass->LoadValuesFromMap(mmapTempValues);
	}
}

//------------------------------------------------------------------

void cModelEditorWorld::LoadWorldData(tinyxml2::XMLElement* apWorldDataElement)
{
	iEditorWorld::LoadWorldData(apWorldDataElement);

	tinyxml2::XMLElement* pXmlVariables = apWorldDataElement->Parent()->ToElement()->FirstChildElement("UserDefinedVariables");
	if(pXmlVariables)
	{
		cEditorUserClassDefinition* pDef = mpEditor->GetClassDefinitionManager()->GetDefinition(eUserClassDefinition_Entity);

		tString sType = GetAttributeString(pXmlVariables, "EntityType");
		tString sSubType = GetAttributeString(pXmlVariables, "EntitySubType");

		bool bValid = false;
		cEditorUserClassType* pBaseType = pDef->GetType(sType);
		if (pBaseType)
		{
			bValid = true;
			cEditorUserClassSubType* pType = pBaseType->GetSubType(sSubType);
			SetType(pType);

			if(mpClass) mpClass->Load(pXmlVariables);
			else		bValid = false;
		}

		if (!bValid)
		{
			tString sMessage = "Model has invalid type : " + sType + " - " + sSubType;
			Error("%s\n", sMessage.c_str());
			mpEditor->ShowMessageBox(_W("Error"), cString::To16Char(sMessage), _W("OK"), _W(""), NULL, NULL);
		}
	}
}

//------------------------------------------------------------------

tinyxml2::XMLElement* cModelEditorWorld::GetWorldDataElement(tinyxml2::XMLElement* apXmlDoc)
{
	return apXmlDoc->FirstChildElement("ModelData");
}

//------------------------------------------------------------------

tinyxml2::XMLElement* cModelEditorWorld::GetWorldObjectsElement(tinyxml2::XMLElement* apWorldDataElement)
{
	return apWorldDataElement;
}

//------------------------------------------------------------------

bool cModelEditorWorld::CustomCategorySaver(tinyxml2::XMLElement* apWorldObjectsElement)
{
	tinyxml2::XMLElement* pMeshElem = apWorldObjectsElement->FirstChildElement("Mesh");
	SetAttributeString(pMeshElem, "Filename",
		cString::To8Char(mpEditor->GetPathRelToWD(cString::To16Char(mpTypeSubMesh->GetMeshFilename()))));

	tinyxml2::XMLElement* pAnimElem = apWorldObjectsElement->FirstChildElement("Animations");
	{
		for(int i=0;i<(int)mvAnimations.size();++i)
		{
			cAnimationWrapper& pAnim = mvAnimations[i];
			tinyxml2::XMLElement* pXmlAnim = pAnimElem->GetDocument()->NewElement("Animation");
			pAnimElem->InsertEndChild(pXmlAnim);
			pAnim.Save(pXmlAnim);
		}
	}


	return true;
}

bool cModelEditorWorld::CustomCategoryLoader(tinyxml2::XMLElement* apWorldObjectsElement, tinyxml2::XMLElement* apCategoryElement)
{
	if(apCategoryElement==NULL)
		return false;

	const tString sCatName = apCategoryElement->Value();
	if(sCatName=="Mesh")
	{
		SetMeshFromElement(apCategoryElement, apWorldObjectsElement->FirstChildElement("Bones"));

		return true;
	}
	else if(sCatName=="Bones")
	{
		return true;
	}
	else if(sCatName=="Animations")
	{
		for(tinyxml2::XMLElement* pElement = apCategoryElement->FirstChildElement(); pElement != NULL; pElement = pElement->NextSiblingElement())
		{
			cAnimationWrapper animation;
			animation.Load(pElement);
			mvAnimations.push_back(animation);
		}

		return true;
	}

	return false;
}

//------------------------------------------------------------------

tinyxml2::XMLElement* cModelEditorWorld::CreateWorldDataElement(tinyxml2::XMLElement* apXmlDoc)
{
	tinyxml2::XMLElement* pModelData = apXmlDoc->GetDocument()->NewElement("ModelData");
	apXmlDoc->InsertEndChild(pModelData);
	return pModelData;
}

//------------------------------------------------------------------

tinyxml2::XMLElement* cModelEditorWorld::CreateWorldObjectsElement(tinyxml2::XMLElement* apWorldDataElement)
{
	return apWorldDataElement;
}

//------------------------------------------------------------------

void cModelEditorWorld::SaveWorldData(tinyxml2::XMLElement* apWorldDataElement)
{
	cEditorUserClassSubType* pSubType = (cEditorUserClassSubType*)mpClass->GetClass();
	cEditorUserClassType* pType = pSubType->GetParent();

	tinyxml2::XMLElement* pXmlUserVars = apWorldDataElement->Parent()->ToElement()->GetDocument()->NewElement("UserDefinedVariables");
	apWorldDataElement->Parent()->ToElement()->InsertEndChild(pXmlUserVars);
	SetAttributeString(pXmlUserVars, "EntityType", pType->GetName());
	SetAttributeString(pXmlUserVars, "EntitySubType", pSubType->GetName());

	mpClass->Save(pXmlUserVars);
}

//------------------------------------------------------------------
