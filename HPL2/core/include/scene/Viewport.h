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
class cFrustum;
class iRenderer;
class cRenderSettings;
class cPostEffectComposite;
class cWorld;
class cGuiSet;
class Image;
class cViewport;
struct cTexture;

//------------------------------------------

// Per-signal context for the viewport pipeline-stage events. Evaluate builds
// one at each signal site so handlers don't have to capture the firing cViewport
// or reach into the global RI bootstrap for the command buffer / frame state.
//
// Rather than one struct with fields that are null at the stages that don't
// produce them, each event carries ONLY what that stage exposes — a common
// recording/camera base plus stage-specific render handles:
//   - OnPreWorldDraw / OnPostDelivery : WorldDrawCtx          (base only)
//   - OnPostTranslucenceDraw          : PostTranslucenceDrawCtx (+ depthView;
//        the linear-HDR scene is reached via viewport->GetBackBuffer(), the
//        pogo does not exist yet)
//   - OnPostWorldDraw                 : PostWorldDrawCtx        (+ pogo +
//        depthView; pogo read half = final post-processed image)
struct WorldDrawCtx {
  cViewport                 *viewport;   // who fired — replaces a captured mpViewport
  struct RICmd            *cmd;        // = &RI.primary.cmds[0]
  struct RIDevice         *device;     // = &RI.device
  RIBootstrap::FrameContext *frame;      // active set (scratch allocs, freelist)
  cFrustum                  *frustum;
  uint32_t                   width, height;
  uint32_t                   frameIndex;
  float                      frameTime;
};

// After the renderer drew the scene (linear HDR in viewport->GetBackBuffer())
// but BEFORE the feed blit + post chain. Depth is read-only
// (DEPTH_ATTACHMENT_OPTIMAL); the pogo is not created yet.
struct PostTranslucenceDrawCtx : WorldDrawCtx {
  struct RITextureView    *depthView;
};

// After the feed blit + post-effect chain: the pogo read half holds the final
// image and the scene depth is still available.
struct PostWorldDrawCtx : WorldDrawCtx {
  struct RI_PogoBuffer      *pogo;
  struct RITextureView    *depthView;
};

//------------------------------------------

// Texture helpers the viewport-state backends build their targets with
// (Update/Dispose impls live in the renderer .cpps). Create lives against
// the device; Release hands every GPU handle to the frame freelist (drained
// once the pipeline is done with them — or at RIBootstrap::Dispose).

// One color image + an owning sampled-image descriptor — the single-image
// equivalent of RI_PogoBufferInit (same create flags / view usage so the
// existing pogo-shaped barriers and pipelines keep matching).
bool CreateViewportColorTexture(struct RIDevice *device, uint32_t width,
                                uint32_t height, enum RI_Format_e format,
                                VkImageUsageFlags usage,
                                struct RITexture *tex,
                                struct RIDescriptor *desc, const char *what);
void ReleaseViewportColorTexture(std::vector<RIFreeHandle> &freelist,
                                 struct RITexture *tex,
                                 struct RIDescriptor *desc);

// One attachment image + a plain view (depth / visibility targets).
bool CreateViewportAttachmentTexture(struct RIDevice *device, uint32_t width,
                                     uint32_t height, enum RI_Format_e format,
                                     VkImageUsageFlags usage,
                                     VkImageAspectFlags aspect,
                                     struct RITexture *tex,
                                     struct RITextureView *view,
                                     const char *what);
