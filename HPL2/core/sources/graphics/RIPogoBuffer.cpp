#include "graphics/RIPogoBuffer.h"
#include "graphics/RIRenderer.h"
#include "graphics/RIVK.h"

#if ( DEVICE_IMPL_VULKAN )
#include <vk_mem_alloc.h>
#endif

void RI_PogoBufferInit( struct RIDevice *device, struct RI_PogoBuffer *pogo, uint32_t width, uint32_t height, enum RI_Format_e format )
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
	// STORAGE_BIT lets the SurfelGI composite (a compute pass) write the pogo
	// attach via an RWTexture2D; still color-attachment + sampled for the raster
	// passes and post-effect chain. TRANSFER_SRC/DST back the guard-band crop
	// blit (overscan render-pogo center → authored pogo) at the end of Draw.
	info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
	             VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
	             VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	info.format = RIFormatToVK( format );

	for( size_t p = 0; p < 2; p++ ) {
		VK_WrapResult( vmaCreateImage( device->vk.vmaAllocator, &info, &mem_reqs, &pogo->textures[p].vk.image, &pogo->textures[p].vk.allocation, NULL ) );

		VkImageViewUsageCreateInfo usageInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO };
		usageInfo.usage = ( VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
		                    VK_IMAGE_USAGE_STORAGE_BIT );

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
		pogo->pogoAttachment[p].finalize( device );
	}
#endif
}

void RI_PogoBufferDestroy( struct RIDevice *device, struct RI_PogoBuffer *pogo )
{
#if ( DEVICE_IMPL_VULKAN )
	for( size_t p = 0; p < 2; p++ ) {
		vkDestroyImageView( device->vk.device, pogo->pogoAttachment[p].vk.image.imageView, NULL );
		vmaDestroyImage( device->vk.vmaAllocator, pogo->textures[p].vk.image, pogo->textures[p].vk.allocation );
		pogo->pogoAttachment[p] = RIDescriptor{};
		pogo->textures[p] = RITexture{};
	}
#endif
}

void RI_PogoBufferToggle( struct RIDevice *device, struct RI_PogoBuffer *pogo, struct RICmd *handle )
{
	struct RITextureBarrier barriers[2] = {};
	barriers[0] = RI_PogoShaderBarrier( &pogo->textures[pogo->attachmentIndex], false );
	pogo->attachmentIndex = ( ( pogo->attachmentIndex + 1 ) % 2 );
	barriers[1] = RI_PogoAttachmentBarrier( &pogo->textures[pogo->attachmentIndex], false );
	handle->textureBarriers<2>( 2, barriers );
}
