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

	// Selects which renderer backend to create. The compiled-in backends are the
	// candidates; when more than one is available (a VK+MTL fat binary) prefer
	// Metal on Apple platforms and Vulkan elsewhere. A future config/env override
	// (e.g. HPL_RENDER_BACKEND) can be slotted in here without touching the call
	// site in Init.
	static RIDeviceAPI_e SelectRenderBackendAPI()
	{
#if (DEVICE_IMPL_VULKAN) && (DEVICE_IMPL_MTL)
	#if defined(__APPLE__)
		return RI_DEVICE_API_MTL;
	#else
		return RI_DEVICE_API_VK;
	#endif
#elif (DEVICE_IMPL_VULKAN)
		return RI_DEVICE_API_VK;
#elif (DEVICE_IMPL_MTL)
		return RI_DEVICE_API_MTL;
#else
		return RI_DEVICE_API_UNKNOWN;
#endif
	}

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
		cMaterial::SetDestroyTypeSpecifics(false); //Material types are destroyed! Remaining materials may not call!

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
		backendInit.applicationName = "HPL2";
		backendInit.api = SelectRenderBackendAPI();
#if (DEVICE_IMPL_VULKAN)
		if(backendInit.api == RI_DEVICE_API_VK)
			backendInit.vk.enableValidationLayer = false;
