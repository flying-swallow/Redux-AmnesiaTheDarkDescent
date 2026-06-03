/// Copyright © 2009-2020 Frictional Games
/// Copyright 2023 Michael Pollind
/// SPDX-License-Identifier: GPL-3.0
//
// Slang port of AmnesiaTheDarkDescent/HPL2/resource/translucency.frag.fsl —
// the non-particle (mesh) translucent path matching MaterialID::Translucent
// in RendererDeferred.cpp:4606-4671.
//
// Scope: diffuse × vertex color × tint, fog, per-blend-mode pre-bias,
// per-object affecting-light level, static cube-map Fresnel reflection with
// rim term, and the UseIlluminationTrans second-draw flow. Hardware blend
// state is selected on the C++ side (one pipeline per blend mode) and the
// shader applies the matching per-pixel color-space transform so the
// output going into the blender is in the right space.
//
// Aligned with the reference translucency.frag.fsl — the one deliberate
// deviation is refraction. The legacy bend-behind distortion (screen-color
// copy + per-fragment UV offset) is moved upstream: the RT V-buffer
// (SurfelVBuffer.rt.slang::closeHit) bends the primary ray through refractive
// materials and MainCompositePass.cs.slang shades the resulting hit, so by
// the time this shader runs the GI composite already shows the refracted
// background at every refractive pixel. The translucent surface just
// alpha-blends on top — no screen-color sample, no refraction block here.
// Reflection stays the material's static cube map, exactly as the reference.


#include "Constants.h"
import bindless;
import SceneTypes;
import PerFrame.resource;
import Fog;
import Scene;

#define BLEND_MODE_ADD          0u
#define BLEND_MODE_MUL          1u
#define BLEND_MODE_MULX2        2u
#define BLEND_MODE_ALPHA        3u
#define BLEND_MODE_PREMUL_ALPHA 4u

// Options bitfield mirroring TranslucencyFlags in RendererDeferred.cpp.
// blendMode stays as a separate uint so the existing switch is cheap; this
// field carries the *behaviour* flags the reference packs into the same
// root constant.
#define TRANS_OPT_USE_ILLUMINATION (1u << 0)

struct TranslucentPushConstants
{
    uint  blendMode;
    float sceneAlpha;
    uint  options;
    uint  _pad;
};

[vk::push_constant]
TranslucentPushConstants pc;

struct TranslucentVSOut
{
    float3 viewPos                 : VIEWPOS;
    float2 uv                      : TEXCRD;
    float3 normal                  : NORMAL;
    float3 tangent                 : TANGENT;
    float3 bitangent               : BITANGENT;
    float4 color                   : COLOR;
    nointerpolation uint objectId  : OBJECTID;
    float4 posH                    : SV_POSITION;
};

// Schlick-Fresnel — mirrors deferred_common.h.fsl::Fresnel(eDotN, bias, pow).
float computeFresnel(float afEDotN, float afFresnelBias, float afFresnelPow)
{
    float fFacing = 1.0f - afEDotN;
    return saturate(afFresnelBias + (1.0f - afFresnelBias) * pow(fFacing, afFresnelPow));
}

