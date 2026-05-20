#ifndef BINDLESS_RESOURCE_GLSL
#define BINDLESS_RESOURCE_GLSL

#extension GL_EXT_nonuniform_qualifier                  : require
#extension GL_EXT_buffer_reference                      : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_scalar_block_layout                   : require

#include "forward_shared.h"

// Engine-wide bindless texture set. Populated by cHybridRenderer's LRU
// texture slot allocator (m_textureBindless); any shader that includes this
// file gets the same slot ids. textures_2d is a sampled-image array
// (HPLTexture binds as VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE); combine with
// `materialSampler` at use sites via `sampler2D(textures_2d[i], materialSampler)`.
layout (set = 0, binding = BINDING_TEXTURES_2D)       uniform texture2D       textures_2d[];

// Cube texture bindless table — populated by cHybridRenderer::resolveCubeTextureSlot.
// Currently only fed by point-light gobo cubes; declare with the same partial-
// bind flags so unused slots stay valid descriptors.
layout (set = 0, binding = BINDING_TEXTURES_CUBE)     uniform textureCube     textures_cube[];

// Per-slot VkDeviceAddress arrays for each opaque vertex stream — raw uint64
// device addresses indexed by BindlessPool slot id. Dereferencing shaders cast
// these to a local `buffer_reference` type (kept out of the descriptor block
// so spirv-reflect can walk the block without diving through a pointer).
layout(set = 0, binding = BINDING_OPAQUE_POSITION_HANDLES) readonly buffer OpaquePositionHandlesBlock {
    uint64_t data[];
} opaquePositionHandles;

layout(set = 0, binding = BINDING_OPAQUE_TANGENT_HANDLES) readonly buffer OpaqueTangentHandlesBlock {
    uint64_t data[];
} opaqueTangentHandles;

layout(set = 0, binding = BINDING_OPAQUE_NORMAL_HANDLES) readonly buffer OpaqueNormalHandlesBlock {
    uint64_t data[];
} opaqueNormalHandles;

layout(set = 0, binding = BINDING_OPAQUE_UV0_HANDLES) readonly buffer OpaqueUv0HandlesBlock {
    uint64_t data[];
} opaqueUv0Handles;

layout(set = 0, binding = BINDING_OPAQUE_COLOR_HANDLES) readonly buffer OpaqueColorHandlesBlock {
    uint64_t data[];
} opaqueColorHandles;

layout(set = 0, binding = BINDING_OPAQUE_INDEX_HANDLES) readonly buffer OpaqueIndexHandlesBlock {
    uint64_t data[];
} opaqueIndexHandles;

// Shared sampler combined with each textures_2d entry at use time. Lives at
// binding 9 to leave room above the BDA buffer block (bindings 3-8).
layout(set = 0, binding = BINDING_MATERIAL_SAMPLER) uniform sampler materialSampler;

// Surfel GI buffers — mirror cHybridRenderer's m_surfel*Buffer members
// (HybridRenderer.h:63-69). Types come from forward_shared.h; scalar layout
// packs without alignment padding, matching the natural C++ layout of the
// Surfel / SurfelRecycleInfo / etc. structs declared there.
layout(set = 0, binding = BINDING_SURFEL_COUNTER, scalar) buffer _SurfelCounter   { SurfelCounter     surfelCounter;       };
layout(set = 0, binding = BINDING_SURFEL_BUFFER,  scalar) buffer _SurfelBuffer    { Surfel            surfelBuffer[];      };
layout(set = 0, binding = BINDING_SURFEL_ALIVE,   scalar) buffer _SurfelAlive     { uint              surfelAlive[];       };
layout(set = 0, binding = BINDING_SURFEL_DEAD,    scalar) buffer _SurfelDead      { uint              surfelDead[];        };
layout(set = 0, binding = BINDING_SURFEL_DIRTY,   scalar) buffer _SurfelDirty     { uint              surfelDirty[];       };
layout(set = 0, binding = BINDING_SURFEL_RECYCLE, scalar) buffer _SurfelRecycle   { SurfelRecycleInfo surfelRecycleInfo[]; };
layout(set = 0, binding = BINDING_SURFEL_RAY,     scalar) buffer _SurfelRayBuffer { SurfelRay         surfelRayBuffer[];   };

// Cell-grid SSBOs — static infrastructure shared by all surfel passes.
// Sizes come from forward_shared.h (TOTAL_CELL_COUNT, CELL_TO_SURFEL_CAPACITY).
layout(set = 0, binding = BINDING_CELL_BUFFER,     scalar) buffer _CellBuffer      { CellInfo          cellBuffer[];        };
layout(set = 0, binding = BINDING_CELL_COUNTER,    scalar) buffer _CellCounter     { CellCounter       cellCounter;         };
layout(set = 0, binding = BINDING_CELL_TO_SURFEL,  scalar) buffer _CellToSurfel    { uint              cellToSurfel[];      };

// Scene-object / opaque-material tables — slot-allocated alongside the other
// bindless pools (m_diffuseObjectBuffer, m_opaqueMaterialBuffer in
// cHybridRenderer). Capacities: OBJECT_SLOT_CAPACITY / MATERIAL_SLOT_CAPACITY.
layout(set = 0, binding = BINDING_SCENE_OBJECTS) readonly buffer SceneObjectsBlock {
    UniformObject data[];
} sceneObjectsBuf;

layout(set = 0, binding = BINDING_OPAQUE_MATERIAL) readonly buffer OpaqueMaterialBlock {
    DiffuseMaterial data[];
} opaqueMaterialBuf;

#define sceneObjects   sceneObjectsBuf.data
#define opaqueMaterial opaqueMaterialBuf.data

// Per-frame point-light SSBO — written by cHybridRenderer::Draw() each frame
// via RI.uploader. Count is in PerFrameConstants::pointLightCount (set 1).
layout(set = 0, binding = BINDING_POINT_LIGHTS, scalar) readonly buffer PointLightBlock {
    PointLight data[];
} pointLightBuf;

#define pointLights pointLightBuf.data

// Per-frame spot-light SSBO. Same upload cadence as pointLights;
// PerFrameConstants::spotLightCount holds the active count.
layout(set = 0, binding = BINDING_SPOT_LIGHTS, scalar) readonly buffer SpotLightBlock {
    SpotLight data[];
} spotLightBuf;

#define spotLights spotLightBuf.data

// Per-frame box-light SSBO — Add-blend volumetric tints. Same upload cadence
// as point/spot lights; PerFrameConstants::boxLightCount holds the count.
layout(set = 0, binding = BINDING_BOX_LIGHTS, scalar) readonly buffer BoxLightBlock {
    BoxLight data[];
} boxLightBuf;

#define boxLights boxLightBuf.data

#endif // BINDLESS_RESOURCE_GLSL
