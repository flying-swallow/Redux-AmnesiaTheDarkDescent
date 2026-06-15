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

#include "EditorUserClassDefinitionManager.h"
#include "EditorWindow.h"

#include <tinyxml2.h>
#include "resources/XmlHelper.h"

//------------------------------------------------------------------------------

////////////////////////////////////////////
// Static helpers

static eEditorVarCategory IdxToCat(int alX)
{
	return (eEditorVarCategory)cMath::Pow2(alX);
}

static int CatToIdx(eEditorVarCategory aCat)
{
	return cMath::Log2ToInt(aCat);
}

//------------------------------------------------------------------------------

cEditorVarGroup::cEditorVarGroup(const tString& asName)
{
	msName = asName;
}

//------------------------------------------------------------------------------

///////////////////////////////////////////////////////////////////////////
// USER CLASS
///////////////////////////////////////////////////////////////////////////

//------------------------------------------------------------------------------

cEditorUserClass::cEditorUserClass(cEditorUserClassDefinition* apDefinition)
{
	mpDefinition = apDefinition;
	mvVariables.resize(eEditorVarCategory_LastEnum);
	mlIndex = 0;
}

//------------------------------------------------------------------------------

cEditorUserClass::~cEditorUserClass()
{
	for(int i=0;i<eEditorVarCategory_LastEnum;++i)
		STLDeleteAll(mvVariables[i]);
}

//------------------------------------------------------------------------------

bool cEditorUserClass::Create(void* apData)
{
	if(apData)
	{
		tinyxml2::XMLElement* pElement = (tinyxml2::XMLElement*)apData;
        msName = GetAttributeString(pElement, "Name");

		for(int i=0;i<eEditorVarCategory_LastEnum;++i)
		{
			eEditorVarCategory cat = IdxToCat(i);
			if(mpDefinition->IsLoadableCategory(cat))
			{
				const tString& sCatName = mpDefinition->GetManager()->GetVarCategoryName(cat);
				tinyxml2::XMLElement* pVarCategory = pElement->FirstChildElement(sCatName.c_str());
				if(pVarCategory==NULL)
					continue;

				AddVariablesFromElement(this, mvVariables[i], pVarCategory);
			}
		}

		//////////////////////////////////////////////////////////////
		// Area compatibility
		{
			tinyxml2::XMLElement* pVarCategory = pElement->FirstChildElement("Vars");
			if(pVarCategory==NULL)
				return true;

			AddVariablesFromElement(this, mvVariables[cMath::Log2ToInt(eEditorVarCategory_Instance)], pVarCategory);
		}
	}

	return true;
}

//------------------------------------------------------------------------------

iEditorVar* cEditorUserClass::CreateClassSpecificVariableFromElement(tinyxml2::XMLElement* apElement)
{
	if(apElement==NULL)
		return NULL;

	tString sGroupName = "Uncategorized";

	if(tString(apElement->Value())=="Group")
	{
		sGroupName = GetAttributeString(apElement, "Name");

		tEditorVarVec vTempVars;
		AddVariablesFromElement(this, vTempVars, apElement);
		if(vTempVars.empty()==false)
		{
			cEditorVarGroup* pGroup = mpDefinition->GetGroup(sGroupName);
			for(int i=0;i<(int)vTempVars.size();++i)
			{
				iEditorVar* pVar = vTempVars[i];
				pVar->SetExtData(pGroup);
			}
			int lIdx = CatToIdx(eEditorVarCategory_Type);
			mvVariables[lIdx].insert(mvVariables[lIdx].end(), vTempVars.begin(), vTempVars.end());
		}
	}
	
	return CreateVariableFromElement(apElement);
}

//------------------------------------------------------------------------------

iEditorVar* cEditorUserClass::GetVariable(eEditorVarCategory aCat, const tWString& asName)
{
	int lIdx = CatToIdx(aCat);
	tEditorVarVec& vVars = mvVariables[lIdx];
	for(int i=0;i<(int)vVars.size();++i)
	{
		iEditorVar* pVar = vVars[i];
		if(pVar->GetName()==asName)
			return pVar;
	}

	return NULL;
}

//------------------------------------------------------------------------------

void cEditorUserClass::AddVariablesToInstance(eEditorVarCategory aCat, cEditorUserClassInstance* apInstance)
{
	int lIdx = CatToIdx(aCat);
	tEditorVarVec& vVars = mvVariables[lIdx];
	for(int i=0;i<(int)vVars.size();++i)
	{
		iEditorVar* pVar = vVars[i];
		cEditorVarInstance* pVarInstance = pVar->CreateInstance();
		
		apInstance->AddVarInstance(pVarInstance);
	}
}

