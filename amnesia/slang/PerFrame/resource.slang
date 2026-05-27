#pragma once

// Per-frame UBO binding for Slang shaders. Mirrors the GLSL declaration at
// amnesia/glsl/per_frame.resource.glsl::PerFrameBlock — same set / binding,
// same struct layout (SceneConstants is shared via SceneTypes.slang).
//
// Usage:
//   import PerFrame.resource;
//   ...
//   float3 camPos = gPerFrame.posW;
//
// Note: do NOT read camera position as `gPerFrame.invViewMat[3].xyz` in
// Slang/HLSL — `M[i]` returns row i, not column i (the GLSL convention),
// so for a standard math invView it resolves to (0,0,0,1). Use the
// dedicated posW field (or `invViewMat._m03_m13_m23` if a per-shot column
// extraction is unavoidable).

#include "../HostDefinitions.h"
import SceneTypes;

[vk::binding(0, 1)]
public ConstantBuffer<SceneConstants> gPerFrame;
