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

#include "graphics/RenderFunctions.h"

#include "graphics/RenderList.h"
#include "graphics/FrameBuffer.h"
#include "graphics/VertexBuffer.h"
#include "graphics/LowLevelGraphics.h"
#include "graphics/Texture.h"
#include "graphics/Graphics.h"

#include "system/LowLevelSystem.h"

#include "math/Math.h"
#include "math/Frustum.h"

namespace hpl {

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PUBLIC METHODS
	//////////////////////////////////////////////////////////////////////////
	
	//-----------------------------------------------------------------------

	void iRenderFunctions::SetupRenderFunctions(iLowLevelGraphics *apLowLevelGraphics)
	{
		mpLowLevelGraphics = apLowLevelGraphics;

		mvScreenSize = mpLowLevelGraphics->GetScreenSizeInt();
		mvScreenSizeFloat = mpLowLevelGraphics->GetScreenSizeFloat();
	}
	
	void iRenderFunctions::InitAndResetRenderFunctions(cFrustum *apFrustum, cRenderTarget *apRenderTarget, bool abLog, bool abUseGlobalScissorRect, const cVector2l& avGlobalScissorRectPos, const cVector2l& avGlobalScissorRectSize)
	{
		mpCurrentFrustum = apFrustum;
		mpCurrentRenderTarget = apRenderTarget;
		mbLog = abLog;

		////////////////////////////////
		// Set up variables
		mpCurrentProjectionMatrix = NULL;
		mbInvertCullMode = false;
		mbCurrentDepthTest = true;
		mbCurrentDepthWrite = true;
		mbCurrentStencilActive = false;
		mvCurrentScissorRectPos =0;
		mvCurrentScissorRectSize = -1;
		mbCurrentScissorActive = false;
		mCurrentDepthTestFunc = eDepthTestFunc_LessOrEqual;
		mbCurrentCullActive = true;
		mCurrentCullMode = eCullMode_CounterClockwise;
		mCurrentChannelMode = eMaterialChannelMode_RGBA;
		mCurrentAlphaMode = eMaterialAlphaMode_LastEnum;
		mfCurrentAlphaLimit = -1;
		mCurrentBlendMode = eMaterialBlendMode_LastEnum;
		mpCurrentVtxBuffer = NULL;
		mpCurrentMatrix = &m_mtxNULL;
		mpCurrentMaterial = NULL;
		mpCurrentMaterialType = NULL;

		for(int i=0; i<kMaxTextureUnits; ++i) mvCurrentTexture[i] = NULL;

		////////////////////////////////
		// Get size of render target
		cVector2l vFrameBufferSize = mpCurrentRenderTarget->mpFrameBuffer ? mpCurrentRenderTarget->mpFrameBuffer->GetSize() : mvScreenSize;
		mvRenderTargetSize.x = mpCurrentRenderTarget->mvSize.x <0 ? vFrameBufferSize.x : mpCurrentRenderTarget->mvSize.x;
		mvRenderTargetSize.y = mpCurrentRenderTarget->mvSize.y <0 ? vFrameBufferSize.y : mpCurrentRenderTarget->mvSize.y;

		mvCurrentFrameBufferSize = vFrameBufferSize;

		////////////////////////////////
		// Set up scissor rect
		mbUseGlobalScissorRect = abUseGlobalScissorRect;
		mvGlobalScissorRectPos = avGlobalScissorRectPos;
		mvGlobalScissorRectSize = avGlobalScissorRectSize;
		mbGlobalScissorRectActive = false;
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PROTECTED METHODS
	//////////////////////////////////////////////////////////////////////////
	
	//-----------------------------------------------------------------------

	void iRenderFunctions::SetVertexBuffer(iVertexBuffer *apVtxBuffer)
	{
		if(mpCurrentVtxBuffer == apVtxBuffer) return;

		if(mbLog) {
			if(apVtxBuffer)
				Log("  Setting vertex buffer: %d\n",apVtxBuffer);
			else
				Log("  Setting vertex buffer: NULL\n");
		}

		if(mpCurrentVtxBuffer) mpCurrentVtxBuffer->UnBind();
		if(apVtxBuffer) apVtxBuffer->Bind();

		mpCurrentVtxBuffer = apVtxBuffer;
	}

	iTexture* iRenderFunctions::CreateRenderTexture(const tString& asName, const cVector2l& avSize, ePixelFormat aPixelFormat, eTextureFilter aFilter, eTextureType aType)
	{
		iTexture *pTexture =NULL;
		pTexture = mpGraphics->CreateTexture(asName,aType,eTextureUsage_RenderTarget);
		if(pTexture->CreateFromRawData(cVector3l(avSize.x, avSize.y,0),aPixelFormat, NULL)==false)
		{
			Error("Could not create texture '%s'\n", asName.c_str());
			return pTexture;
		}
				
		pTexture->SetWrapSTR(eTextureWrap_ClampToEdge);
		pTexture->SetFilter(aFilter);

		return pTexture;
	}
}
