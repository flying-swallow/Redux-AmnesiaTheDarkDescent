#include "graphics/RINulTexture.h"
#include "graphics/RIRenderer.h"
#include "graphics/RITypes.h"
#include <vulkan/vulkan_core.h>

bool RINulTexture::Create2DNulWhite(struct RICmd_s* cmd, struct RIDevice_s* device, struct RINulTexture* tex) {
  memset(tex, 0, sizeof(RINulTexture));
  VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.mipLevels =  1;
  info.arrayLayers = 1;
  info.imageType = VK_IMAGE_TYPE_2D;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.format = VK_FORMAT_R8G8B8A8_UNORM;
  info.extent.width = 1;
  info.extent.height = 1;
  info.extent.depth = 1;
  info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  uint32_t queueFamilies[RI_QUEUE_LEN] = {0};
  info.pQueueFamilyIndices = queueFamilies;
  VK_ConfigureImageQueueFamilies(&info, device->queues, RI_QUEUE_LEN,
                                queueFamilies, RI_QUEUE_LEN);
  info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT |
              VK_IMAGE_CREATE_EXTENDED_USAGE_BIT; // typeless
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VkImageViewUsageCreateInfo usageInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO };
	VkImageSubresourceRange subresource = {
		VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1,
	};
	VkImageViewCreateInfo createInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
  VmaAllocationCreateInfo memReqs = {0};
  memReqs.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
  memReqs.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

	if(!VK_WrapResult(vmaCreateImage(device->vk.vmaAllocator, &info, &memReqs, &tex->tex.vk.image, &tex->vk.vmaAlloc, NULL))) {
	  hpl::FatalError("Failed to create white texture image!\n");
	  return false;
	}
	void* pData = NULL;
  vmaMapMemory(device->vk.vmaAllocator, tex->vk.vmaAlloc, &pData);
  uint8_t white[4] = { 255, 255, 255, 255 };
  memcpy(((uint8_t*)pData), white, sizeof(white));
  vmaUnmapMemory(device->vk.vmaAllocator, tex->vk.vmaAlloc);

	usageInfo.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
	createInfo.pNext = &usageInfo;
	createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	createInfo.format = info.format;
	createInfo.subresourceRange = subresource;
	createInfo.image = tex->tex.vk.image;
	
	tex->binding.flags |= RI_VK_DESC_OWN_IMAGE_VIEW;
	tex->binding.texture = &tex->tex;
	tex->binding.vk.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	tex->binding.vk.image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	VK_WrapResult( vkCreateImageView( device->vk.device, &createInfo, NULL, &tex->binding.vk.image.imageView ) );
	RIFinalizeDescriptor( device, &tex->binding );

  {
			VkImageMemoryBarrier2 imageBarriers[1] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
			imageBarriers[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			imageBarriers[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			imageBarriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			imageBarriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			imageBarriers[0].image = tex->tex.vk.image;
			imageBarriers[0].subresourceRange = VkImageSubresourceRange{
				VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS,
			};
			VkDependencyInfo dependencyInfo = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
			dependencyInfo.imageMemoryBarrierCount = 1;
			dependencyInfo.pImageMemoryBarriers = imageBarriers;
			vkCmdPipelineBarrier2( cmd->vk.cmd, &dependencyInfo );
  }

	return true;
}
