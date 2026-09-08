#include "graphics/FsrUpscalerParams.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace {

int gCaseCount = 0;

bool Near(float a, float b, float epsilon = 2.0e-6f) {
  return std::fabs(a - b) <= epsilon;
}

bool Check(bool condition, const char *name) {
  ++gCaseCount;
  if (!condition) {
    std::printf("failed: %s\n", name);
    return false;
  }
  return true;
}

hpl::FsrFrameParamsInput ValidInput() {
  hpl::FsrFrameParamsInput input;
  input.renderExtent = {1023, 767};
  input.outputExtent = {1920, 1080};
  input.jitterPixels[0] = 0.1f;
  input.jitterPixels[1] = -0.2f;
  input.deltaTimeMs = 16.7f;
  input.zNear = 0.1f;
  input.zFar = 100.0f;
  input.verticalFovRadians = 1.0f;
  input.preExposure = 1.0f;
  input.resetHistory = true;
  return input;
}

bool SameDispatch(const hpl::FsrDispatchParams &a,
                  const hpl::FsrDispatchParams &b) {
  return a.jitterX == b.jitterX && a.jitterY == b.jitterY &&
         a.motionVectorScaleX == b.motionVectorScaleX &&
         a.motionVectorScaleY == b.motionVectorScaleY &&
         a.renderWidth == b.renderWidth && a.renderHeight == b.renderHeight &&
         a.upscaleWidth == b.upscaleWidth && a.upscaleHeight == b.upscaleHeight &&
         a.frameTimeDeltaMs == b.frameTimeDeltaMs &&
         a.cameraNear == b.cameraNear && a.cameraFar == b.cameraFar &&
         a.cameraFovAngleVertical == b.cameraFovAngleVertical &&
         a.preExposure == b.preExposure && a.reset == b.reset &&
         a.enableSharpening == b.enableSharpening &&
         a.sharpness == b.sharpness;
}

bool SameDispatchExceptJitter(const hpl::FsrDispatchParams &a,
                              const hpl::FsrDispatchParams &b) {
  hpl::FsrDispatchParams aWithoutJitter = a;
  hpl::FsrDispatchParams bWithoutJitter = b;
  aWithoutJitter.jitterX = 0.0f;
  aWithoutJitter.jitterY = 0.0f;
  bWithoutJitter.jitterX = 0.0f;
  bWithoutJitter.jitterY = 0.0f;
  return SameDispatch(aWithoutJitter, bWithoutJitter);
}

bool SameMaskPolicy(const hpl::FsrMaskPolicy &a, const hpl::FsrMaskPolicy &b) {
  return a.bindReactiveMask == b.bindReactiveMask &&
         a.bindCompositionMask == b.bindCompositionMask &&
         a.bindOpaqueColor == b.bindOpaqueColor &&
         a.enableAutoReactive == b.enableAutoReactive;
}

hpl::FsrMaskBindingInfo PresentBinding(hpl::FsrExtent extent) {
  hpl::FsrMaskBindingInfo binding;
  binding.present = true;
  binding.extent = extent;
  return binding;
}

bool CheckParamFailure(const hpl::FsrFrameParamsInput &input,
                       hpl::FsrParamsError expected, const char *resultName,
                       const char *untouchedName) {
  hpl::FsrDispatchParams sentinel;
  sentinel.jitterX = 17.0f;
  sentinel.jitterY = -19.0f;
  sentinel.motionVectorScaleX = 23.0f;
  sentinel.motionVectorScaleY = -29.0f;
  sentinel.renderWidth = 31;
  sentinel.renderHeight = 37;
  sentinel.upscaleWidth = 41;
  sentinel.upscaleHeight = 43;
  sentinel.frameTimeDeltaMs = 47.0f;
  sentinel.cameraNear = 53.0f;
  sentinel.cameraFar = 59.0f;
  sentinel.cameraFovAngleVertical = 61.0f;
  sentinel.preExposure = 67.0f;
  sentinel.reset = true;
  sentinel.enableSharpening = true;
  sentinel.sharpness = 71.0f;

  hpl::FsrDispatchParams output = sentinel;
  const hpl::FsrParamsError actual =
      hpl::FsrBuildDispatchParams(input, &output);
  return Check(actual == expected, resultName) &&
         Check(SameDispatch(output, sentinel), untouchedName);
}

