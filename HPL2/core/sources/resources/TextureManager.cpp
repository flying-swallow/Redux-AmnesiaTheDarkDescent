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

#include "resources/TextureManager.h"

#include <algorithm>

#include "system/String.h"
#include "graphics/Graphics.h"
#include "resources/Resources.h"
#include "graphics/LowLevelGraphics.h"
#include "resources/LowLevelResources.h"
#include "system/LowLevelSystem.h"
#include "resources/FileSearcher.h"
#include "graphics/Bitmap.h"
#include "resources/BitmapLoaderHandler.h"

#include "graphics/Image.h"
#include "graphics/Texture.h"


namespace hpl {

	cTextureManager::cTextureManager(cGraphics* apGraphics,cResources *apResources)
		: iResourceManager(apResources->GetFileSearcher(), apResources->GetLowLevel(),
							apResources->GetLowLevelSystem())
	{
		mpGraphics = apGraphics;
		mpResources = apResources;
		
		mpBitmapLoaderHandler = mpResources->GetBitmapLoaderHandler();

		mvCubeSideSuffixes.push_back("_pos_x");
		mvCubeSideSuffixes.push_back("_neg_x");
		mvCubeSideSuffixes.push_back("_pos_y");
		mvCubeSideSuffixes.push_back("_neg_y");
		mvCubeSideSuffixes.push_back("_pos_z");
		mvCubeSideSuffixes.push_back("_neg_z");
	}

	cTextureManager::~cTextureManager()
	{
		DestroyAll();
		Log(" Destroyed all textures\n");
	}

	void cTextureManager::FreeResource(iResourceBase* apResource)
	{
		// Drop the parallel Image-resource tracking entry, if any.
		auto it = std::find(m_imageResources.begin(), m_imageResources.end(), apResource);
		if(it != m_imageResources.end()) m_imageResources.erase(it);

		RemoveResource(apResource);
		hplDelete(apResource);
	}

	SharedResourceHandle<Image> cTextureManager::_wrapperImageResource(const tString& name, std::function<Image*(const tString& name, const tWString& path, cBitmap* bitmap)> create_image_handler) {
		tWString sPath;
		BeginLoad(name);

		Image* resource = FindImageResource(name, sPath);
		if( resource==NULL && sPath!=_W(""))
		{
			// pTexture = FindTexture2D(asName,sPath);
			cBitmap *pBmp = mpBitmapLoaderHandler->LoadBitmap(sPath,0);
			if(!pBmp) {

				Error("Texture manager Couldn't load bitmap '%s'\n", cString::To8Char(sPath).c_str());
				EndLoad();
				return {};
			}
			resource = create_image_handler(name, sPath, pBmp);

			//Bitmap is no longer needed so delete it.
			hplDelete(pBmp);

			AddResource(resource);
			if(resource) m_imageResources.push_back(resource);
		}

		if(resource) {
			resource->AddReference();
		}
		EndLoad();
		return SharedResourceHandle<Image>(this, resource); // adopt the reference taken above
	}


	Image* cTextureManager::FindImageResource(const tString &asName, tWString &asFilePath) {
		Image *pTexture=NULL;

		if(cString::GetFileExt(asName)=="")
		{
			int lMaxCount =-1;

			///////////////////////
			//Iterate the different formats
			tStringVec *apFileFormatsVec = mpBitmapLoaderHandler->GetSupportedTypes();
			for(tStringVecIt it = apFileFormatsVec->begin();it!= apFileFormatsVec->end();++it)
			{
				tWString sTempPath = _W("");
				int lCount=0;

				tString sNewName = cString::SetFileExt(asName,*it);
				auto resource = static_cast<Image*> (FindLoadedResource(sNewName, sTempPath, &lCount));

				///////////////////////
				//Check if the image exists and then check if it has the hightest equal count.
				if((resource==nullptr && sTempPath!=_W("")) || resource != nullptr)
				{
					if(lCount > lMaxCount)
					{
						lMaxCount = lCount;
						asFilePath = sTempPath;
						return resource;
					}
				}
			}
		}
		return static_cast<Image*> (FindLoadedResource(asName, asFilePath));
	}

