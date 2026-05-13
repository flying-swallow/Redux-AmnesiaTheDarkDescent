#ifndef RI_RENDERER_H
#define RI_RENDERER_H

#include "graphics/RITypes.h"
#include <cassert>
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
void WaitRICommandRingElement( struct RIDevice_s *dev, struct RICommandRingElement_s *element );

template<uint32_t MaxPoolCount, uint32_t CmdPerPool>
void InitRICommandRingBuffer( struct RIDevice_s *dev, struct RIQueue_s *queue,
                              struct RICommandRingBuffer_s<MaxPoolCount, CmdPerPool> *ring,
                              uint32_t poolCount, uint32_t cmdPerPool, bool syncPrimitives )
{
	assert( poolCount > 0 && poolCount <= MaxPoolCount );
	assert( cmdPerPool > 0 && cmdPerPool <= CmdPerPool );
	memset( ring, 0, sizeof( *ring ) );
	ring->poolCount = poolCount;
	ring->cmdPerPool = cmdPerPool;
	ring->syncPrimitive = syncPrimitives;

	ring->poolIndex = 0;
	ring->cmdIndex = 0;
	ring->fenceIndex = 0;

	for( uint32_t poolIdx = 0; poolIdx < ring->poolCount; poolIdx++ ) {
		InitRIPool( dev, &ring->pools[poolIdx], queue );
		for( uint32_t cmdIdx = 0; cmdIdx < ring->cmdPerPool; cmdIdx++ ) {
			InitRICmd( dev, &ring->pools[poolIdx], &ring->cmds[poolIdx][cmdIdx] );
#if ( DEVICE_IMPL_VULKAN )
			if( syncPrimitives ) {
				VkSemaphoreCreateInfo semaphoreCreateInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
				VK_WrapResult( vkCreateSemaphore( dev->vk.device, &semaphoreCreateInfo, NULL, &ring->vk.semaphores[poolIdx][cmdIdx] ) );

				VkFenceCreateInfo fenceCreateInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
				fenceCreateInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
				VK_WrapResult( vkCreateFence( dev->vk.device, &fenceCreateInfo, NULL, &ring->vk.fences[poolIdx][cmdIdx] ) );
			}
#endif
		}
	}
}

template<uint32_t MaxPoolCount, uint32_t CmdPerPool>
void FreeRICommandRingBuffer( struct RIDevice_s *dev,
                              struct RICommandRingBuffer_s<MaxPoolCount, CmdPerPool> *ring )
{
	for( uint32_t poolIdx = 0; poolIdx < ring->poolCount; poolIdx++ ) {
		for( uint32_t cmdIdx = 0; cmdIdx < ring->cmdPerPool; cmdIdx++ ) {
			FreeRICmd( dev, &ring->cmds[poolIdx][cmdIdx] );
#if ( DEVICE_IMPL_VULKAN )
			if( ring->syncPrimitive ) {
				vkDestroySemaphore( dev->vk.device, ring->vk.semaphores[poolIdx][cmdIdx], NULL );
				vkDestroyFence( dev->vk.device, ring->vk.fences[poolIdx][cmdIdx], NULL );
			}
#endif
		}
		FreeRIPool( dev, &ring->pools[poolIdx] );
	}
}

template<uint32_t MaxPoolCount, uint32_t CmdPerPool>
void AdvanceRICommandRingBuffer( struct RICommandRingBuffer_s<MaxPoolCount, CmdPerPool> *ring )
{
	ring->poolIndex = ( ring->poolIndex + 1 ) % ring->poolCount;
	ring->cmdIndex = 0;
	ring->fenceIndex = 0;
}

template<uint32_t MaxPoolCount, uint32_t CmdPerPool>
struct RICommandRingElement_s GetRICommandRingElement( struct RIDevice_s *dev,
                                                       struct RICommandRingBuffer_s<MaxPoolCount, CmdPerPool> *ring,
                                                       uint32_t numCmds )
{
	struct RICommandRingElement_s result;
	memset( &result, 0, sizeof( struct RICommandRingElement_s ) );

	assert( numCmds <= ring->cmdPerPool );
	assert( numCmds + ring->cmdIndex <= ring->cmdPerPool );

	result.cmds = &ring->cmds[ring->poolIndex][ring->cmdIndex];
	result.numCmds = numCmds;
	result.pool = &ring->pools[ring->poolIndex];
#if ( DEVICE_IMPL_VULKAN )
	if( ring->syncPrimitive ) {
		result.vk.semaphore = ring->vk.semaphores[ring->poolIndex][ring->fenceIndex];
		result.vk.fence = ring->vk.fences[ring->poolIndex][ring->fenceIndex];
	}
#endif

	ring->fenceIndex += 1;
	ring->cmdIndex += numCmds;

	return result;
}

