/*
 * Copyright © 2009-2020 Frictional Games
 *
 * This file is part of Amnesia: The Dark Descent.
 *
 * Amnesia: The Dark Descent is free software: you can redistribute it and/or
 modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * Amnesia: The Dark Descent is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Amnesia: The Dark Descent.  If not, see
 <https://www.gnu.org/licenses/>.
 */

#ifndef HPL_VIEWPORT_H
#define HPL_VIEWPORT_H

#include "graphics/GraphicsTypes.h"
#include "graphics/RIBootstrap.h"
#include "graphics/RIPogoBuffer.h"
#include "gui/GuiTypes.h"
#include "math/MathTypes.h"
#include "scene/SceneTypes.h"
#include "system/Event.h"

#include <memory>
#include <type_traits>
#include <variant>
#include <vector>

namespace hpl {

//------------------------------------------

class cScene;
class cCamera;
class iRenderer;
class cRenderSettings;
class cPostEffectComposite;
class cWorld;
class cGuiSet;
class Image;
struct HPLTexture;

//------------------------------------------

// Texture helpers the viewport-state backends build their targets with
// (Update/Dispose impls live in the renderer .cpps). Create lives against
// the device; Release hands every GPU handle to the frame freelist (drained
// once the pipeline is done with them — or at RIBootstrap::Dispose).

// One color image + an owning sampled-image descriptor — the single-image
// equivalent of RI_PogoBufferInit (same create flags / view usage so the
// existing pogo-shaped barriers and pipelines keep matching).
bool CreateViewportColorTexture(struct RIDevice_s *device, uint32_t width,
                                uint32_t height, enum RI_Format_e format,
                                VkImageUsageFlags usage,
                                struct RITexture_s *tex,
                                struct RIDescriptor_s *desc, const char *what);
void ReleaseViewportColorTexture(std::vector<struct RIFree> &freelist,
                                 struct RITexture_s *tex,
                                 struct RIDescriptor_s *desc);

// One attachment image + a plain view (depth / visibility targets).
bool CreateViewportAttachmentTexture(struct RIDevice_s *device, uint32_t width,
                                     uint32_t height, enum RI_Format_e format,
                                     VkImageUsageFlags usage,
                                     VkImageAspectFlags aspect,
                                     struct RITexture_s *tex,
                                     struct RITextureView_s *view,
                                     const char *what);
void ReleaseViewportAttachmentTexture(std::vector<struct RIFree> &freelist,
                                      struct RITexture_s *tex,
                                      struct RITextureView_s *view);

//------------------------------------------

class cViewport {
public:
  cViewport(cScene *apScene);
  ~cViewport();

  // Connected event handlers, the backend state, and the pogo buffer are all
  // held by address — a viewport never copies or moves.
  cViewport(const cViewport &) = delete;
  cViewport &operator=(const cViewport &) = delete;
  cViewport(cViewport &&) = delete;
  cViewport &operator=(cViewport &&) = delete;

  // The state's finished color target for the current swapchain image, as
  // the consumer should read it: {x, y, width, height} describe the valid
  // AUTHORED window inside the image — the hybrid backend overdraws by its
  // guard band, so its window is the centered crop; the simple backend is
  // 1:1 (0,0). cScene feeds this window into the viewport pogo in the
  // post-processing step.
  struct BackBuffer {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    struct RITexture_s renderTarget = {};
    struct RIDescriptor_s renderTargetDescriptor = {};
  };

  // cHybridRenderer: overscan intermediates — renderTarget is a SINGLE image
  // (the main draw never ping-pongs; the composite writes once and the
  // forward passes draw on top), depth + packed-TriangleHit visibility back
  // the same overscan frame. Arrays are indexed by RI.swapchainIndex.
  struct HybridViewportState {
    uint32_t width = 0;        // image extent = overscanExtent(target size)
    uint32_t height = 0;
    uint32_t targetWidth = 0;  // authored target size from Update — the
    uint32_t targetHeight = 0; //   BackBuffer crop window

    void Update(RIBootstrap::FrameContext *cntx, cVector2l size);
    void Dispose(RIBootstrap::FrameContext *cntx);
    BackBuffer GetBackBuffer() {
      return {(width - targetWidth) / 2, (height - targetHeight) / 2,
              targetWidth, targetHeight, renderTarget[RI.swapchainIndex],
              renderTargetDescriptor[RI.swapchainIndex]};
    }

    struct RITexture_s renderTarget[RI_MAX_SWAPCHAIN_IMAGES] = {};
    struct RIDescriptor_s renderTargetDescriptor[RI_MAX_SWAPCHAIN_IMAGES] = {};

    struct RITexture_s depthTextures[RI_MAX_SWAPCHAIN_IMAGES] = {};
    struct RITextureView_s depthView[RI_MAX_SWAPCHAIN_IMAGES] = {};

    struct RITexture_s visibilityTexture[RI_MAX_SWAPCHAIN_IMAGES] = {};
    struct RITextureView_s visibilityView[RI_MAX_SWAPCHAIN_IMAGES] = {};
  };

  // cRendererWireFrame + cRendererSimple (identical needs): they draw 1:1
  // into their own color render target and only need a matching depth
  // attachment.
  struct SimpleViewportState {
    uint32_t width = 0;
    uint32_t height = 0;

