#ifndef BINDLESS_RESOURCE_GLSL
#define BINDLESS_RESOURCE_GLSL

#extension GL_EXT_nonuniform_qualifier                  : require
#extension GL_EXT_buffer_reference                      : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

// Engine-wide bindless texture set. Populated by the RIBootstrap registry;
// any shader that includes this file gets the same slot ids.
layout (set = 0, binding = 0) uniform sampler2D textures_2d[];
layout (set = 0, binding = 1) uniform samplerCube textures_cube[];
layout (set = 0, binding = 2) uniform sampler2DArray textures_2d_array[];

// Per-slot VkDeviceAddress arrays for each opaque vertex stream — raw uint64
// device addresses indexed by BindlessPool slot id. Dereferencing shaders cast
// these to a local `buffer_reference` type (kept out of the descriptor block
// so spirv-reflect can walk the block without diving through a pointer).
layout(set = 0, binding = 3) readonly buffer OpaquePositionHandlesBlock {
    uint64_t data[];
} opaquePositionHandles;

layout(set = 0, binding = 4) readonly buffer OpaqueTangentHandlesBlock {
    uint64_t data[];
} opaqueTangentHandles;

layout(set = 0, binding = 5) readonly buffer OpaqueNormalHandlesBlock {
    uint64_t data[];
} opaqueNormalHandles;

layout(set = 0, binding = 6) readonly buffer OpaqueUv0HandlesBlock {
    uint64_t data[];
} opaqueUv0Handles;

layout(set = 0, binding = 7) readonly buffer OpaqueColorHandlesBlock {
    uint64_t data[];
} opaqueColorHandles;

layout(set = 0, binding = 8) readonly buffer OpaqueIndexHandlesBlock {
    uint64_t data[];
} opaqueIndexHandles;

#endif // BINDLESS_RESOURCE_GLSL