bool CheckMaskFailure(const hpl::FsrMaskPolicyInput &input,
                      hpl::FsrMaskError expected, const char *resultName,
                      const char *untouchedName) {
  const hpl::FsrMaskPolicy sentinel = {true, true, true, true};
  hpl::FsrMaskPolicy output = sentinel;
  const hpl::FsrMaskError actual = hpl::FsrBuildMaskPolicy(input, &output);
  return Check(actual == expected, resultName) &&
         Check(SameMaskPolicy(output, sentinel), untouchedName);
}

bool CheckJitterConvention() {
  const float jitters[][2] = {
      {0.25f, -0.375f}, {-0.5f, 0.5f}, {0.1f, 0.2f}};
  for (const auto &jitter : jitters) {
    hpl::FsrFrameParamsInput input = ValidInput();
    input.jitterPixels[0] = jitter[0];
    input.jitterPixels[1] = jitter[1];
    hpl::FsrDispatchParams params;
    if (!Check(hpl::FsrBuildDispatchParams(input, &params) ==
                   hpl::FsrParamsError::None,
               "numeric jitter convention validates")) {
      return false;
    }

    const float engineProjectionX =
        -2.0f * input.jitterPixels[0] /
        static_cast<float>(input.renderExtent.width);
    const float engineProjectionY =
        2.0f * input.jitterPixels[1] /
        static_cast<float>(input.renderExtent.height);
    const float fsrProjectionX =
        2.0f * params.jitterX /
        static_cast<float>(input.renderExtent.width);
    const float fsrProjectionY =
        -2.0f * params.jitterY /
        static_cast<float>(input.renderExtent.height);
    if (!Check(Near(engineProjectionX, fsrProjectionX, 1.0e-7f),
               "numeric jitter x translations agree") ||
        !Check(Near(engineProjectionY, fsrProjectionY, 1.0e-7f),
               "numeric jitter y translations agree")) {
      return false;
    }
  }
  return true;
}

bool CheckOddExtentAndMotionScale() {
  const hpl::FsrFrameParamsInput input = ValidInput();
  hpl::FsrDispatchParams params;
  if (!Check(hpl::FsrBuildDispatchParams(input, &params) ==
                 hpl::FsrParamsError::None,
             "odd render extent validates")) {
    return false;
  }
  return Check(params.motionVectorScaleX == -1023.0f &&
                   params.motionVectorScaleY == -767.0f,
               "motion scale uses negative odd render extent") &&
         Check(params.renderWidth == 1023 && params.renderHeight == 767 &&
                   params.upscaleWidth == 1920 && params.upscaleHeight == 1080,
               "odd render and output extents round-trip");
}

bool CheckNativeAAAndExceedingOutput() {
  hpl::FsrFrameParamsInput input = ValidInput();
  input.renderExtent = {1023, 767};
  input.outputExtent = input.renderExtent;
  hpl::FsrDispatchParams params;
  if (!Check(hpl::FsrBuildDispatchParams(input, &params) ==
                 hpl::FsrParamsError::None,
             "equal render and output extents validate as NativeAA")) {
    return false;
  }

  input = ValidInput();
  input.renderExtent.width = input.outputExtent.width + 1;
  if (!Check(hpl::FsrBuildDispatchParams(input, nullptr) ==
                 hpl::FsrParamsError::RenderExceedsOutput,
             "render exceeding output width is rejected")) {
    return false;
  }

  input = ValidInput();
  input.renderExtent.height = input.outputExtent.height + 1;
  return Check(hpl::FsrBuildDispatchParams(input, nullptr) ==
                   hpl::FsrParamsError::RenderExceedsOutput,
               "render exceeding output height is rejected");
}

