#ifndef HPL_FSR_UPSCALER_H
#define HPL_FSR_UPSCALER_H

#include "graphics/TemporalUpscaler.h"

#include <cstdint>
#include <memory>

namespace hpl {

class cGraphics;

// Vulkan FSR 3.1 implementation of the engine-neutral temporal upscaler
// contract.  The FidelityFX types stay in the source file so a build without
// the optional SDK still exposes the same public interface.
class cFsrUpscaler final : public iTemporalUpscaler {
public:
  explicit cFsrUpscaler(cGraphics *graphics);
  ~cFsrUpscaler() override;

  cFsrUpscaler(const cFsrUpscaler &) = delete;
  cFsrUpscaler &operator=(const cFsrUpscaler &) = delete;
  cFsrUpscaler(cFsrUpscaler &&) = delete;
  cFsrUpscaler &operator=(cFsrUpscaler &&) = delete;

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
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace hpl

#endif // HPL_FSR_UPSCALER_H
