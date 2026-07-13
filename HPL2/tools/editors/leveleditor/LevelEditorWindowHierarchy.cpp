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

#include "LevelEditorWindowHierarchy.h"

#include "LevelEditor.h"
#include "LevelEditorWorld.h"
#include "LevelEditorEntFileSession.h"

#include "../common/EditorWorld.h"
#include "../common/EditorSelection.h"
#include "../common/EditorEditModeSelect.h"
#include "../common/EditorActionEntity.h"
#include "../common/EntityWrapper.h"
#include "../common/EditorWindowEntityEditBox.h"
#include "../common/EditorWindowFactory.h"

//-------------------------------------------------------------------------------

// Category display order (top-level buckets for ungrouped entities).
static const int gvCategoryOrder[] =
{
	eEditorEntityType_StaticObject,
	eEditorEntityType_Light,
	eEditorEntityType_Entity,
	eEditorEntityType_Billboard,
	eEditorEntityType_Sound,
	eEditorEntityType_ParticleSystem,
	eEditorEntityType_Area,
	eEditorEntityType_Primitive,
	eEditorEntityType_Decal,
	eEditorEntityType_FogArea,
	eEditorEntityType_Compound,
};
static const int glCategoryOrderNum = (int)(sizeof(gvCategoryOrder)/sizeof(gvCategoryOrder[0]));

//-------------------------------------------------------------------------------

cLevelEditorWindowHierarchy::cLevelEditorWindowHierarchy(cLevelEditor* apEditor, float afTopOffset)
	: iEditorWindow(apEditor, "Hierarchy")
{
	mfTopOffset   = afTopOffset;
	mpFilter      = NULL;
	mpFilterGhost = NULL;
	mpTreeFrame   = NULL;
	mpTree        = NULL;

	mpScopeExitNode   = NULL;
}

cLevelEditorWindowHierarchy::~cLevelEditorWindowHierarchy()
{
}

//-------------------------------------------------------------------------------

void cLevelEditorWindowHierarchy::OnInitLayout()
{
	float fWidth  = kLevelEditorHierarchyPanelWidth;
	float fHeight = mpSet->GetVirtualSize().y - mfTopOffset - 2.0f;

	mpBGFrame->SetSize(cVector2f(fWidth, fHeight));

	/////////////////////////////////////////////////
	// Filter box
	mpFilter = mpSet->CreateWidgetTextBox(cVector3f(3, 3, 0.1f), cVector2f(fWidth-6, 0), _W(""), mpBGFrame);
	mpFilter->AddCallback(eGuiMessage_TextChange, this, kGuiCallback(Filter_OnTextChange));

	mpFilterGhost = mpSet->CreateWidgetLabel(cVector3f(8, 6, 0.2f), cVector2f(fWidth-12, 0), _W("Filter..."), mpBGFrame);
	mpFilterGhost->SetColorMul(cColor(1, 1, 1, 0.3f));

	/////////////////////////////////////////////////
	// Tree host frame (scrolls when the tree grows past the panel)
	float fTreeTop = 26.0f;
	mpTreeFrame = mpSet->CreateWidgetFrame(cVector3f(3, fTreeTop, 0.1f), cVector2f(fWidth-6, fHeight - fTreeTop - 3.0f),
											true, mpBGFrame, false, true);
	mpTreeFrame->SetBackgroundBgfx(eGuiSkinGfx_ListBoxBackground);
	mpTreeFrame->SetDrawBackground(true);

	mpTree = mpSet->CreateWidgetNodeTree(fWidth-6, mpTreeFrame);
	mpTree->AddCallback(eGuiMessage_SelectionChange, this, kGuiCallback(Tree_OnSelectionChange));
	mpTree->AddCallback(eGuiMessage_MouseDoubleClick, this, kGuiCallback(Tree_OnDoubleClick));

	RebuildTree();
}

//-------------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
// OWN METHODS
////////////////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------------------