#endif

		if(RI.renderer.init(&backendInit) != RI_SUCCESS) {
			return false;
		}

		uint32_t numAdapters = 0;
		if( RI.renderer.enumerateAdapters( NULL, &numAdapters ) != RI_SUCCESS ) {
			return false;
		}
		assert(numAdapters > 0);
		std::vector<RIPhysicalAdapter> physicalAdapters(numAdapters);

		if(RI.renderer.enumerateAdapters(physicalAdapters.data(), &numAdapters) != RI_SUCCESS) {
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
		RI.device.init(&RI.renderer, &deviceInit );
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
				// The SurfelGI composite + post-effect chain write into the
				// viewport backbuffer, so the swapchain view only needs to be a
				// color attachment over the swapchain's per-image texture.
				RITextureViewDesc viewDesc = {};
				viewDesc.viewType = RI_VIEWTYPE_COLOR_ATTACHMENT;
				viewDesc.format = RI.swapchain.format;
				viewDesc.mipNum = 1;
				viewDesc.layerNum = 1;
				RI.swapchainView[i] = RITextureView::create( &RI.device, &RI.swapchain.textures[i], viewDesc );
			}
		}

		struct RIQueue *graphicsQueue = &RI.device.queues[RI_QUEUE_GRAPHICS];
		RI.graphicsCmdRing.init( &RI.device, graphicsQueue,
		                         RI_NUMBER_FRAMES_FLIGHT, RI_NUMBER_SUB_COMMANDS, true );
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
				whiteDesc.depth = 1;
				whiteDesc.mipNum = 1;
				whiteDesc.layerNum = 1;
				whiteDesc.usage = RI_USAGE_SHADER_RESOURCE | RI_USAGE_TRANSFER_DST;
				RI.whiteTexture2D = RITexture::create(&RI.device, whiteDesc);

				const uint8_t whitePixel[4] = { 255, 255, 255, 255 };
				RIBufferDesc stagingDesc = {};
				stagingDesc.size = sizeof(whitePixel);
				stagingDesc.usage = RI_BUFFER_USAGE_TRANSFER_SRC;
				stagingDesc.location = RI_MEMORY_HOST_UPLOAD;
				whiteUploadStaging = RIBuffer::create(&RI.device, stagingDesc);
				memcpy(whiteUploadStaging.mappedAddress, whitePixel, sizeof(whitePixel));
				whiteUploadStaging.flush(&RI.device);

				RITextureBarrier toTransfer = {};
				toTransfer.texture = &RI.whiteTexture2D;
				toTransfer.before = RI_RESOURCE_STATE_UNDEFINED;
				toTransfer.after = RI_RESOURCE_STATE_COPY_DST;
				toTransfer.afterStages = RI_STAGE_COPY;
				toTransfer.mipCount = 1;
				toTransfer.layerCount = 1;
				initElem.cmds[0].vk_d3d12_textureBarrier(toTransfer);

				RIBufferTextureCopyDesc copyRegion = {};
				copyRegion.width = 1;
				copyRegion.height = 1;
				copyRegion.depth = 1;
				copyRegion.bytesPerRow = 4;   // Metal: 1px * RGBA8
				copyRegion.bytesPerImage = 4; // bufferRowLength/Height stay 0 (VK tight-packed)
				initElem.cmds[0].copyBufferToTexture(&RI.renderer, &whiteUploadStaging, &RI.whiteTexture2D, copyRegion);

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

				RITextureViewDesc whiteView = {};
				whiteView.viewType = RI_VIEWTYPE_SHADER_RESOURCE_2D;
				whiteView.format = RI_FORMAT_RGBA8_UNORM;
				whiteView.mipNum = 1;
				whiteView.layerNum = 1;
				RI.whiteTexture2DView = RITextureView::create(&RI.device, &RI.whiteTexture2D, whiteView);

				RI.whiteTexture2DBinding = RIDescriptor::sampledImage(&RI.device, &RI.whiteTexture2DView, hash_random());
				RI.whiteTexture2DBinding.texture = &RI.whiteTexture2D;
			}

			// Zero-filled vertex buffer — small mapped buffer, never modified after init.
			{
				constexpr uint64_t kNulVertexSize = 64;
				RIBufferDesc nulDesc = {};
				nulDesc.size = kNulVertexSize;
				nulDesc.usage = RI_BUFFER_USAGE_VERTEX_BUFFER | RI_BUFFER_USAGE_TRANSFER_DST;
				nulDesc.location = RI_MEMORY_HOST_UPLOAD;
				RI.nulVertexBuffer = RIBuffer::create(&RI.device, nulDesc);
				if (RI.nulVertexBuffer.mappedAddress) {
					memset(RI.nulVertexBuffer.mappedAddress, 0, kNulVertexSize);
					RI.nulVertexBuffer.flush(&RI.device);
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
					RIBufferDesc fbDesc = {};
					fbDesc.size = s.size;
					fbDesc.usage = RI_BUFFER_USAGE_VERTEX_BUFFER | RI_BUFFER_USAGE_TRANSFER_DST;
					fbDesc.location = RI_MEMORY_HOST_UPLOAD;
					*s.target = RIBuffer::create(&RI.device, fbDesc);
					if (s.target->mappedAddress) {
						std::memcpy(s.target->mappedAddress, s.value, s.size);
						s.target->flush(&RI.device);
					}
				}
			}

			initElem.cmds[0].end( &RI.device );

			// One-shot init submit: signal the element's fence/event, then block
			// on the queue until the upload completes.
			initElem.submit( &RI.device, &RI.device.queues[RI_QUEUE_GRAPHICS] );
			RI.device.queues[RI_QUEUE_GRAPHICS].waitIdle(&RI.device);

			whiteUploadStaging.dispose(&RI.device);
			// The one-time init upload used graphicsCmdRing, but it has completed.
			// Rewind the ring so the first real frame starts with a clean command pool.
			RI.graphicsCmdRing.cmdIndex = 0;
			RI.graphicsCmdRing.fenceIndex = 0;
			RI.primary = {};
		}
		{
			// Explicit binding tables for gui.vert.slang / gui.frag.slang (set 0,
			// all program-managed — no external sets). Host wires these by name in
			// cGuiSet::Render: "pass" (UBO via UpdateFrameUBO), "diffuseSampler",
			// "diffuseMap". No push constant (the GuiPass block is a ConstantBuffer).
			//   Vert (gui.vert.slang):  [vk::binding(0,0)] ConstantBuffer<GuiPass> pass
			//   Frag (gui.frag.slang):  [vk::binding(0,0)] ConstantBuffer<GuiPass> pass
			//                           [vk::binding(1,0)] SamplerState diffuseSampler
			//                           [vk::binding(2,0)] Texture2D     diffuseMap
			// Generated MSL:
			//   gui.vert.metal: pass [[buffer(0)]]
			//   gui.frag.metal: pass [[buffer(0)]], diffuseMap [[texture(0)]],
			//                   diffuseSampler [[sampler(0)]]
			static const RIProgram::RIProgramBinding kGui[] = {
				{"pass", RI_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, RI_SHADER_STAGE_VERTEX | RI_SHADER_STAGE_FRAGMENT, {0, 0}, {}, {0}},
				{"diffuseSampler", RI_DESCRIPTOR_TYPE_SAMPLER, 1, RI_SHADER_STAGE_FRAGMENT, {0, 1}, {}, {0}},
				{"diffuseMap", RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, RI_SHADER_STAGE_FRAGMENT, {0, 2}, {}, {0}},
			};

			auto vert_stage = RIProgram::loadShaderStage(apResources->GetFileSearcher(), "gui.vert.spv");
			auto frag_stage = RIProgram::loadShaderStage(apResources->GetFileSearcher(), "gui.frag.spv");
			std::array<RIProgram::ModuleStage, 2> stages = {
				RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, vert_stage, "vsMain"},
				RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, frag_stage, "psMain"}
			};
			RIProgram::RIProgramDescriptor desc = {};
			desc.stages = stages;
			desc.bindings = kGui;
			RI.gui.initialize(&RI.device, desc);
		}
		{
			// Explicit binding tables for posteffect_blit.frag.slang (set 0, all
			// program-managed). Host wires "inputSampler"/"sourceInput" by name in
			// cViewport::DrawPogoToTarget. No push constant. The fullscreen vertex
			// stage (posteffect_fullscreen.vert.slang) has no descriptors (vs empty).
			//   Frag (posteffect_blit.frag.slang):
			//     [vk::binding(0,0)] SamplerState      inputSampler
			//     [vk::binding(1,0)] Texture2D<float4> sourceInput
			// Generated MSL (posteffect_blit.frag.metal):
			//   sourceInput [[texture(0)]], inputSampler [[sampler(0)]]
			static const RIProgram::RIProgramBinding kBlit[] = {
				{"inputSampler", RI_DESCRIPTOR_TYPE_SAMPLER, 1, RI_SHADER_STAGE_FRAGMENT, {0, 0}, {}, {0}},
				{"sourceInput", RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, RI_SHADER_STAGE_FRAGMENT, {0, 1}, {}, {0}},
			};

			auto vert_stage = RIProgram::loadShaderStage(apResources->GetFileSearcher(), "posteffect_fullscreen.vert.spv");
			auto frag_stage = RIProgram::loadShaderStage(apResources->GetFileSearcher(), "posteffect_blit.frag.spv");
			std::array<RIProgram::ModuleStage, 2> stages = {
				RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, vert_stage, "vsMain"},
				RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, frag_stage, "psMain"}
			};
			RIProgram::RIProgramDescriptor desc = {};
			desc.stages = stages;
			desc.bindings = kBlit;
			RI.postEffectBlit.initialize(&RI.device, desc);
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
