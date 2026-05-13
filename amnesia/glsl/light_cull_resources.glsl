#ifndef LIGHT_CULL_RESOURCES_GLSL
#define LIGHT_CULL_RESOURCES_GLSL

#include "per_frame.resource.glsl"

layout(push_constant) uniform RootConstantsBlock {
    uvec2 windowSize;
} uRootConstants;

#define windowSize uRootConstants.windowSize

layout(set = 1, binding = 2) readonly buffer PointLightsCullBlock {
    PointLight data[];
} pointLightsBuf;

layout(set = 1, binding = 3) buffer LightClustersCountBlock {
    uint data[];
} lightClustersCountBuf;

layout(set = 1, binding = 4) buffer LightClustersBlock {
    uint data[];
} lightClustersBuf;

#define pointLights        pointLightsBuf.data
#define lightClustersCount lightClustersCountBuf.data
#define lightClusters      lightClustersBuf.data

#endif // LIGHT_CULL_RESOURCES_GLSL
