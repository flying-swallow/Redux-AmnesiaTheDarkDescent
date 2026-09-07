#ifndef HPL_XESS_UPSCALER_H
#define HPL_XESS_UPSCALER_H

#include "graphics/TemporalUpscaler.h"

#include <cstdint>

#if defined(HPL2_XESS_AVAILABLE) && HPL2_XESS_AVAILABLE
#include <xess/xess.h>
#endif

namespace hpl {

class cGraphics;

// XeSS implementation of the engine-neutral temporal upscaler contract.
// Inputs and the destination binding are borrowed from the presentation
// owner.  The only SDK object retained here is the per-viewport XeSS context;
// its destruction is parked in cGraphics::graphicsDefer.
class cXessUpscaler final : public iTemporalUpscaler {
public:
  explicit cXessUpscaler(cGraphics *graphics = nullptr);
  ~cXessUpscaler() override;

  cXessUpscaler(const cXessUpscaler &) = delete;
  cXessUpscaler &operator=(const cXessUpscaler &) = delete;
  cXessUpscaler(cXessUpscaler &&) = delete;
  cXessUpscaler &operator=(cXessUpscaler &&) = delete;

  bool Supports(TemporalUpscalerProvider provider,
                TemporalUpscalerQuality quality) const override;

  TemporalUpscalerExtent GetRecommendedRenderExtent(
      TemporalUpscalerExtent output,
      TemporalUpscalerQuality quality) const override;

  uint32_t GetJitterPhaseCount(TemporalUpscalerExtent render,
                               TemporalUpscalerExtent output) const override;

  bool PrepareContext(const TemporalUpscalerSettings &settings,
                     TemporalUpscalerExtent render,
                     TemporalUpscalerExtent output,
                     cGraphics::FrameContext *frame) override;

  TemporalUpscalerOutput RecordResolve(
      TemporalUpscalerExtent render, TemporalUpscalerExtent output,
      const TemporalUpscalerFrameInput &input) override;

private:
  cGraphics *Graphics() const;

#if defined(HPL2_XESS_AVAILABLE) && HPL2_XESS_AVAILABLE
  bool EnsureContextForQuery() const;
  void RetireContext();
  bool InitializeContext(TemporalUpscalerExtent render,
                         TemporalUpscalerExtent output,
                         TemporalUpscalerQuality quality,
                         uint32_t initFlags);

  mutable xess_context_handle_t m_context = nullptr;
  bool m_contextInitialized = false;
  bool m_responsiveMaskEnabled = false;
  bool m_hasExecuted = false;
  uint32_t m_initFlags = 0;
  TemporalUpscalerQuality m_preparedQuality =
      TemporalUpscalerQuality::NativeAA;
  TemporalUpscalerExtent m_preparedRender = {};
  TemporalUpscalerExtent m_preparedOutput = {};
#endif

  cGraphics *mpGraphics = nullptr;
};

} // namespace hpl

#endif // HPL_XESS_UPSCALER_H
