/*
 * Copyright 2026 Michael Pollind
 * SPDX-License-Identifier: GPL-3.0
 *
 * DDS bitmap loader backed by tinyddsloader (replaces the DevIL DDS path).
 * Loads compressed BC1/BC2/BC3 (DXT1/3/5) and uncompressed RGBA/BGRA DDS,
 * including mip chains and cubemap faces, straight into a cBitmap.
 */
#ifndef HPL_BITMAP_LOADER_DDS_H
#define HPL_BITMAP_LOADER_DDS_H

#include "resources/BitmapLoader.h"

namespace hpl {

class cBitmapLoaderDDS : public iBitmapLoader {
public:
  cBitmapLoaderDDS();
  ~cBitmapLoaderDDS();

  cBitmap *LoadBitmap(const tWString &asFile, tBitmapLoadFlag aFlags) override;
  bool SaveBitmap(cBitmap *apBitmap, const tWString &asFile,
                  tBitmapLoadFlag aFlags) override;
};

} // namespace hpl
#endif // HPL_BITMAP_LOADER_DDS_H
