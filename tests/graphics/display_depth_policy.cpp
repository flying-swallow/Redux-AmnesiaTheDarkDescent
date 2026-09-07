#include "graphics/DisplayDepthPolicy.h"

#include <cstdio>

namespace {

using hpl::DisplayDepthCandidate;
using hpl::DisplayDepthInputs;
using hpl::DisplayDepthSource;

bool Check(bool condition, const char *name) {
  if (!condition) {
    std::printf("failed: %s\n", name);
    return false;
  }
  return true;
}

DisplayDepthCandidate Candidate(uint32_t width, uint32_t height,
                                bool hasImage = true,
                                bool hasAttachmentView = true) {
  DisplayDepthCandidate candidate;
  candidate.allocatedExtent = {width, height};
  candidate.hasImage = hasImage;
  candidate.hasAttachmentView = hasAttachmentView;
  return candidate;
}

DisplayDepthInputs NativeSceneInputs() {
  DisplayDepthInputs inputs;
  inputs.displayExtent = {1920, 1080};
  inputs.scene = Candidate(1920, 1080);
  inputs.sceneIndexInRange = true;
  inputs.sceneExtentCompatible = true;
  return inputs;
}

bool CheckAllocationContract() {
  DisplayDepthInputs inputs;
  inputs.displayExtent = {1920, 1080};
  inputs.scene = Candidate(960, 540);
  inputs.sceneIndexInRange = true;
  if (!Check(hpl::SelectDisplayDepth(inputs) == DisplayDepthSource::None,
             "reduced scene allocation is rejected"))
    return false;

  inputs.sceneExtentCompatible = true;
  return Check(hpl::SelectDisplayDepth(inputs) == DisplayDepthSource::None,
               "compatible logical extent does not prove allocation size");
}

bool CheckPresentationPreference() {
  DisplayDepthInputs inputs = NativeSceneInputs();
  inputs.presentation = Candidate(1920, 1080);
  inputs.presentationCurrent = true;
  inputs.scene = Candidate(960, 540);
  return Check(hpl::SelectDisplayDepth(inputs) ==
                   DisplayDepthSource::Presentation,
               "current presentation depth wins over reduced scene depth");
}

bool CheckSceneFallback() {
  DisplayDepthInputs inputs = NativeSceneInputs();
  inputs.presentation = Candidate(1920, 1080);
  inputs.presentationCurrent = false;
  if (!Check(hpl::SelectDisplayDepth(inputs) == DisplayDepthSource::Scene,
             "stale presentation falls back to native scene depth"))
    return false;

  inputs.scene.hasAttachmentView = false;
  if (!Check(hpl::SelectDisplayDepth(inputs) == DisplayDepthSource::None,
             "missing scene attachment view has no fallback"))
    return false;

  inputs.scene.hasAttachmentView = true;
  inputs.sceneIndexInRange = false;
  return Check(hpl::SelectDisplayDepth(inputs) == DisplayDepthSource::None,
               "out-of-range scene index has no fallback");
}

bool CheckRequestedExtent() {
  DisplayDepthInputs inputs;
  inputs.displayExtent = {960, 540};
  inputs.scene = Candidate(960, 540);
  inputs.sceneIndexInRange = true;
  inputs.sceneExtentCompatible = true;
  if (!Check(hpl::SelectDisplayDepthForExtent(inputs, 1920, 1080) ==
                 DisplayDepthSource::None,
             "offscreen depth is rejected for a larger GUI extent"))
    return false;

  return Check(hpl::SelectDisplayDepthForExtent(inputs, 960, 540) ==
                   DisplayDepthSource::Scene,
               "offscreen depth is selected at its own extent");
}

bool CheckExtentMismatches() {
  DisplayDepthInputs inputs;
  inputs.displayExtent = {1920, 1080};
  inputs.presentation = Candidate(1919, 1080);
  inputs.presentationCurrent = true;
  if (!Check(hpl::SelectDisplayDepth(inputs) == DisplayDepthSource::None,
             "presentation width mismatch is rejected"))
    return false;

  inputs.presentation = Candidate(1920, 1079);
  if (!Check(hpl::SelectDisplayDepth(inputs) == DisplayDepthSource::None,
             "presentation height mismatch is rejected"))
    return false;

  inputs.presentationCurrent = false;
  inputs.sceneIndexInRange = true;
  inputs.sceneExtentCompatible = true;
  inputs.scene = Candidate(1919, 1080);
  if (!Check(hpl::SelectDisplayDepth(inputs) == DisplayDepthSource::None,
             "scene width mismatch is rejected"))
    return false;

  inputs.scene = Candidate(1920, 1079);
  return Check(hpl::SelectDisplayDepth(inputs) == DisplayDepthSource::None,
               "scene height mismatch is rejected");
}

bool CheckInvalidInputs() {
  DisplayDepthInputs inputs = NativeSceneInputs();
  inputs.displayExtent = {0, 0};
  if (!Check(hpl::SelectDisplayDepth(inputs) == DisplayDepthSource::None,
             "zero display extent is rejected"))
    return false;

  inputs = NativeSceneInputs();
  if (!Check(hpl::SelectDisplayDepthForExtent(inputs, 0, 1080) ==
                 DisplayDepthSource::None,
             "zero requested width is rejected") ||
      !Check(hpl::SelectDisplayDepthForExtent(inputs, 1920, 0) ==
                 DisplayDepthSource::None,
             "zero requested height is rejected"))
    return false;

  inputs = NativeSceneInputs();
  inputs.scene = Candidate(1920, 1080, false, true);
  if (!Check(hpl::SelectDisplayDepth(inputs) == DisplayDepthSource::None,
             "missing scene image is rejected"))
    return false;

  inputs.scene = Candidate(1920, 1080, true, false);
  return Check(hpl::SelectDisplayDepth(inputs) == DisplayDepthSource::None,
               "missing scene attachment view is rejected");
}

} // namespace

bool RunDisplayDepthPolicyTests() {
  if (!CheckAllocationContract() || !CheckPresentationPreference() ||
      !CheckSceneFallback() || !CheckRequestedExtent() ||
      !CheckExtentMismatches() || !CheckInvalidInputs())
    return false;

  std::printf("display depth policy checks passed\n");
  return true;
}
