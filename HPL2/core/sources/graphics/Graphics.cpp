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
#include "graphics/FrameBuffer.h"
#include "graphics/PostEffectComposite.h"
#include "graphics/PostEffect.h"
#include "graphics/MaterialType.h"
#include "graphics/Texture.h"
#include "graphics/GPUProgram.h"
#include "graphics/RIRenderer.h"
#include "graphics/RIResourceUploader.h"
#include "graphics/RITypes.h"
#include "graphics/RIVK.h"
#include "graphics/HybridRenderer.h"

#include "resources/LowLevelResources.h"
#include "resources/Resources.h"
#include "resources/GpuShaderManager.h"
#include "resources/FileSearcher.h"

#include "graphics/MaterialType_BasicSolid.h"
#include "graphics/MaterialType_BasicTranslucent.h"
#include "graphics/MaterialType_Water.h"
#include "graphics/MaterialType_Decal.h"

#include "graphics/PostEffect_Bloom.h"
#include "graphics/PostEffect_ColorConvTex.h"
#include "graphics/PostEffect_ImageTrail.h"
#include "graphics/PostEffect_RadialBlur.h"

#include "graphics/RendererDeferred.h"
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
		STLDeleteAll(mlstFrameBuffers);
		STLDeleteAll(mlstDepthStencilBuffers);
		STLDeleteAll(mlstGpuPrograms);
		STLDeleteAll(mlstTextures);

		hplDelete(mpMeshCreator);
		hplDelete(mpTextureCreator);
		hplDelete(mpDecalCreator);

		Log("--------------------------------------------------------\n\n");
	}

	bool cGraphics::Init(	int alWidth, int alHeight, int alDisplay, int alBpp, int abFullscreen, 
							int alMultisampling,eGpuProgramFormat aGpuProgramFormat,
							const tString &asWindowCaption, const cVector2l &avWindowPos,
							cResources* apResources,
							tFlag alHplSetupFlags)
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
			Log("Init lowlevel graphics: %dx%d disp:%d bpp:%d fs:%d ms:%d gpufmt:%d cap:'%s' pos:(%dx%d)\n",alWidth,alHeight,alDisplay,alBpp,abFullscreen,alMultisampling,aGpuProgramFormat, asWindowCaption.c_str(), avWindowPos.x,avWindowPos.y);
			mpLowLevelGraphics->Init(alWidth,alHeight,alDisplay,alBpp,abFullscreen,alMultisampling,aGpuProgramFormat,asWindowCaption,
									avWindowPos);
			mbScreenIsSetup = true;
		}
		else
		{
			mbScreenIsSetup = false;
		}
		{
		struct RIBackendInit_s backendInit = {};
		backendInit.api = RI_DEVICE_API_VK;
		backendInit.applicationName = "HPL2";
#ifndef NDEBUG
		backendInit.vk.enableValidationLayer = true;
#else
		backendInit.vk.enableValidationLayer = false;
#endif

		if(InitRIRenderer(&backendInit, &RI.renderer) != RI_SUCCESS) {
			return false;
		}

		uint32_t numAdapters = 0;
		if( EnumerateRIAdapters( &RI.renderer, NULL, &numAdapters ) != RI_SUCCESS ) {
			return false;
		}
		assert(numAdapters > 0);
		std::vector<RIPhysicalAdapter_s> physicalAdapters(numAdapters);

		if(EnumerateRIAdapters(&RI.renderer, physicalAdapters.data(), &numAdapters) != RI_SUCCESS) {
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
		struct RIDeviceDesc_s deviceInit = { 0 };
		deviceInit.physicalAdapter = &physicalAdapters[selectedAdapterIdx];
		InitRIDevice(&RI.renderer, &deviceInit, &RI.device );
		RI_InitResourceUploader(&RI.device, &RI.uploader);
		struct RIWindowHandle_s windowHandle = mpLowLevelGraphics->GetWindowHandle(); 
		if(windowHandle.type == RI_WINDOW_UNKNOWN) {
			FatalError("Failed to find valid window handle!\n");
			return false;
		}
		struct RISwapchainDesc_s swapchainInit = { 0 };
		swapchainInit.windowHandle = &windowHandle;
		swapchainInit.requestImageCount = RI_NUMBER_FRAMES_FLIGHT;
		swapchainInit.queue = &RI.device.queues[RI_QUEUE_GRAPHICS];
		swapchainInit.width = alWidth;
		swapchainInit.height = alHeight;
		swapchainInit.format = RI_SWAPCHAIN_BT709_G10_16BIT;
		InitRISwapchain(&RI.device, &swapchainInit, &RI.swapchain);

		{
			uint32_t queueFamilies[RI_QUEUE_LEN] = { 0 };
			assert( RI.swapchain.imageCount > 0 );
			for( uint32_t i = 0; i < RI.swapchain.imageCount; i++ ) {
				VmaAllocationCreateInfo mem_reqs = { 0 };
				mem_reqs.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

				{
					VkImageViewUsageCreateInfo usageInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO };
					VkImageViewCreateInfo createInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
					createInfo.pNext = &usageInfo;
					createInfo.subresourceRange = VkImageSubresourceRange{
						VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1,
					};
					// Mirrors the swapchain's image-level usage. The SurfelGI
					// composite + post-effect chain now write into the pogo
					// buffer, so the swapchain view only needs COLOR_ATTACHMENT.
					usageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
					createInfo.image = RI.swapchain.vk.images[i];
					createInfo.format = RIFormatToVK( RI.swapchain.format );
					createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D; // | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
					//RI.colorAttachment[i].flags |= RI_VK_DESC_OWN_IMAGE_VIEW;
					//RI.colorAttachment[i].texture = &RI.swapchain.textures[i];
					//RI.colorAttachment[i].vk.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
					//RI.colorAttachment[i].vk.image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
					VK_WrapResult( vkCreateImageView( RI.device.vk.device, &createInfo, NULL, &RI.swapchainView[i].vk.image) );

					//VK_WrapResult( vkCreateImageView( RI.device.vk.device, &createInfo, NULL, &RI.colorAttachment[i].vk.image.imageView ) );
					//RIFinalizeDescriptor( &RI.device, &RI.colorAttachment[i] );
				}
				{
					VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
					info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
					info.imageType = VK_IMAGE_TYPE_2D;
					info.extent.width = RI.swapchain.width;
					info.extent.height = RI.swapchain.height;
					info.extent.depth = 1;
					info.mipLevels = 1;
					info.arrayLayers = 1;
					info.samples = VK_SAMPLE_COUNT_1_BIT;
					info.tiling = VK_IMAGE_TILING_OPTIMAL;
					info.pQueueFamilyIndices = queueFamilies;
					VK_ConfigureImageQueueFamilies( &info, RI.device.queues, RI_QUEUE_LEN, queueFamilies, RI_QUEUE_LEN );
					info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
					info.format = RIFormatToVK( RIBootstrap::DepthFormat);
					// SAMPLED_BIT lets surfel_generate / surfel_raytrace bind the
					// depth as `sampler2D depthMap` after the gbuffer pass
					// transitions it to SHADER_READ_ONLY_OPTIMAL.
					info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
								 VK_IMAGE_USAGE_SAMPLED_BIT;
					VK_WrapResult( vmaCreateImage( RI.device.vk.vmaAllocator, &info, &mem_reqs, &RI.depthTextures[i].vk.image, &RI.depthTextures[i].vk.allocation, NULL ) );
				}
				{
					VkImageViewCreateInfo createInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
					createInfo.format = RIFormatToVK( RIBootstrap::DepthFormat );
					createInfo.subresourceRange = VkImageSubresourceRange{
						VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1,
					};
					createInfo.image = RI.depthTextures[i].vk.image;
					createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
					VK_WrapResult( vkCreateImageView( RI.device.vk.device, &createInfo, NULL, &RI.depthView[i].vk.image ) );
				}
				{

					VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
					info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
					info.imageType = VK_IMAGE_TYPE_2D;
					info.extent.width = RI.swapchain.width;
					info.extent.height = RI.swapchain.height;
					info.extent.depth = 1;
					info.mipLevels = 1;
					info.arrayLayers = 1;
					info.samples = VK_SAMPLE_COUNT_1_BIT;
					info.tiling = VK_IMAGE_TILING_OPTIMAL;
					info.pQueueFamilyIndices = queueFamilies;
					VK_ConfigureImageQueueFamilies( &info, RI.device.queues, RI_QUEUE_LEN, queueFamilies, RI_QUEUE_LEN );
					info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
					info.format = RIFormatToVK( RIBootstrap::VisibilityFormat);
					info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
					VK_WrapResult( vmaCreateImage( RI.device.vk.vmaAllocator, &info, &mem_reqs, &RI.visibilityTexture[i].vk.image, &RI.visibilityTexture[i].vk.allocation, NULL ) );
				}
				{
					VkImageViewCreateInfo createInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
					createInfo.format = RIFormatToVK( RIBootstrap::VisibilityFormat);
					createInfo.subresourceRange = VkImageSubresourceRange{
						VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1,
					};
					createInfo.image = RI.visibilityTexture[i].vk.image;
					createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
					VK_WrapResult( vkCreateImageView( RI.device.vk.device, &createInfo, NULL, &RI.visibilityView[i].vk.image ) );
				}
				// The separate normal MRT target was retired when the gbuffer
				// switched to the packed-TriangleHit (RGBA32_UINT visibility)
				// format — instId/primId/barycentrics now encode everything the
				// surfel and lighting passes need, so RIBootstrap::NormalFormat /
				// normalTexture / normalView no longer exist.
				RI_PogoBufferInit( &RI.device, &RI.pogoBuffer[i], RI.swapchain.width, RI.swapchain.height, RI_FORMAT_RGBA8_UNORM );
			}
		}

		struct RIQueue_s *graphicsQueue = &RI.device.queues[RI_QUEUE_GRAPHICS];
		InitRICommandRingBuffer( &RI.device, graphicsQueue, &RI.graphicsCmdRing,
		                         RI_NUMBER_FRAMES_FLIGHT, RI_NUMBER_SUB_COMMANDS, true );
		for(auto& set: RI.frameSets) {
			struct RIScratchAllocDesc_s uboDesc = {
					.blockSize = 256 * 128,
					.alignmentReq = RI.device.physicalAdapter.constantBufferOffsetAlignment,
					.alloc = RIUniformScratchAllocHandler };
			InitRIScratchAlloc( &RI.device, &set.uboScratchAlloc, &uboDesc );

			// AS build scratch pool. 1 MiB blocks fit typical TLAS/BLAS scratch
			// for moderate scenes; oversized builds spill through the allocator's
			// one-shot path.
			struct RIScratchAllocDesc_s accelDesc = {
					.blockSize = 1024 * 1024,
					.alignmentReq = RI.device.physicalAdapter.accelerationStructureScratchOffsetAlignment,
					.alloc = RIAccelScratchAllocHandler };
			InitRIScratchAlloc( &RI.device, &set.accelScratchAlloc, &accelDesc );
		}
		}
		{
			struct RICommandRingElement_s initElem = GetRICommandRingElement( &RI.device, &RI.graphicsCmdRing, 1 );
			ResetRIPool( &RI.device, initElem.pool );
			BeginRICmd( &RI.device, &initElem.cmds[0] );

			VkBuffer whiteUploadStaging = VK_NULL_HANDLE;
			VmaAllocation whiteUploadStagingAlloc = VK_NULL_HANDLE;

			// 1x1 white texture — staged upload, then transitioned to SHADER_READ_ONLY_OPTIMAL.
			{
				VkImageCreateInfo imageInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
				imageInfo.imageType = VK_IMAGE_TYPE_2D;
				imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
				imageInfo.extent = { 1, 1, 1 };
				imageInfo.mipLevels = 1;
				imageInfo.arrayLayers = 1;
				imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
				imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
				imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
				imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
				imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

				uint32_t imageQueueFamilies[RI_QUEUE_LEN] = { 0 };
				imageInfo.pQueueFamilyIndices = imageQueueFamilies;
				VK_ConfigureImageQueueFamilies(&imageInfo, RI.device.queues, RI_QUEUE_LEN, imageQueueFamilies, RI_QUEUE_LEN);

				VmaAllocationCreateInfo imageAllocInfo = {};
				imageAllocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
				if (!VK_WrapResult(vmaCreateImage(RI.device.vk.vmaAllocator, &imageInfo, &imageAllocInfo,
				                                  &RI.whiteTexture2D.vk.image, &RI.whiteTexture2D.vk.allocation, nullptr))) {
					FatalError("Failed to create white texture image!\n");
					return false;
				}

				const uint8_t whitePixel[4] = { 255, 255, 255, 255 };
				VkBufferCreateInfo bufferInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
				bufferInfo.size = sizeof(whitePixel);
				bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
				bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

				VmaAllocationCreateInfo stagingAllocInfo = {};
				stagingAllocInfo.usage = VMA_MEMORY_USAGE_AUTO;
				stagingAllocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
				                         VMA_ALLOCATION_CREATE_MAPPED_BIT;
				VmaAllocationInfo stagingInfo = {};
				if (!VK_WrapResult(vmaCreateBuffer(RI.device.vk.vmaAllocator, &bufferInfo, &stagingAllocInfo,
				                                   &whiteUploadStaging, &whiteUploadStagingAlloc, &stagingInfo))) {
					FatalError("Failed to create white texture staging buffer!\n");
					return false;
				}
				memcpy(stagingInfo.pMappedData, whitePixel, sizeof(whitePixel));
				vmaFlushAllocation(RI.device.vk.vmaAllocator, whiteUploadStagingAlloc, 0, VK_WHOLE_SIZE);

				VkImageSubresourceRange colorRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

				VkImageMemoryBarrier2 toTransfer = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
				toTransfer.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
				toTransfer.srcAccessMask = 0;
				toTransfer.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
				toTransfer.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
				toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
				toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
				toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				toTransfer.image = RI.whiteTexture2D.vk.image;
				toTransfer.subresourceRange = colorRange;
				VkDependencyInfo depToTransfer = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
				depToTransfer.imageMemoryBarrierCount = 1;
				depToTransfer.pImageMemoryBarriers = &toTransfer;
				vkCmdPipelineBarrier2(initElem.cmds[0].vk.cmd, &depToTransfer);

				VkBufferImageCopy copyRegion = {};
				copyRegion.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
				copyRegion.imageExtent = { 1, 1, 1 };
				vkCmdCopyBufferToImage(initElem.cmds[0].vk.cmd, whiteUploadStaging, RI.whiteTexture2D.vk.image,
				                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

				VkImageMemoryBarrier2 toShaderRead = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
				toShaderRead.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
				toShaderRead.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
				toShaderRead.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
				toShaderRead.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
				toShaderRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
				toShaderRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				toShaderRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				toShaderRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				toShaderRead.image = RI.whiteTexture2D.vk.image;
				toShaderRead.subresourceRange = colorRange;
				VkDependencyInfo depToShaderRead = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
				depToShaderRead.imageMemoryBarrierCount = 1;
				depToShaderRead.pImageMemoryBarriers = &toShaderRead;
				vkCmdPipelineBarrier2(initElem.cmds[0].vk.cmd, &depToShaderRead);

				VkImageViewCreateInfo viewInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
				viewInfo.image = RI.whiteTexture2D.vk.image;
				viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
				viewInfo.format = imageInfo.format;
				viewInfo.subresourceRange = colorRange;
				if (!VK_WrapResult(vkCreateImageView(RI.device.vk.device, &viewInfo, nullptr,
				                                    &RI.whiteTexture2DBinding.vk.image.imageView))) {
					FatalError("Failed to create white texture image view!\n");
					return false;
				}
				RI.whiteTexture2DBinding.flags |= RI_VK_DESC_OWN_IMAGE_VIEW;
				RI.whiteTexture2DBinding.texture = &RI.whiteTexture2D;
				RI.whiteTexture2DBinding.vk.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
				RI.whiteTexture2DBinding.vk.image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				RIFinalizeDescriptor(&RI.device, &RI.whiteTexture2DBinding);
			}

			// Zero-filled vertex buffer — small mapped buffer, never modified after init.
			{
				constexpr VkDeviceSize kNulVertexSize = 64;
				uint32_t queueFamilies[RI_QUEUE_LEN] = { 0 };
				VkBufferCreateInfo bufferInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
				bufferInfo.size = kNulVertexSize;
				bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
				VK_ConfigureBufferQueueFamilies(&bufferInfo, RI.device.queues, RI_QUEUE_LEN, queueFamilies, RI_QUEUE_LEN);

				VmaAllocationCreateInfo allocInfo = {};
				allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
				allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
				                  VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
				VmaAllocationInfo allocationInfo = {};
				if (!VK_WrapResult(vmaCreateBuffer(RI.device.vk.vmaAllocator, &bufferInfo, &allocInfo,
				                                   &RI.nulVertexBuffer.vk.buffer, &RI.nulVertexBuffer.vk.allocation,
				                                   &allocationInfo))) {
					FatalError("Failed to create null vertex buffer!\n");
					return false;
				}
				if (allocationInfo.pMappedData) {
					memset(allocationInfo.pMappedData, 0, kNulVertexSize);
					vmaFlushAllocation(RI.device.vk.vmaAllocator, RI.nulVertexBuffer.vk.allocation, 0, VK_WHOLE_SIZE);
				}
				RI.nulVertexBuffer.mappedAddress = allocationInfo.pMappedData;
			}

			EndRICmd( &RI.device, &initElem.cmds[0] );

			VkCommandBufferSubmitInfo cmdSubmitInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
			cmdSubmitInfo.commandBuffer = initElem.cmds[0].vk.cmd;

			VkSubmitInfo2 submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
			submitInfo.commandBufferInfoCount = 1;
			submitInfo.pCommandBufferInfos = &cmdSubmitInfo;

			VK_WrapResult( vkResetFences( RI.device.vk.device, 1, &initElem.vk.fence ) );
			VK_WrapResult( vkQueueSubmit2( RI.device.queues[RI_QUEUE_GRAPHICS].vk.queue, 1, &submitInfo, initElem.vk.fence ) );
			WaitRIQueueIdle(&RI.device, &RI.device.queues[RI_QUEUE_GRAPHICS]);

			if (whiteUploadStaging != VK_NULL_HANDLE) {
				vmaDestroyBuffer(RI.device.vk.vmaAllocator, whiteUploadStaging, whiteUploadStagingAlloc);
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
		////////////////////////////////////////////////
		// Create systems
		mpMeshCreator = hplNew( cMeshCreator,(mpLowLevelGraphics, apResources));
		mpTextureCreator  = hplNew( cTextureCreator,(mpLowLevelGraphics, apResources));
		mpDecalCreator = hplNew( cDecalCreator,(mpLowLevelGraphics, apResources));

		// Create Renderers
		if(alHplSetupFlags & eHplSetup_Screen)
		{
      // Check feature support
      apResources->GetGpuShaderManager()->CheckFeatureSupport();

			mvRenderers.resize(eRenderer_LastEnum, NULL);
			mvRenderers[eRenderer_Main] = hplNew(cHybridRenderer, (this, apResources));
			//mvRenderers[eRenderer_WireFrame] = hplNew(cRendererWireFrame, (this, apResources));
			//mvRenderers[eRenderer_Simple] = hplNew(cRendererSimple, (this, apResources));

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
	
	iFrameBuffer* cGraphics::CreateFrameBuffer(const tString& asName)
	{
		iFrameBuffer* pFrameBuffer = mpLowLevelGraphics->CreateFrameBuffer(asName);
		if(pFrameBuffer == NULL)
		{
			Error("Could not create a frame buffer!\n");
			return NULL;
		}

		mlstFrameBuffers.push_back(pFrameBuffer);

		return pFrameBuffer;
	}
	
	void cGraphics::DestroyFrameBuffer(iFrameBuffer* apFrameBuffer)
	{
		STLFindAndDelete(mlstFrameBuffers,apFrameBuffer);
	}

	//-----------------------------------------------------------------------

	iFrameBuffer* cGraphics::GetTempFrameBuffer(const cVector2l& avSize, ePixelFormat aPixelFormat, int alIndex)
	{
		/////////////////////////
		// Try and find existing frame buffer
		for(size_t i=0; i<mvTempFrameBuffers.size(); ++i)
		{
			cTempFrameBuffer &tempBuffer = mvTempFrameBuffers[i];
			if(	tempBuffer.mvSize == avSize && tempBuffer.mPixelFormat == aPixelFormat && 
				tempBuffer.mlIndex == alIndex)
			{
				return tempBuffer.mpFrameBuffer;
			}
		}

		/////////////////////////
		// Create new buffer
		cTempFrameBuffer tempBuffer;
		tempBuffer.mvSize = avSize;
		tempBuffer.mPixelFormat = aPixelFormat;
		tempBuffer.mlIndex = alIndex;

		//Create texture
		tString sNameSuffix = cString::ToString(avSize.x)+"x"+cString::ToString(avSize.y)+":"+cString::ToString((int)aPixelFormat);
		iTexture *pTexture = CreateTexture("TempBufferTexture"+sNameSuffix, eTextureType_Rect, eTextureUsage_RenderTarget);
		pTexture->CreateFromRawData(cVector3l(avSize.x, avSize.y,0),aPixelFormat,NULL);
		pTexture->SetWrapSTR(eTextureWrap_ClampToEdge);

		//Create frame buffer
    iFrameBuffer *pFrameBuffer = CreateFrameBuffer("TempBuffer"+sNameSuffix);
		pFrameBuffer->SetTexture2D(0, pTexture);
		if(pFrameBuffer->CompileAndValidate()==false)
		{
			DestroyFrameBuffer(pFrameBuffer);
			DestroyTexture(pTexture);
			return NULL;
		}
		tempBuffer.mpFrameBuffer = pFrameBuffer;

		mvTempFrameBuffers.push_back(tempBuffer);

		return pFrameBuffer;
	}

	//-----------------------------------------------------------------------

	iDepthStencilBuffer* cGraphics::CreateDepthStencilBuffer(const cVector2l& avSize, int alDepthBits, int alStencilBits, bool abLookForMatchingFirst)
	{
		iDepthStencilBuffer* pBuffer = NULL;
		
		//////////////////////////////////////////
		// Check for matching
		if(abLookForMatchingFirst)
		{
			pBuffer = FindDepthStencilBuffer(avSize, alDepthBits, alStencilBits);
		}
		//////////////////////////////////////////
		// Create frame buffer and add to list
		if(pBuffer == NULL)
		{
			pBuffer = mpLowLevelGraphics->CreateDepthStencilBuffer(avSize,alDepthBits,alStencilBits);
			if(pBuffer == NULL)
			{
				Error("Could not create a depth stencil buffer size %dx%d, depthbits: %d stencilbits: %d\n",avSize.x, avSize.y,alDepthBits,alStencilBits);
				return NULL;
			}

			mlstDepthStencilBuffers.push_back(pBuffer);
		}
		
		//////////////////////////////////////////
		// Increase user count and return
		pBuffer->IncUserCount();

		return pBuffer;
	}

	//-----------------------------------------------------------------------

	iDepthStencilBuffer* cGraphics::FindDepthStencilBuffer(const cVector2l& avSize, int alMinDepthBits, int alMinStencilBits)
	{
		tDepthStencilBufferListIt it = mlstDepthStencilBuffers.begin();
		for(; it != mlstDepthStencilBuffers.end(); ++it)
		{
			iDepthStencilBuffer *pBuffer = *it;
            
			if(	pBuffer->GetSize() == avSize && 
				pBuffer->GetDepthBits() >= alMinDepthBits && 
				pBuffer->GetStencilBits() >= alMinStencilBits)
			{
				return pBuffer;
			}
		}
		return NULL;
	}

	//-----------------------------------------------------------------------

	void cGraphics::DestoroyDepthStencilBuffer(iDepthStencilBuffer* apBuffer)
	{
		apBuffer->DecUserCount();
		
		if(apBuffer->HasUsers()==false)
		{
			STLFindAndDelete(mlstDepthStencilBuffers,apBuffer);
		}
	}

	//-----------------------------------------------------------------------

	iTexture* cGraphics::CreateTexture(const tString &asName,eTextureType aType,   eTextureUsage aUsage)
	{	
		iTexture *pTexture = mpLowLevelGraphics->CreateTexture(asName,aType, aUsage);
		mlstTextures.push_back(pTexture);
		return pTexture;
	}

	void cGraphics::DestroyTexture(iTexture *apTexture)
	{
		STLFindAndDelete(mlstTextures, apTexture);
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

	iGpuProgram* cGraphics::CreateGpuProgram(const tString& asName)
	{
		iGpuProgram *pProgram = mpLowLevelGraphics->CreateGpuProgram(asName);
		pProgram->SetResources(mpResources);
		mlstGpuPrograms.push_back(pProgram);

		return pProgram;
	}

	iGpuProgram* cGraphics::CreateGpuProgramFromShaders(const tString& asName, const tString& asVtxShader,const tString& asFragShader,
														cParserVarContainer *apVarContainer)
	{
		iGpuShader *pVtxShader = mpResources->GetGpuShaderManager()->CreateShader(asVtxShader,eGpuShaderType_Vertex,apVarContainer);
		if(pVtxShader==NULL) return NULL;
		iGpuShader *pFragShader = mpResources->GetGpuShaderManager()->CreateShader(asFragShader,eGpuShaderType_Fragment,apVarContainer);
		if(pFragShader==NULL){
			mpResources->GetGpuShaderManager()->Destroy(pVtxShader);
			return NULL;
		}

		iGpuProgram *pProgram = CreateGpuProgram(asName);
		pProgram->SetShader(eGpuShaderType_Vertex, pVtxShader);
		pProgram->SetShader(eGpuShaderType_Fragment, pFragShader);
		pProgram->Link();

    return pProgram;		
	}
	
	void cGraphics::DestroyGpuProgram(iGpuProgram* apProgram)
	{
		STLFindAndDelete(mlstGpuPrograms, apProgram);
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
