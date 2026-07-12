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

#include "EditorWindowEntityEditBoxEntity.h"

#include "EntityWrapperEntity.h"

#include "EditorEditModeSelect.h"

//----------------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////////
// CONSTRUCTORS
//////////////////////////////////////////////////////////////////////////////

//----------------------------------------------------------------------------

cEditorWindowEntityEditBoxEntity::cEditorWindowEntityEditBoxEntity(cEditorEditModeSelect* apEditMode, cEntityWrapperEntity* apObject) : cEditorWindowEntityEditBoxUserDefinedEntity(apEditMode, apObject)
{
	mpEntity = apObject;
	mpBOpenModelEditor = NULL;
}

//----------------------------------------------------------------------------

cEditorWindowEntityEditBoxEntity::~cEditorWindowEntityEditBoxEntity()
{
}

//----------------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////////
// PUBLIC METHODS
//////////////////////////////////////////////////////////////////////////////

//----------------------------------------------------------------------------

void cEditorWindowEntityEditBoxEntity::Create()
{
	mpTabGeneral = mpTabs->AddTab(_W("General"));
	mpTabEntity = mpTabs->AddTab(_W("Entity"));

	////////////////////////////////////////
	// Properties in Tab 'General'
	AddPropertyName(mpTabGeneral);
	AddPropertyActive(mpTabGeneral);
	AddPropertyPosition(mpTabGeneral);
	AddPropertyRotation(mpTabGeneral);
	AddPropertyScale(mpTabGeneral);

	cVector3f vPos = cVector3f(10,10,0.1f);
	mpInpName->SetPosition(vPos);
	vPos.y += mpInpName->GetSize().y+5;
	mpInpActive->SetPosition(vPos);
	vPos.y += mpInpActive->GetSize().y+5;
	mpInpPosition->SetPosition(vPos);
	vPos.y += mpInpPosition->GetSize().y+5;
	mpInpRotation->SetPosition(vPos);
	vPos.y += mpInpRotation->GetSize().y+5;
	mpInpScale->SetPosition(vPos);
	vPos.y += mpInpScale->GetSize().y+5;

	mpInpFile = CreateInputFile(vPos, _W("Entity File"), "", mpTabGeneral, 100);
	mpInpFile->SetBrowserType(eEditorResourceType_EntityFile);
	vPos.y += mpInpFile->GetSize().y+10;

	////////////////////////////////////////
	// "Edit in Model Editor" button (only where launching is supported, i.e.
	// the LevelEditor). Opens this entity's .ent in the standalone ModelEditor;
	// the file watcher / MCP reload then updates the placed instances live.
	if(mpEditor->SupportsModelEditorLaunch())
	{
		mpBOpenModelEditor = mpSet->CreateWidgetButton(vPos, cVector2f(160,25), _W("Edit in Model Editor"), mpTabGeneral);
		mpBOpenModelEditor->AddCallback(eGuiMessage_ButtonPressed, this, kGuiCallback(OpenModelEditorCallback));
	}

    ////////////////////////////////////////
	// Properties in Tab 'Specific'
	AddInputs(mpTabEntity, cVector3f(10, 10, 0.1f));
}

//----------------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////////
// PROTECTED METHODS
//////////////////////////////////////////////////////////////////////////////

//----------------------------------------------------------------------------

bool cEditorWindowEntityEditBoxEntity::WindowSpecificInputCallback(iEditorInput* apInput)
{
	if(apInput==mpInpFile)
	{
		mpEditor->AddAction(mpEntity->CreateSetPropertyActionString(eEntityStr_Filename, cString::To8Char(mpInpFile->GetFullPath())));
	}
	else
		return cEditorWindowEntityEditBox::WindowSpecificInputCallback(apInput);

	return true;
}

//----------------------------------------------------------------------------

bool cEditorWindowEntityEditBoxEntity::OpenModelEditorCallback(iWidget* apWidget, const cGuiMessageData& aData)
{
	tString sFile = mpEntity->GetFilename();
	if(sFile.empty())
	{
		Warning("[ModelEditor] selected entity has no .ent file to edit\n");
		return true;
	}

	Log("[ModelEditor] launch requested for '%s'\n", sFile.c_str());
	mpEditor->OpenInModelEditor(sFile);

	return true;
}
kGuiCallbackDeclaredFuncEnd(cEditorWindowEntityEditBoxEntity, OpenModelEditorCallback);

//----------------------------------------------------------------------------

void cEditorWindowEntityEditBoxEntity::OnUpdate(float afTimeStep)
{
	if(mpEntity->GetTypeChanged())
	{
		SetRefreshInputs();
		mpEntity->SetTypeChanged(false);
	}

	cEditorWindowEntityEditBoxUserDefinedEntity::OnUpdate(afTimeStep);

	mpInpFile->SetValue(cString::To16Char(mpEntity->GetFilename()), false);
	mpInpFile->GetInputWidget()->SetToolTip(mpEditor->GetPathRelToWD(mpInpFile->GetFullPath()));
}

//----------------------------------------------------------------------------
