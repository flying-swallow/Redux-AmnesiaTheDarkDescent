#ifndef HPL_CUBE_MIP_GEN_H
#define HPL_CUBE_MIP_GEN_H

#include <cstdint>
#include <vector>

namespace hpl {

// Texel layout understood by the dependency-free CPU cube-mip generator.
struct CubeMipTexelLayout {
  uint32_t channelCount = 4;
  uint32_t bytesPerChannel = 1;
  bool sRGB = false;
};

bool CubeMipGenSupportsLayout(const CubeMipTexelLayout &layout);

// Builds mip levels 1 through mipCount - 1 for six tightly packed square cube
// faces in the standard +X, -X, +Y, -Y, +Z, -Z order.
bool GenerateSeamAwareCubeMips(const uint8_t *const faces[6],
                               uint32_t faceSize,
                               const CubeMipTexelLayout &layout,
                               uint32_t mipCount,
                               std::vector<std::vector<uint8_t>> &outLevels);

} // namespace hpl

#endif // HPL_CUBE_MIP_GEN_H
