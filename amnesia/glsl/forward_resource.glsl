#ifndef FORWARD_RESOURCE_GLSL
#define FORWARD_RESOURCE_GLSL

#include "forward_shader_common.glsl"
#include "bindless.resource.glsl"

// Per-renderer (set 2): scene objects + opaque material table.
layout(set = 2, binding = 0) readonly buffer SceneObjectsBlock {
    UniformObject data[];
} sceneObjectsBuf;

layout(set = 2, binding = 1) readonly buffer OpaqueMaterialBlock {
    DiffuseMaterial data[];
} opaqueMaterialBuf;

#define sceneObjects   sceneObjectsBuf.data
#define opaqueMaterial opaqueMaterialBuf.data

mat3 ToNormalMat(mat4 invModel, mat4 invView) {
    return transpose(mat3(invModel) * mat3(invView));
}

bool fetchSceneTextureFloat4(uint index, vec2 uv, inout vec4 value) {
    if (index != INVALID_TEXTURE_INDEX) {
        value = texture(sampler2D(textures_2d[nonuniformEXT(index)], materialSampler), uv);
        return true;
    }
    return false;
}

#endif // FORWARD_RESOURCE_GLSL
