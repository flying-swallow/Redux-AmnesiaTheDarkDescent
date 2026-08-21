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

#include "scene/RenderableSet.h"

#include "graphics/Renderable.h"
#include "math/BoundingVolume.h"

#include <algorithm>
#include <cassert>

namespace hpl {

	//-----------------------------------------------------------------------

	void cRenderableSet::Add(iRenderable *apObject)
	{
		assert(apObject);
		cBoundingVolume *pBV = apObject->GetBoundingVolume();
		mvAabbs.push_back({ cMath::ToFloat3(pBV->GetMin()), cMath::ToFloat3(pBV->GetMax()) });
		mvObjects.push_back(apObject);
	}

	//-----------------------------------------------------------------------

	void cRenderableSet::Remove(iRenderable *apObject)
	{
		if(apObject==NULL) return;

		auto it = std::find(mvObjects.begin(), mvObjects.end(), apObject);
		if(it == mvObjects.end()) return;
		const size_t lIdx = it - mvObjects.begin();
		int lLast = (int)mvObjects.size()-1;
		if(lIdx != lLast)
		{
			mvObjects[lIdx] = mvObjects[lLast];
			mvAabbs[lIdx] = mvAabbs[lLast];
		}
		mvObjects.pop_back();
		mvAabbs.pop_back();
	}

	//-----------------------------------------------------------------------

	void cRenderableSet::UpdateBeforeRendering()
	{
		if(mbCompiled) return;

		RefreshAabbs();
	}

	//-----------------------------------------------------------------------

	void cRenderableSet::Compile()
	{
		RefreshAabbs();
		mbCompiled = true;
	}

	//-----------------------------------------------------------------------

	void cRenderableSet::RefreshAabbs()
	{
		for(size_t i=0, n=mvObjects.size(); i<n; ++i)
		{
			cBoundingVolume *pBV = mvObjects[i]->GetBoundingVolume();
			mvAabbs[i].mvMin = cMath::ToFloat3(pBV->GetMin());
			mvAabbs[i].mvMax = cMath::ToFloat3(pBV->GetMax());
		}
	}

	//-----------------------------------------------------------------------

	bool cRenderableSet::GetBounds(cVector3f &avMin, cVector3f &avMax) const
	{
		if(mvAabbs.empty()) return false;

		ml::float3 vMin = mvAabbs[0].mvMin;
		ml::float3 vMax = mvAabbs[0].mvMax;
		for(size_t i=1, n=mvAabbs.size(); i<n; ++i)
		{
			vMin = ml::min(vMin, mvAabbs[i].mvMin);
			vMax = ml::max(vMax, mvAabbs[i].mvMax);
		}
		avMin = cVector3f(vMin.x, vMin.y, vMin.z);
		avMax = cVector3f(vMax.x, vMax.y, vMax.z);
		return true;
	}

	//-----------------------------------------------------------------------
}
