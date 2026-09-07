#include "graphics/TemporalCamera.h"

#include <cmath>
#include <cstring>

namespace hpl {
namespace {

float Halton(uint32_t index, uint32_t base) {
  float result = 0.0f;
  float fraction = 1.0f;
  while (index != 0) {
    fraction /= static_cast<float>(base);
    result += static_cast<float>(index % base) * fraction;
    index /= base;
  }
  return result;
}

bool MatricesNearlyEqual(const float a[16], const float b[16]) {
  for (uint32_t i = 0; i < 16; ++i) {
    const float av = a[i];
    const float bv = b[i];
    const float scale = 1.0f +
                        (std::fabs(av) > std::fabs(bv) ? std::fabs(av)
                                                        : std::fabs(bv));
    if (!std::isfinite(av) || !std::isfinite(bv) ||
        std::fabs(av - bv) > 1.0e-4f * scale) {
      return false;
    }
  }
  return true;
}

} // namespace

TemporalJitter TemporalHaltonJitter(uint32_t index, uint32_t phaseCount) {
  // Halton(0) is the singular all-zero endpoint. Numbering samples from one
  // gives the first stable phase while still making phase zero deterministic.
  const uint32_t phase = phaseCount > 1 ? index % phaseCount : 0;
  const uint32_t sample = phase + 1;
  return {Halton(sample, 2) - 0.5f, Halton(sample, 3) - 0.5f};
}

void TemporalJitterToUV(TemporalJitter px, uint32_t width, uint32_t height,
                        float outUV[2]) {
  outUV[0] = width == 0 ? 0.0f : px.x / static_cast<float>(width);
  outUV[1] = height == 0 ? 0.0f : px.y / static_cast<float>(height);
}

void TemporalApplyJitterToProjection(const float unjitteredProj[16],
                                     const float jitterUV[2],
                                     float outProj[16]) {
  std::memcpy(outProj, unjitteredProj, 16 * sizeof(float));

  // Column-major, column-vector layout: row three is clip.w, at [3], [7],
  // [11], [15]. Adding offset * clip.w therefore adds that row to the x/y
  // rows. This is the (-2*x, +2*y) flipped-viewport offset. Merely changing
  // [12]/[13] would multiply the input homogeneous w instead of clip.w.
  if (jitterUV[0] != 0.0f || jitterUV[1] != 0.0f) {
    const float offsetX = -2.0f * jitterUV[0];
    const float offsetY = 2.0f * jitterUV[1];
    for (uint32_t col = 0; col < 4; ++col) {
      const uint32_t index = col * 4;
      outProj[index + 0] += offsetX * unjitteredProj[index + 3];
      outProj[index + 1] += offsetY * unjitteredProj[index + 3];
    }
  }
}

bool TemporalInvertMatrix4x4(const float m[16], float out[16]) {
  float augmented[4][8] = {};
  float scale = 0.0f;

  for (uint32_t row = 0; row < 4; ++row) {
    for (uint32_t col = 0; col < 4; ++col) {
      const float value = m[col * 4 + row];
      if (!std::isfinite(value)) {
        return false;
      }
      augmented[row][col] = value;
      const float absolute = std::fabs(value);
      if (absolute > scale) {
        scale = absolute;
      }
    }
    augmented[row][4 + row] = 1.0f;
  }

  if (scale == 0.0f) {
    return false;
  }

  const float pivotTolerance = scale * 1.0e-8f;
  for (uint32_t col = 0; col < 4; ++col) {
    uint32_t pivotRow = col;
    float pivotAbsolute = std::fabs(augmented[pivotRow][col]);
    for (uint32_t row = col + 1; row < 4; ++row) {
      const float candidate = std::fabs(augmented[row][col]);
      if (candidate > pivotAbsolute) {
        pivotAbsolute = candidate;
        pivotRow = row;
      }
    }

    if (pivotAbsolute <= pivotTolerance) {
      return false;
    }

    if (pivotRow != col) {
      for (uint32_t i = 0; i < 8; ++i) {
        const float temporary = augmented[col][i];
        augmented[col][i] = augmented[pivotRow][i];
        augmented[pivotRow][i] = temporary;
      }
    }

    const float pivot = augmented[col][col];
    for (uint32_t i = 0; i < 8; ++i) {
      augmented[col][i] /= pivot;
    }

    for (uint32_t row = 0; row < 4; ++row) {
      if (row == col) {
        continue;
      }
      const float factor = augmented[row][col];
      if (factor == 0.0f) {
        continue;
      }
      for (uint32_t i = 0; i < 8; ++i) {
        augmented[row][i] -= factor * augmented[col][i];
      }
    }
  }

  for (uint32_t row = 0; row < 4; ++row) {
    for (uint32_t col = 0; col < 4; ++col) {
      out[col * 4 + row] = augmented[row][4 + col];
    }
  }
  return true;
}

void TemporalViewportState::Reset() {
  std::memset(prevViewMat, 0, sizeof(prevViewMat));
  std::memset(prevUnjitteredProjMat, 0, sizeof(prevUnjitteredProjMat));
  std::memset(prevJitterPixels, 0, sizeof(prevJitterPixels));
  sequenceIndex = 0;
  hasPrev = false;
  prevRenderWidth = 0;
  prevRenderHeight = 0;
}

TemporalFrameSnapshot TemporalBeginFrame(TemporalViewportState &state,
                                         const TemporalFrameDesc &desc) {
  TemporalFrameSnapshot snapshot;
  snapshot.renderWidth = desc.renderWidth;
  snapshot.renderHeight = desc.renderHeight;
  snapshot.sequenceIndex = state.sequenceIndex;

  std::memcpy(snapshot.viewMat, desc.viewMat, sizeof(snapshot.viewMat));
  std::memcpy(snapshot.unjitteredProjMat, desc.unjitteredProjMat,
              sizeof(snapshot.unjitteredProjMat));
  snapshot.jitterPixels[0] = desc.jitterPixels[0];
  snapshot.jitterPixels[1] = desc.jitterPixels[1];
  TemporalJitterToUV(
      {snapshot.jitterPixels[0], snapshot.jitterPixels[1]},
      desc.renderWidth, desc.renderHeight, snapshot.jitterUV);
  TemporalApplyJitterToProjection(snapshot.unjitteredProjMat,
                                  snapshot.jitterUV, snapshot.projMat);
  if (!TemporalInvertMatrix4x4(snapshot.projMat, snapshot.invProjMat)) {
    std::memset(snapshot.invProjMat, 0, sizeof(snapshot.invProjMat));
  }

  const bool extentChanged =
      state.hasPrev &&
      (state.prevRenderWidth != desc.renderWidth ||
       state.prevRenderHeight != desc.renderHeight);
  const bool projectionChanged =
      state.hasPrev &&
      !MatricesNearlyEqual(state.prevUnjitteredProjMat,
                           snapshot.unjitteredProjMat);
  // This is advisory only. Comparing unjittered projections prevents a new
  // sample offset from looking like a projection discontinuity, and a
  // projection change by itself must not reset temporal history.
  snapshot.projectionDiscontinuity = projectionChanged;
  snapshot.historyReset = desc.forceHistoryReset || !state.hasPrev ||
                          extentChanged;

  // Only the first frame has no real previous values to copy. A forced or
  // extent reset sets historyReset but preserves the actual previous frame so
  // consumers that inspect these fields still receive correct motion data.
  if (!state.hasPrev) {
    std::memcpy(snapshot.prevViewMat, snapshot.viewMat,
                sizeof(snapshot.prevViewMat));
    std::memcpy(snapshot.prevUnjitteredProjMat, snapshot.unjitteredProjMat,
                sizeof(snapshot.prevUnjitteredProjMat));
    snapshot.prevJitterPixels[0] = snapshot.jitterPixels[0];
    snapshot.prevJitterPixels[1] = snapshot.jitterPixels[1];
  } else {
    std::memcpy(snapshot.prevViewMat, state.prevViewMat,
                sizeof(snapshot.prevViewMat));
    std::memcpy(snapshot.prevUnjitteredProjMat, state.prevUnjitteredProjMat,
                sizeof(snapshot.prevUnjitteredProjMat));
    snapshot.prevJitterPixels[0] = state.prevJitterPixels[0];
    snapshot.prevJitterPixels[1] = state.prevJitterPixels[1];
  }
  // The previous jitter is stored in pixels; converting it using this frame's
  // extent preserves the existing extent handling for previousJitterUV.
  TemporalJitterToUV(
      {snapshot.prevJitterPixels[0], snapshot.prevJitterPixels[1]},
      desc.renderWidth, desc.renderHeight, snapshot.prevJitterUV);

  // Update only after every previous-frame field has been copied into the
  // snapshot. sequenceIndex is the value consumed by this frame and is
  // advanced exactly once here, for this viewport only.
  std::memcpy(state.prevViewMat, snapshot.viewMat, sizeof(state.prevViewMat));
  std::memcpy(state.prevUnjitteredProjMat, snapshot.unjitteredProjMat,
              sizeof(state.prevUnjitteredProjMat));
  state.prevJitterPixels[0] = snapshot.jitterPixels[0];
  state.prevJitterPixels[1] = snapshot.jitterPixels[1];
  state.prevRenderWidth = desc.renderWidth;
  state.prevRenderHeight = desc.renderHeight;
  state.hasPrev = true;
  ++state.sequenceIndex;

  return snapshot;
}

TemporalJitter TemporalPendingJitter(const TemporalViewportState &state,
                                     uint32_t phaseCount) {
  return TemporalHaltonJitter(state.sequenceIndex, phaseCount);
}

} // namespace hpl
