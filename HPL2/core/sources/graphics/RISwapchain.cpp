#include "graphics/RISwapchain.h"
#include "graphics/RIRenderer.h"
#include "graphics/RITypes.h"
#include "system/Types.h"

#include "graphics/RIVK.h"

#include <cassert>
#include <cstdlib>
#include <cstring>


#if ( DEVICE_IMPL_VULKAN )

static uint32_t __priority_BT709_G22_16BIT(const VkSurfaceFormatKHR* surface)  {
		const struct RIFormatProps* props = GetRIFormatProps(VKToRIFormat(surface->format));
    return ((surface->format == VK_FORMAT_R16G16B16A16_SFLOAT) << 0) | 
           (props->isSrgb << 1);
};

static uint32_t __priority_BT709_G22_8BIT(const VkSurfaceFormatKHR* surface) {

    // https://registry.khronos.org/vulkan/specs/1.3-extensions/man/html/vkGetPhysicalDeviceSurfaceFormatsKHR.html
    // There is always a corresponding UNORM, SRGB just need to consider UNORM
    return ((surface->format == VK_FORMAT_R8G8B8A8_UNORM || surface->format == VK_FORMAT_B8G8R8A8_UNORM) << 0) | 
  				 ((surface->colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) << 1);
}

static uint32_t __priority_BT709_G22_10BIT(const VkSurfaceFormatKHR* surface){
    return ((surface->format == VK_FORMAT_A2B10G10R10_UNORM_PACK32) << 0) | 
           ((surface->colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) << 1);
}

static uint32_t __priority_BT2020_G2084_10BIT( const VkSurfaceFormatKHR *surface )
{
	return ( ( surface->format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ) << 0 ) | ( ( surface->colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT ) << 1 );
}

#endif

