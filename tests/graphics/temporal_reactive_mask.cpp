#include "graphics/TemporalReactiveMaskMath.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>

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

bool IsFiniteUnit(float value) {
  return std::isfinite(value) && value >= 0.0f && value <= 1.0f;
}

bool CheckIdenticalImage(const float color[3], const char *name) {
  const hpl::TemporalReactiveMaskSample sample =
      hpl::TemporalReactiveMaskEvaluate(color, color);
  // Identical images must be exactly unchanged, including black where a
  // naive relative metric would divide zero by zero.
  return Check(sample.reactive == 0.0f && sample.composition == 0.0f, name);
}

bool CheckIdenticalImages() {
  const float midGrey[3] = {0.5f, 0.5f, 0.5f};
  const float veryBright[3] = {1.0e4f, 1.0e4f, 1.0e4f};
  const float black[3] = {0.0f, 0.0f, 0.0f};
  return CheckIdenticalImage(midGrey, "identical mid-grey images are zero") &&
         CheckIdenticalImage(veryBright,
                             "identical very bright images are zero") &&
         CheckIdenticalImage(black, "identical black images are exactly zero");
}

bool CheckAdditiveChange(float &additiveReactive) {
  const float black[3] = {0.0f, 0.0f, 0.0f};
  const float additive[3] = {2.0f, 0.0f, 0.0f};
  const hpl::TemporalReactiveMaskSample sample =
      hpl::TemporalReactiveMaskEvaluate(black, additive);
  additiveReactive = sample.reactive;
  // Additive emission over black must remain strongly reactive without
  // producing an invalid ratio or an out-of-range mask.
  return Check(sample.reactive > 0.9f,
               "black plus bright additive color is strongly reactive") &&
         Check(IsFiniteUnit(sample.reactive) &&
                   IsFiniteUnit(sample.composition),
               "additive outputs are finite and in [0,1]");
}

bool CheckMultiplicativeTint(float additiveReactive) {
  const float opaque[3] = {1.0f, 1.0f, 1.0f};
  const float tinted[3] = {1.0f, 0.5f, 0.5f};
  const hpl::TemporalReactiveMaskSample sample =
      hpl::TemporalReactiveMaskEvaluate(opaque, tinted);
  // A tint is a real color change, but is less reactive than the additive
  // emission above; a luminance-only threshold would also lose this signal.
  return Check(sample.reactive > 0.0f && sample.reactive < additiveReactive,
               "multiplicative tint responds below additive response") &&
         Check(IsFiniteUnit(sample.reactive) &&
                   IsFiniteUnit(sample.composition),
               "multiplicative tint outputs are finite and in [0,1]");
}

bool CheckEqualLuminanceChromaticChange() {
  const float opaque[3] = {1.0f, 1.0f, 1.0f};
  const float chromatic[3] = {1.2f, 1.0f, 0.8f};
  const hpl::TemporalReactiveMaskSample sample =
      hpl::TemporalReactiveMaskEvaluate(opaque, chromatic);
  // The channel sum, and therefore simple equal-weight luminance, is equal;
  // this L1 color metric must still respond to the chromatic change.
  return Check(sample.reactive > 0.0f,
               "equal-luminance chromatic change is reactive") &&
         Check(IsFiniteUnit(sample.reactive) &&
                   IsFiniteUnit(sample.composition),
               "chromatic change outputs are finite and in [0,1]");
}

bool CheckBrightHDRValues() {
  const float opaque[3] = {1.0e4f, 1.0e4f, 1.0e4f};
  const float changed[3] = {1.5e4f, 1.5e4f, 1.5e4f};
  const hpl::TemporalReactiveMaskSample changedSample =
      hpl::TemporalReactiveMaskEvaluate(opaque, changed);
  const hpl::TemporalReactiveMaskSample unchangedSample =
      hpl::TemporalReactiveMaskEvaluate(opaque, opaque);
  // HDR values must not overflow the intermediate ratio or escape mask range.
  return Check(IsFiniteUnit(changedSample.reactive) &&
                   IsFiniteUnit(changedSample.composition),
               "bright HDR change stays finite and in [0,1]") &&
         Check(IsFiniteUnit(unchangedSample.reactive) &&
                   IsFiniteUnit(unchangedSample.composition),
               "bright HDR identity stays finite and in [0,1]");
}

bool CheckNonFiniteInputs() {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float infinity = std::numeric_limits<float>::infinity();
  const float opaque[3] = {nan, 1.0f, infinity};
  const float final[3] = {0.0f, infinity, nan};
  const hpl::TemporalReactiveMaskSample sample =
      hpl::TemporalReactiveMaskEvaluate(opaque, final);
  // NaN and Inf components are sanitized as zero rather than reaching masks.
  return Check(IsFiniteUnit(sample.reactive) &&
                   IsFiniteUnit(sample.composition),
               "non-finite components are sanitized to finite unit outputs");
}

