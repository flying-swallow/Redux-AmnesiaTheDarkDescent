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
#include <variant>

#include "graphics/HPLTexture.h"

namespace hpl {
class cBitmap;
struct HPLTexture;
class Image : public iResourceBase {
public:
  struct SingleImage {
    std::shared_ptr<HPLTexture> image;
  };

  struct AnimatedImage {
    std::vector<std::shared_ptr<HPLTexture>> images;
    float frameTime = 0.0f;
    float timeCount = 0.0f;
    float timeDir = 1.0f;
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
  std::shared_ptr<HPLTexture> GetTexture() const;
  void Update(float afTimeStep);
  void SetFrameTime(float frameTime);
  float GetFrameTime() const;
private:
  std::variant<SingleImage, AnimatedImage> value;
};

} // namespace hpl
