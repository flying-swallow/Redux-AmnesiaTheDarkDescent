#ifndef FORWARD_RESOURCE_GLSL
#define FORWARD_RESOURCE_GLSL

#include "forward_shader_common.glsl"
#include "bindless.resource.glsl"

// Per-frame globals (set 1): perFrame UBO is declared in forward_shader_common.glsl.
layout(set = 1, binding = 3) readonly buffer LightClustersCountBlock {
    uint data[];
} lightClustersCountBuf;

layout(set = 1, binding = 4) readonly buffer LightClustersBlock {
    uint data[];
} lightClustersBuf;

layout(set = 1, binding = 5) readonly buffer PointLightsBlock {
    PointLight data[];
} pointLightsBuf;

// Per-renderer (set 2): scene objects + opaque material table.
layout(set = 2, binding = 0) readonly buffer SceneObjectsBlock {
    UniformObject data[];
} sceneObjectsBuf;

layout(set = 2, binding = 1) readonly buffer OpaqueMaterialBlock {
    DiffuseMaterial data[];
} opaqueMaterialBuf;

#define sceneObjects       sceneObjectsBuf.data
#define opaqueMaterial     opaqueMaterialBuf.data
#define lightClustersCount lightClustersCountBuf.data
#define lightClusters      lightClustersBuf.data
#define pointLights        pointLightsBuf.data

mat3 ToNormalMat(mat4 invModel, mat4 invView) {
    return transpose(mat3(invModel) * mat3(invView));
}

bool fetchSceneTextureFloat4(uint index, vec2 uv, inout vec4 value) {
    if (index != INVALID_TEXTURE_INDEX) {
        value = texture(textures_2d[nonuniformEXT(index)], uv);
        return true;
    }
    return false;
}

bool fetchSceneTextureFloat3(uint index, vec2 uv, inout vec3 value) {
    if (index != INVALID_TEXTURE_INDEX) {
        value = texture(textures_2d[nonuniformEXT(index)], uv).xyz;
        return true;
    }
    return false;
}

bool fetchSceneTextureFloat2(uint index, vec2 uv, inout vec2 value) {
    if (index != INVALID_TEXTURE_INDEX) {
        value = texture(textures_2d[nonuniformEXT(index)], uv).xy;
        return true;
    }
    return false;
}

#endif // FORWARD_RESOURCE_GLSL
