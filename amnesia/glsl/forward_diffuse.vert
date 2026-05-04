#version 440
/// Copyright © 2009-2020 Frictional Games
/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
#include "forward_resource.glsl"

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec2 a_texcoord;
layout(location = 2) in vec3 a_normal;
layout(location = 3) in vec3 a_tangent;

layout(location = 0) out vec3 v_pos;
layout(location = 1) out vec2 v_uv;
layout(location = 2) out vec3 v_normal;
layout(location = 3) out vec3 v_tangent;
layout(location = 4) out vec3 v_bitangent;
layout(location = 5) flat out uint v_drawId;

void main() {
    uint instanceId = uint(gl_InstanceIndex);

    mat4 modelView    = viewMat * sceneObjects[instanceId].modelMat;
    mat4 modelViewPrj = projMat * modelView;

    v_pos = (modelView * vec4(a_position, 1.0)).xyz;
    v_uv  = (sceneObjects[instanceId].uvMat * vec4(a_texcoord, 0.0, 1.0)).xy;

    mat3 normalMat = ToNormalMat(sceneObjects[instanceId].invModelMat, invViewMat);
    v_normal    = normalize(normalMat * a_normal);
    v_tangent   = normalize(normalMat * a_tangent);
    v_bitangent = normalize(normalMat * cross(a_tangent, a_normal));
    v_drawId    = instanceId;

    gl_Position = modelViewPrj * vec4(a_position, 1.0);
}
