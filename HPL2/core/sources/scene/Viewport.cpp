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

#include "scene/Viewport.h"

#include "graphics/RIBootstrap.h"
#include "graphics/RIRenderer.h"
#include "graphics/RIVK.h"
#include "graphics/Renderer.h"

#include "system/LowLevelSystem.h"

#if (DEVICE_IMPL_VULKAN)
#include <vk_mem_alloc.h>
#endif

#include "scene/Scene.h"
#include "scene/World.h"

namespace hpl {

	//////////////////////////////////////////////////////////////////////////
	// CONSTRUCTORS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	cViewport::cViewport(cScene *apScene)
	{
		mpScene = apScene;

		mbActive = true;
		mbVisible = true;

		mpRenderSettings = std::make_unique<cRenderSettings>();

		mpWorld = NULL;
		mpCamera = NULL;
		mpRenderer = NULL;
		mpPostEffectComposite = NULL;

		mbIsListener = false;
	}

	//-----------------------------------------------------------------------

	cViewport::~cViewport()
	{
		// GPU teardown of the viewport-owned targets: everything goes to the
		// frame freelist — freed once the in-flight pipeline is done with it
		// (or in RIBootstrap::Dispose at shutdown). No stall.
		RIBootstrap::FrameContext *cntx = RI.GetActiveSet();
		std::visit(
			[cntx](auto &&arg) {
				using T = std::decay_t<decltype(arg)>;
				if constexpr (!std::is_same_v<T, std::monostate>) {
					arg.Dispose(cntx);
				}
			},
			m_state);
		ReleasePogoBuffer(cntx);
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// TARGET / STATE VISITORS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	cVector2l cViewport::GetTargetSize() const
	{
		return std::visit(
			[](auto &&arg) -> cVector2l {
				using T = std::decay_t<decltype(arg)>;
				if constexpr (std::is_same_v<T, TargetSwapchain>) {
					return cVector2l((int)RI.swapchain.width,
									 (int)RI.swapchain.height);
				} else {
					return cVector2l((int)arg.width, (int)arg.height);
				}
			},
			mTarget);
	}

	struct RITextureView_s* cViewport::GetDepthView()
	{
		return std::visit(
			[](auto &&arg) -> struct RITextureView_s * {
				using T = std::decay_t<decltype(arg)>;
				if constexpr (std::is_same_v<T, std::monostate>) {
					return nullptr;
				} else {
					return arg.width != 0 ? &arg.depthView[RI.swapchainIndex]
										  : nullptr;
				}
			},
			m_state);
	}

	cViewport::BackBuffer cViewport::GetBackBuffer()
	{
		return std::visit(
			[](auto &&arg) -> BackBuffer {
				using T = std::decay_t<decltype(arg)>;
				if constexpr (std::is_same_v<T, std::monostate>) {
					return BackBuffer{}; // zeroed — check renderTarget.vk.image
				} else {
					return arg.width != 0 ? arg.GetBackBuffer() : BackBuffer{};
				}
			},
			m_state);
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// VIEWPORT TEXTURE HELPERS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

bool CreateViewportColorTexture(struct RIDevice_s *device, uint32_t width,
								uint32_t height, enum RI_Format_e format,
								VkImageUsageFlags usage,
								struct RITexture_s *tex,
								struct RIDescriptor_s *desc, const char *what) {
	uint32_t queueFamilies[RI_QUEUE_LEN] = {0};

	VmaAllocationCreateInfo memReqs = {};
	memReqs.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

	VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT |
				 VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
	info.imageType = VK_IMAGE_TYPE_2D;
	info.extent = {width, height, 1};
	info.mipLevels = 1;
	info.arrayLayers = 1;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.pQueueFamilyIndices = queueFamilies;
	VK_ConfigureImageQueueFamilies(&info, device->queues, RI_QUEUE_LEN,
								   queueFamilies, RI_QUEUE_LEN);
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	info.usage = usage;
	info.format = RIFormatToVK(format);

	if (!VK_WrapResult(vmaCreateImage(device->vk.vmaAllocator, &info, &memReqs,
									  &tex->vk.image, &tex->vk.allocation,
									  NULL))) {
		Error("%s: failed to create %ux%u color image\n", what, width, height);
		return false;
	}

	VkImageViewUsageCreateInfo usageInfo = {
		VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
	usageInfo.usage =
		usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
				 VK_IMAGE_USAGE_STORAGE_BIT);

	VkImageViewCreateInfo createInfo = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	createInfo.pNext = &usageInfo;
	createInfo.subresourceRange =
		VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	createInfo.image = tex->vk.image;
	createInfo.format = RIFormatToVK(format);
	createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;

	desc->flags |= RI_VK_DESC_OWN_IMAGE_VIEW;
	desc->texture = tex;
	desc->vk.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	desc->vk.image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	if (!VK_WrapResult(vkCreateImageView(device->vk.device, &createInfo, NULL,
										 &desc->vk.image.imageView))) {
		Error("%s: failed to create image view\n", what);
		vmaDestroyImage(device->vk.vmaAllocator, tex->vk.image,
						tex->vk.allocation);
		*tex = RITexture_s{};
		*desc = RIDescriptor_s{};
		return false;
	}
	RIFinalizeDescriptor(device, desc);
	return true;
}

void ReleaseViewportColorTexture(std::vector<struct RIFree> &freelist,
								 struct RITexture_s *tex,
								 struct RIDescriptor_s *desc) {
	if (desc->vk.image.imageView) {
		freelist.emplace_back(desc->vk.image.imageView);
	}
	if (tex->vk.image) {
		freelist.emplace_back(tex->vk.image);
		freelist.emplace_back(tex->vk.allocation);
	}
	*desc = RIDescriptor_s{};
	*tex = RITexture_s{};
}

// Depth / visibility creation — ported verbatim from the retired swapchain-init
// block in Graphics.cpp, parameterized by extent.
bool CreateViewportAttachmentTexture(struct RIDevice_s *device, uint32_t width,
									 uint32_t height, enum RI_Format_e format,
									 VkImageUsageFlags usage,
									 VkImageAspectFlags aspect,
									 struct RITexture_s *tex,
									 struct RITextureView_s *view,
									 const char *what) {
	uint32_t queueFamilies[RI_QUEUE_LEN] = {0};

	VmaAllocationCreateInfo memReqs = {};
	memReqs.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

	VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT |
				 VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
	info.imageType = VK_IMAGE_TYPE_2D;
	info.extent = {width, height, 1};
	info.mipLevels = 1;
	info.arrayLayers = 1;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.pQueueFamilyIndices = queueFamilies;
	VK_ConfigureImageQueueFamilies(&info, device->queues, RI_QUEUE_LEN,
								   queueFamilies, RI_QUEUE_LEN);
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	info.format = RIFormatToVK(format);
	info.usage = usage;
	if (!VK_WrapResult(vmaCreateImage(device->vk.vmaAllocator, &info, &memReqs,
									  &tex->vk.image, &tex->vk.allocation,
									  NULL))) {
		Error("%s: failed to create %ux%u image\n", what, width, height);
		return false;
	}

	VkImageViewCreateInfo createInfo = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	createInfo.format = RIFormatToVK(format);
	createInfo.subresourceRange = VkImageSubresourceRange{aspect, 0, 1, 0, 1};
	createInfo.image = tex->vk.image;
	createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	if (!VK_WrapResult(vkCreateImageView(device->vk.device, &createInfo, NULL,
										 &view->vk.image))) {
		Error("%s: failed to create image view\n", what);
		vmaDestroyImage(device->vk.vmaAllocator, tex->vk.image,
						tex->vk.allocation);
		*tex = RITexture_s{};
		return false;
	}
	return true;
}

void ReleaseViewportAttachmentTexture(std::vector<struct RIFree> &freelist,
									  struct RITexture_s *tex,
									  struct RITextureView_s *view) {
	if (view->vk.image) {
		freelist.emplace_back(view->vk.image);
	}
	if (tex->vk.image) {
		freelist.emplace_back(tex->vk.image);
		freelist.emplace_back(tex->vk.allocation);
	}
	*view = RITextureView_s{};
	*tex = RITexture_s{};
}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// POGO BUFFER
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	RI_PogoBuffer* cViewport::PreparePogoBuffer(RIBootstrap::FrameContext *cntx)
	{
		const cVector2l vSize = GetTargetSize();
		const uint32_t alWidth = (uint32_t)vSize.x;
		const uint32_t alHeight = (uint32_t)vSize.y;
		if(alWidth == 0 || alHeight == 0)
		{
			return PogoBuffer();
		}
		if(mlPogoWidth == alWidth && mlPogoHeight == alHeight)
		{
			return &mPogoBuffer;
		}

		// Resize: hand the old halves to the frame freelist (the in-flight
		// pipeline still reads them) and recreate at the new extent — no
		// stall.
		ReleasePogoBuffer(cntx);

		RI_PogoBufferInit(&RI.device, &mPogoBuffer, alWidth, alHeight,
		                  RIBootstrap::PogoColorFormat);
		mlPogoWidth = alWidth;
		mlPogoHeight = alHeight;
		return &mPogoBuffer;
	}

	void cViewport::ReleasePogoBuffer(RIBootstrap::FrameContext *cntx)
	{
		if(mlPogoWidth == 0) return;

		for(size_t p = 0; p < 2; p++)
		{
			if(mPogoBuffer.pogoAttachment[p].vk.image.imageView)
			{
				cntx->freelist.emplace_back(mPogoBuffer.pogoAttachment[p].vk.image.imageView);
			}
			if(mPogoBuffer.textures[p].vk.image)
			{
				cntx->freelist.emplace_back(mPogoBuffer.textures[p].vk.image);
				cntx->freelist.emplace_back(mPogoBuffer.textures[p].vk.allocation);
			}
			mPogoBuffer.pogoAttachment[p] = RIDescriptor_s{};
			mPogoBuffer.textures[p] = RITexture_s{};
		}
		mPogoBuffer.attachmentIndex = 0;
		mlPogoWidth = 0;
		mlPogoHeight = 0;
	}


	//////////////////////////////////////////////////////////////////////////
	// PUBLIC METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------
	
	void cViewport::SetWorld(cWorld *apWorld)
	{ 
		if(mpWorld != NULL && mpScene->WorldExists(mpWorld))
		{
			mpWorld->SetIsSoundEmitter(false);
		}

		mpWorld = apWorld;
		if(mpWorld) mpWorld->SetIsSoundEmitter(true);

		mpRenderSettings->ResetVariables();
	}
	

	//-----------------------------------------------------------------------

	void cViewport::AddGuiSet(cGuiSet *apSet)
	{
		m_guiSets.push_back(apSet);
	}
	void cViewport::RemoveGuiSet(cGuiSet *apSet)
	{
		STLFindAndRemove(m_guiSets, apSet);
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PRIVATE METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------
	

	//-----------------------------------------------------------------------

}
