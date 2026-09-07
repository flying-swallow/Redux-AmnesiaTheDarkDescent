#ifndef HPL_TEMPORAL_CAMERA_H
#define HPL_TEMPORAL_CAMERA_H

// Temporal-camera convention:
//
// Pixel coordinates use a top-left origin. pixelUV = (pixel + 0.5) /
// renderExtent, with y increasing downwards. jitterPixels is the sample offset
// from that pixel center, in render-extent pixels, and each component is in
// [-0.5, 0.5]. jitterUV = jitterPixels / renderExtent, also top-left and
// y-down. A pinhole ray for a pixel passes through pixelUV + jitterUV.
//
// This engine rasterizes with a flipped viewport:
//     ndc = float2(2, -2) * uv + float2(-1, 1)
// Therefore a jittered projection applies the clip-space offset
// (-2 * jitterUV.x, +2 * jitterUV.y) * clip.w. Geometry consequently moves by
// -jitter, so sampling the center of a jittered raster pixel samples
// pixelUV + jitterUV in the unjittered image.
//
// Matrices are float[16], column-major, and use column-vector math. To add a
// per-clip-w offset, TemporalApplyJitterToProjection adds the projection's
// output-w row ([3], [7], [11], [15]) to the output-x/output-y rows. For a
// conventional perspective matrix this changes [8] and [9], not [12] and
// [13]: [12]/[13] multiply the input homogeneous w.

#include <cstdint>

namespace hpl {

struct TemporalJitter {
  float x = 0.0f;
  float y = 0.0f;
};

// Returns a Halton(2, 3) sample remapped from [0, 1] to [-0.5, 0.5]. A
// phaseCount of zero or one selects the single first phase; otherwise the
// caller-supplied phase is index % phaseCount.
TemporalJitter TemporalHaltonJitter(uint32_t index, uint32_t phaseCount);

void TemporalJitterToUV(TemporalJitter px, uint32_t width, uint32_t height,
                        float outUV[2]);

// Applies the signed clip-space offset required by the flipped viewport.
void TemporalApplyJitterToProjection(const float unjitteredProj[16],
                                     const float jitterUV[2],
                                     float outProj[16]);

bool TemporalInvertMatrix4x4(const float m[16], float out[16]);

struct TemporalViewportState {
  float prevViewMat[16] = {};
  float prevUnjitteredProjMat[16] = {};
  float prevJitterPixels[2] = {};
  uint32_t sequenceIndex = 0; // The phase index the next frame will use.
  bool hasPrev = false;

  // These fields support extent-discontinuity detection. They are not camera
  // matrices and are kept here so each viewport advances independently.
  uint32_t prevRenderWidth = 0;
  uint32_t prevRenderHeight = 0;

  void Reset();
};

struct TemporalFrameDesc {
  const float *viewMat = nullptr;
  const float *unjitteredProjMat = nullptr;
  uint32_t renderWidth = 0;
  uint32_t renderHeight = 0;
  float jitterPixels[2] = {};
  bool forceHistoryReset = false;
};

struct TemporalFrameSnapshot {
  float viewMat[16] = {};
  float projMat[16] = {};
  float invProjMat[16] = {};
  float unjitteredProjMat[16] = {};
  float prevViewMat[16] = {};
  float prevUnjitteredProjMat[16] = {};
  float jitterPixels[2] = {};
  float jitterUV[2] = {};
  float prevJitterPixels[2] = {};
  float prevJitterUV[2] = {};
  uint32_t renderWidth = 0;
  uint32_t renderHeight = 0;
  uint32_t sequenceIndex = 0;
  // Advisory camera-cut hint: true when the unjittered projection changed
  // beyond the comparison tolerance. This does not reset temporal history;
  // comparing unjittered projections keeps a new sample offset from looking
  // like a projection discontinuity.
  bool projectionDiscontinuity = false;
  bool historyReset = false;
};

TemporalFrameSnapshot TemporalBeginFrame(TemporalViewportState &state,
                                         const TemporalFrameDesc &desc);

// Returns the jitter selected by the state without changing the state.
TemporalJitter TemporalPendingJitter(const TemporalViewportState &state,
                                     uint32_t phaseCount);

} // namespace hpl

#endif // HPL_TEMPORAL_CAMERA_H
