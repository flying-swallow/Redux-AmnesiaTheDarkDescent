#ifndef BINDLESS_RESOURCE_GLSL
#define BINDLESS_RESOURCE_GLSL

#extension GL_EXT_nonuniform_qualifier                  : require
#extension GL_EXT_buffer_reference                      : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_scalar_block_layout                   : require

#include "host_device.h"

// Engine-wide bindless texture set. Populated by cHybridRenderer's LRU
// texture slot allocator (m_textureBindless); any shader that includes this
// file gets the same slot ids. textures_2d is a sampled-image array
// (HPLTexture binds as VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE); combine with
// `materialSampler` at use sites via `sampler2D(textures_2d[i], materialSampler)`.
layout (set = 0, binding = 0) uniform texture2D textures_2d[];
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

// Shared sampler combined with each textures_2d entry at use time. Lives at
// binding 9 to leave room above the BDA buffer block (bindings 3-8).
layout(set = 0, binding = 9) uniform sampler materialSampler;

// Surfel GI buffers — mirror cHybridRenderer's m_surfel*Buffer members
// (HybridRenderer.h:63-69). Types come from host_device.h; scalar layout
// matches the C++ side in HPL2/core/include/graphics/HostDevice.h. Each
// surfel_*.comp shader will eventually include this file in place of its
// current inlined set=0 block.
layout(set = 0, binding = 10, scalar) buffer _SurfelCounter   { SurfelCounter     surfelCounter;       };
layout(set = 0, binding = 11, scalar) buffer _SurfelBuffer    { Surfel            surfelBuffer[];      };
layout(set = 0, binding = 12, scalar) buffer _SurfelAlive     { uint              surfelAlive[];       };
layout(set = 0, binding = 13, scalar) buffer _SurfelDead      { uint              surfelDead[];        };
layout(set = 0, binding = 14, scalar) buffer _SurfelDirty     { uint              surfelDirty[];       };
layout(set = 0, binding = 15, scalar) buffer _SurfelRecycle   { SurfelRecycleInfo surfelRecycleInfo[]; };
layout(set = 0, binding = 16, scalar) buffer _SurfelRayBuffer { SurfelRay         surfelRayBuffer[];   };

#endif // BINDLESS_RESOURCE_GLSL