cWidgetTreeNode* cLevelEditorWindowHierarchy::MakeNode(cWidgetTreeNode* apParent, const tWString& asText,
														const tWString& asKey, bool abDefaultExtended)
{
	cWidgetTreeNode* pNode = apParent ? apParent->AddTreeNode(asText) : mpTree->AddTreeNode(asText);
	pNode->SetName(asKey);
	mmapNodeKey[pNode] = asKey;

	std::map<tWString, bool>::iterator it = mmapExpandState.find(asKey);
	pNode->SetExtended(it != mmapExpandState.end() ? it->second : abDefaultExtended);

	return pNode;
}

//-------------------------------------------------------------------------------

void cLevelEditorWindowHierarchy::SaveExpandState()
{
	std::map<cWidgetTreeNode*, tWString>::iterator it = mmapNodeKey.begin();
	for(; it != mmapNodeKey.end(); ++it)
		mmapExpandState[it->second] = it->first->IsExtended();
}

//-------------------------------------------------------------------------------

tWString cLevelEditorWindowHierarchy::GetCategoryLabel(int alTypeID)
{
	switch(alTypeID)
	{
	case eEditorEntityType_StaticObject:   return _W("Static Objects");
	case eEditorEntityType_Light:          return _W("Lights");
	case eEditorEntityType_Entity:         return _W("Entities");
	case eEditorEntityType_Billboard:      return _W("Billboards");
	case eEditorEntityType_Sound:          return _W("Sounds");
	case eEditorEntityType_ParticleSystem: return _W("Particle Systems");
	case eEditorEntityType_Area:           return _W("Areas");
	case eEditorEntityType_Primitive:      return _W("Primitives");
	case eEditorEntityType_Decal:          return _W("Decals");
	case eEditorEntityType_FogArea:        return _W("Fog Areas");
	case eEditorEntityType_Compound:       return _W("Compounds");
	default:                               return _W("Other");
	}
}

//-------------------------------------------------------------------------------

cWidgetTreeNode* cLevelEditorWindowHierarchy::AddEntityNode(cWidgetTreeNode* apParent, iEntityWrapper* apEnt, int alLayerIdx)
{
	tWString sText = cString::To16Char(apEnt->GetName());
	if(sText.empty())
		sText = _W("#") + cString::ToStringW(apEnt->GetID());

	tWString sKey = cString::ToStringW(alLayerIdx) + _W("/ent:") + cString::ToStringW(apEnt->GetID());
	cWidgetTreeNode* pNode = MakeNode(apParent, sText, sKey, false);

	int lID = apEnt->GetID();
	mmapNodeEntity[pNode] = lID;
	mmapEntityNode[lID]   = pNode;

	//////////////////////////////
	// Compound components nest under the compound.
	iEntityWrapperAggregate* pAgg = dynamic_cast<iEntityWrapperAggregate*>(apEnt);
	if(pAgg)
	{
		const tEntityWrapperList& lstComponents = pAgg->GetComponents();
		tEntityWrapperList::const_iterator it = lstComponents.begin();
		for(; it != lstComponents.end(); ++it)
			AddEntityNode(pNode, *it, alLayerIdx);
	}

	//////////////////////////////
	// Attachment children nest under their parent.
	tEntityWrapperList& lstChildren = apEnt->GetChildren();
	tEntityWrapperListIt cit = lstChildren.begin();
	for(; cit != lstChildren.end(); ++cit)
		AddEntityNode(pNode, *cit, alLayerIdx);

	return pNode;
}

//-------------------------------------------------------------------------------

