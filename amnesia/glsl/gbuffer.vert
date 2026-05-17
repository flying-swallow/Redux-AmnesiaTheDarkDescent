#version 440
#extension GL_GOOGLE_include_directive                  : require
#extension GL_EXT_scalar_block_layout                   : require
#extension GL_EXT_buffer_reference                      : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
/// Copyright © 2009-2020 Frictional Games
/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
#include "bindless.resource.glsl"
#include "gbuffer.resource.glsl"

// Local buffer_reference types — kept out of the bindless SSBO declarations
// (which hold raw uint64 device addresses) so spirv-reflect can walk those
// descriptor blocks without following a pointer.
layout(buffer_reference, scalar) readonly buffer PositionBuf { vec4 v[]; };
layout(buffer_reference, scalar) readonly buffer TangentBuf  { vec4 v[]; };
layout(buffer_reference, scalar) readonly buffer NormalBuf   { vec3 v[]; };
layout(buffer_reference, scalar) readonly buffer Uv0Buf      { vec3 v[]; };
layout(buffer_reference, scalar) readonly buffer IndexBuf    { uint v[]; };

layout(location = 0) out vec3 v_pos;
layout(location = 1) out vec2 v_uv;
layout(location = 2) out vec3 v_normal;
layout(location = 3) out vec3 v_tangent;
layout(location = 4) out vec3 v_bitangent;
layout(location = 5) flat out uint v_objectId;

void main() {
    uint instanceId = uint(gl_InstanceIndex);

    uint64_t indexAddr = opaqueIndexHandles.data[instanceId];
    uint64_t posAddr   = opaquePositionHandles.data[instanceId];
    uint64_t uv0Addr   = opaqueUv0Handles.data[instanceId];
    uint64_t nrmAddr   = opaqueNormalHandles.data[instanceId];
    uint64_t tanAddr   = opaqueTangentHandles.data[instanceId];

    // Missing index buffer → fall back to non-indexed (each gl_VertexIndex is
    // its own vertex). Missing attribute streams → safe zero / identity values
    // so the rest of the transform doesn't NaN out.
    uint idx = (indexAddr != 0ul)
        ? IndexBuf(indexAddr).v[gl_VertexIndex]
        : gl_VertexIndex;

    vec3 a_position = (posAddr != 0ul)
        ? PositionBuf(posAddr).v[idx].xyz
        : vec3(0.0);
    vec2 a_texcoord = (uv0Addr != 0ul)
        ? Uv0Buf(uv0Addr).v[idx].xy
        : vec2(0.0);
    vec3 a_normal = (nrmAddr != 0ul)
        ? NormalBuf(nrmAddr).v[idx]
        : vec3(0.0, 0.0, 1.0);
    vec3 a_tangent = (tanAddr != 0ul)
        ? TangentBuf(tanAddr).v[idx].xyz
        : vec3(1.0, 0.0, 0.0);

    mat4 modelView    = viewMat * sceneObjects[instanceId].modelMat;
    mat4 modelViewPrj = projMat * modelView;

    v_pos = (modelView * vec4(a_position, 1.0)).xyz;
    v_uv  = (sceneObjects[instanceId].uvMat * vec4(a_texcoord, 0.0, 1.0)).xy;

    mat3 normalMat = ToNormalMat(sceneObjects[instanceId].invModelMat);
    v_normal    = normalize(normalMat * a_normal);
    v_tangent   = normalize(normalMat * a_tangent);
    v_bitangent = normalize(normalMat * cross(a_tangent, a_normal));
    v_objectId    = instanceId;

    gl_Position = modelViewPrj * vec4(a_position, 1.0);
}
