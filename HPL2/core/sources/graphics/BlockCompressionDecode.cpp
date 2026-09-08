#include "graphics/BlockCompressionDecode.h"

#include <algorithm>
#include <limits>

namespace hpl {
namespace {

constexpr size_t kBC1BlockBytes = 8;
constexpr size_t kBC3BlockBytes = 16;

bool IsBC1(RI_Format format) {
  return format == RI_FORMAT_BC1_RGBA_UNORM ||
         format == RI_FORMAT_BC1_RGBA_SRGB;
}

bool IsBC3(RI_Format format) {
  return format == RI_FORMAT_BC3_RGBA_UNORM ||
         format == RI_FORMAT_BC3_RGBA_SRGB;
}

uint16_t ReadLE16(const unsigned char* data) {
  return static_cast<uint16_t>(data[0]) |
         static_cast<uint16_t>(data[1]) << 8;
}

uint32_t ReadLE32(const unsigned char* data) {
  return static_cast<uint32_t>(data[0]) |
         static_cast<uint32_t>(data[1]) << 8 |
         static_cast<uint32_t>(data[2]) << 16 |
         static_cast<uint32_t>(data[3]) << 24;
}

uint8_t Expand5(uint32_t value) {
  return static_cast<uint8_t>((value << 3) | (value >> 2));
}

uint8_t Expand6(uint32_t value) {
  return static_cast<uint8_t>((value << 2) | (value >> 4));
}

void Decode565(uint16_t value, unsigned char* color) {
  color[0] = Expand5((value >> 11) & 0x1f);
  color[1] = Expand6((value >> 5) & 0x3f);
  color[2] = Expand5(value & 0x1f);
  color[3] = 255;
}

void DecodeColorPalette(const unsigned char* block, bool forceFourColorMode,
                        unsigned char colors[4][4]) {
  const uint16_t color0 = ReadLE16(block);
  const uint16_t color1 = ReadLE16(block + 2);
  Decode565(color0, colors[0]);
  Decode565(color1, colors[1]);

  if (forceFourColorMode || color0 > color1) {
    for (unsigned int channel = 0; channel < 3; ++channel) {
      colors[2][channel] = static_cast<unsigned char>(
          (2 * colors[0][channel] + colors[1][channel]) / 3);
      colors[3][channel] = static_cast<unsigned char>(
          (colors[0][channel] + 2 * colors[1][channel]) / 3);
    }
    colors[2][3] = 255;
    colors[3][3] = 255;
  } else {
    for (unsigned int channel = 0; channel < 3; ++channel) {
      colors[2][channel] = static_cast<unsigned char>(
          (colors[0][channel] + colors[1][channel]) / 2);
      colors[3][channel] = 0;
    }
    colors[2][3] = 255;
    // BC1's color0 <= color1 mode is the one-bit punch-through alpha mode.
    colors[3][3] = 0;
  }
}

void DecodeBC3AlphaPalette(const unsigned char* block,
                           unsigned char alpha[8]) {
  const unsigned int alpha0 = block[0];
  const unsigned int alpha1 = block[1];
  alpha[0] = static_cast<unsigned char>(alpha0);
  alpha[1] = static_cast<unsigned char>(alpha1);

  if (alpha0 > alpha1) {
    alpha[2] = static_cast<unsigned char>((6 * alpha0 + alpha1) / 7);
    alpha[3] = static_cast<unsigned char>((5 * alpha0 + 2 * alpha1) / 7);
    alpha[4] = static_cast<unsigned char>((4 * alpha0 + 3 * alpha1) / 7);
    alpha[5] = static_cast<unsigned char>((3 * alpha0 + 4 * alpha1) / 7);
    alpha[6] = static_cast<unsigned char>((2 * alpha0 + 5 * alpha1) / 7);
    alpha[7] = static_cast<unsigned char>((alpha0 + 6 * alpha1) / 7);
  } else {
    alpha[2] = static_cast<unsigned char>((4 * alpha0 + alpha1) / 5);
    alpha[3] = static_cast<unsigned char>((3 * alpha0 + 2 * alpha1) / 5);
    alpha[4] = static_cast<unsigned char>((2 * alpha0 + 3 * alpha1) / 5);
    alpha[5] = static_cast<unsigned char>((alpha0 + 4 * alpha1) / 5);
    alpha[6] = 0;
    alpha[7] = 255;
  }
}

bool CheckedMultiply(size_t left, size_t right, size_t* result) {
  if (right != 0 && left > std::numeric_limits<size_t>::max() / right) {
    return false;
  }
  *result = left * right;
  return true;
}

} // namespace

bool BC_FormatIsDecodable(RI_Format format) {
  return IsBC1(format) || IsBC3(format);
}

RI_Format BC_DecodedFormat(RI_Format format) {
  switch (format) {
  case RI_FORMAT_BC1_RGBA_SRGB:
  case RI_FORMAT_BC3_RGBA_SRGB:
    return RI_FORMAT_RGBA8_SRGB;
  case RI_FORMAT_BC1_RGBA_UNORM:
  case RI_FORMAT_BC3_RGBA_UNORM:
    return RI_FORMAT_RGBA8_UNORM;
  default:
    return RI_FORMAT_UNKNOWN;
  }
}

size_t BC_DecodedSizeBytes(uint32_t width, uint32_t height) {
  size_t pixelCount = 0;
  if (!CheckedMultiply(static_cast<size_t>(width),
                       static_cast<size_t>(height), &pixelCount)) {
    return std::numeric_limits<size_t>::max();
  }

  size_t byteCount = 0;
  if (!CheckedMultiply(pixelCount, 4, &byteCount)) {
    return std::numeric_limits<size_t>::max();
  }
  return byteCount;
}

bool BC_DecodeToRGBA8(RI_Format format, const unsigned char* src,
                      size_t srcSize, uint32_t width, uint32_t height,
                      unsigned char* dst, size_t dstSize) {
  if (!BC_FormatIsDecodable(format)) {
    return false;
  }

  const size_t decodedSize = BC_DecodedSizeBytes(width, height);
  if (decodedSize == std::numeric_limits<size_t>::max() ||
      dstSize < decodedSize) {
    return false;
  }

  const size_t blocksWide = (static_cast<size_t>(width) + 3) / 4;
  const size_t blocksHigh = (static_cast<size_t>(height) + 3) / 4;
  size_t blockCount = 0;
  if (!CheckedMultiply(blocksWide, blocksHigh, &blockCount)) {
    return false;
  }

  const size_t blockBytes = IsBC3(format) ? kBC3BlockBytes : kBC1BlockBytes;
  size_t sourceSize = 0;
  if (!CheckedMultiply(blockCount, blockBytes, &sourceSize) ||
      srcSize < sourceSize) {
    return false;
  }
  if ((decodedSize != 0 && dst == nullptr) ||
      (sourceSize != 0 && src == nullptr)) {
    return false;
  }

  if (decodedSize == 0) {
    return true;
  }

  for (size_t blockY = 0; blockY < blocksHigh; ++blockY) {
    for (size_t blockX = 0; blockX < blocksWide; ++blockX) {
      const size_t blockIndex = blockY * blocksWide + blockX;
      const unsigned char* block = src + blockIndex * blockBytes;
      unsigned char colors[4][4];
      DecodeColorPalette(block + (IsBC3(format) ? 8 : 0), IsBC3(format),
                         colors);

      unsigned char alpha[8] = {};
      uint64_t alphaIndices = 0;
      if (IsBC3(format)) {
        DecodeBC3AlphaPalette(block, alpha);
        for (unsigned int byte = 0; byte < 6; ++byte) {
          alphaIndices |= static_cast<uint64_t>(block[2 + byte])
                          << (byte * 8);
        }
      }

      const uint32_t colorIndices =
          ReadLE32(block + (IsBC3(format) ? 12 : 4));
      for (unsigned int pixelY = 0; pixelY < 4; ++pixelY) {
        const size_t y = blockY * 4 + pixelY;
        if (y >= height) {
          continue;
        }
        for (unsigned int pixelX = 0; pixelX < 4; ++pixelX) {
          const size_t x = blockX * 4 + pixelX;
          if (x >= width) {
            continue;
          }

          const unsigned int pixelIndex = pixelY * 4 + pixelX;
          const unsigned int colorIndex =
              (colorIndices >> (pixelIndex * 2)) & 0x3;
          unsigned char* output = dst + (y * width + x) * 4;
          output[0] = colors[colorIndex][0];
          output[1] = colors[colorIndex][1];
          output[2] = colors[colorIndex][2];
          output[3] = IsBC3(format)
                          ? alpha[(alphaIndices >> (pixelIndex * 3)) & 0x7]
                          : colors[colorIndex][3];
        }
      }
    }
  }
  return true;
}

} // namespace hpl