void cLevelEditorWindowHierarchy::BuildLayerNode(cWidgetTreeNode* apRoot, iEditorWorld* apWorld, int alLayerIdx)
{
	cLevelEditor* pLevelEditor = (cLevelEditor*)mpEditor;
	tWString sPrefix = cString::ToStringW(alLayerIdx) + _W("/");

	tEntityWrapperMap& mapEntities = apWorld->GetEntities();

	//////////////////////////////////////////////////////
	// Filter mode: flat list of matching entities, no grouping.
	tString sFilter = cString::ToLowerCase(cString::To8Char(mpFilter->GetText()));
	if(sFilter.empty() == false)
	{
		tEntityWrapperMapIt it = mapEntities.begin();
		for(; it != mapEntities.end(); ++it)
		{
			iEntityWrapper* pEnt = it->second;
			tString sName = cString::ToLowerCase(pEnt->GetName());
			if(cString::GetFirstStringPos(sName, sFilter) < 0)
				continue;

			tWString sText = cString::To16Char(pEnt->GetName());
			if(sText.empty())
				sText = _W("#") + cString::ToStringW(pEnt->GetID());

			tWString sKey = sPrefix + _W("ent:") + cString::ToStringW(pEnt->GetID());
			cWidgetTreeNode* pNode = MakeNode(apRoot, sText, sKey, false);
			mmapNodeEntity[pNode]     = pEnt->GetID();
			mmapEntityNode[pEnt->GetID()] = pNode;
		}
		return;
	}

	//////////////////////////////////////////////////////
	// Bucket top-level entities into group / category buckets. An entity that
	// belongs to a compound or is attached to a parent is shown under that
	// anchor (added recursively), so it is skipped at the top level here.
	std::map<int, tEntityWrapperList> mapGroupBuckets;
	std::map<int, tEntityWrapperList> mapCatBuckets;

	tEntityWrapperMapIt it = mapEntities.begin();
	for(; it != mapEntities.end(); ++it)
	{
		iEntityWrapper* pEnt = it->second;
		if(pEnt->BelongsToCompoundObject()) continue;
		if(pEnt->GetParent() != NULL)       continue;

		int lGroupID = 0;
		cLevelEditorEntityExtData* pExt = (cLevelEditorEntityExtData*)pEnt->GetEntityExtData();
		if(pExt) lGroupID = pExt->mlGroupID;

		if(lGroupID != 0)
			mapGroupBuckets[lGroupID].push_back(pEnt);
		else
			mapCatBuckets[pEnt->GetTypeID()].push_back(pEnt);
	}

	//////////////////////////////////////////////////////
	// Groups section
	if(mapGroupBuckets.empty() == false)
	{
		cWidgetTreeNode* pGroupsNode = MakeNode(apRoot, _W("Groups"), sPrefix + _W("groups"), true);
		tGroupMap& mapGroups = pLevelEditor->GetGroups();

		std::map<int, tEntityWrapperList>::iterator git = mapGroupBuckets.begin();
		for(; git != mapGroupBuckets.end(); ++git)
		{
			int lGroupID = git->first;
			tWString sGroupName;
			tGroupMapIt found = mapGroups.find((unsigned int)lGroupID);
			if(found != mapGroups.end())
				sGroupName = cString::To16Char(found->second.GetName());
			else
				sGroupName = _W("Group ") + cString::ToStringW(lGroupID);

			cWidgetTreeNode* pGroupNode = MakeNode(pGroupsNode, sGroupName,
													sPrefix + _W("grp:") + cString::ToStringW(lGroupID), false);
			mmapNodeGroup[pGroupNode] = lGroupID;

			tEntityWrapperListIt eit = git->second.begin();
			for(; eit != git->second.end(); ++eit)
				AddEntityNode(pGroupNode, *eit, alLayerIdx);
		}
	}

	//////////////////////////////////////////////////////
	// Category sections (fixed order)
	for(int i=0; i<glCategoryOrderNum; ++i)
	{
		int lTypeID = gvCategoryOrder[i];
		std::map<int, tEntityWrapperList>::iterator cit = mapCatBuckets.find(lTypeID);
		if(cit == mapCatBuckets.end())
			continue;

		cWidgetTreeNode* pCatNode = MakeNode(apRoot, GetCategoryLabel(lTypeID),
												sPrefix + _W("cat:") + cString::ToStringW(lTypeID), true);

		tEntityWrapperListIt eit = cit->second.begin();
		for(; eit != cit->second.end(); ++eit)
			AddEntityNode(pCatNode, *eit, alLayerIdx);

		mapCatBuckets.erase(cit);
	}

	// Safety: any type not in the fixed order (should not happen for LevelEditor).
	std::map<int, tEntityWrapperList>::iterator rem = mapCatBuckets.begin();
	for(; rem != mapCatBuckets.end(); ++rem)
	{
		cWidgetTreeNode* pCatNode = MakeNode(apRoot, GetCategoryLabel(rem->first),
												sPrefix + _W("cat:") + cString::ToStringW(rem->first), true);
		tEntityWrapperListIt eit = rem->second.begin();
		for(; eit != rem->second.end(); ++eit)
			AddEntityNode(pCatNode, *eit, alLayerIdx);
	}
}

