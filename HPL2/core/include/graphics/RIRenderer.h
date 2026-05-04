#ifndef RI_RENDERER_H
#define RI_RENDERER_H

#include "graphics/RITypes.h"
#include <stdint.h>

static inline uint32_t RIGetQueueFlags(struct RIRenderer_s* renderer,const struct RIQueue_s* queue) {
#if ( DEVICE_IMPL_VULKAN )
  return (queue->vk.queueFlags & VK_QUEUE_GRAPHICS_BIT ? RI_QUEUE_GRAPHICS_BIT : 0) |
    (queue->vk.queueFlags  & VK_QUEUE_COMPUTE_BIT ? RI_QUEUE_COMPUTE_BIT  : 0) |
    (queue->vk.queueFlags & VK_QUEUE_TRANSFER_BIT ? RI_QUEUE_TRANSFER_BIT  : 0) | 
    (queue->vk.queueFlags & VK_QUEUE_SPARSE_BINDING_BIT ? RI_QUEUE_SPARSE_BINDING_BIT  : 0) |  
    (queue->vk.queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR ? RI_QUEUE_VIDEO_DECODE_BIT : 0) |
    (queue->vk.queueFlags & VK_QUEUE_VIDEO_ENCODE_BIT_KHR ? RI_QUEUE_VIDEO_ENCODE_BIT  : 0) |
    (queue->vk.queueFlags & VK_QUEUE_PROTECTED_BIT ? RI_QUEUE_PROTECTED_BIT  : 0) |
    (queue->vk.queueFlags & VK_QUEUE_OPTICAL_FLOW_BIT_NV ? RI_QUEUE_OPTICAL_FLOW_BIT_NV : 0);
#endif
  return 0;
}

struct RIDeviceDesc_s {
	struct RIPhysicalAdapter_s *physicalAdapter;
};
int InitRIRenderer( const struct RIBackendInit_s *init, struct RIRenderer_s *renderer );
void ShutdownRIRenderer( struct RIRenderer_s *renderer );

int EnumerateRIAdapters( struct RIRenderer_s *renderer, struct RIPhysicalAdapter_s *adapters, uint32_t *numAdapters );
int InitRIDevice( struct RIRenderer_s *renderer, struct RIDeviceDesc_s *init, struct RIDevice_s *device );

void WaitRIQueueIdle( struct RIDevice_s *device, struct RIQueue_s *queue );

int FreeRIDevice( struct RIDevice_s *dev );
void FreeRIFree( struct RIDevice_s *dev, struct RIFree *mem );

// RIDescriptor
void RIFinalizeDescriptor( struct RIDevice_s *dev, struct RIDescriptor_s *desc ); // after configure an RIDescriptor call update to configure it
void FreeRIDescriptor( struct RIDevice_s *dev, struct RIDescriptor_s *desc );
static inline bool RI_IsEmptyDescriptor( struct RIDescriptor_s *desc ) { return desc->cookie == 0; }

// RITexture
void FreeRITexture( struct RIDevice_s *dev, struct RITexture_s *tex );
void FreeRITextureView( struct RIDevice_s *dev, struct RITextureView_s *view );
struct RITextureView_s TextureviewRIDescriptor( struct RIDescriptor_s *desc );

// RIPool
void InitRIPool( struct RIDevice_s *dev, struct RIPool_s *pool, struct RIQueue_s *queue );
void FreeRIPool( struct RIDevice_s *dev, struct RIPool_s *pool );
void ResetRIPool( struct RIDevice_s *dev, struct RIPool_s *pool );

// RICmd
void InitRICmd( struct RIDevice_s *dev, struct RIPool_s *pool, struct RICmd_s *cmd );
void BeginRICmd( struct RIDevice_s *dev, struct RICmd_s *cmd );
void EndRICmd( struct RIDevice_s *dev, struct RICmd_s *cmd );
void FreeRICmd( struct RIDevice_s *dev, struct RICmd_s *cmd);

// RICommandRing
void InitRICommandRingBuffer( struct RIDevice_s *dev, struct RIQueue_s *queue, struct RICommandRingBuffer_s *ring, bool syncPrimitives );
void FreeRICommandRingBuffer( struct RIDevice_s *dev, struct RICommandRingBuffer_s *ring );
void AdvanceRICommandRingBuffer( struct RICommandRingBuffer_s *ring );
struct RICommandRingElement_s GetRICommandRingElement( struct RIDevice_s *dev, struct RICommandRingBuffer_s *ring, uint32_t numCmds );
void WaitRICommandRingElement( struct RIDevice_s *dev, struct RICommandRingElement_s *element );

#if DEVICE_IMPL_VULKAN
void VK_ConfigureBufferQueueFamilies( VkBufferCreateInfo *info, struct RIQueue_s *queues, size_t numQueues, uint32_t *queueFamiliesIdx, size_t reservedLen );
void VK_ConfigureImageQueueFamilies( VkImageCreateInfo *info, struct RIQueue_s *queues, size_t numQueues, uint32_t *queueFamiliesIdx, size_t reservedLen );
void VK_FillQueueFamilies( struct RIDevice_s *dev, uint32_t *queueFamilies, uint32_t *queueFamiliesIdx, size_t reservedLen );
#endif

#endif

