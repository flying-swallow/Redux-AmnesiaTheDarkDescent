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

#include "graphics/GraphicUtils.h"

#include <cmath>

#include "graphics/RenderList.h"
#include "graphics/Renderable.h"
#include "math/BoundingVolume.h"
#include "math/Frustum.h"
#include "math/Math.h"
#include "scene/RenderableContainer.h"

namespace hpl {
	namespace rendering {

		bool IsObjectIsVisible(iRenderable* apObject, tRenderableFlag neededFlags, std::span<cPlanef> clipPlanes) {
			if (apObject->IsVisible() == false)
				return false;

			if ((apObject->GetRenderFlags() & neededFlags) != neededFlags)
				return false;

			if (!clipPlanes.empty()) {
				cBoundingVolume* pBV = apObject->GetBoundingVolume();
				for (auto& plane : clipPlanes) {
					if (cMath::CheckPlaneBVCollision(plane, *pBV) == eCollision_Outside) {
						return false;
					}
				}
			}
			return true;
		}

		bool IsRenderableNodeIsVisible(iRenderableContainerNode* apNode, std::span<cPlanef> clipPlanes) {
			for (auto& plane : clipPlanes) {
				if (cMath::CheckPlaneAABBCollision(plane, apNode->GetMin(), apNode->GetMax(),
						apNode->GetCenter(), apNode->GetRadius()) == eCollision_Outside) {
					return false;
				}
			}
			return true;
		}

		cRect2l GetClipRectFromObject(iRenderable* apObject, float afPaddingPercent, cFrustum* apFrustum,
			const cVector2l& avScreenSize, float afHalfFovTan) {
			cBoundingVolume* pBV = apObject->GetBoundingVolume();

			cRect2l clipRect;
			if (afHalfFovTan == 0)
				afHalfFovTan = std::tan(apFrustum->GetFOV() * 0.5f);
			cMath::GetClipRectFromBV(clipRect, *pBV, apFrustum, avScreenSize, afHalfFovTan);

			// Add 20% padding on clip rect
			int lXInc = (int)((float)clipRect.w * afHalfFovTan);
			int lYInc = (int)((float)clipRect.h * afHalfFovTan);

			clipRect.x = cMath::Max(clipRect.x - lXInc, 0);
			clipRect.y = cMath::Max(clipRect.y - lYInc, 0);
			clipRect.w = cMath::Min(clipRect.w + lXInc * 2, avScreenSize.x - clipRect.x);
			clipRect.h = cMath::Min(clipRect.h + lYInc * 2, avScreenSize.y - clipRect.y);

			return clipRect;
		}

		void WalkAndPrepareRenderList(iRenderableContainer* container, cFrustum* frustum,
			std::function<void(iRenderable*)> handler, tRenderableFlag renderableFlag) {

			std::function<void(iRenderableContainerNode * childNode)> walkRenderables;
			walkRenderables = [&](iRenderableContainerNode* parentNode) {
				parentNode->UpdateBeforeUse();
				for (auto& childNode : parentNode->GetChildNodes()) {
					childNode->UpdateBeforeUse();
					eCollision frustumCollision = frustum->CollideNode(childNode);
					if (frustumCollision == eCollision_Outside) {
						continue;
					}
					if (frustum->CheckAABBNearPlaneIntersection(childNode->GetMin(), childNode->GetMax())) {
						cVector3f vViewSpacePos = cMath::MatrixMul(frustum->GetViewMatrix(), childNode->GetCenter());
						childNode->SetViewDistance(vViewSpacePos.z);
						childNode->SetInsideView(true);
					} else {
						// Frustum origin is outside of node. Do intersection test.
						cVector3f vIntersection;
						cMath::CheckAABBLineIntersection(
							childNode->GetMin(), childNode->GetMax(), frustum->GetOrigin(), childNode->GetCenter(), &vIntersection, NULL);
						cVector3f vViewSpacePos = cMath::MatrixMul(frustum->GetViewMatrix(), vIntersection);
						childNode->SetViewDistance(vViewSpacePos.z);
						childNode->SetInsideView(false);
					}
					walkRenderables(childNode);
				}
				for (auto& pObject : parentNode->GetObjects()) {
					if (!IsObjectIsVisible(pObject, renderableFlag, {})) {
						continue;
					}
					handler(pObject);
				}
			};
			auto rootNode = container->GetRoot();
			rootNode->UpdateBeforeUse();
			rootNode->SetInsideView(true);
			walkRenderables(rootNode);
		}

		void UpdateRenderListWalkAllNodesTestFrustumAndVisibility(
			cRenderList* apRenderList,
			cFrustum* frustum,
			iRenderableContainerNode* apNode,
			std::span<cPlanef> clipPlanes,
			tRenderableFlag neededFlags) {
			apNode->UpdateBeforeUse();

			///////////////////////////////////////
			// Get frustum collision, if previous was inside, then this is too!
			eCollision frustumCollision = frustum->CollideNode(apNode);

			////////////////////////////////
			// Do a visible check but always iterate the root node!
			if (apNode->GetParent()) {
				if (frustumCollision == eCollision_Outside) {
					return;
				}
				if (IsRenderableNodeIsVisible(apNode, clipPlanes) == false) {
					return;
				}
			}

			////////////////////////
			// Iterate children
			if (apNode->HasChildNodes()) {
				for (auto& node : apNode->GetChildNodes()) {
					UpdateRenderListWalkAllNodesTestFrustumAndVisibility(apRenderList, frustum, node, clipPlanes, neededFlags);
				}
			}

			/////////////////////////////
			// Iterate objects
			if (apNode->HasObjects()) {
				for (auto& object : apNode->GetObjects()) {
					if (IsObjectIsVisible(object, neededFlags) == false) {
						continue;
					}

					if (frustumCollision == eCollision_Inside || object->CollidesWithFrustum(frustum)) {
						apRenderList->AddObject(object);
					}
				}
			}
		}

		void UpdateRenderListWalkAllNodesTestFrustumAndVisibility(
			cRenderList* apRenderList,
			cFrustum* frustum,
			iRenderableContainer* apContainer,
			std::span<cPlanef> clipPlanes,
			tRenderableFlag neededFlags) {
			apContainer->UpdateBeforeRendering();

			UpdateRenderListWalkAllNodesTestFrustumAndVisibility(
				apRenderList,
				frustum,
				apContainer->GetRoot(),
				clipPlanes,
				neededFlags);
		}

	} // namespace rendering
} // namespace hpl
