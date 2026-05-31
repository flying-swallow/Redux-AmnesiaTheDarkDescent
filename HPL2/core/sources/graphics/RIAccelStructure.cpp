#include "graphics/RITypes.h"
#include <cassert>


void RIAccelStructure_s::dispose(struct RIDevice_s *device) {
  vkDestroyAccelerationStructureKHR(device->vk.device, vk.handle, NULL);
}

void RIAccelStructure_s::setDebugObjectName(struct RIDevice_s *device,
                                            const char *name) {
  assert(vk.handle);
  if (vkSetDebugUtilsObjectNameEXT && vk.handle) {
    VkDebugUtilsObjectNameInfoEXT nameInfo = {
        VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT, NULL,
        VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR, (uint64_t)vk.handle, name};
    VK_WrapResult(vkSetDebugUtilsObjectNameEXT(device->vk.device, &nameInfo));
  }
}


