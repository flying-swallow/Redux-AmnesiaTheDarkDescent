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

#include "gui/WidgetNodeTree.h"

#include "math/Math.h"

#include "gui/GuiSet.h"
#include "gui/GuiSkin.h"
#include "gui/GuiGfxElement.h"

namespace hpl {

	//////////////////////////////////////////////////////////////////////////
	// CONSTRUCTORS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	cWidgetNodeTree::cWidgetNodeTree(cGuiSet *apSet, cGuiSkin *apSkin) : iWidget(eWidgetType_NodeTree,apSet, apSkin)
	{
		mpSelectedNode = NULL;
		mpFocusedNode = NULL;
	}

	cWidgetTreeNode::cWidgetTreeNode(cWidgetNodeTree* apNodeTree)
	{
		mpNodeTree = apNodeTree;

		mbExtended = false;

		mfNodeHeight = 16;
		mfNodeIndentation = 0;

		mpParentNode = NULL;
	}

	//-----------------------------------------------------------------------

	cWidgetNodeTree::~cWidgetNodeTree()
	{
		STLDeleteAll(mvNodes);
	}

	cWidgetTreeNode::~cWidgetTreeNode()
	{
		STLDeleteAll(mvChildNodes);
		mpNodeTree = NULL;
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PUBLIC METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	void cWidgetTreeNode::UpdateNodeHeight()
	{
		if (mbExtended)
		{
			float fNodeHeight = 16;

			for (int i = 0; i < (int)mvChildNodes.size(); ++i)
			{
				cWidgetTreeNode* pItem = mvChildNodes[i];
				if (pItem == NULL)
					continue;

				fNodeHeight += pItem->GetNodeHeight();
			}

			SetNodeHeight(fNodeHeight);
		}
		else
		{
			SetNodeHeight(16);
		}

		if (mpParentNode) mpParentNode->UpdateNodeHeight();
	}

	//-----------------------------------------------------------------------

	void cWidgetTreeNode::SetExtended(bool abX)
	{
		mbExtended = abX;

		UpdateNodeHeight();
		mpNodeTree->UpdateTreeHeight();
	}

	//-----------------------------------------------------------------------

	void cWidgetNodeTree::SelectNode(cWidgetTreeNode* apNode, bool abGenCallback)
	{
		mpFocusedNode = apNode;
		if (mpSelectedNode == apNode) return;
		mpSelectedNode = apNode;

		cGuiMessageData data;
		if (abGenCallback)
			ProcessMessage(eGuiMessage_SelectionChange, data);
	}

	void cWidgetTreeNode::SetSelected(bool abX, bool abGenCallback)
	{
		mpNodeTree->SelectNode(abX ? this : NULL, abGenCallback);
	}

	//-----------------------------------------------------------------------

	void cWidgetNodeTree::AddChildNode(cWidgetTreeNode* apChildNode)
	{
		mvNodes.push_back(apChildNode);

		UpdateTreeHeight();
	}

	void cWidgetTreeNode::AddChildNode(cWidgetTreeNode* apChildNode)
	{
		mvChildNodes.push_back(apChildNode);
		apChildNode->SetParentNode(this);

		UpdateNodeHeight();
		mpNodeTree->UpdateTreeHeight();
	}

	//-----------------------------------------------------------------------

	void cWidgetNodeTree::RemoveChildNode(cWidgetTreeNode* apNode, bool abDelete)
	{
		if (mpSelectedNode == apNode) mpSelectedNode = NULL;
		if (mpFocusedNode == apNode) mpFocusedNode = NULL;

		tWidgetTreeNodeVecIt it = mvNodes.begin();
		for (; it != mvNodes.end(); ++it)
		{
			if (*it == apNode)
			{
				apNode->SetParentNode(NULL);
				if (abDelete) hplDelete(*it);
				mvNodes.erase(it);
				break;
			}
		}

		UpdateTreeHeight();
	}

	void cWidgetTreeNode::RemoveChildNode(cWidgetTreeNode* apNode, bool abDelete)
	{
		tWidgetTreeNodeVecIt it = mvChildNodes.begin();
		for (; it != mvChildNodes.end(); ++it)
		{
			if (*it == apNode)
			{
				apNode->SetParentNode(NULL);
				if (abDelete) hplDelete(*it);
				mvChildNodes.erase(it);
				break;
			}
		}

		UpdateNodeHeight();
		mpNodeTree->UpdateTreeHeight();
	}

	//-----------------------------------------------------------------------

	cWidgetTreeNode* cWidgetNodeTree::AddTreeNode(const tWString& asNode)
	{
		cWidgetTreeNode* pNode = hplNew(cWidgetTreeNode,(this));
		pNode->SetText(asNode);
		AddChildNode(pNode);

		return pNode;
	}

	cWidgetTreeNode* cWidgetTreeNode::AddTreeNode(const tWString& asNode)
	{
		cWidgetTreeNode* pNode = hplNew(cWidgetTreeNode, (mpNodeTree));
		pNode->SetText(asNode);
		AddChildNode(pNode);

		return pNode;
	}

	//-----------------------------------------------------------------------

	void cWidgetNodeTree::ClearTreeNodes()
	{
		// The selected/focused node is owned by the hierarchy being deleted.
		mpSelectedNode = NULL;
		mpFocusedNode = NULL;

		STLDeleteAll(mvNodes);

		UpdateTreeHeight();
	}

	void cWidgetTreeNode::ClearTreeNodes()
	{
		STLDeleteAll(mvChildNodes);

		UpdateNodeHeight();
		mpNodeTree->UpdateTreeHeight();
	}

	//-----------------------------------------------------------------------

	void cWidgetNodeTree::UpdateTreeHeight()
	{
		float fTotalHeight = 0;
		for (int i = 0; i < (int)mvNodes.size(); ++i)
		{
			cWidgetTreeNode* pItem = mvNodes[i];
			if (pItem == NULL)
				continue;

			fTotalHeight += pItem->GetNodeHeight();
		}

		SetSize(cVector2f(mvSize.x, fTotalHeight));
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PROTECTED METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	void cWidgetNodeTree::OnDraw(float afTimeStep, cGuiClipRegion* apClipRegion)
	{
		cVector3f vPos = GetGlobalPosition();

		for (int i = 0; i < (int)mvNodes.size(); ++i)
		{
			cWidgetTreeNode* pItem = mvNodes[i];
			if (pItem == NULL)
				continue;

			DrawTreeNode(pItem, vPos);
			vPos.y += pItem->GetNodeHeight();
		}
	}

	//-----------------------------------------------------------------------

	void cWidgetNodeTree::OnLoadGraphics()
	{
		mpGfxBackground = mpSkin->GetGfx(eGuiSkinGfx_FrameBackground);
		mpGfxButtonBackground = mpSkin->GetGfx(eGuiSkinGfx_ButtonUpBackground);

		mvGfxButtonBorders[0] = mpSkin->GetGfx(eGuiSkinGfx_ButtonUpBorderRight);
		mvGfxButtonBorders[1] = mpSkin->GetGfx(eGuiSkinGfx_ButtonUpBorderLeft);
		mvGfxButtonBorders[2] = mpSkin->GetGfx(eGuiSkinGfx_ButtonUpBorderUp);
		mvGfxButtonBorders[3] = mpSkin->GetGfx(eGuiSkinGfx_ButtonUpBorderDown);

		mvGfxButtonCorners[0] = mpSkin->GetGfx(eGuiSkinGfx_ButtonUpCornerLU);
		mvGfxButtonCorners[1] = mpSkin->GetGfx(eGuiSkinGfx_ButtonUpCornerRU);
		mvGfxButtonCorners[2] = mpSkin->GetGfx(eGuiSkinGfx_ButtonUpCornerRD);
		mvGfxButtonCorners[3] = mpSkin->GetGfx(eGuiSkinGfx_ButtonUpCornerLD);
	}

	//-----------------------------------------------------------------------

	bool cWidgetNodeTree::OnMouseDown(const cGuiMessageData& aData)
	{
		if (IsEnabled() && aData.mlVal == eGuiMouseButton_Left)
		{
			float fHeight = GetGlobalPosition().y;

			for (int i = 0; i < (int)mvNodes.size(); ++i)
			{
				cWidgetTreeNode* pItem = mvNodes[i];
				if (pItem == NULL)
					continue;

				ProcessNodeClick(pItem, fHeight, aData.mvPos);
				fHeight += pItem->GetNodeHeight();
			}

			return true;
		}
		return false;
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// OWN METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	void cWidgetNodeTree::DrawTreeNode(cWidgetTreeNode* apNode, const cVector3f avPos)
	{
		cVector3f vPos = cVector3f(avPos.x, avPos.y, avPos.z + 0.1f);
		cVector2f vSize = cVector2f(mvSize.x, 16);

		if (apNode->GetParentNode())
		{
			apNode->SetNodeIndentation(apNode->GetParentNode()->GetNodeIndentation() + 11);
		}

		float fIndentation = apNode->GetNodeIndentation();

		/////////////////////////
		// Highlight stuff
		if (mpFocusedNode == apNode)
		{
			mpSet->DrawGfx(mpGfxBackground, vPos,
				vSize, cColor(0.6f, 0.6f, 0.6f, 0.5f));
		}
		if (mpSelectedNode && mpSelectedNode == apNode)
		{
			mpSet->DrawGfx(mpGfxBackground, vPos + cVector3f(fIndentation + 12, 0, 0),
				vSize - cVector2f(fIndentation + 12, 0), cColor(0.438f, 0.598f, 1, 1));
		}

		tWidgetTreeNodeVec& vpChildren = apNode->GetChildNodes();

		/////////////////////////
		// Extend button
		if (vpChildren.empty() == false)
		{
			DrawBordersAndCorners(mpGfxButtonBackground, mvGfxButtonBorders, mvGfxButtonCorners,
				vPos + cVector3f(fIndentation, 3, 0.1f),
				cVector2f(10, 10));

			DrawDefaultText(apNode->IsExtended() ? _W("-") : _W("+"), vPos + cVector3f(fIndentation + 5, 0, 0.2f), eFontAlign_Center);
		}

		mpSet->DrawFont(apNode->GetText(), mpDefaultFontType, vPos + cVector3f(fIndentation + 17, 1.25f, 0.1f), mvDefaultFontSize * 0.875f, mDefaultFontColor * mColorMul, eFontAlign_Left);

		if (apNode->IsExtended() == false) return;

		float fNodeHeight = 16;

		for (int i = 0; i < (int)vpChildren.size(); ++i)
		{
			cWidgetTreeNode* pItem = vpChildren[i];
			if (pItem == NULL)
				continue;

			DrawTreeNode(pItem, cVector3f(vPos.x, vPos.y + fNodeHeight, vPos.z));
			fNodeHeight += pItem->GetNodeHeight();
		}
	}

	void cWidgetNodeTree::ProcessNodeClick(cWidgetTreeNode* apNode, float afHeight, const cVector2f avMouse)
	{
		if (avMouse.y < afHeight) return;

		tWidgetTreeNodeVec& vpChildren = apNode->GetChildNodes();
		bool bHasChildren = (vpChildren.empty() == false);

		if (avMouse.y <= afHeight + 16)
		{
			float fIndentation = apNode->GetNodeIndentation();

			if (bHasChildren)
			{
				//calc for extend/retract button
				float fButtonX = (mvGlobalPosition.x + fIndentation);
				float fButtonY = (afHeight + 3);

				if (avMouse.x >= fButtonX		&&
					avMouse.x < (fButtonX + 10) &&
					avMouse.y >= fButtonY		&&
					avMouse.y < (fButtonY + 10)	)
				{
					mpFocusedNode = apNode;
					apNode->SetExtended(!apNode->IsExtended());
					return;
				}
			}

			if (avMouse.y >= afHeight && avMouse.y < afHeight + 16)
			{
				mpFocusedNode = apNode;

				if (avMouse.x >= mvGlobalPosition.x + fIndentation + 12)
					SelectNode(apNode, true);

				return;
			}
		}

		if (!bHasChildren || !apNode->IsExtended()) return;

		float fNextHeight = afHeight + 16;

		for (int i = 0; i < (int)vpChildren.size(); ++i)
		{
			cWidgetTreeNode* pItem = vpChildren[i];
			if (pItem == NULL)
				continue;

			ProcessNodeClick(pItem, fNextHeight, avMouse);
			fNextHeight += pItem->GetNodeHeight();
		}
	}
}
