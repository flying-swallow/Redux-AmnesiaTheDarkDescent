#ifndef HPL_DISPLAY_DEPTH_POLICY_H
#define HPL_DISPLAY_DEPTH_POLICY_H

#include <cstdint>

namespace hpl {

struct DisplayDepthExtent {
  uint32_t width = 0;
  uint32_t height = 0;
};

// A candidate is described by the image extent that was actually allocated,
// not by a negotiated or logical extent. Both the image and its attachment
// view must exist before the candidate can be selected.
struct DisplayDepthCandidate {
  DisplayDepthExtent allocatedExtent = {};
  bool hasImage = false;
  bool hasAttachmentView = false;
};

struct DisplayDepthInputs {
  DisplayDepthExtent displayExtent = {};   // logical output extent
  DisplayDepthCandidate presentation = {}; // resolved display-side depth
  bool presentationCurrent = false; // owner already checked frame index, extent
                                    // and swapchain index
  DisplayDepthCandidate scene = {}; // render-extent scene depth, from the
                                    // viewport state's own width/height
  bool sceneIndexInRange = false;   // active swapchain index < array size
  bool sceneExtentCompatible = false; // negotiated render extent == display
                                      // extent guard
};

// X11's X.h (pulled in via the Vulkan WSI headers by whatever included this)
// defines None as a macro, which collides with the enumerator below. Nothing
// in the engine uses the X11 macro directly and X.h is include-guarded, so it
// stays gone — same treatment as DebugDraw.h.
#ifdef None
#undef None
#endif

enum class DisplayDepthSource { None, Presentation, Scene };

inline DisplayDepthSource SelectDisplayDepth(const DisplayDepthInputs &in) {
  if (in.displayExtent.width == 0 || in.displayExtent.height == 0)
    return DisplayDepthSource::None;

  if (in.presentationCurrent && in.presentation.hasImage &&
      in.presentation.hasAttachmentView &&
      in.presentation.allocatedExtent.width == in.displayExtent.width &&
      in.presentation.allocatedExtent.height == in.displayExtent.height)
    return DisplayDepthSource::Presentation;

  if (in.sceneIndexInRange && in.scene.hasImage &&
      in.scene.hasAttachmentView && in.sceneExtentCompatible &&
      in.scene.allocatedExtent.width == in.displayExtent.width &&
      in.scene.allocatedExtent.height == in.displayExtent.height)
    return DisplayDepthSource::Scene;

  return DisplayDepthSource::None;
}

inline DisplayDepthSource SelectDisplayDepthForExtent(
    const DisplayDepthInputs &in, uint32_t width, uint32_t height) {
  if (width == 0 || height == 0 || width != in.displayExtent.width ||
      height != in.displayExtent.height)
    return DisplayDepthSource::None;

  return SelectDisplayDepth(in);
}

} // namespace hpl

#endif // HPL_DISPLAY_DEPTH_POLICY_H
