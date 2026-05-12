#ifndef FORWARD_SHADER_COMMON_GLSL
#define FORWARD_SHADER_COMMON_GLSL

// Including TUs must enable GL_GOOGLE_include_directive themselves.
#include "forward_shared.h"

#ifndef ALPHA_REJECT
#define ALPHA_REJECT 0.5
#endif

// `struct DiffuseMaterial` is provided by forward_shared.h.

uint DiffuseMaterial_DiffuseTexture_ID(DiffuseMaterial m)        { return  m.tex[0]        & 0xffffu; }
uint DiffuseMaterial_NormalTexture_ID(DiffuseMaterial m)         { return (m.tex[0] >> 16) & 0xffffu; }
uint DiffuseMaterial_AlphaTexture_ID(DiffuseMaterial m)          { return  m.tex[1]        & 0xffffu; }
uint DiffuseMaterial_SpecularTexture_ID(DiffuseMaterial m)       { return (m.tex[1] >> 16) & 0xffffu; }
uint DiffuseMaterial_HeightTexture_ID(DiffuseMaterial m)         { return  m.tex[2]        & 0xffffu; }
uint DiffuseMaterial_IlluminiationTexture_ID(DiffuseMaterial m)  { return (m.tex[2] >> 16) & 0xffffu; }
uint DiffuseMaterial_DissolveAlphaTexture_ID(DiffuseMaterial m)  { return  m.tex[3]        & 0xffffu; }
uint DiffuseMaterial_CubeMapAlphaTexture_ID(DiffuseMaterial m)   { return (m.tex[3] >> 16) & 0xffffu; }

// `struct UniformObject` is provided by forward_shared.h.

#define MATERIAL_ID(o) ((o).materialID)

layout(set = 1, binding = 0) uniform PerFrameBlock {
    PerFrameConstants u;
} perFrame;

#define invViewRotationMat perFrame.u.invViewRotationMat
#define viewMat            perFrame.u.viewMat
#define invViewMat         perFrame.u.invViewMat
#define projMat            perFrame.u.projMat
#define viewProjMat        perFrame.u.viewProjMat
#define worldFogStart      perFrame.u.worldFogStart
#define worldFogLength     perFrame.u.worldFogLength
#define oneMinusFogAlpha   perFrame.u.oneMinusFogAlpha
#define fogFalloffExp      perFrame.u.fogFalloffExp
#define worldFogColor      perFrame.u.worldFogColor
#define viewTexel          perFrame.u.viewTexel
#define viewportSize       perFrame.u.viewportSize
#define afT                perFrame.u.afT

#endif // FORWARD_SHADER_COMMON_GLSL
