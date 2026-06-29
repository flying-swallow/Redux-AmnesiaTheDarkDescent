
#ifndef HPL_GRAPHICS_BACKEND_H
#define HPL_GRAPHICS_BACKEND_H

#include "graphics/GraphicsTypes.h"
#include "graphics/HPLGraphicsConfig.h"
#include "graphics/RIGpuProfiler.h"
#include "graphics/RISegmentAlloc.h"
#include "graphics/RITimeline.h"
#include "graphics/RITypes.h"
#include <array>
#include <cassert>
#include <cstdint>
#include <deque>
#include <functional>
#include <utility>

#include <graphics/RIPogoBuffer.h>
#include <graphics/RIProgram.h>
#include <graphics/RIResourceUploader.h>
#include <graphics/RIScratchAlloc.h>
#include <memory>
#include <optional>
#include <resources/ResourceBase.h>
#include <variant>

namespace hpl {
struct cTexture;
class GlobalManagedSets; // owned here as RI.globalset (pointer breaks the
                         // GlobalManagedSets.h -> RIBootstrap.h header cycle)

struct RIBootstrap {
public:
  // CPU-side queue of deferred resource releases latched to a GPU timeline
  // value (graphicsTimeline). The timeline answers "has the GPU passed value
  // N?"; this holds the resources that become safe to free once it has. Lives
  // here rather than in the RI layer because it is frame-orchestration state,
  // not a device primitive.
  //
  // Entries release in their destructor (RISharedPointer drops its sole
  // reference — held only by this queue — and disposes; SharedResourcePin
  // releases likewise; a std::function operation releases on invocation), so
  // they are stored DIRECTLY in the variant.
  // Two structures cooperate:
  //   * `entries` — a FIFO of every still-pending deferral, oldest at front.
  //   * `batches` — a ring of {timeline value, count} records partitioning the
  //     queue across in-flight frames. seal() appends one record at submit;
  //     drain() pops whole batches from the front once the GPU passes them.
  // Single-threaded (render thread), like the rest of the frame loop.
  struct FrameDeferral {
    static constexpr size_t CAPACITY = RI_NUMBER_FRAMES_FLIGHT + 2;

    using Entry = std::variant<SharedResourcePin,
                               RISharedPointer<RIAccelStructure>,
                               RISharedPointer<RISampler>,
                               RISharedPointer<RITextureView>,
                               RISharedPointer<RIBuffer>,
                               RISharedPointer<RITexture>,
                               std::function<void()>>;

    struct Batch {
      uint64_t value;
      size_t count;
    };

    // Queue a deferral for the frame currently being built.
    template<typename T>
    void push(T e) {
      if constexpr (
          std::is_same_v<T, RISharedPointer<RIAccelStructure>> ||
          std::is_same_v<T, RISharedPointer<RISampler>> ||
          std::is_same_v<T, RISharedPointer<RITextureView>> ||
          std::is_same_v<T, RISharedPointer<RIBuffer>> ||
          std::is_same_v<T, RISharedPointer<RITexture>>
      ) {
        if(!e.isEmpty()) {
          entries.push_back(std::move(e));
          ++pendingCount;
        }
      } else {
        entries.push_back(std::move(e));
        ++pendingCount;
      }
    }
   

    // At submit: seal everything accumulated since the last seal() under this
    // frame's timeline value. No-op when nothing was deferred this frame.
    void seal(uint64_t value) {
      if (pendingCount == 0)
        return;
      assert(batchCount < CAPACITY && "FrameDeferral ring overflow");
      batches[(head + batchCount) % CAPACITY] = {value, pendingCount};
      pendingCount = 0;
      ++batchCount;
    }

    // At frame start: release every sealed batch whose value the GPU has
    // reached. Batches seal in non-decreasing value order, so everything ready
    // is at the head. Cheap and non-blocking.
    void drain(uint64_t completed) {
      while (batchCount > 0 && batches[head].value <= completed) {
        releaseFront(batches[head].count);
        head = (head + 1) % CAPACITY;
        --batchCount;
      }
    }

    // Teardown: release everything, sealed and still-pending. Caller must
    // ensure the GPU is idle first.
    void drainAll() {
      releaseFront(entries.size());
      head = 0;
      batchCount = 0;
      pendingCount = 0;
    }

