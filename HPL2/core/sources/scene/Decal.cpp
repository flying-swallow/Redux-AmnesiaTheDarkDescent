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

#include "scene/Decal.h"

#include "scene/Scene.h"
#include "graphics/HybridRenderer.h"
#include "graphics/RenderList2.h"

namespace hpl {

	//////////////////////////////////////////////////////////////////////////
	// CONSTRUCTORS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	cDecal::cDecal(const tString& asName, cGraphics* apGraphics, cMaterial* apMaterial,
				   const cColor& aColor, const cVector2l& avSubDiv) : iRenderable(asName)
	{
		mpGraphics = apGraphics;
		mpMaterial = apMaterial;
		mColor     = aColor;
		mvSubDiv   = avSubDiv;
		mlReceiverMask = eDecalReceiver_All;

		// Decals are placed at map load and never move.
		mbStatic = true;

		// The bounding volume is the oriented box: a unit cube that the world
		// matrix (translate * rotate * scale) maps onto the decal volume. The
		// container/grid use this for culling and binning.
		mbApplyTransformToBV = true;
		mBoundingVolume.SetSize(cVector3f(1.0f));
	}

	//-----------------------------------------------------------------------

	cDecal::~cDecal()
	{
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PUBLIC METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	cMatrixf* cDecal::GetModelMatrix(cFrustum* apFrustum)
	{
		m_mtxModelOutput = GetWorldMatrix();

		return &m_mtxModelOutput;
	}

	//-----------------------------------------------------------------------

}