VkSurfaceKHR RICreateWindowSurface( const struct RIWindowHandle *handle )
{
#if ( DEVICE_IMPL_VULKAN )
	assert( handle );
	VkSurfaceKHR surface = VK_NULL_HANDLE;
	VkResult result = VK_SUCCESS;
	switch( handle->type ) {
#ifdef VK_USE_PLATFORM_XLIB_KHR
		case RI_WINDOW_X11: {
			VkXlibSurfaceCreateInfoKHR xlibSurfaceInfo = { VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR };
			xlibSurfaceInfo.dpy = (Display*)handle->x11.dpy;
			xlibSurfaceInfo.window = (Window)handle->x11.window;
			result = vkCreateXlibSurfaceKHR( RIGetVkInstance(), &xlibSurfaceInfo, NULL, &surface );
			if( result != VK_SUCCESS ) { VK_WrapResult( result ); return VK_NULL_HANDLE; }
			break;
		}
#endif
#ifdef VK_USE_PLATFORM_WIN32_KHR
		case RI_WINDOW_WIN32: {
			VkWin32SurfaceCreateInfoKHR win32SurfaceInfo = { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
			win32SurfaceInfo.hwnd = (HWND)handle->windows.hwnd;
			result = vkCreateWin32SurfaceKHR( RIGetVkInstance(), &win32SurfaceInfo, NULL, &surface );
			if( result != VK_SUCCESS ) { VK_WrapResult( result ); return VK_NULL_HANDLE; }
			break;
		}
#endif
#ifdef VK_USE_PLATFORM_METAL_EXT
		case RI_WINDOW_METAL: {
			VkMetalSurfaceCreateInfoEXT metalSurfaceCreateInfo = { VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT };
			metalSurfaceCreateInfo.pLayer = (const CAMetalLayer *)handle->metal.caMetalLayer;
			result = vkCreateMetalSurfaceEXT( RIGetVkInstance(), &metalSurfaceCreateInfo, NULL, &surface );
			if( result != VK_SUCCESS ) { VK_WrapResult( result ); return VK_NULL_HANDLE; }
			break;
		}
#endif
#ifdef VK_USE_PLATFORM_WAYLAND_KHR
		case RI_WINDOW_WAYLAND: {
			VkWaylandSurfaceCreateInfoKHR waylandSurfaceInfo = { VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR };
			waylandSurfaceInfo.display = (wl_display*)handle->wayland.display;
			waylandSurfaceInfo.surface = (wl_surface*)handle->wayland.surface;
			result = vkCreateWaylandSurfaceKHR( RIGetVkInstance(), &waylandSurfaceInfo, NULL, &surface );
			if( result != VK_SUCCESS ) { VK_WrapResult( result ); return VK_NULL_HANDLE; }
			break;
		}
#endif
		default:
			break;
	}
	return surface;
#else
	return VK_NULL_HANDLE;
#endif
}

RISwapchain RISwapchain::create( struct RIDevice *device, const struct RISwapchainDesc &desc )
{
	RISwapchain sc = {};
	assert( desc.requestImageCount <= ARRAY_COUNT( sc.vk.images ) && desc.requestImageCount > 0 );
	sc.width = desc.width;
	sc.height = desc.height;
	sc.presentQueue = desc.queue;
#if ( DEVICE_IMPL_VULKAN )
	VkResult result = VK_SUCCESS;
	// The swapchain owns its surface. desc.source selects where it comes from:
	//  - RIWindowHandle : first create — make a fresh surface here (adopted by
	//                     this swapchain on success; freed here on failure).
	//  - RISwapchain*   : recreate — reuse the old swapchain's surface (ownership
	//                     transferred below on success) and hand its
	//                     VkSwapchainKHR to oldSwapchain so the driver can reuse
	//                     resources.
	VkSurfaceKHR   surface           = VK_NULL_HANDLE;
	VkSwapchainKHR retiringSwapchain = VK_NULL_HANDLE;
	struct RISwapchain* oldSwapchain = nullptr;
	bool ownsFreshSurface = false;
	if (const RIWindowHandle* handle = std::get_if<RIWindowHandle>(&desc.source)) {
		surface = RICreateWindowSurface(handle);
		ownsFreshSurface = true;
	} else if (struct RISwapchain* const* pOld = std::get_if<struct RISwapchain*>(&desc.source)) {
		oldSwapchain = *pOld;
		if (oldSwapchain) {
			surface           = oldSwapchain->vk.surface;
			retiringSwapchain = oldSwapchain->vk.swapchain;
		}
	}
	// No usable surface (surface creation failed, or a null/handle-less source):
	// return an empty swapchain so the caller can retry.
	if (surface == VK_NULL_HANDLE) {
		return sc;
	}
	VkSurfaceCapabilitiesKHR surfaceCaps = {0};
	{
		VkBool32 supported = VK_FALSE;
		result = vkGetPhysicalDeviceSurfaceSupportKHR(device->physicalAdapter.vk.physicalDevice, desc.queue->vk.queueFamilyIdx, surface, &supported);
		VK_WrapResult(result);

		result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device->physicalAdapter.vk.physicalDevice, surface, &surfaceCaps);
		VK_WrapResult(result);
	}

	uint32_t numSurfaceFormats = 0;
	result = vkGetPhysicalDeviceSurfaceFormatsKHR( device->physicalAdapter.vk.physicalDevice, surface, &numSurfaceFormats, NULL );
	VK_WrapResult(result);
	VkSurfaceFormatKHR *surfaceFormats = (VkSurfaceFormatKHR*)malloc( sizeof( VkSurfaceFormatKHR ) * numSurfaceFormats );
	result = vkGetPhysicalDeviceSurfaceFormatsKHR( device->physicalAdapter.vk.physicalDevice, surface, &numSurfaceFormats, surfaceFormats );
	VK_WrapResult(result);
	VkSurfaceFormatKHR *selectedSurf = surfaceFormats ;
	{
		uint32_t ( *priorityHandler )( const VkSurfaceFormatKHR *surface ) = __priority_BT709_G22_8BIT;
		switch( desc.format ) {
			case RI_SWAPCHAIN_BT709_G10_16BIT:
				priorityHandler = __priority_BT709_G22_16BIT;
				break;
			case RI_SWAPCHAIN_BT709_G22_8BIT:
				priorityHandler = __priority_BT709_G22_8BIT;
				break;
			case RI_SWAPCHAIN_BT709_G22_10BIT:
				priorityHandler = __priority_BT709_G22_10BIT;
				break;
			case RI_SWAPCHAIN_BT2020_G2084_10BIT:
				priorityHandler = __priority_BT2020_G2084_10BIT;
				break;
		}
		assert(priorityHandler);

		uint32_t selectedPriority = priorityHandler(selectedSurf);

		for (size_t i = 1; i < numSurfaceFormats; i++) {
			uint32_t candidatePriority = priorityHandler(surfaceFormats + i);

			if (candidatePriority > selectedPriority) {
				selectedSurf = surfaceFormats + i;
				selectedPriority = candidatePriority;
			}
		}
	}

	uint32_t presentModeCount = 0;
  result = vkGetPhysicalDeviceSurfacePresentModesKHR(device->physicalAdapter.vk.physicalDevice, surface, &presentModeCount, NULL);
	VK_WrapResult(result);
	VkPresentModeKHR* supportedPresentMode = (VkPresentModeKHR*)malloc(presentModeCount * sizeof(VkPresentModeKHR));
  result = vkGetPhysicalDeviceSurfacePresentModesKHR(device->physicalAdapter.vk.physicalDevice, surface, &presentModeCount, supportedPresentMode);
	VK_WrapResult(result);

  // The VK_PRESENT_MODE_FIFO_KHR mode must always be present as per spec
  // This mode waits for the vertical blank ("v-sync")
  VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;

  // vsync on  -> FIFO: wait for the v-blank, no tearing, frame-capped.
  // vsync off -> prefer IMMEDIATE (may tear), then FIFO_RELAXED, falling back to FIFO.
  VkPresentModeKHR vsyncModeList[]   = { VK_PRESENT_MODE_FIFO_KHR };
  VkPresentModeKHR noVsyncModeList[] = {
      VK_PRESENT_MODE_IMMEDIATE_KHR,
      VK_PRESENT_MODE_FIFO_RELAXED_KHR,
      VK_PRESENT_MODE_FIFO_KHR
  };
  const VkPresentModeKHR* preferredModeList = desc.vsync ? vsyncModeList : noVsyncModeList;
  const size_t preferredModeCount = desc.vsync ? ARRAY_COUNT( vsyncModeList ) : ARRAY_COUNT( noVsyncModeList );
  for( size_t j = 0; j < preferredModeCount; j++ ) {
	  VkPresentModeKHR mode = preferredModeList[j];
	  uint32_t i = 0;
	  for(; i < presentModeCount; ++i ) {
	  	if(supportedPresentMode[i] == mode) {
	  		break;
	  	}
	  }
	  if( i < presentModeCount ) {
		  presentMode = mode;
		  break;
	  }
  }
	{
		VkSwapchainCreateInfoKHR swapChainCreateInfo = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
		swapChainCreateInfo.flags = 0;
		swapChainCreateInfo.surface = surface;
		// clamp the requested image count to the surface capabilities. maxImageCount == 0 means "no upper limit" per Vulkan spec.
		uint32_t desiredImageCount = desc.requestImageCount;
		if( surfaceCaps.minImageCount > 0 && desiredImageCount < surfaceCaps.minImageCount )
			desiredImageCount = surfaceCaps.minImageCount;
		if( surfaceCaps.maxImageCount > 0 && desiredImageCount > surfaceCaps.maxImageCount )
			desiredImageCount = surfaceCaps.maxImageCount;
		swapChainCreateInfo.minImageCount = desiredImageCount;
		swapChainCreateInfo.imageFormat = selectedSurf->format;
		swapChainCreateInfo.imageColorSpace = selectedSurf->colorSpace;
		// Clamp the requested extent to what the surface actually allows. When
		// currentExtent is defined (width != 0xFFFFFFFF), the compositor dictates
		// the size (common on Wayland) and the requested size must be ignored;
		// otherwise clamp to [minImageExtent, maxImageExtent]. This keeps
		// vkCreateSwapchainKHR well-formed even when the OS/compositor disagrees
		// with the window size we were handed.
		VkExtent2D imageExtent = { (uint32_t)desc.width, (uint32_t)desc.height };
		if( surfaceCaps.currentExtent.width != 0xFFFFFFFFu ) {
			imageExtent = surfaceCaps.currentExtent;
		} else {
			if( imageExtent.width < surfaceCaps.minImageExtent.width )
				imageExtent.width = surfaceCaps.minImageExtent.width;
			if( imageExtent.width > surfaceCaps.maxImageExtent.width )
				imageExtent.width = surfaceCaps.maxImageExtent.width;
			if( imageExtent.height < surfaceCaps.minImageExtent.height )
				imageExtent.height = surfaceCaps.minImageExtent.height;
			if( imageExtent.height > surfaceCaps.maxImageExtent.height )
				imageExtent.height = surfaceCaps.maxImageExtent.height;
		}
		swapChainCreateInfo.imageExtent = imageExtent;
		// Reflect the extent actually used (may differ from the requested size
		// after the surface-caps clamp) so viewport sizing / the "nothing changed"
		// resize check read the real backbuffer dimensions.
		sc.width = (uint16_t)imageExtent.width;
		sc.height = (uint16_t)imageExtent.height;
		swapChainCreateInfo.imageArrayLayers = 1;
		// SurfelGI composite now writes into the pogo buffer (a separate
		// color attachment) instead of the swapchain image, so STORAGE_BIT
		// is no longer required on the swapchain — and many sRGB swapchain
		// formats (e.g. VK_FORMAT_B8G8R8A8_SRGB with OPTIMAL tiling) don't
		// advertise STORAGE in their format-feature flags, which the
		// validation layer flags via VUID-VkSwapchainCreateInfoKHR-imageFormat-01778.
		swapChainCreateInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
		                                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		swapChainCreateInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
		swapChainCreateInfo.queueFamilyIndexCount = 0;
		swapChainCreateInfo.pQueueFamilyIndices = NULL;
		swapChainCreateInfo.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
		swapChainCreateInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
		swapChainCreateInfo.presentMode = presentMode;
		swapChainCreateInfo.clipped = VK_TRUE;
		swapChainCreateInfo.oldSwapchain = retiringSwapchain;
		result = vkCreateSwapchainKHR( device->vk.device, &swapChainCreateInfo, NULL, &sc.vk.swapchain);
		if( result != VK_SUCCESS ) {
			VK_WrapResult( result );
			// Only a first-create surface is ours to free; a recreate surface
			// still belongs to oldSwapchain (ownership isn't transferred on failure).
			if( ownsFreshSurface )
				vkDestroySurfaceKHR( RIGetVkInstance(), surface, NULL );
			free( supportedPresentMode );
			free( surfaceFormats );
			return sc;
		}
	}

	{
		uint32_t imageNum = 0;
		vkGetSwapchainImagesKHR(device->vk.device, sc.vk.swapchain, &imageNum, NULL);
		assert(imageNum <= sc.MAX_IMAGE_COUNT);
		vkGetSwapchainImagesKHR(device->vk.device, sc.vk.swapchain, &imageNum, sc.vk.images);
		for(size_t i = 0; i < imageNum; i++) {
			sc.textures[i].vk.image = sc.vk.images[i];
		}
		sc.imageCount = imageNum;
		sc.format = VKToRIFormat(selectedSurf->format);
		// Fresh swapchain, fresh frame bookkeeping.
		sc.vk.frameIndex = 0;
		sc.vk.textureIndex = 0;
		sc.vk.presentID = 0;

		for(size_t i = 0; i < RI_MAX_SWAPCHAIN_IMAGES; i++) {
			VkSemaphoreCreateInfo createInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
      VkSemaphoreTypeCreateInfo timelineCreateInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
      timelineCreateInfo.semaphoreType = VK_SEMAPHORE_TYPE_BINARY;
			R_VK_ADD_STRUCT(&createInfo, &timelineCreateInfo);

			result = vkCreateSemaphore(device->vk.device, &createInfo, NULL, &sc.vk.imageAcquireSem[i]);
			VK_WrapResult(result);

			result = vkCreateSemaphore(device->vk.device, &createInfo, NULL, &sc.vk.finishSem[i]);
			VK_WrapResult(result);
		}
	}

	// Per-image color-attachment views owned by the swapchain.
	for( uint32_t i = 0; i < sc.imageCount; i++ ) {
		VkImageViewUsageCreateInfo usageInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO };
		VkImageViewCreateInfo createInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		createInfo.pNext = &usageInfo;
		createInfo.subresourceRange = VkImageSubresourceRange{
			VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1,
		};
		// The SurfelGI composite + post-effect chain write into the viewport
		// backbuffer, so the swapchain view only needs COLOR_ATTACHMENT.
		usageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		createInfo.image = sc.vk.images[i];
		createInfo.format = RIFormatToVK(sc.format);
		createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		VK_WrapResult(vkCreateImageView(device->vk.device, &createInfo, NULL, &sc.views[i].vk.image));
	}

	// Success: the new swapchain adopts the surface. On a recreate transfer
	// ownership away from the retiring swapchain so exactly one owner remains
	// (its dispose() then frees only the VkSwapchainKHR, not the shared surface).
	// This runs only here — the failure return above leaves the surface with the
	// old swapchain so the caller can retry.
	sc.vk.surface = surface;
	if (oldSwapchain)
		oldSwapchain->vk.surface = VK_NULL_HANDLE;

	free(supportedPresentMode);
	free(surfaceFormats);
