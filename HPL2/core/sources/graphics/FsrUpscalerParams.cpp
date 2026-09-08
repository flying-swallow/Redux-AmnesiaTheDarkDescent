#include "graphics/FsrUpscalerParams.h"

#include <cmath>

namespace hpl {

FsrParamsError FsrBuildDispatchParams(const FsrFrameParamsInput &input,
                                      FsrDispatchParams *out) {
  constexpr uint32_t kMaxExtent = 16384;
  constexpr float kJitterLimit = 0.5f;
  constexpr float kJitterEpsilon = 1.0e-4f;
  constexpr float kMaxDeltaTimeMs = 1000.0f;
  constexpr float kPi = 3.14159265358979323846f;

  const bool validRenderExtent =
      input.renderExtent.width != 0 && input.renderExtent.height != 0 &&
      input.renderExtent.width <= kMaxExtent &&
      input.renderExtent.height <= kMaxExtent;
  if (!validRenderExtent) {
    return FsrParamsError::BadRenderExtent;
  }

  const bool validOutputExtent =
      input.outputExtent.width != 0 && input.outputExtent.height != 0 &&
      input.outputExtent.width <= kMaxExtent &&
      input.outputExtent.height <= kMaxExtent;
  if (!validOutputExtent) {
    return FsrParamsError::BadOutputExtent;
  }

  if (input.renderExtent.width > input.outputExtent.width ||
      input.renderExtent.height > input.outputExtent.height) {
    return FsrParamsError::RenderExceedsOutput;
  }

  const auto validJitter = [](float value) {
    return std::isfinite(value) && value >= -kJitterLimit - kJitterEpsilon &&
           value <= kJitterLimit + kJitterEpsilon;
  };
  if (!validJitter(input.jitterPixels[0]) ||
      !validJitter(input.jitterPixels[1])) {
    return FsrParamsError::BadJitter;
  }

  if (!std::isfinite(input.zNear) || !std::isfinite(input.zFar) ||
      input.zNear <= 0.0f || input.zFar <= 0.0f ||
      input.zNear >= input.zFar) {
    return FsrParamsError::BadCameraPlanes;
  }

  if (!std::isfinite(input.verticalFovRadians) ||
      input.verticalFovRadians <= 0.0f ||
      input.verticalFovRadians >= kPi) {
    return FsrParamsError::BadFov;
  }

  if (!std::isfinite(input.deltaTimeMs) || input.deltaTimeMs < 0.0f) {
    return FsrParamsError::BadDeltaTime;
  }

  if (!std::isfinite(input.preExposure) || input.preExposure <= 0.0f) {
    return FsrParamsError::BadPreExposure;
  }

  FsrDispatchParams params;

  // The signs convert the engine's top-left sample displacement to FSR's
  // projection translation, as derived with the input convention above.
  params.jitterX = -input.jitterPixels[0];
  params.jitterY = -input.jitterPixels[1];

  // Shared velocity is current-minus-previous unjittered UV. Scaling by the
  // render extent gives pixel displacement, while FSR expects
  // previous-minus-current, hence the negative sign.
  params.motionVectorScaleX = -(float)input.renderExtent.width;
  params.motionVectorScaleY = -(float)input.renderExtent.height;

  params.renderWidth = input.renderExtent.width;
  params.renderHeight = input.renderExtent.height;
  params.upscaleWidth = input.outputExtent.width;
  params.upscaleHeight = input.outputExtent.height;
  // The SDK receives milliseconds, but an extreme hitch should not make its
  // temporal history jump by an unbounded amount, so clamp it to one second.
  params.frameTimeDeltaMs = input.deltaTimeMs > kMaxDeltaTimeMs
                                ? kMaxDeltaTimeMs
                                : input.deltaTimeMs;
  params.cameraNear = input.zNear;
  params.cameraFar = input.zFar;
  params.cameraFovAngleVertical = input.verticalFovRadians;
  params.preExposure = input.preExposure;
  params.reset = input.resetHistory;

  // Sharpening is deliberately disabled here; it is a separate policy from
  // temporal reconstruction and must not alter this parameter contract.
  params.enableSharpening = false;
  params.sharpness = 0.0f;

  if (out != nullptr) {
    *out = params;
  }
  return FsrParamsError::None;
}

const char *FsrParamsErrorString(FsrParamsError error) {
  switch (error) {
  case FsrParamsError::None:
    return "none";
  case FsrParamsError::BadRenderExtent:
    return "bad render extent";
  case FsrParamsError::BadOutputExtent:
    return "bad output extent";
  case FsrParamsError::RenderExceedsOutput:
    return "render exceeds output";
  case FsrParamsError::BadJitter:
    return "bad jitter";
  case FsrParamsError::BadCameraPlanes:
    return "bad camera planes";
  case FsrParamsError::BadFov:
    return "bad field of view";
  case FsrParamsError::BadDeltaTime:
    return "bad delta time";
  case FsrParamsError::BadPreExposure:
    return "bad pre-exposure";
  }

  return "unknown FSR parameter error";
}

FsrMaskError FsrBuildMaskPolicy(const FsrMaskPolicyInput &input,
                                FsrMaskPolicy *out) {
  constexpr uint32_t kMaxExtent = 16384;
  constexpr uint32_t kMaxSubresourceOffset = 65535;

  const bool validRenderExtent =
      input.renderExtent.width != 0 && input.renderExtent.height != 0 &&
      input.renderExtent.width <= kMaxExtent &&
      input.renderExtent.height <= kMaxExtent;
  if (!validRenderExtent) {
    return FsrMaskError::BadRenderExtent;
  }

  const auto validBinding = [&input, kMaxSubresourceOffset](
                                const FsrMaskBindingInfo &binding) {
    return !binding.present ||
           (binding.mipCount == 1 && binding.layerCount == 1 &&
            binding.mipOffset <= kMaxSubresourceOffset &&
            binding.layerOffset <= kMaxSubresourceOffset &&
            binding.extent.width == input.renderExtent.width &&
            binding.extent.height == input.renderExtent.height);
  };
  if (!validBinding(input.reactiveMask)) {
    return FsrMaskError::BadReactiveMask;
  }
  if (!validBinding(input.compositionMask)) {
    return FsrMaskError::BadCompositionMask;
  }
  if (!validBinding(input.opaqueColor)) {
    return FsrMaskError::BadOpaqueColor;
  }

  const bool bindReactiveMask = input.reactiveMask.present;
  const bool bindCompositionMask = input.compositionMask.present;
  const bool anyMask = bindReactiveMask || bindCompositionMask;
  FsrMaskPolicy policy;
  policy.bindReactiveMask = bindReactiveMask;
  policy.bindCompositionMask = bindCompositionMask;
  policy.bindOpaqueColor = !anyMask && input.opaqueColor.present;
  policy.enableAutoReactive = policy.bindOpaqueColor;

  if (out != nullptr) {
    *out = policy;
  }
  return FsrMaskError::None;
}

const char *FsrMaskErrorString(FsrMaskError error) {
  switch (error) {
  case FsrMaskError::None:
    return "none";
  case FsrMaskError::BadRenderExtent:
    return "bad render extent";
  case FsrMaskError::BadReactiveMask:
    return "bad reactive mask";
  case FsrMaskError::BadCompositionMask:
    return "bad composition mask";
  case FsrMaskError::BadOpaqueColor:
    return "bad opaque color";
  }

  return "unknown FSR mask error";
}

} // namespace hpl
