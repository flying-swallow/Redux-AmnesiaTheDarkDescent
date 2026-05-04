#ifndef FORWARD_RESOURCE_GLSL
#define FORWARD_RESOURCE_GLSL

#extension GL_EXT_nonuniform_qualifier : require

#include "forward_shader_common.glsl"

layout(set = 0, binding = 0) uniform sampler nearestClampSampler;

layout(set = 0, binding = 1) readonly buffer SceneObjectsBlock {
    UniformObject data[];
} sceneObjectsBuf;

layout(set = 0, binding = 2) readonly buffer OpaqueMaterialBlock {
    DiffuseMaterial data[];
} opaqueMaterialBuf;

layout(set = 1, binding = 3) readonly buffer LightClustersCountBlock {
    uint data[];
} lightClustersCountBuf;

layout(set = 1, binding = 4) readonly buffer LightClustersBlock {
    uint data[];
} lightClustersBuf;

layout(set = 1, binding = 5) readonly buffer PointLightsBlock {
    PointLight data[];
} pointLightsBuf;

layout(set = 2, binding = 13) uniform sampler materialSampler;

layout(set = 0, binding = 10) uniform texture2D sceneTextures[SCENE_MAX_TEXTURE_COUNT];

#define sceneObjects     sceneObjectsBuf.data
#define opaqueMaterial   opaqueMaterialBuf.data
#define lightClustersCount lightClustersCountBuf.data
#define lightClusters    lightClustersBuf.data
#define pointLights      pointLightsBuf.data

mat3 ToNormalMat(mat4 invModel, mat4 invView) {
    return transpose(mat3(invModel) * mat3(invView));
}

bool fetchSceneTextureFloat4(uint index, vec2 uv, inout vec4 value) {
    if (index < uint(SCENE_MAX_TEXTURE_COUNT)) {
        value = texture(sampler2D(sceneTextures[nonuniformEXT(index)], materialSampler), uv);
    }
    return index != INVALID_TEXTURE_INDEX;
}

bool fetchSceneTextureFloat3(uint index, vec2 uv, inout vec3 value) {
    if (index < uint(SCENE_MAX_TEXTURE_COUNT)) {
        value = texture(sampler2D(sceneTextures[nonuniformEXT(index)], materialSampler), uv).xyz;
    }
    return index != INVALID_TEXTURE_INDEX;
}

bool fetchSceneTextureFloat2(uint index, vec2 uv, inout vec2 value) {
    if (index < uint(SCENE_MAX_TEXTURE_COUNT)) {
        value = texture(sampler2D(sceneTextures[nonuniformEXT(index)], materialSampler), uv).xy;
    }
    return index != INVALID_TEXTURE_INDEX;
}

#endif // FORWARD_RESOURCE_GLSL
