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

#ifndef HPL_TEXTURE_MANAGER_H
#define HPL_TEXTURE_MANAGER_H

#include "resources/ResourceManager.h"
#include "graphics/Texture.h"

#include <functional>
#include <vector>

namespace hpl {

	class cGraphics;
	class cResources;
	class iTexture;
	class cBitmapLoaderHandler;
	class Image;

	//------------------------------------------------------
	
	typedef std::map<tString, iTexture*> tTextureAttenuationMap;
	typedef std::map<tString, iTexture*>::iterator tTextureAttenuationMapIt;
	
	//------------------------------------------------------

	class cTextureManager : public iResourceManager
	{
	public:
		cTextureManager(cGraphics* apGraphics,cResources *apResources);
		~cTextureManager();

		// abSRGB requests an sRGB-format view: the sampler decodes sRGB→linear
		// on read. Set on perceptual color sources (diffuse, illumination);
		// leave false for linear data (normals, packed specular, height, LUTs).
		Image* Create1DImage(const tString& asName,bool abUseMipMaps, eTextureUsage aUsage=eTextureUsage_Normal,
							unsigned int alTextureSizeLevel=0, bool abSRGB=false);

		Image* Create2DImage(const tString& asName,bool abUseMipMaps,eTextureType aType= eTextureType_2D,
							eTextureUsage aUsage=eTextureUsage_Normal,unsigned int alTextureSizeLevel=0,
							bool abSRGB=false);

		Image* Create3DImage(const tString& asName,bool abUseMipMaps, eTextureUsage aUsage=eTextureUsage_Normal,
							unsigned int alTextureSizeLevel=0, bool abSRGB=false);

		Image* CreateCubeMapImage(const tString& asName,bool abUseMipMaps, eTextureUsage aUsage=eTextureUsage_Normal,
					unsigned int alTextureSizeLevel=0, bool abSRGB=false);

		/**
		 * Create an animated Image from a frame-numbered sequence. The first frame name must
		 * end in "01.<ext>"; subsequent frames are discovered as "02.<ext>", "03.<ext>", ...
		 * The returned Image holds an Image::AnimatedImage variant; cTextureManager::Update
		 * advances the frame index over time.
		 */
		Image* CreateAnimImage(const tString& asName, bool abUseMipMaps, eTextureType aType,
								eTextureUsage aUsage=eTextureUsage_Normal,
								unsigned int alTextureSizeLevel=0, bool abSRGB=false);

		[[deprecated("create_1D")]]
		iTexture* Create1D(	const tString& asName,bool abUseMipMaps, eTextureUsage aUsage=eTextureUsage_Normal,
							unsigned int alTextureSizeLevel=0);
		
		[[deprecated("create_2D")]]
		iTexture* Create2D(	const tString& asName,bool abUseMipMaps,eTextureType aType= eTextureType_2D,
							eTextureUsage aUsage=eTextureUsage_Normal,unsigned int alTextureSizeLevel=0);

		[[deprecated("create_3D")]]
		iTexture* Create3D(	const tString& asName,bool abUseMipMaps, eTextureUsage aUsage=eTextureUsage_Normal,
							unsigned int alTextureSizeLevel=0);
		
		/**
		 * Creates an animated texture. The name must be [name]01.[ext]. And then the textures in the animation must
		 * be named [name]01.[ext], [name]02.[ext], etc 
		 */
		iTexture* CreateAnim(	const tString& asFirstFrameName,bool abUseMipMaps, eTextureType aType,
								eTextureUsage aUsage=eTextureUsage_Normal,
								unsigned int alTextureSizeLevel=0);

		[[deprecated("create_cubemap")]]
		iTexture* CreateCubeMap(const tString& asName,bool abUseMipMaps, eTextureUsage aUsage=eTextureUsage_Normal,
								unsigned int alTextureSizeLevel=0);


		void Destroy(iResourceBase* apResource);
		void Unload(iResourceBase* apResource);

		void Update(float afTimeStep);

		// Stub — memory accounting was dropped during the Image* migration; no consumer reads
		// the per-byte total today and the old iTexture cast in Destroy() was UB for Image*.
		int GetMemoryUsage(){ return 0; }

	private:
		iTexture* CreateSimpleTexture(const tString& asName,bool abUseMipMaps, 
									eTextureUsage aUsage, eTextureType aType, 
									unsigned int alTextureSizeLevel);

		iTexture* FindTexture2D(const tString &asName, tWString &asFilePath);
		
		Image* _wrapperImageResource(const tString& asName, std::function<Image*(const tString& asName, const tWString& path, cBitmap* bitmap)> createImageHandler);
		Image* FindImageResource(const tString &asName, tWString &asFilePath);

		tTextureAttenuationMap m_mapAttenuationTextures;

		tStringVec mvCubeSideSuffixes;

		// Image resources produced by Create*Image. Maintained alongside the base-class
		// m_mapResources so Update() can iterate only Image-typed entries (the map can
		// also hold iTexture* via the legacy CreateSimpleTexture path). Stored as
		// iResourceBase* to avoid any cross-hierarchy cast at insert/erase time;
		// Update() casts to Image* only when it's known the entry came from here.
		std::vector<iResourceBase*> m_imageResources;

		cGraphics* mpGraphics;
		cResources* mpResources;
		cBitmapLoaderHandler *mpBitmapLoaderHandler;
	};

};
#endif // HPL_TEXTURE_MANAGER_H
