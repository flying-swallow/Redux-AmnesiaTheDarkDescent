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

#ifndef LEVEL_EDITOR_H
#define LEVEL_EDITOR_H

#include "hpl.h"

using namespace hpl;

#include "../common/EditorBaseClasses.h"
#include "../common/EditorHostActions.h"
#include "../common/EditorTypes.h"
#include "LevelEditorTypes.h"

//--------------------------------------------------------------------

namespace tinyxml2 { class XMLElement; class XMLDocument; }

class iEditorWindow;

class iEditorEditMode;
class cEditorWindowViewport;
class cEditorActionHandler;

class cLevelEditor;
class cEditorGrid;
class iEditorWorld;

class cEditorWindowEntitySearch;
class cLevelEditorWindowGroup;
class cLevelEditorWindowHierarchy;
class cLevelEditorEntFileSession;
class cEditorWindowEntityEditBox;

class iEditorWindowLowerToolbar;
class iEditorWindowEditModeSidebar;

class cEditorWindowTextureBrowser;
class cEditorWindowOptions;

class cLevelEditorMCPServer;
class cLevelEditorCameraCapture;

//--------------------------------------------------------------------

enum eDirExt
{
	eDir_Maps = eDir_LastEnum,
	eDir_StaticObjects,
	eDir_Entities,

	eDirExt_LastEnum
};


//--------------------------------------------------------------------

////////////////////////////////////
// Level Editor Group
//  Used to group entities int the level editor. Entities keep ID for a group stored.
//
class cLevelEditorGroup
{
public:
	cLevelEditorGroup();
	cLevelEditorGroup(cLevelEditor* apEditor,unsigned int alID, const tString& asName);

	/**
	 * Iterates all entities to find those of correct ID and set visibility
	 */
	void SetVisibility(bool abX);
	bool GetVisibility() { return mbVisibility; }

	void SetName(const tString& asName) { msName = asName; }
	const tString& GetName() { return msName; }

	unsigned int GetID() { return mlID; }

	static unsigned int GetGroupCount() { return mlGroupCounter; }
	static void IncGroupCount() { ++mlGroupCounter; }

protected:
	cLevelEditor* mpEditor;
	unsigned int mlID;
	tString msName;
	bool mbVisibility;


	static unsigned int mlGroupCounter;
};


//--------------------------------------------------------------------

////////////////////////////////////
// Level Editor
//  Base class for the level editor. Contains global functions specific to the level editor. 
//  It is also responsible for (by using functionality from iEditorBase) created all needed GUI
//  and structures needed when Engine calls the Run command.
class cLevelEditor : public iEditorBase, public iEditorHostActions
{
public:
	cLevelEditor();
	~cLevelEditor();

	void LookAtEntity(int alEntityID);

	// Open up a file picker for importing the contents of a map file into the current map. 
	void Command_Import();
	
	
	// Open up a file picker for exporting all currently selected entities into a map file.
	void Command_Export();

	// Programmatic save/load to an explicit path, bypassing the GUI file picker
	// (used by the MCP server). These reach the protected msSaveFilename /
	// mvLoadFilenames that the base Save()/Load() consume. Return false on
	// obvious failure (empty path / missing file).
	bool SaveToFile(const tWString& asPath);
	bool LoadFromFile(const tWString& asPath);

	// Current .map path (empty if unsaved). Public accessor for the MCP server.
	const tWString& GetCurrentMapFilename() { return msSaveFilename; }

	// Re-scan resource directories so files created after startup become
	// resolvable (the FileSearcher index is otherwise built once at init).
	// asSingleDir non-empty: index just that directory (recursive). Otherwise
	// replays the startup setup: resources.cfg + every registered lookup dir
	// (which covers the ExtraStaticObjectDir*/ExtraEntityDir* config dirs).
	// Returns the number of newly indexed files, or -1 if asSingleDir does
	// not exist. Used by the MCP 'refresh_assets' tool.
	int RefreshResourceIndex(const tWString& asSingleDir = _W(""));

