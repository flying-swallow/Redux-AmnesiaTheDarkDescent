#ifndef RI_RENDERER_H
#define RI_RENDERER_H

#include "graphics/RITypes.h"
#include <cassert>
#include <stdint.h>

struct RIDeviceDesc_s {
	struct RIPhysicalAdapter_s *physicalAdapter;
};

// Loader-level teardown: disposes the renderer, then finalizes volk.
void ShutdownRIRenderer( struct RIRenderer_s *renderer );

#if DEVICE_IMPL_VULKAN
void VK_ConfigureBufferQueueFamilies( VkBufferCreateInfo *info, struct RIQueue_s *queues, size_t numQueues, uint32_t *queueFamiliesIdx, size_t reservedLen );
void VK_ConfigureImageQueueFamilies( VkImageCreateInfo *info, struct RIQueue_s *queues, size_t numQueues, uint32_t *queueFamiliesIdx, size_t reservedLen );
void VK_FillQueueFamilies( struct RIDevice_s *dev, uint32_t *queueFamilies, uint32_t *queueFamiliesIdx, size_t reservedLen );
#endif

#endif

