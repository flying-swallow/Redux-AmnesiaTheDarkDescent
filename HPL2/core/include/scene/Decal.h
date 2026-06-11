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

#ifndef HPL_DECAL_H
#define HPL_DECAL_H

#include "graphics/GraphicsTypes.h"
#include "graphics/Renderable.h"
#include "graphics/Graphics.h"
#include "graphics/Color.h"
#include "math/MathTypes.h"

namespace hpl {

	//------------------------------------------

	class cMaterial;
	class cFrustum;

	//------------------------------------------

	// Which receiver categories a decal projects onto. Replaces the editor's
	// edit-time IsAffectedByDecal mesh-clipping (OnStatic/OnPrimitive/OnEntity):
	// the projection shader now tests the hit object's category against this
	// mask instead. Bit values are mirrored on the GPU side (see Constants.h).
	enum eDecalReceiver
	{
		eDecalReceiver_Static    = 0x1,
		eDecalReceiver_Primitive = 0x2,
		eDecalReceiver_Entity    = 0x4,
		eDecalReceiver_All       = 0x7,
	};

	//------------------------------------------

	// Oriented-box (OOB) decal renderable. Unlike the legacy decals this carries
	// no geometry: it is just the decal's oriented box (the iEntity3D world
	// matrix maps a unit cube [-0.5,0.5]^3 to the box), plus the decal material
	// and tint. The renderer packs these into a GPU array + grid and projects
	// them in screen space — see HybridRenderer / DecalPass. GetVertexBuffer()
	// returns NULL so the old mesh-decal raster loop skips it.
	class cDecal : public iRenderable
	{
	public:
		cDecal(const tString& asName, cGraphics* apGraphics, cMaterial* apMaterial,
			   const cColor& aColor, const cVector2l& avSubDiv);
		virtual ~cDecal();

		//////////////////////////////
		//iEntity implementation
		tString GetEntityType(){ return "cDecal"; }

		///////////////////////////////
		//Renderable implementation:
		cMaterial *GetMaterial(){ return mpMaterial; }
		cVertexBuffer* GetVertexBuffer(){ return NULL; }

		eRenderableType GetRenderType(){ return eRenderableType_Decal; }

		int GetMatrixUpdateCount(){ return GetTransformUpdateCount(); }
		cMatrixf* GetModelMatrix(cFrustum* apFrustum);

		///////////////////////////////
		//Decal data
		const cColor& GetDecalColor() const { return mColor; }
		const cVector2l& GetSubDiv() const { return mvSubDiv; }

		// Selected atlas cell within the SubDiv grid (editor CurrentSubDiv,
		// 0-based). The projection pass remaps UVs into this cell.
		void SetCurrentSubDiv(int alX){ mlCurrentSubDiv = alX; }
		int GetCurrentSubDiv() const { return mlCurrentSubDiv; }

		// Receiver-category mask (eDecalReceiver bits) — which object categories
		// this decal is allowed to project onto.
		void SetReceiverMask(int alMask){ mlReceiverMask = alMask; }
		int GetReceiverMask() const { return mlReceiverMask; }

	private:
		cGraphics* mpGraphics;
		cMaterial* mpMaterial;
		cColor mColor;
		cVector2l mvSubDiv;
		int mlCurrentSubDiv = 0;
		int mlReceiverMask;

		cMatrixf m_mtxModelOutput;
	};

};
#endif // HPL_DECAL_H