	///////////////////////////////
	// Pending in-memory entity files (MCP 'define_entity_file'): JSON-authored
	// .ent documents held in memory (loaded through the SAME entity load path
	// via iEditorBase::GetPendingEntFileRoot) and written to disk as XML when
	// the map is saved (OnPostSave -> FlushPendingEntFiles). Cleared on map
	// reset/new/load — pending files not saved with the map are discarded.
	// These delegate to the iEditorBase-owned cPrefabManager (see PrefabManager.h),
	// the single home for the in-memory .ent definitions.
	tinyxml2::XMLElement* GetPendingEntFileRoot(const tString& asFile); // override iEditorBase
	// Takes ownership of apDoc (root must be <Entity>); replaces + re-dirties
	// an existing entry with the same path. abBroadcast=true fires the prefab
	// manager's change broadcast (the world then live-reloads matching instances).
	void SetPendingEntFile(const tString& asPath, tinyxml2::XMLDocument* apDoc, bool abBroadcast = true);
	// Write every dirty pending file to its path (creating folders), re-index
	// each directory, and clear the dirty set. Returns files written.
	int FlushPendingEntFiles();
	int GetPendingEntFileDirtyCount();
	void ClearPendingEntFiles();

	// Off-screen virtual-camera capture backing the MCP 'capture_view' tool.
	// NULL until OnInit, or if the headless render setup failed.
	cLevelEditorCameraCapture* GetCameraCapture() { return mpCameraCapture; }

	///////////////////////////////
	// MCP server hooks (override iEditorBase; drive the Options "MCP" tab)
	bool SupportsMCP() { return true; }
	void GetMCPState(bool& abEnabled, int& alPort, tString& asToken, tString& asStatus);
	void ApplyMCPConfig(bool abEnabled, int alPort, const tString& asToken);
	int GetMCPClientCount();
	tString GetMCPClientLabel(int alIdx);
	tString GetMCPClientSnippet(int alIdx);

	///////////////////////////////
	// iEditorHostActions — the capability interface the shared entity/particle
	// edit-boxes and the edit-mode sidebar reach via dynamic_cast (so iEditorBase
	// stays free of these LevelEditor-only concepts, see common/EditorHostActions.h).

	// External editor launch (drives the entity edit-box "Edit in Model Editor" and
	// the particle edit-box "Edit in Particle Editor" buttons). Launches the
	// co-located standalone editor on the given file, handing it this editor's MCP
	// endpoint so it can push live reload notifications back on save.
	void OpenInModelEditor(const tString& asEntFile) override;
	void OpenInParticleEditor(const tString& asPsFile) override;

	// Entity-file scope (drives the entity edit-box "Edit Contents" button).
	// "Scoping into" a placed model re-roots the hierarchy to that model's embedded
	// lights/particles/sounds/bodies and lets them be edited in place — the .ent
	// definition, so every placed instance updates live — WITHOUT launching the
	// standalone ModelEditor. Backed by a cLevelEditorEntFileSession.
	//
	// Enter/Exit are invoked from GUI button callbacks (the edit-box "Edit Contents"
	// and the sidebar "Return to World"), so they only RECORD the request; the actual
	// enter/exit — which destroys + rebuilds toolbar buttons and property panels —
	// runs on the next OnUpdate, outside widget callback processing (destroying a
	// widget inside its own ProcessCallbacks is a use-after-free).
	void EnterEntFileScope(int alEntityID) override;
	void ExitEntFileScope() override;
	bool IsEntFileScoped() override { return mpScopedSession!=NULL; }

	cLevelEditorEntFileSession* GetScopedSession() { return mpScopedSession; }
	int GetScopedEntityID() { return mlScopedEntityID; }

	// The world the shared edit machinery is currently acting on: the scope
	// session's world while scoped into a placed .ent, else the map world.
	// (Override of iEditorBase; out-of-line so the session type is complete.)
	iEditorWorld* GetActiveEditorWorld() override;

	// Toggle the scoped-into entity's OWN property box (name/transform/user-vars) in the
	// right-pane slot — wired to the edit-mode sidebar's "Entity" button. On-demand: no-op
	// unless scoped; shows it if hidden, hides it if shown. See mpScopedEntityBox.
	void ShowScopedEntityProperties() override;