#endif
	return sc;
}

RISwapchainStatus_e RISwapchainAcquireNextTexture( struct RIDevice *dev, RISwapchain *swapchain, uint32_t *outTextureIndex )
{
	assert( swapchain->imageCount > 0 );
#if ( DEVICE_IMPL_VULKAN )
	{
		VkSemaphore imageAcquiredSemaphore = swapchain->vk.imageAcquireSem[swapchain->vk.frameIndex];
		VkResult result = vkAcquireNextImageKHR( dev->vk.device, swapchain->vk.swapchain, 5000 * 1000000ull, imageAcquiredSemaphore, VK_NULL_HANDLE, &swapchain->vk.textureIndex );
		// The surface changed (usually a resize): the caller must recreate the
		// swapchain before it can acquire again. Don't touch outTextureIndex.
		if( result == VK_ERROR_OUT_OF_DATE_KHR )
			return RI_SWAPCHAIN_STATUS_OUT_OF_DATE;
		// SUBOPTIMAL still produced a usable image this frame; anything else that
		// isn't success is genuinely unexpected, so log it.
		if( result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR )
			VK_WrapResult( result );
		if( outTextureIndex )
			*outTextureIndex = swapchain->vk.textureIndex;
		return result == VK_SUBOPTIMAL_KHR ? RI_SWAPCHAIN_STATUS_SUBOPTIMAL : RI_SWAPCHAIN_STATUS_OK;
	}
#endif
	if( outTextureIndex )
		*outTextureIndex = 0;
	return RI_SWAPCHAIN_STATUS_OK;
}

