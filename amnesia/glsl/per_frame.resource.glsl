#ifndef PER_FRAME_RESOURCE_GLSL
#define PER_FRAME_RESOURCE_GLSL

// Including TUs must enable GL_GOOGLE_include_directive themselves.
#include "forward_shared.h"

#ifndef ALPHA_REJECT
#define ALPHA_REJECT 0.5
#endif

// `struct DiffuseMaterial` is provided by forward_shared.h.

// Each tex[i] is a full uint32 bindless slot index (or INVALID_TEXTURE_INDEX).
uint DiffuseMaterial_DiffuseTexture_ID(DiffuseMaterial m)        { return m.tex[0]; }
uint DiffuseMaterial_NormalTexture_ID(DiffuseMaterial m)         { return m.tex[1]; }
uint DiffuseMaterial_AlphaTexture_ID(DiffuseMaterial m)          { return m.tex[2]; }
uint DiffuseMaterial_SpecularTexture_ID(DiffuseMaterial m)       { return m.tex[3]; }
uint DiffuseMaterial_HeightTexture_ID(DiffuseMaterial m)         { return m.tex[4]; }
uint DiffuseMaterial_IlluminiationTexture_ID(DiffuseMaterial m)  { return m.tex[5]; }
uint DiffuseMaterial_DissolveAlphaTexture_ID(DiffuseMaterial m)  { return m.tex[6]; }
uint DiffuseMaterial_CubeMapAlphaTexture_ID(DiffuseMaterial m)   { return m.tex[7]; }

// `struct UniformObject` is provided by forward_shared.h.

#define MATERIAL_ID(o) ((o).materialID)

layout(set = 1, binding = 0) uniform PerFrameBlock {
    PerFrameConstants u;
} perFrame;

#define invViewRotationMat perFrame.u.invViewRotationMat
#define viewMat            perFrame.u.viewMat
#define invViewMat         perFrame.u.invViewMat
#define projMat            perFrame.u.projMat
#define invProjMat         perFrame.u.invProjMat
#define worldFogStart      perFrame.u.worldFogStart
#define worldFogLength     perFrame.u.worldFogLength
#define oneMinusFogAlpha   perFrame.u.oneMinusFogAlpha
#define fogFalloffExp      perFrame.u.fogFalloffExp
#define worldFogColor      perFrame.u.worldFogColor
#define viewTexel          perFrame.u.viewTexel
#define viewportSize       perFrame.u.viewportSize
#define afT                perFrame.u.afT
#define totalFrames        perFrame.u.totalFrames
#define cameraFov          perFrame.u.cameraFov

#endif // PER_FRAME_RESOURCE_GLSL