	SharedResourceHandle<Image> cTextureManager::Create1DImage(const tString& asName,bool abUseMipMaps, eTextureUsage aUsage,
						unsigned int alTextureSizeLevel, bool abSRGB) {
		return _wrapperImageResource(asName, [&abUseMipMaps, &abSRGB, this](const tString& asName, const tWString& path, cBitmap* pBmp) -> Image* {
				Image::SingleImage singleImage = {};
				hpl::cTexture::BitmapLoadOptions opts = {0};
				opts.use_mipmaps = abUseMipMaps;
				opts.sRGB = abSRGB;
				singleImage.image = std::shared_ptr<cTexture>(new cTexture{}, cTexture::cTexture_Delete);
				if(!singleImage.image->LoadBitmap(RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT, *pBmp, opts)) {
					Error("Texture manager Couldn't load bitmap '%s'\n", cString::To8Char(path).c_str());
					return NULL;
				}
				auto resource = new Image(asName, path, std::move(singleImage));
				return resource;
		});
	}

	SharedResourceHandle<Image> cTextureManager::Create2DImage(const tString& asName,bool abUseMipMaps,eTextureType aType,
						eTextureUsage aUsage,unsigned int alTextureSizeLevel, bool abSRGB) {
		return _wrapperImageResource(asName, [&abUseMipMaps, &abSRGB, this](const tString& asName, const tWString& path, cBitmap* pBmp) -> Image* {
				Image::SingleImage singleImage = {};
				cTexture::BitmapLoadOptions opts = {0};
				opts.use_mipmaps = abUseMipMaps;
				opts.sRGB = abSRGB;
				singleImage.image = std::shared_ptr<cTexture>(new cTexture{}, cTexture::cTexture_Delete);
				if(!singleImage.image->LoadBitmap(RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT, *pBmp, opts)) {
					Error("Texture manager Couldn't load bitmap '%s'\n", cString::To8Char(path).c_str());
					return NULL;
				}
				return new Image(asName, path, std::move(singleImage));//, &cTexture::cTexture_Delete);
		});
	}

	SharedResourceHandle<Image> cTextureManager::Create3DImage(const tString& asName,bool abUseMipMaps, eTextureUsage aUsage,
						unsigned int alTextureSizeLevel, bool abSRGB){
		return _wrapperImageResource(asName, [&abUseMipMaps, &abSRGB, this](const tString& asName, const tWString& path, cBitmap* pBmp) -> Image* {
				Image::SingleImage singleImage = {};
				cTexture::BitmapLoadOptions opts = {0};
				opts.use_mipmaps = abUseMipMaps;
				opts.sRGB = abSRGB;
				singleImage.image =  std::shared_ptr<cTexture>(new cTexture{}, cTexture::cTexture_Delete);
				if(!singleImage.image->LoadBitmap(RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT, *pBmp, opts)) {
					Error("Texture manager Couldn't load bitmap '%s'\n", cString::To8Char(path).c_str());
					return NULL;
				}
				return new Image(asName, path, std::move(singleImage));//, &cTexture::cTexture_Delete);
		});
	}

