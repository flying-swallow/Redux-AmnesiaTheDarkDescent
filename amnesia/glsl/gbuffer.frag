#version 440
#extension GL_GOOGLE_include_directive : require
/// Copyright © 2009-2020 Frictional Games
/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
#include "bindless.resource.glsl"
#include "gbuffer.resource.glsl"
#include "common.glsl"

layout(location = 0) in vec3 v_pos;
layout(location = 1) in vec2 v_uv;
layout(location = 2) in vec3 v_normal;
layout(location = 3) in vec3 v_tangent;
layout(location = 4) in vec3 v_bitangent;
layout(location = 5) flat in uint v_objectId;

layout(location = 0) out vec4 out_normal;
layout(location = 1) out uint out_visibility;

void main() {
    UniformObject   object     = sceneObjects[v_objectId];
    DiffuseMaterial diffuseMat = opaqueMaterial[MATERIAL_ID(object)];

    vec4 diffuseColor = vec4(0.0, 0.0, 0.0, 1.0);
    fetchSceneTextureFloat4(DiffuseMaterial_DiffuseTexture_ID(diffuseMat), v_uv, diffuseColor);
    if (diffuseColor.w < ALPHA_REJECT) {
        discard;
    }

    out_normal     = vec4(v_normal, 1.0);
    out_visibility = pack_visibility(v_objectId, uint(gl_PrimitiveID));
}