	// Exit scope (if any) WITHOUT saving and drop the session. Called on new/load
	// map and in the dtor (while the engine scene is still alive) — the map is being
	// abandoned, so edits are not flushed here (they already live in the pending-ent
	// cache from the per-edit debounce, and go/stay with the map).
	void ClearEntFileSessions();

	///////////////////////////////
	// Group Tool stuff
	void AddGroup(unsigned int alID, const tString& asName);
	void RemoveGroup(unsigned int alID);

	cLevelEditorGroup& GetGroup(unsigned int alID);
	tGroupMap& GetGroups() { return mmapGroups; }

	void SetUpClassDefinitions(cEditorUserClassDefinitionManager* apManager);

	const tWStringVec& GetStaticObjectExtraDirs() { return mvExtraSODirs; }
	const tWStringVec& GetEntityExtraDirs() { return mvExtraEntDirs; }

protected:
	///////////////////////////
	// Own functions
	bool MainMenu_ItemClick(iWidget* apWidget, const cGuiMessageData& aData);
	kGuiCallbackDeclarationEnd(MainMenu_ItemClick);

	bool MainMenu_UndoRedo(iWidget* apWidget, const cGuiMessageData& aData);
	kGuiCallbackDeclarationEnd(MainMenu_UndoRedo);

	void AppSpecificReset();
	//void AppSpecificLoad(tinyxml2::XMLElement* apDoc);
	//void AppSpecificSave(tinyxml2::XMLElement* apDoc);

	void LoadEditorSession(tinyxml2::XMLElement* apDoc, tinyxml2::XMLElement** apElement);
	void SaveEditorSession(tinyxml2::XMLElement* apDoc, tinyxml2::XMLElement** apElement);

	bool ImportFileCallback(iWidget* apWidget, const cGuiMessageData& aData);
	kGuiCallbackDeclarationEnd(ImportFileCallback);
	bool ExportFileCallback(iWidget* apWidget, const cGuiMessageData& aData);
	kGuiCallbackDeclarationEnd(ExportFileCallback);

	iEditorWorld* CreateSpecificWorld();

	// LevelEditor lays the edit-mode buttons out horizontally (toolbar under the
	// menu) so the left edge is free for the hierarchy panel.
	iEditorWindowEditModeSidebar* CreateSpecificEditModeSidebar();
	// Reserves the left column for the hierarchy panel and the row under the menu
	// for the horizontal edit-mode toolbar; the viewport/right-pane math follows.
	void SetUpWindowAreas();

	cWidgetMainMenu* CreateMainMenu();
	void UpdateEditMenu();

	///////////////////////////
	// Implemented functions
	void OnSetUpDirectories();

	void OnUpdate(float afTimeStep);
	// Pumps the async camera-capture jobs (render + GPU readback happen across
	// frames) and hands finished captures to the MCP server. Runs from the base
	// OnDraw (after the scene render), so the editor world's per-frame GPU data
	// is published.
	void OnPostRender(float afTimeStep);
	void OnSetSelectedViewport(){}

	void OnPostUpdateLayout();

	void OnInit();
	void OnInitInput();
	void OnInitLayout();

	void OnLoadConfig();
	void OnSaveConfig();

	// Writes pending MCP-defined entity files to disk after a successful map
	// save, so the saved map's .ent references resolve on the filesystem.
	void OnPostSave();

	///////////////////////////
	// Data
	tWString msLastImportFile;	//Last file path used for an import operation. Default = ??
	tWString msLastImportPath;	//Last file path used for an export operation. Default = ??

	tWStringVec mvExtraSODirs;
	tWStringVec mvExtraEntDirs;

	/////////////////////////////
	// Main menu
	cWidgetMainMenu* mpMainMenu;
	
