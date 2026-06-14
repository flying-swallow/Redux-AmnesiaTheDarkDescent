#ifndef RI_RESOURCE_UPLOAD_H
#define RI_RESOURCE_UPLOAD_H

#define RI_RESOURCE_MAX_SETS 2
#define RI_RESOURCE_STAGE_BUFFER_SIZE (8 * MB_TO_BYTE)

#include "RITypes.h"

// Slice of mapped staging memory handed back to a transaction.
struct RIMappedMemoryRange {
	size_t offset;
	size_t size;
	void *data;
	// Staging source for the recorded copy (the persistent staging buffer or a
	// temporary). Unset on the UMA direct-write path, where 'data' points straight
	// into the target and no copy is recorded.
	struct RIBuffer buffer;
};

// One queue + per-set cmd pool / cmd buffer / staging buffer / fence / semaphore.
struct RITransferCommandGroup {
	struct RIQueue *queue;

	bool is_recording;
	size_t active_set;

	struct RIPool cmd_pool[RI_RESOURCE_MAX_SETS];
	struct RICmd cmd[RI_RESOURCE_MAX_SETS];

	size_t staging_buffer_offset; // running tail in active set's staging buffer
	struct RIBuffer staging_buffer[RI_RESOURCE_MAX_SETS];

	// per-set overflow buffers (stb_ds arrays); freed when the set is reused
	struct RIBuffer *temporary_buffers[RI_RESOURCE_MAX_SETS];

	union {
#if ( DEVICE_IMPL_VULKAN )
		struct {
			VkFence fences[RI_RESOURCE_MAX_SETS];
			VkSemaphore semaphores[RI_RESOURCE_MAX_SETS];
		} vk;
#endif
#if ( DEVICE_IMPL_MTL )
		struct {
			// Per-set timeline fence: the active set's command buffer signals
			// 'values[set]' on commit; __AcquireCmd host-waits it before reuse.
			MTL::SharedEvent *events[RI_RESOURCE_MAX_SETS];
			uint64_t values[RI_RESOURCE_MAX_SETS];
		} mtl;
#endif
	};
};

struct RIResourceUploader {
	struct RITransferCommandGroup upload_resource; // graphics queue
	struct RITransferCommandGroup copy_resource;   // transfer / copy queue
};

struct RIResourceBufferTransaction {
	struct RIBuffer target;
	size_t size;
	size_t offset; // destination offset inside 'target'

	// State the target is in before the copy and the state to return it to
	// afterwards (RIBarrier.h). currentState == COPY_DST skips the pre-barrier;
	// postState == UNDEFINED or COPY_DST skips the post-barrier. Stage hints
	// follow the usual rule: 0 derives a conservative mask from the state.
	enum RIResourceState_e currentState;
	uint32_t currentStages; // RIStageBits_e
	enum RIResourceState_e postState;
	uint32_t postStages; // RIStageBits_e

	// [mtl] Caller opt-in for the UMA direct-write fast path: set true ONLY when no
	// in-flight frame can read 'target' while it is rewritten (write-once / load-time
	// uploads). When true on a unified-memory device with a host-visible target,
	// the uploader memcpys straight into the buffer and records no copy. Per-frame
	// read-modify-write targets MUST leave this false so the staging+blit path keeps
	// the copy ordered against the previous frame's reads. Ignored on Vulkan.
	bool directWriteSafe = false;

	// filled by RI_ResourceBeginCopyBuffer
	struct RIMappedMemoryRange mapped;
};

struct RIResourceTextureTransaction {
	struct RITexture target;

	// Same semantics as the buffer transaction states above; barriers cover
	// only the single (mipOffset, arrayOffset) subresource being written.
	enum RIResourceState_e currentState;
	uint32_t currentStages; // RIStageBits_e
	enum RIResourceState_e postState;
	uint32_t postStages; // RIStageBits_e

	uint32_t format; // RI_Format_e
	uint32_t sliceNum;
	uint32_t rowPitch;

	uint16_t x;
	uint16_t y;
	uint16_t z;
	uint32_t width;
	uint32_t height;
	uint32_t depth;

	uint32_t arrayOffset;
	uint32_t mipOffset;

	// filled by RI_ResourceBeginCopyTexture
	uint32_t alignRowPitch;
	uint32_t alignSlicePitch;
	struct RIMappedMemoryRange mapped;
};

void RI_InitResourceUploader( struct RIDevice *device, struct RIResourceUploader *res );
void RI_FreeResourceUploader( struct RIDevice *device, struct RIResourceUploader *res );

void RI_ResourceBeginCopyBuffer( struct RIDevice *device, struct RIResourceUploader *res, struct RIResourceBufferTransaction *trans );
void RI_ResourceEndCopyBuffer( struct RIDevice *device, struct RIResourceUploader *res, struct RIResourceBufferTransaction *trans );

void RI_ResourceBeginCopyTexture( struct RIDevice *device, struct RIResourceUploader *res, struct RIResourceTextureTransaction *trans );
void RI_ResourceEndCopyTexture( struct RIDevice *device, struct RIResourceUploader *res, struct RIResourceTextureTransaction *trans );

struct RIResourceUploaderVKResult {
	bool signaled;

	union {
#if ( DEVICE_IMPL_VULKAN )
		struct {
			VkFence fence;
			VkSemaphore semaphore;
		} vk;
#endif
#if ( DEVICE_IMPL_MTL )
		struct {
			MTL::SharedEvent *event;
			uint64_t value; // value the committed upload signals on 'event'
		} mtl;
#endif
	};
};

#if ( DEVICE_IMPL_VULKAN )
struct RIResourceUploaderVKResult RI_VKFlushResourceUpdate( struct RIDevice *device, struct RIResourceUploader *res, size_t num_semaphores, VkSemaphoreSubmitInfo *wait_semaphore_info );
#endif

#if ( DEVICE_IMPL_MTL )
// Commit the active set's upload command buffer (if anything was recorded),
// signalling its per-set SharedEvent so a later submit can wait on it. When
// nothing was recorded, returns signaled=false with the current set's event/value.
struct RIResourceUploaderVKResult RI_MTLFlushResourceUpdate( struct RIDevice *device, struct RIResourceUploader *res );
#endif

#endif
