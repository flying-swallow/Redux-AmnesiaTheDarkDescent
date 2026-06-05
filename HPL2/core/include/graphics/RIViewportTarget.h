/**
 * Copyright 2026 Michael Pollind
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <cstdint>
#include <memory>

namespace hpl {

class Image;
struct HPLTexture;

// Offscreen destination for a cViewport (editor panes, thumbnails, material
// previews). The renderer keeps using the global overscan intermediates
// (RI.renderPogo / RI.depthView / RI.visibilityView — all sized >= any
// viewport, since panes fit inside the window and the overscan extent exceeds
// the swapchain); only the tail diverges: instead of the guard-band crop into
// the authored RI.pogoBuffer, cHybridRenderer::Draw blits the viewport-sized
// {0,0,w,h} window of the pogo read-half into this target's sampled
// HPLTexture and leaves it SHADER_READ_ONLY for the GUI to sample.
//
// The wrapping `Image` is stable across Resize — the GUI gfx element keeps
// its Image* while Resize swaps the HPLTexture inside it (GuiSet re-resolves
// the descriptor from the Image every draw). Old textures are released
// through HPLTexture_Delete, which defers the GPU frees onto the frame
// freelist, and any frame that sampled the texture pinned it via
// cntx->textureLink — so Resize needs no explicit fencing.
class cViewportTarget {
public:
  cViewportTarget() = default;
  ~cViewportTarget() = default;
  cViewportTarget(const cViewportTarget &) = delete;
  cViewportTarget &operator=(const cViewportTarget &) = delete;

  // (Re)creates the sampled color image at the given extent. No-op when the
  // size is unchanged. Safe to call mid-flight (see class comment).
  void Resize(uint32_t width, uint32_t height);

  bool IsValid() const { return mpTexture != nullptr; }
  uint32_t GetWidth() const { return mlWidth; }
  uint32_t GetHeight() const { return mlHeight; }

  const std::shared_ptr<HPLTexture> &GetTexture() const { return mpTexture; }
  // Stable wrapper for cGui::CreateGfxTexture / cGuiGfxElement. Created on
  // first Resize and reused for the lifetime of the target.
  const std::shared_ptr<Image> &GetImage() const { return mpImage; }

private:
  std::shared_ptr<HPLTexture> mpTexture;
  std::shared_ptr<Image> mpImage;
  uint32_t mlWidth = 0;
  uint32_t mlHeight = 0;
};

} // namespace hpl
