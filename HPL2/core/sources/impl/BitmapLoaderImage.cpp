/*
 * Copyright 2026 Michael Pollind
 * SPDX-License-Identifier: GPL-3.0
 */
#include "impl/BitmapLoaderImage.h"

#include "graphics/Bitmap.h"
#include "system/LowLevelSystem.h"
#include "system/Platform.h"
#include "system/String.h"

#include <csetjmp>
#include <cstring>
#include <vector>

#include <png.h>

extern "C" {
#include <jpeglib.h>
}

namespace hpl {

//-----------------------------------------------------------------------

static bool ReadFileBytes(const tWString &asFile, std::vector<unsigned char> &out) {
  FILE *pFile = cPlatform::OpenFile(asFile, _W("rb"));
  if (pFile == NULL)
    return false;
  fseek(pFile, 0, SEEK_END);
  long lSize = ftell(pFile);
  fseek(pFile, 0, SEEK_SET);
  if (lSize <= 0) {
    fclose(pFile);
    return false;
  }
  out.resize((size_t)lSize);
  size_t lRead = fread(out.data(), 1, (size_t)lSize, pFile);
  fclose(pFile);
  return lRead == (size_t)lSize;
}

// Build a single-image, single-mip uncompressed cBitmap from a decoded buffer.
static cBitmap *MakeBitmap(int alWidth, int alHeight, int alBpp,
                           ePixelFormat aFormat,
                           const unsigned char *apData, int alSize) {
  cBitmap *pBitmap = hplNew(cBitmap, ());
  pBitmap->SetSize(cVector3l(alWidth, alHeight, 1));
  pBitmap->SetPixelFormat(aFormat);
  pBitmap->SetBytesPerPixel((char)alBpp);
  pBitmap->SetIsCompressed(false);
  pBitmap->GetData(0, 0)->SetData(apData, alSize);
  return pBitmap;
}

//-----------------------------------------------------------------------
// PNG (libpng simplified API -> RGBA)

static cBitmap *LoadPNG(const std::vector<unsigned char> &bytes,
                        const tWString &asFile) {
  png_image image;
  memset(&image, 0, sizeof(image));
  image.version = PNG_IMAGE_VERSION;

  if (!png_image_begin_read_from_memory(&image, bytes.data(), bytes.size())) {
    Error("PNG '%s': %s\n", cString::To8Char(asFile).c_str(), image.message);
    return NULL;
  }
  image.format = PNG_FORMAT_RGBA;

  std::vector<unsigned char> buf(PNG_IMAGE_SIZE(image));
  if (!png_image_finish_read(&image, NULL, buf.data(), 0, NULL)) {
    Error("PNG '%s': %s\n", cString::To8Char(asFile).c_str(), image.message);
    png_image_free(&image);
    return NULL;
  }
  int w = (int)image.width, h = (int)image.height;
  png_image_free(&image);
  return MakeBitmap(w, h, 4, ePixelFormat_RGBA, buf.data(), (int)buf.size());
}

//-----------------------------------------------------------------------
// JPEG (libjpeg -> RGB)

struct hplJpegErr {
  struct jpeg_error_mgr pub;
  jmp_buf setjmp_buffer;
};
static void hplJpegErrorExit(j_common_ptr cinfo) {
  longjmp(((hplJpegErr *)cinfo->err)->setjmp_buffer, 1);
}

static cBitmap *LoadJPEG(const std::vector<unsigned char> &bytes,
                         const tWString &asFile) {
  struct jpeg_decompress_struct cinfo;
  hplJpegErr jerr;
  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = hplJpegErrorExit;
  std::vector<unsigned char> buf;
  int w = 0, h = 0;

  if (setjmp(jerr.setjmp_buffer)) {
    jpeg_destroy_decompress(&cinfo);
    Error("JPEG '%s': decode failed\n", cString::To8Char(asFile).c_str());
    return NULL;
  }

  jpeg_create_decompress(&cinfo);
  jpeg_mem_src(&cinfo, bytes.data(), (unsigned long)bytes.size());
  jpeg_read_header(&cinfo, TRUE);
  cinfo.out_color_space = JCS_RGB;
  jpeg_start_decompress(&cinfo);

  w = (int)cinfo.output_width;
  h = (int)cinfo.output_height;
  const int comps = cinfo.output_components; // 3 for JCS_RGB
  buf.resize((size_t)w * h * comps);
  while (cinfo.output_scanline < cinfo.output_height) {
    JSAMPROW row = buf.data() + (size_t)cinfo.output_scanline * w * comps;
    jpeg_read_scanlines(&cinfo, &row, 1);
  }
  jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);
  return MakeBitmap(w, h, comps, ePixelFormat_RGB, buf.data(), (int)buf.size());
}

//-----------------------------------------------------------------------
// TGA (in-tree decoder: uncompressed + RLE, truecolor 24/32 + grayscale 8,
// BGR(A) order kept; rows flipped to top-left origin to match the old DevIL
// IL_ORIGIN_UPPER_LEFT setting).

