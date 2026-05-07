#include "graphics/RINulTexture.h"
#include "graphics/RIRenderer.h"
#include "graphics/RITypes.h"
#include "graphics/RIVK.h"

bool RINulTexture::Create2DNulWhite(struct RICmd_s* cmd, struct RIDevice_s* device, struct RINulTexture* tex)
{
    if (!cmd || !device || !tex) {
        hpl::Error("Create2DNulWhite: null argument!\n");
        return false;
    }

    if (cmd->vk.cmd == VK_NULL_HANDLE) {
        hpl::Error("Create2DNulWhite: null command buffer!\n");
        return false;
    }

    if (device->vk.device == VK_NULL_HANDLE) {
        hpl::Error("Create2DNulWhite: null VkDevice!\n");
        return false;
    }

    if (device->vk.vmaAllocator == VK_NULL_HANDLE) {
        hpl::Error("Create2DNulWhite: null VMA allocator!\n");
        return false;
    }

    memset(tex, 0, sizeof(RINulTexture));

    tex->tex.vk.image = VK_NULL_HANDLE;
    tex->binding.vk.image.imageView = VK_NULL_HANDLE;
    tex->vk.vmaAlloc = VK_NULL_HANDLE;
    tex->vk.uploadStagingBuffer = VK_NULL_HANDLE;
    tex->vk.uploadStagingAlloc = VK_NULL_HANDLE;

    // Create the final GPU image
    VkImageCreateInfo imageInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.extent.width = 1;
    imageInfo.extent.height = 1;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.flags = 0;

    uint32_t queueFamilies[RI_QUEUE_LEN] = { 0 };
    imageInfo.pQueueFamilyIndices = queueFamilies;

    VK_ConfigureImageQueueFamilies(&imageInfo, device->queues, RI_QUEUE_LEN, queueFamilies, RI_QUEUE_LEN);

    VmaAllocationCreateInfo imageAllocInfo = {};
    imageAllocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    VkResult result = vmaCreateImage(device->vk.vmaAllocator, &imageInfo, &imageAllocInfo, &tex->tex.vk.image, &tex->vk.vmaAlloc, nullptr);

    if (!VK_WrapResult(result)) {
        hpl::Error("Create2DNulWhite: failed to create GPU image!\n");
        return false;
    }

    // Create a tiny host-visible staging buffer
    const uint8_t whitePixel[4] = { 255, 255, 255, 255 };

    VkBufferCreateInfo bufferInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bufferInfo.size = sizeof(whitePixel);
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo bufferAllocInfo = {};
    bufferAllocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    bufferAllocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VmaAllocationInfo stagingAllocInfo = {};

    result = vmaCreateBuffer(device->vk.vmaAllocator, &bufferInfo, &bufferAllocInfo, &tex->vk.uploadStagingBuffer, &tex->vk.uploadStagingAlloc, &stagingAllocInfo);

    if (!VK_WrapResult(result)) {
        hpl::Error("Create2DNulWhite: failed to create staging buffer!\n");

        if (tex->tex.vk.image != VK_NULL_HANDLE)
            vmaDestroyImage(device->vk.vmaAllocator, tex->tex.vk.image, tex->vk.vmaAlloc);

        memset(tex, 0, sizeof(RINulTexture));
        return false;
    }

    if (!stagingAllocInfo.pMappedData) {
        hpl::Error("Create2DNulWhite: staging buffer is not mapped!\n");

        vmaDestroyBuffer(device->vk.vmaAllocator, tex->vk.uploadStagingBuffer, tex->vk.uploadStagingAlloc);
        vmaDestroyImage(device->vk.vmaAllocator, tex->tex.vk.image, tex->vk.vmaAlloc);
        memset(tex, 0, sizeof(RINulTexture));
        return false;
    }

    memcpy(stagingAllocInfo.pMappedData, whitePixel, sizeof(whitePixel));
    vmaFlushAllocation(device->vk.vmaAllocator, tex->vk.uploadStagingAlloc, 0, VK_WHOLE_SIZE);

    // Transition image: UNDEFINED -> TRANSFER_DST_OPTIMAL
    VkImageSubresourceRange colorRange = {};
    colorRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    colorRange.baseMipLevel = 0;
    colorRange.levelCount = 1;
    colorRange.baseArrayLayer = 0;
    colorRange.layerCount = 1;

    VkImageMemoryBarrier2 toTransfer = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    toTransfer.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    toTransfer.srcAccessMask = 0;
    toTransfer.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    toTransfer.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = tex->tex.vk.image;
    toTransfer.subresourceRange = colorRange;

    VkDependencyInfo depToTransfer = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    depToTransfer.imageMemoryBarrierCount = 1;
    depToTransfer.pImageMemoryBarriers = &toTransfer;

    vkCmdPipelineBarrier2(cmd->vk.cmd, &depToTransfer);

    // Copy staging buffer -> image
    VkBufferImageCopy copyRegion = {};
    copyRegion.bufferOffset = 0;
    copyRegion.bufferRowLength = 0;
    copyRegion.bufferImageHeight = 0;
    copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyRegion.imageSubresource.mipLevel = 0;
    copyRegion.imageSubresource.baseArrayLayer = 0;
    copyRegion.imageSubresource.layerCount = 1;
    copyRegion.imageOffset = { 0, 0, 0 };
    copyRegion.imageExtent = { 1, 1, 1 };

    vkCmdCopyBufferToImage(cmd->vk.cmd, tex->vk.uploadStagingBuffer, tex->tex.vk.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

    // Transition image: TRANSFER_DST_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL
    VkImageMemoryBarrier2 toShaderRead = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    toShaderRead.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    toShaderRead.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    toShaderRead.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    toShaderRead.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    toShaderRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toShaderRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toShaderRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toShaderRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toShaderRead.image = tex->tex.vk.image;
    toShaderRead.subresourceRange = colorRange;

    VkDependencyInfo depToShaderRead = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    depToShaderRead.imageMemoryBarrierCount = 1;
    depToShaderRead.pImageMemoryBarriers = &toShaderRead;

    vkCmdPipelineBarrier2(cmd->vk.cmd, &depToShaderRead);

    // Create image view
    VkImageViewCreateInfo viewInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    viewInfo.image = tex->tex.vk.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = imageInfo.format;
    viewInfo.subresourceRange = colorRange;

    result = vkCreateImageView(device->vk.device, &viewInfo, nullptr, &tex->binding.vk.image.imageView);

    if (!VK_WrapResult(result)) {
        hpl::Error("Create2DNulWhite: failed to create image view!\n");

        if (tex->vk.uploadStagingBuffer != VK_NULL_HANDLE)
            vmaDestroyBuffer(device->vk.vmaAllocator, tex->vk.uploadStagingBuffer, tex->vk.uploadStagingAlloc);

        if (tex->tex.vk.image != VK_NULL_HANDLE)
            vmaDestroyImage(device->vk.vmaAllocator, tex->tex.vk.image, tex->vk.vmaAlloc);

        memset(tex, 0, sizeof(RINulTexture));
        return false;
    }

    // Fill descriptor binding
    tex->binding.flags |= RI_VK_DESC_OWN_IMAGE_VIEW;
    tex->binding.texture = &tex->tex;
    tex->binding.vk.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    tex->binding.vk.image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RIFinalizeDescriptor(device, &tex->binding);

    return true;
}

void RINulTexture::Free2DNulWhiteUploadScratch(struct RIDevice_s* device, struct RINulTexture* tex)
{
    if (!device || !tex) return;

    if (device->vk.vmaAllocator == VK_NULL_HANDLE) return;

    if (tex->vk.uploadStagingBuffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(device->vk.vmaAllocator, tex->vk.uploadStagingBuffer, tex->vk.uploadStagingAlloc);
        tex->vk.uploadStagingBuffer = VK_NULL_HANDLE;
        tex->vk.uploadStagingAlloc = VK_NULL_HANDLE;
    }
}
