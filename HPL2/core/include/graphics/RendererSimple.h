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

#ifndef HPL_RENDERER_SIMPLE_H
#define HPL_RENDERER_SIMPLE_H

#include "graphics/Renderer.h"
#include "graphics/RenderList2.h"
#include "graphics/RIProgram.h"

namespace hpl {

    //---------------------------------------------

	// Unlit textured forward renderer — the editor/debug "shaded but cheap"
	// view between WireFrame and the full Hybrid path. Same Draw() frame
	// skeleton as cRendererWireFrame (pogo buffer + per-draw UBO), plus
	// per-draw diffuse texture binding and per-material blend variants.
	class cRendererSimple : public  iRenderer
	{
	public:
		cRendererSimple(cGraphics *apGraphics,cResources* apResources);

		bool LoadData() override { return true; }
		void DestroyData() override {}

		virtual void Draw(
				RIBootstrap::FrameContext* cntx,
				cViewport* viewport,
				float afFrameTime,
				cFrustum* apFrustum,
				cWorld* apWorld,
				cRenderSettings* apSettings,
				bool abSendFrameBufferToPostEffects) override;

	private:
		// Pure virtuals from the legacy GL chain — never invoked; Draw() owns rendering.
		void CopyToFrameBuffer() override {}
		void SetupRenderList() override {}
		void RenderObjects() override {}

		RIProgram m_simple;
		cRenderList2 m_rendererList;
	};

	//---------------------------------------------

};
#endif // HPL_RENDERER_SIMPLE_H