//-------------------------------------------------------------------------------

void cLevelEditorWindowHierarchy::RebuildTree()
{
	if(mpTree == NULL)
		return;

	SaveExpandState();

	mpTree->ClearTreeNodes();
	mmapNodeEntity.clear();
	mmapEntityNode.clear();
	mmapNodeGroup.clear();
	mmapNodeKey.clear();
	mmapNodeSubEntity.clear();
	mpScopeExitNode = NULL;

	cLevelEditor* pLevelEditor = (cLevelEditor*)mpEditor;

	//////////////////////////////
	// Scoped-into-entity view: re-root to the model's embedded sub-entities.
	if(pLevelEditor->IsEntFileScoped())
	{
		cLevelEditorEntFileSession* pSession = pLevelEditor->GetScopedSession();

		tWString sMapName = cString::GetFileNameW(pLevelEditor->GetCurrentMapFilename());
		if(sMapName.empty())
			sMapName = _W("Unsaved Map");

		tWString sEntName;
		iEntityWrapper* pScopedEnt = mpEditor->GetEditorWorld()->GetEntity(pLevelEditor->GetScopedEntityID());
		if(pScopedEnt)
			sEntName = cString::To16Char(pScopedEnt->GetName());
		if(sEntName.empty())
			sEntName = cString::To16Char(cString::GetFileName(pSession->GetFile()));

		// Breadcrumb row (click -> exit scope, back to the map).
		cWidgetTreeNode* pCrumb = MakeNode(NULL, _W("< ") + sMapName + _W(" / ") + sEntName,
											_W("scope/") + sEntName, true);
		mpScopeExitNode = pCrumb;

		BuildScopedView(pCrumb, pSession);

		mpTree->UpdateTreeHeight();
		UpdateHighlights();
		return;
	}

	//////////////////////////////
	// Layer root (one per layer; only the open map for now)
	tWString sMapName = cString::GetFileNameW(pLevelEditor->GetCurrentMapFilename());
	if(sMapName.empty())
		sMapName = _W("Unsaved Map");

	cWidgetTreeNode* pRoot = MakeNode(NULL, sMapName, _W("0/map"), true);
	BuildLayerNode(pRoot, mpEditor->GetEditorWorld(), 0);

	mpTree->UpdateTreeHeight();
	UpdateHighlights();
}

//-------------------------------------------------------------------------------

