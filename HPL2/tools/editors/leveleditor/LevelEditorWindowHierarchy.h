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

#ifndef LEVEL_EDITOR_WINDOW_HIERARCHY_H
#define LEVEL_EDITOR_WINDOW_HIERARCHY_H

#include "hpl.h"
using namespace hpl;

#include "../common/EditorWindow.h"

#include "gui/WidgetNodeTree.h"

#include <map>

//---------------------------------------------------------------

class cLevelEditor;
class iEntityWrapper;
class iEditorWorld;
class cLevelEditorEntFileSession;

//---------------------------------------------------------------

// Width (px) of the left-column hierarchy panel. Also consumed by
// cLevelEditor::SetUpWindowAreas to reserve the left margin.
static const float kLevelEditorHierarchyPanelWidth = 200.0f;

//---------------------------------------------------------------

////////////////////////////////////////////////////////////////
// cLevelEditorWindowHierarchy
//	Scene outliner docked on the left edge (replaces the old edit-mode strip).
//	Shows the open map as a tree: layer root -> Groups / per-type categories ->
//	entities, with compound components and attachment children nested. Clicking a
//	row selects the entity (Shift toggles); double-click focuses the camera on it.
//	Kept in sync via the OnWorldModify / OnSelectionChange window fan-outs.
//
//	The tree is authored one root node per "layer"; today only the open map is a
//	layer, but the shape leaves room for previewed/linked maps later.
class cLevelEditorWindowHierarchy : public iEditorWindow
{
public:
	// afTopOffset = y of the panel's top edge (just under the horizontal
	// edit-mode toolbar); the panel sizes itself down to the lower toolbar.
	cLevelEditorWindowHierarchy(cLevelEditor* apEditor, float afTopOffset);
	~cLevelEditorWindowHierarchy();

	// Rebuild the tree from scratch. Called by the LevelEditor when the ent-file
	// scope is entered/exited (which re-roots the tree) — a full rebuild, same as
	// the OnWorldModify fan-out.
	void RefreshTree() { RebuildTree(); }

	// Called by the LevelEditor before it tears down the scoped session. The scoped
	// sub-entity property panel is owned by the session's Select mode (destroyed when
	// that mode stops being current, which TeardownScope does first), so there is
	// nothing to close here — kept as a teardown hook. (Auto-persist of scoped edits
	// now lives on the session, driven by cLevelEditor::OnUpdate, not this window.)
	void CloseScopeUI() {}

protected:
	///////////////////////////////
	// Callbacks
	bool Filter_OnTextChange(iWidget* apWidget, const cGuiMessageData& aData);
	kGuiCallbackDeclarationEnd(Filter_OnTextChange);

	bool Tree_OnSelectionChange(iWidget* apWidget, const cGuiMessageData& aData);
	kGuiCallbackDeclarationEnd(Tree_OnSelectionChange);

	bool Tree_OnDoubleClick(iWidget* apWidget, const cGuiMessageData& aData);
	kGuiCallbackDeclarationEnd(Tree_OnDoubleClick);

	///////////////////////////////
	// Own functions
	void RebuildTree();

	// Scoped view: when the LevelEditor is "scoped into" a placed model, the tree
	// re-roots to a breadcrumb + that model's embedded sub-entities (from the
	// entity-file session's headless world) instead of the map.
	void BuildScopedView(cWidgetTreeNode* apBreadcrumb, cLevelEditorEntFileSession* apSession);
	void AddSubEntityNode(cWidgetTreeNode* apParent, iEntityWrapper* apEnt);

	void BuildLayerNode(cWidgetTreeNode* apRoot, iEditorWorld* apWorld, int alLayerIdx);
	// Adds apEnt (and, recursively, its compound components + attachment
	// children) as a node under apParent. alLayerIdx prefixes the persistence key.
	cWidgetTreeNode* AddEntityNode(cWidgetTreeNode* apParent, iEntityWrapper* apEnt, int alLayerIdx);

	void UpdateHighlights();

	// Creates a node under apParent, tags it with asKey (stored in the widget's
	// unused name slot + mmapNodeKey) and restores its expand state from the last
	// rebuild (falling back to abDefaultExtended for never-seen keys).
	cWidgetTreeNode* MakeNode(cWidgetTreeNode* apParent, const tWString& asText,
							  const tWString& asKey, bool abDefaultExtended);
	void SaveExpandState();

	static tWString GetCategoryLabel(int alTypeID);

	///////////////////////////////
	// Implemented functions
	void OnInitLayout();
	void OnWorldModify();
	void OnSelectionChange();

	///////////////////////////////
	// Data
	float mfTopOffset;

	cWidgetTextBox* mpFilter;
	cWidgetLabel*   mpFilterGhost;
	cWidgetFrame*   mpTreeFrame;
	cWidgetNodeTree* mpTree;

	// node <-> entity id, node -> group id (for click routing) and a key->extended
	// map so expand state survives full rebuilds.
	std::map<cWidgetTreeNode*, int> mmapNodeEntity;
	std::map<int, cWidgetTreeNode*> mmapEntityNode;
	std::map<cWidgetTreeNode*, int> mmapNodeGroup;
	std::map<cWidgetTreeNode*, tWString> mmapNodeKey;
	std::map<tWString, bool>        mmapExpandState;

	// Scoped view: node -> sub-entity id (in the scoped session's world), and the
	// breadcrumb row that exits scope when clicked.
	std::map<cWidgetTreeNode*, int> mmapNodeSubEntity;
	cWidgetTreeNode* mpScopeExitNode;
};

//---------------------------------------------------------------

#endif // LEVEL_EDITOR_WINDOW_HIERARCHY_H
