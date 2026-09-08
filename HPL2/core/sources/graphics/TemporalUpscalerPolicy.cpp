#include "graphics/TemporalUpscalerPolicy.h"

#include <cmath>

namespace hpl {
namespace {

bool SameExtent(TemporalUpscalerExtent left, TemporalUpscalerExtent right) {
  return left.width == right.width && left.height == right.height;
}

bool IsNonNativeDevelopmentScale(float scale) {
  return scale > 0.0f && scale < 1.0f;
}

uint32_t ScaledDimension(uint32_t display, float scale) {
  // The viewport only reaches this helper for positive values below one.
  // Adding one half before truncation matches round-to-nearest for that
  // domain without pulling a math or engine dependency into this module.
  const uint32_t scaled = static_cast<uint32_t>(
      static_cast<float>(display) * scale + 0.5f);
  if (scaled < 1u)
    return 1u;
  return scaled > display ? display : scaled;
}

TemporalRenderExtentDecision SetExtent(
    const TemporalRenderExtentState &current,
    TemporalRenderExtentOwner owner, TemporalUpscalerExtent extent,
    bool forceHistoryReset = false) {
  TemporalRenderExtentDecision decision;
  decision.state.owner = owner;
  decision.state.extent = extent;
  decision.ownershipChanged = current.owner != owner;
  decision.historyReset = forceHistoryReset || decision.ownershipChanged ||
                          !SameExtent(current.extent, extent);
  return decision;
}

} // namespace

bool TemporalUpscalerNegotiatedExtentValid(
    TemporalUpscalerExtent render, TemporalUpscalerExtent display,
    TemporalUpscalerQuality quality) {
  if (render.width == 0 || render.height == 0 || display.width == 0 ||
      display.height == 0)
    return false;
  if (render.width > display.width || render.height > display.height)
    return false;
  if (quality == TemporalUpscalerQuality::NativeAA &&
      !SameExtent(render, display))
    return false;
  return true;
}

float TemporalMaterialMipBias(TemporalUpscalerExtent render,
                              TemporalUpscalerExtent display,
                              bool providerPrepared) {
  if (!providerPrepared)
    return 0.0f;
  if (display.width == 0 || display.height == 0 || render.width == 0 ||
      render.height == 0)
    return 0.0f;
  if (render.width >= display.width)
    return 0.0f;
  return std::log2(static_cast<float>(render.width) /
                   static_cast<float>(display.width)) -
         1.0f;
}

TemporalRenderExtentDecision ResolveTemporalRenderExtent(
    const TemporalRenderExtentResolution &input) {
  if (input.providerPrepared) {
    TemporalRenderExtentDecision decision = SetExtent(
        input.current, TemporalRenderExtentOwner::Provider,
        input.negotiatedExtent);
    decision.devRenderScaleIgnored =
        IsNonNativeDevelopmentScale(input.devRenderScale);
    return decision;
  }

  if (IsNonNativeDevelopmentScale(input.devRenderScale) &&
      input.displayExtent.width != 0 && input.displayExtent.height != 0) {
    const TemporalUpscalerExtent scaled = {
        ScaledDimension(input.displayExtent.width, input.devRenderScale),
        ScaledDimension(input.displayExtent.height, input.devRenderScale)};
    return SetExtent(input.current, TemporalRenderExtentOwner::DevRenderScale,
                     scaled);
  }

  // A degenerate display is handled by GetRenderExtent. The native sentinel
  // keeps the raw policy state aligned with the display fallback.
  return SetExtent(input.current, TemporalRenderExtentOwner::Native, {});
}

TemporalRenderExtentDecision ClaimTemporalProviderRenderExtent(
    const TemporalRenderExtentState &current,
    TemporalUpscalerExtent negotiatedExtent) {
  return SetExtent(current, TemporalRenderExtentOwner::Provider,
                   negotiatedExtent);
}

TemporalRenderExtentDecision ReleaseTemporalProviderRenderExtent(
    const TemporalRenderExtentState &current) {
  return SetExtent(current, TemporalRenderExtentOwner::Native, {});
}

TemporalRenderExtentDecision ConsumeTemporalProviderFailure(
    const TemporalRenderExtentState &current) {
  return SetExtent(current, TemporalRenderExtentOwner::Native, {}, true);
}

bool TemporalProviderFailureKeyMatches(const TemporalProviderFailureKey &left,
                                       const TemporalProviderFailureKey &right) {
  return left.provider == right.provider && left.quality == right.quality &&
         SameExtent(left.displayExtent, right.displayExtent);
}

bool TemporalProviderFailureSuppressed(
    const TemporalProviderFailureState &state,
    const TemporalProviderFailureKey &key) {
  return state.latched && TemporalProviderFailureKeyMatches(state.key, key);
}

void LatchTemporalProviderFailure(TemporalProviderFailureState &state,
                                  const TemporalProviderFailureKey &key) {
  state.latched = true;
  state.key = key;
}

void ClearTemporalProviderFailure(TemporalProviderFailureState &state) {
  state = {};
}

bool TemporalHistoryResetRequired(const TemporalHistoryResetTriggers &triggers) {
  return triggers.firstFrame || triggers.inputExtentChanged ||
         triggers.outputExtentChanged || triggers.providerChanged ||
         triggers.qualityChanged || triggers.worldReplaced ||
         triggers.cameraReplaced || triggers.missedRenderedFrames ||
         triggers.cameraCut || triggers.providerFailure;
}

} // namespace hpl
