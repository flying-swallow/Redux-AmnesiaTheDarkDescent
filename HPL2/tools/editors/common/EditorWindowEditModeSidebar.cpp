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

#include "EditorWindowEditModeSidebar.h"
#include "EditorBaseClasses.h"
#include "EditorEditMode.h"
#include "EditorHostActions.h"

//----------------------------------------------------------------

iEditorWindowEditModeSidebar::iEditorWindowEditModeSidebar(iEditorBase* apEditor, bool abHorizontal) : iEditorWindow(apEditor,"Edit Mode Sidebar")
{
	mbHorizontal = abHorizontal;
}

//----------------------------------------------------------------

iEditorWindowEditModeSidebar::~iEditorWindowEditModeSidebar()
{
}

//----------------------------------------------------------------

void iEditorWindowEditModeSidebar::OnInitLayout()
{
	RebuildButtons(mpEditor->GetEditModes(), false);
}

//----------------------------------------------------------------

void iEditorWindowEditModeSidebar::RebuildButtons(const std::vector<iEditorEditMode*>& avModes, bool abShowReturn)
{
	// Drop the previous shortcuts (they point at the buttons we're about to destroy)
	// then the buttons themselves.
	for(size_t i=0;i<mvShortcuts.size();++i)
		mpSet->RemoveGlobalShortcut(mvShortcuts[i]);
	mvShortcuts.clear();

	for(size_t i=0;i<mvEditModeButtons.size();++i)
		mpSet->DestroyWidget(mvEditModeButtons[i]);
	mvEditModeButtons.clear();

	cVector3f vPos = cVector3f(3,5,0.1f);
	float fButtonSize = 25.0f;
	eKey key = eKey_1;
	int lKeyMod = 0;
	tWString sModStrings[] = { _W("Ctrl"), _W("Shift"), _W("Alt") };

	for(int i=0;i<(int)avModes.size();++i)
	{
		iEditorEditMode* pEditMode = avModes[i];
		tString sFilename = cString::ToLowerCase(cString::ReplaceCharTo(pEditMode->GetName(), " ", ""));
		cGuiGfxElement* pImg = mpSet->GetGui()->CreateGfxImage("editmode_" + sFilename + ".tga", eGuiMaterial_Alpha);

		cWidgetButton* pButton = mpSet->CreateWidgetButton(vPos, fButtonSize, _W(""), mpBGFrame, true);
		pButton->AddCallback(eGuiMessage_ButtonPressed, this, kGuiCallback(Button_Pressed));
		mvShortcuts.push_back(mpSet->AddGlobalShortcut(lKeyMod, key, pButton, eGuiMessage_ButtonPressed));
		pButton->SetUserData(pEditMode);
		pButton->SetImage(pImg);

		tWString sTooltip = cString::ToStringW(key-eKey_0);
		if(lKeyMod!=0)
			sTooltip = sModStrings[lKeyMod-1] + _W("+") + sTooltip;
		sTooltip = cString::To16Char(pEditMode->GetName()) + _W(" (") + sTooltip + _W(")");
		pButton->SetToolTip(sTooltip);

		mvEditModeButtons.push_back(pButton);

		if(mbHorizontal)
			vPos.x += fButtonSize+2.0f;
		else
			vPos.y += fButtonSize+2.0f;

		////////////////
		// Assign key
		key = (eKey)(key+1);
		if(key==eKey_1 && lKeyMod==0)
			lKeyMod = 1;
		if(key>eKey_9)
		{
			lKeyMod = lKeyMod<<1;
			key=eKey_0;
		}
	}

	// While scoped into an entity, a text button to leave. Sentinel UserData==NULL
	// distinguishes it in Button_Pressed. No shortcut (Esc is handled elsewhere).
	float fReturnWidth = 110.0f;
	if(abShowReturn)
	{
		cWidgetButton* pButton = mpSet->CreateWidgetButton(vPos, cVector2f(fReturnWidth, fButtonSize), _W("Return to World"), mpBGFrame, false);
		pButton->AddCallback(eGuiMessage_ButtonPressed, this, kGuiCallback(Button_Pressed));
		pButton->SetUserData(NULL);
		pButton->SetToolTip(_W("Finish editing this entity and return to the map"));
		mvEditModeButtons.push_back(pButton);

		if(mbHorizontal)
			vPos.x += fReturnWidth+2.0f;
		else
			vPos.y += fButtonSize+2.0f;
	}

	if(mbHorizontal)
		SetSize(cVector2f(vPos.x, fButtonSize+6));
	else
		SetSize(cVector2f((abShowReturn?fReturnWidth:fButtonSize)+6,vPos.y));

	mpEditor->SetLayoutNeedsUpdate(true);
}

//----------------------------------------------------------------

bool iEditorWindowEditModeSidebar::Button_Pressed(iWidget* apWidget, const cGuiMessageData& aData)
{
	// Sentinel NULL user-data = the "Return to World" button (only present while scoped).
	// It drives a host-only action (iEditorHostActions), reached via the capability cast.
	iEditorEditMode* pMode = (iEditorEditMode*)apWidget->GetUserData();
	if(pMode==NULL)
	{
		if(iEditorHostActions* pHost = dynamic_cast<iEditorHostActions*>(mpEditor))
			pHost->ExitEntFileScope();
		return true;
	}

	mpEditor->SetCurrentEditMode(pMode);
	mpEditor->SetLayoutNeedsUpdate(true);

	return true;
}
kGuiCallbackDeclaredFuncEnd(iEditorWindowEditModeSidebar, Button_Pressed);

//----------------------------------------------------------------

void iEditorWindowEditModeSidebar::OnUpdate(float afTimeStep)
{
	iEditorEditMode* pEditMode = mpEditor->GetCurrentEditMode();
	for(int i=0; i<(int)mvEditModeButtons.size(); ++i)
	{
		bool bPressed = (pEditMode==(iEditorEditMode*)mvEditModeButtons[i]->GetUserData());

		mvEditModeButtons[i]->SetPressed(bPressed, false);
	}
}


//----------------------------------------------------------------