[shader("fragment")]
float4 psMain(TranslucentVSOut psIn) : SV_TARGET
{
    UniformObject       obj = gSceneObjects[psIn.objectId];
    TranslucentMaterial mat = gScene.getTranslucentMaterial(obj.materialID);

    // Reference mask: when the second cube-map-only draw runs, the diffuse /
    // illumination / etc. sample paths are switched off so only the
    // reflection contribution is added on top of the main draw.
    uint textureConfig = mat.materialConfig;
    if ((pc.options & TRANS_OPT_USE_ILLUMINATION) != 0u)
    {
        textureConfig &= (kMaterialFlagEnableNormal
                        | kMaterialFlagEnableCubeMap
                        | kMaterialFlagEnableCubeMapAlpha);
    }

    float4 diffuseColor = float4(0.0f, 0.0f, 0.0f, 1.0f);
    if ((textureConfig & kMaterialFlagEnableDiffuse) != 0u)
    {
        uint diffuseTex = mat.tex[0];
        if (diffuseTex != kInvalidTextureIndex)
        {
            diffuseColor = gTextures2D[NonUniformResourceIndex(diffuseTex)]
                               .Sample(gMaterialSampler, psIn.uv);
        }
        diffuseColor *= psIn.color;
    }

    // Per-pixel fog (world + per-area). The shared Fog helper returns a
    // visibility scalar — translucents multiply it into the per-pixel alpha
    // so glass/decals fade behind fog without painting the fog colour.
    float3 surfacePosW = mul(gPerFrame.invViewMat, float4(psIn.viewPos, 1.0f)).xyz;
    FogResult fog = computeFog(surfacePosW, gPerFrame.posW);
    float finalAlpha = pc.sceneAlpha * fog.visibility;

    float4 finalColor = diffuseColor;
    switch (pc.blendMode)
    {
        case BLEND_MODE_ADD:
        {
            finalColor.xyz *= finalAlpha * obj.lightLevel;
            break;
        }
        case BLEND_MODE_MUL:
        {
            finalColor.xyz += (float3(1.0f) - finalColor.xyz) * (1.0f - finalAlpha);
            break;
        }
        case BLEND_MODE_MULX2:
        {
            float blendMulAlpha = obj.lightLevel * finalAlpha;
            finalColor.xyz = finalColor.xyz * blendMulAlpha
                           + float3(0.5f) * (1.0f - blendMulAlpha);
            break;
        }
        case BLEND_MODE_ALPHA:
        {
            finalColor.xyz *= obj.lightLevel;
            finalColor.a   *= finalAlpha;
            break;
        }
        case BLEND_MODE_PREMUL_ALPHA:
        {
            finalColor      *= finalAlpha;
            finalColor.xyz  *= obj.lightLevel;
            break;
        }
    }

    // Fresnel reflection. Gated on the material carrying a reflection cube map
    // (the "reflective" signal — same gate the legacy used), but the
    // reflection *source* is now the live ray-traced reflection buffer
    // (gReflectionResolve) instead of the static cube sample. The Fresnel /
    // rim weighting is unchanged.
    bool hasCubeMap = (textureConfig & kMaterialFlagEnableCubeMap) != 0u
                      && mat.cubeMapTextureIndex != kInvalidTextureIndex;
    if (hasCubeMap)
    {
        // Reconstruct view-space surface normal. If a normal map is bound,
        // sample it and rotate into view space via the tangent frame the VS
        // built; otherwise use the interpolated normal directly.
        float3 screenNormal;
        if ((textureConfig & kMaterialFlagEnableNormal) != 0u)
        {
            uint normalTex = mat.tex[1];
            if (normalTex != kInvalidTextureIndex)
            {
                float3 mapNormal = gTextures2D[NonUniformResourceIndex(normalTex)]
                                       .Sample(gMaterialSampler, psIn.uv).xyz
                                   * 2.0f - 1.0f;
                screenNormal = normalize(mapNormal.x * psIn.tangent
                                       + mapNormal.y * psIn.bitangent
                                       + mapNormal.z * psIn.normal);
            }
            else
            {
                screenNormal = normalize(psIn.normal);
            }
        }
        else
        {
            screenNormal = normalize(psIn.normal);
        }

        float3 eyeVec   = normalize(psIn.viewPos);
        float  edotN    = max(dot(-eyeVec, screenNormal), 0.0f);
        float  fresnel  = computeFresnel(edotN, mat.frenselBias, mat.frenselPow);

        // reflect() takes the incoming vector — in view space the eye points
        // from camera (origin) toward fragment, which matches eyeVec.
        // Transform back to world via invViewRotationMat to sample the cube
        // (which lives in world space). Matches the reference
        // translucency.frag.fsl cube-map reflection.
        float3 envDirView  = reflect(eyeVec, screenNormal);
        float3 envDirWorld = mul(gPerFrame.invViewRotationMat,
                                 float4(envDirView, 1.0f)).xyz;
        float4 reflectionColor =
            gTexturesCube[NonUniformResourceIndex(mat.cubeMapTextureIndex)]
                .Sample(gMaterialSampler, envDirWorld);

        // Optional 2D alpha mask that fades the reflection contribution by UV.
        if ((textureConfig & kMaterialFlagEnableCubeMapAlpha) != 0u)
        {
            uint cubeAlphaTex = mat.tex[7];
            if (cubeAlphaTex != kInvalidTextureIndex)
            {
                float fEnvMapAlpha =
                    gTextures2D[NonUniformResourceIndex(cubeAlphaTex)]
                        .Sample(gMaterialSampler, psIn.uv).w;
                reflectionColor *= fEnvMapAlpha;
            }
        }

        finalColor.xyz += reflectionColor.xyz
                        * fresnel * finalAlpha * obj.lightLevel;

        // Rim reflection term — view-space normal vs -Z.
        if (mat.rimLightMul > 0.0f)
        {
            float fRim = dot(screenNormal, float3(0.0f, 0.0f, -1.0f));
            fRim = pow(1.0f - abs(fRim), mat.rimLightPow) * mat.rimLightMul;
            finalColor.xyz += reflectionColor.xyz
                            * fRim * finalAlpha * obj.lightLevel;
        }
    }

    // Reference uses `finalColor.a == 0.0` (no epsilon). The strict check
    // matches; keeps premul-alpha fully-faded fragments out of the depth
    // test interactions when MSAA resolves later.
    if (finalColor.a <= 0.0f)
    {
        discard;
    }

    return finalColor;
}