bool CheckStationaryJitteredCamera() {
  hpl::FsrFrameParamsInput firstInput = ValidInput();
  firstInput.jitterPixels[0] = 0.25f;
  firstInput.jitterPixels[1] = -0.375f;
  hpl::FsrFrameParamsInput secondInput = firstInput;
  secondInput.jitterPixels[0] = -0.5f;
  secondInput.jitterPixels[1] = 0.5f;

  hpl::FsrDispatchParams first;
  hpl::FsrDispatchParams second;
  if (!Check(hpl::FsrBuildDispatchParams(firstInput, &first) ==
                 hpl::FsrParamsError::None &&
                 hpl::FsrBuildDispatchParams(secondInput, &second) ==
                     hpl::FsrParamsError::None,
             "stationary jittered camera validates")) {
    return false;
  }
  return Check(first.cameraNear == firstInput.zNear &&
                   first.cameraFar == firstInput.zFar &&
                   first.cameraFovAngleVertical ==
                       firstInput.verticalFovRadians &&
                   second.cameraNear == secondInput.zNear &&
                   second.cameraFar == secondInput.zFar &&
                   second.cameraFovAngleVertical ==
                       secondInput.verticalFovRadians,
               "stationary camera scalars pass through verbatim") &&
         Check(SameDispatchExceptJitter(first, second) &&
                   first.jitterX != second.jitterX &&
                   first.jitterY != second.jitterY,
               "stationary camera changes only jitter fields");
}

bool CheckCameraMotionAndDelta() {
  hpl::FsrFrameParamsInput input = ValidInput();
  input.zNear = 0.25f;
  input.zFar = 250.0f;
  input.verticalFovRadians = 0.9f;
  input.deltaTimeMs = 16.7f;
  input.preExposure = 0.75f;
  hpl::FsrDispatchParams params;
  if (!Check(hpl::FsrBuildDispatchParams(input, &params) ==
                 hpl::FsrParamsError::None,
             "nonzero camera motion validates")) {
    return false;
  }
  if (!Check(params.cameraNear == input.zNear &&
                 params.cameraFar == input.zFar &&
                 params.cameraFovAngleVertical == input.verticalFovRadians &&
                 params.frameTimeDeltaMs == 16.7f,
             "camera motion scalars and normal delta pass through") ||
      !Check(params.frameTimeDeltaMs != 0.0f,
             "nonzero camera motion delta remains nonzero")) {
    return false;
  }

  input.deltaTimeMs = 50000.0f;
  if (!Check(hpl::FsrBuildDispatchParams(input, &params) ==
                 hpl::FsrParamsError::None,
             "absurd hitch validates before clamping")) {
    return false;
  }
  return Check(params.frameTimeDeltaMs == 1000.0f,
               "absurd hitch delta clamps at one second");
}

