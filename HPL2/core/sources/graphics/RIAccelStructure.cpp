#include "graphics/RITypes.h"
#include <cassert>


void RIAccelStructure::dispose(struct RIDevice *device) {
#if (DEVICE_IMPL_VULKAN)
  if (device->renderer->is_target_selected(RI_DEVICE_API_VK)) {
    if (vk.handle != VK_NULL_HANDLE) {
      vkDestroyAccelerationStructureKHR(device->vk.device, vk.handle, NULL);
      vk.handle = VK_NULL_HANDLE;
    }
    return;
  }
#endif
#if (DEVICE_IMPL_MTL)
  if (device->renderer->is_target_selected(RI_DEVICE_API_MTL)) {
    if (mtl.handle) {
      mtl.handle->release();
      mtl.handle = nullptr;
    }
    return;
  }
#endif
  assert(false && "unhandled backend");
}

void RIAccelStructure::setDebugObjectName(struct RIDevice *device,
                                            const char *name) {
#if (DEVICE_IMPL_VULKAN)
  if (device->renderer->is_target_selected(RI_DEVICE_API_VK)) {
    assert(vk.handle);
    if (vkSetDebugUtilsObjectNameEXT && vk.handle) {
      VkDebugUtilsObjectNameInfoEXT nameInfo = {
          VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT, NULL,
          VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR, (uint64_t)vk.handle, name};
      VK_WrapResult(vkSetDebugUtilsObjectNameEXT(device->vk.device, &nameInfo));
    }
    return;
  }
#endif
#if (DEVICE_IMPL_MTL)
  if (device->renderer->is_target_selected(RI_DEVICE_API_MTL)) {
    if (mtl.handle)
      mtl.handle->setLabel(NS::String::string(name, NS::UTF8StringEncoding));
    return;
  }
#endif
  assert(false && "unhandled backend");
}
