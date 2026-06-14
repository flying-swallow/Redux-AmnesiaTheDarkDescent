#include "graphics/RIResourceUploader.h"
#include "graphics/RIFormat.h"

#if ( DEVICE_IMPL_MTL )
#include "graphics/RIMTL.h"
#endif

#include <system/Types.h>
#include <system/stb_ds.h>

#include <cassert>
#include <cstring>

#if ( DEVICE_IMPL_MTL )
// Unified-memory predicate: the Metal adapter type is set from
// MTL::Device::hasUnifiedMemory() in RIRenderer::enumerateAdapters.
static inline bool RI_DeviceIsUMA( struct RIDevice *device )
{
	return device->physicalAdapter.type == RI_ADAPTER_TYPE_INTEGRATED_GPU;
}

// Opt-in UMA direct-write predicate. Begin and End both consult this (its inputs
// don't change between them) so End knows to skip the copy when Begin wrote straight
// into the target.
static inline bool __IsUMADirect( struct RIDevice *device, struct RIResourceBufferTransaction *trans )
{
	return trans->directWriteSafe && RI_DeviceIsUMA( device ) && trans->target.mappedAddress;
}

// Ensure the active set is recording on Metal: host-wait the set's prior use,
// reset its staging tail / temporaries, and open a fresh command buffer.
static struct RICmd *__AcquireCmdMTL( struct RIDevice *device, struct RITransferCommandGroup *group )
{
	const size_t set = group->active_set;
	if( !group->is_recording ) {
		if( group->mtl.events[set] )
			group->mtl.events[set]->waitUntilSignaledValue( group->mtl.values[set], UINT64_MAX );

		group->staging_buffer_offset = 0;
		for( size_t j = 0; j < (size_t)arrlen( group->temporary_buffers[set] ); j++ )
			group->temporary_buffers[set][j].dispose( device );
		arrsetlen( group->temporary_buffers[set], 0 );

		group->cmd[set].mtl.cmd = group->queue->mtl.queue->commandBuffer()->retain();
		group->is_recording = true;
	}
	return &group->cmd[set];
}
#endif

// Wait for the active set's fence (signalled means the GPU is done with the
// previous use), free overflow temporaries, reset the pool, begin the cmd
// buffer. No-op if the group is already recording.
#if ( DEVICE_IMPL_VULKAN )
static VkCommandBuffer __AcquireCmd( struct RIDevice *device, struct RITransferCommandGroup *group )
{
	if( !group->is_recording ) {
		VkFence fence = group->vk.fences[group->active_set];
		if( vkGetFenceStatus( device->vk.device, fence ) == VK_NOT_READY ) {
			VK_WrapResult( vkWaitForFences( device->vk.device, 1, &fence, VK_TRUE, UINT64_MAX ) );
		}

		group->staging_buffer_offset = 0;
		for( size_t j = 0; j < (size_t)arrlen( group->temporary_buffers[group->active_set] ); j++ ) {
			vkDestroyBuffer( device->vk.device, group->temporary_buffers[group->active_set][j].vk.buffer, NULL );
			vmaFreeMemory( device->vk.vmaAllocator, group->temporary_buffers[group->active_set][j].vk.allocation );
		}
		arrsetlen( group->temporary_buffers[group->active_set], 0 );

		VK_WrapResult( vkResetCommandPool( device->vk.device, group->cmd_pool[group->active_set].vk.pool, 0 ) );
		VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		VK_WrapResult( vkBeginCommandBuffer( group->cmd[group->active_set].vk.cmd, &beginInfo ) );

		group->is_recording = true;
	}
	return group->cmd[group->active_set].vk.cmd;
}
#endif

