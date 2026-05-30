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
    return ((surface->format == VK_FORMAT_R16G16B16A16_SFLOAT) << 0) | 
           ((surface->colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT) << 1);
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

static const char* VkFormatName(VkFormat fmt)
{
	switch (fmt) {
	case VK_FORMAT_UNDEFINED: return "VK_FORMAT_UNDEFINED";

	case VK_FORMAT_B8G8R8A8_UNORM: return "VK_FORMAT_B8G8R8A8_UNORM";
	case VK_FORMAT_R8G8B8A8_UNORM: return "VK_FORMAT_R8G8B8A8_UNORM";
	case VK_FORMAT_B8G8R8A8_SRGB: return "VK_FORMAT_B8G8R8A8_SRGB";
	case VK_FORMAT_R8G8B8A8_SRGB: return "VK_FORMAT_R8G8B8A8_SRGB";

	case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "VK_FORMAT_A2B10G10R10_UNORM_PACK32";
	case VK_FORMAT_A2R10G10B10_UNORM_PACK32: return "VK_FORMAT_A2R10G10B10_UNORM_PACK32";

	case VK_FORMAT_R16G16B16A16_UNORM: return "VK_FORMAT_R16G16B16A16_UNORM";
	case VK_FORMAT_R16G16B16A16_SFLOAT: return "VK_FORMAT_R16G16B16A16_SFLOAT";

	default: return "UNKNOWN_VK_FORMAT";
	}
}

static const char* VkColorSpaceName(VkColorSpaceKHR cs)
{
	switch (cs) {
	case VK_COLOR_SPACE_SRGB_NONLINEAR_KHR:
		return "VK_COLOR_SPACE_SRGB_NONLINEAR_KHR";

#ifdef VK_COLOR_SPACE_DISPLAY_P3_NONLINEAR_EXT
	case VK_COLOR_SPACE_DISPLAY_P3_NONLINEAR_EXT:
		return "VK_COLOR_SPACE_DISPLAY_P3_NONLINEAR_EXT";
#endif

#ifdef VK_COLOR_SPACE_DISPLAY_P3_LINEAR_EXT
	case VK_COLOR_SPACE_DISPLAY_P3_LINEAR_EXT:
		return "VK_COLOR_SPACE_DISPLAY_P3_LINEAR_EXT";
#endif

#ifdef VK_COLOR_SPACE_DCI_P3_NONLINEAR_EXT
	case VK_COLOR_SPACE_DCI_P3_NONLINEAR_EXT:
		return "VK_COLOR_SPACE_DCI_P3_NONLINEAR_EXT";
#endif

#ifdef VK_COLOR_SPACE_BT709_LINEAR_EXT
	case VK_COLOR_SPACE_BT709_LINEAR_EXT:
		return "VK_COLOR_SPACE_BT709_LINEAR_EXT";
#endif

#ifdef VK_COLOR_SPACE_BT709_NONLINEAR_EXT
	case VK_COLOR_SPACE_BT709_NONLINEAR_EXT:
		return "VK_COLOR_SPACE_BT709_NONLINEAR_EXT";
#endif

#ifdef VK_COLOR_SPACE_BT2020_LINEAR_EXT
	case VK_COLOR_SPACE_BT2020_LINEAR_EXT:
		return "VK_COLOR_SPACE_BT2020_LINEAR_EXT";
#endif

#ifdef VK_COLOR_SPACE_HDR10_ST2084_EXT
	case VK_COLOR_SPACE_HDR10_ST2084_EXT:
		return "VK_COLOR_SPACE_HDR10_ST2084_EXT";
#endif

#ifdef VK_COLOR_SPACE_HDR10_HLG_EXT
	case VK_COLOR_SPACE_HDR10_HLG_EXT:
		return "VK_COLOR_SPACE_HDR10_HLG_EXT";
#endif

	default:
		return "UNKNOWN_VK_COLOR_SPACE";
	}
}

