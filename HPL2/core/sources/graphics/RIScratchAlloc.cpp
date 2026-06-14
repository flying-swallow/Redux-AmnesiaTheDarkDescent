#include "graphics/RIScratchAlloc.h"
#include "system/stb_ds.h"
#include "graphics/RIRenderer.h"
#include "system/Types.h"
#include <cassert>

struct RIBlockMem RIUniformScratchAllocHandler( struct RIDevice *device, struct RIScratchAlloc *scratch, size_t size )
{
	struct RIBlockMem mem = {};
	// Host-mapped uniform block, addressable as a raw pointer (BDA on Vulkan).
	RIBufferDesc desc = {};
	desc.size = size;
	desc.usage = RI_BUFFER_USAGE_CONSTANT_BUFFER | RI_BUFFER_USAGE_DEVICE_ADDRESS;
	desc.location = RI_MEMORY_HOST_UPLOAD;
	desc.alignment = scratch->alignmentReq;
	mem.buffer = RIBuffer::create( device, desc );
	mem.deviceAddress = mem.buffer.GetDeviceHandle( device );
	return mem;
}

struct RIBlockMem RIAccelScratchAllocHandler( struct RIDevice *device, struct RIScratchAlloc *scratch, size_t size )
{
	struct RIBlockMem mem = {};
	// AS build scratch: GPU-only storage buffer, addressable as a raw pointer so
	// the build can read it. mappedAddress stays null — never host-touched.
	RIBufferDesc desc = {};
	desc.size = size;
	desc.usage = RI_BUFFER_USAGE_SHADER_RESOURCE_STORAGE | RI_BUFFER_USAGE_DEVICE_ADDRESS;
	desc.location = RI_MEMORY_DEVICE;
	desc.alignment = scratch->alignmentReq;
	mem.buffer = RIBuffer::create( device, desc );
	mem.deviceAddress = mem.buffer.GetDeviceHandle( device );
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
	if( device->renderer->is_target_selected( RI_DEVICE_API_VK ) ) {
		return block->buffer.vk.buffer == NULL;
	}
#endif
#if ( DEVICE_IMPL_MTL )
	if( device->renderer->is_target_selected( RI_DEVICE_API_MTL ) ) {
		return block->buffer.mtl.buffer == nullptr;
	}
#endif
	return false;
}

static inline void __FreeRIBlockMem(struct RIDevice *device,struct RIBlockMem *block ) {
	if( !__isPoolSlotEmpty( device, block ) ) {
		block->buffer.dispose( device );
	}
}

void FreeRIScratchAlloc( struct RIDevice *device, struct RIScratchAlloc *pool ) {
	if( !__isPoolSlotEmpty( device, &pool->current ) ) {
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
		RIDeviceSize curBda     = pool->current.deviceAddress + pool->blockOffset;
		RIDeviceSize alignedBda = ALIGN_TO( curBda, pool->alignmentReq );
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
		RIDeviceSize curBda     = pool->current.deviceAddress + pool->blockOffset;
		RIDeviceSize alignedBda = ALIGN_TO( curBda, pool->alignmentReq );
		pool->blockOffset += (size_t)( alignedBda - curBda );
	}

	// Invariant: both BDA re-anchors above leave blockOffset a no-op pad today
	// (blocks are created with desc.alignment == alignmentReq, so their BDA is
	// already aligned) and alignReqSize <= blockSize is guaranteed by the oversized
	// path. Assert it so a future per-request alignment stronger than alignmentReq
	// — which would make the re-anchor pad non-zero — can't silently hand back an
	// out-of-bounds offset.
	assert( pool->blockOffset + alignReqSize <= pool->blockSize );

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
	if( device->renderer->is_target_selected( RI_DEVICE_API_VK ) ) {
		VK_WrapResult( vmaFlushAllocation( device->vk.vmaAllocator, req->block.buffer.vk.allocation, req->bufferOffset, req->bufferSize ) );
	}
#endif
	// Metal: RI_MEMORY_HOST_UPLOAD blocks use StorageModeShared, which is
	// CPU/GPU-coherent on unified memory — no explicit flush needed.
}