static void __InitTransferCommandGroup( struct RIDevice *device, struct RITransferCommandGroup *group, struct RIQueue *queue )
{
	memset( group, 0, sizeof( *group ) );
	group->queue = queue;
	group->active_set = 0;
	group->is_recording = false;

#if ( DEVICE_IMPL_VULKAN )
	for( size_t i = 0; i < RI_RESOURCE_MAX_SETS; i++ ) {
		{
			VkCommandPoolCreateInfo info = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
			info.queueFamilyIndex = queue->vk.queueFamilyIdx;
			info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
			group->cmd_pool[i].vk.queue = queue->vk.queue;
			VK_WrapResult( vkCreateCommandPool( device->vk.device, &info, NULL, &group->cmd_pool[i].vk.pool ) );

			VkCommandBufferAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
			allocInfo.commandPool = group->cmd_pool[i].vk.pool;
			allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
			allocInfo.commandBufferCount = 1;
			VK_WrapResult( vkAllocateCommandBuffers( device->vk.device, &allocInfo, &group->cmd[i].vk.cmd ) );
			group->cmd[i].vk.pool = group->cmd_pool[i].vk.pool;
		}

		{
			RIBufferDesc desc = {};
			desc.size = RI_RESOURCE_STAGE_BUFFER_SIZE;
			desc.usage = RI_BUFFER_USAGE_TRANSFER_SRC | RI_BUFFER_USAGE_TRANSFER_DST;
			desc.location = RI_MEMORY_HOST_UPLOAD;
			group->staging_buffer[i] = RIBuffer::create( device, desc );
		}

		// Fence starts signalled so the first acquire doesn't block.
		{
			VkFenceCreateInfo info = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
			info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
			VK_WrapResult( vkCreateFence( device->vk.device, &info, NULL, &group->vk.fences[i] ) );
		}

		{
			VkSemaphoreCreateInfo info = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
			VK_WrapResult( vkCreateSemaphore( device->vk.device, &info, NULL, &group->vk.semaphores[i] ) );
		}

		group->temporary_buffers[i] = NULL;
	}
#endif
#if ( DEVICE_IMPL_MTL )
	for( size_t i = 0; i < RI_RESOURCE_MAX_SETS; i++ ) {
		// One Shared (CPU-coherent) staging buffer per set; command buffers come
		// from the queue lazily (no command pool on Metal).
		group->staging_buffer[i] = RIBuffer::MTL_create( device, RI_RESOURCE_STAGE_BUFFER_SIZE, /*hostVisible*/ true );
		group->cmd_pool[i].mtl.queue = queue->mtl.queue;
		group->mtl.events[i] = device->mtl.device->newSharedEvent();
		group->mtl.values[i] = 0;
		group->temporary_buffers[i] = NULL;
	}
#endif
}

static void __FreeTransferCommandGroup( struct RIDevice *device, struct RITransferCommandGroup *group )
{
#if ( DEVICE_IMPL_VULKAN )
	for( size_t i = 0; i < RI_RESOURCE_MAX_SETS; i++ ) {
		for( size_t j = 0; j < (size_t)arrlen( group->temporary_buffers[i] ); j++ ) {
			vkDestroyBuffer( device->vk.device, group->temporary_buffers[i][j].vk.buffer, NULL );
			vmaFreeMemory( device->vk.vmaAllocator, group->temporary_buffers[i][j].vk.allocation );
		}
		arrfree( group->temporary_buffers[i] );

		vkFreeCommandBuffers( device->vk.device, group->cmd_pool[i].vk.pool, 1, &group->cmd[i].vk.cmd );
		vkDestroyCommandPool( device->vk.device, group->cmd_pool[i].vk.pool, NULL );

		vmaDestroyBuffer( device->vk.vmaAllocator, group->staging_buffer[i].vk.buffer, group->staging_buffer[i].vk.allocation );

		vkDestroyFence( device->vk.device, group->vk.fences[i], NULL );
		vkDestroySemaphore( device->vk.device, group->vk.semaphores[i], NULL );
	}
#endif
#if ( DEVICE_IMPL_MTL )
	for( size_t i = 0; i < RI_RESOURCE_MAX_SETS; i++ ) {
		for( size_t j = 0; j < (size_t)arrlen( group->temporary_buffers[i] ); j++ )
			group->temporary_buffers[i][j].dispose( device );
		arrfree( group->temporary_buffers[i] );

		group->staging_buffer[i].dispose( device );

		if( group->cmd[i].mtl.cmd ) {
			group->cmd[i].mtl.cmd->release();
			group->cmd[i].mtl.cmd = nullptr;
		}
		if( group->mtl.events[i] ) {
			group->mtl.events[i]->release();
			group->mtl.events[i] = nullptr;
		}
	}
#endif
}

