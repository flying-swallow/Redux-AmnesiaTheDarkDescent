#ifndef HPL_BLOCK_COMPRESSION_DECODE_H
#define HPL_BLOCK_COMPRESSION_DECODE_H

#include "graphics/RIFormat.h"

#include <cstddef>
#include <cstdint>

namespace hpl {

// True for the BC formats implemented by the small CPU fallback decoder.
bool BC_FormatIsDecodable(RI_Format format);

// Return the corresponding uncompressed RGBA8 format, preserving sRGB.
RI_Format BC_DecodedFormat(RI_Format format);

// Required destination byte count for a tightly packed RGBA8 surface.
size_t BC_DecodedSizeBytes(uint32_t width, uint32_t height);

// Decode one BC1 or BC3 surface to tightly packed row-major RGBA8.
bool BC_DecodeToRGBA8(RI_Format format, const unsigned char* src, size_t srcSize,
                      uint32_t width, uint32_t height,
                      unsigned char* dst, size_t dstSize);

} // namespace hpl

#endif // HPL_BLOCK_COMPRESSION_DECODE_H