	// File menu
	cWidgetMenuItem* mpMainMenuNew;
	cWidgetMenuItem* mpMainMenuSave;
	cWidgetMenuItem* mpMainMenuSaveAs;
	cWidgetMenuItem* mpMainMenuLoad;
	cWidgetMenuItem* mpMainMenuRecent;
	cWidgetMenuItem* mpMainMenuImport;
	cWidgetMenuItem* mpMainMenuExport;
	cWidgetMenuItem* mpMainMenuExit;

	// Edit menu
	cWidgetMenuItem* mpMainMenuUndo;
	cWidgetMenuItem* mpMainMenuRedo;
	cWidgetMenuItem* mpMainMenuDelete;
	cWidgetMenuItem* mpMainMenuClone;
	cWidgetMenuItem* mpMainMenuSearch;
	cWidgetMenuItem* mpMainMenuGroup;
	cWidgetMenuItem* mpMainMenuLevelSettings;
	cWidgetMenuItem* mpMainMenuOptions;
	cWidgetMenuItem* mpMainMenuCompound;

	
	////////////////////////////////
	// Some windows
	cEditorWindowOptions* mpWindowOptions;
	cEditorWindowEntitySearch* mpWindowSearch;
	cEditorWindowTextureBrowser* mpWindowTextureBrowser;

	cLevelEditorWindowGroup* mpWindowGroup;

	// Scene hierarchy / outliner panel that replaces the old left-edge edit-mode
	// strip. Created in OnInitLayout; kept in sync via the OnWorldModify /
	// OnSelectionChange window fan-outs.
	cLevelEditorWindowHierarchy* mpWindowHierarchy;

	// The single transient session currently "scoped into" (NULL = normal map view),
	// plus the id of the placed entity it was entered from (for the breadcrumb /
	// re-select on exit). Created on EnterEntFileScope, destroyed on exit/reset.
	cLevelEditorEntFileSession* mpScopedSession;
	int mlScopedEntityID;

	// The scoped-into entity's own property box while shown on demand (the sidebar
	// "Entity" button). NULL = hidden. Hosted directly by the editor (not by a Select
	// mode): built in ShowScopedEntityProperties, auto-hidden in OnUpdate when a sub-
	// object is selected (both use the same right-pane slot), destroyed in TeardownScope.
	cEditorWindowEntityEditBox* mpScopedEntityBox;

	// Deferred scope request set by the callback-facing Enter/ExitEntFileScope and
	// consumed once in OnUpdate (mlPendingScopeEnter = entity id, or -1 for none).
	int  mlPendingScopeEnter;
	bool mbPendingScopeExit;
	// Run the recorded request; called from OnUpdate (outside widget callbacks).
	void ProcessPendingScope();

	// The real enter/exit work (widget surgery). DoEnter builds+mounts the session,
	// makes its Select mode current and swaps the toolbar; DoExit is TeardownScope.
	void DoEnterEntFileScope(int alEntityID);

	// Shared teardown for a user exit (abSave=true, "Return to World") and map
	// reset/dtor (abSave=false): restores the map toolbar + Select mode, clears
	// selection + undo history, deletes the session.
	void TeardownScope(bool abSave, bool abRestoreUI);


	////////////////////////////////
	// Groups
	tGroupMap mmapGroups;	//Map for all entity groups currently created. (map specific)

	///////////////////////
	// Config stuff
	cConfigFile* mpLocalConfig;

	///////////////////////
	// MCP server (optional, localhost). Lets an external AI agent drive the
	// editor over HTTP/JSON-RPC. NULL when disabled in config.
	cLevelEditorMCPServer* mpMCPServer;

	// Off-screen camera capture for the MCP 'capture_view' tool. Created in
	// OnInit, pumped from OnPostRender, deleted in the dtor. NULL if disabled /
	// setup failed.
	cLevelEditorCameraCapture* mpCameraCapture;

	// Config-backed settings (loaded in OnLoadConfig, edited via the Options
	// "MCP" tab, persisted in OnSaveConfig). Tears down + rebuilds the server.
	bool    mbMCPEnabled;
	int     mlMCPPort;
	tString msMCPToken;
	void RestartMCPServer();
};

//----------------------------------------------------------

#endif //LEVEL_EDITOR_H