static bool __AllocateFromStageBuffer( struct RIDevice *device, struct RITransferCommandGroup *group, size_t size, size_t alignment, struct RIMappedMemoryRange *out )
{
	(void)device;
	const size_t alignedSize = ALIGN_TO( size, alignment );
	const size_t alignedOffset = ALIGN_TO( group->staging_buffer_offset, alignment );

	if( alignedOffset >= RI_RESOURCE_STAGE_BUFFER_SIZE )
		return false;
	if( alignedSize > RI_RESOURCE_STAGE_BUFFER_SIZE - alignedOffset )
		return false;

	// staging_buffer[set].mappedAddress is the CPU-write pointer (VMA pMappedData on
	// Vulkan, MTL Shared contents() on Metal); the RIBuffer handle carries the
	// matching vk.buffer / mtl.buffer for the recorded copy.
	const size_t set = group->active_set;
	out->offset = alignedOffset;
	out->size = alignedSize;
	out->data = (uint8_t *)group->staging_buffer[set].mappedAddress + alignedOffset;
	out->buffer = group->staging_buffer[set];

	group->staging_buffer_offset = alignedOffset + alignedSize;
	return true;
}

static void __AllocateTemporaryBuffer( struct RIDevice *device, struct RITransferCommandGroup *group, size_t size, struct RIMappedMemoryRange *out )
{
	RIBufferDesc desc = {};
	desc.size = size;
	desc.usage = RI_BUFFER_USAGE_TRANSFER_SRC | RI_BUFFER_USAGE_TRANSFER_DST;
	desc.location = RI_MEMORY_HOST_UPLOAD;
	struct RIBuffer tmp = RIBuffer::create( device, desc );

	// Tracked for lifetime / freeing in temporary_buffers; 'out->buffer' is a handle
	// alias used only to record the copy.
	arrpush( group->temporary_buffers[group->active_set], tmp );

	out->offset = 0;
	out->size = size;
	out->data = tmp.mappedAddress;
	out->buffer = tmp;
}

static void __ResolveStageMemory( struct RIDevice *device, struct RITransferCommandGroup *group, size_t size, size_t alignment, struct RIMappedMemoryRange *out )
{
	if( !__AllocateFromStageBuffer( device, group, size, alignment, out ) )
		__AllocateTemporaryBuffer( device, group, size, out );
}

void RI_InitResourceUploader( struct RIDevice *device, struct RIResourceUploader *res )
{
	assert( res );
	memset( res, 0, sizeof( *res ) );

	__InitTransferCommandGroup( device, &res->upload_resource, &device->queues[RI_QUEUE_GRAPHICS] );

	struct RIQueue *copyQueue = &device->queues[RI_QUEUE_COPY];
#if ( DEVICE_IMPL_VULKAN )
	if( copyQueue->vk.queue == VK_NULL_HANDLE )
		copyQueue = &device->queues[RI_QUEUE_GRAPHICS];
#endif
	__InitTransferCommandGroup( device, &res->copy_resource, copyQueue );
}

void RI_FreeResourceUploader( struct RIDevice *device, struct RIResourceUploader *res )
{
	assert( res );
	__FreeTransferCommandGroup( device, &res->upload_resource );
	__FreeTransferCommandGroup( device, &res->copy_resource );
	memset( res, 0, sizeof( *res ) );
}

