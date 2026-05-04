#ifndef FORWARD_SHADER_COMMON_GLSL
#define FORWARD_SHADER_COMMON_GLSL

#define LIGHT_CLUSTER_WIDTH 8
#define LIGHT_CLUSTER_HEIGHT 8
#define LIGHT_CLUSTER_COUNT 128

#define POINT_LIGHT_MAX_COUNT 1024
#define SCENE_MAX_TEXTURE_COUNT 4096

#define SCENE_OPAQUE_COUNT 512

#define LIGHT_CLUSTER_COUNT_POS(ix, iy)     (((iy) * LIGHT_CLUSTER_WIDTH) + (ix))
#define LIGHT_CLUSTER_DATA_POS(il, ix, iy)  (LIGHT_CLUSTER_COUNT_POS(ix, iy) * LIGHT_CLUSTER_COUNT + (il))

#define INVALID_TEXTURE_INDEX (0xffffu)

#ifndef ALPHA_REJECT
#define ALPHA_REJECT 0.5
#endif

struct DiffuseMaterial {
    uint  tex[4];
    uint  materialConfig;
    float heightMapScale;
    float heightMapBias;
    float frenselBias;
    float frenselPow;
};

uint DiffuseMaterial_DiffuseTexture_ID(DiffuseMaterial m)        { return  m.tex[0]        & 0xffffu; }
uint DiffuseMaterial_NormalTexture_ID(DiffuseMaterial m)         { return (m.tex[0] >> 16) & 0xffffu; }
uint DiffuseMaterial_AlphaTexture_ID(DiffuseMaterial m)          { return  m.tex[1]        & 0xffffu; }
uint DiffuseMaterial_SpecularTexture_ID(DiffuseMaterial m)       { return (m.tex[1] >> 16) & 0xffffu; }
uint DiffuseMaterial_HeightTexture_ID(DiffuseMaterial m)         { return  m.tex[2]        & 0xffffu; }
uint DiffuseMaterial_IlluminiationTexture_ID(DiffuseMaterial m)  { return (m.tex[2] >> 16) & 0xffffu; }
uint DiffuseMaterial_DissolveAlphaTexture_ID(DiffuseMaterial m)  { return  m.tex[3]        & 0xffffu; }
uint DiffuseMaterial_CubeMapAlphaTexture_ID(DiffuseMaterial m)   { return (m.tex[3] >> 16) & 0xffffu; }

struct UniformObject {
    float dissolveAmount;
    uint  materialID;
    float lightLevel;
    float illuminationAmount;
    mat4  modelMat;
    mat4  invModelMat;
    mat4  uvMat;
};

#define MATERIAL_ID(o) ((o).materialID)

struct PointLight {
    mat4  mvp;
    vec3  lightPos;
    uint  config;
    vec4  lightColor;
    float lightRadius;
};

layout(set = 1, binding = 0) uniform PerFrameConstants {
    mat4  invViewRotationMat;
    mat4  viewMat;
    mat4  invViewMat;
    mat4  projMat;
    mat4  viewProjMat;

    float worldFogStart;
    float worldFogLength;
    float oneMinusFogAlpha;
    float fogFalloffExp;
    vec4  worldFogColor;

    vec2  viewTexel;
    vec2  viewportSize;
    float afT;
} perFrame;

#define invViewRotationMat perFrame.invViewRotationMat
#define viewMat            perFrame.viewMat
#define invViewMat         perFrame.invViewMat
#define projMat            perFrame.projMat
#define viewProjMat        perFrame.viewProjMat
#define worldFogStart      perFrame.worldFogStart
#define worldFogLength     perFrame.worldFogLength
#define oneMinusFogAlpha   perFrame.oneMinusFogAlpha
#define fogFalloffExp      perFrame.fogFalloffExp
#define worldFogColor      perFrame.worldFogColor
#define viewTexel          perFrame.viewTexel
#define viewportSize       perFrame.viewportSize
#define afT                perFrame.afT

#endif // FORWARD_SHADER_COMMON_GLSL
