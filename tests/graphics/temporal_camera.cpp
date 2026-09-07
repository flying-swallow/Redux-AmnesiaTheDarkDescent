#include "graphics/TemporalCamera.h"
#include "graphics/FsrUpscalerParams.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

bool RunTemporalReactiveMaskTests();
bool RunWaterReflectionJitterTests();
bool RunTemporalUpscalerPolicyTests();
bool RunDisplayDepthPolicyTests();
bool RunRayConeLodTests();
bool RunCubeMipGenTests();
bool RunBlockCompressionDecodeTests();
bool RunBcBlockLayoutTests();
bool RunUploadRowPitchTests();

namespace {

using hpl::TemporalBeginFrame;
using hpl::TemporalFrameDesc;
using hpl::TemporalFrameSnapshot;
using hpl::TemporalJitter;
using hpl::TemporalJitterToUV;
using hpl::TemporalPendingJitter;
using hpl::TemporalViewportState;

bool Near(float a, float b, float epsilon = 2.0e-5f) {
  return std::fabs(a - b) <= epsilon;
}

bool NearMatrix(const float a[16], const float b[16],
                float epsilon = 3.0e-4f) {
  for (int i = 0; i < 16; ++i) {
    if (!Near(a[i], b[i], epsilon)) {
      return false;
    }
  }
  return true;
}

void Identity(float matrix[16]) {
  for (int i = 0; i < 16; ++i) {
    matrix[i] = 0.0f;
  }
  matrix[0] = 1.0f;
  matrix[5] = 1.0f;
  matrix[10] = 1.0f;
  matrix[15] = 1.0f;
}

void Perspective(float matrix[16]) {
  for (int i = 0; i < 16; ++i) {
    matrix[i] = 0.0f;
  }
  const float f = 1.0f / std::tan(0.5f * 1.0471975512f);
  const float aspect = 16.0f / 9.0f;
  const float nearPlane = 0.1f;
  const float farPlane = 100.0f;
  matrix[0] = f / aspect;
  matrix[5] = f;
  matrix[10] = (farPlane + nearPlane) / (nearPlane - farPlane);
  matrix[11] = -1.0f;
  matrix[14] = (2.0f * farPlane * nearPlane) / (nearPlane - farPlane);
}

void Multiply(const float a[16], const float b[16], float out[16]) {
  for (int col = 0; col < 4; ++col) {
    for (int row = 0; row < 4; ++row) {
      out[col * 4 + row] = 0.0f;
      for (int k = 0; k < 4; ++k) {
        out[col * 4 + row] += a[k * 4 + row] * b[col * 4 + k];
      }
    }
  }
}

void Transform(const float matrix[16], const float point[4], float out[4]) {
  for (int row = 0; row < 4; ++row) {
    out[row] = matrix[row] * point[0] + matrix[4 + row] * point[1] +
               matrix[8 + row] * point[2] + matrix[12 + row] * point[3];
  }
}

void ClipToUV(const float clip[4], float uv[2]) {
  const float inverseW = 1.0f / clip[3];
  const float ndcX = clip[0] * inverseW;
  const float ndcY = clip[1] * inverseW;
  uv[0] = 0.5f * (ndcX + 1.0f);
  uv[1] = 0.5f * (1.0f - ndcY);
}

void ProjectWithView(const float projection[16], const float view[16],
                     const float point[4], float uv[2]) {
  float viewPoint[4];
  float clip[4];
  Transform(view, point, viewPoint);
  Transform(projection, viewPoint, clip);
  ClipToUV(clip, uv);
}

void PointAtUV(const float projection[16], float uvX, float uvY, float z,
              float point[4]) {
  const float w = -z;
  const float ndcX = 2.0f * uvX - 1.0f;
  const float ndcY = 1.0f - 2.0f * uvY;
  point[0] = ndcX * w / projection[0];
  point[1] = ndcY * w / projection[5];
  point[2] = z;
  point[3] = 1.0f;
}

bool Check(bool condition, const char *name) {
  if (!condition) {
    std::printf("failed: %s\n", name);
    return false;
  }
  return true;
}

TemporalFrameDesc Desc(const float view[16], const float projection[16],
                       uint32_t width, uint32_t height, float jitterX,
                       float jitterY) {
  TemporalFrameDesc desc;
  desc.viewMat = view;
  desc.unjitteredProjMat = projection;
  desc.renderWidth = width;
  desc.renderHeight = height;
  desc.jitterPixels[0] = jitterX;
  desc.jitterPixels[1] = jitterY;
  return desc;
}

bool CheckIdentityProduct(const float matrix[16], const float inverse[16],
                          const char *name) {
  float product[16];
  float identity[16];
  Multiply(matrix, inverse, product);
  Identity(identity);
  return Check(NearMatrix(product, identity, 5.0e-4f), name);
}

bool CheckZeroJitterAndInverse(const float projection[16], const float view[16]) {
  const float zeroUV[2] = {0.0f, 0.0f};
  float copied[16];
  hpl::TemporalApplyJitterToProjection(projection, zeroUV, copied);
  if (!Check(std::memcmp(projection, copied, sizeof(copied)) == 0,
             "zero jitter is bit-identical")) {
    return false;
  }

  float inverse[16];
  if (!Check(hpl::TemporalInvertMatrix4x4(projection, inverse),
             "unjittered projection is invertible") ||
      !CheckIdentityProduct(projection, inverse,
                            "unjittered projection inverse")) {
    return false;
  }

  const float nonzeroJitterUV[2] = {0.37f / 1920.0f, -0.29f / 1080.0f};
  float jitteredProjection[16];
  float jitteredInverse[16];
  hpl::TemporalApplyJitterToProjection(projection, nonzeroJitterUV,
                                       jitteredProjection);
  if (!Check(hpl::TemporalInvertMatrix4x4(jitteredProjection, jitteredInverse),
             "jittered projection is invertible") ||
      !CheckIdentityProduct(jitteredProjection, jitteredInverse,
                            "jittered projection inverse")) {
    return false;
  }

  TemporalViewportState state;
  const TemporalFrameDesc desc = Desc(view, projection, 1920, 1080, 0.0f, 0.0f);
  const TemporalFrameSnapshot snapshot = TemporalBeginFrame(state, desc);
  return Check(std::memcmp(snapshot.projMat, projection,
                           sizeof(snapshot.projMat)) == 0,
               "zero-jitter snapshot projection") &&
         CheckIdentityProduct(snapshot.projMat, snapshot.invProjMat,
                              "jittered snapshot inverse");
}

bool CheckRasterAndPinhole(const float projection[16]) {
  const float sampleLocations[5][2] = {{0.5f, 0.5f},
                                       {0.02f, 0.02f},
                                       {0.98f, 0.02f},
                                       {0.02f, 0.98f},
                                       {0.98f, 0.98f}};
  const TemporalJitter jitters[] = {
      {0.41f, 0.0f},  {-0.41f, 0.0f}, {0.0f, 0.43f},
      {0.0f, -0.43f}, {0.37f, -0.31f}, {-0.37f, 0.31f}};

  float inverse[16];
  if (!Check(hpl::TemporalInvertMatrix4x4(projection, inverse),
             "pinhole projection inverse")) {
    return false;
  }

  for (const TemporalJitter jitter : jitters) {
    const float jitterUV[2] = {jitter.x / 1920.0f, jitter.y / 1080.0f};
    float jitteredProjection[16];
    hpl::TemporalApplyJitterToProjection(projection, jitterUV,
                                         jitteredProjection);

    for (int location = 0; location < 5; ++location) {
      float world[4];
      PointAtUV(projection, sampleLocations[location][0],
                sampleLocations[location][1], -2.0f, world);
      float unjitteredClip[4];
      float jitteredClip[4];
      Transform(projection, world, unjitteredClip);
      Transform(jitteredProjection, world, jitteredClip);

      float unjitteredUV[2];
      float rasterUV[2];
      ClipToUV(unjitteredClip, unjitteredUV);
      ClipToUV(jitteredClip, rasterUV);
      const bool rasterAgrees =
          Near(rasterUV[0], unjitteredUV[0] - jitterUV[0], 2.0e-5f) &&
          Near(rasterUV[1], unjitteredUV[1] - jitterUV[1], 2.0e-5f);
      if (!rasterAgrees) {
        Check(false, "flipped raster agrees with pinhole sample");
        return false;
      }

      // The jittered raster pixel center samples this point's unjittered UV
      // after adding the sample offset back before pinhole reconstruction.
      const float sampleUV[2] = {rasterUV[0] + jitterUV[0],
                                 rasterUV[1] + jitterUV[1]};
      float reconstructedClip[4] = {
          2.0f * sampleUV[0] - 1.0f,
          1.0f - 2.0f * sampleUV[1],
          unjitteredClip[2] / unjitteredClip[3], 1.0f};
      float reconstructed[4];
      Transform(inverse, reconstructedClip, reconstructed);
      const float inverseW = 1.0f / reconstructed[3];
      if (!Check(Near(reconstructed[0] * inverseW, world[0], 4.0e-5f) &&
                     Near(reconstructed[1] * inverseW, world[1], 4.0e-5f) &&
                     Near(reconstructed[2] * inverseW, world[2], 4.0e-5f),
                 "pinhole reconstruction lands on world point")) {
        return false;
      }
    }
  }
  return true;
}

bool CheckHistoryLookup(const float projection[16], const float view[16]) {
  TemporalViewportState state;
  const TemporalFrameDesc first = Desc(view, projection, 1920, 1080, 0.35f,
                                       -0.27f);
  const TemporalFrameDesc second = Desc(view, projection, 1920, 1080, -0.31f,
                                        0.22f);
  const TemporalFrameSnapshot firstSnapshot = TemporalBeginFrame(state, first);
  const TemporalFrameSnapshot secondSnapshot =
      TemporalBeginFrame(state, second);

  const float world[4] = {0.4f, -0.25f, -3.0f, 1.0f};
  float firstClip[4];
  float secondClip[4];
  Transform(firstSnapshot.projMat, world, firstClip);
  Transform(secondSnapshot.projMat, world, secondClip);
  float firstPixelUV[2];
  float currentPixelUV[2];
  ClipToUV(firstClip, firstPixelUV);
  ClipToUV(secondClip, currentPixelUV);

  float previousUnjitteredClip[4];
  float currentUnjitteredClip[4];
  Transform(firstSnapshot.unjitteredProjMat, world, previousUnjitteredClip);
  Transform(secondSnapshot.unjitteredProjMat, world, currentUnjitteredClip);
  float previousUV[2];
  float currentUV[2];
  ClipToUV(previousUnjitteredClip, previousUV);
  ClipToUV(currentUnjitteredClip, currentUV);
  const float velocity[2] = {currentUV[0] - previousUV[0],
                             currentUV[1] - previousUV[1]};
  const float historyUV[2] = {
      currentPixelUV[0] - velocity[0] + secondSnapshot.jitterUV[0] -
          secondSnapshot.prevJitterUV[0],
      currentPixelUV[1] - velocity[1] + secondSnapshot.jitterUV[1] -
          secondSnapshot.prevJitterUV[1]};

  return Check(firstSnapshot.historyReset && !secondSnapshot.historyReset,
               "jitter alternation does not reset history") &&
         Check(Near(firstSnapshot.prevJitterPixels[0], first.jitterPixels[0]) &&
                  Near(firstSnapshot.prevJitterPixels[1], first.jitterPixels[1]),
              "history stores previous jitter") &&
         Check(Near(historyUV[0], firstPixelUV[0], 3.0e-5f) &&
                  Near(historyUV[1], firstPixelUV[1], 3.0e-5f),
              "history lookup recovers previous raster pixel");
}

bool CheckProjectionDiscontinuity(const float projection[16],
                                  const float view[16]) {
  float changedProjection[16];
  std::memcpy(changedProjection, projection, sizeof(changedProjection));
  changedProjection[0] *= 1.15f;
  changedProjection[5] *= 1.15f;

  float movedView[16];
  std::memcpy(movedView, view, sizeof(movedView));
  movedView[12] = -0.2f;
  movedView[13] = 0.1f;

  TemporalViewportState state;
  const TemporalFrameSnapshot first = TemporalBeginFrame(
      state, Desc(view, projection, 1920, 1080, 0.0f, 0.0f));
  const TemporalFrameSnapshot projectionChange = TemporalBeginFrame(
      state, Desc(movedView, changedProjection, 1920, 1080, 0.0f, 0.0f));

  const float world[4] = {0.4f, -0.25f, -3.0f, 1.0f};
  float previousUV[2];
  float currentUV[2];
  ProjectWithView(projectionChange.prevUnjitteredProjMat,
                  projectionChange.prevViewMat, world, previousUV);
  ProjectWithView(projectionChange.unjitteredProjMat,
                  projectionChange.viewMat, world, currentUV);
  const float velocity[2] = {currentUV[0] - previousUV[0],
                             currentUV[1] - previousUV[1]};

  if (!Check(!projectionChange.historyReset,
             "projection change does not reset history") ||
      !Check(projectionChange.projectionDiscontinuity,
             "projection change reports discontinuity") ||
      !Check(NearMatrix(projectionChange.prevViewMat, first.viewMat),
             "projection change keeps true previous view") ||
      !Check(NearMatrix(projectionChange.prevUnjitteredProjMat,
                        first.unjitteredProjMat),
             "projection change keeps true previous projection") ||
      !Check(!Near(velocity[0], 0.0f) || !Near(velocity[1], 0.0f),
             "projection change retains camera velocity")) {
    return false;
  }

  const TemporalFrameSnapshot jitterChange = TemporalBeginFrame(
      state, Desc(movedView, changedProjection, 1920, 1080, 0.31f, -0.23f));
  return Check(!jitterChange.historyReset,
               "jitter-only change does not reset history") &&
         Check(!jitterChange.projectionDiscontinuity,
               "jitter-only change has no projection discontinuity") &&
         Check(NearMatrix(jitterChange.prevViewMat,
                          projectionChange.viewMat) &&
                  NearMatrix(jitterChange.prevUnjitteredProjMat,
                             projectionChange.unjitteredProjMat),
              "jitter-only change keeps previous camera");
}

bool CheckMovingCameraJitterIndependence(const float projection[16]) {
  float previousView[16];
  float currentView[16];
  Identity(previousView);
  Identity(currentView);
  currentView[12] = -0.2f;
  currentView[13] = 0.1f;

  TemporalViewportState stateA;
  TemporalViewportState stateB;
  TemporalBeginFrame(stateA,
                     Desc(previousView, projection, 1920, 1080, 0.4f, -0.3f));
  TemporalBeginFrame(stateB,
                     Desc(previousView, projection, 1920, 1080, -0.4f, 0.3f));
  const TemporalFrameSnapshot a = TemporalBeginFrame(
      stateA, Desc(currentView, projection, 1920, 1080, -0.2f, 0.25f));
  const TemporalFrameSnapshot b = TemporalBeginFrame(
      stateB, Desc(currentView, projection, 1920, 1080, 0.2f, -0.25f));

  const float world[4] = {-0.3f, 0.2f, -3.0f, 1.0f};
  // Projecting through the frame view matrices is the unjittered motion input.
  float aPreviousUV[2];
  float aCurrentUV[2];
  ProjectWithView(a.prevUnjitteredProjMat, a.prevViewMat, world, aPreviousUV);
  ProjectWithView(a.unjitteredProjMat, a.viewMat, world, aCurrentUV);
  const float velocityA[2] = {aCurrentUV[0] - aPreviousUV[0],
                              aCurrentUV[1] - aPreviousUV[1]};

  float bPreviousUV[2];
  float bCurrentUV[2];
  ProjectWithView(b.prevUnjitteredProjMat, b.prevViewMat, world, bPreviousUV);
  ProjectWithView(b.unjitteredProjMat, b.viewMat, world, bCurrentUV);
  const float velocityB[2] = {bCurrentUV[0] - bPreviousUV[0],
                              bCurrentUV[1] - bPreviousUV[1]};

  return Check(Near(velocityA[0], velocityB[0], 2.0e-6f) &&
                  Near(velocityA[1], velocityB[1], 2.0e-6f),
              "moving-camera velocity ignores jitter") &&
         Check(!Near(velocityA[0], 0.0f) || !Near(velocityA[1], 0.0f),
               "moving-camera case is non-stationary");
}

bool CheckIndependentStates(const float projection[16], const float view[16]) {
  TemporalViewportState first;
  TemporalViewportState second;
  const TemporalFrameDesc initial = Desc(view, projection, 640, 360, 0.0f, 0.0f);
  const TemporalFrameSnapshot firstInitial =
      TemporalBeginFrame(first, initial);
  const TemporalFrameSnapshot secondInitial =
      TemporalBeginFrame(second, initial);
  if (!Check(firstInitial.historyReset && secondInitial.historyReset,
             "both states reset on first frame") ||
      !Check(firstInitial.sequenceIndex == 0 && secondInitial.sequenceIndex == 0,
             "both states begin at sequence zero")) {
    return false;
  }

  const uint32_t skippedSequence = second.sequenceIndex;
  const TemporalFrameSnapshot firstJitter = TemporalBeginFrame(
      first, Desc(view, projection, 640, 360, 0.45f, -0.45f));
  if (!Check(!firstJitter.historyReset && firstJitter.sequenceIndex == 1,
             "jitter change alone does not reset") ||
      !Check(second.sequenceIndex == skippedSequence,
             "skipped viewport does not advance") ||
      !Check(TemporalPendingJitter(second, 8).x ==
                 hpl::TemporalHaltonJitter(skippedSequence, 8).x,
             "pending jitter has no side effects")) {
    return false;
  }

  const TemporalFrameSnapshot extentReset = TemporalBeginFrame(
      first, Desc(view, projection, 800, 360, 0.0f, 0.0f));
  TemporalFrameDesc forcedDesc =
      Desc(view, projection, 640, 360, 0.1f, 0.1f);
  forcedDesc.forceHistoryReset = true;
  const TemporalFrameSnapshot forcedReset =
      TemporalBeginFrame(second, forcedDesc);
  return Check(extentReset.historyReset,
               "render extent change resets history") &&
         Check(forcedReset.historyReset,
               "forced reset sets history reset") &&
         Check(first.sequenceIndex == 3 && second.sequenceIndex == 2,
               "state sequence indices advance independently");
}

bool CheckMaskPolicy() {
  const hpl::FsrExtent renderExtent = {1920, 1080};
  const auto Present = [renderExtent]() {
    hpl::FsrMaskBindingInfo binding;
    binding.present = true;
    binding.extent = renderExtent;
    return binding;
  };

  hpl::FsrMaskPolicyInput input;
  input.renderExtent = renderExtent;
  input.opaqueColor = Present();
  hpl::FsrMaskPolicy policy;
  if (!Check(hpl::FsrBuildMaskPolicy(input, &policy) ==
                 hpl::FsrMaskError::None,
             "opaque-only mask policy validates") ||
      !Check(!policy.bindReactiveMask && !policy.bindCompositionMask &&
                 policy.bindOpaqueColor && policy.enableAutoReactive,
             "opaque-only mask policy enables automatic reactive generation")) {
    return false;
  }

  input.opaqueColor = {};
  if (!Check(hpl::FsrBuildMaskPolicy(input, &policy) ==
                 hpl::FsrMaskError::None,
             "no-mask policy validates") ||
      !Check(!policy.bindReactiveMask && !policy.bindCompositionMask &&
                 !policy.bindOpaqueColor && !policy.enableAutoReactive,
             "no-mask policy binds nothing and disables automatic generation")) {
    return false;
  }

  input.reactiveMask = Present();
  if (!Check(hpl::FsrBuildMaskPolicy(input, &policy) ==
                 hpl::FsrMaskError::None,
             "reactive-only mask policy validates") ||
      !Check(policy.bindReactiveMask && !policy.bindCompositionMask &&
                 !policy.bindOpaqueColor && !policy.enableAutoReactive,
             "reactive-only policy disables automatic generation")) {
    return false;
  }

  input.reactiveMask = {};
  input.compositionMask = Present();
  if (!Check(hpl::FsrBuildMaskPolicy(input, &policy) ==
                 hpl::FsrMaskError::None,
             "composition-only mask policy validates") ||
      !Check(!policy.bindReactiveMask && policy.bindCompositionMask &&
                 !policy.bindOpaqueColor && !policy.enableAutoReactive,
             "composition-only policy disables automatic generation")) {
    return false;
  }

  input.reactiveMask = Present();
  input.opaqueColor = Present();
  if (!Check(hpl::FsrBuildMaskPolicy(input, &policy) ==
                 hpl::FsrMaskError::None,
             "both-mask policy validates") ||
      !Check(policy.bindReactiveMask && policy.bindCompositionMask &&
                 !policy.bindOpaqueColor && !policy.enableAutoReactive,
             "both-mask policy does not bind opaque color")) {
    return false;
  }

  hpl::FsrMaskPolicy untouched = {true, true, true, true};
  input.compositionMask = {};
  input.reactiveMask.extent.height = 1079;
  if (!Check(hpl::FsrBuildMaskPolicy(input, &untouched) ==
                 hpl::FsrMaskError::BadReactiveMask,
             "wrong-extent reactive mask is rejected") ||
      !Check(untouched.bindReactiveMask && untouched.bindCompositionMask &&
                 untouched.bindOpaqueColor && untouched.enableAutoReactive,
             "wrong-extent failure leaves policy output untouched")) {
    return false;
  }

  input.reactiveMask = Present();
  input.reactiveMask.mipCount = 2;
  if (!Check(hpl::FsrBuildMaskPolicy(input, nullptr) ==
                 hpl::FsrMaskError::BadReactiveMask,
             "multi-mip reactive mask is rejected")) {
    return false;
  }

  input.reactiveMask = {};
  input.compositionMask = Present();
  input.compositionMask.layerCount = 2;
  if (!Check(hpl::FsrBuildMaskPolicy(input, nullptr) ==
                 hpl::FsrMaskError::BadCompositionMask,
             "multi-layer composition mask is rejected")) {
    return false;
  }

  input.compositionMask = {};
  input.renderExtent = {0, 1080};
  if (!Check(hpl::FsrBuildMaskPolicy(input, nullptr) ==
                 hpl::FsrMaskError::BadRenderExtent,
             "zero render extent is rejected")) {
    return false;
  }
  input.renderExtent = {16385, 1080};
  if (!Check(hpl::FsrBuildMaskPolicy(input, nullptr) ==
                 hpl::FsrMaskError::BadRenderExtent,
             "oversized render extent is rejected")) {
    return false;
  }

  const struct {
    hpl::FsrMaskError error;
    const char *text;
  } errors[] = {
      {hpl::FsrMaskError::None, "none"},
      {hpl::FsrMaskError::BadRenderExtent, "bad render extent"},
      {hpl::FsrMaskError::BadReactiveMask, "bad reactive mask"},
      {hpl::FsrMaskError::BadCompositionMask, "bad composition mask"},
      {hpl::FsrMaskError::BadOpaqueColor, "bad opaque color"},
  };
  for (const auto &entry : errors) {
    if (!Check(hpl::FsrMaskErrorString(entry.error) != nullptr &&
                   std::strcmp(hpl::FsrMaskErrorString(entry.error), entry.text) ==
                       0,
               "mask error string is stable and non-null")) {
      return false;
    }
  }
  return Check(std::strcmp(hpl::FsrMaskErrorString(
                               static_cast<hpl::FsrMaskError>(999)),
                           "unknown FSR mask error") == 0,
               "unknown mask error string is stable");
}

} // namespace

int main() {
  float projection[16];
  float view[16];
  Perspective(projection);
  Identity(view);

  if (!CheckZeroJitterAndInverse(projection, view) ||
      !CheckRasterAndPinhole(projection) ||
      !CheckHistoryLookup(projection, view) ||
      !CheckProjectionDiscontinuity(projection, view) ||
      !CheckMovingCameraJitterIndependence(projection) ||
      !CheckIndependentStates(projection, view) ||
      !CheckMaskPolicy() ||
      !RunTemporalUpscalerPolicyTests() ||
      !RunDisplayDepthPolicyTests() ||
      !RunRayConeLodTests() ||
      !RunCubeMipGenTests() ||
      !RunBlockCompressionDecodeTests() ||
      !RunBcBlockLayoutTests() ||
      !RunUploadRowPitchTests() ||
      !RunTemporalReactiveMaskTests() ||
      !RunWaterReflectionJitterTests()) {
    return 1;
  }

  std::printf("temporal camera checks passed\n");
  return 0;
}