bool CheckDispatchValidation() {
  hpl::FsrFrameParamsInput input = ValidInput();

  input.renderExtent.width = 0;
  if (!CheckParamFailure(input, hpl::FsrParamsError::BadRenderExtent,
                         "zero render extent is rejected",
                         "zero render extent leaves output untouched")) {
    return false;
  }
  input = ValidInput();
  input.renderExtent.height = 16385;
  if (!CheckParamFailure(input, hpl::FsrParamsError::BadRenderExtent,
                         "oversized render extent is rejected",
                         "oversized render extent leaves output untouched")) {
    return false;
  }

  input = ValidInput();
  input.outputExtent.width = 0;
  if (!CheckParamFailure(input, hpl::FsrParamsError::BadOutputExtent,
                         "zero output extent is rejected",
                         "zero output extent leaves output untouched")) {
    return false;
  }
  input = ValidInput();
  input.outputExtent.height = 16385;
  if (!CheckParamFailure(input, hpl::FsrParamsError::BadOutputExtent,
                         "oversized output extent is rejected",
                         "oversized output extent leaves output untouched")) {
    return false;
  }

  input = ValidInput();
  input.renderExtent.width = input.outputExtent.width + 1;
  if (!CheckParamFailure(input, hpl::FsrParamsError::RenderExceedsOutput,
                         "render exceeding output is reachable",
                         "render exceeds output leaves output untouched")) {
    return false;
  }

  input = ValidInput();
  input.jitterPixels[0] = 0.6f;
  if (!CheckParamFailure(input, hpl::FsrParamsError::BadJitter,
                         "out-of-range jitter is rejected",
                         "bad jitter leaves output untouched")) {
    return false;
  }
  input = ValidInput();
  input.jitterPixels[0] = std::numeric_limits<float>::quiet_NaN();
  if (!CheckParamFailure(input, hpl::FsrParamsError::BadJitter,
                         "NaN jitter is rejected",
                         "NaN jitter leaves output untouched")) {
    return false;
  }
  input = ValidInput();
  input.jitterPixels[1] = std::numeric_limits<float>::infinity();
  if (!CheckParamFailure(input, hpl::FsrParamsError::BadJitter,
                         "infinite jitter is rejected",
                         "infinite jitter leaves output untouched")) {
    return false;
  }

  input = ValidInput();
  input.zNear = 0.0f;
  if (!CheckParamFailure(input, hpl::FsrParamsError::BadCameraPlanes,
                         "non-positive near plane is rejected",
                         "bad near plane leaves output untouched")) {
    return false;
  }
  input = ValidInput();
  input.zFar = 0.0f;
  if (!CheckParamFailure(input, hpl::FsrParamsError::BadCameraPlanes,
                         "non-positive far plane is rejected",
                         "bad far plane leaves output untouched")) {
    return false;
  }
  input = ValidInput();
  input.zNear = 100.0f;
  if (!CheckParamFailure(input, hpl::FsrParamsError::BadCameraPlanes,
                         "reversed camera planes are rejected",
                         "reversed camera planes leave output untouched")) {
    return false;
  }
  input = ValidInput();
  input.zNear = std::numeric_limits<float>::quiet_NaN();
  if (!CheckParamFailure(input, hpl::FsrParamsError::BadCameraPlanes,
                         "NaN near plane is rejected",
                         "NaN near plane leaves output untouched")) {
    return false;
  }
  input = ValidInput();
  input.zFar = std::numeric_limits<float>::infinity();
  if (!CheckParamFailure(input, hpl::FsrParamsError::BadCameraPlanes,
                         "infinite far plane is rejected",
                         "infinite far plane leaves output untouched")) {
    return false;
  }

  input = ValidInput();
  input.verticalFovRadians = 0.0f;
  if (!CheckParamFailure(input, hpl::FsrParamsError::BadFov,
                         "zero field of view is rejected",
                         "zero field of view leaves output untouched")) {
    return false;
  }
  input = ValidInput();
  input.verticalFovRadians = 3.14159265358979323846f;
  if (!CheckParamFailure(input, hpl::FsrParamsError::BadFov,
                         "pi field of view is rejected",
                         "pi field of view leaves output untouched")) {
    return false;
  }
  input = ValidInput();
  input.verticalFovRadians = std::numeric_limits<float>::quiet_NaN();
  if (!CheckParamFailure(input, hpl::FsrParamsError::BadFov,
                         "NaN field of view is rejected",
                         "NaN field of view leaves output untouched")) {
    return false;
  }
  input = ValidInput();
  input.verticalFovRadians = std::numeric_limits<float>::infinity();
  if (!CheckParamFailure(input, hpl::FsrParamsError::BadFov,
                         "infinite field of view is rejected",
                         "infinite field of view leaves output untouched")) {
    return false;
  }

  input = ValidInput();
  input.deltaTimeMs = -1.0f;
  if (!CheckParamFailure(input, hpl::FsrParamsError::BadDeltaTime,
                         "negative delta time is rejected",
                         "negative delta time leaves output untouched")) {
    return false;
  }
  input = ValidInput();
  input.deltaTimeMs = std::numeric_limits<float>::quiet_NaN();
  if (!CheckParamFailure(input, hpl::FsrParamsError::BadDeltaTime,
                         "NaN delta time is rejected",
                         "NaN delta time leaves output untouched")) {
    return false;
  }
  input = ValidInput();
  input.deltaTimeMs = std::numeric_limits<float>::infinity();
  if (!CheckParamFailure(input, hpl::FsrParamsError::BadDeltaTime,
                         "infinite delta time is rejected",
                         "infinite delta time leaves output untouched")) {
    return false;
  }

  input = ValidInput();
  input.preExposure = 0.0f;
  if (!CheckParamFailure(input, hpl::FsrParamsError::BadPreExposure,
                         "zero pre-exposure is rejected",
                         "zero pre-exposure leaves output untouched")) {
    return false;
  }
  input = ValidInput();
  input.preExposure = -1.0f;
  if (!CheckParamFailure(input, hpl::FsrParamsError::BadPreExposure,
                         "negative pre-exposure is rejected",
                         "negative pre-exposure leaves output untouched")) {
    return false;
  }
  input = ValidInput();
  input.preExposure = std::numeric_limits<float>::quiet_NaN();
  if (!CheckParamFailure(input, hpl::FsrParamsError::BadPreExposure,
                         "NaN pre-exposure is rejected",
                         "NaN pre-exposure leaves output untouched")) {
    return false;
  }
  input = ValidInput();
  input.preExposure = std::numeric_limits<float>::infinity();
  if (!CheckParamFailure(input, hpl::FsrParamsError::BadPreExposure,
                         "infinite pre-exposure is rejected",
                         "infinite pre-exposure leaves output untouched")) {
    return false;
  }

  input = ValidInput();
  if (!Check(hpl::FsrBuildDispatchParams(input, nullptr) ==
                 hpl::FsrParamsError::None,
             "null dispatch output is safe for validation-only success")) {
    return false;
  }
  input.jitterPixels[0] = 0.6f;
  return Check(hpl::FsrBuildDispatchParams(input, nullptr) ==
                   hpl::FsrParamsError::BadJitter,
               "null dispatch output is safe for validation-only failure");
}

