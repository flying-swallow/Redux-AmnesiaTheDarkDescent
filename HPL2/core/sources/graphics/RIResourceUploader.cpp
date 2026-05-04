#include "graphics/RIResourceUploader.h"
#include "graphics/RIFormat.h"

#include <system/Types.h>
#include <system/stb_ds.h>

#include <cassert>
#include <cstring>

// Wait for the active set's fence (signalled means the GPU is done with the
// previous use), free overflow temporaries, reset the pool, begin the cmd
// buffer. No-op if the group is already recording.
static VkCommandBuffer __AcquireCmd( struct RIDevice_s *device, struct RITransferCommandGroup_s *group )
{
#if ( DEVICE_IMPL_VULKAN )
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
#else
	return VK_NULL_HANDLE;
#endif
}

static void __InitTransferCommandGroup( struct RIDevice_s *device, struct RITransferCommandGroup_s *group, struct RIQueue_s *queue )
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
			VmaAllocationCreateInfo allocInfo = { 0 };
			allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
			allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;

			VkBufferCreateInfo bufInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
			bufInfo.size = RI_RESOURCE_STAGE_BUFFER_SIZE;
			bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

			VmaAllocationInfo vmaInfo = { 0 };
			VK_WrapResult( vmaCreateBuffer( device->vk.vmaAllocator, &bufInfo, &allocInfo, &group->staging_buffer[i].vk.buffer, &group->staging_buffer[i].vk.allocation, &vmaInfo ) );
			group->staging_buffer[i].mappedAddress = vmaInfo.pMappedData;
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
}

static void __FreeTransferCommandGroup( struct RIDevice_s *device, struct RITransferCommandGroup_s *group )
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
}

static bool __AllocateFromStageBuffer( struct RIDevice_s *device, struct RITransferCommandGroup_s *group, size_t size, size_t alignment, struct RIMappedMemoryRange *out )
{
#if ( DEVICE_IMPL_VULKAN )
	const size_t alignedSize = ALIGN_TO( size, alignment );
	const size_t alignedOffset = ALIGN_TO( group->staging_buffer_offset, alignment );

	if( alignedOffset >= RI_RESOURCE_STAGE_BUFFER_SIZE )
		return false;
	if( alignedSize > RI_RESOURCE_STAGE_BUFFER_SIZE - alignedOffset )
		return false;

	const size_t set = group->active_set;
	out->offset = alignedOffset;
	out->size = alignedSize;
	out->data = (uint8_t *)group->staging_buffer[set].mappedAddress + alignedOffset;
	out->buffer = group->staging_buffer[set].vk.buffer;
	out->alloc = NULL; // belongs to the persistent staging buffer

	group->staging_buffer_offset = alignedOffset + alignedSize;
	return true;
#else
	return false;
#endif
}

static void __AllocateTemporaryBuffer( struct RIDevice_s *device, struct RITransferCommandGroup_s *group, size_t size, struct RIMappedMemoryRange *out )
{
#if ( DEVICE_IMPL_VULKAN )
	VmaAllocationCreateInfo allocInfo = { 0 };
	allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
	allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;

	VkBufferCreateInfo bufInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	bufInfo.size = size;
	bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

	struct RIBuffer_s tmp = { 0 };
	VmaAllocationInfo vmaInfo = { 0 };
	VK_WrapResult( vmaCreateBuffer( device->vk.vmaAllocator, &bufInfo, &allocInfo, &tmp.vk.buffer, &tmp.vk.allocation, &vmaInfo ) );

	arrpush( group->temporary_buffers[group->active_set], tmp );

	out->offset = 0;
	out->size = size;
	out->data = vmaInfo.pMappedData;
	out->buffer = tmp.vk.buffer;
	out->alloc = tmp.vk.allocation;
#endif
}

static void __ResolveStageMemory( struct RIDevice_s *device, struct RITransferCommandGroup_s *group, size_t size, struct RIMappedMemoryRange *out )
{
	if( !__AllocateFromStageBuffer( device, group, size, 4, out ) )
		__AllocateTemporaryBuffer( device, group, size, out );
}

void RI_InitResourceUploader( struct RIDevice_s *device, struct RIResourceUploader_s *res )
{
	assert( res );
	memset( res, 0, sizeof( *res ) );

	__InitTransferCommandGroup( device, &res->upload_resource, &device->queues[RI_QUEUE_GRAPHICS] );

	struct RIQueue_s *copyQueue = &device->queues[RI_QUEUE_COPY];
#if ( DEVICE_IMPL_VULKAN )
	if( copyQueue->vk.queue == VK_NULL_HANDLE )
		copyQueue = &device->queues[RI_QUEUE_GRAPHICS];
#endif
	__InitTransferCommandGroup( device, &res->copy_resource, copyQueue );
}