// RIAccelStructure
// Query backing-storage and scratch sizes before InitRIAccelStructure; any out-pointer may be NULL.
// For BLAS, desc->geometries describes the geometry layout used to compute sizes; the actual
// vertex/index buffer addresses don't need to be valid until the build command.
void GetRIAccelStructureMemoryReqs( struct RIDevice_s *dev,
                                    const struct RIAccelStructureDesc_s *desc,
                                    uint64_t *outStorageSize,
                                    uint64_t *outBuildScratchSize,
                                    uint64_t *outUpdateScratchSize );
// Creates VkAccelerationStructureKHR backed by desc->storage at desc->storageOffset.
// Caller must allocate desc->storage with VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR
// and VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, sized at least desc->storageSize bytes.
int  InitRIAccelStructure( struct RIDevice_s *dev,
                           const struct RIAccelStructureDesc_s *desc,
                           struct RIAccelStructure_s *outAS );
void FreeRIAccelStructure( struct RIDevice_s *dev, struct RIAccelStructure_s *as );
uint64_t GetRIAccelStructureDeviceAddress( struct RIDevice_s *dev, const struct RIAccelStructure_s *as );
void RIFinalizeAccelStructureDescriptor( struct RIDevice_s *dev,
                                         struct RIDescriptor_s *desc,
                                         struct RIAccelStructure_s *as );

// Build-command wrappers; numDescs structures are submitted in a single backend call.
// Caller-supplied scratchBuffer must include VK_BUFFER_USAGE_STORAGE_BUFFER_BIT and
// VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, with scratchOffset aligned to
// minAccelerationStructureScratchOffsetAlignment. Input vertex/index/instance buffers must
// include VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR and
// VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT.
void CmdBuildRIBlas( struct RIDevice_s *dev, struct RICmd_s *cmd, const struct RIBuildBlasDesc_s *descs, uint32_t numDescs );
void CmdBuildRITlas( struct RIDevice_s *dev, struct RICmd_s *cmd, const struct RIBuildTlasDesc_s *descs, uint32_t numDescs );

// Leaf dispatch/draw command helpers. Pipeline binding is done separately
// (RIProgram::bindPipeline / bindComputePipeline / bindRayTracingPipeline);
// these are the "go" calls that issue the actual work. Core Vulkan 1.0 —
// no RIDevice_s needed because there's no fn-pointer indirection.
void CmdDispatch( struct RICmd_s *cmd,
                  uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ );
void CmdDispatchIndirect( struct RICmd_s *cmd,
                          struct RIBuffer_s *buffer, VkDeviceSize offset );
void CmdDraw( struct RICmd_s *cmd,
              uint32_t vertexCount, uint32_t instanceCount,
              uint32_t firstVertex, uint32_t firstInstance );
void CmdDrawIndexed( struct RICmd_s *cmd,
                     uint32_t indexCount, uint32_t instanceCount,
                     uint32_t firstIndex, int32_t vertexOffset,
                     uint32_t firstInstance );
void CmdDrawIndirect( struct RICmd_s *cmd,
                      struct RIBuffer_s *buffer, VkDeviceSize offset,
                      uint32_t drawCount, uint32_t stride );
void CmdDrawIndexedIndirect( struct RICmd_s *cmd,
                             struct RIBuffer_s *buffer, VkDeviceSize offset,
                             uint32_t drawCount, uint32_t stride );

#if DEVICE_IMPL_VULKAN
void VK_ConfigureBufferQueueFamilies( VkBufferCreateInfo *info, struct RIQueue_s *queues, size_t numQueues, uint32_t *queueFamiliesIdx, size_t reservedLen );
void VK_ConfigureImageQueueFamilies( VkImageCreateInfo *info, struct RIQueue_s *queues, size_t numQueues, uint32_t *queueFamiliesIdx, size_t reservedLen );
void VK_FillQueueFamilies( struct RIDevice_s *dev, uint32_t *queueFamilies, uint32_t *queueFamiliesIdx, size_t reservedLen );
#endif

#endif