void ReleaseViewportAttachmentTexture(std::vector<RIFreeHandle> &freelist,
                                      struct RITexture *tex,
                                      struct RITextureView *view);

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
    struct RITexture renderTarget = {};
    struct RIDescriptor renderTargetDescriptor = {};
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

    struct RITexture renderTarget[RI_MAX_SWAPCHAIN_IMAGES] = {};
    struct RIDescriptor renderTargetDescriptor[RI_MAX_SWAPCHAIN_IMAGES] = {};

    struct RITexture depthTextures[RI_MAX_SWAPCHAIN_IMAGES] = {};
    struct RITextureView depthView[RI_MAX_SWAPCHAIN_IMAGES] = {};

    struct RITexture visibilityTexture[RI_MAX_SWAPCHAIN_IMAGES] = {};
    struct RITextureView visibilityView[RI_MAX_SWAPCHAIN_IMAGES] = {};

    // Surfel-generation output — full-res HDR storage image written by
    // surfel_generation_pass (set=3, binding=1). One per swapchain image.
    struct RITexture surfelResultTexture[RI_MAX_SWAPCHAIN_IMAGES] = {};
    struct RITextureView surfelResultView[RI_MAX_SWAPCHAIN_IMAGES] = {};

    // Stage B packed visibility — RGBA32UI storage image written by the
    // surfel_vbuffer RT pipeline, sampled by the Stage D / F surfel update
    // and generation passes. Per-frame layout is UNDEFINED → GENERAL →
    // SHADER_READ_ONLY, like the gbuffer outputs.
    struct RITexture packedHitInfoTexture[RI_MAX_SWAPCHAIN_IMAGES] = {};
    struct RITextureView packedHitInfoView[RI_MAX_SWAPCHAIN_IMAGES] = {};

    // Screen-space velocity (motion vectors), RG16F — the gbuffer's 2nd
    // color target; sampled by temporal passes.
    struct RITexture velocityTexture[RI_MAX_SWAPCHAIN_IMAGES] = {};
    struct RITextureView velocityView[RI_MAX_SWAPCHAIN_IMAGES] = {};

    // Direct-lighting accumulation history (RGBA16F ping-pong, kept in
    // GENERAL). [directLightingIndex] is this frame's write target; [^1] is
    // the history the direct pass reprojects. NOT swapchain-indexed (history
    // spans frames). directLightingInit triggers the one-time
    // UNDEFINED→GENERAL + clear — re-armed by Update on resize, since
    // recreation invalidates the history.
    struct RITexture directLightingTexture[2] = {};
    struct RITextureView directLightingView[2] = {};
    // Parallel surface-key ping-pong (viewZ, normal.xyz) for disocclusion
    // rejection; shares directLightingIndex with the colour history above.
    struct RITexture directKeyTexture[2] = {};
    struct RITextureView directKeyView[2] = {};
    uint32_t directLightingIndex = 0;
    bool directLightingInit = false;

    // SVGF-lite à-trous scratch (RGBA16F ping-pong, GENERAL). NOT history — each
    // iteration is fully written before it is read, so these only need a one-time
    // UNDEFINED→GENERAL transition (folded into the directLighting init). The
    // composite samples the final iteration's output instead of directLighting.
    struct RITexture directAtrousTexture[2] = {};
    struct RITextureView directAtrousView[2] = {};

    // ReSTIR DI reservoirs (RGBA32F = packed light index + W + M; exact uint
    // index needs full-float storage). [reservoirHistory] ping-pongs across
    // frames (shares directLightingIndex): DirectLightingPass reads [^1] as the
    // temporal history and DirectSpatialReusePass writes [cur] as next frame's
    // history. [reservoirTemporal] is the intra-frame hand-off from the temporal
    // pass to the spatial pass. All GENERAL, one-time clear with the others.
    struct RITexture reservoirTexture[2] = {};
    struct RITextureView reservoirView[2] = {};
    struct RITexture reservoirTemporalTexture = {};
    struct RITextureView reservoirTemporalView = {};

    // Previous-frame camera for velocity / history reprojection —
    // per-viewport camera state. hasPrevCamera seeds prev = current on the
    // first frame (and after resize, when the histories were invalidated).
    float prevViewMat[16] = {};
    float prevProjMat[16] = {};
    bool hasPrevCamera = false;
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

    struct RITexture renderTarget[RI_MAX_SWAPCHAIN_IMAGES] = {};
    struct RIDescriptor renderTargetDescriptor[RI_MAX_SWAPCHAIN_IMAGES] = {};

    struct RITexture depthTextures[RI_MAX_SWAPCHAIN_IMAGES] = {};
    struct RITextureView depthView[RI_MAX_SWAPCHAIN_IMAGES] = {};
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
    struct RITexture texture;
    RITextureView view;
    // Format of `view` — the delivery draw's color attachment (and pipeline
    // key). Defaults to the pogo format; readback consumers (thumbnails) can
    // hand an RGBA8 target instead.
    VkFormat format = RIBootstrap::PogoColorFormatVk;
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

  // Fully evaluate the viewport for this frame: world draw -> feed (the
  // BackBuffer crop window blitted into the pogo read half) -> post-effect
  // chain -> delivery per Target (TargetView draw / swapchain tail blit; the
  // primary viewport's GUI block stays in cScene::Render). Returns whether
  // the world was rendered (the GUI block keys LOAD vs CLEAR off this).
  //
  // Normally driven by cScene::Render for visible viewports, but callable
  // directly during the command-recording window (OnDraw ->
  // FlushRendering) for HEADLESS viewports outside the visible set — e.g.
  // the editor thumbnail builder. At most ONE Evaluate per viewport per
  // frame: the renderers' initial UNDEFINED backbuffer barriers have an
  // empty before-scope, so a second draw would race the first evaluation's
  // pending feed blit of the shared renderTarget[RI.swapchainIndex].
  bool Evaluate(RIBootstrap::FrameContext *cntx, float afFrameTime, tFlag alFlags);


  struct RITextureView *GetDepthView();
  // The depth+stencil attachment image backing GetDepthView() — for callers
  // that need to issue their own subresource (e.g. stencil-aspect) barriers.
  struct RITexture *GetDepthTexture();
  BackBuffer GetBackBuffer();

  // Pipeline-stage events, signaled by Evaluate. Handlers run inside the
  // frame's command-recording window and may record their own commands into
  // RI.primary.cmds[0] (e.g. a readback copy of the delivered TargetView —
  // see the editor thumbnail builder).
  //  - OnPreWorldDraw: before the renderer Draw (also on world-less frames).
  //  - OnPostTranslucenceDraw: after the renderer Draw but BEFORE the feed
  //    blit + post-effect chain — the BackBuffer (GetBackBuffer()) still holds
  //    the LINEAR-HDR scene and depth is DEPTH_ATTACHMENT_OPTIMAL, so handlers
  //    can draw additive/translucent geometry that bloom + tonemap then
  //    process (e.g. the pickup-flash / enemy glow). pogo is null here (not yet
  //    created). Only signaled when the world was actually rendered.
  //  - OnPostWorldDraw: after the world draw + feed blit + post-effect
  //    chain — the pogo read half holds the final image. Only signaled when
  //    the world was actually rendered this Evaluate.
  //  - OnPostDelivery: after the Target delivery (TargetView left
  //    SHADER_READ_ONLY / swapchain tail-blitted). Only signaled when
  //    delivery actually ran, so handlers never touch a stale target.
  Event<const WorldDrawCtx &> &OnPreWorldDraw() { return m_onPreWorldDraw; }
  Event<const PostTranslucenceDrawCtx &> &OnPostTranslucenceDraw() {
    return m_onPostTranslucenceDraw;
  }
  Event<const PostWorldDrawCtx &> &OnPostWorldDraw() { return m_onPostWorldDraw; }
  Event<const WorldDrawCtx &> &OnPostDelivery() { return m_onPostDelivery; }

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

  Event<const WorldDrawCtx &> m_onPreWorldDraw;
  Event<const PostTranslucenceDrawCtx &> m_onPostTranslucenceDraw;
  Event<const PostWorldDrawCtx &> m_onPostWorldDraw;
  Event<const WorldDrawCtx &> m_onPostDelivery;
  std::vector<cGuiSet *> m_guiSets;

  ViewportState m_state;
  Target mTarget = TargetSwapchain{};
  std::unique_ptr<cRenderSettings> mpRenderSettings;

  uint32_t mlLastEvaluatedFrame = UINT32_MAX; // once-per-frame guard
};

//------------------------------------------

}; // namespace hpl
#endif // HPL_VIEWPORT_H
