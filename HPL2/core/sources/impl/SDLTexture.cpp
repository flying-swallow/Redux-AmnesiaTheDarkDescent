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

#include "impl/SDLTexture.h"

#include "system/LowLevelSystem.h"
#include "math/Math.h"
#include "graphics/Bitmap.h"

#include "impl/LowLevelGraphicsSDL.h"

#include <cmath>
#include <cstring>

namespace hpl {

	//////////////////////////////////////////////////////////////////////////
	// CONSTRUCTORS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	cSDLTexture::cSDLTexture(const tString& asName, eTextureType aType, eTextureUsage aUsage, iLowLevelGraphics* apLowLevelGraphics) : iTexture(asName,_W(""),aType, aUsage, apLowLevelGraphics)
	{
		mbContainsData = false;
				
		mpGfxSDL = static_cast<cLowLevelGraphicsSDL*>(mpLowLevelGraphics);

		mlTextureIndex = 0;
		mfTimeCount =0;

		mfTimeDir = 1;
	}

	cSDLTexture::~cSDLTexture() {}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PUBLIC METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	void cSDLTexture::Update(float afTimeStep)
	{
		if(mvTextureHandles.size() > 1)
		{
			float fMax = (float)(mvTextureHandles.size());
			mfTimeCount += afTimeStep * (1.0f/mfFrameTime) * mfTimeDir;

			if(mfTimeDir > 0)
			{
				if(mfTimeCount >= fMax)
				{
					if(mAnimMode == eTextureAnimMode_Loop)
					{
						mfTimeCount =0;
					}
					else
					{
						mfTimeCount = fMax - 1.0f;
						mfTimeDir = -1.0f;
					}
				}
			}
			else
			{
				if(mfTimeCount < 0)
				{
					mfTimeCount =1;
					mfTimeDir = 1.0f;
				}
			}
		}
	}

	//-----------------------------------------------------------------------

	bool cSDLTexture::HasAnimation()
	{
		return mvTextureHandles.size() > 1;
	}
	
	void cSDLTexture::NextFrame()
	{
		mfTimeCount += mfTimeDir;

		if(mfTimeDir > 0)
		{
			float fMax = (float)(mvTextureHandles.size());
			if(mfTimeCount >= fMax)
			{
				if(mAnimMode == eTextureAnimMode_Loop)
				{
					mfTimeCount =0;
				}
				else
				{
					mfTimeCount = fMax - 1.0f;
					mfTimeDir = -1.0f;
				}
			}
		}
		else
		{
			if(mfTimeCount < 0)
			{
				mfTimeCount =1;
				mfTimeDir = 1.0f;
			}
		}
	}

	void cSDLTexture::PrevFrame()
	{
		mfTimeCount -= mfTimeDir;

		if(mfTimeDir < 0)
		{
			float fMax = (float)(mvTextureHandles.size());
			if(mfTimeCount >= fMax)
			{
				if(mAnimMode == eTextureAnimMode_Loop)
				{
					mfTimeCount =0;
				}
				else
				{
					mfTimeCount = fMax - 1.0f;
					mfTimeDir = -1.0f;
				}
			}
		}
		else
		{
			if(mfTimeCount < 0)
			{
				mfTimeCount =1;
				mfTimeDir = 1.0f;
			}
		}
	}
	
	float cSDLTexture::GetT()
	{
		return cMath::Modulus(mfTimeCount,1.0f);
	}

	float cSDLTexture::GetTimeCount()
	{
		return mfTimeCount;
	}
	void cSDLTexture::SetTimeCount(float afX)
	{
		mfTimeCount = afX;
	}
}