void RI_ResourceBeginCopyBuffer( struct RIDevice *device, struct RIResourceUploader *res, struct RIResourceBufferTransaction *trans )
{
#if ( DEVICE_IMPL_VULKAN )
	if( device->renderer->is_target_selected( RI_DEVICE_API_VK ) ) {
		__AcquireCmd( device, &res->upload_resource );
		__ResolveStageMemory( device, &res->upload_resource, trans->size, 4, &trans->mapped );
		return;
	}
#endif
#if ( DEVICE_IMPL_MTL )
	if( device->renderer->is_target_selected( RI_DEVICE_API_MTL ) ) {
		// Opt-in UMA direct write: only safe when the caller guarantees no in-flight
		// frame reads 'target' (write-once / load-time). Otherwise stage + blit so the
		// copy stays ordered on the queue against the previous frame's reads.
		if( __IsUMADirect( device, trans ) ) {
			trans->mapped.offset = 0;
			trans->mapped.size = trans->size;
			trans->mapped.data = (uint8_t *)trans->target.mappedAddress + trans->offset;
			return;
		}
		__AcquireCmdMTL( device, &res->upload_resource );
		__ResolveStageMemory( device, &res->upload_resource, trans->size, 4, &trans->mapped );
		return;
	}
#endif
	assert( false && "unhandled backend" );
}

void RI_ResourceEndCopyBuffer( struct RIDevice *device, struct RIResourceUploader *res, struct RIResourceBufferTransaction *trans )
{
	(void)device;
#if ( DEVICE_IMPL_MTL )
	// Direct-write path wrote straight into the Shared target — nothing to copy.
	if( __IsUMADirect( device, trans ) )
		return;
#endif

	// Begin acquired the recording command buffer; record the copy on it. The
	// pre/post barriers are Vulkan-only — on Metal the blit is queue-ordered after
	// the previous frame's reads and hazard tracking serialises this frame's reads
	// after it (mirrors what the Vulkan barrier provides).
	struct RICmd *barrierCmd = &res->upload_resource.cmd[res->upload_resource.active_set];

#if ( DEVICE_IMPL_VULKAN )
	if( trans->currentState != RI_RESOURCE_STATE_COPY_DST ) {
		struct RIBufferBarrier pre_barrier = {};
		pre_barrier.buffer = &trans->target;
		pre_barrier.before = trans->currentState;
		pre_barrier.beforeStages = trans->currentStages;
		pre_barrier.after = RI_RESOURCE_STATE_COPY_DST;
		pre_barrier.afterStages = RI_STAGE_COPY;
		barrierCmd->vk_d3d12_bufferBarrier( pre_barrier );
	}
#endif

	barrierCmd->copyBuffer(device->renderer, &trans->mapped.buffer, trans->mapped.offset, &trans->target, trans->offset, trans->size );

#if ( DEVICE_IMPL_VULKAN )
	if( trans->postState != RI_RESOURCE_STATE_UNDEFINED && trans->postState != RI_RESOURCE_STATE_COPY_DST ) {
		struct RIBufferBarrier post_barrier = {};
		post_barrier.buffer = &trans->target;
		post_barrier.before = RI_RESOURCE_STATE_COPY_DST;
		post_barrier.beforeStages = RI_STAGE_COPY;
		post_barrier.after = trans->postState;
		post_barrier.afterStages = trans->postStages;
		barrierCmd->vk_d3d12_bufferBarrier( post_barrier );
	}
#endif
}

void RI_ResourceBeginCopyTexture( struct RIDevice *device, struct RIResourceUploader *res, struct RIResourceTextureTransaction *trans )
{
	const struct RIFormatProps *formatProps = GetRIFormatProps( trans->format );
	const uint64_t alignedRowPitch = ALIGN_TO( trans->rowPitch, device->physicalAdapter.uploadBufferTextureRowAlignment );
	const uint64_t alignedSlicePitch = (uint64_t)trans->sliceNum * alignedRowPitch;

	trans->alignRowPitch = (uint32_t)alignedRowPitch;
	trans->alignSlicePitch = (uint32_t)alignedSlicePitch;

	// bufferOffset must be a multiple of the texel block size and the device's
	// optimalBufferCopyOffsetAlignment (Vulkan spec, vkCmdCopyBufferToImage).
	size_t offsetAlign = device->physicalAdapter.uploadBufferOffsetAlignment;
	if( formatProps->stride > offsetAlign )
		offsetAlign = formatProps->stride;
	if( offsetAlign < 4 )
		offsetAlign = 4;

#if ( DEVICE_IMPL_VULKAN )
	if( device->renderer->is_target_selected( RI_DEVICE_API_VK ) ) {
		__AcquireCmd( device, &res->upload_resource );
		__ResolveStageMemory( device, &res->upload_resource, alignedSlicePitch, offsetAlign, &trans->mapped );
		return;
	}
#endif
#if ( DEVICE_IMPL_MTL )
	if( device->renderer->is_target_selected( RI_DEVICE_API_MTL ) ) {
		// Textures always stage on Metal (Private storage, swizzled layout) — no UMA
		// direct path.
		__AcquireCmdMTL( device, &res->upload_resource );
		__ResolveStageMemory( device, &res->upload_resource, alignedSlicePitch, offsetAlign, &trans->mapped );
		return;
	}
#endif
	assert( false && "unhandled backend" );
}

