#include "graphics/RIFormat.h"

#include <cstdint>
#include <cstdio>

namespace {

struct UploadRowPitchCase {
  RI_Format format;
  uint32_t deviceRowAlignment;
  uint32_t width;
  uint32_t sliceNum;
  uint32_t expectedStride;
  uint32_t expectedBlockWidth;
  uint32_t expectedBlockHeight;
  const char *name;
};

bool Check(bool condition, const char *name) {
  if (!condition) {
    std::printf("failed: %s\n", name);
    return false;
  }
  return true;
}

uint64_t ExpectedLcm(uint32_t rowAlignment, uint32_t stride) {
  const uint64_t rowAlign = rowAlignment ? rowAlignment : 1u;
  const uint64_t blockStride = stride ? stride : 1u;
  uint64_t a = rowAlign;
  uint64_t b = blockStride;

  // Keep this gcd/lcm calculation independent of RIFormatAlignRowPitch so
  // this test catches a regression rather than restating the implementation.
  while (b != 0) {
    const uint64_t remainder = a % b;
    a = b;
    b = remainder;
  }
  return (rowAlign / a) * blockStride;
}

bool CheckUploadRowPitch(const UploadRowPitchCase &testCase) {
  const RIFormatProps *props = GetRIFormatProps(testCase.format);
  if (!Check(props != nullptr, testCase.name)) {
    return false;
  }

  if (!Check(props->stride == testCase.expectedStride &&
                 props->blockWidth == testCase.expectedBlockWidth &&
                 props->blockHeight == testCase.expectedBlockHeight,
             testCase.name)) {
    return false;
  }

  const uint64_t rowPitch =
      static_cast<uint64_t>(RIFormatBlockCount(testCase.width,
                                               props->blockWidth)) *
      props->stride;
  const uint64_t alignRowPitch = RIFormatAlignRowPitch(
      rowPitch, testCase.deviceRowAlignment, props->stride);
  const uint64_t expectedLcm =
      ExpectedLcm(testCase.deviceRowAlignment, props->stride);

  if (!Check(alignRowPitch % props->stride == 0, testCase.name) ||
      !Check(alignRowPitch % testCase.deviceRowAlignment == 0,
             testCase.name) ||
      !Check(alignRowPitch >= rowPitch, testCase.name) ||
      !Check(alignRowPitch - rowPitch < expectedLcm, testCase.name)) {
    return false;
  }

  const uint64_t rowBlockNum = alignRowPitch / props->stride;
  const uint64_t bufferRowLength = rowBlockNum * props->blockWidth;
  if (!Check((bufferRowLength / props->blockWidth) * props->stride ==
                 alignRowPitch,
             testCase.name)) {
    return false;
  }

  const uint64_t alignSlicePitch =
      static_cast<uint64_t>(testCase.sliceNum) * alignRowPitch;
  return Check(alignSlicePitch % alignRowPitch == 0, testCase.name) &&
         Check(alignSlicePitch / alignRowPitch == testCase.sliceNum,
               testCase.name);
}

} // namespace

bool RunUploadRowPitchTests() {
  const UploadRowPitchCase cases[] = {
      {RI_FORMAT_RGB8_UNORM, 256, 5, 2, 3, 1, 1, "RGB8 5x2 row alignment 256"},
      {RI_FORMAT_BGR8_UNORM, 256, 5, 2, 3, 1, 1, "BGR8 5x2 row alignment 256"},
      {RI_FORMAT_RGB8_UNORM, 1, 6, 1, 3, 1, 1, "RGB8 width multiple of stride"},
      {RI_FORMAT_BGR8_UNORM, 4, 5, 3, 3, 1, 1, "BGR8 row alignment 4"},
      {RI_FORMAT_RGB8_UNORM, 64, 6, 2, 3, 1, 1, "RGB8 row alignment 64"},
      {RI_FORMAT_BGR8_UNORM, 512, 7, 4, 3, 1, 1, "BGR8 row alignment 512"},

      {RI_FORMAT_R8_UNORM, 1, 5, 1, 1, 1, 1, "R8 width not aligned"},
      {RI_FORMAT_R8_UNORM, 64, 64, 2, 1, 1, 1, "R8 width aligned"},
      {RI_FORMAT_RGBA8_UNORM, 4, 5, 2, 4, 1, 1, "RGBA8 width not aligned"},
      {RI_FORMAT_RGBA8_UNORM, 256, 64, 3, 4, 1, 1, "RGBA8 width aligned"},
      {RI_FORMAT_RG16_UNORM, 64, 7, 2, 4, 1, 1, "RG16 width not aligned"},
      {RI_FORMAT_RG16_SFLOAT, 512, 8, 4, 4, 1, 1, "RG16 width aligned"},

      {RI_FORMAT_BC1_RGBA_UNORM, 1, 4, 1, 8, 4, 4, "BC1 4x4"},
      {RI_FORMAT_BC1_RGBA_UNORM, 256, 5, 2, 8, 4, 4, "BC1 5x5"},
      {RI_FORMAT_BC1_RGBA_UNORM, 64, 7, 2, 8, 4, 4, "BC1 7x7"},
      {RI_FORMAT_BC1_RGBA_UNORM, 4, 8, 2, 8, 4, 4, "BC1 8x8"},
      {RI_FORMAT_BC3_RGBA_UNORM, 256, 5, 2, 16, 4, 4, "BC3 5x5"},
      {RI_FORMAT_BC3_RGBA_UNORM, 512, 7, 2, 16, 4, 4, "BC3 7x7"},
      {RI_FORMAT_BC3_RGBA_UNORM, 4, 8, 2, 16, 4, 4, "BC3 8x8"},
      {RI_FORMAT_BC7_RGBA_UNORM, 256, 5, 2, 16, 4, 4, "BC7 5x5"},
      {RI_FORMAT_BC7_RGBA_UNORM, 64, 7, 2, 16, 4, 4, "BC7 7x7"},
      {RI_FORMAT_BC7_RGBA_UNORM, 4, 8, 2, 16, 4, 4, "BC7 8x8"},
  };

  for (const UploadRowPitchCase &testCase : cases) {
    if (!CheckUploadRowPitch(testCase)) {
      return false;
    }
  }

  std::printf("upload row pitch checks passed\n");
  return true;
}