	SharedResourceHandle<Image> cTextureManager::CreateCubeMapImage(const tString& asPathName, bool abUseMipMaps, eTextureUsage aUsage,
				unsigned int alTextureSizeLevel, bool abSRGB){

		tString sExt = cString::ToLowerCase(cString::GetFileExt(asPathName));

		// Single-file (DDS) cubemap: bitmap already contains 6 faces.
		if(sExt == "dds")
		{
			return _wrapperImageResource(asPathName,
				[&abUseMipMaps, &abSRGB](const tString& asName, const tWString& path, cBitmap* pBmp) -> Image* {
					Image::SingleImage singleImage = {};
					cTexture::BitmapLoadOptions opts = {0};
					opts.use_mipmaps = abUseMipMaps;
					opts.use_cubemap = true;
					opts.sRGB = abSRGB;
					singleImage.image = std::shared_ptr<cTexture>(new cTexture{}, cTexture::cTexture_Delete);
					if(!singleImage.image->LoadBitmap(RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT, *pBmp, opts)) {
						Error("Texture manager Couldn't load cubemap '%s'\n", cString::To8Char(path).c_str());
						return nullptr;
					}
					return new Image(asName, path, std::move(singleImage));
				});
		}

		// Multi-file cubemap: discover 6 face files and aggregate them into a single cBitmap.
		tString sName = cString::SetFileExt(asPathName, "");
		tWString sFakeFullPath = cString::To16Char(sName);
		Image* image = static_cast<Image*>(GetResource(sFakeFullPath));

		BeginLoad(asPathName);

		if(image == nullptr)
		{
			tWStringVec vPaths;
			tWString sPath = _W("");
			for(int i = 0; i < 6; ++i)
			{
				tStringVec* apFileFormatsVec = mpBitmapLoaderHandler->GetSupportedTypes();
				for(tStringVecIt it = apFileFormatsVec->begin(); it != apFileFormatsVec->end(); ++it)
				{
					tString sNewName = sName + mvCubeSideSuffixes[i] + "." + *it;
					sPath = mpFileSearcher->GetFilePath(sNewName);
					if(sPath != _W("")) break;
				}

				if(sPath == _W(""))
				{
					tString sNewName = sName + mvCubeSideSuffixes[i];
					Error("Couldn't find %d-face '%s', for cubemap '%s' in path: '%s'\n",
						i, sNewName.c_str(), sName.c_str(), asPathName.c_str());
					EndLoad();
					return {};
				}
				vPaths.push_back(sPath);
			}

			std::vector<cBitmap*> vBitmaps;
			for(int i = 0; i < 6; ++i)
			{
				cBitmap* pBmp = mpBitmapLoaderHandler->LoadBitmap(vPaths[i], 0);
				if(pBmp == nullptr)
				{
					Error("Couldn't load bitmap '%s'!\n", cString::To8Char(vPaths[i]).c_str());
					for(cBitmap* pPrev : vBitmaps) hplDelete(pPrev);
					EndLoad();
					return {};
				}
				vBitmaps.push_back(pBmp);
			}

			// Aggregate the 6 face bitmaps into a single cBitmap with 6 images so that
			// cTexture::LoadBitmap (use_cubemap = true) sees the expected layout.
			cBitmap aggregate;
			aggregate.SetSize(vBitmaps[0]->GetSize());
			aggregate.SetPixelFormat(vBitmaps[0]->GetPixelFormat());
			aggregate.SetBytesPerPixel(vBitmaps[0]->GetBytesPerPixel());
			aggregate.SetIsCompressed(vBitmaps[0]->IsCompressed());
			aggregate.SetFileName(cString::To16Char(sName));
			const int lMips = vBitmaps[0]->GetNumOfMipMaps();
			aggregate.SetUpData(6, lMips);
			for(int face = 0; face < 6; ++face)
			{
				for(int mip = 0; mip < lMips; ++mip)
				{
					cBitmapData* src = vBitmaps[face]->GetData(0, mip);
					cBitmapData* dst = aggregate.GetData(face, mip);
					if(src && dst && src->mpData) dst->SetData(src->mpData, src->mlSize);
				}
			}

			Image::SingleImage singleImage = {};
			cTexture::BitmapLoadOptions opts = {0};
			opts.use_mipmaps = abUseMipMaps;
			opts.use_cubemap = true;
			opts.sRGB = abSRGB;
			singleImage.image = std::shared_ptr<cTexture>(new cTexture{}, cTexture::cTexture_Delete);

			bool ok = singleImage.image->LoadBitmap(RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT, aggregate, opts);

			for(cBitmap* pBmp : vBitmaps) hplDelete(pBmp);

			if(!ok)
			{
				Error("Couldn't create cubemap '%s'!\n", sName.c_str());
				EndLoad();
				return {};
			}

			image = new Image(sName, sFakeFullPath, std::move(singleImage));
			AddResource(image);
			m_imageResources.push_back(image);
		}

		if(image) image->AddReference();
		EndLoad();
		return SharedResourceHandle<Image>(this, image); // adopt the reference taken above
	}

