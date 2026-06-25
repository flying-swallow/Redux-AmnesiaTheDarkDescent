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
#include "resources/ResourceBase.h"
#include "graphics/GraphicsTypes.h"
#include "graphics/IndexPool.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace hpl {

	class cGraphics;
	class cResources;
	class cBitmapLoaderHandler;
	class cBitmap;
	class Image;

	//------------------------------------------------------

	class cTextureManager : public iResourceManager
	{
	public:
		cTextureManager(cGraphics* apGraphics,cResources *apResources);
		~cTextureManager();

		// abSRGB requests an sRGB-format view: the sampler decodes sRGB→linear
		// on read. Set on perceptual color sources (diffuse, illumination);
		// leave false for linear data (normals, packed specular, height, LUTs).
		SharedResourceHandle<Image> Create1DImage(const tString& asName,bool abUseMipMaps, eTextureUsage aUsage=eTextureUsage_Normal,
							unsigned int alTextureSizeLevel=0, bool abSRGB=false);

		SharedResourceHandle<Image> Create2DImage(const tString& asName,bool abUseMipMaps,eTextureType aType= eTextureType_2D,
							eTextureUsage aUsage=eTextureUsage_Normal,unsigned int alTextureSizeLevel=0,
							bool abSRGB=false);

		SharedResourceHandle<Image> Create3DImage(const tString& asName,bool abUseMipMaps, eTextureUsage aUsage=eTextureUsage_Normal,
							unsigned int alTextureSizeLevel=0, bool abSRGB=false);

		SharedResourceHandle<Image> CreateCubeMapImage(const tString& asName,bool abUseMipMaps, eTextureUsage aUsage=eTextureUsage_Normal,
					unsigned int alTextureSizeLevel=0, bool abSRGB=false);

		/**
		 * Create an animated Image from a frame-numbered sequence. The first frame name must
		 * end in "01.<ext>"; subsequent frames are discovered as "02.<ext>", "03.<ext>", ...
		 * The returned Image holds an Image::AnimatedImage variant. aAnimMode / aFrameTime are
		 * baked into the per-slot gAnimTex record (uploaded once when the bindless slot is
		 * assigned); frame selection then happens entirely in-shader from gPerFrame.afT, so
		 * there is no per-frame GPU transfer regardless of how many animated gobos a map has.
		 */
		SharedResourceHandle<Image> CreateAnimImage(const tString& asName, bool abUseMipMaps, eTextureType aType,
								eTextureUsage aUsage=eTextureUsage_Normal,
								unsigned int alTextureSizeLevel=0, bool abSRGB=false,
								eTextureAnimMode aAnimMode=eTextureAnimMode_Loop, float aFrameTime=1.0f);

		void Unload(iResourceBase* apResource);

		// Drops the parallel Image-resource tracking entry before the base free.
		void FreeResource(iResourceBase* apResource) override;

		void Update(float afTimeStep);

		// Stub — memory accounting was dropped during the Image* migration; no consumer reads
		// the per-byte total today and the old iTexture cast in Destroy() was UB for Image*.
		int GetMemoryUsage(){ return 0; }

		// === Bindless texture heap (this manager owns it) ===
		// Return a freed slot to its pool. Called (deferred) from
		// ReleaseImageBindlessSlot when an Image is destroyed.
		void ReturnBindlessSlot(uint32_t slot, bool isCube, bool isArray);

		// Editor live-reload: set an animated Image's anim mode / frame time and
		// re-upload its per-slot gAnimTex record, but only if the values changed.
		// The runtime bakes these at the single load-time write and never calls this.
		void UpdateAnimParams(Image* apImage, eTextureAnimMode aMode, float aFrameTime);

	private:
		SharedResourceHandle<Image> _wrapperImageResource(const tString& asName, std::function<Image*(const tString& asName, const tWString& path, cBitmap* bitmap)> createImageHandler);
		Image* FindImageResource(const tString &asName, tWString &asFilePath);

		// Assign a lifetime-stable bindless slot to `apImage` (idempotent) and write
		// its descriptor into the global set's textures_2d[] / textures_cube[] array.
		void AssignBindlessSlot(Image* apImage, bool abCube);
		// (Re)write apImage's descriptor at its slot if the active view changed.
		void WriteImageDescriptor(Image* apImage);
		// (Re)upload an animated Image's per-slot gAnimTex record from its current params.
		void WriteAnimRecord(Image* apImage);

		tStringVec mvCubeSideSuffixes;

		// Image resources produced by Create*Image. Maintained alongside the
		// base-class m_mapResources so Update() can iterate only Image-typed
		// entries. Stored as iResourceBase* to avoid any cross-hierarchy cast
		// at insert/erase time; Update() casts to Image* only when it's known
		// the entry came from here.
		std::vector<iResourceBase*> m_imageResources;

		// Bindless slot heap: textures_2d[] (binding 0), textures_cube[] (binding 1),
		// and textures_2d_array[] (binding 2 — one slot per animated Image, frame =
		// layer). Slot 0 of each is reserved (never handed out) as a safe default
		// for any errant/invalid index. A slot lives for the Image's whole lifetime.
		IndexPool m_texture2DPool;
		IndexPool m_textureCubePool;
		IndexPool m_texture2DArrayPool;

		cGraphics* mpGraphics;
		cResources* mpResources;
		cBitmapLoaderHandler *mpBitmapLoaderHandler;
	};

	// Free the bindless slot an Image holds (if any), deferred past in-flight
	// frames. Safe to call on a manager-less / standalone Image and during engine
	// shutdown (a missing manager just leaks the index harmlessly).
	void ReleaseImageBindlessSlot(Image* apImage);

};
#endif // HPL_TEXTURE_MANAGER_H
