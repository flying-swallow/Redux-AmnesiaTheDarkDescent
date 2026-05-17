#ifndef LIGHT_CULL_RESOURCES_GLSL
#define LIGHT_CULL_RESOURCES_GLSL

// Convenience header for the cluster-build compute shaders. All cluster
// SSBOs now live in the bindless set (set 0, BINDING_LIGHT_CLUSTERS_*).
// Per-frame UBO at set 1 binding 0 supplies viewMat/invProjMat/zNear/zFar.
//
// bindless.resource.glsl must come before per_frame.resource.glsl: the
// former pulls in host_device.h (which declares a `float
// fireflyClampThreshold` struct field), while the latter installs a
// `#define fireflyClampThreshold perFrame.u.fireflyClampThreshold` macro
// that would otherwise corrupt that field declaration.
#include "bindless.resource.glsl"
#include "per_frame.resource.glsl"

#endif // LIGHT_CULL_RESOURCES_GLSL
