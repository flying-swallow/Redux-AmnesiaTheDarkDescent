#version 440
#extension GL_GOOGLE_include_directive : require
/// Copyright © 2009-2020 Frictional Games
/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Port of AmnesiaTheDarkDescent/HPL2/resource/decal.frag.fsl to the bindless
// renderer. Hardware blend factors are picked per pipeline on the C++ side;
// the shader just samples the diffuse texture, modulates by vertex color, and
// alpha-rejects very-transparent fragments (matches the legacy AlphaLimit of
// 0.01 set in cRendererDeferred::RenderDecals).
#include "bindless.resource.glsl"
#include "per_frame.resource.glsl"

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 2) flat in uint v_objectId;

layout(location = 0) out vec4 out_color;

void main() {
    UniformObject   object = sceneObjects[v_objectId];
    DiffuseMaterial mat    = opaqueMaterial[MATERIAL_ID(object)];

    vec4 diffuseColor = vec4(1.0);
    uint diffuseTex = DiffuseMaterial_DiffuseTexture_ID(mat);
    if (diffuseTex != INVALID_TEXTURE_INDEX) {
        diffuseColor = texture(sampler2D(textures_2d[nonuniformEXT(diffuseTex)],
                                         materialSampler),
                               v_uv);
    }

    vec4 finalColor = diffuseColor * v_color;
    if (finalColor.a < 0.01) {
        discard;
    }

    out_color = finalColor;
}
