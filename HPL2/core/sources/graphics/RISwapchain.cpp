#include "graphics/RISwapchain.h"
#include "graphics/RIRenderer.h"
#include "graphics/RITypes.h"
#include "system/Types.h"

#include "graphics/RIVK.h"

#if (DEVICE_IMPL_MTL)
#include "graphics/RIMTL.h"
#endif

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

int InitRISwapchain( struct RIDevice *dev, struct RISwapchainDesc *init, RISwapchain<> *swapchain )
{
	assert( init->windowHandle );
	assert( init );
	assert( swapchain );
	assert( init->requestImageCount > 0 );
	swapchain->width = init->width;
	swapchain->height = init->height;
	swapchain->presentQueue = init->queue;

#if ( DEVICE_IMPL_VULKAN )
	if (dev->renderer->is_target_selected(RI_DEVICE_API_VK)) {
	assert( init->requestImageCount <= ARRAY_COUNT( swapchain->vk.images ) );
	VkResult result = VK_SUCCESS;
	{
		switch( init->windowHandle->type ) {
#ifdef VK_USE_PLATFORM_XLIB_KHR
			case RI_WINDOW_X11: {
				VkXlibSurfaceCreateInfoKHR xlibSurfaceInfo = { VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR };
				xlibSurfaceInfo.dpy = (Display*)init->windowHandle->x11.dpy;
				xlibSurfaceInfo.window = (Window)init->windowHandle->x11.window;
				result = vkCreateXlibSurfaceKHR( dev->renderer->vk.instance, &xlibSurfaceInfo, NULL, &swapchain->vk.surface );
				VK_WrapResult( result );
				break;
			}
#endif
#ifdef VK_USE_PLATFORM_WIN32_KHR
			case RI_WINDOW_WIN32: {
				VkWin32SurfaceCreateInfoKHR win32SurfaceInfo = { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
				win32SurfaceInfo.hwnd = (HWND)init->windowHandle->windows.hwnd;

				result = vkCreateWin32SurfaceKHR( dev->renderer->vk.instance, &win32SurfaceInfo, NULL, &swapchain->vk.surface );
				VK_WrapResult( result );
				break;
			}
#endif
#ifdef VK_USE_PLATFORM_METAL_EXT
			case RI_WINDOW_METAL: {
				VkMetalSurfaceCreateInfoEXT metalSurfaceCreateInfo = { VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT };
				metalSurfaceCreateInfo.pLayer = (const CAMetalLayer *)init->windowHandle->metal.caMetalLayer;
				result = vkCreateMetalSurfaceEXT( dev->renderer->vk.instance, &metalSurfaceCreateInfo, NULL, &swapchain->vk.surface );
				VK_WrapResult( result );
				break;
			}
#endif
#ifdef VK_USE_PLATFORM_WAYLAND_KHR
			case RI_WINDOW_WAYLAND: {
				VkWaylandSurfaceCreateInfoKHR waylandSurfaceInfo = { VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR };
				waylandSurfaceInfo.display = (wl_display*)init->windowHandle->wayland.display;
				waylandSurfaceInfo.surface = (wl_surface*)init->windowHandle->wayland.surface;
				result = vkCreateWaylandSurfaceKHR( dev->renderer->vk.instance, &waylandSurfaceInfo, NULL, &swapchain->vk.surface );
				VK_WrapResult( result );
				break;
			}
#endif
			default:
				break;
		}
	}
	VkSurfaceCapabilitiesKHR surfaceCaps = {0};
	{
		VkBool32 supported = VK_FALSE;
		result = vkGetPhysicalDeviceSurfaceSupportKHR(dev->physicalAdapter.vk.physicalDevice, init->queue->vk.queueFamilyIdx, swapchain->vk.surface, &supported);
		VK_WrapResult(result);

		result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(dev->physicalAdapter.vk.physicalDevice, swapchain->vk.surface, &surfaceCaps);
		VK_WrapResult(result);
	}
	
	uint32_t numSurfaceFormats = 0;
	result = vkGetPhysicalDeviceSurfaceFormatsKHR( dev->physicalAdapter.vk.physicalDevice, swapchain->vk.surface, &numSurfaceFormats, NULL );
	VK_WrapResult(result);
	VkSurfaceFormatKHR *surfaceFormats = (VkSurfaceFormatKHR*)malloc( sizeof( VkSurfaceFormatKHR ) * numSurfaceFormats );
	result = vkGetPhysicalDeviceSurfaceFormatsKHR( dev->physicalAdapter.vk.physicalDevice, swapchain->vk.surface, &numSurfaceFormats, surfaceFormats );
	VK_WrapResult(result);
	VkSurfaceFormatKHR *selectedSurf = surfaceFormats ;
	{
		uint32_t ( *priorityHandler )( const VkSurfaceFormatKHR *surface ) = __priority_BT709_G22_8BIT;
		switch( init->format ) {
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
  result = vkGetPhysicalDeviceSurfacePresentModesKHR(dev->physicalAdapter.vk.physicalDevice, swapchain->vk.surface, &presentModeCount, NULL);
	VK_WrapResult(result);
	VkPresentModeKHR* supportedPresentMode = (VkPresentModeKHR*)malloc(presentModeCount * sizeof(VkPresentModeKHR));
  result = vkGetPhysicalDeviceSurfacePresentModesKHR(dev->physicalAdapter.vk.physicalDevice, swapchain->vk.surface, &presentModeCount, supportedPresentMode);
	VK_WrapResult(result);

  // The VK_PRESENT_MODE_FIFO_KHR mode must always be present as per spec
  // This mode waits for the vertical blank ("v-sync")
  VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;

  VkPresentModeKHR preferredModeList[] = {
      VK_PRESENT_MODE_IMMEDIATE_KHR,
      VK_PRESENT_MODE_FIFO_RELAXED_KHR,
      VK_PRESENT_MODE_FIFO_KHR
  };
  for( size_t j = 0; j < ARRAY_COUNT( preferredModeList ); j++ ) {
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
		swapChainCreateInfo.surface = swapchain->vk.surface;
		// clamp the requested image count to the surface capabilities. maxImageCount == 0 means "no upper limit" per Vulkan spec.
		uint32_t desiredImageCount = init->requestImageCount;
		if( surfaceCaps.minImageCount > 0 && desiredImageCount < surfaceCaps.minImageCount )
			desiredImageCount = surfaceCaps.minImageCount;
		if( surfaceCaps.maxImageCount > 0 && desiredImageCount > surfaceCaps.maxImageCount )
			desiredImageCount = surfaceCaps.maxImageCount;
		swapChainCreateInfo.minImageCount = desiredImageCount;
		swapChainCreateInfo.imageFormat = selectedSurf->format;
		swapChainCreateInfo.imageColorSpace = selectedSurf->colorSpace;
		swapChainCreateInfo.imageExtent.width = init->width ;
		swapChainCreateInfo.imageExtent.height = init->height;
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
		swapChainCreateInfo.oldSwapchain = 0;
		result = vkCreateSwapchainKHR( dev->vk.device, &swapChainCreateInfo, NULL, &swapchain->vk.swapchain);
	}

	{
		uint32_t imageNum = 0;
		vkGetSwapchainImagesKHR(dev->vk.device, swapchain->vk.swapchain, &imageNum, NULL);
		assert(imageNum <= swapchain->MAX_IMAGE_COUNT);
		vkGetSwapchainImagesKHR(dev->vk.device, swapchain->vk.swapchain, &imageNum, swapchain->vk.images);
		for(size_t i = 0; i < imageNum; i++) {
			swapchain->textures[i].vk.image = swapchain->vk.images[i];
		}
		swapchain->imageCount = imageNum;
		swapchain->format = VKToRIFormat(selectedSurf->format);

		for(size_t i = 0; i < RI_MAX_SWAPCHAIN_IMAGES; i++) {
			VkSemaphoreCreateInfo createInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
      VkSemaphoreTypeCreateInfo timelineCreateInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
      timelineCreateInfo.semaphoreType = VK_SEMAPHORE_TYPE_BINARY;
			R_VK_ADD_STRUCT(&createInfo, &timelineCreateInfo);

			result = vkCreateSemaphore(dev->vk.device, &createInfo, NULL, &swapchain->vk.imageAcquireSem[i]);
			VK_WrapResult(result);
			
			result = vkCreateSemaphore(dev->vk.device, &createInfo, NULL, &swapchain->vk.finishSem[i]);
			VK_WrapResult(result);
		}
	}
	free(supportedPresentMode);
	free(surfaceFormats);
	return RI_SUCCESS;
	}
#endif
#if ( DEVICE_IMPL_MTL )
	if (dev->renderer->is_target_selected(RI_DEVICE_API_MTL)) {
  swapchain->mtl.layer =
      (CA::MetalLayer *)init->windowHandle->metal.caMetalLayer;
  if (!swapchain->mtl.layer) {
    swapchain->imageCount = 0;
    return RI_FAIL;
  }
  swapchain->mtl.layer->setDevice(dev->mtl.device);
  uint32_t riFormat = RISwapchainFormatToRIFormat(init->format);
  swapchain->mtl.layer->setPixelFormat(RIToMTLFormat(riFormat));
  CGSize drawableSize;
  drawableSize.width = init->width;
  drawableSize.height = init->height;
  swapchain->mtl.layer->setDrawableSize(drawableSize);
  swapchain->format = riFormat;
  // CAMetalLayer manages its own (2-3) drawables internally; expose the
  // requested count so the engine's frame indexing stays valid.
  swapchain->imageCount = init->requestImageCount;
  swapchain->mtl.textureIndex = 0;
  return RI_SUCCESS;
	}
#endif
	swapchain->imageCount = 0;
	return RI_FAIL;
}

uint32_t RISwapchainAcquireNextTexture( struct RIDevice *dev, RISwapchain<> *swapchain )
{
	assert( swapchain->imageCount > 0 );
#if ( DEVICE_IMPL_VULKAN )
	{
		VkSemaphore imageAcquiredSemaphore = swapchain->vk.imageAcquireSem[swapchain->vk.frameIndex];
		VK_WrapResult( vkAcquireNextImageKHR( dev->vk.device, swapchain->vk.swapchain, 5000 * 1000000ull, imageAcquiredSemaphore, VK_NULL_HANDLE, &swapchain->vk.textureIndex ) );
		return swapchain->vk.textureIndex;
	}
#endif
#if ( DEVICE_IMPL_MTL )
	{
		// Metal vends a fresh drawable per frame; cache it and rebind its
		// backing texture into the single swapchain texture slot.
		if (swapchain->mtl.drawable)
			swapchain->mtl.drawable->release();
		auto *drawable = swapchain->mtl.layer->nextDrawable();
		if (!drawable) {
			// nextDrawable() returns nil under window minimize / drawable-pool
			// exhaustion / resource pressure. Don't deref it: drop the slot (the
			// previous drawable was already released last present) so callers see a
			// null target instead of dereferencing nil or a dangling texture.
			// TODO: a real frame-skip path in BeginActiveSet would be cleaner than
			// rendering into a null drawable for the dropped frame.
			hpl::Warning("Metal swapchain: nextDrawable() returned nil; skipping frame drawable\n");
			swapchain->mtl.drawable = nullptr;
			swapchain->textures[0].mtl.texture = nullptr;
			swapchain->mtl.textureIndex = 0;
			return 0;
		}
		swapchain->mtl.drawable = drawable->retain();
		swapchain->textures[0].mtl.texture = swapchain->mtl.drawable->texture();
		swapchain->mtl.textureIndex = 0;
		return 0;
	}
#endif
	return 0;
}

void RISwapchainPresent(struct RIDevice* dev, RISwapchain<>* swapchain) {
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
			VK_WrapResult( vkQueuePresentKHR( swapchain->presentQueue->vk.queue, &presentInfo ) );
		}
		swapchain->vk.presentID++;
		swapchain->vk.frameIndex = ( swapchain->vk.frameIndex + 1 ) % RI_MAX_SWAPCHAIN_IMAGES;
	}
#endif
#if ( DEVICE_IMPL_MTL )
	{
		// Present rides its own command buffer; never reuse a committed RICmd.
		MTL::CommandBuffer *cb = swapchain->presentQueue->mtl.queue->commandBuffer();
		if (swapchain->mtl.drawable)
			cb->presentDrawable(swapchain->mtl.drawable);
		cb->commit();
		if (swapchain->mtl.drawable) {
			swapchain->mtl.drawable->release();
			swapchain->mtl.drawable = nullptr;
		}
	}
#endif
}

