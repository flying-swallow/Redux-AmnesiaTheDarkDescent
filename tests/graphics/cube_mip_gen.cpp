#include "graphics/CubeMipGen.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

bool Check(bool condition, const char *name) {
  if (!condition) {
    std::printf("failed: %s\n", name);
    return false;
  }
  return true;
}

bool CheckFloatClose(float actual, float expected, float tolerance,
                     const char *name) {
  return Check(std::fabs(actual - expected) <= tolerance, name);
}

uint8_t ReadByte(const std::vector<std::vector<uint8_t>> &levels,
                 uint32_t mip, uint32_t face, uint32_t x, uint32_t y,
                 uint32_t faceSize, uint32_t channel = 0) {
  const uint32_t size = std::max(1u, faceSize >> mip);
  const uint64_t index =
      (static_cast<uint64_t>(y) * size + x) * 4 + channel;
  return levels[(mip - 1) * 6 + face][index];
}

void FillFaces(std::vector<std::vector<uint8_t>> &storage,
               const uint8_t *faces[6], uint32_t faceSize,
               const uint8_t values[6]) {
  storage.resize(6);
  for (uint32_t face = 0; face < 6; ++face) {
    storage[face].resize(static_cast<size_t>(faceSize) * faceSize * 4);
    for (uint32_t y = 0; y < faceSize; ++y) {
      for (uint32_t x = 0; x < faceSize; ++x) {
        const size_t index =
            (static_cast<size_t>(y) * faceSize + x) * 4;
        storage[face][index + 0] = values[face];
        storage[face][index + 1] = values[face];
        storage[face][index + 2] = values[face];
        storage[face][index + 3] = 255;
      }
    }
    faces[face] = storage[face].data();
  }
}

bool CheckFaceDirectionRoundTrip() {
  // This mirrors the cube spec equations used by the generator so the
  // sampled directions cover every face, edge-near region, and both axes.
  const float samples[] = {0.07f, 0.23f, 0.41f, 0.59f, 0.77f, 0.93f};
  for (uint32_t face = 0; face < 6; ++face) {
    for (const float u : samples) {
      for (const float v : samples) {
        const float s = 2.0f * u - 1.0f;
        const float t = 2.0f * v - 1.0f;
        float dx;
        float dy;
        float dz;
        switch (face) {
        case 0:
          dx = 1.0f;
          dy = -t;
          dz = -s;
          break;
        case 1:
          dx = -1.0f;
          dy = -t;
          dz = s;
          break;
        case 2:
          dx = s;
          dy = 1.0f;
          dz = t;
          break;
        case 3:
          dx = s;
          dy = -1.0f;
          dz = -t;
          break;
        case 4:
          dx = s;
          dy = -t;
          dz = 1.0f;
          break;
        default:
          dx = -s;
          dy = -t;
          dz = -1.0f;
          break;
        }

        const float ax = std::fabs(dx);
        const float ay = std::fabs(dy);
        const float az = std::fabs(dz);
        uint32_t resolvedFace;
        float resolvedS;
        float resolvedT;
        if (ax >= ay && ax >= az) {
          resolvedFace = dx >= 0.0f ? 0 : 1;
          resolvedS = dx >= 0.0f ? -dz / ax : dz / ax;
          resolvedT = -dy / ax;
        } else if (ay >= az) {
          resolvedFace = dy >= 0.0f ? 2 : 3;
          resolvedS = dx / ay;
          resolvedT = dy >= 0.0f ? dz / ay : -dz / ay;
        } else {
          resolvedFace = dz >= 0.0f ? 4 : 5;
          resolvedS = dz >= 0.0f ? dx / az : -dx / az;
          resolvedT = -dy / az;
        }
        if (!Check(resolvedFace == face &&
                       std::fabs(0.5f * (resolvedS + 1.0f) - u) < 1.0e-6f &&
                       std::fabs(0.5f * (resolvedT + 1.0f) - v) < 1.0e-6f,
                   "cube face UV direction round trip is stable"))
          return false;
      }
    }
  }
  return true;
}

bool CheckConstantCube() {
  const uint32_t faceSize = 8;
  const uint8_t values[6] = {73, 73, 73, 73, 73, 73};
  std::vector<std::vector<uint8_t>> source;
  const uint8_t *faces[6] = {};
  FillFaces(source, faces, faceSize, values);
  hpl::CubeMipTexelLayout layout;
  std::vector<std::vector<uint8_t>> levels;
  if (!Check(hpl::GenerateSeamAwareCubeMips(faces, faceSize, layout, 6,
                                            levels),
             "constant cube mip generation succeeds") ||
      !Check(levels.size() == 30, "constant cube has all requested levels"))
    return false;

  for (uint32_t mip = 1; mip < 6; ++mip) {
    const uint32_t size = std::max(1u, faceSize >> mip);
    for (uint32_t face = 0; face < 6; ++face) {
      for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
          if (!Check(ReadByte(levels, mip, face, x, y, faceSize) == 73,
                     "constant cube stays constant through wrapped taps"))
            return false;
        }
      }
    }
  }
  return CheckFaceDirectionRoundTrip();
}

