#ifndef HPL_FSR_UPSCALER_PARAMS_H
#define HPL_FSR_UPSCALER_PARAMS_H

#include <cstdint>

namespace hpl {

struct FsrExtent {
  uint32_t width = 0;
  uint32_t height = 0;
};

struct FsrFrameParamsInput {
  FsrExtent renderExtent = {};
  FsrExtent outputExtent = {};

  // Jitter is a sample displacement from the pixel center, in render pixels.
  // The engine's TemporalApplyJitterToProjection adds
  // (-2 * jitterUV.x, +2 * jitterUV.y) * w to clip space, where
  // jitterUV = jitterPixels / renderExtent. FSR3 describes jitterOffset as
  // producing (+2 * jx / W, -2 * jy / H). Matching x gives
  // -2 * jitterPixels.x / W = +2 * jx / W, so jx = -jitterPixels.x;
  // matching y gives +2 * jitterPixels.y / H = -2 * jy / H, so
  // jy = -jitterPixels.y. This preserves the top-left, y-down engine
  // convention when the values are passed to FSR. Both input components must
  // be finite and lie in [-0.5, 0.5], allowing a small boundary tolerance.
  float jitterPixels[2] = {};
  float deltaTimeMs = 0.0f; // Finite and non-negative; dispatch clamps it.
  float zNear = 0.0f; // Finite, positive, and less than zFar.
  float zFar = 0.0f; // Finite, positive, non-reversed far plane.
  float verticalFovRadians = 0.0f; // Finite and strictly between 0 and pi.
  float preExposure = 1.0f; // Finite and strictly positive HDR exposure.
  bool resetHistory = false;
};

// Mirrors the scalar fields of FfxFsr3UpscalerDispatchDescription without
// naming the SDK type. This metadata stays independent of the SDK so the
// parameter layer can be validated and tested by a headless CPU caller.
struct FsrDispatchParams {
  float jitterX = 0.0f, jitterY = 0.0f;

  // The shared velocity is current-minus-previous unjittered UV at the
  // render extent. The negative render-size scale converts it to the
  // previous-minus-current pixel displacement expected by FSR.
  float motionVectorScaleX = 0.0f, motionVectorScaleY = 0.0f;
  uint32_t renderWidth = 0, renderHeight = 0;
  uint32_t upscaleWidth = 0, upscaleHeight = 0;

  // Dispatch receives milliseconds and clamps unusually long hitches to one
  // second so a stalled frame cannot hand an unbounded delta to the SDK.
  float frameTimeDeltaMs = 0.0f;
  float cameraNear = 0.0f, cameraFar = 0.0f;
  float cameraFovAngleVertical = 0.0f;
  float preExposure = 1.0f;
  bool reset = false;

  // Sharpening is intentionally disabled; reconstruction and sharpening are
  // separate policies, and this parameter layer owns only the former.
  bool enableSharpening = false;
  float sharpness = 0.0f;
};

// Describes a caller-produced image binding without naming an engine or SDK
// type. Present means the caller handed over a binding whose IsValid() was
// true; the policy validates the remaining 2D subresource contract.
struct FsrMaskBindingInfo {
  bool present = false;
  FsrExtent extent = {};
  uint32_t mipOffset = 0, mipCount = 1;
  uint32_t layerOffset = 0, layerCount = 1;
};

struct FsrMaskPolicyInput {
  FsrExtent renderExtent = {};
  FsrMaskBindingInfo reactiveMask = {};
  FsrMaskBindingInfo compositionMask = {};
  FsrMaskBindingInfo opaqueColor = {};
};

struct FsrMaskPolicy {
  bool bindReactiveMask = false;
  bool bindCompositionMask = false;
  bool bindOpaqueColor = false;
  bool enableAutoReactive = false;
};

enum class FsrMaskError {
  None,
  BadRenderExtent,
  BadReactiveMask,
  BadCompositionMask,
  BadOpaqueColor
};

enum class FsrParamsError {
  None,
  BadRenderExtent,
  BadOutputExtent,
  RenderExceedsOutput,
  BadJitter,
  BadCameraPlanes,
  BadFov,
  BadDeltaTime,
  BadPreExposure
};

// Validation runs in the enum's order and reports the first failure. On
// failure, out is not modified; a null out is allowed for validation-only
// calls and never gets dereferenced. Equal render/output extents are valid
// because they represent NativeAA. Render and output dimensions are bounded
// to 16384, while camera values must be finite and use a non-reversed,
// finite depth range.
FsrParamsError FsrBuildDispatchParams(const FsrFrameParamsInput &input,
                                      FsrDispatchParams *out);

// The returned text is stable, human-readable, and never null, including for
// an unknown enum value.
const char *FsrParamsErrorString(FsrParamsError error);

// A present binding is valid only when it is a one-mip, one-layer 2D view,
// both subresource offsets fit in uint16_t, and its extent equals the render
// extent. A malformed present binding is an error, not a silent fallback, and
// absent bindings are simply not bound. Validation runs in the enum's order
// and reports the first failure. On failure, out is not modified; a null out
// is allowed for validation-only calls and never gets dereferenced. When
// either explicit mask is bound, automatic reactive generation is disabled and
// opaqueColor is not bound. When neither mask is bound, FSR's automatic
// reactive generation is enabled exactly when a valid opaqueColor is present.
// Render dimensions use the same non-zero, at-most-16384 bound as dispatch.
FsrMaskError FsrBuildMaskPolicy(const FsrMaskPolicyInput &input,
                                FsrMaskPolicy *out);

// The returned text is stable, human-readable, and never null, including for
// an unknown enum value.
const char *FsrMaskErrorString(FsrMaskError error);

} // namespace hpl

#endif // HPL_FSR_UPSCALER_PARAMS_H
