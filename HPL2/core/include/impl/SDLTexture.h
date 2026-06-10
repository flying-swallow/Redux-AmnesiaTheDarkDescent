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

#ifndef HPL_SDL_TEXTURE_H
#define HPL_SDL_TEXTURE_H

#include "graphics/Texture.h"
#include "impl/LowLevelGraphicsSDL.h"

namespace hpl {

	class cBitmapData;

	class cSDLTexture : public iTexture
	{
	public:
		cSDLTexture(const tString& asName, eTextureType aType, eTextureUsage aUsage, iLowLevelGraphics* apLowLevelGraphics);
		~cSDLTexture();

		bool CreateFromBitmap(cBitmap* pBmp) { return true; } // STUB
		bool CreateAnimFromBitmapVec(std::vector<cBitmap*>* avBitmaps) { return true; } // STUB
		bool CreateCubeFromBitmapVec(std::vector<cBitmap*> *avBitmaps) { return true; } // STUB
		bool CreateFromRawData(const cVector3l &avSize,ePixelFormat aPixelFormat, unsigned char *apData) { return true; } // STUB

		virtual void SetRawData(int alLevel, const cVector3l& avOffset, const cVector3l& avSize, ePixelFormat aPixelFormat, void* apData) {} // STUB
		
		void SetFilter(eTextureFilter aFilter) {} // STUB
		void SetAnisotropyDegree(float afX) {} // STUB

		void SetWrapS(eTextureWrap aMode) {} // STUB
		void SetWrapT(eTextureWrap aMode) {} // STUB
		void SetWrapR(eTextureWrap aMode) {} // STUB
		void SetWrapSTR(eTextureWrap aMode) {} // STUB

		void SetCompareMode(eTextureCompareMode aMode) {} // STUB
		void SetCompareFunc(eTextureCompareFunc aFunc) {} // STUB

		void AutoGenerateMipmaps() {} // STUB

		void Update(float afTimeStep);

		bool HasAnimation();
		void NextFrame();
		void PrevFrame();
		float GetT();
		float GetTimeCount();
		void SetTimeCount(float afX);
		int GetCurrentLowlevelHandle() { return NULL; } // STUB

		/// SDL / OGL Specific ///////////

		unsigned int GetTextureHandle() { return NULL; } // STUB
	private:
		void GenerateHandles(int alNumOfHandles) {} // STUB
		bool CreateFromBitmapToIndex(cBitmap* apBmp, int alIdx) { return true; } // STUB
		bool CreateTexture(int alTextureHandle, cBitmapData* apBitmapImage, int alNumOfMipMaps, const cVector3l avSize, ePixelFormat aPixelFormat, int alFaceNum,bool abGenerateMipMaps, bool abCheckForResize) { return true; } // STUB
		bool CopyTextureDataToGL(int alTextureHandle, int alLevel,unsigned char *apData,int alDataSize, const cVector3l avSize, ePixelFormat aPixelFormat,int alFaceNum) { return true; } // STUB
		void SetupProperties(int alTextureHandle) {} // STUB
		unsigned char* ResizePixelData(unsigned char* apData, int alBytesPerPixel) { return NULL; } // STUB
		
		tUIntVec mvTextureHandles;
		bool mbContainsData;
		cLowLevelGraphicsSDL* mpGfxSDL;

		float mfTimeCount;
		int mlTextureIndex;
		float mfTimeDir;
	};

};
#endif // HPL_SDL_TEXTURE_H
