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

#ifndef MODEL_EDITOR_H
#define MODEL_EDITOR_H

#include "../common/StdAfx.h"
using namespace hpl;

#include "../common/EditorBaseClasses.h"

#include "../common/EditorTypes.h"
#include "../common/EditorVar.h"

#include "ModelEditorTypes.h"

namespace tinyxml2 { class XMLElement; }

//--------------------------------------------------------------------

class iEditorWindow;

class iEditorEditMode;
class cEditorWindowViewport;
class cEditorActionHandler;

class cModelEditor;
class cEditorGrid;
class cEntityCustomVar;

class cEditorWindowEntitySearch;

class cModelEditorEditModeToolbox;
class cModelEditorLowerToolbar;
class cModelEditorWindowPhysiscTest;

enum eDirExt
{
	eDir_Maps = eDir_LastEnum,

	eDirExt_LastEnum
};


enum eModelEditorFlags
{
	eModelEditorFlag_TestWindowActive = eEditorFlag_LastEnum,

	eModelEditorFlag_LastEnum,
};



//--------------------------------------------------------------------

class cModelEditor : public iEditorBase
{
public:
	cModelEditor();
	~cModelEditor();

	int GetLastSelectedAnimation() { return mlLastTestAnimation; }
	void SetLastSelectedAnimation(int alX) { mlLastTestAnimation = alX; }

	void SetUpClassDefinitions(cEditorUserClassDefinitionManager* apManager);

	///////////////////////////
	// LevelEditor launch integration (set by ModelEditorMain from the command
	// line when opened via the LevelEditor's "Edit in Model Editor" button).
	void SetStartupFile(const tString& asFile) { msStartupFile = asFile; }
	void SetReloadNotifyEndpoint(int alPort, const tString& asToken) { mlNotifyPort = alPort; msNotifyToken = asToken; }

	// Opens the startup .ent (if one was set) after Init(). Resolves the
	// resource-relative path through the file searcher, then loads it.
	void OpenStartupFile();

protected:
	///////////////////////////
	// Own functions
	bool MainMenu_ItemClick(iWidget* apWidget, const cGuiMessageData& aData);
	kGuiCallbackDeclarationEnd(MainMenu_ItemClick);

	bool QuitCallback(iWidget* apWidget, const cGuiMessageData& aData);
	kGuiCallbackDeclarationEnd(QuitCallback);

	bool MainMenu_UndoRedo(iWidget* apWidget, const cGuiMessageData& aData);
	kGuiCallbackDeclarationEnd(MainMenu_UndoRedo);

	bool MeshImport_Callback(iWidget* apWidget, const cGuiMessageData& aData);
	kGuiCallbackDeclarationEnd(MeshImport_Callback);

	bool OnChangeFlags(int alFlags);

	
	void UpdateEditMenu();

	iEditorWindowLowerToolbar* CreateSpecificLowerToolbar();

	///////////////////////////
	// Implemented functions
	iEditorWorld* CreateSpecificWorld();

	void OnSetUpDirectories();

	void OnUpdate(float afTimeStep);
	void OnSetSelectedViewport(){}

	void OnPostUpdateLayout();

	void OnInit();
	void OnInitInput();
	void OnInitLayout();

	void OnLoadConfig();
	void OnSaveConfig();

	void AppSpecificReset();
	void AppSpecificLoad(tinyxml2::XMLElement* apDoc);
	void AppSpecificSave(tinyxml2::XMLElement* apDoc);

	// Fires after a successful Save(): push a live-reload notification to the
	// LevelEditor that launched us (no-op when launched standalone).
	void OnPostSave();

	cWidgetMainMenu* CreateMainMenu();

	///////////////////////////
	// Data
	// Menu stuff
	// File menu
	cWidgetMenuItem* mpMainMenuNew;
	cWidgetMenuItem* mpMainMenuSave;
	cWidgetMenuItem* mpMainMenuSaveAs;
	cWidgetMenuItem* mpMainMenuLoad;
	cWidgetMenuItem* mpMainMenuRecent;
	cWidgetMenuItem* mpMainMenuImport;
	cWidgetMenuItem* mpMainMenuExit;

	// Edit menu
	cWidgetMenuItem* mpMainMenuUndo;
	cWidgetMenuItem* mpMainMenuRedo;
	cWidgetMenuItem* mpMainMenuDelete;
	cWidgetMenuItem* mpMainMenuClone;
	cWidgetMenuItem* mpMainMenuSearch;
	cWidgetMenuItem* mpMainMenuGroup;
	
	// Settings menu
	cWidgetMenuItem* mpMainMenuUserSettings;
	cWidgetMenuItem* mpMainMenuAnimations;

	// View menu
	cWidgetMenuItem* mpMainMenuOutline;
	
	///////////////////////////////////
	// Test window
	iEditorWindow* mpTestWindow;
	int mlLastTestAnimation;
	iEditorWindow* mpWindowAnimations;
	
	///////////////////////
	// Config stuff
	cConfigFile* mpLocalConfig;

	///////////////////////
	// Saving / Loading
	tWString msImportedMeshFilename;
	tWString msLastMeshPath;

	///////////////////////
	// LevelEditor sync (set from the command line when launched from the
	// LevelEditor; all default/empty means we were opened standalone).
	tString msStartupFile;   // resource-relative .ent to open + notify about
	int     mlNotifyPort;    // LevelEditor MCP port (0 = no notify)
	tString msNotifyToken;   // LevelEditor MCP bearer token (may be empty)

	///////////////////////
	// Model User Settings
	tString msDefaultTypeName;
	tString msDefaultSubTypeName;

	tString msCurrentTypeName;
	tString msCurrentSubTypeName;

	//tCustomVarList mlstCurrentUserVars; // Vars in use by current model
	//tCustomVarMap mmapUserVarPool; // Var pool (to keep values of non active vars, in case they are active again)
};

//----------------------------------------------------------

#endif //MODEL_EDITOR_H