bool CheckSeamColourMixing() {
  const uint32_t faceSize = 8;
  const uint8_t values[6] = {32, 32, 32, 32, 32, 224};
  std::vector<std::vector<uint8_t>> source;
  const uint8_t *faces[6] = {};
  FillFaces(source, faces, faceSize, values);
  hpl::CubeMipTexelLayout layout;
  std::vector<std::vector<uint8_t>> levels;
  if (!Check(hpl::GenerateSeamAwareCubeMips(faces, faceSize, layout, 5,
                                            levels),
             "seam colour cube mip generation succeeds"))
    return false;

  // +X and -Z share the +X right edge. Normal 8-to-4 filtering keeps the
  // centre texel on +X at its own colour. The later 2-to-1 and 1-to-1 passes
  // exercise the wrapped edge taps, so the final +X texel includes -Z.
  const uint8_t centre = ReadByte(levels, 1, 0, 1, 1, faceSize);
  const uint8_t edge = ReadByte(levels, 4, 0, 0, 0, faceSize);
  return Check(centre == values[0],
               "a face-centre texel keeps its own constant colour") &&
         Check(edge > values[0] && edge < values[5],
               "a seam texel is strictly between adjacent face colours");
}

bool CheckSrgbAverage() {
  const uint32_t faceSize = 2;
  std::vector<std::vector<uint8_t>> source(6,
                                           std::vector<uint8_t>(2 * 2 * 4));
  const uint8_t *faces[6] = {};
  for (uint32_t face = 0; face < 6; ++face) {
    const uint8_t values[4] = {0, 255, 255, 0};
    for (uint32_t texel = 0; texel < 4; ++texel) {
      for (uint32_t channel = 0; channel < 3; ++channel)
        source[face][texel * 4 + channel] = values[texel];
      source[face][texel * 4 + 3] = 255;
    }
    faces[face] = source[face].data();
  }

  hpl::CubeMipTexelLayout layout;
  layout.sRGB = true;
  std::vector<std::vector<uint8_t>> levels;
  if (!Check(hpl::GenerateSeamAwareCubeMips(faces, faceSize, layout, 2,
                                            levels),
             "sRGB cube mip generation succeeds"))
    return false;
  const uint8_t value = levels[0][0];
  return CheckFloatClose(static_cast<float>(value), 188.0f, 1.0f,
                         "linear half encodes near sRGB 188") &&
         Check(std::abs(static_cast<int>(value) - 188) <
                   std::abs(static_cast<int>(value) - 128),
               "sRGB averaging happens in linear space");
}

bool CheckSizingAndRejection() {
  const uint32_t faceSize = 4;
  const uint8_t values[6] = {1, 2, 3, 4, 5, 6};
  std::vector<std::vector<uint8_t>> source;
  const uint8_t *faces[6] = {};
  FillFaces(source, faces, faceSize, values);
  hpl::CubeMipTexelLayout layout;
  std::vector<std::vector<uint8_t>> levels;
  if (!Check(hpl::GenerateSeamAwareCubeMips(faces, faceSize, layout, 4,
                                            levels),
             "sizing cube mip generation succeeds") ||
      !Check(levels.size() == (4 - 1) * 6,
             "output level count follows mip count"))
    return false;

  for (uint32_t mip = 1; mip < 4; ++mip) {
    const uint32_t size = std::max(1u, faceSize >> mip);
    for (uint32_t face = 0; face < 6; ++face) {
      if (!Check(levels[(mip - 1) * 6 + face].size() ==
                     static_cast<size_t>(size) * size * 4,
                 "output level byte size is tightly packed"))
        return false;
    }
  }

  hpl::CubeMipTexelLayout unsupported = layout;
  unsupported.bytesPerChannel = 2;
  levels.resize(1);
  if (!Check(!hpl::GenerateSeamAwareCubeMips(faces, faceSize, unsupported, 2,
                                             levels) &&
                 levels.empty(),
             "unsupported channel width is rejected"))
    return false;
  levels.resize(1);
  if (!Check(!hpl::GenerateSeamAwareCubeMips(faces, 3, layout, 2, levels) &&
                 levels.empty(),
             "non-power-of-two face size is rejected"))
    return false;
  levels.resize(1);
  if (!Check(!hpl::GenerateSeamAwareCubeMips(faces, faceSize, layout, 1,
                                             levels) &&
                 levels.empty(),
             "single-level request is rejected"))
    return false;
  return true;
}

} // namespace

bool RunCubeMipGenTests() {
  if (!CheckConstantCube() || !CheckSeamColourMixing() ||
      !CheckSrgbAverage() || !CheckSizingAndRejection())
    return false;

  std::printf("cube mip generation checks passed\n");
  return true;
}