void cLevelEditorWindowHierarchy::BuildScopedView(cWidgetTreeNode* apBreadcrumb, cLevelEditorEntFileSession* apSession)
{
	if(apSession==NULL)
		return;

	//////////////////////////////
	// Bucket the session's editable sub-entities by category, then emit them in
	// the session's fixed category order (Lights, Particle Systems, Sounds, ...).
	tEntityWrapperList lstSubs;
	apSession->GetSubEntities(lstSubs);

	std::map<int, tEntityWrapperList> mapCatBuckets;
	tEntityWrapperListIt it = lstSubs.begin();
	for(; it != lstSubs.end(); ++it)
		mapCatBuckets[(*it)->GetTypeID()].push_back(*it);

	int lCount = 0;
	const int* pOrder = cLevelEditorEntFileSession::GetCategoryOrder(lCount);
	for(int i=0; i<lCount; ++i)
	{
		int lTypeID = pOrder[i];
		std::map<int, tEntityWrapperList>::iterator cit = mapCatBuckets.find(lTypeID);
		if(cit == mapCatBuckets.end())
			continue;

		cWidgetTreeNode* pCatNode = MakeNode(apBreadcrumb, GetCategoryLabel(lTypeID),
											 _W("scope/cat:") + cString::ToStringW(lTypeID), true);

		tEntityWrapperListIt eit = cit->second.begin();
		for(; eit != cit->second.end(); ++eit)
			AddSubEntityNode(pCatNode, *eit);
	}
}

//-------------------------------------------------------------------------------

void cLevelEditorWindowHierarchy::AddSubEntityNode(cWidgetTreeNode* apParent, iEntityWrapper* apEnt)
{
	tWString sText = cString::To16Char(apEnt->GetName());
	if(sText.empty())
		sText = _W("#") + cString::ToStringW(apEnt->GetID());

	tWString sKey = _W("scope/sub:") + cString::ToStringW(apEnt->GetID());
	cWidgetTreeNode* pNode = MakeNode(apParent, sText, sKey, false);
	mmapNodeSubEntity[pNode] = apEnt->GetID();
}

//-------------------------------------------------------------------------------

void cLevelEditorWindowHierarchy::UpdateHighlights()
{
	if(mpTree == NULL)
		return;

	std::map<cWidgetTreeNode*, int>::iterator nit = mmapNodeEntity.begin();
	for(; nit != mmapNodeEntity.end(); ++nit)
		nit->first->SetHighlighted(false);

	cEditorSelection* pSelection = mpEditor->GetSelection();
	tIntList& lstIDs = pSelection->GetEntityIDs();

	cWidgetTreeNode* pFirst = NULL;
	tIntListIt it = lstIDs.begin();
	for(; it != lstIDs.end(); ++it)
	{
		std::map<int, cWidgetTreeNode*>::iterator found = mmapEntityNode.find(*it);
		if(found == mmapEntityNode.end())
			continue;

		found->second->SetHighlighted(true);
		if(pFirst == NULL)
			pFirst = found->second;
	}

	// Keep the widget's own single-select pointer in sync (no callback) so a
	// re-click of the same row still fires, and the focus row is consistent.
	mpTree->SelectNode(pFirst, false);
}

//-------------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
// CALLBACKS
////////////////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------------------

bool cLevelEditorWindowHierarchy::Filter_OnTextChange(iWidget* apWidget, const cGuiMessageData& aData)
{
	if(mpFilterGhost)
		mpFilterGhost->SetVisible(mpFilter->GetText().empty());

	RebuildTree();
	return true;
}
kGuiCallbackDeclaredFuncEnd(cLevelEditorWindowHierarchy, Filter_OnTextChange);

//-------------------------------------------------------------------------------