//------------------------------------------------------------------------------

tEditorVarVec cEditorUserClass::GetEditorSetupVars()
{
	tEditorVarVec vEditorSetupVars;
	DumpEditorSetupVars(vEditorSetupVars);

	return vEditorSetupVars;
}

void cEditorUserClass::DumpEditorSetupVars(tEditorVarVec& avVars)
{
	int lIdx = CatToIdx(eEditorVarCategory_EditorSetup);
	avVars.insert(avVars.end(), mvVariables[lIdx].begin(), mvVariables[lIdx].end());
}

//------------------------------------------------------------------------------

///////////////////////////////////////////////////////////////////////////
// USER CLASS BASE
///////////////////////////////////////////////////////////////////////////

//------------------------------------------------------------------------------

cEditorUserClassBase::cEditorUserClassBase(cEditorUserClassDefinition* apDefinition) : cEditorUserClass(apDefinition)
{
}

//------------------------------------------------------------------------------

///////////////////////////////////////////////////////////////////////////
// USER CLASS TYPE
///////////////////////////////////////////////////////////////////////////

//------------------------------------------------------------------------------

cEditorUserClassType::cEditorUserClassType(cEditorUserClassDefinition* apDefinition) : cEditorUserClass(apDefinition)
{
}

//------------------------------------------------------------------------------

cEditorUserClassType::~cEditorUserClassType()
{
	STLDeleteAll(mvSubTypes);
}

//------------------------------------------------------------------------------

bool cEditorUserClassType::Create(void* apData)
{
	if(cEditorUserClass::Create(apData)==false)
		return false;

	tinyxml2::XMLElement* pElement = (tinyxml2::XMLElement*)apData;

	tString sBaseClassName = GetAttributeString(pElement, "BaseClass");
	mpBaseClass = mpDefinition->GetBaseClass(sBaseClassName);

	tString sDefaultSubType = GetAttributeString(pElement, "Default");
	mlDefaultSubType = 0;

	tinyxml2::XMLElement* pSubTypes = pElement->FirstChildElement("SubTypes");
	if(pSubTypes)
	{
		int i=0;
		for(tinyxml2::XMLElement* pSubTypeData = pSubTypes->FirstChildElement(); pSubTypeData != NULL; pSubTypeData = pSubTypeData->NextSiblingElement())
		{
			cEditorUserClassSubType* pSubType = hplNew(cEditorUserClassSubType, (this));
			if(pSubType->Create(pSubTypeData)==false)
			{
				hplDelete(pSubType);
				return false;
			}
			pSubType->SetIndex(i++);
			mvSubTypes.push_back(pSubType);

			//////////////////////////////////
			// Set default subtype index
			if(pSubType->GetName()==sDefaultSubType)
				mlDefaultSubType = (int)mvSubTypes.size()-1;
		}
	}
	else
	{
		cEditorUserClassSubType* pSubType = hplNew(cEditorUserClassSubType, (this));
		mvSubTypes.push_back(pSubType);
	}

	return true;
}

//------------------------------------------------------------------------------

void cEditorUserClassType::AddVariablesToInstance(eEditorVarCategory aCat, cEditorUserClassInstance* apInstance)
{
	if(mpBaseClass)
		((cEditorUserClassBase*)mpBaseClass)->AddVariablesToInstance(aCat, apInstance);

	cEditorUserClass::AddVariablesToInstance(aCat, apInstance);
}

//------------------------------------------------------------------------------

void cEditorUserClassType::DumpEditorSetupVars(tEditorVarVec& avVars)
{
	if(mpBaseClass) ((cEditorUserClassBase*)mpBaseClass)->DumpEditorSetupVars(avVars);

	cEditorUserClass::DumpEditorSetupVars(avVars);
}

//------------------------------------------------------------------------------

iEditorVar* cEditorUserClassType::GetVariable(eEditorVarCategory aCat, const tWString& asName)
{
	iEditorVar* pVar = cEditorUserClass::GetVariable(aCat, asName);
	if(pVar)
		return pVar;

	if(mpBaseClass) 
		return ((cEditorUserClassBase*)mpBaseClass)->GetVariable(aCat, asName);

	return NULL;
}

//------------------------------------------------------------------------------

int cEditorUserClassType::GetSubTypeNum()
{
	return (int)mvSubTypes.size();
}

//------------------------------------------------------------------------------

