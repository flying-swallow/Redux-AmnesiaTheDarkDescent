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

#include "graphics/Graphics.h"

#include "engine/EngineTypes.h"
#include "engine/Updateable.h"

#include "graphics/RIFormat.h"
#include "system/LowLevelSystem.h"
#include "system/String.h"
#include "system/Platform.h"

#include "graphics/LowLevelGraphics.h"
#include "graphics/MeshCreator.h"
#include "graphics/TextureCreator.h"
#include "graphics/DecalCreator.h"
#include "graphics/PostEffectComposite.h"
#include "graphics/PostEffect.h"
#include "graphics/MaterialType.h"
#include "graphics/RIRenderer.h"
#include "graphics/RIResourceUploader.h"
#include "graphics/RITypes.h"
#include "graphics/RIVK.h"
#include "graphics/HybridRenderer.h"

#include "resources/LowLevelResources.h"
#include "resources/Resources.h"
#include "resources/FileSearcher.h"

#include "graphics/MaterialType_BasicSolid.h"
#include "graphics/MaterialType_BasicTranslucent.h"
#include "graphics/MaterialType_Water.h"
#include "graphics/MaterialType_Decal.h"

#include "graphics/PostEffect_Bloom.h"
#include "graphics/PostEffect_ToneMap.h"
#include "graphics/PostEffect_ColorConvTex.h"
#include "graphics/PostEffect_ImageTrail.h"
#include "graphics/PostEffect_RadialBlur.h"

#include "graphics/DebugDraw.h"
#include "graphics/RendererWireFrame.h"
#include "graphics/RendererSimple.h"
#include "graphics/RIScratchAlloc.h"
#include <cassert>
#include <vulkan/vulkan_core.h>

#include "graphics/RIBootstrap.h"

namespace hpl {

	//////////////////////////////////////////////////////////////////////////
	// CONSTRUCTORS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	cGraphics::cGraphics(iLowLevelGraphics *apLowLevelGraphics, iLowLevelResources *apLowLevelResources) : iUpdateable("HPL_Graphics")
	{
		mpLowLevelGraphics = apLowLevelGraphics;
		mpLowLevelResources = apLowLevelResources;

		mpMeshCreator = NULL;
		mpTextureCreator = NULL;
		mpDecalCreator = NULL;
		mpDebugDraw = NULL;
	}

	cGraphics::~cGraphics()
	{
		Log("Exiting Graphics Module\n");
		Log("--------------------------------------------------------\n");

		tMaterialTypeMapIt it = m_mapMaterialTypes.begin();
		for(;it != m_mapMaterialTypes.end(); ++it)
		{
			iMaterialType *pType = it->second;
			pType->DestroyData();
		}
		STLMapDeleteAll(m_mapMaterialTypes);

		STLDeleteAll(mvPostEffectTypes);

		for(size_t i=0; i<mvRenderers.size(); ++i)
		{
			if(mvRenderers[i])
			{
				mvRenderers[i]->DestroyData();
				hplDelete(mvRenderers[i]);
			}
		}
		mvRenderers.clear();

		STLDeleteAll(mlstPostEffectComposites);
		STLDeleteAll(mlstPostEffects);

		hplDelete(mpMeshCreator);
		hplDelete(mpTextureCreator);
		hplDelete(mpDecalCreator);
		if(mpDebugDraw) hplDelete(mpDebugDraw);

		// Tear down the RI backend now, while the resource managers (owned by
		// cResources, deleted after cGraphics in cEngine::~cEngine) are still
		// alive. RI.graphicsDefer holds SharedResourcePins that call back into
		// their owning manager's FreeResource() on release; draining here (rather
		// than letting the static RI global drain at atexit, after the managers
		// are gone) keeps those manager pointers valid. RI.Dispose() does not
		// destroy the vk device, so the device-deferrals ~cResources pushes
		// afterwards (vertex buffers, texture disposes) still have a live device.
		RI.Dispose();

		Log("--------------------------------------------------------\n\n");
	}

