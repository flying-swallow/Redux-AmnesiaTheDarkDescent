#ifndef GBUFFER_RESOURCE_GLSL
#define GBUFFER_RESOURCE_GLSL

#include "per_frame.resource.glsl"     // UniformObject, DiffuseMaterial, MATERIAL_ID, ALPHA_REJECT, INVALID_TEXTURE_INDEX, perFrame UBO
#include "bindless.resource.glsl"      // textures_2d, materialSampler, GL_EXT_nonuniform_qualifier

// sceneObjects[] and opaqueMaterial[] now live in bindless.resource.glsl
// (set=0, bindings 20..21).

mat3 ToNormalMat(mat4 invModel) {
    return transpose(mat3(invModel));
}

bool fetchSceneTextureFloat4(uint index, vec2 uv, inout vec4 value) {
    if (index != INVALID_TEXTURE_INDEX) {
        value = texture(sampler2D(textures_2d[nonuniformEXT(index)], materialSampler), uv);
        return true;
    }
    return false;
}

#endif // GBUFFER_RESOURCE_GLSL