	SharedResourceHandle<Image> cTextureManager::CreateAnimImage(const tString& asName, bool abUseMipMaps, eTextureType aType,
											eTextureUsage aUsage, unsigned int alTextureSizeLevel, bool abSRGB)
	{
		BeginLoad(asName);

		// First frame must contain "01" before the extension.
		int lPos = cString::GetFirstStringPos(asName, "01");
		if(lPos < 0)
		{
			Error("First frame of animation '%s' must contain '01'!\n", asName.c_str());
			EndLoad();
			return {};
		}

		tString sSub1 = cString::Sub(asName, 0, lPos);
		tString sSub2 = cString::Sub(asName, lPos + 2);
		tString sBaseName = sSub1 + sSub2;

		if(sSub2.size() == 0 || sSub2[0] != '.')
		{
			Error("First frame of animation '%s' must contain '01' before extension!\n", asName.c_str());
			EndLoad();
			return {};
		}

		tWString sFirstFramePath = mpFileSearcher->GetFilePath(asName);
		if(sFirstFramePath == _W(""))
		{
			Error("First frame of animation '%s' could not be found!\n", asName.c_str());
			EndLoad();
			return {};
		}
		tWString sFakeFullPath = cString::GetFilePathW(sFirstFramePath) + cString::To16Char(cString::GetFileName(sBaseName));

		Image* image = static_cast<Image*>(GetResource(sFakeFullPath));

		if(image == nullptr)
		{
			tString sFileExt = cString::GetFileExt(sBaseName);
			tString sFileName = cString::SetFileExt(cString::GetFileName(sBaseName), "");

			tString sTest = sFileName + "01." + sFileExt;
			int lNum = 2;
			tWStringVec vPaths;

			while(true)
			{
				tWString sPath = mpFileSearcher->GetFilePath(sTest);
				if(sPath == _W("")) break;
				vPaths.push_back(sPath);
				if(lNum < 10)
					sTest = sFileName + "0" + cString::ToString(lNum) + "." + sFileExt;
				else
					sTest = sFileName + cString::ToString(lNum) + "." + sFileExt;
				++lNum;
			}

			if(vPaths.empty())
			{
				Error("No textures found for animation %s\n", sBaseName.c_str());
				EndLoad();
				return {};
			}

			std::vector<cBitmap*> vBitmaps;
			for(size_t i = 0; i < vPaths.size(); ++i)
			{
				cBitmap* pBmp = mpBitmapLoaderHandler->LoadBitmap(vPaths[i], 0);
				if(pBmp == nullptr)
				{
					Error("Couldn't load bitmap '%s'!\n", cString::To8Char(vPaths[i]).c_str());
					for(cBitmap* pPrev : vBitmaps) hplDelete(pPrev);
					EndLoad();
					return {};
				}
				vBitmaps.push_back(pBmp);
			}

			Image::AnimatedImage anim = {};
			anim.frameTime = 1.0f;
			anim.timeCount = 0.0f;
			anim.timeDir = 1.0f;
			anim.animMode = eTextureAnimMode_Loop;
			anim.images.reserve(vBitmaps.size());

			bool ok = true;
			for(cBitmap* pBmp : vBitmaps)
			{
				cTexture::BitmapLoadOptions opts = {0};
				opts.use_mipmaps = abUseMipMaps;
				opts.use_cubemap = (aType == eTextureType_CubeMap);
				opts.sRGB = abSRGB;
				auto tex = std::shared_ptr<cTexture>(new cTexture{}, cTexture::cTexture_Delete);
				if(!tex->LoadBitmap(RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT, *pBmp, opts))
				{
					Error("Couldn't load animation frame for '%s'!\n", sBaseName.c_str());
					ok = false;
					break;
				}
				anim.images.push_back(std::move(tex));
			}

			for(cBitmap* pBmp : vBitmaps) hplDelete(pBmp);

			if(!ok || anim.images.empty())
			{
				EndLoad();
				return {};
			}

			image = new Image(sBaseName, sFakeFullPath, std::move(anim));
			AddResource(image);
			m_imageResources.push_back(image);
		}

		if(image) image->AddReference();
		EndLoad();
		return SharedResourceHandle<Image>(this, image); // adopt the reference taken above
	}

	//-----------------------------------------------------------------------

	void cTextureManager::Unload(iResourceBase* apResource)
	{

	}
	//-----------------------------------------------------------------------


	//-----------------------------------------------------------------------

	void cTextureManager::Update(float afTimeStep)
	{
		// Only Create*Image-produced resources (tracked in m_imageResources)
		// advance animation here.
		for(iResourceBase* pBase : m_imageResources)
		{
			static_cast<Image*>(pBase)->Update(afTimeStep);
		}
	}

	//-----------------------------------------------------------------------

	
	//////////////////////////////////////////////////////////////////////////
	// PRIVATE METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------



	//-----------------------------------------------------------------------
}
