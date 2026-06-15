
#ifndef HPL_TEXTURE__H__
#define HPL_TEXTURE__H__

#include "graphics/GraphicsTypes.h"
#include "graphics/RIFormat.h"
#include "graphics/RITypes.h"

struct RIResourceUploader;

namespace hpl {
struct RIBootstrap;
class cBitmap;

RI_Format to_image_supported_format(ePixelFormat format);

struct cTexture {
public:
  // Image + its VMA allocation (handle.vk.allocation); handle.dispose()
  // releases both.
  struct RITexture handle;
  RI_Format format = RI_FORMAT_UNKNOWN;
  // Sampled-image view over `handle`, owned by this texture. The descriptor is
  // produced on demand by descriptor(); the bindless cache key (cookie) lives
  // on the view (stamped at creation).
  struct RITextureView view;
  uint16_t width;
  uint16_t height;
  uint16_t depth;
  uint8_t mipNum;
  // RI_Format the image was created with (set by LoadBitmap) — lets
  // material setup probe channel count (e.g. single-channel alpha maps).

  ~cTexture();

  static void cTexture_Delete(cTexture *tex);

  // Sampled-image descriptor, produced on demand. Owns nothing; references
  // `view`. `cookie` is the stable bindless descriptor-set cache key.
  RIDescriptor descriptor();

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
  // postState/postStages: resource state the texture is transitioned to once
  // the upload completes (RIBarrier.h); stages 0 derives from the state.
  bool LoadBitmap(enum RIResourceState_e postState, uint32_t postStages,
                  cBitmap &bitmap, const BitmapLoadOptions &options);
  // Re-upload pixels into the already-created image (mip 0 / layer 0; bitmap
  // size and format must match the original LoadBitmap). The image is assumed
  // fragment-sampled and is returned to that state — the uploader emits the
  // pre/post barriers. SetRawData replacement for procedural textures.
  bool UpdateBitmap(cBitmap &bitmap);
  void setDebugName(const tWString &name);
  void setDebugName(const char *name);
};

} // namespace hpl

#endif
