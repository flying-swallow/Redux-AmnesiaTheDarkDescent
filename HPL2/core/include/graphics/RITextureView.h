#ifndef RI_TEXTURE_VIEW_H
#define RI_TEXTURE_VIEW_H

#include "graphics/RIDefines.h"
#include "graphics/RITexture.h"
#include "system/Hasher.h"
#include <cstring>
#include <optional>
#include <stdint.h>

#include "graphics/RIPreamble.h"

struct RIDevice;
struct RIRenderer;

enum RITextureViewType_e {
  RI_VIEWTYPE_SHADER_RESOURCE_1D,
  RI_VIEWTYPE_SHADER_RESOURCE_1D_ARRAY,
  RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_1D,
  RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_1D_ARRAY,
  RI_VIEWTYPE_SHADER_RESOURCE_2D,
  RI_VIEWTYPE_SHADER_RESOURCE_2D_ARRAY,
  RI_VIEWTYPE_SHADER_RESOURCE_CUBE,
  RI_VIEWTYPE_SHADER_RESOURCE_CUBE_ARRAY,
  RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_2D,
  RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_2D_ARRAY,
  RI_VIEWTYPE_SHADER_RESOURCE_3D,
  RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_3D,

  RI_VIEWTYPE_COLOR_ATTACHMENT,
  RI_VIEWTYPE_DEPTH_STENCIL_ATTACHMENT,
  RI_VIEWTYPE_DEPTH_READONLY_STENCIL_ATTACHMENT,
  RI_VIEWTYPE_DEPTH_ATTACHMENT_STENCIL_READONLY,
  RI_VIEWTYPE_DEPTH_STENCIL_READONLY,
  RI_VIEWTYPE_SHADING_RATE_ATTACHMENT
};

// Backend-neutral image-view descriptor consumed by RITextureView::create.
struct RITextureViewDesc {
  enum RITextureViewType_e viewType;
  uint32_t format; // RI_Format_e (view format; may reinterpret the image format)
  uint32_t baseMip;
  uint32_t mipNum;
  uint32_t baseLayer;
  uint32_t layerNum;
};

struct RITextureView {
  RITextureView() { memset(this, 0, sizeof(*this)); }
  // Backend-neutral view creation over `tex`. VK: vkCreateImageView. The caller
  // owns the returned view and disposes it. `cookie` is stamped from the backend
  // handle for use as a bindless descriptor-set cache key.
  static struct RITextureView create(struct RIDevice *device,
                                     const struct RITexture *tex,
                                     const struct RITextureViewDesc &desc, std::optional<hash_t> hash = {});
  // Destroys the image view and zeroes the struct.
  void dispose(struct RIDevice *device);
  bool isEmpty() const;
  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      VkImageView image;
    } vk;
#endif
  };
  hash_t cookie;
};

#endif // RI_TEXTURE_VIEW_H
