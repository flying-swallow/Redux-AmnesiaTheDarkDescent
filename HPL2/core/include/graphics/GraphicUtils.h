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

#ifndef HPL_GRAPHIUTILS_H
#define HPL_GRAPHIUTILS_H

#include <functional>
#include <span>

#include "graphics/GraphicsTypes.h"
#include "math/MathTypes.h"

namespace hpl {

	class iRenderable;
	class iRenderableContainer;
	class iRenderableContainerNode;
	class cRenderList;
	class cFrustum;

	namespace rendering {

		bool IsObjectIsVisible(iRenderable* apObject, tRenderableFlag neededFlags,
			std::span<cPlanef> clipPlanes = {});

		bool IsRenderableNodeIsVisible(iRenderableContainerNode* apNode,
			std::span<cPlanef> clipPlanes);

		cRect2l GetClipRectFromObject(iRenderable* apObject, float afPaddingPercent,
			cFrustum* apFrustum, const cVector2l& avScreenSize, float afHalfFovTan);

		void WalkAndPrepareRenderList(iRenderableContainer* container, cFrustum* frustum,
			std::function<void(iRenderable*)> handler, tRenderableFlag renderableFlag);

		void UpdateRenderListWalkAllNodesTestFrustumAndVisibility(
			cRenderList* apRenderList, cFrustum* frustum,
			iRenderableContainerNode* apNode,
			std::span<cPlanef> clipPlanes, tRenderableFlag neededFlags);

		void UpdateRenderListWalkAllNodesTestFrustumAndVisibility(
			cRenderList* apRenderList, cFrustum* frustum,
			iRenderableContainer* apContainer,
			std::span<cPlanef> clipPlanes, tRenderableFlag neededFlags);

	} // namespace rendering

} // namespace hpl

#endif // HPL_GRAPHIUTILS_H