    void Update(RIBootstrap::FrameContext *cntx, cVector2l size);
    void Dispose(RIBootstrap::FrameContext *cntx);
    BackBuffer GetBackBuffer() {
      return {0, 0, width, height, renderTarget[RI.swapchainIndex],
              renderTargetDescriptor[RI.swapchainIndex]};
    }

    struct RITexture_s renderTarget[RI_MAX_SWAPCHAIN_IMAGES] = {};
    struct RIDescriptor_s renderTargetDescriptor[RI_MAX_SWAPCHAIN_IMAGES] = {};

    struct RITexture_s depthTextures[RI_MAX_SWAPCHAIN_IMAGES] = {};
    struct RITextureView_s depthView[RI_MAX_SWAPCHAIN_IMAGES] = {};
  };

  using ViewportState =
      std::variant<std::monostate, HybridViewportState, SimpleViewportState>;

  // Where the viewport's finished image is delivered by cScene after the
  // viewport is fully evaluated (world draw + post processing):
  //  - TargetSwapchain: composited to the swapchain (tail blit + GUI block);
  //    the viewport extent follows the swapchain. At most one visible
  //    viewport may target the swapchain (asserted by GetPrimaryViewport).
  //  - TargetView: standalone at the given extent; when `view` is non-null
  //    the pogo read half is drawn into it at the tail and left
  //    SHADER_READ_ONLY for the consumer (editor panes sample it).
  struct TargetSwapchain {};
  struct TargetView {
    uint32_t width;
    uint32_t height;
    // Caller-owned color destination (e.g. the editor's pane texture):
    // cScene's delivery renders the viewport's finished pogo read half into
    // `view` and leaves the image SHADER_READ_ONLY for the consumer.
    // `texture` is the view's backing image — needed for the layout
    // transitions around the delivery draw.
    struct RITexture_s texture;
    RITextureView_s view;
  };
  using Target = std::variant<TargetSwapchain, TargetView>;

  void SetActive(bool abX) { mbActive = abX; }
  void SetVisible(bool abX) { mbVisible = abX; }
  bool IsActive() { return mbActive; }
  bool IsVisible() { return mbVisible; }

  void SetIsListener(bool abX) { mbIsListener = abX; }
  bool IsListener() { return mbIsListener; }

  void SetCamera(cCamera *apCamera) { mpCamera = apCamera; }
  cCamera *GetCamera() { return mpCamera; }

  void SetWorld(cWorld *apWorld);
  cWorld *GetWorld() { return mpWorld; }

  void SetRenderer(iRenderer *apRenderer) { mpRenderer = apRenderer; }
  iRenderer *GetRenderer() { return mpRenderer; }

  cRenderSettings *GetRenderSettings() { return mpRenderSettings.get(); }

  void SetPostEffectComposite(cPostEffectComposite *apPostEffectComposite) {
    mpPostEffectComposite = apPostEffectComposite;
  }
  cPostEffectComposite *GetPostEffectComposite() {
    return mpPostEffectComposite;
  }

  void AddGuiSet(cGuiSet *apSet);
  void RemoveGuiSet(cGuiSet *apSet);
  const std::vector<cGuiSet *> &GetGuiSets() const { return m_guiSets; }

  // Per-viewport pogo (ping-pong), sized to GetTargetSize(). Resizes hand
  // the old halves to the frame freelist — no stall.
  RI_PogoBuffer *PreparePogoBuffer(RIBootstrap::FrameContext *cntx);
  // Read-only: the existing pogo, or nullptr if none was created yet (no
  // world evaluated at this viewport so far).
  RI_PogoBuffer *PogoBuffer() { return mlPogoWidth != 0 ? &mPogoBuffer : nullptr; }

  void SetTarget(const Target &aTarget) { mTarget = aTarget; }
  const Target &GetTarget() const { return mTarget; }
  cVector2l GetTargetSize() const;

  struct RITextureView_s *GetDepthView();
  BackBuffer GetBackBuffer();
  Event<> &OnPreWorldDraw() { return m_onPreWorldDraw; }

  template <typename Backend>
  Backend *PrepareToRender(RIBootstrap::FrameContext *cntx) {
    const cVector2l size = GetTargetSize();
    if (!std::holds_alternative<Backend>(m_state)) {
      std::visit(
          [cntx](auto &&arg) {
            using T = std::decay_t<decltype(arg)>; // Get the clean type

            if constexpr (!std::is_same_v<T, std::monostate>) {
              arg.Dispose(cntx);
            }
          },
          m_state);
      m_state.emplace<Backend>();
    }
    Backend &state = std::get<Backend>(m_state);
    state.Update(cntx, size);
    return &state;
  }

private:
  // Hands the pogo halves' GPU resources to the frame freelist.
  void ReleasePogoBuffer(RIBootstrap::FrameContext *cntx);

  cScene *mpScene;

  cCamera *mpCamera;
  cWorld *mpWorld;

  bool mbActive;
  bool mbVisible;

  bool mbIsListener;

  iRenderer *mpRenderer;
  cPostEffectComposite *mpPostEffectComposite;

  RI_PogoBuffer mPogoBuffer = {};
  uint32_t mlPogoWidth = 0;
  uint32_t mlPogoHeight = 0;

  Event<> m_onPreWorldDraw;
  std::vector<cGuiSet *> m_guiSets;

  ViewportState m_state;
  Target mTarget = TargetSwapchain{};
  std::unique_ptr<cRenderSettings> mpRenderSettings;
};

//------------------------------------------

}; // namespace hpl
#endif // HPL_VIEWPORT_H
