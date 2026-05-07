#ifndef RI_VK_H
#define RI_VK_H

#include "RITypes.h"
#include "RIFormat.h"
#include "graphics/GraphicsTypes.h"
#include <cassert>
#include <vulkan/vulkan_core.h>

#if DEVICE_IMPL_VULKAN
// VkResult RI_VK_InitImageView( struct RIDevice_s *dev, VkImageViewCreateInfo *info, struct RIDescriptor_s *desc, VkDescriptorType type );
#define RI_VK_DESCRIPTOR_IS_IMAGE( desc ) ( desc.vk.type == VK_DESCRIPTOR_TYPE_SAMPLER || desc.vk.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || desc.vk.type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE )

namespace hpl {
static inline VkSamplerAddressMode RI_VK_TextureWrap(eTextureWrap wrap) {
  switch (wrap) {
  case eTextureWrap_Repeat:
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
  case eTextureWrap_Clamp:
  case eTextureWrap_ClampToEdge:
    return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
  case eTextureWrap_ClampToBorder:
    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
  case eTextureWrap_LastEnum:
    break;
  }
  assert(false);
  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
}
} // namespace hpl

static inline void RI_VK_FillColorAttachment(VkRenderingAttachmentInfo *info,
                                             struct RIDescriptor_s *desc,
                                             bool attachAndClear) {
  info->imageView = desc->vk.image.imageView;
  info->imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  info->resolveMode = VK_RESOLVE_MODE_NONE;
  info->resolveImageView = VK_NULL_HANDLE;
  info->resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  info->loadOp =
      attachAndClear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
  info->storeOp = VK_ATTACHMENT_STORE_OP_STORE;
}

static inline void RI_VK_FillDepthAttachment( VkRenderingAttachmentInfo *info, struct RIDescriptor_s *desc, bool attachAndClear )
{
	info->imageView = desc->vk.image.imageView;
	info->imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
	info->resolveMode = VK_RESOLVE_MODE_NONE;
	info->resolveImageView = VK_NULL_HANDLE;
	info->resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	info->loadOp = attachAndClear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
	info->storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	info->clearValue.depthStencil.depth = 1.0f;
}

const VkFormat RIFormatToVK(uint32_t format);
const enum RI_Format_e VKToRIFormat(VkFormat);

static inline VkAccelerationStructureTypeKHR RI_VK_AccelStructureType(enum RIAccelStructureType_e type) {
  switch (type) {
  case RI_ACCEL_STRUCTURE_TYPE_BOTTOM_LEVEL:
    return VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
  case RI_ACCEL_STRUCTURE_TYPE_TOP_LEVEL:
    return VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
  }
  assert(false);
  return VK_ACCELERATION_STRUCTURE_TYPE_GENERIC_KHR;
}

static inline VkBuildAccelerationStructureFlagsKHR RI_VK_AccelBuildFlags(uint32_t flags) {
  VkBuildAccelerationStructureFlagsKHR out = 0;
  if (flags & RI_ACCEL_BUILD_ALLOW_UPDATE)      out |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
  if (flags & RI_ACCEL_BUILD_ALLOW_COMPACTION)  out |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
  if (flags & RI_ACCEL_BUILD_ALLOW_DATA_ACCESS) out |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_DATA_ACCESS_KHR;
  if (flags & RI_ACCEL_BUILD_PREFER_FAST_TRACE) out |= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
  if (flags & RI_ACCEL_BUILD_PREFER_FAST_BUILD) out |= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
  if (flags & RI_ACCEL_BUILD_MINIMIZE_MEMORY)   out |= VK_BUILD_ACCELERATION_STRUCTURE_LOW_MEMORY_BIT_KHR;
  return out;
}

static inline VkGeometryFlagsKHR RI_VK_AccelGeometryFlags(uint32_t flags) {
  VkGeometryFlagsKHR out = 0;
  if (flags & RI_ACCEL_GEOMETRY_OPAQUE)                          out |= VK_GEOMETRY_OPAQUE_BIT_KHR;
  if (flags & RI_ACCEL_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION) out |= VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR;
  return out;
}

static inline VkGeometryInstanceFlagsKHR RI_VK_AccelInstanceFlags(uint32_t flags) {
  VkGeometryInstanceFlagsKHR out = 0;
  if (flags & RI_ACCEL_INSTANCE_TRIANGLE_CULL_DISABLE) out |= VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
  if (flags & RI_ACCEL_INSTANCE_TRIANGLE_FLIP_FACING)  out |= VK_GEOMETRY_INSTANCE_TRIANGLE_FLIP_FACING_BIT_KHR;
  if (flags & RI_ACCEL_INSTANCE_FORCE_OPAQUE)          out |= VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;
  if (flags & RI_ACCEL_INSTANCE_FORCE_NON_OPAQUE)      out |= VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR;
  return out;
}

#endif

#endif

