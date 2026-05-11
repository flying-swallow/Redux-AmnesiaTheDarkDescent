#version 440
#extension GL_GOOGLE_include_directive : require
/// Copyright © 2009-2020 Frictional Games
/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
#include "forward_resource.glsl"

layout(location = 0) in vec3 v_pos;
layout(location = 1) in vec2 v_uv;
layout(location = 2) in vec3 v_normal;
layout(location = 3) in vec3 v_tangent;
layout(location = 4) in vec3 v_bitangent;
layout(location = 5) flat in uint v_drawId;

layout(location = 0) out uint out_visibility;

void main() {
    UniformObject   object     = sceneObjects[v_drawId];
    DiffuseMaterial diffuseMat = opaqueMaterial[MATERIAL_ID(object)];

    vec4 diffuseColor = vec4(0.0, 0.0, 0.0, 1.0);
    fetchSceneTextureFloat4(DiffuseMaterial_DiffuseTexture_ID(diffuseMat), v_uv, diffuseColor);
    if (diffuseColor.w < ALPHA_REJECT) {
        discard;
    }

    //vec2 specular = vec2(0.0);
    //fetchSceneTextureFloat2(DiffuseMaterial_SpecularTexture_ID(diffuseMat), v_uv, specular);

    //vec3 normal = vec3(0.0);
    //vec3 normalSample = vec3(0.0);
    //bool hasNormal = fetchSceneTextureFloat3(DiffuseMaterial_NormalTexture_ID(diffuseMat), v_uv, normalSample);
    //if (hasNormal) {
    //    normalSample -= 0.5;
    //    normal = normalize(normalSample.x * v_tangent +
    //                       normalSample.y * v_bitangent +
    //                       normalSample.z * v_normal);
    //} else {
    //    normal = normalize(v_normal);
    //}

    //vec3  normalizedNormal = normalize(v_normal);
    //vec2  texelPos         = gl_FragCoord.xy * viewTexel;
    //uvec2 clusterCoords    = uvec2(floor(texelPos * vec2(LIGHT_CLUSTER_WIDTH, LIGHT_CLUSTER_HEIGHT)));
    //uint  numLightsInCluster = lightClustersCount[LIGHT_CLUSTER_COUNT_POS(clusterCoords.x, clusterCoords.y)];

    //vec4 result = vec4(0.0);
    //for (uint j = 0u; j < numLightsInCluster; ++j) {
    //    uint       lightId = lightClusters[LIGHT_CLUSTER_DATA_POS(j, clusterCoords.x, clusterCoords.y)];
    //    PointLight pl      = pointLights[lightId];

    //    vec4 lightPosWorldSpace = vec4(pl.lightPos.xyz, 1.0);
    //    vec4 lightCameraSpace   = viewMat * lightPosWorldSpace;

    //    const vec3  lightDir       = (lightCameraSpace.xyz - v_pos) * (1.0 / pl.lightRadius);
    //    const vec3  normalLightDir = normalize(lightDir);
    //    const float attenuation    = clamp(1.0 - dot(lightDir, lightDir), 0.0, 1.0);
    //    const float fLDotN         = max(dot(normalizedNormal, normalLightDir), 0.0);

    //    float specularValue = 0.0;
    //    if (pl.lightColor.w > 0.0) {
    //        vec3  halfVec       = normalize(normalLightDir + normalize(-v_pos));
    //        float specIntensity = specular.x;
    //        float specPower     = exp2(specular.y * 10.0) + 1.0;
    //        specularValue = pl.lightColor.w * specIntensity *
    //                        pow(clamp(dot(halfVec, normalizedNormal), 0.0, 1.0), specPower);
    //    }

    //    result += vec4(((vec3(specularValue) * pl.lightColor.xyz) +
    //                    (diffuseColor.xyz * pl.lightColor.xyz * fLDotN)) * attenuation,
    //                   0.0);
    //}

    //if (object.illuminationAmount > 0.0) {
    //    vec4 illuminationColor = vec4(0.0);
    //    bool hasIllumination = fetchSceneTextureFloat4(
    //        DiffuseMaterial_IlluminiationTexture_ID(diffuseMat), v_uv, illuminationColor);
    //    if (hasIllumination) {
    //        result += object.illuminationAmount * illuminationColor;
    //    }
    //}

    out_visibility = (v_drawId << 16) | (uint(gl_PrimitiveID) & 0xffffu);
}