    std::deque<Entry> entries; // FIFO, oldest at front
    std::array<Batch, CAPACITY> batches = {};
    size_t head = 0; // ring window [head, head+batchCount) modulo CAPACITY
    size_t batchCount = 0;
    size_t pendingCount = 0; // entries pushed since the last seal()

  private:
    void releaseFront(size_t n) {
      for (size_t i = 0; i < n; ++i) {
        Entry &e = entries.front();
        // Operations release on invocation; deferrals / pins release when
        // pop_front() destroys the entry.
        if (auto *op = std::get_if<std::function<void()>>(&e); op && *op)
          (*op)();
        entries.pop_front();
      }
    }
  };

  // Raster G-buffer output — packed TriangleHit (uint4), same layout the RT
  // V-buffer writes into the viewport state's packedHitInfoTexture. Decoded
  // with unpackHit() in
  // visibility_shade.frag and the surfel passes (see surfel_vbuffer.rgen for
  // the canonical pack convention).
  static constexpr RI_Format_e VisibilityFormat = RI_FORMAT_RGBA32_UINT;
  // Depth+stencil: the depth aspect drives all the normal Z passes; the
  // stencil aspect is used by cLuxEffectRenderer's outline pass (mark the
  // silhouette, then composite with a NOTEQUAL stencil test). The Hybrid
  // viewport's depth view is created with both aspects so the effect can
  // attach stencil; the depth aspect is never sampled (the surfel passes
  // sample their own depth atlas, not scene depth), so no separate
  // depth-only view is required.
  static constexpr RI_Format_e DepthFormat = RI_FORMAT_D32_SFLOAT_S8_UINT;
  // Screen-space motion vectors (gbuffer 2nd MRT), RG16F.
  static constexpr RI_Format_e VelocityFormat = RI_FORMAT_RG16_SFLOAT;

  // HDR format for the pogo ping-pong buffer and every post-effect
  // intermediate target / pipeline color attachment that feeds it. The
  // SurfelGI composite + bloom keep linear values >1 here (the swapchain is
  // 16-bit linear scRGB); all these targets must share one format or Vulkan
  // raises an attachment-format mismatch — so this is the single source. Call
  // sites in the RI vocabulary use this directly; the few genuine Vulkan
  // boundaries convert with RIFormatToVK(PogoColorFormat).
  static constexpr RI_Format_e PogoColorFormat = RI_FORMAT_RGBA16_SFLOAT;

  explicit RIBootstrap() {}

  struct FrameContext {
    struct RIScratchAlloc uboScratchAlloc;
    struct RIScratchAlloc accelScratchAlloc;
  };

  RIDevice device;
  RIProgram gui;
  // Fullscreen passthrough (posteffect_fullscreen.vert + posteffect_blit.frag)
  // used to blit the renderer's pogo "read" half to the swapchain. Lives here
  // so cScene can present after applying the viewport's post-effect composite.
  RIProgram postEffectBlit;

  // 1x1 white texture used as the default texture binding when no real
  // texture is available. Its descriptor is produced on demand via
  // whiteTexture2DDescriptor() (cookie lives on the view).
  struct RITexture whiteTexture2D;
  struct RITextureView whiteTexture2DView;

  // Zero-filled vertex buffer bound into vertex input slots that don't have
  // a real stream — the vertex fetcher reads zeros for those attributes.
  struct RIBuffer nulVertexBuffer;

  // Default-value fallback vertex streams bound when a renderable omits an
  // optional stream in the fixed-function raster passes (translucent / water /
  // decal): normal = +Z, tangent = +X with +handedness, color = white, uv = 0.
  // Each holds a single vertex — the pipeline zeroes the binding stride for an
  // absent stream (see TranslucentMeshPipelineDesc / DecalPipelineDesc), so the
  // one element feeds every vertex. Filled once at init (see Graphics.cpp);
  // consumed by detail::BindVertexStreams in HybridRenderer.cpp.
  struct RIBuffer fallbackNormalVertex;
  struct RIBuffer fallbackTangentVertex;
  struct RIBuffer fallbackColorVertex;
  struct RIBuffer fallbackUv0Vertex;

  // Per-viewport render targets (backbuffer, overscan render target, depth,
  // visibility) live on cViewport (scene/Viewport.h),
  // not here — one renderer instance serves every viewport.
  RISwapchain<RI_MAX_SWAPCHAIN_IMAGES> swapchain;
  struct RITextureView swapchainView[RI_MAX_SWAPCHAIN_IMAGES];

