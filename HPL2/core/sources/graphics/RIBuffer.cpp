#include "graphics/RITypes.h"
#include "graphics/RIRenderer.h" // VK_ConfigureBufferQueueFamilies, RI_QUEUE_LEN
#include "graphics/RIVK.h"       // ri_vk_RIBufferUsageToVK
#include <cassert>

void RIBuffer::setDebugObjectName(struct RIDevice *device,
                                    const char *name) {
  assert(vk.buffer);
  if (vkSetDebugUtilsObjectNameEXT && vk.buffer) {
    VkDebugUtilsObjectNameInfoEXT nameInfo = {
        VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT, NULL,
        VK_OBJECT_TYPE_BUFFER, (uint64_t)vk.buffer, name};
    VK_WrapResult(vkSetDebugUtilsObjectNameEXT(device->vk.device, &nameInfo));
  }
}


void RIBuffer::dispose(struct RIDevice *device) {
#if (DEVICE_IMPL_VULKAN)
	if (device->renderer->is_target_selected(RI_DEVICE_API_VK)) {
		vkDestroyBuffer(device->vk.device, vk.buffer, NULL);
		vmaFreeMemory(device->vk.vmaAllocator, vk.allocation);
	}
#endif
}

struct RIBuffer RIBuffer::create(struct RIDevice *device,
                                 const struct RIBufferDesc &desc,
                                 std::optional<hash_t> hash) {
#if (DEVICE_IMPL_VULKAN)
  uint32_t queueFamilies[RI_QUEUE_LEN] = {0};
  VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  VK_ConfigureBufferQueueFamilies(&bci, device->queues, RI_QUEUE_LEN,
                                  queueFamilies, RI_QUEUE_LEN);
  bci.size = desc.size;
  bci.usage = ri_vk_RIBufferUsageToVK(desc.usage);

  VmaAllocationCreateInfo aci = {};
  if (desc.location == RI_MEMORY_HOST_UPLOAD) {
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
  } else {
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
  }

  VmaAllocationInfo allocationInfo = {};
  RIBuffer buf = {};
  if (desc.alignment > 0) {
    VK_WrapResult(vmaCreateBufferWithAlignment(
        device->vk.vmaAllocator, &bci, &aci, desc.alignment, &buf.vk.buffer,
        &buf.vk.allocation, &allocationInfo));
  } else {
    VK_WrapResult(vmaCreateBuffer(device->vk.vmaAllocator, &bci, &aci,
                                  &buf.vk.buffer, &buf.vk.allocation,
                                  &allocationInfo));
  }
  buf.mappedAddress = (desc.location == RI_MEMORY_HOST_UPLOAD)
                          ? allocationInfo.pMappedData
                          : nullptr;
  buf.cookie = hash.value_or(hash_random());
  return buf;
#else
  (void)device;
  (void)desc;
  assert(false && "unhandled backend");
  return RIBuffer{};
#endif
}

uint64_t RIBuffer::GetDeviceHandle(struct RIDevice *device) {
  if (vk.buffer == NULL)
    return 0;
  VkBufferDeviceAddressInfo info = {
      VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
  info.buffer = vk.buffer;
  uint64_t handle = vkGetBufferDeviceAddress(device->vk.device, &info);
  assert(handle > 0);
  return handle;
}