bool CheckCompositionContract() {
  const float black[3] = {0.0f, 0.0f, 0.0f};
  const float additive[3] = {2.0f, 0.0f, 0.0f};
  const hpl::TemporalReactiveMaskSample strong =
      hpl::TemporalReactiveMaskEvaluate(black, additive);
  const hpl::TemporalReactiveMaskParams params = {};
  const float unchanged[3] = {1.0f, 1.0f, 1.0f};
  const float justBelow[3] = {1.0f + params.changeEpsilon * 0.5f, 1.0f,
                              1.0f};
  const hpl::TemporalReactiveMaskSample unchangedSample =
      hpl::TemporalReactiveMaskEvaluate(unchanged, unchanged);
  const hpl::TemporalReactiveMaskSample belowEpsilon =
      hpl::TemporalReactiveMaskEvaluate(unchanged, justBelow, params);
  // Composition is deliberately conservative and independently scaled: a
  // strong change reaches compositionScale, not full history rejection.
  if (!Check(Near(strong.reactive, 1.0f, 1.0e-3f) &&
                 Near(strong.composition, params.compositionScale),
             "strong change reaches reactive one and compositionScale") ||
      !Check(strong.composition < 1.0f,
             "strong change composition remains below one") ||
      !Check(unchangedSample.composition == 0.0f,
             "unchanged pixel composition is exactly zero")) {
    return false;
  }

  // The epsilon boundary suppresses composition for changes that are just
  // below the documented threshold.
  return Check(belowEpsilon.composition == 0.0f,
               "change below changeEpsilon has zero composition");
}

bool CheckUV(uint32_t x, uint32_t y,
             hpl::TemporalReactiveMaskMathExtent extent,
             const float jitter[2], const float expected[2],
             const char *name) {
  float actual[2] = {};
  hpl::TemporalReactiveMaskUnjitteredSampleUV(x, y, extent, jitter, actual);
  return Check(Near(actual[0], expected[0]) && Near(actual[1], expected[1]),
               name);
}

bool CheckUnjitteredSampleUV() {
  const float zeroJitter[2] = {0.0f, 0.0f};
  const hpl::TemporalReactiveMaskMathExtent oneByOne = {1, 1};
  const float oneByOneCenter[2] = {0.5f, 0.5f};
  if (!CheckUV(0, 0, oneByOne, zeroJitter, oneByOneCenter,
               "1x1 zero jitter maps to pixel center")) {
    return false;
  }

  const hpl::TemporalReactiveMaskMathExtent oddExtent = {1279, 721};
  const float oddCenter[2] = {1278.5f / 1279.0f, 720.5f / 721.0f};
  if (!CheckUV(1278, 720, oddExtent, zeroJitter, oddCenter,
               "odd extent zero jitter maps to pixel center")) {
    return false;
  }

  const uint32_t x = 17;
  const uint32_t y = 23;
  const float center[2] = {(static_cast<float>(x) + 0.5f) / 1279.0f,
                           (static_cast<float>(y) + 0.5f) / 721.0f};
  const float positiveXNegativeY[2] = {0.25f, -0.4f};
  const float expectedPositiveNegative[2] = {
      center[0] - positiveXNegativeY[0] / 1279.0f,
      center[1] - positiveXNegativeY[1] / 721.0f};
  float actualPositiveNegative[2] = {};
  hpl::TemporalReactiveMaskUnjitteredSampleUV(
      x, y, oddExtent, positiveXNegativeY, actualPositiveNegative);
  // A positive x jitter moves the sample left, while a negative y jitter
  // moves it down; these signs protect XeSS's responsive-mask lookup.
  if (!Check(Near(actualPositiveNegative[0], expectedPositiveNegative[0]) &&
                 Near(actualPositiveNegative[1], expectedPositiveNegative[1]),
             "positive and negative jitter translate by negative extent") ||
      !Check(actualPositiveNegative[0] < center[0] &&
                 actualPositiveNegative[1] > center[1],
             "positive x and negative y jitter have explicit signs")) {
    return false;
  }

  const float negativeXPositiveY[2] = {-0.25f, 0.4f};
  const float expectedNegativePositive[2] = {
      center[0] - negativeXPositiveY[0] / 1279.0f,
      center[1] - negativeXPositiveY[1] / 721.0f};
  float actualNegativePositive[2] = {};
  hpl::TemporalReactiveMaskUnjitteredSampleUV(
      x, y, oddExtent, negativeXPositiveY, actualNegativePositive);
  // Reversing both jitter components must reverse both UV translations.
  if (!Check(Near(actualNegativePositive[0], expectedNegativePositive[0]) &&
                 Near(actualNegativePositive[1], expectedNegativePositive[1]),
             "negative and positive jitter translate by negative extent") ||
      !Check(actualNegativePositive[0] > center[0] &&
                 actualNegativePositive[1] < center[1],
             "negative x and positive y jitter have explicit signs")) {
    return false;
  }

  const hpl::TemporalReactiveMaskMathExtent zeroExtent = {};
  float zeroUV[2] = {1.0f, 1.0f};
  hpl::TemporalReactiveMaskUnjitteredSampleUV(0, 0, zeroExtent, zeroJitter,
                                              zeroUV);
  // A zero extent is a safe disabled/sentinel state and must never divide.
  return Check(zeroUV[0] == 0.0f && zeroUV[1] == 0.0f,
               "zero extent returns zero UV without division");
}

} // namespace

bool RunTemporalReactiveMaskTests() {
  if (!CheckIdenticalImages())
    return false;

  float additiveReactive = 0.0f;
  if (!CheckAdditiveChange(additiveReactive) ||
      !CheckMultiplicativeTint(additiveReactive) ||
      !CheckEqualLuminanceChromaticChange() || !CheckBrightHDRValues() ||
      !CheckNonFiniteInputs() || !CheckCompositionContract() ||
      !CheckUnjitteredSampleUV()) {
    return false;
  }

  std::printf("temporal reactive mask checks passed\n");
  return true;
}
