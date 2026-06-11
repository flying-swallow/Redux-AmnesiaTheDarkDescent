#include "graphics/RIScratchAlloc.h"
#include "system/stb_ds.h"
#include "graphics/RIRenderer.h"
#include "system/Types.h"
#include <cassert>

struct RIBlockMem RIUniformScratchAllocHandler( struct RIDevice *device, struct RIScratchAlloc *scratch, size_t size )
{
	struct RIBlockMem mem = {};
#if ( DEVICE_IMPL_VULKAN )
	{
		uint32_t queueFamilies[RI_QUEUE_LEN] = { 0 };
		VkBufferCreateInfo stageBufferCreateInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
		stageBufferCreateInfo.pNext = NULL;
		stageBufferCreateInfo.flags = 0;
		stageBufferCreateInfo.size = size;
		stageBufferCreateInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT
		                            | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
		VK_ConfigureBufferQueueFamilies( &stageBufferCreateInfo, device->queues, RI_QUEUE_LEN, queueFamilies, RI_QUEUE_LEN );

		VmaAllocationInfo allocationInfo = { 0 };
		VmaAllocationCreateInfo allocInfo = { 0 };
		allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
		allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
			VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT |
			VMA_ALLOCATION_CREATE_MAPPED_BIT;

		VK_WrapResult(vmaCreateBufferWithAlignment(device->vk.vmaAllocator, &stageBufferCreateInfo, &allocInfo, scratch->alignmentReq, &mem.buffer.vk.buffer, &mem.buffer.vk.allocation, &allocationInfo));

		VkMemoryPropertyFlags memPropFlags;
		vmaGetAllocationMemoryProperties( device->vk.vmaAllocator, mem.buffer.vk.allocation, &memPropFlags );

		mem.buffer.mappedAddress = allocationInfo.pMappedData;

		VkBufferDeviceAddressInfo bdaInfo = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
		bdaInfo.buffer = mem.buffer.vk.buffer;
		mem.deviceAddress = vkGetBufferDeviceAddress( device->vk.device, &bdaInfo );
	}
#endif
	return mem;
}

struct RIBlockMem RIAccelScratchAllocHandler( struct RIDevice *device, struct RIScratchAlloc *scratch, size_t size )
{
	struct RIBlockMem mem = {};
#if ( DEVICE_IMPL_VULKAN )
	{
		uint32_t queueFamilies[RI_QUEUE_LEN] = { 0 };
		VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
		bci.pNext = NULL;
		bci.flags = 0;
		bci.size  = size;
		// AS build scratch: storage-buffer access from the AS build, plus BDA so
		// the build can read it as a raw pointer.
		bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
		          | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
		VK_ConfigureBufferQueueFamilies( &bci, device->queues, RI_QUEUE_LEN, queueFamilies, RI_QUEUE_LEN );

		// GPU-only memory — no host access, no mapping.
		VmaAllocationCreateInfo aci = { 0 };
		aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

		VmaAllocationInfo allocationInfo = { 0 };
		VK_WrapResult( vmaCreateBufferWithAlignment( device->vk.vmaAllocator, &bci, &aci, scratch->alignmentReq, &mem.buffer.vk.buffer, &mem.buffer.vk.allocation, &allocationInfo ) );

		VkBufferDeviceAddressInfo bdaInfo = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
		bdaInfo.buffer = mem.buffer.vk.buffer;
		mem.deviceAddress = vkGetBufferDeviceAddress( device->vk.device, &bdaInfo );

		// mem.buffer.mappedAddress stays NULL — AS scratch is never read or written by the host.
	}
#endif
	return mem;
}

void InitRIScratchAlloc( struct RIDevice *device, struct RIScratchAlloc *pool, const struct RIScratchAllocDesc *desc ) {
	memset( pool, 0, sizeof( struct RIScratchAlloc ) );
  pool->alignmentReq = desc->alignmentReq;
  pool->blockSize = desc->blockSize;
  pool->alloc = desc->alloc;
}

static inline bool __isPoolSlotEmpty( struct RIDevice *device, struct RIBlockMem *block )
{
#if ( DEVICE_IMPL_VULKAN )
	{
		return block->buffer.vk.buffer == NULL;
	}
#endif
	return false;
}

static inline void __FreeRIBlockMem(struct RIDevice *device,struct RIBlockMem *block ) {
#if ( DEVICE_IMPL_VULKAN )
	if( block->buffer.vk.buffer ) {
		block->buffer.dispose( device );
	}
#endif
}

void FreeRIScratchAlloc( struct RIDevice *device, struct RIScratchAlloc *pool ) {
#if ( DEVICE_IMPL_VULKAN )
	if( pool->current.buffer.vk.buffer ) {
		__FreeRIBlockMem( device, &pool->current );
	}

	for( size_t i = 0; i < arrlen( pool->recycle ); i++ ) {
		__FreeRIBlockMem( device, &pool->recycle[i] );
	}

	for( size_t i = 0; i < arrlen( pool->pool ); i++ ) {
		__FreeRIBlockMem( device, &pool->pool[i] );
	}

	for( size_t i = 0; i < arrlen( pool->oversized ); i++ ) {
		__FreeRIBlockMem( device, &pool->oversized[i] );
	}
#endif
	arrfree( pool->recycle );
	arrfree( pool->pool );
	arrfree( pool->oversized );
}

