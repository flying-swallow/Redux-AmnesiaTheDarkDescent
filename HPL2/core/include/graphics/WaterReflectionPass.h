#ifndef HPL_WATER_REFLECTION_PASS_H
#define HPL_WATER_REFLECTION_PASS_H

#include "graphics/NrdIntegration.h"
#include "graphics/RIProgram.h"

#include <cstddef>
#include <cstdint>
#include <memory>

struct RICmd;
struct RIAccelStructure;
struct RITextureView;

namespace hpl {

class cGraphics;
class cResources;

// The returned halfPositionViewZ and halfNormalWeight views are one reusable
// half-resolution guide scratch set. They stay valid only until the next
// RecordSurface call, so the caller must issue this surface's MUL and ADD
// water draws before recording another surface. Call RecordSurface outside a
// dynamic-rendering scope; it returns outside the guide scope as well. All
// returned views are in GENERAL; sampled descriptors must declare
// RI_RESOURCE_STATE_GENERAL, not SHADER_READ_ONLY_OPTIMAL.
struct WaterReflectionResult {
  bool available = false; // false means the caller must use its fallback.
  RITextureView *specularRadianceHitDist = nullptr;
  RITextureView *halfPositionViewZ = nullptr;
  RITextureView *halfNormalWeight = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
};

// Owns one reusable guide/pack scratch set and the per-surface NRD cache for a
// viewport. It is deliberately neither copyable nor movable: command buffers
// may still refer to the resources while a viewport is being retired.
class WaterReflectionViewportState {
public:
  WaterReflectionViewportState();
  ~WaterReflectionViewportState();

  WaterReflectionViewportState(const WaterReflectionViewportState &) = delete;
  WaterReflectionViewportState &operator=(const WaterReflectionViewportState &) = delete;
  WaterReflectionViewportState(WaterReflectionViewportState &&) = delete;
  WaterReflectionViewportState &operator=(WaterReflectionViewportState &&) = delete;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
  friend class WaterReflectionPass;
};

struct WaterReflectionFrameDesc {
  uint32_t width = 0;  // full overscan extent
  uint32_t height = 0; // full overscan extent
  uint32_t frameIndex = 0; // perFrame.totalFrames
  bool resetHistory = false;
  // The caller fills NrdFrameData::cameraJitter and
  // NrdFrameData::cameraJitterPrev in FULL render pixels (the full
  // overscan width/height above), matching the main render extent's
  // jitterPixels and prevJitterPixels. BeginFrame performs the single
  // conversion into the half-resolution NRD instance's input pixels.
  // cameraJitterPrev uses the current frame's ratio because an extent change
  // resets water history. Everything else in NrdFrameData (matrices,
  // frameIndex, denoisingRange, and timeDeltaMs) passes through unchanged.
  NrdFrameData nrd = {};
};

struct WaterReflectionSurfaceDesc {
  // This is the stable renderable identity, rather than a pointer or a
  // transient bindless slot, so NRD history follows the object across frames.
  uint64_t renderableCookie = 0;
  uint64_t materialSignature = 0;
  uint32_t objectSlot = 0;
  uint32_t indexCount = 0;
  uint32_t vertexPresentMask = 0;
  RITextureView *opaqueDepthView = nullptr;
  RIAccelStructure *tlas = nullptr;
  const RIProgram::DescriptorBinding *sharedBindings = nullptr;
  size_t sharedBindingCount = 0;
};

// Renderer-owned programs and pipelines; viewport scratch textures live in
// WaterReflectionViewportState so a viewport with no water allocates nothing.
class WaterReflectionPass {
public:
  WaterReflectionPass() = default;
  ~WaterReflectionPass() = default;

  WaterReflectionPass(const WaterReflectionPass &) = delete;
  WaterReflectionPass &operator=(const WaterReflectionPass &) = delete;
  WaterReflectionPass(WaterReflectionPass &&) = delete;
  WaterReflectionPass &operator=(WaterReflectionPass &&) = delete;

  void Initialize(cGraphics *graphics, cResources *resources);
  void Dispose(cGraphics *graphics);
  void BeginFrame(WaterReflectionViewportState &state,
                  const WaterReflectionFrameDesc &desc);
  WaterReflectionResult RecordSurface(
      RICmd *cmd, WaterReflectionViewportState &state,
      const WaterReflectionSurfaceDesc &surface);
  void EndFrame(WaterReflectionViewportState &state);

private:
  cGraphics *m_graphics = nullptr;
  RIProgram m_guide;
  RIProgram m_reduce;
  RIProgram m_trace;
  RIProgram m_pack;
};

} // namespace hpl

#endif // HPL_WATER_REFLECTION_PASS_H