void RI_FreeResourceUploader( struct RIDevice_s *device, struct RIResourceUploader_s *res )
{
	assert( res );
	__FreeTransferCommandGroup( device, &res->upload_resource );
	__FreeTransferCommandGroup( device, &res->copy_resource );
	memset( res, 0, sizeof( *res ) );
}

void RI_ResourceBeginCopyBuffer( struct RIDevice_s *device, struct RIResourceUploader_s *res, struct RIResourceBufferTransaction_s *trans )
{
	__AcquireCmd( device, &res->upload_resource );
	__ResolveStageMemory( device, &res->upload_resource, trans->size, &trans->mapped );
}

void RI_ResourceEndCopyBuffer( struct RIDevice_s *device, struct RIResourceUploader_s *res, struct RIResourceBufferTransaction_s *trans )
{
#if ( DEVICE_IMPL_VULKAN )
	VkBufferCopy region = { 0 };
	region.srcOffset = trans->mapped.offset;
	region.dstOffset = trans->offset;
	region.size = trans->size;

	VkCommandBuffer cmd = __AcquireCmd( device, &res->upload_resource );

	if( trans->vk.current_stage != VK_PIPELINE_STAGE_2_COPY_BIT || trans->vk.current_access != VK_ACCESS_2_TRANSFER_WRITE_BIT ) {
		VkBufferMemoryBarrier2 pre_barrier = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 };
		pre_barrier.srcStageMask = trans->vk.current_stage;
		pre_barrier.srcAccessMask = trans->vk.current_access;
		pre_barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
		pre_barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
		pre_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		pre_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		pre_barrier.buffer = trans->target.vk.buffer;
		pre_barrier.offset = 0;
		pre_barrier.size = VK_WHOLE_SIZE;

		VkDependencyInfo pre_dependency_info = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
		pre_dependency_info.bufferMemoryBarrierCount = 1;
		pre_dependency_info.pBufferMemoryBarriers = &pre_barrier;
		vkCmdPipelineBarrier2( cmd, &pre_dependency_info );
	}

	vkCmdCopyBuffer( cmd, trans->mapped.buffer, trans->target.vk.buffer, 1, &region );

	if( trans->vk.post_stage != 0 &&
	    ( trans->vk.post_stage != VK_PIPELINE_STAGE_2_COPY_BIT || trans->vk.post_access != VK_ACCESS_2_TRANSFER_WRITE_BIT ) ) {
		VkBufferMemoryBarrier2 post_barrier = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 };
		post_barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
		post_barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
		post_barrier.dstStageMask = trans->vk.post_stage;
		post_barrier.dstAccessMask = trans->vk.post_access;
		post_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		post_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		post_barrier.buffer = trans->target.vk.buffer;
		post_barrier.offset = 0;
		post_barrier.size = VK_WHOLE_SIZE;

		VkDependencyInfo post_dependency_info = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
		post_dependency_info.bufferMemoryBarrierCount = 1;
		post_dependency_info.pBufferMemoryBarriers = &post_barrier;
		vkCmdPipelineBarrier2( cmd, &post_dependency_info );
	}
#endif
}

void RI_ResourceBeginCopyTexture( struct RIDevice_s *device, struct RIResourceUploader_s *res, struct RIResourceTextureTransaction_s *trans )
{
	const uint64_t alignedRowPitch = ALIGN_TO( trans->rowPitch, device->physicalAdapter.uploadBufferTextureRowAlignment );
	const uint64_t alignedSlicePitch = ALIGN_TO( (uint64_t)trans->sliceNum * alignedRowPitch, device->physicalAdapter.uploadBufferTextureSliceAlignment );

	trans->alignRowPitch = (uint32_t)alignedRowPitch;
	trans->alignSlicePitch = (uint32_t)alignedSlicePitch;

	__AcquireCmd( device, &res->upload_resource );
	__ResolveStageMemory( device, &res->upload_resource, alignedSlicePitch, &trans->mapped );
}