int InitRISwapchain( struct RIDevice_s *dev, struct RISwapchainDesc_s *init, RISwapchain_s<> *swapchain )
{
	assert( init->windowHandle );
	assert( init );
	assert( swapchain );
	assert( init->requestImageCount <= ARRAY_COUNT( swapchain->vk.images ) && init->requestImageCount > 0 );
	swapchain->width = init->width;
	swapchain->height = init->height;
	swapchain->presentQueue = init->queue;
	VkResult result = VK_SUCCESS;
#if ( DEVICE_IMPL_VULKAN )
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
				metalSurfaceCreateInfo.pLayer = (CAMetalLayer *)swapChainDesc.window.metal.caMetalLayer;

				VkResult result = vk.CreateMetalSurfaceEXT( m_Device, &metalSurfaceCreateInfo, m_Device.GetAllocationCallbacks(), &m_Surface );
				RETURN_ON_FAILURE( &m_Device, result == VK_SUCCESS, GetReturnCode( result ), "vkCreateMetalSurfaceEXT returned %d", (int32_t)result );
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
#endif
	VkSurfaceCapabilitiesKHR surfaceCaps = {0};
	{
		VkBool32 supported = VK_FALSE;
		result = vkGetPhysicalDeviceSurfaceSupportKHR(dev->physicalAdapter.vk.physicalDevice, init->queue->vk.queueFamilyIdx, swapchain->vk.surface, &supported);
		VK_WrapResult(result);

		result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(dev->physicalAdapter.vk.physicalDevice, swapchain->vk.surface, &surfaceCaps);
		VK_WrapResult(result);
	}
	
	uint32_t numSurfaceFormats = 0;

	result = vkGetPhysicalDeviceSurfaceFormatsKHR(
		dev->physicalAdapter.vk.physicalDevice,
		swapchain->vk.surface,
		&numSurfaceFormats,
		NULL
	);
	VK_WrapResult(result);

	hpl::Log("Swapchain: physical device reports %u surface formats", (unsigned)numSurfaceFormats);

	VkSurfaceFormatKHR* surfaceFormats =
		(VkSurfaceFormatKHR*)malloc(sizeof(VkSurfaceFormatKHR) * numSurfaceFormats);

	result = vkGetPhysicalDeviceSurfaceFormatsKHR(
		dev->physicalAdapter.vk.physicalDevice,
		swapchain->vk.surface,
		&numSurfaceFormats,
		surfaceFormats
	);
	VK_WrapResult(result);

	VkSurfaceFormatKHR* selectedSurf = surfaceFormats;

	{
		uint32_t(*priorityHandler)(const VkSurfaceFormatKHR * surface) =
			__priority_BT709_G22_8BIT;

		switch (init->format) {
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

		hpl::Log("Swapchain: available surface formats:");

		for (uint32_t i = 0; i < numSurfaceFormats; i++) {
			uint32_t priority = priorityHandler(surfaceFormats + i);

			hpl::Log(
				"  [%u] format=%s (%d), colorSpace=%s (%d), priority=%u%s",
				(unsigned)i,
				VkFormatName(surfaceFormats[i].format),
				(int)surfaceFormats[i].format,
				VkColorSpaceName(surfaceFormats[i].colorSpace),
				(int)surfaceFormats[i].colorSpace,
				(unsigned)priority,
				(surfaceFormats + i == selectedSurf) ? " initial" : ""
			);

			if (priorityHandler(surfaceFormats + i) > priorityHandler(selectedSurf)) {
				hpl::Log(
					"    -> New best: [%u] priority %u > %u",
					(unsigned)i,
					(unsigned)priority,
					(unsigned)priorityHandler(selectedSurf)
				);

				selectedSurf = surfaceFormats + i;
			}
		}

		hpl::Log(
			"Swapchain: selected surface format: format=%s (%d), colorSpace=%s (%d), priority=%u",
			VkFormatName(selectedSurf->format),
			(int)selectedSurf->format,
			VkColorSpaceName(selectedSurf->colorSpace),
			(int)selectedSurf->colorSpace,
			(unsigned)priorityHandler(selectedSurf)
		);
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

uint32_t RISwapchainAcquireNextTexture( struct RIDevice_s *dev, RISwapchain_s<> *swapchain )
{
	assert( swapchain->imageCount > 0 );
#if ( DEVICE_IMPL_VULKAN )
	{
		VkSemaphore imageAcquiredSemaphore = swapchain->vk.imageAcquireSem[swapchain->vk.frameIndex];
		VK_WrapResult( vkAcquireNextImageKHR( dev->vk.device, swapchain->vk.swapchain, 5000 * 1000000ull, imageAcquiredSemaphore, VK_NULL_HANDLE, &swapchain->vk.textureIndex ) );
		return swapchain->vk.textureIndex;
	}
#endif
	return 0;
}

void RISwapchainPresent(struct RIDevice_s* dev, RISwapchain_s<>* swapchain) {
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
}

void FreeRISwapchain( struct RIDevice_s *dev, RISwapchain_s<> *swapchain )
{
#if ( DEVICE_IMPL_VULKAN )
	{
		for( size_t p = 0; p < RI_MAX_SWAPCHAIN_IMAGES; p++ ) {
			if( swapchain->vk.imageAcquireSem[p] )
				vkDestroySemaphore( dev->vk.device, swapchain->vk.imageAcquireSem[p], NULL );
			if( swapchain->vk.finishSem[p] )
				vkDestroySemaphore( dev->vk.device, swapchain->vk.finishSem[p], NULL );
		}
		if( swapchain->vk.swapchain )
			vkDestroySwapchainKHR( dev->vk.device, swapchain->vk.swapchain, NULL );
		if( swapchain->vk.surface )
			vkDestroySurfaceKHR( dev->renderer->vk.instance, swapchain->vk.surface, NULL );
	}
	memset( swapchain, 0, sizeof( RISwapchain_s<> ) );
#endif
}

