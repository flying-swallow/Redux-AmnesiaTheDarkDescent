#version 440
#extension GL_GOOGLE_include_directive                  : require
#extension GL_EXT_scalar_block_layout                   : require
#extension GL_EXT_buffer_reference                      : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
/// Copyright © 2009-2020 Frictional Games
/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Port of AmnesiaTheDarkDescent/HPL2/resource/decal.vert.fsl to the bindless
// renderer. Decals share the OBJECT_SLOT bindless pool with solid geometry:
// cHybridRenderer writes the decal vertex buffer's BDA into the same
// opaque*Handles[] arrays, and gl_InstanceIndex selects the slot.
//
// Decal vertex layout matches the legacy DecalCreator output:
//   position : Float x 3 -> uses vec4 stride 16 (matches particle layout)
//   texture0 : Float x 2 -> uses vec3 stride 12 (matches particle layout)
//   color0   : Float x 4 stride 16
// We reuse the particle buffer-reference types (vec4 position, vec3 uv0)
// because cHybridRenderer fans out the same opaque*Handles array, and the
// legacy DecalCreator emits the same per-stream widths.
#include "bindless.resource.glsl"
#include "per_frame.resource.glsl"

layout(buffer_reference, scalar) readonly buffer DecalPositionBuf { vec4 v[]; };
layout(buffer_reference, scalar) readonly buffer DecalUv0Buf      { vec3 v[]; };
layout(buffer_reference, scalar) readonly buffer DecalColorBuf    { vec4 v[]; };
layout(buffer_reference, scalar) readonly buffer DecalIndexBuf    { uint v[]; };

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
layout(location = 2) flat out uint v_objectId;

void main() {
    uint instanceId = uint(gl_InstanceIndex);

    uint64_t indexAddr = opaqueIndexHandles.data[instanceId];
    uint64_t posAddr   = opaquePositionHandles.data[instanceId];
    uint64_t uv0Addr   = opaqueUv0Handles.data[instanceId];
    uint64_t colAddr   = opaqueColorHandles.data[instanceId];

    uint idx = (indexAddr != 0ul)
        ? DecalIndexBuf(indexAddr).v[gl_VertexIndex]
        : gl_VertexIndex;

    vec3 a_position = (posAddr != 0ul)
        ? DecalPositionBuf(posAddr).v[idx].xyz
        : vec3(0.0);
    vec2 a_texcoord = (uv0Addr != 0ul)
        ? DecalUv0Buf(uv0Addr).v[idx].xy
        : vec2(0.0);
    vec4 a_color = (colAddr != 0ul)
        ? DecalColorBuf(colAddr).v[idx]
        : vec4(1.0);

    mat4 modelView    = viewMat * sceneObjects[instanceId].modelMat;
    mat4 modelViewPrj = projMat * modelView;

    v_uv       = (sceneObjects[instanceId].uvMat * vec4(a_texcoord, 0.0, 1.0)).xy;
    v_color    = a_color;
    v_objectId = instanceId;

    gl_Position = modelViewPrj * vec4(a_position, 1.0);
}
