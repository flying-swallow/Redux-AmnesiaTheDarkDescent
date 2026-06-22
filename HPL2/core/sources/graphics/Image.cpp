#include "graphics/Image.h"

#include "resources/TextureManager.h" // ReleaseImageBindlessSlot

namespace hpl {

// Standalone Images are allocated with plain `new` and freed by the handle's
// default (monostate) policy — plain `delete`. This matches the per-frame
// SharedResourcePin, which also deletes a manager-less resource, so whichever
// of the owner or a lingering pin holds the last reference frees it the same way.
SharedResourceHandle<Image> AdoptStandaloneImage(Image *apImage) {
  return AdoptResource<Image>(apImage);
}

Image::Image() : iResourceBase("", _W(""), 0) {
}

void Image::SetImage(SingleImage &&singleImage) {
    value = std::move(singleImage);
}
void Image::SetImage(AnimatedImage &&animatedImage) {
    value = std::move(animatedImage);
}


Image::Image(SingleImage &&image)
    : iResourceBase("", _W(""), 0) {
  value = std::move(image);
}
Image::Image(AnimatedImage &&image)
    : iResourceBase("", _W(""), 0) {
  value = std::move(image);
}

Image::Image(const tString &asName, const tWString &asFullPath,
             SingleImage &&image)
    : iResourceBase(asName, asFullPath, 0) {
  value = std::move(image);
}
Image::Image(const tString &asName, const tWString &asFullPath,
             AnimatedImage &&image)
    : iResourceBase(asName, asFullPath, 0) {
  value = std::move(image);
}
Image::Image(Image &&other)
    : iResourceBase(other.GetName(), other.GetFullPath(), 0) {
  value = std::move(other.value);
  // Steal the bindless slot so only one Image owns (and later frees) it.
  mBindlessSlot = other.mBindlessSlot;
  mBindlessViewCookie = other.mBindlessViewCookie;
  mBindlessIsCube = other.mBindlessIsCube;
  mBindlessIsArray = other.mBindlessIsArray;
  other.mBindlessSlot = 0xffffffffu;
}

cTexture* Image::GetTexture() const {
  if (const SingleImage *singleImage = std::get_if<SingleImage>(&value)) {
    return singleImage->image ? const_cast<cTexture *>(&*singleImage->image) : nullptr;
  } else if (const AnimatedImage *animImage = std::get_if<AnimatedImage>(&value)) {
    // The whole animation is one Texture2DArray; the current layer is picked
    // in-shader, so just hand back the array texture.
    return const_cast<cTexture *>(&animImage->image);
  }
  return nullptr;
}

uint32_t Image::GetAnimFrameCount() const {
  if (const AnimatedImage *animImage = std::get_if<AnimatedImage>(&value)) {
    return animImage->frameCount;
  }
  return 0;
}

eTextureAnimMode Image::GetAnimMode() {
  if (const AnimatedImage *animImage = std::get_if<AnimatedImage>(&value)) {
    return animImage->animMode;
  }
  return eTextureAnimMode_None;
}

bool Image::isAnimated() const {
  return std::holds_alternative<AnimatedImage>(value);
}

bool Image::IsValid() const {
  return GetTexture() != nullptr;
}

void Image::SetAnimMode(eTextureAnimMode aMode) {
  if (AnimatedImage *animImage = std::get_if<AnimatedImage>(&value)) {
    animImage->animMode = aMode;
  }
}

void Image::Update(float afTimeStep) {
  // No-op: animated images are one Texture2DArray and the current frame is
  // selected in-shader from gPerFrame.afT (no CPU frame cursor, no descriptor
  // rewrite). Kept for ABI compatibility with existing callers.
  (void)afTimeStep;
}
void Image::SetFrameTime(float t) {
  if (AnimatedImage *animImage = std::get_if<AnimatedImage>(&value)) {
    animImage->frameTime = t;
  }
}
float Image::GetFrameTime() const {
  if (const AnimatedImage *animImage = std::get_if<AnimatedImage>(&value)) {
    return animImage->frameTime;
  }
  return 0.0f;
}

uint16_t Image::GetWidth() const {
  if (cTexture *image = GetTexture()) {
    return image->width;
  }
  return 0;
}
uint16_t Image::GetHeight() const {
  if (cTexture *image = GetTexture()) {
    return image->height;
  }
  return 0;
}

bool Image::Reload() { return false; }
void Image::Unload() {}
void Image::Destroy() {}

cVector2l Image::GetImageSize() const {
  return cVector2l(GetWidth(), GetHeight());
}

Image::~Image() {
  // Return the bindless slot to cTextureManager's heap (deferred past in-flight
  // frames). No-op when no slot was assigned.
  ReleaseImageBindlessSlot(this);
}

void Image::operator=(Image &&other) {
  // Release our own slot before taking over the moved-from one.
  ReleaseImageBindlessSlot(this);
  value = std::move(other.value);
  mBindlessSlot = other.mBindlessSlot;
  mBindlessViewCookie = other.mBindlessViewCookie;
  mBindlessIsCube = other.mBindlessIsCube;
  mBindlessIsArray = other.mBindlessIsArray;
  other.mBindlessSlot = 0xffffffffu;
}

} // namespace hpl