static cBitmap *LoadTGA(const std::vector<unsigned char> &b,
                        const tWString &asFile) {
  if (b.size() < 18) {
    Error("TGA '%s': truncated header\n", cString::To8Char(asFile).c_str());
    return NULL;
  }
  const uint8_t idLen = b[0];
  const uint8_t cmapType = b[1];
  const uint8_t imgType = b[2];
  const int width = b[12] | (b[13] << 8);
  const int height = b[14] | (b[15] << 8);
  const int bits = b[16];
  const uint8_t descriptor = b[17];
  const bool topLeft = (descriptor & 0x20) != 0;
  const bool rle = (imgType == 10 || imgType == 11);
  const int channels = bits / 8;

  if (cmapType != 0 || width <= 0 || height <= 0 ||
      !(channels == 1 || channels == 3 || channels == 4)) {
    Error("TGA '%s': unsupported variant (type %u, %d-bit, cmap %u)\n",
          cString::To8Char(asFile).c_str(), imgType, bits, cmapType);
    return NULL;
  }

  size_t off = (size_t)18 + idLen;
  const size_t total = (size_t)width * height * channels;
  std::vector<unsigned char> out(total);

  if (!rle) {
    if (off + total > b.size()) {
      Error("TGA '%s': truncated pixel data\n",
            cString::To8Char(asFile).c_str());
      return NULL;
    }
    memcpy(out.data(), b.data() + off, total);
  } else {
    size_t i = off, o = 0;
    while (o < total && i < b.size()) {
      const uint8_t pkt = b[i++];
      const int count = (pkt & 0x7F) + 1;
      if (pkt & 0x80) { // run-length packet: one pixel repeated
        if (i + channels > b.size())
          break;
        for (int c = 0; c < count && o + channels <= total; ++c) {
          memcpy(out.data() + o, b.data() + i, channels);
          o += channels;
        }
        i += channels;
      } else { // raw packet
        size_t n = (size_t)count * channels;
        if (i + n > b.size())
          n = b.size() - i;
        if (o + n > total)
          n = total - o;
        memcpy(out.data() + o, b.data() + i, n);
        o += n;
        i += n;
      }
    }
  }

  if (!topLeft) {
    const size_t rowBytes = (size_t)width * channels;
    std::vector<unsigned char> tmp(rowBytes);
    for (int y = 0; y < height / 2; ++y) {
      unsigned char *a = out.data() + (size_t)y * rowBytes;
      unsigned char *c = out.data() + (size_t)(height - 1 - y) * rowBytes;
      memcpy(tmp.data(), a, rowBytes);
      memcpy(a, c, rowBytes);
      memcpy(c, tmp.data(), rowBytes);
    }
  }

  const ePixelFormat fmt = channels == 1   ? ePixelFormat_Luminance
                           : channels == 3 ? ePixelFormat_BGR
                                           : ePixelFormat_BGRA;
  return MakeBitmap(width, height, channels, fmt, out.data(), (int)total);
}

//-----------------------------------------------------------------------

cBitmapLoaderImage::cBitmapLoaderImage() {
  AddSupportedExtension("jpg");
  AddSupportedExtension("jpeg");
  AddSupportedExtension("png");
  AddSupportedExtension("tga");
}
cBitmapLoaderImage::~cBitmapLoaderImage() {}

//-----------------------------------------------------------------------

cBitmap *cBitmapLoaderImage::LoadBitmap(const tWString &asFile,
                                        tBitmapLoadFlag aFlags) {
  (void)aFlags; // these formats are never block-compressed
  std::vector<unsigned char> bytes;
  if (!ReadFileBytes(asFile, bytes)) {
    Error("Could not open image file '%s' for reading!\n",
          cString::To8Char(asFile).c_str());
    return NULL;
  }

  const tWString ext = cString::ToLowerCaseW(cString::GetFileExtW(asFile));
  if (ext == _W("png"))
    return LoadPNG(bytes, asFile);
  if (ext == _W("jpg") || ext == _W("jpeg"))
    return LoadJPEG(bytes, asFile);
  if (ext == _W("tga"))
    return LoadTGA(bytes, asFile);

  Error("Image '%s' has an unsupported extension!\n",
        cString::To8Char(asFile).c_str());
  return NULL;
}

//-----------------------------------------------------------------------

bool cBitmapLoaderImage::SaveBitmap(cBitmap *apBitmap, const tWString &asFile,
                                    tBitmapLoadFlag aFlags) {
  (void)aFlags;
  const tWString ext = cString::ToLowerCaseW(cString::GetFileExtW(asFile));
  if (ext != _W("png")) {
    Error("Saving image '%s' failed: only PNG save is supported.\n",
          cString::To8Char(asFile).c_str());
    return false;
  }

  png_image image;
  memset(&image, 0, sizeof(image));
  image.version = PNG_IMAGE_VERSION;
  image.width = (png_uint_32)apBitmap->GetWidth();
  image.height = (png_uint_32)apBitmap->GetHeight();
  switch (apBitmap->GetPixelFormat()) {
  case ePixelFormat_RGBA: image.format = PNG_FORMAT_RGBA; break;
  case ePixelFormat_RGB:  image.format = PNG_FORMAT_RGB;  break;
  case ePixelFormat_BGRA: image.format = PNG_FORMAT_BGRA; break;
  case ePixelFormat_BGR:  image.format = PNG_FORMAT_BGR;  break;
  default:
    Error("Saving PNG '%s' failed: unsupported pixel format.\n",
          cString::To8Char(asFile).c_str());
    return false;
  }

  FILE *pFile = cPlatform::OpenFile(asFile, _W("wb"));
  if (pFile == NULL) {
    Error("Could not open '%s' for writing!\n",
          cString::To8Char(asFile).c_str());
    return false;
  }
  int ok = png_image_write_to_stdio(&image, pFile, 0,
                                    apBitmap->GetData(0, 0)->mpData, 0, NULL);
  fclose(pFile);
  if (!ok)
    Error("Failed to write PNG '%s'!\n", cString::To8Char(asFile).c_str());
  return ok != 0;
}

} // namespace hpl
