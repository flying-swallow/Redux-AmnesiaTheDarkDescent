#include "graphics/RIFormat.h"

#include <cstdint>
#include <cstdio>

namespace {

struct LayoutCase {
  RI_Format format;
  uint32_t width;
  uint32_t height;
  const char *name;
};

bool Check(bool condition, const char *name) {
  if (!condition) {
    std::printf("failed: %s\n", name);
    return false;
  }
  return true;
}

bool CheckFormatProps(RI_Format format, uint32_t expectedBlockBytes,
                      const char *name) {
  const RIFormatProps *props = GetRIFormatProps(format);
  return Check(props != nullptr && props->blockWidth == 4 &&
                   props->blockHeight == 4 && props->stride == expectedBlockBytes,
               name);
}

bool CheckLayout(const LayoutCase &testCase) {
  const RIFormatProps *props = GetRIFormatProps(testCase.format);

  // This is intentionally independent of RIFormatBlockCount: the test must
  // catch a regression to floor division rather than restate the helper.
  const uint32_t expectedBlocksWide =
      (testCase.width + props->blockWidth - 1u) / props->blockWidth;
  const uint32_t expectedBlocksHigh =
      (testCase.height + props->blockHeight - 1u) / props->blockHeight;
  const uint32_t expectedRowPitch = expectedBlocksWide * props->stride;
  const uint32_t expectedStagingBytes = expectedRowPitch * expectedBlocksHigh;

  const uint32_t blocksWide =
      RIFormatBlockCount(testCase.width, props->blockWidth);
  const uint32_t blocksHigh =
      RIFormatBlockCount(testCase.height, props->blockHeight);
  const uint32_t rowPitch = blocksWide * props->stride;
  const uint32_t stagingBytes = rowPitch * blocksHigh;

  return Check(blocksWide == expectedBlocksWide, testCase.name) &&
         Check(blocksHigh == expectedBlocksHigh, testCase.name) &&
         Check(rowPitch == expectedRowPitch, testCase.name) &&
         Check(stagingBytes == expectedStagingBytes, testCase.name);
}

} // namespace

bool RunBcBlockLayoutTests() {
  if (!CheckFormatProps(RI_FORMAT_BC1_RGBA_UNORM, 8,
                        "BC1 format props use 4x4 blocks and 8 bytes") ||
      !CheckFormatProps(RI_FORMAT_BC3_RGBA_UNORM, 16,
                        "BC3 format props use 4x4 blocks and 16 bytes")) {
    return false;
  }

  // Every listed compressed case is checked for both BC1 and BC3 so the
  // staging arithmetic is covered with both block strides.
  const LayoutCase cases[] = {
      {RI_FORMAT_BC1_RGBA_UNORM, 5, 5, "BC1 5x5"},
      {RI_FORMAT_BC1_RGBA_UNORM, 6, 3, "BC1 6x3"},
      {RI_FORMAT_BC1_RGBA_UNORM, 7, 7, "BC1 7x7"},
      {RI_FORMAT_BC1_RGBA_UNORM, 4, 6, "BC1 4x6"},
      {RI_FORMAT_BC3_RGBA_UNORM, 5, 5, "BC3 5x5"},
      {RI_FORMAT_BC3_RGBA_UNORM, 6, 3, "BC3 6x3"},
      {RI_FORMAT_BC3_RGBA_UNORM, 7, 7, "BC3 7x7"},
      {RI_FORMAT_BC3_RGBA_UNORM, 4, 6, "BC3 4x6"},

      {RI_FORMAT_BC1_RGBA_UNORM, 1, 1, "BC1 1x1 tail mip"},
      {RI_FORMAT_BC1_RGBA_UNORM, 2, 2, "BC1 2x2 tail mip"},
      {RI_FORMAT_BC1_RGBA_UNORM, 3, 3, "BC1 3x3 tail mip"},
      {RI_FORMAT_BC1_RGBA_UNORM, 1, 4, "BC1 1x4 tail mip"},
      {RI_FORMAT_BC1_RGBA_UNORM, 4, 1, "BC1 4x1 tail mip"},
      {RI_FORMAT_BC3_RGBA_UNORM, 1, 1, "BC3 1x1 tail mip"},
      {RI_FORMAT_BC3_RGBA_UNORM, 2, 2, "BC3 2x2 tail mip"},
      {RI_FORMAT_BC3_RGBA_UNORM, 3, 3, "BC3 3x3 tail mip"},
      {RI_FORMAT_BC3_RGBA_UNORM, 1, 4, "BC3 1x4 tail mip"},
      {RI_FORMAT_BC3_RGBA_UNORM, 4, 1, "BC3 4x1 tail mip"},

      {RI_FORMAT_BC1_RGBA_UNORM, 4, 4, "BC1 4x4 exact multiple"},
      {RI_FORMAT_BC1_RGBA_UNORM, 8, 8, "BC1 8x8 exact multiple"},
      {RI_FORMAT_BC1_RGBA_UNORM, 16, 16, "BC1 16x16 exact multiple"},
      {RI_FORMAT_BC1_RGBA_UNORM, 16, 4, "BC1 16x4 exact multiple"},
      {RI_FORMAT_BC3_RGBA_UNORM, 4, 4, "BC3 4x4 exact multiple"},
      {RI_FORMAT_BC3_RGBA_UNORM, 8, 8, "BC3 8x8 exact multiple"},
      {RI_FORMAT_BC3_RGBA_UNORM, 16, 16, "BC3 16x16 exact multiple"},
      {RI_FORMAT_BC3_RGBA_UNORM, 16, 4, "BC3 16x4 exact multiple"},

      {RI_FORMAT_BC1_RGBA_UNORM, 1023, 17, "BC1 1023x17"},
      {RI_FORMAT_BC3_RGBA_UNORM, 1023, 17, "BC3 1023x17"},

      {RI_FORMAT_RGBA8_UNORM, 5, 1, "RGBA8 5 texels with blockDim 1"},
  };

  const RIFormatProps *rgba8Props = GetRIFormatProps(RI_FORMAT_RGBA8_UNORM);
  if (!Check(rgba8Props != nullptr && rgba8Props->blockWidth == 1 &&
                 rgba8Props->blockHeight == 1 && rgba8Props->stride == 4,
             "RGBA8 format props use one 4-byte texel")) {
    return false;
  }

  for (const LayoutCase &testCase : cases) {
    if (!CheckLayout(testCase)) {
      return false;
    }
  }

  std::printf("BC block layout checks passed\n");
  return true;
}
