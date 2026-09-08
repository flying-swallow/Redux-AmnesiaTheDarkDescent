#ifndef HPL_TEMPORAL_UPSCALER_POLICY_H
#define HPL_TEMPORAL_UPSCALER_POLICY_H

#include <cstdint>

#include "graphics/TemporalUpscalerTypes.h"

namespace hpl {

// A provider's negotiated input extent must be a non-empty subextent of the
// display. NativeAA is the one quality mode that must use the whole display.
bool TemporalUpscalerNegotiatedExtentValid(
    TemporalUpscalerExtent render, TemporalUpscalerExtent display,
    TemporalUpscalerQuality quality);

// Material texture mip bias for a frame whose scene/input extent is smaller
// than the display extent. Vendor guidance (FSR/XeSS) is
// log2(renderWidth / displayWidth) - 1.0, applied to material sampling only.
// Returns 0 unless a provider is prepared AND the render extent is genuinely
// reduced, so Off, NativeAA, and any native-extent frame are unbiased.
// A reduced extent owned by DevRenderScale is also unbiased: the override has
// no reconstruction behind it.
float TemporalMaterialMipBias(TemporalUpscalerExtent render,
                              TemporalUpscalerExtent display,
                              bool providerPrepared);

enum class TemporalRenderExtentOwner { Native, DevRenderScale, Provider };

struct TemporalRenderExtentState {
  TemporalRenderExtentOwner owner = TemporalRenderExtentOwner::Native;
  TemporalUpscalerExtent extent = {};
};

struct TemporalRenderExtentDecision {
  TemporalRenderExtentState state = {};
  bool historyReset = false;
  bool ownershipChanged = false;
  bool devRenderScaleIgnored = false;
};

struct TemporalRenderExtentResolution {
  bool providerPrepared = false;
  TemporalUpscalerExtent negotiatedExtent = {};
  float devRenderScale = 1.0f;
  TemporalUpscalerExtent displayExtent = {};
  TemporalRenderExtentState current = {};
};

// Resolves the owner for the current evaluation. A prepared provider wins
// even when its negotiated extent is NativeAA/display-sized. The development
// scale is rounded and clamped exactly as the viewport's old override was.
TemporalRenderExtentDecision ResolveTemporalRenderExtent(
    const TemporalRenderExtentResolution &input);

// These transitions are intentionally separate so a headless caller can model
// preparation and teardown without constructing the engine viewport.
TemporalRenderExtentDecision ClaimTemporalProviderRenderExtent(
    const TemporalRenderExtentState &current,
    TemporalUpscalerExtent negotiatedExtent);
TemporalRenderExtentDecision ReleaseTemporalProviderRenderExtent(
    const TemporalRenderExtentState &current);
TemporalRenderExtentDecision ConsumeTemporalProviderFailure(
    const TemporalRenderExtentState &current);

struct TemporalProviderFailureKey {
  TemporalUpscalerProvider provider = TemporalUpscalerProvider::Off;
  TemporalUpscalerQuality quality = TemporalUpscalerQuality::Quality;
  TemporalUpscalerExtent displayExtent = {};
};

struct TemporalProviderFailureState {
  bool latched = false;
  TemporalProviderFailureKey key = {};
};

bool TemporalProviderFailureKeyMatches(const TemporalProviderFailureKey &left,
                                       const TemporalProviderFailureKey &right);
bool TemporalProviderFailureSuppressed(
    const TemporalProviderFailureState &state,
    const TemporalProviderFailureKey &key);
void LatchTemporalProviderFailure(TemporalProviderFailureState &state,
                                  const TemporalProviderFailureKey &key);
void ClearTemporalProviderFailure(TemporalProviderFailureState &state);

struct TemporalHistoryResetTriggers {
  bool firstFrame = false;
  bool inputExtentChanged = false;
  bool outputExtentChanged = false;
  bool providerChanged = false;
  bool qualityChanged = false;
  bool worldReplaced = false;
  bool cameraReplaced = false;
  bool missedRenderedFrames = false;
  bool cameraCut = false;
  bool providerFailure = false;

  // Deliberately not reset causes. They are named here so callers and tests
  // can make the contract explicit: normal movement and a new Halton sample
  // preserve history.
  bool ordinaryCameraMovement = false;
  bool haltonSampleChanged = false;
};

bool TemporalHistoryResetRequired(const TemporalHistoryResetTriggers &triggers);

} // namespace hpl

#endif // HPL_TEMPORAL_UPSCALER_POLICY_H