void RI_ResourceEndCopyTexture( struct RIDevice *device, struct RIResourceUploader *res, struct RIResourceTextureTransaction *trans )
{
	(void)device;
	const struct RIFormatProps *formatProps = GetRIFormatProps( trans->format );

	// Staging layout in both representations: Vulkan's VkBufferImageCopy takes
	// texel bufferRowLength/bufferImageHeight, Metal's blit takes raw byte pitches.
	// The CPU writes rows at alignRowPitch (== rowPitch on AMD/Intel where
	// optimalBufferCopyRowPitchAlignment == 1, but 256-aligned on NVIDIA); deriving
	// from the unaligned rowPitch caused texture corruption on NVIDIA.
	const uint32_t rowBlockNum = trans->alignRowPitch / formatProps->stride;
	const uint32_t sliceRowNum = trans->alignSlicePitch / trans->alignRowPitch;

	struct RIBufferTextureCopyDesc copy = {};
	copy.bufferOffset = trans->mapped.offset;
	copy.bufferRowLength = rowBlockNum * formatProps->blockWidth;
	copy.bufferImageHeight = sliceRowNum * formatProps->blockHeight;
	copy.bytesPerRow = trans->alignRowPitch;
	copy.bytesPerImage = trans->alignSlicePitch;
	copy.mipLevel = trans->mipOffset;
	copy.arrayLayer = trans->arrayOffset;
	copy.x = trans->x;
	copy.y = trans->y;
	copy.z = trans->z;
	copy.width = trans->width;
	copy.height = trans->height;
	copy.depth = trans->depth;

	// Begin acquired the recording command buffer. Barriers cover only the single
	// (mipOffset, arrayOffset) subresource and are Vulkan-only — Metal is
	// hazard-tracked and queue-ordered.
	struct RICmd *barrierCmd = &res->upload_resource.cmd[res->upload_resource.active_set];

#if ( DEVICE_IMPL_VULKAN )
	if( trans->currentState != RI_RESOURCE_STATE_COPY_DST ) {
		struct RITextureBarrier pre_barrier = {};
		pre_barrier.texture = &trans->target;
		pre_barrier.before = trans->currentState;
		pre_barrier.beforeStages = trans->currentStages;
		pre_barrier.after = RI_RESOURCE_STATE_COPY_DST;
		pre_barrier.afterStages = RI_STAGE_COPY;
		pre_barrier.baseMip = trans->mipOffset;
		pre_barrier.mipCount = 1;
		pre_barrier.baseLayer = trans->arrayOffset;
		pre_barrier.layerCount = 1;
		barrierCmd->vk_d3d12_textureBarrier( pre_barrier );
	}
#endif

	barrierCmd->copyBufferToTexture(device->renderer, &trans->mapped.buffer, &trans->target, copy );

#if ( DEVICE_IMPL_VULKAN )
	if( trans->postState != RI_RESOURCE_STATE_UNDEFINED && trans->postState != RI_RESOURCE_STATE_COPY_DST ) {
		struct RITextureBarrier post_barrier = {};
		post_barrier.texture = &trans->target;
		post_barrier.before = RI_RESOURCE_STATE_COPY_DST;
		post_barrier.beforeStages = RI_STAGE_COPY;
		post_barrier.after = trans->postState;
		post_barrier.afterStages = trans->postStages;
		post_barrier.baseMip = trans->mipOffset;
		post_barrier.mipCount = 1;
		post_barrier.baseLayer = trans->arrayOffset;
		post_barrier.layerCount = 1;
		barrierCmd->vk_d3d12_textureBarrier( post_barrier );
	}
#endif
}

