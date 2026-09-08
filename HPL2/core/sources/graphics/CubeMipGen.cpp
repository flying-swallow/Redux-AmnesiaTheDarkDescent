#include "graphics/CubeMipGen.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace hpl {
namespace {

struct Color {
  float r;
  float g;
  float b;
  float a;
};

struct Direction {
  float x;
  float y;
  float z;
};

float DecodeSrgb(float value) {
  return value <= 0.04045f
             ? value / 12.92f
             : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

float EncodeSrgb(float value) {
  return value <= 0.0031308f
             ? value * 12.92f
             : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}

float ClampUnit(float value) {
  return std::fmin(std::fmax(value, 0.0f), 1.0f);
}

float ReadFloat32(const uint8_t *bytes) {
  union FloatBytes {
    float value;
    uint8_t bytes[4];
  } result;
  for (uint32_t i = 0; i < 4; ++i)
    result.bytes[i] = bytes[i];
  return result.value;
}

void WriteFloat32(uint8_t *bytes, float value) {
  union FloatBytes {
    float value;
    uint8_t bytes[4];
  } source;
  source.value = value;
  for (uint32_t i = 0; i < 4; ++i)
    bytes[i] = source.bytes[i];
}

Color DecodeTexel(const uint8_t *data, uint32_t x, uint32_t y,
                 uint32_t faceSize, const CubeMipTexelLayout &layout) {
  const uint64_t texelIndex = static_cast<uint64_t>(y) * faceSize + x;
  const uint64_t byteOffset =
      texelIndex * layout.channelCount * layout.bytesPerChannel;
  const uint8_t *texel = data + byteOffset;
  Color result = {0.0f, 0.0f, 0.0f, 1.0f};

  if (layout.bytesPerChannel == 1) {
    float *channels[] = {&result.r, &result.g, &result.b, &result.a};
    for (uint32_t channel = 0; channel < layout.channelCount; ++channel) {
      const float value = static_cast<float>(texel[channel]) / 255.0f;
      *channels[channel] = layout.sRGB && channel < 3
                              ? DecodeSrgb(value)
                              : value;
    }
  } else {
    float *channels[] = {&result.r, &result.g, &result.b, &result.a};
    for (uint32_t channel = 0; channel < layout.channelCount; ++channel)
      *channels[channel] = ReadFloat32(texel + channel * 4);
  }
  return result;
}

void EncodeTexel(uint8_t *data, uint32_t x, uint32_t y, uint32_t faceSize,
                 const CubeMipTexelLayout &layout, Color color) {
  const uint64_t texelIndex = static_cast<uint64_t>(y) * faceSize + x;
  const uint64_t byteOffset =
      texelIndex * layout.channelCount * layout.bytesPerChannel;
  uint8_t *texel = data + byteOffset;

  if (layout.bytesPerChannel == 1) {
    const float channels[] = {color.r, color.g, color.b, color.a};
    for (uint32_t channel = 0; channel < layout.channelCount; ++channel) {
      float value = ClampUnit(channels[channel]);
      if (layout.sRGB && channel < 3)
        value = EncodeSrgb(value);
      texel[channel] = static_cast<uint8_t>(
          std::floor(ClampUnit(value) * 255.0f + 0.5f));
    }
  } else {
    const float channels[] = {color.r, color.g, color.b, color.a};
    for (uint32_t channel = 0; channel < layout.channelCount; ++channel)
      WriteFloat32(texel + channel * 4, channels[channel]);
  }
}

Direction DirectionFromFaceUv(uint32_t face, float u, float v) {
  const float s = 2.0f * u - 1.0f;
  const float t = 2.0f * v - 1.0f;
  switch (face) {
  case 0:
    return {1.0f, -t, -s};
  case 1:
    return {-1.0f, -t, s};
  case 2:
    return {s, 1.0f, t};
  case 3:
    return {s, -1.0f, -t};
  case 4:
    return {s, -t, 1.0f};
  default:
    return {-s, -t, -1.0f};
  }
}

void FaceUvFromDirection(Direction direction, uint32_t &face, float &u,
                         float &v) {
  const float absX = std::fabs(direction.x);
  const float absY = std::fabs(direction.y);
  const float absZ = std::fabs(direction.z);
  float s;
  float t;

  // The >= ordering is the cube-spec major-axis tie break. It is important at
  // an edge: using a different tie break from DirectionFromFaceUv changes the
  // face selected by a wrapped fetch exactly where faces meet.
  if (absX >= absY && absX >= absZ) {
    face = direction.x >= 0.0f ? 0 : 1;
    s = direction.x >= 0.0f ? -direction.z / absX
                            : direction.z / absX;
    t = -direction.y / absX;
  } else if (absY >= absZ) {
    face = direction.y >= 0.0f ? 2 : 3;
    s = direction.x / absY;
    t = direction.y >= 0.0f ? direction.z / absY
                            : -direction.z / absY;
  } else {
    face = direction.z >= 0.0f ? 4 : 5;
    s = direction.z >= 0.0f ? direction.x / absZ
                            : -direction.x / absZ;
    t = -direction.y / absZ;
  }

  u = 0.5f * (s + 1.0f);
  v = 0.5f * (t + 1.0f);
}

Color FetchTexel(const uint8_t *const levelFaces[6], uint32_t face,
                int64_t x, int64_t y, uint32_t faceSize,
                const CubeMipTexelLayout &layout) {
  if (x >= 0 && x < static_cast<int64_t>(faceSize) && y >= 0 &&
      y < static_cast<int64_t>(faceSize)) {
    return DecodeTexel(levelFaces[face], static_cast<uint32_t>(x),
                       static_cast<uint32_t>(y), faceSize, layout);
  }

  const float u = (static_cast<float>(x) + 0.5f) /
                  static_cast<float>(faceSize);
  const float v = (static_cast<float>(y) + 0.5f) /
                  static_cast<float>(faceSize);
  const Direction direction = DirectionFromFaceUv(face, u, v);
  uint32_t wrappedFace;
  float wrappedU;
  float wrappedV;
  FaceUvFromDirection(direction, wrappedFace, wrappedU, wrappedV);

  int64_t wrappedX = static_cast<int64_t>(
      std::floor(wrappedU * static_cast<float>(faceSize)));
  int64_t wrappedY = static_cast<int64_t>(
      std::floor(wrappedV * static_cast<float>(faceSize)));
  wrappedX = std::max<int64_t>(
      0, std::min<int64_t>(wrappedX, static_cast<int64_t>(faceSize) - 1));
  wrappedY = std::max<int64_t>(
      0, std::min<int64_t>(wrappedY, static_cast<int64_t>(faceSize) - 1));

  // If x and y both cross an edge, the direction can land at a three-face
  // corner. Clamping selects the nearest in-range corner texel of that face.
  return DecodeTexel(levelFaces[wrappedFace], static_cast<uint32_t>(wrappedX),
                     static_cast<uint32_t>(wrappedY), faceSize, layout);
}

Color BilinearFetch(const uint8_t *const levelFaces[6], uint32_t face,
                    float u, float v, uint32_t faceSize,
                    const CubeMipTexelLayout &layout) {
  const float tx = u * static_cast<float>(faceSize) - 0.5f;
  const float ty = v * static_cast<float>(faceSize) - 0.5f;
  const float floorX = std::floor(tx);
  const float floorY = std::floor(ty);
  const int64_t x0 = static_cast<int64_t>(floorX);
  const int64_t y0 = static_cast<int64_t>(floorY);
  const int64_t x1 = x0 + 1;
  const int64_t y1 = y0 + 1;
  const float fx = tx - floorX;
  const float fy = ty - floorY;

  const Color c00 = FetchTexel(levelFaces, face, x0, y0, faceSize, layout);
  const Color c10 = FetchTexel(levelFaces, face, x1, y0, faceSize, layout);
  const Color c01 = FetchTexel(levelFaces, face, x0, y1, faceSize, layout);
  const Color c11 = FetchTexel(levelFaces, face, x1, y1, faceSize, layout);
  const float topWeight = 1.0f - fy;
  const float bottomWeight = fy;
  const float leftWeight = 1.0f - fx;
  const float rightWeight = fx;
  return {
      (c00.r * leftWeight + c10.r * rightWeight) * topWeight +
          (c01.r * leftWeight + c11.r * rightWeight) * bottomWeight,
      (c00.g * leftWeight + c10.g * rightWeight) * topWeight +
          (c01.g * leftWeight + c11.g * rightWeight) * bottomWeight,
      (c00.b * leftWeight + c10.b * rightWeight) * topWeight +
          (c01.b * leftWeight + c11.b * rightWeight) * bottomWeight,
      (c00.a * leftWeight + c10.a * rightWeight) * topWeight +
          (c01.a * leftWeight + c11.a * rightWeight) * bottomWeight,
  };
}

uint32_t MipFaceSize(uint32_t faceSize, uint64_t mip) {
  return mip >= 31 ? 1 : std::max(1u, faceSize >> mip);
}

bool LevelByteCount(uint32_t faceSize, const CubeMipTexelLayout &layout,
                    std::vector<uint8_t>::size_type &byteCount) {
  const uint64_t bytesPerTexel =
      static_cast<uint64_t>(layout.channelCount) * layout.bytesPerChannel;
  const uint64_t pixelCount = static_cast<uint64_t>(faceSize) * faceSize;
  const uint64_t maxBytes =
      static_cast<uint64_t>(std::vector<uint8_t>().max_size());
  if (bytesPerTexel == 0 || pixelCount > maxBytes / bytesPerTexel)
    return false;
  byteCount = static_cast<std::vector<uint8_t>::size_type>(
      pixelCount * bytesPerTexel);
  return true;
}

} // namespace

bool CubeMipGenSupportsLayout(const CubeMipTexelLayout &layout) {
  return layout.channelCount >= 1 && layout.channelCount <= 4 &&
         (layout.bytesPerChannel == 1 || layout.bytesPerChannel == 4) &&
         (layout.bytesPerChannel == 1 || !layout.sRGB);
}

bool GenerateSeamAwareCubeMips(const uint8_t *const faces[6],
                               uint32_t faceSize,
                               const CubeMipTexelLayout &layout,
                               uint32_t mipCount,
                               std::vector<std::vector<uint8_t>> &outLevels) {
  outLevels.clear();
  if (!faces || !CubeMipGenSupportsLayout(layout) || faceSize == 0 ||
      (faceSize & (faceSize - 1)) != 0 || mipCount < 2)
    return false;
  for (uint32_t face = 0; face < 6; ++face) {
    if (!faces[face])
      return false;
  }

  const uint64_t levelCount = (static_cast<uint64_t>(mipCount) - 1) * 6;
  std::vector<std::vector<uint8_t>> levels;
  if (levelCount > levels.max_size())
    return false;
  levels.resize(static_cast<std::vector<std::vector<uint8_t>>::size_type>(
      levelCount));

  const uint8_t *previousFaces[6] = {
      faces[0], faces[1], faces[2], faces[3], faces[4], faces[5]};
  uint32_t previousSize = faceSize;

  for (uint64_t mip = 1; mip < mipCount; ++mip) {
    const uint32_t destinationSize = MipFaceSize(faceSize, mip);
    std::vector<uint8_t>::size_type byteCount;
    if (!LevelByteCount(destinationSize, layout, byteCount))
      return false;

    const uint64_t levelOffset = (mip - 1) * 6;
    for (uint32_t face = 0; face < 6; ++face)
      levels[levelOffset + face].resize(byteCount);

    for (uint32_t face = 0; face < 6; ++face) {
      uint8_t *destination = levels[levelOffset + face].data();
      for (uint32_t y = 0; y < destinationSize; ++y) {
        for (uint32_t x = 0; x < destinationSize; ++x) {
          Color result = {0.0f, 0.0f, 0.0f, 0.0f};
          for (uint32_t tapY = 0; tapY < 2; ++tapY) {
            for (uint32_t tapX = 0; tapX < 2; ++tapX) {
              const float u =
                  (static_cast<float>(x) + 0.25f + 0.5f * tapX) /
                  static_cast<float>(destinationSize);
              const float v =
                  (static_cast<float>(y) + 0.25f + 0.5f * tapY) /
                  static_cast<float>(destinationSize);
              const Direction direction = DirectionFromFaceUv(face, u, v);
              uint32_t sampleFace;
              float sampleU;
              float sampleV;
              FaceUvFromDirection(direction, sampleFace, sampleU, sampleV);
              const Color sample = BilinearFetch(
                  previousFaces, sampleFace, sampleU, sampleV, previousSize,
                  layout);
              result.r += sample.r * 0.25f;
              result.g += sample.g * 0.25f;
              result.b += sample.b * 0.25f;
              result.a += sample.a * 0.25f;
            }
          }
          EncodeTexel(destination, x, y, destinationSize, layout, result);
        }
      }
    }

    for (uint32_t face = 0; face < 6; ++face)
      previousFaces[face] = levels[levelOffset + face].data();
    previousSize = destinationSize;
  }

  outLevels.swap(levels);
  return true;
}

} // namespace hpl