  RICommandRingBuffer<RI_COMMAND_RING_POOL_COUNT, RI_COMMAND_RING_CMD_PER_POOL>
      graphicsCmdRing;
  struct RICommandRingElement primary;
  struct RICommandRingElement blasSubmit;

  FrameDeferral graphicsDefer;
  RITimeline graphicsTimeline;

  // Per-pass GPU timing (timestamp queries) + debug-utils labels. Reset/resolved
  // each frame in BeginActiveSet; scopes are opened at pass sites in
  // cHybridRenderer::Draw via RIGpuScope.
  RIGpuProfiler profiler;

  struct RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS> guiVertexAlloc;
  RISharedPointer<RIBuffer> guiVertexBuffer;
  struct RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS> guiIndexAlloc;
  RISharedPointer<RIBuffer> guiIndexBuffer;

  // Per-viewport camera-facing translucent geometry (particles): each
  // pane builds its verts/indices straight into its own per-frame segment
  // of these host-mapped buffers and binds them at a byte offset —
  // bypassing the object's persistent cVertexBuffer, whose uploader
  // copies all coalesce into the fenced pre-pass (last pane's copy would
  // win for EVERY pane). Same pattern as the gui*Alloc pair above. The
  // vertex allocator is float-granular (stride 4 bytes) so mixed streams
  // (pos 16B / col 16B / uv 12B) share it; the index allocator is
  // uint32-granular.
  struct RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS> translucentVtxAlloc;
  RISharedPointer<RIBuffer> translucentVtxBuffer;
  struct RISegmentAlloc<RI_NUMBER_FRAME_SEGMENTS> translucentIdxAlloc;
  RISharedPointer<RIBuffer> translucentIdxBuffer;

  std::array<FrameContext, RI_NUMBER_FRAMES_FLIGHT> frameSets;
  // Direct-indexed sampler cache keyed by the three wrap modes and filter
  // mode. There are currently 4 * 4 * 4 * 3 = 192 possible
  // configurations. The array index encodes the configuration; cookie ==
  // 0 means the slot has not yet been initialized.
  // resolve_filter_descriptor() creates samplers on demand, and all
  // created samplers are disposed at shutdown.
  static constexpr size_t kSamplerCombinationCount =
      static_cast<size_t>(eTextureWrap_LastEnum) *
      static_cast<size_t>(eTextureWrap_LastEnum) *
      static_cast<size_t>(eTextureWrap_LastEnum) *
      static_cast<size_t>(eTextureFilter_LastEnum);
  std::array<RISampler, kSamplerCombinationCount> cachedSamplers;

  uint32_t swapchainIndex;
  uint32_t frameIndex = 0;

  // Debug override: ignore authored CastShadows / ShadowCaster flags and use
  // the pre-shadow-flag behavior where every opaque instance blocks shadow rays.
  bool forceShadows = false;

  struct RIResourceUploader uploader = {};

  // Engine-lifetime set-0 tables (bindless / object / material / light); owned
  // here, built by InitGlobalManagedSets and freed by ShutdownGlobalManagedSets.
  GlobalManagedSets *globalset = nullptr;

  void IncrementFrame();
  std::optional<RIDescriptor> resolve_filter_descriptor(eTextureWrap wrapS,
                                                        eTextureWrap wrapT,
                                                        eTextureWrap wrapR,
                                                        eTextureFilter filter);
  // Default 1x1 white texture descriptor, produced on demand.
  RIDescriptor whiteTexture2DDescriptor();
  FrameContext *GetActiveSet() {
    return &frameSets[frameIndex % RI_NUMBER_FRAMES_FLIGHT];
  }

  void UpdateFrameUBO(RIDescriptor *descriptor, void *data, size_t size);

  // Claim per-frame segments of the translucent scratch buffers (grow ×1.5 +
  // recreate on overflow, old buffer parked on the active set's freelist).
  // numFloats / numIndices are element counts; the returned req's
  // elementOffset is in elements (multiply by 4 for the bind byte offset).
  bool RequestTranslucentVtx(FrameContext *cntx, size_t numFloats,
                             struct RISegmentReq *req);
  bool RequestTranslucentIdx(FrameContext *cntx, size_t numIndices,
                             struct RISegmentReq *req);

  void CloseAndSubmitActiveSet();
  void BeginActiveSet();
  void Dispose();
};
extern struct RIBootstrap RI;

}; // namespace hpl

#endif