void RIResetScratchAlloc( struct RIDevice *device, struct RIScratchAlloc *pool )
{
	for( size_t i = 0; i < arrlen( pool->recycle ); i++ ) {
		arrpush( pool->pool, pool->recycle[i] );
	}
	arrsetlen( pool->recycle, 0 );

	// Oversized one-shots can't be reused — they don't match blockSize.
	// Free them outright; a caller asking for the same size next frame
	// will pay for a fresh allocation.
	for( size_t i = 0; i < arrlen( pool->oversized ); i++ ) {
		__FreeRIBlockMem( device, &pool->oversized[i] );
	}
	arrsetlen( pool->oversized, 0 );

	pool->blockOffset = 0;
}

size_t RINumberOfUsedBlock(struct RIDevice *device,struct RIScratchAlloc* pool) {
	size_t numBlock = 0;
	if( !__isPoolSlotEmpty( device, &pool->current ) ) {
		numBlock++;
	}
	numBlock += arrlen(pool->recycle);
	numBlock += arrlen(pool->oversized);
	return numBlock;
}
struct RIBlockMem *RIGetUsedBlock( struct RIDevice *device, struct RIScratchAlloc *pool, size_t index )
{
	// Iteration order: current (if any) → recycle[..] → oversized[..].
	size_t cursor = 0;
	if( !__isPoolSlotEmpty( device, &pool->current ) ) {
		if( index == 0 )
			return &pool->current;
		cursor = 1;
	}
	const size_t recycleEnd = cursor + (size_t)arrlen( pool->recycle );
	if( index < recycleEnd )
		return &pool->recycle[index - cursor];
	return &pool->oversized[index - recycleEnd];
}

struct RIBufferScratchAllocReq RIAllocBufferFromScratchAlloc( struct RIDevice *device, struct RIScratchAlloc *pool, size_t reqSize )
{
	const size_t alignReqSize = ALIGN_TO( reqSize, pool->alignmentReq );
	assert(pool->alloc);

	// Oversized one-shot: allocate a block sized exactly to this request,
	// stash it on the oversized list so reset frees it, and hand the whole
	// thing back. Leaves pool->current untouched so any subsequent normal
	// allocation keeps filling it at the same offset.
	if( alignReqSize > pool->blockSize ) {
		struct RIBlockMem oneShot = pool->alloc( device, pool, alignReqSize );
		arrpush( pool->oversized, oneShot );

		struct RIBufferScratchAllocReq req = {};
		req.block = oneShot;
		req.pMappedAddress = oneShot.buffer.mappedAddress;
		req.deviceAddress = oneShot.deviceAddress;
		req.bufferOffset = 0;
		req.bufferSize = reqSize;
		return req;
	}

	if( __isPoolSlotEmpty( device, &pool->current ) ) {
		pool->current = pool->alloc( device, pool, pool->blockSize );
		pool->blockOffset = 0;
	}

	// BDA-anchored alignment: pad blockOffset so (BDA + offset) is a multiple
	// of pool->alignmentReq. vmaCreateBufferWithAlignment guarantees a fresh
	// block's BDA is already aligned, and we advance by alignReqSize (a
	// multiple of alignmentReq) afterward, so this is normally a no-op — but
	// it keeps the allocator correct if a future change introduces per-request
	// alignments stronger than pool->alignmentReq.
	{
		VkDeviceAddress curBda     = pool->current.deviceAddress + pool->blockOffset;
		VkDeviceAddress alignedBda = ALIGN_TO( curBda, pool->alignmentReq );
		pool->blockOffset += (size_t)( alignedBda - curBda );
	}

	if( pool->blockOffset + alignReqSize > pool->blockSize ) {
		arrpush( pool->recycle, pool->current );
		const size_t poolSize = arrlen( pool->pool );
		if( poolSize > 0 ) {
			memcpy( &( pool->current ), &( pool->pool[poolSize - 1] ), sizeof( struct RIBlockMem ) );
			arrsetlen( pool->pool, poolSize - 1 );
		} else {
		  pool->current = pool->alloc( device, pool, pool->blockSize );
		}
		pool->blockOffset = 0;
		// Re-anchor offset to the new/recycled block's BDA.
		VkDeviceAddress curBda     = pool->current.deviceAddress + pool->blockOffset;
		VkDeviceAddress alignedBda = ALIGN_TO( curBda, pool->alignmentReq );
		pool->blockOffset += (size_t)( alignedBda - curBda );
	}

	struct RIBufferScratchAllocReq req = {};
	req.block = pool->current;
	req.pMappedAddress = pool->current.buffer.mappedAddress;
	req.deviceAddress = pool->current.deviceAddress + pool->blockOffset;
	req.bufferOffset = pool->blockOffset;
	req.bufferSize = reqSize;
	pool->blockOffset += alignReqSize;
	return req;
}

void RIFinishScrachReq( struct RIDevice *device, struct RIBufferScratchAllocReq *req )
{
#if ( DEVICE_IMPL_VULKAN )
	VK_WrapResult( vmaFlushAllocation( device->vk.vmaAllocator, req->block.buffer.vk.allocation, req->bufferOffset, req->bufferSize ) );
#endif
}

