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

#ifndef HPL_WIDGET_MESH_OBJECT_LIST_H
#define HPL_WIDGET_MESH_OBJECT_LIST_H

#include "gui/Widget.h"

namespace hpl {

	class iWidgetMeshObjectItem;

	typedef std::vector<iWidgetMeshObjectItem*> tWidgetMeshObjectItemVec;
	typedef tWidgetMeshObjectItemVec::iterator tWidgetMeshObjectItemVecIt;

	//------------------------------------------------------------------
	// Thumbnail grid widget: 2-column scrolling grid of mesh-object items,
	// each drawing a lazily-resolved thumbnail (cGuiGfxElement*) + name label,
	// with hover tooltip, click selection and a text filter. Ported from the
	// Remodded2 editor UI; the per-item thumbnail source is supplied by the
	// item subclass's LoadThumbnail() override (see EditorWindowObjectBrowser).
	//------------------------------------------------------------------

	class cWidgetMeshObjectList : public iWidget
	{
	public:
		cWidgetMeshObjectList(cGuiSet *apSet, cGuiSkin *apSkin);
		~cWidgetMeshObjectList();

		void AddItem(iWidgetMeshObjectItem* apItem);

		void ClearItems();

		void UpdateProperties();

		void SetFilter(const tWString& asFilter);
		const tWString& GetFilter() { return msFilter; }

		int GetSelectedItemIdx() { return mlSelectedItem; }
		void SetSelectedItemIdx(int alIdx) { mlSelectedItem = alIdx; }

	protected:
		/////////////////////////
		// Implemented functions
		void OnChangeSize();
		void OnLoadGraphics();

		void OnDraw(float afTimeStep, cGuiClipRegion* apClipRegion);

		bool OnMouseMove(const cGuiMessageData& aData);
		bool OnMouseDown(const cGuiMessageData& aData);
		bool OnMouseEnter(const cGuiMessageData& aData);
		bool OnMouseLeave(const cGuiMessageData& aData);

		/////////////////////////
		// Data
		tWString msFilter;

		float mfItemWidth;
		float mfItemHeight;
		float mfItemLabelPadding;

		int mlHoveredItem;
		int mlSelectedItem;

		cGuiGfxElement* mpGfxSelection;
		cGuiGfxElement* mpGfxBlank;

		tWidgetMeshObjectItemVec mvItems;
		tIntVec mvVisualItems;
	};

	//------------------------------------------------------------------

	class iWidgetMeshObjectItem
	{
	public:
		iWidgetMeshObjectItem(const tWString& asName);
		virtual ~iWidgetMeshObjectItem();

		void SetList(cWidgetMeshObjectList* apList) { mpList = apList; }

		void SetName(const tWString& asName) { msName = asName; }
		const tWString& GetName() { return msName; }

		void SetFullPath(const tWString& asFullPath) { msFullPath = asFullPath; }
		const tWString& GetFullPath() { return msFullPath; }

		virtual void LoadThumbnail(){}
		virtual void DisposeThumbnail(){}

		cGuiGfxElement* GetThumbnail();

	protected:
		/////////////////////////
		// Data
		tWString msName;
		tWString msFullPath;

		cWidgetMeshObjectList* mpList;
		cGuiGfxElement* mpThumbnail;
	};

};
#endif // HPL_WIDGET_MESH_OBJECT_LIST_H
