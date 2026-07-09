
#ifndef HPL_TEXTURE__H__
#define HPL_TEXTURE__H__

#include "graphics/GraphicsTypes.h"
#include "graphics/RIFormat.h"
#include "graphics/RITypes.h"

struct RIResourceUploader;

namespace hpl {
class cBitmap;

RI_Format to_image_supported_format(ePixelFormat format);

struct cTexture {
public:
  struct RITexture handle;
  RI_Format format = RI_FORMAT_UNKNOWN;
  struct RITextureView view;
  uint16_t width = 0;
  uint16_t height = 0;
  uint16_t depth = 0;
  uint8_t mipNum = 0;

  cTexture() = default;
  // Move-only: a cTexture owns GPU resources (handle + view). Moving steals them
  // and zeroes the source so its destructor is a no-op; copying is forbidden.
  cTexture(const cTexture &) = delete;
  cTexture &operator=(const cTexture &) = delete;
  cTexture(cTexture &&other) noexcept;
  cTexture &operator=(cTexture &&other) noexcept;
  ~cTexture();

  RIDescriptor descriptor();

  struct BitmapLoadOptions {
  public:
    bool use_cubemap = false;
    bool use_array = false;
    bool use_mipmaps = false;
    bool sRGB = false;
  };
  bool LoadBitmap(enum RIResourceState_e postState, uint32_t postStages,
                  cBitmap &bitmap, const BitmapLoadOptions &options);
  bool UpdateBitmap(cBitmap &bitmap);
  void setDebugName(const tWString &name);
  void setDebugName(const char *name);
};

} // namespace hpl

#endif
