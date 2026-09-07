#ifndef HPL_TEMPORAL_UPSCALER_TYPES_H
#define HPL_TEMPORAL_UPSCALER_TYPES_H

#include <cstdint>

namespace hpl {

// Vendor-independent temporal upscaling providers.  An implementation may
// expose more than one provider, but must report the exact combinations it
// can use through iTemporalUpscaler::Supports.
enum class TemporalUpscalerProvider { Off, Fsr, XeSS };

// NativeAA keeps the output at the input resolution while applying temporal
// anti-aliasing.  Providers advertise which quality values they support;
// unsupported values must be rejected, never silently substituted.
enum class TemporalUpscalerQuality {
  NativeAA,
  Quality,
  Balanced,
  Performance,
  UltraPerformance
};

struct TemporalUpscalerSettings {
  TemporalUpscalerProvider provider = TemporalUpscalerProvider::Off;
  TemporalUpscalerQuality quality = TemporalUpscalerQuality::Quality;
};

struct TemporalUpscalerStatus {
  TemporalUpscalerProvider requestedProvider = TemporalUpscalerProvider::Off;
  TemporalUpscalerQuality requestedQuality = TemporalUpscalerQuality::Quality;
  TemporalUpscalerProvider effectiveProvider = TemporalUpscalerProvider::Off;
  TemporalUpscalerQuality effectiveQuality = TemporalUpscalerQuality::Quality;
  bool available = false;
  // Static string literal, never owned by the caller; null when available.
  const char *unavailableReason = nullptr;
};

// The render extent is the smaller input extent used for shading.  The output
// extent is the display-resolution extent produced by the upscaler.
struct TemporalUpscalerExtent {
  uint32_t width = 0;
  uint32_t height = 0;
};

} // namespace hpl

#endif // HPL_TEMPORAL_UPSCALER_TYPES_H