///////////////////////////////////////////////////////////////////////////
// USER CLASS SUBTYPE
///////////////////////////////////////////////////////////////////////////

//------------------------------------------------------------------------------

cEditorUserClassSubType* cEditorUserClassType::GetSubType(int alX)
{
	return (cEditorUserClassSubType*)GetClassByIdx(mvSubTypes, alX);
}

//------------------------------------------------------------------------------

cEditorUserClassSubType* cEditorUserClassType::GetSubType(const tString& asName)
{
	cEditorUserClassSubType* pSubType = (cEditorUserClassSubType*)mvSubTypes.front();
	if(mvSubTypes.size()>1)
		pSubType = (cEditorUserClassSubType*)GetClassByName(mvSubTypes, asName);

	return pSubType;
}

//------------------------------------------------------------------------------

cEditorUserClassSubType::cEditorUserClassSubType(cEditorUserClassType* apParent) : cEditorUserClass(apParent->GetDefinition())
{
	mpParent = apParent;
}

//------------------------------------------------------------------------------

cEditorClassInstance* cEditorUserClassSubType::CreateInstance(eEditorVarCategory aCat)
{
	cEditorUserClassInstance* pInstance = hplNew(cEditorUserClassInstance,(this, aCat));

	mpParent->AddVariablesToInstance(aCat,pInstance);
	cEditorUserClass::AddVariablesToInstance(aCat, pInstance);

	return pInstance;
}

//------------------------------------------------------------------------------

void cEditorUserClassSubType::DumpEditorSetupVars(tEditorVarVec& avVars)
{
	mpParent->DumpEditorSetupVars(avVars);
	cEditorUserClass::DumpEditorSetupVars(avVars);
}

iEditorVar* cEditorUserClassSubType::GetVariable(eEditorVarCategory aCat, const tWString& asName)
{
	iEditorVar* pVar = cEditorUserClass::GetVariable(aCat, asName);
	if(pVar)
		return pVar;

	return mpParent->GetVariable(aCat, asName);
}

//------------------------------------------------------------------------------

bool cEditorUserClassSubType::IsDefaultSubType()
{
	return mpParent->GetSubType(mpParent->GetDefaultSubTypeIndex())==this;
}

//------------------------------------------------------------------------------

///////////////////////////////////////////////////////////////////////////
// USER CLASS INSTANCE
///////////////////////////////////////////////////////////////////////////

//------------------------------------------------------------------------------

cEditorUserClassInstance::cEditorUserClassInstance(cEditorUserClassSubType* apClass, eEditorVarCategory aCat) : cEditorClassInstance(apClass)
{
	mCat = aCat;
}

//------------------------------------------------------------------------------

cEditorClassInstance* cEditorUserClassInstance::CreateSpecificCopy()
{
	return ((cEditorUserClassSubType*)mpClass)->CreateInstance(mCat);
}

//------------------------------------------------------------------------------

cEditorVarInputPanel* cEditorUserClassInstance::CreateInputPanel(iEditorWindow* apWindow, iWidget* apParent, bool abRows)
{
	cEditorVarInputPanel* pPanel = NULL;
	if(mCat==eEditorVarCategory_Type)
		pPanel = hplNew(cEditorUserClassInputPanel,(this));
	else
		pPanel = hplNew(cEditorVarInputPanel,(this));

	pPanel->SetDeployInputsOnRows(abRows);

	pPanel->Create(apWindow, apParent);

	return pPanel;
}

//------------------------------------------------------------------------------

///////////////////////////////////////////////////////////////////////////
// USER CLASS INPUT PANEL
///////////////////////////////////////////////////////////////////////////

//------------------------------------------------------------------------------

cEditorUserClassInputPanel::cEditorUserClassInputPanel(cEditorUserClassInstance* apInstance) : cEditorVarInputPanel(apInstance)
{
}

//------------------------------------------------------------------------------