RISwapchainStatus_e RISwapchainPresent(struct RIDevice* dev, RISwapchain* swapchain) {
	RISwapchainStatus_e status = RI_SWAPCHAIN_STATUS_OK;
#if ( DEVICE_IMPL_VULKAN )
	{
		VkSemaphore renderingFinishedSemaphore = swapchain->vk.finishSem[swapchain->vk.frameIndex];
		{
			VkPresentInfoKHR presentInfo = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
			presentInfo.waitSemaphoreCount = 1;
			presentInfo.pWaitSemaphores = &renderingFinishedSemaphore;
			presentInfo.swapchainCount = 1;
			presentInfo.pSwapchains = &swapchain->vk.swapchain;
			presentInfo.pImageIndices = &swapchain->vk.textureIndex;

			VkPresentIdKHR presentId = { VK_STRUCTURE_TYPE_PRESENT_ID_KHR };
			presentId.swapchainCount = 1;
			presentId.pPresentIds = &swapchain->vk.presentID;

			if( dev->physicalAdapter.vk.isPresentIDSupported )
				presentInfo.pNext = &presentId;
			VkResult result = vkQueuePresentKHR( swapchain->presentQueue->vk.queue, &presentInfo );
			if( result == VK_ERROR_OUT_OF_DATE_KHR )
				status = RI_SWAPCHAIN_STATUS_OUT_OF_DATE;
			else if( result == VK_SUBOPTIMAL_KHR )
				status = RI_SWAPCHAIN_STATUS_SUBOPTIMAL;
			else if( result != VK_SUCCESS )
				VK_WrapResult( result );
		}
		swapchain->vk.presentID++;
		swapchain->vk.frameIndex = ( swapchain->vk.frameIndex + 1 ) % RI_MAX_SWAPCHAIN_IMAGES;
	}
#endif
	return status;
}

