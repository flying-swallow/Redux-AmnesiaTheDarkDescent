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
// deviation is refraction, which is NOT implemented in this port. The legacy
// bend-behind distortion (screen-color copy + per-fragment UV offset) was
// dropped, and the RT V-buffer that was meant to supply the refracted
// background by bending the primary ray was retired without ever being wired
// up. So the background behind a refractive pixel is unrefracted and this
// pass simply alpha-blends over it — no screen-color sample, no refraction
// block here.
// Reflection stays the material's static cube map, exactly as the reference.


#include "Constants.h"
import bindless;
import SceneTypes;
import PerFrame.resource;
import Fog;
import Scene;
import Material;             // load*Material / materialAlphaTest / traceShadowRay / traceReflectionHit
// Shared legacy blend math (kBlendMode* + the display-space modulation /
// decode stages). This pass keeps the two stages SEPARATE — unlike
// Particle.frag — because the display-space cube-map reflection + rim add
// must sit between the source modulation and the single decode to linear.
import BlendModes;

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
// The base is clamped to a tiny positive epsilon: eDotN of two normalize()d
// vectors can exceed 1.0 by a ULP at dead-on view angles, and pow() of a
// negative base is NaN (exp2(y·log2(x))) — with MUL blending that poisons the
// framebuffer pixel permanently (saturate doesn't help: FClamp(NaN) is
// undefined in Vulkan). Hit by amnesia_bottle.mat (FrenselPow=0.5).
float computeFresnel(float afEDotN, float afFresnelBias, float afFresnelPow)
{
    float fFacing = max(1.0f - afEDotN, 1.0e-6f);
    return saturate(afFresnelBias + (1.0f - afFresnelBias) * pow(fFacing, afFresnelPow));
}

[shader("fragment")]
float4 psMain(TranslucentVSOut psIn) : SV_TARGET
{
    UniformObject       obj = gSceneObjects[psIn.objectId];
    TranslucentMaterial mat = loadTranslucentMaterial(obj.materialID);

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

    // Legacy display-space source: the base game blended raw gamma texels ×
    // authored vertex colors in a UNORM display target. Re-encode the
    // hardware-sRGB-decoded sample so the product (and all the blend-mode /
    // reflection math below) runs in the space it was authored for; the
    // result is decoded to linear ONCE at the end (see the conversion switch).
    float4 diffuseColor = float4(0.0f, 0.0f, 0.0f, 1.0f);
    if ((textureConfig & kMaterialFlagEnableDiffuse) != 0u)
    {
        uint diffuseTex = mat.tex[0];
        if (diffuseTex != kInvalidTextureIndex)
        {
            float4 s = sampleBindless2D(diffuseTex, psIn.uv);
            diffuseColor = displaySpaceSample(s);
        }
        diffuseColor *= psIn.color;
    }

    // Per-pixel fog (world + per-area). The shared Fog helper returns a
    // visibility scalar — translucents multiply it into the per-pixel alpha
    // so glass/decals fade behind fog without painting the fog colour.
    float3 surfacePosW = mul(gPerFrame.invViewMat, float4(psIn.viewPos, 1.0f)).xyz;
    FogResult fog = computeFog(surfacePosW, gPerFrame.posW);
    float finalAlpha = pc.sceneAlpha * fog.visibility;

    // AffectedByLightLevel materials dim with the surrounding analytic light
    // (base-game afLightLevel behavior — see lightLevelAt). Per-pixel,
    // gated on the raw material config (not the illumination-draw-masked
    // textureConfig) so the cube-map second draw dims its reflection too,
    // matching the legacy shader's afLightLevel use on the reflection terms.
    float lightLevel = 1.0f;
    if ((mat.materialConfig & kMaterialFlagAffectedByLightLevel) != 0u)
    {
        lightLevel = lightLevelAt(surfacePosW);
    }

    // Stage 1 of the shared blend scheme: display-space source modulation
    // (the legacy per-mode pre-bias). The reflection/rim add below stays in
    // display space, so the decode (stage 2) runs after it.
    float4 finalColor = applyBlendModulation(pc.blendMode, diffuseColor,
                                             finalAlpha, lightLevel);

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
                float3 mapNormal = sampleBindless2D(normalTex, psIn.uv).xyz
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
        // No re-encode needed: cube maps keep a UNORM view (only Diffuse /
        // Illumination get sRGB views — MaterialManager.cpp), so the sample
        // is already the raw gamma value the legacy display-space add wants.
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
                    sampleBindless2D(cubeAlphaTex, psIn.uv).w;
                reflectionColor *= fEnvMapAlpha;
            }
        }

        finalColor.xyz += reflectionColor.xyz
                        * fresnel * finalAlpha * lightLevel;

        // Rim reflection term — view-space normal vs -Z. Base clamped positive
        // for the same ULP reason as computeFresnel (abs(dot) can exceed 1).
        if (mat.rimLightMul > 0.0f)
        {
            float fRim = dot(screenNormal, float3(0.0f, 0.0f, -1.0f));
            fRim = pow(max(1.0f - abs(fRim), 1.0e-6f), mat.rimLightPow) * mat.rimLightMul;
            finalColor.xyz += reflectionColor.xyz
                            * fRim * finalAlpha * lightLevel;
        }
    }

    // Reference uses `finalColor.a == 0.0` (no epsilon). The strict check
    // matches; keeps premul-alpha fully-faded fragments out of the depth
    // test interactions when MSAA resolves later. Tested on the display-space
    // alpha, before the blend-weight reshape below.
    float displayAlpha = finalColor.a;
    if (displayAlpha <= 0.0f)
    {
        discard;
    }

    // Everything above (pre-bias, reflection, rim) is legacy display-space
    // math. Stage 2 of the shared blend scheme decodes to linear ONCE and
    // reshapes the dst-blend weights (see BlendModes.slang).
    return encodeBlendOutputLinear(pc.blendMode, finalColor);
}
