#include "graphics/WaterReflectionJitterMath.h"

namespace hpl {

uint32_t WaterReflectionHalfResExtent(uint32_t full) {
  return (full + 1u) / 2u;
}

void WaterReflectionScaleJitterToHalfRes(
    const float fullPixelJitter[2], uint32_t fullWidth, uint32_t fullHeight,
    float outHalfPixelJitter[2]) {
  const uint32_t halfWidth = WaterReflectionHalfResExtent(fullWidth);
  const uint32_t halfHeight = WaterReflectionHalfResExtent(fullHeight);
  const float jitterScaleX =
      fullWidth == 0
          ? 0.0f
          : static_cast<float>(halfWidth) / static_cast<float>(fullWidth);
  const float jitterScaleY =
      fullHeight == 0
          ? 0.0f
          : static_cast<float>(halfHeight) / static_cast<float>(fullHeight);
  outHalfPixelJitter[0] = fullPixelJitter[0] * jitterScaleX;
  outHalfPixelJitter[1] = fullPixelJitter[1] * jitterScaleY;
}

} // namespace hpl
