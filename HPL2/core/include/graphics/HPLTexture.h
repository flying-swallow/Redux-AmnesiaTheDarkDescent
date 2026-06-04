
#ifndef HPL_TEXTURE__H__
#define HPL_TEXTURE__H__

#include "graphics/GraphicsTypes.h"
#include "graphics/RIFormat.h"
#include "graphics/RITypes.h"

struct RIResourceUploader_s;

namespace hpl {
struct RIBootstrap;
class cBitmap;

RI_Format to_image_supported_format(ePixelFormat format); 

struct HPLTexture {
public:
  struct RITexture_s handle;
  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      struct VmaAllocation_T *vmaAlloc;
    } vk;
#endif
  };
  uint16_t width;
  uint16_t height;
  uint16_t depth;
  uint8_t mipNum;
  // RI_Format the image was created with (set by LoadBitmap) — lets
  // material setup probe channel count (e.g. single-channel alpha maps).
  RI_Format format = RI_FORMAT_UNKNOWN;
  struct RIDescriptor_s binding;

  static void HPLTexture_Delete(HPLTexture* tex); 

  struct BitmapLoadOptions {
  public:
    bool use_cubemap = false;
    bool use_array = false;
    bool use_mipmaps = false;
    // When true, the image view is created with an sRGB format variant so
    // the hardware decodes sample values from sRGB→linear on read. Set on
    // perceptual color sources (diffuse, illumination); leave false for
    // linear data (normals, packed specular, height, attenuation LUTs).
    bool sRGB = false;
  };
  bool LoadBitmap(RIBarrierImageHandle_s postBarrier, 
                  cBitmap &bitmap,
                  const BitmapLoadOptions &options);
  void setDebugName(const tWString& name);
  void setDebugName(const char* name);
};

} // namespace hpl

#endif
