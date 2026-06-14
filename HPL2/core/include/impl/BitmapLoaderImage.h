/*
 * Copyright 2026 Michael Pollind
 * SPDX-License-Identifier: GPL-3.0
 *
 * Uncompressed image loader (replaces the DevIL "misc" path): PNG via libpng,
 * JPEG via libjpeg-turbo, TGA via an in-tree decoder. Produces an uncompressed
 * cBitmap. SaveBitmap writes PNG (used by the editor thumbnail builders).
 */
#ifndef HPL_BITMAP_LOADER_IMAGE_H
#define HPL_BITMAP_LOADER_IMAGE_H

#include "resources/BitmapLoader.h"

namespace hpl {

class cBitmapLoaderImage : public iBitmapLoader {
public:
  cBitmapLoaderImage();
  ~cBitmapLoaderImage();

  cBitmap *LoadBitmap(const tWString &asFile, tBitmapLoadFlag aFlags) override;
  bool SaveBitmap(cBitmap *apBitmap, const tWString &asFile,
                  tBitmapLoadFlag aFlags) override;
};

} // namespace hpl
#endif // HPL_BITMAP_LOADER_IMAGE_H
