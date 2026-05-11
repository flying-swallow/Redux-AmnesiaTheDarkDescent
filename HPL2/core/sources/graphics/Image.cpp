#include "graphics/Image.h"

namespace hpl {

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
}

std::shared_ptr<HPLTexture> Image::GetTexture() const {
  if (const SingleImage *singleImage = std::get_if<SingleImage>(&value)) {
    return singleImage->image;
  } else if (const AnimatedImage *animImage = std::get_if<AnimatedImage>(&value)) {
    size_t frameIdx = static_cast<size_t>(animImage->timeCount);
    assert(frameIdx < animImage->images.size() && "Frame index out of range");
    return animImage->images[frameIdx];
  }
  return nullptr;
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
  if (AnimatedImage *animImage = std::get_if<AnimatedImage>(&value)) {
    float fMax = (float)(animImage->images.size());
    animImage->timeCount +=
        afTimeStep * (1.0f / animImage->frameTime) * animImage->timeDir;

    if (animImage->timeDir > 0) {
      if (animImage->timeCount >= fMax) {
        if (animImage->animMode == eTextureAnimMode_Loop) {
          animImage->timeCount = 0;
        } else {
          animImage->timeCount = fMax - 1.0f;
          animImage->timeDir = -1.0f;
        }
      }
    } else {
      if (animImage->timeCount < 0) {
        animImage->timeCount = 1;
        animImage->timeDir = 1.0f;
      }
    }
  }
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
  if (const std::shared_ptr<HPLTexture> image = GetTexture()) {
    return image->width;
  }
  return 0;
}
uint16_t Image::GetHeight() const {
  if (const std::shared_ptr<HPLTexture> image = GetTexture()) {
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

Image::~Image() {}

void Image::operator=(Image &&other) {
  value = std::move(other.value);
}

} // namespace hpl
