#version 440
#extension GL_GOOGLE_include_directive : require
/// Copyright © 2009-2020 Frictional Games
/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Port of AmnesiaTheDarkDescent/HPL2/resource/translucency_particle.frag.fsl
// to the bindless renderer. Hardware blend state is selected on the C++ side
// (one pipeline per blend mode); this shader applies the matching per-pixel
// color-space transform the legacy shader did so the output going into the
// blender is in the right space (e.g. mul/mulx2 pre-bias before SRC*DST blend).
#include "bindless.resource.glsl"
#include "per_frame.resource.glsl"

#define BLEND_MODE_ADD          0u
#define BLEND_MODE_MUL          1u
#define BLEND_MODE_MULX2        2u
#define BLEND_MODE_ALPHA        3u
#define BLEND_MODE_PREMUL_ALPHA 4u

layout(push_constant) uniform ParticlePushConstants {
    uint  blendMode;
    float sceneAlpha;
} pc;

layout(location = 0) in vec3 v_viewPos;
layout(location = 1) in vec2 v_uv;
layout(location = 2) in vec4 v_color;
layout(location = 3) flat in uint v_objectId;

layout(location = 0) out vec4 out_color;

void main() {
    UniformObject   object = sceneObjects[v_objectId];
    DiffuseMaterial mat    = opaqueMaterial[MATERIAL_ID(object)];

    vec4 diffuseColor = vec4(0.0, 0.0, 0.0, 1.0);
    uint diffuseTex = DiffuseMaterial_DiffuseTexture_ID(mat);
    if (diffuseTex != INVALID_TEXTURE_INDEX) {
        diffuseColor = texture(sampler2D(textures_2d[nonuniformEXT(diffuseTex)],
                                         materialSampler),
                               v_uv);
    }
    diffuseColor *= v_color;

    float finalAlpha = pc.sceneAlpha;
    if (worldFogLength > 0.0) {
        float fogAmount = pow(
            clamp((-v_viewPos.z - worldFogStart) / worldFogLength, 0.0, 1.0),
            fogFalloffExp);
        finalAlpha = (oneMinusFogAlpha * fogAmount + (1.0 - fogAmount)) * pc.sceneAlpha;
    }

    vec4 finalColor = diffuseColor;
    switch (pc.blendMode) {
        case BLEND_MODE_ADD: {
            finalColor.xyz *= finalAlpha * object.lightLevel;
            break;
        }
        case BLEND_MODE_MUL: {
            finalColor.xyz += (vec3(1.0) - finalColor.xyz) * (1.0 - finalAlpha);
            break;
        }
        case BLEND_MODE_MULX2: {
            float blendMulAlpha = object.lightLevel * finalAlpha;
            finalColor.xyz = finalColor.xyz * blendMulAlpha + vec3(0.5) * (1.0 - blendMulAlpha);
            break;
        }
        case BLEND_MODE_ALPHA: {
            finalColor.xyz *= object.lightLevel;
            finalColor.a   *= finalAlpha;
            break;
        }
        case BLEND_MODE_PREMUL_ALPHA: {
            finalColor      *= finalAlpha;
            finalColor.xyz  *= object.lightLevel;
            break;
        }
    }

    if (finalColor.a < 0.01) {
        discard;
    }

    out_color = finalColor;
}
