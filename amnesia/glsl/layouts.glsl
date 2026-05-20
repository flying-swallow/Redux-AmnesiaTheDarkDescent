#ifndef LAYOUTS_GLSL
#define LAYOUTS_GLSL 1

// Surfel-raytrace ray-tracing descriptor layout.
//
// History: this file used to host the NVIDIA RTX-sample pathtracer's full
// scene model (TLAS at set 0, GLTF scene at set 2, sun-sky/HDR env at set 3).
// That model collided with bindless.resource.glsl (set 0) and pulled in a
// scene representation the bindless renderer does not maintain. The path-
// tracer chain (pathtrace.glsl, traceray_rq.glsl, shaderUtils_surfel_cell.glsl
// guarded by LAYOUTS_GLSL) was rewritten to read bindless types directly, so
// this header is now just the TLAS hookup.

#extension GL_EXT_ray_tracing : require
#extension GL_EXT_ray_query   : require

#include "forward_shared.h"

// Pushed per-dispatch via RIProgram::bindDescriptors as "topLevelAS".
// Set 2 leaves set 0 (bindless) and set 1 (perFrame) intact and matches the
// cadence of surfel_generate, which also uses set 2 for its pushed samplers.
layout(set = 2, binding = 0) uniform accelerationStructureEXT topLevelAS;

#endif  // LAYOUTS_GLSL