#if ( DEVICE_IMPL_VULKAN )
struct RIResourceUploaderVKResult RI_VKFlushResourceUpdate( struct RIDevice *device, struct RIResourceUploader *res, size_t num_semaphores, VkSemaphoreSubmitInfo *wait_semaphore_info )
{
	struct RITransferCommandGroup *group = &res->upload_resource;
	const size_t active_set = group->active_set;

	// Nothing recorded — return current set's fence/semaphore so the caller can branch on signaled.
	if( !group->is_recording ) {
		struct RIResourceUploaderVKResult result = {};
		result.signaled = false;
		result.vk.fence = group->vk.fences[active_set];
		result.vk.semaphore = group->vk.semaphores[active_set];
		return result;
	}

	VK_WrapResult( vkEndCommandBuffer( group->cmd[active_set].vk.cmd ) );

	VkCommandBufferSubmitInfo cmdSubmit = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
	cmdSubmit.commandBuffer = group->cmd[active_set].vk.cmd;
	cmdSubmit.deviceMask = 0;

	VkSemaphoreSubmitInfo signalSem = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
	signalSem.semaphore = group->vk.semaphores[active_set];
	signalSem.value = 0; /* binary semaphore */
	signalSem.stageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
	signalSem.deviceIndex = 0;

	VkSubmitInfo2 submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
	submitInfo.commandBufferInfoCount = 1;
	submitInfo.pCommandBufferInfos = &cmdSubmit;
	submitInfo.waitSemaphoreInfoCount = (uint32_t)num_semaphores;
	submitInfo.pWaitSemaphoreInfos = wait_semaphore_info;
	submitInfo.signalSemaphoreInfoCount = 1;
	submitInfo.pSignalSemaphoreInfos = &signalSem;

	/* Fence must already be signalled before we reset it; assert to catch bugs. */
	assert( vkGetFenceStatus( device->vk.device, group->vk.fences[active_set] ) == VK_SUCCESS );
	VK_WrapResult( vkResetFences( device->vk.device, 1, &group->vk.fences[active_set] ) );
	VK_WrapResult( vkQueueSubmit2( group->queue->vk.queue, 1, &submitInfo, group->vk.fences[active_set] ) );

	group->active_set = ( active_set + 1 ) % RI_RESOURCE_MAX_SETS;
	group->is_recording = false;

	struct RIResourceUploaderVKResult result = {};
	result.signaled = true;
	result.vk.fence = group->vk.fences[active_set];
	result.vk.semaphore = group->vk.semaphores[active_set];
	return result;
}
#endif

#if ( DEVICE_IMPL_MTL )
struct RIResourceUploaderVKResult RI_MTLFlushResourceUpdate( struct RIDevice *device, struct RIResourceUploader *res )
{
	(void)device;
	struct RITransferCommandGroup *group = &res->upload_resource;
	const size_t active_set = group->active_set;

	struct RIResourceUploaderVKResult result = {};

	// Nothing recorded (e.g. only UMA direct writes this frame) — no command buffer
	// to commit; return the current set's event/value so callers can branch.
	if( !group->is_recording ) {
		result.signaled = false;
		result.mtl.event = group->mtl.events[active_set];
		result.mtl.value = group->mtl.values[active_set];
		return result;
	}

	struct RICmd *cmd = &group->cmd[active_set];
	cmd->mtl_encoderEnd(); // close the blit encoder before commit

	const uint64_t signalValue = ++group->mtl.values[active_set];
	cmd->mtl.cmd->encodeSignalEvent( group->mtl.events[active_set], signalValue );
	cmd->mtl.cmd->commit();
	cmd->mtl.cmd->release();
	cmd->mtl.cmd = nullptr;

	group->active_set = ( active_set + 1 ) % RI_RESOURCE_MAX_SETS;
	group->is_recording = false;

	result.signaled = true;
	result.mtl.event = group->mtl.events[active_set];
	result.mtl.value = group->mtl.values[active_set];
	return result;
}
#endif
