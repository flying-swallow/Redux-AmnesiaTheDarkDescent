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

#include "gui/WidgetMeshObjectList.h"

#include "math/Math.h"
#include "system/String.h"

#include "gui/Gui.h"
#include "gui/GuiSet.h"
#include "gui/GuiSkin.h"
#include "gui/GuiGfxElement.h"

namespace hpl {

	//////////////////////////////////////////////////////////////////////////
	// CONSTRUCTORS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	cWidgetMeshObjectList::cWidgetMeshObjectList(cGuiSet *apSet, cGuiSkin *apSkin) : iWidget(eWidgetType_MeshObjectList,apSet, apSkin)
	{
		mlHoveredItem = -1;
		mlSelectedItem = -1;
	}

	//-----------------------------------------------------------------------

	cWidgetMeshObjectList::~cWidgetMeshObjectList()
	{
		STLDeleteAll(mvItems);
	}

	//-----------------------------------------------------------------------

	iWidgetMeshObjectItem::iWidgetMeshObjectItem(const tWString& asName)
	{
		msName = asName;

		mpList = NULL;
		mpThumbnail = NULL;
	}

	//-----------------------------------------------------------------------

	iWidgetMeshObjectItem::~iWidgetMeshObjectItem()
	{
		DisposeThumbnail();

		mpList = NULL;
		mpThumbnail = NULL;
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PUBLIC METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	void cWidgetMeshObjectList::AddItem(iWidgetMeshObjectItem* apItem)
	{
		int lIdx = (int)mvItems.size();
		mvItems.push_back(apItem);
		apItem->SetList(this);

		if (msFilter.empty()) mvVisualItems.push_back(lIdx);
		else if(cString::GetFirstStringPosW(apItem->GetName(), msFilter) != -1) mvVisualItems.push_back(lIdx);
	}

	//-----------------------------------------------------------------------

	void cWidgetMeshObjectList::ClearItems()
	{
		STLDeleteAll(mvItems);
		mvVisualItems.clear();

		mlSelectedItem = -1;

		UpdateProperties();
	}

	//-----------------------------------------------------------------------

	cGuiGfxElement* iWidgetMeshObjectItem::GetThumbnail()
	{
		if (mpThumbnail) return mpThumbnail;
		// Lazy: LoadThumbnail() may still be pending (async GPU thumbnail) and
		// leave mpThumbnail NULL — re-checked next draw until it resolves.
		LoadThumbnail();
		return mpThumbnail;
	}

	//-----------------------------------------------------------------------

	void cWidgetMeshObjectList::UpdateProperties()
	{
		mfItemLabelPadding = GetDefaultFontSize().y + 2;
		mfItemWidth = (mvSize.x / 2);
		mfItemHeight = mfItemWidth + mfItemLabelPadding;

		int lVisItems = (int)mvVisualItems.size();

		float fRows = floorf(((float)lVisItems - 1) / 2);
		float fTotalContentHeight = mfItemHeight * (fRows + 1);

		SetSize(cVector2f(mvSize.x, fTotalContentHeight));
	}

	//-----------------------------------------------------------------------

	void cWidgetMeshObjectList::SetFilter(const tWString& asFilter)
	{
		msFilter = asFilter;

		mvVisualItems.clear();
		int lItemsNum = (int)mvItems.size();

		if (msFilter.empty())
		{
			for (int i = 0; i < lItemsNum; ++i)
			{
				mvVisualItems.push_back(i);
			}

			return;
		}

		for (int i = 0; i < lItemsNum; ++i)
		{
			iWidgetMeshObjectItem* pItem = mvItems[i];
			if (pItem == NULL)
				continue;

			if (cString::GetFirstStringPosW(pItem->GetName(), msFilter) != -1) mvVisualItems.push_back(i);
		}
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PROTECTED METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	void cWidgetMeshObjectList::OnChangeSize()
	{
		//UpdateProperties();
	}

	//-----------------------------------------------------------------------

	void cWidgetMeshObjectList::OnLoadGraphics()
	{
		mpGfxSelection = mpSkin->GetGfx(eGuiSkinGfx_TextBoxSelectedTextBack);
		// Solid-color quad used for the blank placeholder + selection overlay.
		// FrameBackground is the skin's untextured white rect (color-tinted on
		// draw), the current equivalent of the old GetBlankRect().
		mpGfxBlank = mpSkin->GetGfx(eGuiSkinGfx_FrameBackground);

		SetDefaultFontSize(11);
	}

	//-----------------------------------------------------------------------

	void cWidgetMeshObjectList::OnDraw(float afTimeStep, cGuiClipRegion* apClipRegion)
	{
		cVector3f vPosition = GetGlobalPosition() + cVector3f(0, 0, 0.4f);
		cVector2f vItemSize = cVector2f(mfItemWidth, mfItemHeight);

		int lNumVisItems = (int)mvVisualItems.size();

		for (int i = 0; i < lNumVisItems; ++i)
		{
			int lItemIndex = mvVisualItems[i];
			iWidgetMeshObjectItem* pItem = mvItems[lItemIndex];
			if (pItem == NULL)
				continue;

			float fX = (bool)(i % 2) ? vItemSize.x : 0;
			float fY = vItemSize.y * floorf((float)i / 2);
			if (fY + vPosition.y > apClipRegion->mRect.y + apClipRegion->mRect.h) break;

			cVector3f vItemPosition = vPosition + cVector3f(fX, fY, 0.1f);

			if (!cMath::CheckRectIntersection(apClipRegion->mRect, cRect2f(cVector2f(vItemPosition.x, vItemPosition.y), vItemSize)))
				continue;

			cGuiClipRegion* pRegion = apClipRegion->CreateChild(vItemPosition, vItemSize);
			mpSet->SetCurrentClipRegion(pRegion);

			cGuiGfxElement* pThumbnail = pItem->GetThumbnail();
			if (pThumbnail && pThumbnail->GetTextureNum() > 0)
			{
				mpSet->DrawGfx(pThumbnail,
					vItemPosition + cVector3f(3, 3, 0.1f),
					vItemSize - cVector2f(6, 6 + mfItemLabelPadding));
			}
			else
			{
				mpSet->DrawGfx(mpGfxBlank,
					vItemPosition + cVector3f(3,3,0.1f),
					vItemSize - cVector2f(6, 6 + mfItemLabelPadding),
					cColor(0.7f,0.7f,0.7f));
			}

			if(mlSelectedItem == lItemIndex)
				mpSet->DrawGfx(mpGfxBlank, vItemPosition, vItemSize, cColor(0.5f,0.5f,0.5f, 0.5f));

			DrawDefaultText(pItem->GetName(), vItemPosition + cVector3f(vItemSize.x * 0.5f, vItemSize.y - mfItemLabelPadding, 0.15f), eFontAlign_Center);

			mpSet->SetCurrentClipRegion(apClipRegion);
		}
	}

	//-----------------------------------------------------------------------

	bool cWidgetMeshObjectList::OnMouseMove(const cGuiMessageData& aData)
	{
		cVector3f vLocalPos = WorldToLocalPosition(aData.mvPos);

		int lRow = (int)floorf(vLocalPos.y / mfItemHeight);
		int lColumn = (int)floorf(vLocalPos.x / mfItemWidth);

		mlHoveredItem = (2 * lRow) + lColumn;

		if (mlHoveredItem < 0 || mlHoveredItem >= (int)mvVisualItems.size()) mlHoveredItem = -1;
		else mlHoveredItem = mvVisualItems[mlHoveredItem];

		if (mlHoveredItem > -1) SetToolTip(mvItems[mlHoveredItem]->GetFullPath());
		else SetToolTip(_W(""));

		return true;
	}

	bool cWidgetMeshObjectList::OnMouseDown(const cGuiMessageData& aData)
	{
		if (IsEnabled() && aData.mlVal == eGuiMouseButton_Left)
		{
			mlSelectedItem = mlHoveredItem;

			ProcessMessage(eGuiMessage_SelectionChange, aData);

			return true;
		}
		return false;
	}

	bool cWidgetMeshObjectList::OnMouseEnter(const cGuiMessageData& aData)
	{
		SetToolTipEnabled(true);
		return true;
	}

	bool cWidgetMeshObjectList::OnMouseLeave(const cGuiMessageData& aData)
	{
		mlHoveredItem = -1;
		SetToolTipEnabled(false);
		return true;
	}
}
