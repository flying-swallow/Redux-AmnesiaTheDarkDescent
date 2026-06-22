/**
 * Copyright 2023 Michael Pollind
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once


#include "math/MathTypes.h"
#include "resources/ResourceBase.h"

#include <memory>
#include <cassert>
#include <cstdint>
#include <optional>
#include <variant>

#include "graphics/Texture.h"

namespace hpl {
class cBitmap;
struct cTexture;
class Image : public iResourceBase {
public:
  struct SingleImage {
    std::optional<cTexture> image;
  };

  // An animated Image is ONE Texture2DArray (N frames = N array layers). The
  // current frame is selected in-shader from gPerFrame.afT (see gAnimTex /
  // sampleBindless2D), so there is no CPU frame cursor — frameCount/frameTime/
  // mode are uploaded once into the GPU animation record. (Was a vector of
  // per-frame cTextures with a CPU timeCount that rewrote the descriptor.)
  struct AnimatedImage {
    cTexture image;                                   // the N-layer array texture
    uint32_t frameCount = 0;
    float frameTime = 0.0f;
    eTextureAnimMode animMode = eTextureAnimMode_Loop;
  };

  Image();
  explicit Image(SingleImage &&singleImage);
  explicit Image(AnimatedImage &&singleImage);
  explicit Image(const tString &asName, const tWString &asFullPath, SingleImage &&singleImage);
  explicit Image(const tString &asName, const tWString &asFullPath, AnimatedImage &&singleImage);
  ~Image();

  void SetImage(SingleImage &&singleImage);
  void SetImage(AnimatedImage &&animatedImage);

  Image(Image &&other);
  Image(const Image &other) = delete;
  bool isAnimated() const;
  bool IsValid() const;
  Image &operator=(const Image &other) = delete;
  void operator=(Image &&other);

  virtual bool Reload() override;
  virtual void Unload() override;
  virtual void Destroy() override;
  uint16_t GetWidth() const;
  uint16_t GetHeight() const; 
  eTextureAnimMode GetAnimMode();
  void SetAnimMode(eTextureAnimMode aMode);

  cVector2l GetImageSize() const;
  cTexture* GetTexture() const;
  void Update(float afTimeStep);
  void SetFrameTime(float frameTime);
  float GetFrameTime() const;

  // Lifetime-stable bindless slot, owned/assigned by cTextureManager (the
  // texture heap), written once. The slot indexes textures_2d[] (binding 0),
  // textures_cube[] (binding 1), or — for an animated image — textures_2d_array[]
  // (binding 2). kInvalidTextureIndex (0xffffffff) until assigned. GetBindlessSlot()
  // tags an array slot with the high bit (kAnimatedTextureBit) so consumers
  // (submitMaterial tex[], light gobos) carry it unchanged and the shader sample
  // helper decodes it — no per-frame descriptor rewrite.
  uint32_t GetBindlessSlot() const {
    return (mBindlessIsArray && mBindlessSlot != 0xffffffffu)
               ? (mBindlessSlot | 0x80000000u) // kAnimatedTextureBit
               : mBindlessSlot;
  }
  uint32_t GetRawBindlessSlot() const { return mBindlessSlot; } // untagged (for the manager's pool free)
  bool IsBindlessCube() const { return mBindlessIsCube; }
  bool IsBindlessArray() const { return mBindlessIsArray; }
  hash_t GetBindlessViewCookie() const { return mBindlessViewCookie; }
  void SetBindlessSlot(uint32_t slot, bool isCube, bool isArray = false) {
    mBindlessSlot = slot; mBindlessIsCube = isCube; mBindlessIsArray = isArray;
  }
  void SetBindlessViewCookie(hash_t cookie) { mBindlessViewCookie = cookie; }

  // Animation params for the GPU record (valid when isAnimated()).
  uint32_t GetAnimFrameCount() const;

private:
  std::variant<SingleImage, AnimatedImage> value;

  uint32_t mBindlessSlot = 0xffffffffu; // kInvalidTextureIndex
  hash_t   mBindlessViewCookie = 0;
  bool     mBindlessIsCube = false;
  bool     mBindlessIsArray = false;    // slot indexes textures_2d_array[] (animated)
};

// Adopt a freshly created, manager-less Image (allocated with hplNew) into a
// reference-counted handle. The handle frees the Image with hplDelete once the
// last reference drops — including any transient per-frame keep-alive pin, so
// the Image (and the cTexture it owns) survives until the GPU is done with it.
SharedResourceHandle<Image> AdoptStandaloneImage(Image *apImage);

} // namespace hpl