bool cLevelEditorWindowHierarchy::Tree_OnSelectionChange(iWidget* apWidget, const cGuiMessageData& aData)
{
	cWidgetTreeNode* pNode = mpTree->GetSelectedNode();
	if(pNode == NULL)
		return true;

	cLevelEditor* pLevelEditor = (cLevelEditor*)mpEditor;

	//////////////////////////////
	// Scoped-into-entity routing (independent of the map selection).
	if(pLevelEditor->IsEntFileScoped())
	{
		// Breadcrumb row -> back to the map.
		if(pNode == mpScopeExitNode)
		{
			pLevelEditor->ExitEntFileScope();
			return true;
		}

		// Sub-entity row -> select it through the SESSION's Select mode (already the
		// current edit mode while scoped), exactly as a viewport click on its icon
		// would. That select fires the Select mode's own ShowEditBox, which hosts the
		// sub-entity's property panel in the right-pane slot. Category rows do nothing.
		std::map<cWidgetTreeNode*, int>::iterator sit = mmapNodeSubEntity.find(pNode);
		if(sit != mmapNodeSubEntity.end())
		{
			cLevelEditorEntFileSession* pSession = pLevelEditor->GetScopedSession();
			if(pSession)
			{
				int lMod = mpEditor->GetEngine()->GetInput()->GetKeyboard()->GetModifier();
				eSelectActionType type = (lMod & eKeyModifier_Shift) ? eSelectActionType_Toggle : eSelectActionType_Select;

				tIntList lstSubIDs;
				lstSubIDs.push_back(sit->second);
				mpEditor->AddAction(pSession->GetSelectMode()->CreateSelectEntityAction(lstSubIDs, type));
			}
		}

		return true;
	}

	tIntList lstIDs;

	std::map<cWidgetTreeNode*, int>::iterator eit = mmapNodeEntity.find(pNode);
	if(eit != mmapNodeEntity.end())
	{
		lstIDs.push_back(eit->second);
	}
	else
	{
		// A group node selects all its members.
		std::map<cWidgetTreeNode*, int>::iterator git = mmapNodeGroup.find(pNode);
		if(git != mmapNodeGroup.end())
		{
			int lGroupID = git->second;
			tEntityWrapperMap& mapEntities = mpEditor->GetEditorWorld()->GetEntities();
			tEntityWrapperMapIt mit = mapEntities.begin();
			for(; mit != mapEntities.end(); ++mit)
			{
				cLevelEditorEntityExtData* pExt = (cLevelEditorEntityExtData*)mit->second->GetEntityExtData();
				if(pExt && pExt->mlGroupID == lGroupID)
					lstIDs.push_back(mit->second->GetID());
			}
		}
	}

	// Root / category rows are not selectable.
	if(lstIDs.empty())
		return true;

	int lMod = mpEditor->GetEngine()->GetInput()->GetKeyboard()->GetModifier();
	eSelectActionType type = (lMod & eKeyModifier_Shift) ? eSelectActionType_Toggle : eSelectActionType_Select;

	// Act like a viewport click: ensure Select mode is current, then dispatch the
	// selection through the same undoable action the viewport / search window use.
	mpEditor->SetCurrentEditMode(mpEditor->GetEditMode("Select"));
	mpEditor->SetLayoutNeedsUpdate(true);

	cEditorEditModeSelect* pSelectMode = (cEditorEditModeSelect*)mpEditor->GetEditMode("Select");
	mpEditor->AddAction(pSelectMode->CreateSelectEntityAction(lstIDs, type));

	return true;
}
kGuiCallbackDeclaredFuncEnd(cLevelEditorWindowHierarchy, Tree_OnSelectionChange);

//-------------------------------------------------------------------------------

bool cLevelEditorWindowHierarchy::Tree_OnDoubleClick(iWidget* apWidget, const cGuiMessageData& aData)
{
	cWidgetTreeNode* pNode = mpTree->GetSelectedNode();
	if(pNode == NULL)
		return true;

	// Only entity rows focus the camera; the preceding single click already
	// selected the entity.
	if(mmapNodeEntity.find(pNode) == mmapNodeEntity.end())
		return true;

	mpEditor->AddAction(mpEditor->CreateFocusOnSelectionAction());
	return true;
}
kGuiCallbackDeclaredFuncEnd(cLevelEditorWindowHierarchy, Tree_OnDoubleClick);

//-------------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
// IMPLEMENTED FUNCTIONS
////////////////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------------------

void cLevelEditorWindowHierarchy::OnWorldModify()
{
	RebuildTree();
}

//-------------------------------------------------------------------------------

void cLevelEditorWindowHierarchy::OnSelectionChange()
{
	UpdateHighlights();
}

//-------------------------------------------------------------------------------
