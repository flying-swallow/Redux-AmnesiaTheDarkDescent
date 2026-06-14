#ifndef R_DESCRIPTOR_POOL_H
#define R_DESCRIPTOR_POOL_H

#include <cstddef>
#include <stdint.h>

#include "graphics/RITypes.h"
#include "system/Hasher.h"

#define RESERVE_BLOCK_SIZE 1024
#define ALLOC_HASH_RESERVE 256
#define DESCRIPTOR_MAX_SIZE 64
#define DESCRIPTOR_RESERVED_SIZE 64

struct RIDescriptorSetSlot {
	hash_t hash;
	uint32_t frameCount;
	// queue
	struct RIDescriptorSetSlot *quNext;
	struct RIDescriptorSetSlot *quPrev;

	// hash
	struct RIDescriptorSetSlot *hNext;
	struct RIDescriptorSetSlot *hPrev;
	union {
#if ( DEVICE_IMPL_VULKAN )
		struct {
			VkDescriptorPool pool;
			VkDescriptorSet handle;
		} vk;
#endif
#if ( DEVICE_IMPL_MTL )
		// On Metal a descriptor set is an argument buffer; this slot owns one,
		// encoded (on a cache miss) by the set's RIDescriptorSetAlloc::mtl.encoder.
		struct {
			MTL::Buffer *argumentBuffer;
		} mtl;
#endif
	};
};

struct RIDescriptorPoolAllocSlot {
		union {
#if ( DEVICE_IMPL_VULKAN )
			struct {
				VkDescriptorPool handle;
			} vk;
#endif
		};
};

struct RIDescriptorSetAlloc;
typedef void ( *RIDescriptorSetAlloc_Create )( struct RIDevice *device, struct RIDescriptorSetAlloc *alloc );

struct RIDescriptorSetAlloc {
	RIDescriptorSetAlloc_Create descriptor_alloc_handle;
	uint8_t framesInFlight; // the number of frames in flight 

	struct RIDescriptorSetSlot *hash_slots[ALLOC_HASH_RESERVE];
	struct RIDescriptorSetSlot *queue_begin;
	struct RIDescriptorSetSlot *queue_end;

	struct RIDescriptorSetSlot **reservedSlots; // stb arrays
	struct RIDescriptorPoolAllocSlot* pools; // stb arrays
	struct RIDescriptorSetSlot **blocks;
	size_t blockIndex;
#if ( DEVICE_IMPL_MTL )
	// Per-set argument-buffer encoder (built once at program init) + the byte
	// size each reserved slot's argument buffer must be. mtlDescriptorSetAlloc
	// allocates the buffers; bindDescriptors re-targets the encoder per miss.
	struct {
		MTL::ArgumentEncoder *encoder;
		uint64_t encodedLength;
	} mtl;
#endif
};

struct RIDescriptorSetResult {
	bool found;
	struct RIDescriptorSetSlot *set; // the associated slot
};

struct RIDescriptorSetResult resolveDescriptorSetAlloc( struct RIDevice *device,
													 struct RIDescriptorSetAlloc *alloc,
													 uint32_t frameCount,
													 hash_t hash);
void freeDescriptorSetAlloc( struct RIDevice *device, struct RIDescriptorSetAlloc *alloc );

// utility
struct RIDescriptorSetSlot *allocDescriptorSetSlot( struct RIDescriptorSetAlloc *alloc );
void attachDescriptorSlot( struct RIDescriptorSetAlloc *alloc, struct RIDescriptorSetSlot *slot );
void detachDescriptorSlot( struct RIDescriptorSetAlloc *alloc, struct RIDescriptorSetSlot *slot );

#endif
