#version 440
#extension GL_GOOGLE_include_directive                  : require
#extension GL_EXT_scalar_block_layout                   : require
#extension GL_EXT_buffer_reference                      : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
/// Copyright © 2009-2020 Frictional Games
/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Port of AmnesiaTheDarkDescent/HPL2/resource/translucency_particle.vert.fsl
// to the bindless renderer. Particles share the OBJECT_SLOT bindless pool with
// solid geometry: cHybridRenderer writes the particle vertex buffer's BDA into
// the same opaque*Handles[] arrays, and gl_InstanceIndex selects the slot.
//
// Per-vertex stream strides come from iParticleEmitter::iParticleEmitter
// (ParticleEmitter.cpp:111-113):
//   position : Float x 4  (stride 16, only .xyz read)
//   color    : Float x 4  (stride 16, full rgba)
//   texture0 : Float x 3  (stride 12, only .xy read)
// Using vec3 for position / vec2 for uv0 in the buffer_reference here would
// make the GPU step by 12 / 8 bytes per vertex instead of 16 / 12 and read
// garbage offsets — manifests as billboards anchored to the camera instead of
// the world (interpolated positions become noise-tracking-camera).
#include "bindless.resource.glsl"
#include "per_frame.resource.glsl"

layout(buffer_reference, scalar) readonly buffer ParticlePositionBuf { vec4 v[]; };
layout(buffer_reference, scalar) readonly buffer ParticleUv0Buf      { vec3 v[]; };
layout(buffer_reference, scalar) readonly buffer ParticleColorBuf    { vec4 v[]; };
layout(buffer_reference, scalar) readonly buffer ParticleIndexBuf    { uint v[]; };

layout(location = 0) out vec3 v_viewPos;   // view-space position (for fog)
layout(location = 1) out vec2 v_uv;
layout(location = 2) out vec4 v_color;
layout(location = 3) flat out uint v_objectId;

void main() {
    uint instanceId = uint(gl_InstanceIndex);

    uint64_t indexAddr = opaqueIndexHandles.data[instanceId];
    uint64_t posAddr   = opaquePositionHandles.data[instanceId];
    uint64_t uv0Addr   = opaqueUv0Handles.data[instanceId];
    uint64_t colAddr   = opaqueColorHandles.data[instanceId];

    uint idx = (indexAddr != 0ul)
        ? ParticleIndexBuf(indexAddr).v[gl_VertexIndex]
        : gl_VertexIndex;

    vec3 a_position = (posAddr != 0ul)
        ? ParticlePositionBuf(posAddr).v[idx].xyz
        : vec3(0.0);
    vec2 a_texcoord = (uv0Addr != 0ul)
        ? ParticleUv0Buf(uv0Addr).v[idx].xy
        : vec2(0.0);
    vec4 a_color = (colAddr != 0ul)
        ? ParticleColorBuf(colAddr).v[idx]
        : vec4(1.0);

    mat4 modelView    = viewMat * sceneObjects[instanceId].modelMat;
    mat4 modelViewPrj = projMat * modelView;

    v_viewPos = (modelView * vec4(a_position, 1.0)).xyz;
    v_uv      = (sceneObjects[instanceId].uvMat * vec4(a_texcoord, 0.0, 1.0)).xy;
    v_color   = a_color;
    v_objectId = instanceId;

    gl_Position = modelViewPrj * vec4(a_position, 1.0);
}
