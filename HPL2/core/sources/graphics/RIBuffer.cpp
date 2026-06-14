#include "graphics/RITypes.h"
#include <cassert>

#if (DEVICE_IMPL_VULKAN)
#include "graphics/RIRenderer.h" // VK_ConfigureBufferQueueFamilies, RI_QUEUE_LEN
#include "graphics/RIVK.h"       // ri_vk_RIBufferUsageToVK
#endif

#if (DEVICE_IMPL_MTL)
#include "graphics/RIMTL.h"
#endif

void RIBuffer::setDebugObjectName(struct RIDevice *device,
                                    const char *name) {
#if (DEVICE_IMPL_VULKAN)
  if (device->renderer->is_target_selected(RI_DEVICE_API_VK)) {
    assert(vk.buffer);
    if (vkSetDebugUtilsObjectNameEXT && vk.buffer) {
      VkDebugUtilsObjectNameInfoEXT nameInfo = {
          VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT, NULL,
          VK_OBJECT_TYPE_BUFFER, (uint64_t)vk.buffer, name};
      VK_WrapResult(vkSetDebugUtilsObjectNameEXT(device->vk.device, &nameInfo));
    }
    return;
  }
#endif
#if (DEVICE_IMPL_MTL)
  if (device->renderer->is_target_selected(RI_DEVICE_API_MTL)) {
    if (mtl.buffer)
      mtl.buffer->setLabel(NS::String::string(name, NS::UTF8StringEncoding));
    return;
  }
#endif
  assert(false && "unhandled backend");
}


void RIBuffer::dispose(struct RIDevice *device) {
#if (DEVICE_IMPL_VULKAN)
  if (device->renderer->is_target_selected(RI_DEVICE_API_VK)) {
    vkDestroyBuffer(device->vk.device, vk.buffer, NULL);
    vmaFreeMemory(device->vk.vmaAllocator, vk.allocation);
    return;
  }
#endif
#if (DEVICE_IMPL_MTL)
  if (device->renderer->is_target_selected(RI_DEVICE_API_MTL)) {
    if (mtl.buffer) {
      mtl.buffer->release();
      mtl.buffer = nullptr;
    }
    mappedAddress = nullptr;
    return;
  }
#endif
  assert(false && "unhandled backend");
}

#if (DEVICE_IMPL_VULKAN)
struct RIBuffer
RIBuffer::VK_createFromVMA(struct RIDevice *device,
                             VkBufferCreateInfo *vk_info,
                             VmaAllocationCreateInfo *vma_info) {


  RIBuffer buf = {};
  VK_WrapResult(vmaCreateBuffer(device->vk.vmaAllocator, vk_info, vma_info,
                                &buf.vk.buffer, &buf.vk.allocation, nullptr));
  return buf;
}
#endif

#if (DEVICE_IMPL_MTL)
struct RIBuffer RIBuffer::MTL_create(struct RIDevice *device, uint64_t size,
                                     bool hostVisible) {
  RIBuffer buf = {};
  // Unified-memory Apple GPUs expose Shared (CPU+GPU coherent) memory; use
  // Private for GPU-only resources. NRI BufferMTL::Create uses the same
  // single-shot newBuffer path (no separate heap/bind step in this HAL).
  MTL::ResourceOptions opt = hostVisible ? MTL::ResourceStorageModeShared
                                         : MTL::ResourceStorageModePrivate;
  buf.mtl.buffer = device->mtl.device->newBuffer(size, opt);
  buf.mappedAddress = hostVisible ? buf.mtl.buffer->contents() : nullptr;
  return buf;
}
#endif

struct RIBuffer RIBuffer::create(struct RIDevice *device,
                                 const struct RIBufferDesc &desc) {
#if (DEVICE_IMPL_VULKAN)
  if (device->renderer->is_target_selected(RI_DEVICE_API_VK)) {
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
    return buf;
  }
#endif
#if (DEVICE_IMPL_MTL)
  if (device->renderer->is_target_selected(RI_DEVICE_API_MTL)) {
    // Storage mode / mapping is driven by the location; usage and alignment
    // need no Metal handling (newBuffer is page-aligned).
    return MTL_create(device, desc.size,
                      desc.location == RI_MEMORY_HOST_UPLOAD);
  }
#endif
  assert(false && "unhandled backend");
  return RIBuffer{};
}

uint64_t RIBuffer::GetDeviceHandle(struct RIDevice *device) {
#if (DEVICE_IMPL_VULKAN)
  if (device->renderer->is_target_selected(RI_DEVICE_API_VK)) {
    if (vk.buffer == NULL)
      return 0;
    VkBufferDeviceAddressInfo info = {
        VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    info.buffer = vk.buffer;
    return vkGetBufferDeviceAddress(device->vk.device, &info);
  }
#endif
#if (DEVICE_IMPL_MTL)
  if (device->renderer->is_target_selected(RI_DEVICE_API_MTL)) {
    return mtl.buffer ? mtl.buffer->gpuAddress() : 0;
  }
#endif
  return 0;
}

void RIBuffer::flush(struct RIDevice *device, uint64_t offset, uint64_t size) {
#if (DEVICE_IMPL_VULKAN)
  if (device->renderer->is_target_selected(RI_DEVICE_API_VK)) {
    // ~0ull == VK_WHOLE_SIZE; vmaFlushAllocation is a no-op for coherent memory.
    VK_WrapResult(vmaFlushAllocation(device->vk.vmaAllocator, vk.allocation,
                                     offset, size));
    return;
  }
#endif
#if (DEVICE_IMPL_MTL)
  if (device->renderer->is_target_selected(RI_DEVICE_API_MTL)) {
    (void)offset;
    (void)size; // Shared storage is CPU/GPU-coherent — nothing to flush.
    return;
  }
#endif
  assert(false && "unhandled backend");
}
