#include "graphics/RIPogoBuffer.h"
#include "graphics/RIRenderer.h"
#include "graphics/RIVK.h"

#if ( DEVICE_IMPL_VULKAN )
#include <vk_mem_alloc.h>

VkImageMemoryBarrier2 VK_RI_PogoShaderMemoryBarrier2( VkImage image, bool initial )
{
	VkImageMemoryBarrier2 imageBarriers = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
	imageBarriers.srcStageMask = initial ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
	imageBarriers.srcAccessMask = initial ? VK_ACCESS_2_NONE : VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
	imageBarriers.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
	imageBarriers.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
	imageBarriers.oldLayout = initial ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	imageBarriers.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	imageBarriers.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	imageBarriers.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	imageBarriers.image = image;
	imageBarriers.subresourceRange = VkImageSubresourceRange{
		VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS,
	};
	return imageBarriers;
}

VkImageMemoryBarrier2 VK_RI_PogoAttachmentMemoryBarrier2( VkImage image, bool initial )
{
	VkImageMemoryBarrier2 imageBarriers = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
	imageBarriers.srcStageMask = initial ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
	imageBarriers.srcAccessMask = initial ? VK_ACCESS_2_NONE : VK_ACCESS_2_SHADER_READ_BIT;
	imageBarriers.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
	imageBarriers.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
	imageBarriers.oldLayout = initial ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	imageBarriers.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	imageBarriers.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	imageBarriers.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	imageBarriers.image = image;
	imageBarriers.subresourceRange = VkImageSubresourceRange{
		VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS,
	};
	return imageBarriers;
}
#endif

void RI_PogoBufferInit( struct RIDevice_s *device, struct RI_PogoBuffer *pogo, uint32_t width, uint32_t height, enum RI_Format_e format )
{
	pogo->attachmentIndex = 0;
#if ( DEVICE_IMPL_VULKAN )
	uint32_t queueFamilies[RI_QUEUE_LEN] = { 0 };

	VmaAllocationCreateInfo mem_reqs = { 0 };
	mem_reqs.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

	VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
	info.imageType = VK_IMAGE_TYPE_2D;
	info.extent.width = width;
	info.extent.height = height;
	info.extent.depth = 1;
	info.mipLevels = 1;
	info.arrayLayers = 1;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.pQueueFamilyIndices = queueFamilies;
	VK_ConfigureImageQueueFamilies( &info, device->queues, RI_QUEUE_LEN, queueFamilies, RI_QUEUE_LEN );
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	info.format = RIFormatToVK( format );

	for( size_t p = 0; p < 2; p++ ) {
		VK_WrapResult( vmaCreateImage( device->vk.vmaAllocator, &info, &mem_reqs, &pogo->textures[p].vk.image, &pogo->textures[p].vk.allocation, NULL ) );

		VkImageViewUsageCreateInfo usageInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO };
		usageInfo.usage = ( VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT );

		VkImageViewCreateInfo createInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		createInfo.pNext = &usageInfo;
		createInfo.subresourceRange = VkImageSubresourceRange{
			VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1,
		};
		createInfo.image = pogo->textures[p].vk.image;
		createInfo.format = RIFormatToVK( format );
		createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;

		pogo->pogoAttachment[p].flags |= RI_VK_DESC_OWN_IMAGE_VIEW;
		pogo->pogoAttachment[p].texture = &pogo->textures[p];
		pogo->pogoAttachment[p].vk.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		pogo->pogoAttachment[p].vk.image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		VK_WrapResult( vkCreateImageView( device->vk.device, &createInfo, NULL, &pogo->pogoAttachment[p].vk.image.imageView ) );
		RIFinalizeDescriptor( device, &pogo->pogoAttachment[p] );
	}
#endif
}

void RI_PogoBufferDestroy( struct RIDevice_s *device, struct RI_PogoBuffer *pogo )
{
#if ( DEVICE_IMPL_VULKAN )
	for( size_t p = 0; p < 2; p++ ) {
		vkDestroyImageView( device->vk.device, pogo->pogoAttachment[p].vk.image.imageView, NULL );
		vmaDestroyImage( device->vk.vmaAllocator, pogo->textures[p].vk.image, pogo->textures[p].vk.allocation );
		pogo->pogoAttachment[p] = RIDescriptor_s{};
		pogo->textures[p] = RITexture_s{};
	}
#endif
}

void RI_PogoBufferToggle( struct RIDevice_s *device, struct RI_PogoBuffer *pogo, struct RICmd_s *handle )
{
#if ( DEVICE_IMPL_VULKAN )
	VkImageMemoryBarrier2 imageBarriers[2] = {};
	imageBarriers[0] = VK_RI_PogoShaderMemoryBarrier2( pogo->textures[pogo->attachmentIndex].vk.image, false );
	pogo->attachmentIndex = ( ( pogo->attachmentIndex + 1 ) % 2 );
	imageBarriers[1] = VK_RI_PogoAttachmentMemoryBarrier2( pogo->textures[pogo->attachmentIndex].vk.image, false );

	VkDependencyInfo dependencyInfo = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
	dependencyInfo.imageMemoryBarrierCount = 2;
	dependencyInfo.pImageMemoryBarriers = imageBarriers;
	vkCmdPipelineBarrier2( handle->vk.cmd, &dependencyInfo );
#endif
}