void cEditorUserClassInputPanel::Create(iEditorWindow* apWindow, iWidget* apWidget)
{
	cGuiSet* pSet = apWindow->GetSet();
	cEditorUserClassInstance* pClass = (cEditorUserClassInstance*)mpClass;

	if(pClass->GetCategory()==eEditorVarCategory_Type)
	{
		cWidgetTabFrame* pTabFrame = pSet->CreateWidgetTabFrame(0,apWidget->GetSize()-4, _W(""), apWidget);

		cEditorUserClassInstance* pClass = (cEditorUserClassInstance*)mpClass;
		tVector3fVec vPositions;

		for(int i=0;i<pClass->GetVarInstanceNum();++i)
		{
			cWidgetTab* pTab = NULL;
			tWString sGroupName = _W("Uncategorized");

			cEditorVarInstance* pVar = pClass->GetVarInstance(i);
			iEditorVar* pVarType = pVar->GetVarType();
			cEditorVarGroup* pGroup = (cEditorVarGroup*)pVarType->GetExtData();
			if(pGroup)
				sGroupName = cString::To16Char(pGroup->GetName());

			pTab = pTabFrame->GetTab(sGroupName);
			if(pTab==NULL)
			{
				pTab = pTabFrame->AddTab(sGroupName);
				vPositions.push_back(cVector3f(10,10,0.1f));
			}

			iEditorVarInput* pInput = pVar->CreateInput(apWindow, pTab);
			pInput->SetPanel(this);
			pInput->GetInput()->SetPosition(vPositions[pTab->GetIndex()]);
			pInput->GetInput()->SetTabWidth(0.4f*pTab->GetSize().x);
			pInput->GetInput()->UpdateLayout();

			mvInputs.push_back(pInput);

			vPositions[pTab->GetIndex()].y += pInput->GetInput()->GetSize().y + 5;
		}

		mpHandle = pTabFrame;
	}
	else
	{
		cEditorVarInputPanel::Create(apWindow, apWidget);
	}
}


//------------------------------------------------------------------------------

///////////////////////////////////////////////////////////////////////////
// USER CLASS DEFINITION
///////////////////////////////////////////////////////////////////////////

//------------------------------------------------------------------------------

cEditorUserClassDefinition::cEditorUserClassDefinition(cEditorUserClassDefinitionManager* apManager)
{
	mpManager = apManager;
	mlLoadFlags = 0x00000007;
}

//------------------------------------------------------------------------------

cEditorUserClassDefinition::~cEditorUserClassDefinition()
{
	STLDeleteAll(mvBaseClasses);
	STLDeleteAll(mvTypes);
	STLDeleteAll(mvGroups);
}

//------------------------------------------------------------------------------

bool cEditorUserClassDefinition::Create(const tString& asFilename, int alLoadFlags)
{
	mlLoadFlags = alLoadFlags;

	cResources* pRes = mpManager->GetEditor()->GetEngine()->GetResources();
	tinyxml2::XMLElement* pDoc = pRes->LoadXmlDocument(asFilename);

	////////////////////////////////////////////
	// Check XML validity
	if(pDoc==NULL ||
		pDoc->FirstChildElement("Types")==NULL)
	{
		tString sError = (pDoc==NULL)?"file not found":"error found in file";
		pRes->DestroyXmlDocument(pDoc);

		FatalError("Failed compilation of Custom Variable definition %s: %s\n",
					cString::GetFileName(asFilename).c_str(),
					sError.c_str());

		return false;
	}

	bool bSuccessfullyCreated = true;

	////////////////////////////////////////////
	// Create Base Classes
	tinyxml2::XMLElement* pBaseClasses = pDoc->FirstChildElement("BaseClasses");
	if(pBaseClasses)
	{
		int i=0;
		for(tinyxml2::XMLElement* pXmlBaseClass = pBaseClasses->FirstChildElement(); pXmlBaseClass != NULL; pXmlBaseClass = pXmlBaseClass->NextSiblingElement())
		{
			cEditorUserClassBase* pBaseClass = hplNew(cEditorUserClassBase, (this));
			if(pBaseClass->Create(pXmlBaseClass)==false)
			{
				bSuccessfullyCreated = false;
				hplDelete(pBaseClass);
				break;
			}

			pBaseClass->SetIndex(i++);
			mvBaseClasses.push_back(pBaseClass);
		}
	}

	////////////////////////////////////////////
	// Create Types
	tinyxml2::XMLElement* pTypes = pDoc->FirstChildElement("Types");
	if(pTypes)
	{
		int i=0;
		for(tinyxml2::XMLElement* pXmlType = pTypes->FirstChildElement(); pXmlType != NULL; pXmlType = pXmlType->NextSiblingElement())
		{
			cEditorUserClassType* pType = hplNew(cEditorUserClassType, (this));
			if(pType->Create(pXmlType)==false)
			{
				bSuccessfullyCreated = false;
				hplDelete(pType);
				break;
			}
			pType->SetIndex(i++);
			mvTypes.push_back(pType);
		}
	}

	pRes->DestroyXmlDocument(pDoc);

	return bSuccessfullyCreated;
}

//------------------------------------------------------------------------------

