#ifndef RI_SCRATCH_ALLOC_H
#define RI_SCRATCH_ALLOC_H
#include "RITypes.h"

#define RI_UNIFORM_SCRATCH_ALLLOC_SIZE (256 * 128)
#define RI_UNIFORM_SCRATCH_REQ_ALIGNMENT (256) 

struct RIBlockMem_s {
	struct RIBuffer_s buffer;       // VkBuffer + VmaAllocation + mappedAddress
#if ( DEVICE_IMPL_VULKAN )
	VkDeviceAddress deviceAddress;  // BDA of buffer offset 0, set at alloc time
#endif
};

struct RIScratchAlloc_s;
typedef struct RIBlockMem_s ( *RIAllocBlock_Func)(struct RIDevice_s* device, struct RIScratchAlloc_s* scratch, size_t size);

struct RIScratchAlloc_s {
	struct RIBlockMem_s *recycle;
	struct RIBlockMem_s *pool;
	// One-shot blocks for reqSize > blockSize. Freed on reset rather than
	// pooled — the wrong size to be reused for normal block-sized allocs.
	struct RIBlockMem_s *oversized;

	size_t alignmentReq;
	size_t blockSize;
	RIAllocBlock_Func alloc;

	// the current buffer
	struct RIBlockMem_s current;
	size_t blockOffset;
};

struct RIScratchAllocDesc_s{
	size_t blockSize;
	size_t alignmentReq;
	RIAllocBlock_Func alloc;
};

struct RIBufferScratchAllocReq_s {
	struct RIBlockMem_s block;
	void* pMappedAddress;
	size_t bufferOffset;
	size_t bufferSize;
	VkDeviceAddress deviceAddress;  // = block.vk.deviceAddress + bufferOffset
};

size_t RINumberOfUsedBlock(struct RIDevice_s *device,struct RIScratchAlloc_s* pool);
struct RIBlockMem_s* RIGetUsedBlock(struct RIDevice_s *device,struct RIScratchAlloc_s* pool,size_t index);

struct RIBlockMem_s RIUniformScratchAllocHandler(struct RIDevice_s* device, struct RIScratchAlloc_s* scratch, size_t size);
struct RIBlockMem_s RIAccelScratchAllocHandler(struct RIDevice_s* device, struct RIScratchAlloc_s* scratch, size_t size);

void InitRIScratchAlloc( struct RIDevice_s *device, struct RIScratchAlloc_s *pool, const struct RIScratchAllocDesc_s *desc );
void FreeRIScratchAlloc( struct RIDevice_s *device, struct RIScratchAlloc_s *pool ); 
void RIResetScratchAlloc( struct RIDevice_s *device, struct RIScratchAlloc_s  *pool );

struct RIBufferScratchAllocReq_s RIAllocBufferFromScratchAlloc( struct RIDevice_s *device, struct RIScratchAlloc_s  *pool, size_t reqSize );
void RIFinishScrachReq( struct RIDevice_s *device, struct RIBufferScratchAllocReq_s *req );

#endif