void RI_ResourceEndCopyTexture( struct RIDevice_s *device, struct RIResourceUploader_s *res, struct RIResourceTextureTransaction_s *trans )
{
#if ( DEVICE_IMPL_VULKAN )
	const struct RIFormatProps_s *formatProps = GetRIFormatProps( trans->format );

	const uint32_t rowBlockNum = trans->rowPitch / formatProps->stride;
	const uint32_t bufferRowLength = rowBlockNum * formatProps->blockWidth;
	const uint32_t sliceRowNum = trans->alignSlicePitch / trans->rowPitch;
	const uint32_t bufferImageHeight = sliceRowNum * formatProps->blockWidth;

	VkBufferImageCopy region = { 0 };
	region.bufferOffset = trans->mapped.offset;
	region.bufferRowLength = bufferRowLength;
	region.bufferImageHeight = bufferImageHeight;
	region.imageOffset.x = trans->x;
	region.imageOffset.y = trans->y;
	region.imageOffset.z = trans->z;
	region.imageExtent.width = trans->width;
	region.imageExtent.height = trans->height;
	region.imageExtent.depth = trans->depth;
	region.imageSubresource.mipLevel = trans->mipOffset;
	region.imageSubresource.baseArrayLayer = trans->arrayOffset;
	region.imageSubresource.layerCount = 1;
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;

	VkCommandBuffer cmd = __AcquireCmd( device, &res->upload_resource );

	if( trans->vk.current_stage != VK_PIPELINE_STAGE_2_COPY_BIT || trans->vk.current_access != VK_ACCESS_2_TRANSFER_WRITE_BIT ) {
		VkImageMemoryBarrier2 pre_barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
		pre_barrier.srcStageMask = trans->vk.current_stage;
		pre_barrier.srcAccessMask = trans->vk.current_access;
		pre_barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
		pre_barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
		pre_barrier.oldLayout = trans->vk.current_layout;
		pre_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		pre_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		pre_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		pre_barrier.image = trans->target.vk.image;
		pre_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		pre_barrier.subresourceRange.baseMipLevel = trans->mipOffset;
		pre_barrier.subresourceRange.levelCount = 1;
		pre_barrier.subresourceRange.baseArrayLayer = trans->arrayOffset;
		pre_barrier.subresourceRange.layerCount = 1;

		VkDependencyInfo pre_dependency_info = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
		pre_dependency_info.imageMemoryBarrierCount = 1;
		pre_dependency_info.pImageMemoryBarriers = &pre_barrier;
		vkCmdPipelineBarrier2( cmd, &pre_dependency_info );
	}

	vkCmdCopyBufferToImage( cmd, trans->mapped.buffer, trans->target.vk.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region );

	if( trans->vk.post_stage != 0 &&
	    ( trans->vk.post_stage != VK_PIPELINE_STAGE_2_COPY_BIT || trans->vk.post_access != VK_ACCESS_2_TRANSFER_WRITE_BIT ) ) {
		VkImageMemoryBarrier2 post_barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
		post_barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
		post_barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
		post_barrier.dstStageMask = trans->vk.post_stage;
		post_barrier.dstAccessMask = trans->vk.post_access;
		post_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		post_barrier.newLayout = trans->vk.post_layout != VK_IMAGE_LAYOUT_UNDEFINED ? trans->vk.post_layout : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		post_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		post_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		post_barrier.image = trans->target.vk.image;
		post_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		post_barrier.subresourceRange.baseMipLevel = trans->mipOffset;
		post_barrier.subresourceRange.levelCount = 1;
		post_barrier.subresourceRange.baseArrayLayer = trans->arrayOffset;
		post_barrier.subresourceRange.layerCount = 1;

		VkDependencyInfo post_dependency_info = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
		post_dependency_info.imageMemoryBarrierCount = 1;
		post_dependency_info.pImageMemoryBarriers = &post_barrier;
		vkCmdPipelineBarrier2( cmd, &post_dependency_info );
	}
#endif
}

#if ( DEVICE_IMPL_VULKAN )
struct RIResourceUploaderVKResult_s RI_VKFlushResourceUpdate( struct RIDevice_s *device, struct RIResourceUploader_s *res, size_t num_semaphores, VkSemaphoreSubmitInfo *wait_semaphore_info )
{
	struct RITransferCommandGroup_s *group = &res->upload_resource;
	const size_t active_set = group->active_set;

	// Nothing recorded — return current set's fence/semaphore so the caller can branch on signaled.
	if( !group->is_recording ) {
		struct RIResourceUploaderVKResult_s result = {};
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
	signalSem.value = 0; // binary semaphore
	signalSem.stageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
	signalSem.deviceIndex = 0;

	VkSubmitInfo2 submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
	submitInfo.commandBufferInfoCount = 1;
	submitInfo.pCommandBufferInfos = &cmdSubmit;
	submitInfo.waitSemaphoreInfoCount = (uint32_t)num_semaphores;
	submitInfo.pWaitSemaphoreInfos = wait_semaphore_info;
	submitInfo.signalSemaphoreInfoCount = 1;
	submitInfo.pSignalSemaphoreInfos = &signalSem;

	assert( vkGetFenceStatus( device->vk.device, group->vk.fences[active_set] ) == VK_SUCCESS );
	VK_WrapResult( vkResetFences( device->vk.device, 1, &group->vk.fences[active_set] ) );
	VK_WrapResult( vkQueueSubmit2( group->queue->vk.queue, 1, &submitInfo, group->vk.fences[active_set] ) );

	group->active_set = ( active_set + 1 ) % RI_RESOURCE_MAX_SETS;
	group->is_recording = false;

	struct RIResourceUploaderVKResult_s result = {};
	result.signaled = true;
	result.vk.fence = group->vk.fences[active_set];
	result.vk.semaphore = group->vk.semaphores[active_set];
	return result;
}
#endif
