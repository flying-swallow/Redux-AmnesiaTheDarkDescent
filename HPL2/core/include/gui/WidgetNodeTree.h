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

#ifndef HPL_WIDGET_NODE_TREE_H
#define HPL_WIDGET_NODE_TREE_H

#include "gui/Widget.h"

namespace hpl {

	class cWidgetTreeNode;

	typedef std::vector<cWidgetTreeNode*> tWidgetTreeNodeVec;
	typedef tWidgetTreeNodeVec::iterator tWidgetTreeNodeVecIt;

	// Hierarchical expand/collapse tree (HPL3-style object browser folders).
	// The widget owns its cWidgetTreeNode hierarchy; node rows are a fixed 16
	// units high and child rows indent under their parent's expand button.
	// Selecting a row fires eGuiMessage_SelectionChange. Ported from the
	// Remodded2 fork (NB-NutBoi).
	class cWidgetNodeTree : public iWidget
	{
	public:
		cWidgetNodeTree(cGuiSet *apSet, cGuiSkin *apSkin);
		virtual ~cWidgetNodeTree();

		void SelectNode(cWidgetTreeNode* apNode, bool abGenCallback = false);
		cWidgetTreeNode* GetSelectedNode() { return mpSelectedNode; }

		void FocusNode(cWidgetTreeNode* apNode) { mpFocusedNode = apNode; }
		cWidgetTreeNode* GetFocusedNode() { return mpFocusedNode; }

		void AddChildNode(cWidgetTreeNode* apChildNode);
		void RemoveChildNode(cWidgetTreeNode* apNode, bool abDelete = false);

		cWidgetTreeNode* AddTreeNode(const tWString& asNode);
		void ClearTreeNodes();

		void UpdateTreeHeight();

	protected:
		/////////////////////////
		// Own Funcs
		void DrawTreeNode(cWidgetTreeNode* apNode, const cVector3f avPos);
		void ProcessNodeClick(cWidgetTreeNode* apNode, float afHeight, const cVector2f avMouse);

		/////////////////////////
		// Implemented functions
		void OnDraw(float afTimeStep, cGuiClipRegion* apClipRegion);

		void OnLoadGraphics();

		bool OnMouseDown(const cGuiMessageData& aData);

		/////////////////////////
		// Data
		tWidgetTreeNodeVec mvNodes;
		cWidgetTreeNode* mpSelectedNode;
		cWidgetTreeNode* mpFocusedNode;

		cGuiGfxElement* mpGfxBackground;
		cGuiGfxElement* mpGfxButtonBackground;
		cGuiGfxElement* mvGfxButtonBorders[4];
		cGuiGfxElement* mvGfxButtonCorners[4];
	};

	class cWidgetTreeNode
	{
	public:
		cWidgetTreeNode(cWidgetNodeTree* apNodeTree);
		virtual ~cWidgetTreeNode();

		void UpdateNodeHeight();

		void SetText(const tWString& asText) { msText = asText; }
		const tWString& GetText() { return msText; }

		void SetName(const tWString& asName) { msName = asName; }
		const tWString& GetName() { return msName; }

		void SetExtended(bool abX);
		bool IsExtended() { return mbExtended; }

		void SetSelected(bool abX, bool abGenCallback = false);

		// Extra selection highlight independent of the tree's single mpSelectedNode,
		// so a client (e.g. the level-editor hierarchy) can mirror a multi-entity
		// selection. Purely visual; does not affect click routing. Default off, so
		// existing single-select users (object browser) are unaffected.
		void SetHighlighted(bool abX) { mbHighlighted = abX; }
		bool IsHighlighted() { return mbHighlighted; }

		void SetParentNode(cWidgetTreeNode* apNode) { mpParentNode = apNode; }
		cWidgetTreeNode* GetParentNode() { return mpParentNode; }

		void SetNodeHeight(float afX) { mfNodeHeight = afX; }
		float GetNodeHeight() { return mfNodeHeight; }
		void SetNodeIndentation(float afX) { mfNodeIndentation = afX; }
		float GetNodeIndentation() { return mfNodeIndentation; }

		void AddChildNode(cWidgetTreeNode* apChildNode);
		void RemoveChildNode(cWidgetTreeNode* apNode, bool abDelete = false);
		tWidgetTreeNodeVec& GetChildNodes() { return mvChildNodes; }

		cWidgetTreeNode* AddTreeNode(const tWString& asNode);
		void ClearTreeNodes();

	protected:
		/////////////////////////
		// Data
		cWidgetNodeTree* mpNodeTree;

		cWidgetTreeNode* mpParentNode;
		tWidgetTreeNodeVec mvChildNodes;

		bool mbExtended;
		bool mbHighlighted;

		float mfNodeHeight;
		float mfNodeIndentation;

		tWString msName;
		tWString msText;
	};

};
#endif // HPL_WIDGET_NODE_TREE_H
