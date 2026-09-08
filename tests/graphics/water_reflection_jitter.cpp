#include "graphics/WaterReflectionJitterMath.h"

#include <cmath>
#include <cstdint>
#include <cstdio>

namespace {

bool Near(float a, float b, float epsilon = 2.0e-5f) {
  return std::fabs(a - b) <= epsilon;
}

bool Check(bool condition, const char *name) {
  if (!condition) {
    std::printf("failed: %s\n", name);
    return false;
  }
  return true;
}

bool CheckScale(const float input[2], uint32_t width, uint32_t height,
                const float expected[2], const char *name,
                float epsilon = 2.0e-5f) {
  float actual[2] = {};
  hpl::WaterReflectionScaleJitterToHalfRes(input, width, height, actual);
  return Check(Near(actual[0], expected[0], epsilon) &&
                   Near(actual[1], expected[1], epsilon),
               name);
}

} // namespace

bool RunWaterReflectionJitterTests() {
  if (!Check(hpl::WaterReflectionHalfResExtent(1280) == 640,
             "even full extent uses half extent") ||
      !Check(hpl::WaterReflectionHalfResExtent(1281) == 641,
             "odd full extent uses ceil half extent") ||
      !Check(hpl::WaterReflectionHalfResExtent(1) == 1,
             "one-pixel extent remains one pixel")) {
    return false;
  }

  const float evenJitter[2] = {0.5f, -0.5f};
  const float evenExpected[2] = {0.25f, -0.25f};
  float evenActual[2] = {};
  hpl::WaterReflectionScaleJitterToHalfRes(evenJitter, 1280, 720,
                                           evenActual);
  if (!Check(Near(evenActual[0], evenExpected[0]) &&
                 Near(evenActual[1], evenExpected[1]) &&
                 evenActual[0] != 0.125f && evenActual[1] != -0.125f,
             "even jitter scales once to 0.25, not 0.125")) {
    return false;
  }

  const float oddJitter[2] = {0.5f, -0.5f};
  const float oddRatioX = 641.0f / 1281.0f;
  const float oddRatioY = 361.0f / 721.0f;
  const float oddExpected[2] = {0.5f * oddRatioX, -0.5f * oddRatioY};
  if (!CheckScale(oddJitter, 1281, 721, oddExpected,
                  "odd extent uses 641/1281 and 361/721 ratios", 1.0e-7f)) {
    return false;
  }

  const float oneJitter[2] = {0.37f, -0.41f};
  const float oneExpected[2] = {0.37f, -0.41f};
  if (!CheckScale(oneJitter, 1, 1, oneExpected,
                  "one-pixel extent keeps jitter unchanged")) {
    return false;
  }

  const float zeroJitter[2] = {0.5f, -0.5f};
  const float zeroExpected[2] = {0.0f, 0.0f};
  if (!CheckScale(zeroJitter, 0, 0, zeroExpected,
                  "zero extent returns zero jitter without division")) {
    return false;
  }

  const float zeroWidthExpected[2] = {0.0f, -0.5f * (361.0f / 721.0f)};
  if (!CheckScale(zeroJitter, 0, 721, zeroWidthExpected,
                  "zero width only clears the x jitter axis")) {
    return false;
  }

  const float independentJitter[2] = {0.4f, -0.3f};
  const float independentExpected[2] = {0.4f * (640.0f / 1280.0f),
                                         -0.3f * (361.0f / 721.0f)};
  if (!CheckScale(independentJitter, 1280, 721, independentExpected,
                  "width and height scale jitter independently")) {
    return false;
  }

  const float currentJitter[2] = {0.25f, -0.4f};
  const float previousJitter[2] = {-0.5f, 0.5f};
  const float currentExpected[2] = {0.25f * oddRatioX,
                                    -0.4f * oddRatioY};
  const float previousExpected[2] = {-0.5f * oddRatioX, 0.5f * oddRatioY};
  float currentActual[2] = {};
  float previousActual[2] = {};
  hpl::WaterReflectionScaleJitterToHalfRes(currentJitter, 1281, 721,
                                            currentActual);
  hpl::WaterReflectionScaleJitterToHalfRes(previousJitter, 1281, 721,
                                            previousActual);
  if (!Check(Near(currentActual[0], currentExpected[0]) &&
                 Near(currentActual[1], currentExpected[1]) &&
                 Near(previousActual[0], previousExpected[0]) &&
                 Near(previousActual[1], previousExpected[1]),
             "cameraJitterPrev uses the current frame ratios")) {
    return false;
  }

  const float inputs[] = {-0.5f, -0.37f, -0.125f, 0.0f,
                          0.125f, 0.37f, 0.5f};
  for (float input : inputs) {
    const float pair[2] = {input, -input};
    float actual[2] = {};
    hpl::WaterReflectionScaleJitterToHalfRes(pair, 1, 1281, actual);
    if (!Check(actual[0] >= -0.5f && actual[0] <= 0.5f &&
                   actual[1] >= -0.5f && actual[1] <= 0.5f,
               "jitter in NRD range stays in NRD range")) {
      return false;
    }
  }

  std::printf("water reflection jitter checks passed\n");
  return true;
}