bool CheckParamErrorStrings() {
  const hpl::FsrParamsError errors[] = {
      hpl::FsrParamsError::None,
      hpl::FsrParamsError::BadRenderExtent,
      hpl::FsrParamsError::BadOutputExtent,
      hpl::FsrParamsError::RenderExceedsOutput,
      hpl::FsrParamsError::BadJitter,
      hpl::FsrParamsError::BadCameraPlanes,
      hpl::FsrParamsError::BadFov,
      hpl::FsrParamsError::BadDeltaTime,
      hpl::FsrParamsError::BadPreExposure,
      static_cast<hpl::FsrParamsError>(-1),
      static_cast<hpl::FsrParamsError>(999),
  };
  for (const hpl::FsrParamsError error : errors) {
    const char *text = hpl::FsrParamsErrorString(error);
    if (!Check(text != nullptr && text[0] != '\0',
               "parameter error string is non-null and non-empty")) {
      return false;
    }
  }
  return true;
}

bool CheckMaskPolicy() {
  const hpl::FsrExtent renderExtent = {1023, 767};
  const hpl::FsrMaskBindingInfo present = PresentBinding(renderExtent);
  hpl::FsrMaskPolicyInput input;
  input.renderExtent = renderExtent;
  hpl::FsrMaskPolicy policy;

  if (!Check(hpl::FsrBuildMaskPolicy(input, &policy) ==
                 hpl::FsrMaskError::None,
             "no mask policy validates") ||
      !Check(!policy.bindReactiveMask && !policy.bindCompositionMask &&
                 !policy.bindOpaqueColor && !policy.enableAutoReactive,
             "no masks bind nothing")) {
    return false;
  }

  input.opaqueColor = present;
  if (!Check(hpl::FsrBuildMaskPolicy(input, &policy) ==
                 hpl::FsrMaskError::None,
             "opaque-only policy validates") ||
      !Check(!policy.bindReactiveMask && !policy.bindCompositionMask &&
                 policy.bindOpaqueColor && policy.enableAutoReactive,
             "valid opaque color enables automatic reactive generation")) {
    return false;
  }

  input.reactiveMask = present;
  if (!Check(hpl::FsrBuildMaskPolicy(input, &policy) ==
                 hpl::FsrMaskError::None,
             "explicit reactive policy validates") ||
      !Check(policy.bindReactiveMask && !policy.bindCompositionMask &&
                 !policy.bindOpaqueColor && !policy.enableAutoReactive,
             "explicit reactive mask suppresses automatic generation")) {
    return false;
  }

  input.reactiveMask = {};
  input.compositionMask = present;
  if (!Check(hpl::FsrBuildMaskPolicy(input, &policy) ==
                 hpl::FsrMaskError::None,
             "explicit composition policy validates") ||
      !Check(!policy.bindReactiveMask && policy.bindCompositionMask &&
                 !policy.bindOpaqueColor && !policy.enableAutoReactive,
             "explicit composition mask suppresses automatic generation")) {
    return false;
  }

  input.reactiveMask = present;
  if (!Check(hpl::FsrBuildMaskPolicy(input, &policy) ==
                 hpl::FsrMaskError::None,
             "both explicit masks policy validates") ||
      !Check(policy.bindReactiveMask && policy.bindCompositionMask &&
                 !policy.bindOpaqueColor && !policy.enableAutoReactive,
             "explicit masks suppress opaque color binding")) {
    return false;
  }

  input = {};
  input.renderExtent = renderExtent;
  input.reactiveMask = present;
  input.reactiveMask.extent.width++;
  if (!CheckMaskFailure(input, hpl::FsrMaskError::BadReactiveMask,
                        "wrong extent reactive mask is rejected",
                        "wrong extent reactive mask leaves output untouched")) {
    return false;
  }

  input = {};
  input.renderExtent = renderExtent;
  input.reactiveMask = present;
  input.reactiveMask.mipCount = 2;
  if (!CheckMaskFailure(input, hpl::FsrMaskError::BadReactiveMask,
                        "multi-mip reactive mask is rejected",
                        "multi-mip reactive mask leaves output untouched")) {
    return false;
  }

  input = {};
  input.renderExtent = renderExtent;
  input.compositionMask = present;
  input.compositionMask.layerCount = 2;
  if (!CheckMaskFailure(input, hpl::FsrMaskError::BadCompositionMask,
                        "multi-layer composition mask is rejected",
                        "multi-layer composition mask leaves output untouched")) {
    return false;
  }

  input = {};
  input.renderExtent = renderExtent;
  input.opaqueColor = present;
  input.opaqueColor.extent.height++;
  if (!CheckMaskFailure(input, hpl::FsrMaskError::BadOpaqueColor,
                        "wrong extent opaque color is rejected",
                        "wrong extent opaque color leaves output untouched")) {
    return false;
  }

  input = {};
  input.renderExtent = {0, renderExtent.height};
  if (!CheckMaskFailure(input, hpl::FsrMaskError::BadRenderExtent,
                        "mask zero render extent is rejected",
                        "mask bad render extent leaves output untouched")) {
    return false;
  }

  input = {};
  input.renderExtent = {16385, renderExtent.height};
  if (!CheckMaskFailure(input, hpl::FsrMaskError::BadRenderExtent,
                        "mask oversized render extent is rejected",
                        "mask oversized render extent leaves output untouched")) {
    return false;
  }

  hpl::FsrMaskPolicyInput validInput;
  validInput.renderExtent = renderExtent;
  if (!Check(hpl::FsrBuildMaskPolicy(validInput, nullptr) ==
                 hpl::FsrMaskError::None,
             "null mask output is safe for validation-only success")) {
    return false;
  }

  const hpl::FsrMaskError errors[] = {
      hpl::FsrMaskError::None,
      hpl::FsrMaskError::BadRenderExtent,
      hpl::FsrMaskError::BadReactiveMask,
      hpl::FsrMaskError::BadCompositionMask,
      hpl::FsrMaskError::BadOpaqueColor,
      static_cast<hpl::FsrMaskError>(-1),
      static_cast<hpl::FsrMaskError>(999),
  };
  for (const hpl::FsrMaskError error : errors) {
    const char *text = hpl::FsrMaskErrorString(error);
    if (!Check(text != nullptr && text[0] != '\0',
               "mask error string is non-null and non-empty")) {
      return false;
    }
  }
  return true;
}

} // namespace

int main() {
  if (!CheckJitterConvention() || !CheckOddExtentAndMotionScale() ||
      !CheckNativeAAAndExceedingOutput() || !CheckStationaryJitteredCamera() ||
      !CheckCameraMotionAndDelta() || !CheckDispatchValidation() ||
      !CheckParamErrorStrings() || !CheckMaskPolicy()) {
    return 1;
  }

  std::printf("fsr upscaler params checks passed (%d cases)\n", gCaseCount);
  return 0;
}