bool cEditorUserClassDefinition::IsLoadableCategory(eEditorVarCategory aCat)
{
	return (mlLoadFlags & aCat)!=0;
}

//------------------------------------------------------------------------------

cEditorUserClassBase* cEditorUserClassDefinition::GetBaseClass(const tString& asName)
{
	return (cEditorUserClassBase*)iEditorClass::GetClassByName(mvBaseClasses, asName);
}

//------------------------------------------------------------------------------


//------------------------------------------------------------------------------

int cEditorUserClassDefinition::GetTypeNum()
{
	return (int)mvTypes.size();
}

//------------------------------------------------------------------------------

cEditorUserClassType* cEditorUserClassDefinition::GetType(int alX)
{
	return (cEditorUserClassType*)iEditorClass::GetClassByIdx(mvTypes, alX);
}

//------------------------------------------------------------------------------

cEditorUserClassType* cEditorUserClassDefinition::GetType(const tString& asName)
{
	return (cEditorUserClassType*)iEditorClass::GetClassByName(mvTypes, asName);
}


//------------------------------------------------------------------------------

int cEditorUserClassDefinition::GetGroupNum()
{
	return (int)mvGroups.size();
}

//------------------------------------------------------------------------------

cEditorVarGroup* cEditorUserClassDefinition::GetGroup(int alIdx)
{
	if(alIdx<0 || alIdx>GetGroupNum())
		return NULL;

	return mvGroups[alIdx];
}

//------------------------------------------------------------------------------

cEditorVarGroup* cEditorUserClassDefinition::GetGroup(const tString& asName)
{
	cEditorVarGroup* pGroup = (cEditorVarGroup*) STLFindByName(mvGroups, asName);

	if(pGroup==NULL)
	{
		pGroup = hplNew(cEditorVarGroup,(asName));
		mvGroups.push_back(pGroup);
	}

	return pGroup;
}

//------------------------------------------------------------------------------

///////////////////////////////////////////////////////////////////////////
// USER CLASS DEFINITION MANAGER
///////////////////////////////////////////////////////////////////////////

//------------------------------------------------------------------------------

cEditorUserClassDefinitionManager::cEditorUserClassDefinitionManager(iEditorBase* apEditor)
{
	mpEditor = apEditor;

	mvVarCategoryNames.resize(eEditorVarCategory_LastEnum);
	mvVarCategoryNames[CatToIdx(eEditorVarCategory_Type)] = "TypeVars";
	mvVarCategoryNames[CatToIdx(eEditorVarCategory_Instance)] = "InstanceVars";
	mvVarCategoryNames[CatToIdx(eEditorVarCategory_EditorSetup)] = "EditorSetupVars";
}

//------------------------------------------------------------------------------

cEditorUserClassDefinitionManager::~cEditorUserClassDefinitionManager()
{
	STLDeleteAll(mvDefinitions);
}

//------------------------------------------------------------------------------

void cEditorUserClassDefinitionManager::RegisterDefFilename(int alIndex, const tString& asName, int alLoadFlags)
{
	mvDefIndices.push_back(alIndex);
	mvDefFilenames.push_back(asName);
	mvLoadFlags.push_back(alLoadFlags);
}

//------------------------------------------------------------------------------

void cEditorUserClassDefinitionManager::Init()
{
	for(int i=0;i<(int)mvDefFilenames.size();++i)
	{
		int lDefIndex = mvDefIndices[i];
		const tString& sDefFilename = mvDefFilenames[i];
		int lLoadFlags = mvLoadFlags[i];

		cEditorUserClassDefinition* pDef = hplNew(cEditorUserClassDefinition,(this));
		if(pDef->Create(sDefFilename,lLoadFlags))
		{
			mmapIndices[lDefIndex] = (int)mvDefinitions.size();
			mvDefinitions.push_back(pDef);
		}
		else
			hplDelete(pDef);
	}
}

//------------------------------------------------------------------------------

cEditorUserClassDefinition* cEditorUserClassDefinitionManager::GetDefinition(int alX)
{
	int lIndex=-1;
	std::map<int, int>::iterator it = mmapIndices.find(alX);
	if(it!=mmapIndices.end())
		lIndex = it->second;

	if(lIndex<0 || lIndex>=(int)mvDefinitions.size())
		return NULL;
	
	return mvDefinitions[lIndex];
}

//------------------------------------------------------------------------------

const tString& cEditorUserClassDefinitionManager::GetVarCategoryName(eEditorVarCategory aCat)
{
	return mvVarCategoryNames[CatToIdx(aCat)];
}

//------------------------------------------------------------------------------