	bool cGraphics::Init(int alWidth, int alHeight, int alDisplay, int alBpp, int abFullscreen, const tString &asWindowCaption, const cVector2l &avWindowPos, cResources* apResources, tFlag alHplSetupFlags)
	{
		Log("Initializing Graphics Module\n");
		Log("--------------------------------------------------------\n");
		
		mpResources = apResources;

		////////////////////////////////////////////////
		//Setup the graphic directories:
		apResources->AddResourceDir(_W("core/shaders"),false);
		apResources->AddResourceDir(_W("core/textures"),false);
		apResources->AddResourceDir(_W("core/models"),false);
		apResources->AddResourceDir(_W("compiled_shaders"),false);


		////////////////////////////////////////////////
		// LowLevel Init
		if(alHplSetupFlags & eHplSetup_Screen)
		{
			Log("Init lowlevel graphics: %dx%d disp:%d bpp:%d fs:%d cap:'%s' pos:(%dx%d)\n",alWidth,alHeight,alDisplay,alBpp,abFullscreen, asWindowCaption.c_str(), avWindowPos.x,avWindowPos.y);
			mpLowLevelGraphics->Init(alWidth, alHeight, alDisplay, alBpp, abFullscreen, asWindowCaption, avWindowPos);
			mbScreenIsSetup = true;
		}
		else
		{
			mbScreenIsSetup = false;
		}
		{
		struct RIBackendInit backendInit = {};
		backendInit.api = RI_DEVICE_API_VK;
		backendInit.applicationName = "HPL2";
		backendInit.vk.enableValidationLayer = true;

		if(InitRIRenderer(&backendInit) != RI_SUCCESS) {
			return false;
		}

		uint32_t numAdapters = 0;
		if( EnumerateRIAdapters( NULL, &numAdapters ) != RI_SUCCESS ) {
			return false;
		}
		assert(numAdapters > 0);
		std::vector<RIPhysicalAdapter> physicalAdapters(numAdapters);

		if(EnumerateRIAdapters(physicalAdapters.data(), &numAdapters) != RI_SUCCESS) {
			return false;
		}
		uint32_t selectedAdapterIdx = 0;
		for( size_t i = 1; i < numAdapters; i++ ) {
			if( physicalAdapters[i].type > physicalAdapters[selectedAdapterIdx].type )
				selectedAdapterIdx = i;
			if( physicalAdapters[i].type < physicalAdapters[selectedAdapterIdx].type )
				continue;

			if( physicalAdapters[i].presetLevel > physicalAdapters[selectedAdapterIdx].presetLevel ) 
				selectedAdapterIdx = i;
			if( physicalAdapters[i].presetLevel < physicalAdapters[selectedAdapterIdx].presetLevel )
				continue;
			
			if(physicalAdapters[i].videoMemorySize > physicalAdapters[selectedAdapterIdx].videoMemorySize) 
				selectedAdapterIdx = i;
		}
		struct RIDeviceDesc deviceInit = { 0 };
		deviceInit.physicalAdapter = &physicalAdapters[selectedAdapterIdx];
		RI.device.init(&deviceInit );
		RI_InitResourceUploader(&RI.device, &RI.uploader);
		struct RIWindowHandle windowHandle = mpLowLevelGraphics->GetWindowHandle(); 
		if(windowHandle.type == RI_WINDOW_UNKNOWN) {
			FatalError("Failed to find valid window handle!\n");
			return false;
		}
		struct RISwapchainDesc swapchainInit = { 0 };
		swapchainInit.windowHandle = &windowHandle;
		swapchainInit.requestImageCount = RI_NUMBER_FRAMES_FLIGHT;
		swapchainInit.queue = &RI.device.queues[RI_QUEUE_GRAPHICS];
		swapchainInit.width = alWidth;
		swapchainInit.height = alHeight;
		swapchainInit.format = RI_SWAPCHAIN_BT709_G22_8BIT;
		InitRISwapchain(&RI.device, &swapchainInit, &RI.swapchain);

		// Per-viewport render targets (backbuffer, overscan render target,
		// depth, visibility) are created lazily by each renderer's Draw on its
		// cViewport (see scene/Viewport.h) — only the swapchain views
		// remain global.
		{
			assert( RI.swapchain.imageCount > 0 );
			for( uint32_t i = 0; i < RI.swapchain.imageCount; i++ ) {
				VkImageViewUsageCreateInfo usageInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO };
				VkImageViewCreateInfo createInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
				createInfo.pNext = &usageInfo;
				createInfo.subresourceRange = VkImageSubresourceRange{
					VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1,
				};
				// Mirrors the swapchain's image-level usage. The SurfelGI
				// composite + post-effect chain write into the viewport
				// backbuffer, so the swapchain view only needs COLOR_ATTACHMENT.
				usageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
				createInfo.image = RI.swapchain.vk.images[i];
				createInfo.format = RIFormatToVK( RI.swapchain.format );
				createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
				VK_WrapResult( vkCreateImageView( RI.device.vk.device, &createInfo, NULL, &RI.swapchainView[i].vk.image) );
			}
		}

		struct RIQueue *graphicsQueue = &RI.device.queues[RI_QUEUE_GRAPHICS];
		RI.graphicsCmdRing.init( &RI.device, graphicsQueue,
		                         RI_NUMBER_FRAMES_FLIGHT, RI_NUMBER_SUB_COMMANDS, true );
		RI.graphicsTimeline.init( &RI.device );
		for(auto& set: RI.frameSets) {
			struct RIScratchAllocDesc uboDesc = {
					.blockSize = 256 * 128,
					.alignmentReq = RI.device.physicalAdapter.constantBufferOffsetAlignment,
					.alloc = RIUniformScratchAllocHandler };
			InitRIScratchAlloc( &RI.device, &set.uboScratchAlloc, &uboDesc );

			// AS build scratch pool. 1 MiB blocks fit typical TLAS/BLAS scratch
			// for moderate scenes; oversized builds spill through the allocator's
			// one-shot path.
			struct RIScratchAllocDesc accelDesc = {
					.blockSize = 1024 * 1024,
					.alignmentReq = RI.device.physicalAdapter.accelerationStructureScratchOffsetAlignment,
					.alloc = RIAccelScratchAllocHandler };
			InitRIScratchAlloc( &RI.device, &set.accelScratchAlloc, &accelDesc );
		}
		}
		{
			struct RICommandRingElement initElem = RI.graphicsCmdRing.acquire( &RI.device, 1 );
			initElem.pool->reset( &RI.device );
			initElem.cmds[0].begin( &RI.device );

			RIBuffer whiteUploadStaging = {};

			// 1x1 white texture — staged upload, then transitioned to SHADER_READ_ONLY_OPTIMAL.
			{
				RITextureDesc whiteDesc = {};
				whiteDesc.type = RI_TEXTURE_2D;
				whiteDesc.format = RI_FORMAT_RGBA8_UNORM;
				whiteDesc.width = 1;
				whiteDesc.height = 1;
				whiteDesc.usage = RI_USAGE_SHADER_RESOURCE | RI_USAGE_TRANSFER_DST;
				RI.whiteTexture2D = RITexture::create(&RI.device, whiteDesc);
				if (RI.whiteTexture2D.isEmpty()) {
					FatalError("Failed to create white texture image!\n");
					return false;
				}

				const uint8_t whitePixel[4] = { 255, 255, 255, 255 };
				whiteUploadStaging = RIBuffer::create(
					&RI.device, {(uint64_t)sizeof(whitePixel),
					             RI_BUFFER_USAGE_TRANSFER_SRC, RI_MEMORY_HOST_UPLOAD, 0});
				if (whiteUploadStaging.isEmpty()) {
					FatalError("Failed to create white texture staging buffer!\n");
					return false;
				}
				memcpy(whiteUploadStaging.mappedAddress, whitePixel, sizeof(whitePixel));
				vmaFlushAllocation(RI.device.vk.vmaAllocator, whiteUploadStaging.vk.allocation, 0, VK_WHOLE_SIZE);

				RITextureBarrier toTransfer = {};
				toTransfer.texture = &RI.whiteTexture2D;
				toTransfer.before = RI_RESOURCE_STATE_UNDEFINED;
				toTransfer.after = RI_RESOURCE_STATE_COPY_DST;
				toTransfer.afterStages = RI_STAGE_COPY;
				toTransfer.mipCount = 1;
				toTransfer.layerCount = 1;
				initElem.cmds[0].vk_d3d12_textureBarrier(toTransfer);

				VkBufferImageCopy copyRegion = {};
				copyRegion.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
				copyRegion.imageExtent = { 1, 1, 1 };
				vkCmdCopyBufferToImage(initElem.cmds[0].vk.cmd, whiteUploadStaging.vk.buffer, RI.whiteTexture2D.vk.image,
				                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

				// afterStages 0 derives the all-shader mask — sampled reads can
				// only happen in shader stages.
				RITextureBarrier toShaderRead = {};
				toShaderRead.texture = &RI.whiteTexture2D;
				toShaderRead.before = RI_RESOURCE_STATE_COPY_DST;
				toShaderRead.beforeStages = RI_STAGE_COPY;
				toShaderRead.after = RI_RESOURCE_STATE_SHADER_RESOURCE;
				toShaderRead.mipCount = 1;
				toShaderRead.layerCount = 1;
				initElem.cmds[0].vk_d3d12_textureBarrier(toShaderRead);

				RITextureViewDesc whiteViewDesc = {};
				whiteViewDesc.viewType = RI_VIEWTYPE_SHADER_RESOURCE_2D;
				whiteViewDesc.format = RI_FORMAT_RGBA8_UNORM;
				whiteViewDesc.mipNum = 1;
				whiteViewDesc.layerNum = 1;
				RI.whiteTexture2DView =
					RITextureView::create(&RI.device, &RI.whiteTexture2D, whiteViewDesc);
				if (RI.whiteTexture2DView.isEmpty()) {
					FatalError("Failed to create white texture image view!\n");
					return false;
				}
			}

			// Zero-filled vertex buffer — small mapped buffer, never modified after init.
			{
				constexpr VkDeviceSize kNulVertexSize = 64;
				RI.nulVertexBuffer = RIBuffer::create(
					&RI.device, {(uint64_t)kNulVertexSize,
					             RI_BUFFER_USAGE_VERTEX_BUFFER | RI_BUFFER_USAGE_TRANSFER_DST,
					             RI_MEMORY_HOST_UPLOAD, 0});
				if (RI.nulVertexBuffer.isEmpty()) {
					FatalError("Failed to create null vertex buffer!\n");
					return false;
				}
				if (RI.nulVertexBuffer.mappedAddress) {
					memset(RI.nulVertexBuffer.mappedAddress, 0, kNulVertexSize);
					vmaFlushAllocation(RI.device.vk.vmaAllocator, RI.nulVertexBuffer.vk.allocation, 0, VK_WHOLE_SIZE);
				}
			}

			// Default-value fallback vertex streams (see RIBootstrap). Each is a
			// single vertex, host-mapped and filled once here; bound for
			// renderables that omit an optional stream, where the pipeline zeroes
			// that binding's stride so this one element feeds every vertex.
			{
				struct FallbackSpec {
					struct RIBuffer *target;
					uint32_t size;       // single-vertex byte size (the binding stride)
					float    value[4];
				};
				const FallbackSpec specs[] = {
					{ &RI.fallbackNormalVertex,  sizeof(float) * 3, { 0.f, 0.f, 1.f, 0.f } }, // +Z
					{ &RI.fallbackTangentVertex, sizeof(float) * 4, { 1.f, 0.f, 0.f, 1.f } }, // +X, handedness +1
					{ &RI.fallbackColorVertex,   sizeof(float) * 4, { 1.f, 1.f, 1.f, 1.f } }, // white
					{ &RI.fallbackUv0Vertex,     sizeof(float) * 3, { 0.f, 0.f, 0.f, 0.f } }, // origin
				};
				for ( const FallbackSpec &s : specs ) {
					*s.target = RIBuffer::create(
						&RI.device, {(uint64_t)s.size,
						             RI_BUFFER_USAGE_VERTEX_BUFFER | RI_BUFFER_USAGE_TRANSFER_DST,
						             RI_MEMORY_HOST_UPLOAD, 0});
					if (s.target->isEmpty()) {
						FatalError("Failed to create fallback vertex buffer!\n");
						return false;
					}
					if (s.target->mappedAddress) {
						std::memcpy(s.target->mappedAddress, s.value, s.size);
						vmaFlushAllocation(RI.device.vk.vmaAllocator, s.target->vk.allocation, 0, VK_WHOLE_SIZE);
					}
				}
			}

			initElem.cmds[0].end( &RI.device );

			VkCommandBufferSubmitInfo cmdSubmitInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
			cmdSubmitInfo.commandBuffer = initElem.cmds[0].vk.cmd;

			VkSubmitInfo2 submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
			submitInfo.commandBufferInfoCount = 1;
			submitInfo.pCommandBufferInfos = &cmdSubmitInfo;

			VK_WrapResult( vkResetFences( RI.device.vk.device, 1, &initElem.vk.fence ) );
			VK_WrapResult( vkQueueSubmit2( RI.device.queues[RI_QUEUE_GRAPHICS].vk.queue, 1, &submitInfo, initElem.vk.fence ) );
			RI.device.queues[RI_QUEUE_GRAPHICS].waitIdle(&RI.device);

			if (!whiteUploadStaging.isEmpty()) {
				whiteUploadStaging.dispose(&RI.device);
			}
			// The one-time init upload used graphicsCmdRing, but it has completed.
			// Rewind the ring so the first real frame starts with a clean command pool.
			RI.graphicsCmdRing.cmdIndex = 0;
			RI.graphicsCmdRing.fenceIndex = 0;
			RI.primary = {};
		}
		{
			auto vert_stage = RIProgram::loadShaderStage(apResources->GetFileSearcher(), "gui.vert.spv");
			auto frag_stage = RIProgram::loadShaderStage(apResources->GetFileSearcher(), "gui.frag.spv");
			std::array<RIProgram::ModuleStage, 2> stages = {
				RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, vert_stage, "vsMain"},
				RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, frag_stage, "psMain"}
			};
			RI.gui.initialize(&RI.device, stages);
		}
		{
			auto vert_stage = RIProgram::loadShaderStage(apResources->GetFileSearcher(), "posteffect_fullscreen.vert.spv");
			auto frag_stage = RIProgram::loadShaderStage(apResources->GetFileSearcher(), "posteffect_blit.frag.spv");
			std::array<RIProgram::ModuleStage, 2> stages = {
				RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, vert_stage, "vsMain"},
				RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, frag_stage, "psMain"}
			};
			RI.postEffectBlit.initialize(&RI.device, stages);
		}
		////////////////////////////////////////////////
		// Create systems
		mpMeshCreator = hplNew( cMeshCreator,(mpLowLevelGraphics, apResources));
		mpTextureCreator  = hplNew( cTextureCreator,(mpLowLevelGraphics, apResources));
		mpDecalCreator = hplNew( cDecalCreator,(mpLowLevelGraphics, apResources));

		// Create Renderers
		if(alHplSetupFlags & eHplSetup_Screen)
		{

			mvRenderers.resize(eRenderer_LastEnum, NULL);
			mvRenderers[eRenderer_Main] = hplNew(cHybridRenderer, (this, apResources));
			mvRenderers[eRenderer_WireFrame] = hplNew(cRendererWireFrame, (this, apResources));
			mvRenderers[eRenderer_Simple] = hplNew(cRendererSimple, (this, apResources));

			// Editor / debug overlay batcher — flushed by HybridRenderer's
			// offscreen tail (and reusable for thumbnails / previews).
			mpDebugDraw = hplNew(DebugDraw, ());
			mpDebugDraw->Init(apResources);

			//for(size_t i=0; i<mvRenderers.size(); ++i)
			//{
			//	if(mvRenderers[i])
			//	{
			//		if(mvRenderers[i]->LoadData()==false)
			//		{
			//			FatalError("Renderer #%d could not be initialized! Make sure your graphic card drivers are up to date. Check log file for more information.\n", i);
			//		}
			//	}
			//}
		}
		
		////////////////////////////////////////////////
		// Create Data
		if(alHplSetupFlags & eHplSetup_Screen)
		{
			////////////////////////////////////////////////
			//Add all the materials.
			Log(" Adding engine materials\n");

			AddMaterialType(hplNew( cMaterialType_SolidDiffuse, (this, apResources) ), "soliddiffuse");
			AddMaterialType(hplNew( cMaterialType_Translucent, (this, apResources) ), "translucent");
			AddMaterialType(hplNew( cMaterialType_Water, (this, apResources) ), "water");
			AddMaterialType(hplNew( cMaterialType_Decal, (this, apResources) ), "decal");


			////////////////////////////////////////////////
			//Add all the post effects
			Log(" Adding engine post effects\n");
			AddPostEffectType(hplNew( cPostEffectType_Bloom, (this, apResources)) );
			AddPostEffectType(hplNew( cPostEffectType_ToneMap, (this, apResources)) );
			AddPostEffectType(hplNew( cPostEffectType_ColorConvTex, (this, apResources)) );
			AddPostEffectType(hplNew( cPostEffectType_ImageTrail, (this, apResources)) );
			AddPostEffectType(hplNew( cPostEffectType_RadialBlur, (this, apResources)) );
		}
		
		Log("--------------------------------------------------------\n\n");
		
		return true;
	}
	

	//-----------------------------------------------------------------------

	void cGraphics::Update(float afTimeStep)
	{
		for(size_t i=0; i< mvRenderers.size(); ++i)
		{
			iRenderer *pRenderer = mvRenderers[i];

			pRenderer->Update(afTimeStep);
		}
	}

	//-----------------------------------------------------------------------

	iRenderer* cGraphics::GetRenderer(eRenderer aType)
	{
		if(aType >= (int)mvRenderers.size()) return NULL;

		return mvRenderers[aType];
	}

	//-----------------------------------------------------------------------

	void cGraphics::ReloadRendererData()
	{
		for(size_t i=0; i< mvRenderers.size(); ++i)
		{
			iRenderer *pRenderer = mvRenderers[i];

			pRenderer->DestroyData();
			pRenderer->LoadData();
		}
	}

	//-----------------------------------------------------------------------

	cPostEffectComposite* cGraphics::CreatePostEffectComposite()
	{
		cPostEffectComposite *pComposite = hplNew( cPostEffectComposite, (this) );
		mlstPostEffectComposites.push_back(pComposite);

		return pComposite;
	}
	
	void cGraphics::DestroyPostEffectComposite(cPostEffectComposite* apComposite)
	{
		STLFindAndDelete(mlstPostEffectComposites, apComposite);
	}

	//-----------------------------------------------------------------------

	void  cGraphics::AddPostEffectType(iPostEffectType *apPostEffectBase)
	{
		mvPostEffectTypes.push_back(apPostEffectBase);
	}

	//-----------------------------------------------------------------------

	iPostEffect* cGraphics::CreatePostEffect(iPostEffectParams *apParams)
	{
		iPostEffectType *pType = (iPostEffectType*)STLFindByName(mvPostEffectTypes, apParams->GetName());
		if(pType == NULL){
			Error("Could not find post effect type %s\n", apParams->GetName().c_str());
			return NULL;
		}

		iPostEffect *pPostEffect = pType->CreatePostEffect(apParams);
		pPostEffect->SetParams(apParams);

		mlstPostEffects.push_back(pPostEffect);

		return pPostEffect;
	}
	
	//-----------------------------------------------------------------------

	void cGraphics::DestroyPostEffect(iPostEffect* apPostEffect)
	{
		STLFindAndDelete(mlstPostEffects,apPostEffect);
	}

	//-----------------------------------------------------------------------
	
	void cGraphics::AddMaterialType(iMaterialType *apType, const tString& asName)
	{
		apType->SetName(asName);
		apType->LoadData();
		m_mapMaterialTypes.insert(tMaterialTypeMap::value_type(asName, apType));
	}
	
	iMaterialType *cGraphics::GetMaterialType(const tString& asName)
	{
		tString sLowName = cString::ToLowerCase(asName);

		tMaterialTypeMapIt it = m_mapMaterialTypes.find(sLowName);
		if(it == m_mapMaterialTypes.end()) return NULL;

		return it->second;
	}

	tStringVec cGraphics::GetMaterialTypeNames()
	{
		tStringVec vNames;
		tMaterialTypeMapIt it = m_mapMaterialTypes.begin();
		for(;it!=m_mapMaterialTypes.end();++it)
		{
			vNames.push_back(it->first);
		}

		return vNames;
	}

	void cGraphics::ReloadMaterials()
	{
		tMaterialTypeMapIt it = m_mapMaterialTypes.begin();
		for(;it != m_mapMaterialTypes.end(); ++it)
		{
			iMaterialType *pType = it->second;
			pType->Reload();
		}
	}

	//-----------------------------------------------------------------------

}
