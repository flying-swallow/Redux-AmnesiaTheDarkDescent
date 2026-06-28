#include "graphics/RIDescriptorSetAllocator.h"
#include "system/stb_ds.h"
#include <cassert>

struct RIDescriptorSetSlot *allocDescriptorSetSlot( struct RIDescriptorSetAlloc *alloc )
{
	if( alloc->blocks == NULL || alloc->blockIndex == RESERVE_BLOCK_SIZE ) {
		struct RIDescriptorSetSlot *block = (struct RIDescriptorSetSlot*)calloc( RESERVE_BLOCK_SIZE, sizeof( struct RIDescriptorSetSlot ) );
		alloc->blockIndex = 0;
		arrpush( alloc->blocks, block );
		return block + ( alloc->blockIndex++ );
	}
	return alloc->blocks[arrlen( alloc->blocks ) - 1] + ( alloc->blockIndex++ );
}

void attachDescriptorSlot( struct RIDescriptorSetAlloc *alloc, struct RIDescriptorSetSlot *slot )
{
	assert( slot );
	{
		slot->quNext = NULL;
		slot->quPrev = alloc->queue_end;
		if( alloc->queue_end ) {
			alloc->queue_end->quNext = slot;
		}
		alloc->queue_end = slot;
		if( !alloc->queue_begin ) {
			alloc->queue_begin = slot;
		}
	}
	{
		const size_t hashIndex = slot->hash % ALLOC_HASH_RESERVE;
		slot->hPrev = NULL;
		slot->hNext = NULL;
		if( alloc->hash_slots[hashIndex] ) {
			alloc->hash_slots[hashIndex]->hPrev = slot;
			slot->hNext = alloc->hash_slots[hashIndex];
		}
		alloc->hash_slots[hashIndex] = slot;
	}
}

void detachDescriptorSlot( struct RIDescriptorSetAlloc *alloc, struct RIDescriptorSetSlot *slot )
{
	assert( slot );
	// remove from queue
	{
		if( alloc->queue_begin == slot ) {
			alloc->queue_begin = slot->quNext;
			if( slot->quNext ) {
				slot->quNext->quPrev = NULL;
			}
		} else if( alloc->queue_end == slot ) {
			alloc->queue_end = slot->quPrev;
			if( slot->quPrev ) {
				slot->quPrev->quNext = NULL;
			}
		} else {
			if( slot->quPrev ) {
				slot->quPrev->quNext = slot->quNext;
			}
			if( slot->quNext ) {
				slot->quNext->quPrev = slot->quPrev;
			}
		}
	}
	// free from hashTable
	{
		const size_t hashIndex = slot->hash % ALLOC_HASH_RESERVE;
		if( alloc->hash_slots[hashIndex] == slot ) {
			alloc->hash_slots[hashIndex] = slot->hNext;
			if( slot->hNext ) {
				slot->hNext->hPrev = NULL;
			}
		} else {
			if( slot->hPrev ) {
				slot->hPrev->hNext = slot->hNext;
			}
			if( slot->hNext ) {
				slot->hNext->hPrev = slot->hPrev;
			}
		}
		slot->hPrev = NULL;
		slot->hNext = NULL;
	}
}

struct RIDescriptorSetResult resolveDescriptorSetAlloc( struct RIDevice *device, struct RIDescriptorSetAlloc *alloc, uint32_t frameCount, hash_t hash )
{
	struct RIDescriptorSetResult result = { 0 };
	const size_t hashIndex = hash % ALLOC_HASH_RESERVE;
	for( struct RIDescriptorSetSlot *c = alloc->hash_slots[hashIndex]; c; c = c->hNext ) {
		if( c->hash == hash ) {
			if( alloc->queue_end != c ) {
				if( alloc->queue_begin == c ) {
					alloc->queue_begin = c->quNext;
					if( c->quNext ) {
						c->quNext->quPrev = NULL;
					}
				} else {
					if( c->quPrev ) {
						c->quPrev->quNext = c->quNext;
					}
					if( c->quNext ) {
						c->quNext->quPrev = c->quPrev;
					}
				}
				c->quNext = NULL;
				c->quPrev = alloc->queue_end;
				if( alloc->queue_end ) {
					alloc->queue_end->quNext = c;
				}
				alloc->queue_end = c;
			}

			c->frameCount = frameCount;
			result.set = c;
			result.found = true;
			assert(result.set);
			return result;
		}
	}

	if( alloc->queue_begin && frameCount > alloc->queue_begin->frameCount + alloc->framesInFlight) {
		struct RIDescriptorSetSlot *slot = alloc->queue_begin;
		detachDescriptorSlot( alloc, slot );
		slot->frameCount = frameCount;
		slot->hash = hash;
		attachDescriptorSlot( alloc, slot );
		result.set = slot;
		result.found = false;
		assert(result.set);
		return result;
	}

	if( arrlen( alloc->reservedSlots ) == 0 ) {
		alloc->descriptor_alloc_handle(device, alloc);
		assert(arrlen(alloc->reservedSlots) > 0); // we didn't reserve any slots ...
	}
	struct RIDescriptorSetSlot *slot = arrpop( alloc->reservedSlots );
	slot->hash = hash;
	slot->frameCount = frameCount;
	attachDescriptorSlot( alloc, slot );
	result.set = slot;
	result.found = false;
	assert(result.set);
	return result;
}

void freeDescriptorSetAlloc( struct RIDevice *device, struct RIDescriptorSetAlloc *alloc )
{
#if ( DEVICE_IMPL_VULKAN )
	for( size_t i = 0; i < static_cast<size_t>(arrlen( alloc->blocks )); i++ ) {
		// TODO: do i need to free indivudal descriptor sets or can i just free the entire pool
		// for(size_t blockIdx = 0; blockIdx < RESERVE_BLOCK_SIZE; blockIdx++) {
		//	vkFreeDescriptorSets(device->vk.device, alloc->blocks[i]->vk.pool, )
		//}
		free( alloc->blocks[i] );
	}
	arrfree( alloc->blocks );
	for( size_t i = 0; i < static_cast<size_t>(arrlen( alloc->pools )); i++ ) {
		vkDestroyDescriptorPool( device->vk.device, alloc->pools[i].vk.handle, NULL );
	}
	arrfree( alloc->pools );
#endif
}
