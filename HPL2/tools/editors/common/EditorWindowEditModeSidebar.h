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

#ifndef HPLEDITOR_EDITOR_WINDOW_EDIT_MODE_SIDEBAR_H
#define HPLEDITOR_EDITOR_WINDOW_EDIT_MODE_SIDEBAR_H

#include "../common/StdAfx.h"

using namespace hpl;

#include "EditorWindow.h"

//----------------------------------------------------------------

//----------------------------------------------------------------

class iEditorWindowEditModeSidebar : public iEditorWindow
{
public:
	// abHorizontal lays the edit-mode buttons out in a row (toolbar under the
	// menu bar) instead of the default vertical strip on the left edge. Only
	// the LevelEditor passes true; the other editors keep the vertical strip.
	iEditorWindowEditModeSidebar(iEditorBase* apEditor, bool abHorizontal=false);
	virtual ~iEditorWindowEditModeSidebar();

	void OnInitLayout();
	void OnUpdate(float afTimeStep);

	// Rebuild the edit-mode buttons from the given mode list, destroying the previous
	// buttons + their global shortcuts first. Passing the editor's own modes (what
	// OnInitLayout does) shows the normal toolbar; the LevelEditor passes a scoped
	// ent-file session's modes + abShowReturn to add a "Return to World" button that
	// exits scope. Safe to call repeatedly.
	void RebuildButtons(const std::vector<iEditorEditMode*>& avModes, bool abShowReturn);

protected:
	///////////////////////////
	// Own functions
	bool Button_Pressed(iWidget* apWidget, const cGuiMessageData& aData);
	kGuiCallbackDeclarationEnd(Button_Pressed);

	///////////////////////////
	// Data
	std::vector<cWidgetButton*> mvEditModeButtons;
	// Global (numeric) shortcuts we created for the current buttons, so RebuildButtons
	// can drop them before destroying the widgets they point at (no dangling shortcut).
	std::vector<cGuiGlobalShortcut*> mvShortcuts;

	bool mbHorizontal;
};

//----------------------------------------------------------------

#endif // EDIT_MODE_TOOLBOX_H

