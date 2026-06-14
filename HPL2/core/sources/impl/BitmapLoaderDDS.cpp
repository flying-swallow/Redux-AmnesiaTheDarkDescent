/*
 * Copyright 2026 Michael Pollind
 * SPDX-License-Identifier: GPL-3.0
 */
#include "impl/BitmapLoaderDDS.h"

#include "graphics/Bitmap.h"
#include "system/LowLevelSystem.h"
#include "system/Platform.h"
#include "system/String.h"

#include <vector>

#define TINYDDSLOADER_IMPLEMENTATION
#include "tinyddsloader.h"

namespace hpl {

using tinyddsloader::DDSFile;

//-----------------------------------------------------------------------

static bool ReadFileBytes(const tWString &asFile, std::vector<uint8_t> &out) {
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

// DXGI/BC -> ePixelFormat. Only the formats the engine's ePixelFormat enum +
// the texture-upload path support (DXT1/3/5 and 8-bit RGBA/BGRA). Sets
// abCompressed for the block-compressed cases.
static ePixelFormat DXGIToHPL(DDSFile::DXGIFormat fmt, bool &abCompressed) {
  using F = DDSFile::DXGIFormat;
  abCompressed = false;
  switch (fmt) {
  case F::BC1_Typeless:
  case F::BC1_UNorm:
  case F::BC1_UNorm_SRGB:
    abCompressed = true;
    return ePixelFormat_DXT1;
  case F::BC2_Typeless:
  case F::BC2_UNorm:
  case F::BC2_UNorm_SRGB:
    abCompressed = true;
    return ePixelFormat_DXT3;
  case F::BC3_Typeless:
  case F::BC3_UNorm:
  case F::BC3_UNorm_SRGB:
    abCompressed = true;
    return ePixelFormat_DXT5;
  case F::R8G8B8A8_Typeless:
  case F::R8G8B8A8_UNorm:
  case F::R8G8B8A8_UNorm_SRGB:
    return ePixelFormat_RGBA;
  case F::B8G8R8A8_Typeless:
  case F::B8G8R8A8_UNorm:
  case F::B8G8R8A8_UNorm_SRGB:
    return ePixelFormat_BGRA;
  default:
    return ePixelFormat_Unknown;
  }
}

//-----------------------------------------------------------------------

cBitmapLoaderDDS::cBitmapLoaderDDS() { AddSupportedExtension("dds"); }
cBitmapLoaderDDS::~cBitmapLoaderDDS() {}

//-----------------------------------------------------------------------

cBitmap *cBitmapLoaderDDS::LoadBitmap(const tWString &asFile,
                                      tBitmapLoadFlag aFlags) {
  std::vector<uint8_t> bytes;
  if (!ReadFileBytes(asFile, bytes)) {
    Error("Could not open DDS file '%s' for reading!\n",
          cString::To8Char(asFile).c_str());
    return NULL;
  }

  DDSFile dds;
  if (dds.Load(bytes.data(), bytes.size()) != tinyddsloader::Result::Success) {
    Error("Failed to parse DDS file '%s'!\n",
          cString::To8Char(asFile).c_str());
    return NULL;
  }

  bool bCompressed = false;
  ePixelFormat pixelFormat = DXGIToHPL(dds.GetFormat(), bCompressed);
  if (pixelFormat == ePixelFormat_Unknown) {
    Error("DDS '%s' uses an unsupported format (DXGI %u); only DXT1/3/5 and "
          "8-bit RGBA/BGRA are supported.\n",
          cString::To8Char(asFile).c_str(), (unsigned)dds.GetFormat());
    return NULL;
  }

  // tinyddsloader stores cubemap faces in the array dimension (arraySize == 6).
  const uint32_t lNumOfMipMaps = dds.GetMipCount() > 0 ? dds.GetMipCount() : 1;
  const uint32_t lNumOfImages = dds.GetArraySize() > 0 ? dds.GetArraySize() : 1;

  // ForceNoCompression can't decompress BC blocks here (tinyddsloader doesn't
  // decode); compressed DDS is only loaded compressed. Engine UI assets that
  // request this are .tga/.png, not BC-dds, so warn rather than fail.
  if (bCompressed && (aFlags & eBitmapLoadFlag_ForceNoCompression)) {
    Warning("DDS '%s' is block-compressed but eBitmapLoadFlag_ForceNoCompression "
            "was requested; loading compressed.\n",
            cString::To8Char(asFile).c_str());
  }

  cBitmap *pBitmap = hplNew(cBitmap, ());
  if (lNumOfImages > 1 || lNumOfMipMaps > 1)
    pBitmap->SetUpData((int)lNumOfImages, (int)lNumOfMipMaps);

  pBitmap->SetSize(cVector3l((int)dds.GetWidth(), (int)dds.GetHeight(),
                             (int)(dds.GetDepth() > 0 ? dds.GetDepth() : 1)));
  pBitmap->SetPixelFormat(pixelFormat);
  // Per-pixel byte count is unused by the texture path (it keys on the format);
  // report the uncompressed equivalent for completeness.
  pBitmap->SetBytesPerPixel(bCompressed ? 0 : 4);
  pBitmap->SetIsCompressed(bCompressed);

  for (uint32_t image = 0; image < lNumOfImages; ++image) {
    for (uint32_t mip = 0; mip < lNumOfMipMaps; ++mip) {
      const DDSFile::ImageData *pData = dds.GetImageData(mip, image);
      if (pData == NULL || pData->m_mem == NULL) {
        Error("DDS '%s' missing data for face %u mip %u!\n",
              cString::To8Char(asFile).c_str(), image, mip);
        hplDelete(pBitmap);
        return NULL;
      }
      cBitmapData *pImage = pBitmap->GetData((int)image, (int)mip);
      pImage->SetData((const unsigned char *)pData->m_mem,
                      (int)pData->m_memSlicePitch);
    }
  }

  return pBitmap;
}

//-----------------------------------------------------------------------

bool cBitmapLoaderDDS::SaveBitmap(cBitmap *apBitmap, const tWString &asFile,
                                  tBitmapLoadFlag aFlags) {
  (void)apBitmap;
  (void)aFlags;
  Error("Saving DDS files is not supported ('%s').\n",
        cString::To8Char(asFile).c_str());
  return false;
}

} // namespace hpl
