#ifndef FORWARD_SHARED_H
#define FORWARD_SHARED_H

#ifdef __cplusplus
#include <cstdint>
namespace hpl {
typedef uint32_t uint;
typedef float    mat4[16];
typedef float    vec4[4];
typedef float    vec3[3];
typedef float    vec2[2];
#endif

// Bindless slot capacities. The C++ side sizes its BindlessPool / SSBO
// allocations from these; the GLSL side uses TEXTURE_SLOT_CAPACITY as the
// `textures_2d[]` array length.
#define OBJECT_SLOT_CAPACITY    (16384u * 2u)
#define TEXTURE_SLOT_CAPACITY   16384u
#define MATERIAL_SLOT_CAPACITY  16384u
#define POINT_SLOT_LIGHT_CAPACITY     256u

// Clustered shading froxel grid. Mirrors AmnesiaTheDarkDescent's
// scene_defs.h.fsl light-cluster layout: 16x9 tiles in screen space, 24
// exponential depth slices, up to 128 lights per froxel. Cluster count and
// data SSBOs are sized from these on the C++ side and indexed in the GLSL
// passes via LIGHT_FROXEL_*_POS below.
#define LIGHT_CLUSTER_WIDTH               16u
#define LIGHT_CLUSTER_HEIGHT              9u
#define LIGHT_CLUSTER_SLICE               24u
#define LIGHT_CLUSTER_MAX_LIGHTS_PER_FROXEL 128u
#define LIGHT_FROXEL_TOTAL_COUNT \
    (LIGHT_CLUSTER_WIDTH * LIGHT_CLUSTER_HEIGHT * LIGHT_CLUSTER_SLICE)
#define LIGHT_FROXEL_COUNT_POS(ix, iy, iz) \
    ((LIGHT_CLUSTER_WIDTH * LIGHT_CLUSTER_HEIGHT * (iz)) + \
     ((iy) * LIGHT_CLUSTER_WIDTH) + (ix))
#define LIGHT_FROXEL_DATA_POS(ix, iy, iz, il) \
    (LIGHT_FROXEL_COUNT_POS(ix, iy, iz) * LIGHT_CLUSTER_MAX_LIGHTS_PER_FROXEL + (il))

// Surfel-GI capacities. Mirrors `kMaxSurfelCount` / `kMaxRayCount` in
// amnesia/glsl/host_device.h — kept here in C-preprocessor form so the C++
// allocator (cHybridRenderer) and any GLSL pass that includes this header
// share a single source of truth for sizing the surfel SSBOs declared in
// bindless.resource.glsl (set 0, bindings 10..16).
#define SURFEL_MAX_CAPACITY     150000u
#define MAX_RAY_COUNT           (SURFEL_MAX_CAPACITY * 64u)

// Cell-grid capacities. The cell grid is static infrastructure shared by all
// surfel passes (set 0, bindings 17..19 in bindless.resource.glsl). The grid
// has two regions encoded into a single flat cellBuffer[] (see
// shaderUtil_grid.glsl::getFlattenCellIndexNonUniform):
//   - CELL_COUNT       = uniform cube region (CELL_GRID_DIM^3 cells)
//   - frustum layers   = 6 face frustums, each with CELL_FRUSTUM_LAYERS
//                        log-spaced depth slices of CELL_GRID_DIM^2 cells
// TOTAL_CELL_COUNT is the size of cellBuffer[] and must match
// host_device.h::n^3 + 6*n^2*m so spatial-hash lookups outside the central
// 96-unit cube don't write/read out of bounds.
// CELL_TO_SURFEL_CAPACITY budgets the flat per-cell surfel-index table: a
// single surfel can intersect up to 27 neighbour cells (3x3x3 stamp in
// surfel_update.comp), so worst case = SURFEL_MAX_CAPACITY * 27.
#define CELL_GRID_DIM           64u
#define CELL_FRUSTUM_LAYERS     16u
#define CELL_COUNT              (CELL_GRID_DIM * CELL_GRID_DIM * CELL_GRID_DIM)
#define TOTAL_CELL_COUNT        (CELL_COUNT + 6u * CELL_GRID_DIM * CELL_GRID_DIM * CELL_FRUSTUM_LAYERS)
#define CELL_TO_SURFEL_CAPACITY (SURFEL_MAX_CAPACITY * 27u)

// Sentinel returned by the renderer's texture slot allocator when a slot is
// missing or the pool is exhausted. Matches the full-uint32 entries in
// DiffuseMaterial::tex[] — uses UINT32_MAX so it's distinguishable from any
// valid bindless slot index even past the current TEXTURE_SLOT_CAPACITY.
#define INVALID_TEXTURE_INDEX   (0xffffffffu)

// Forward-pass set 0 binding indices (the engine-owned bindless set).
#define BINDING_TEXTURES_2D                 0
#define BINDING_TEXTURES_CUBE               1
#define BINDING_TEXTURES_2D_ARRAY           2
#define BINDING_OPAQUE_POSITION_HANDLES     3
#define BINDING_OPAQUE_TANGENT_HANDLES      4
#define BINDING_OPAQUE_NORMAL_HANDLES       5
#define BINDING_OPAQUE_UV0_HANDLES          6
#define BINDING_OPAQUE_COLOR_HANDLES        7
#define BINDING_OPAQUE_INDEX_HANDLES        8
#define BINDING_MATERIAL_SAMPLER            9
// Surfel-GI SSBOs (mirrors bindless.resource.glsl set=0, bindings 10..16).
#define BINDING_SURFEL_COUNTER              10
#define BINDING_SURFEL_BUFFER               11
#define BINDING_SURFEL_ALIVE                12
#define BINDING_SURFEL_DEAD                 13
#define BINDING_SURFEL_DIRTY                14
#define BINDING_SURFEL_RECYCLE              15
#define BINDING_SURFEL_RAY                  16
// Cell-grid SSBOs (mirrors bindless.resource.glsl set=0, bindings 17..19).
#define BINDING_CELL_BUFFER                 17
#define BINDING_CELL_COUNTER                18
#define BINDING_CELL_TO_SURFEL              19
// Scene-object / opaque-material tables (mirrors bindless.resource.glsl
// set=0, bindings 20..21). Slot-allocated by cHybridRenderer alongside the
// other bindless pools (OBJECT_SLOT_CAPACITY / MATERIAL_SLOT_CAPACITY).
#define BINDING_SCENE_OBJECTS               20
#define BINDING_OPAQUE_MATERIAL             21
// Per-frame point-light SSBO. Filled by cHybridRenderer's light loop and
// uploaded device-local via RI.uploader (no host-mapped destination), so the
// GPU never reads partially-written data from an in-flight frame.
#define BINDING_POINT_LIGHTS                22
// Per-variant material SSBOs (mirrors MaterialID::Translucent / Water / Decal
// on the C++ side; see Material.h). Each variant has its own bindless table so
// structs stay at their natural size and shaders never branch on a runtime
// `materialType` discriminator to interpret fields. The render pass binds
// whichever buffer it needs. SolidDiffuse keeps BINDING_OPAQUE_MATERIAL above.
#define BINDING_TRANSLUCENT_MATERIAL        24
#define BINDING_WATER_MATERIAL              25
#define BINDING_DECAL_MATERIAL              26
// Clustered-shading SSBOs. Written every frame by the cluster build chain
// (light_clusters_clear + point_light_clusters) and read by any future
// forward+ shading pass. Live alongside the other bindless SSBOs so a
// consumer reaches them through the same set 0 binding that already
// carries point lights, scene objects, etc.
#define BINDING_LIGHT_CLUSTERS_COUNT        27
#define BINDING_LIGHT_CLUSTERS_DATA         28
// Per-frame spot-light SSBO. Same upload path as point lights; consumed by
// shading passes alongside pointLights[].
#define BINDING_SPOT_LIGHTS                 29

// Texture-slot indices into the bindless `textures_2d[]` array (set=0,
// binding=0). One uint32 per slot — see per_frame.resource.glsl for the
// `DiffuseMaterial_*Texture_ID` accessors that index this array.
// Slot layout: 0=Diffuse, 1=NMap, 2=Alpha, 3=Specular,
//              4=Height, 5=Illumination, 6=DissolveAlpha, 7=CubeMapAlpha.
struct DiffuseMaterial {
    uint  tex[8];
    uint  materialConfig;
    float heightMapScale;
    float heightMapBias;
    float frenselBias;
    float frenselPow;
};

// GPU twin of MaterialTranslucent (Material.h). All bool flags
// (isAffectedByLightLevel, hasRefraction, refractionEdgeCheck,
// refractionNormals) and the eMaterialBlendMode pack into materialConfig.
struct TranslucentMaterial {
    uint  tex[8];
    uint  materialConfig;
    float refractionScale;
    float frenselBias;
    float frenselPow;
    float rimLightMul;
    float rimLightPow;
};

// GPU twin of MaterialWater (Material.h). Bool flags (hasReflection,
// isLargeSurface, worldReflectionOcclusionTest) pack into materialConfig.
struct WaterMaterial {
    uint  tex[8];
    uint  materialConfig;
    float refractionScale;
    float frenselBias;
    float frenselPow;
    float reflectionFadeStart;
    float reflectionFadeEnd;
    float waveSpeed;
    float waveAmplitude;
    float waveFreq;
};

// GPU twin of MaterialDecal (Material.h). Only carries texture slots and
// the blend mode (encoded in materialConfig); no per-instance scalars.
struct DecalMaterial {
    uint tex[8];
    uint materialConfig;
};

struct UniformObject {
    float dissolveAmount;
    uint  materialID;
    float lightLevel;
    float illuminationAmount;
    mat4  modelMat;
    mat4  invModelMat;
    mat4  uvMat;
};

// Scalar-layout point light. Position+radius and color+intensity pair into
// natural 16-byte slots; the C++ side memcpy's an array of these into the
// device-local SSBO via RI.uploader each frame.
// `attenuationTextureIndex` is the bindless slot of the artist-authored
// radial falloff LUT (canonical HPL2 attenuation, indexed by (d/r)²). Set
// to INVALID_TEXTURE_INDEX when no map is bound and the shader will fall
// back to saturate(1 - (d/r)²).
struct PointLight {
    vec3  position;
    float radius;
    vec3  color;
    float intensity;
    uint  attenuationTextureIndex;
    uint  _pad0;
    uint  _pad1;
    uint  _pad2;
};

// Scalar-layout spot light. Mirrors PointLight but adds a cone forward +
// pre-baked cos(angle/2) for the cone cull, optional gobo slot, optional
// shadow bit, and the light's view-projection matrix for projecting the
// gobo (and any future shadow-map UV) into the cone.
struct SpotLight {
    vec3  position;
    float radius;
    vec3  direction;        // world-space outward forward
    float cosOuterAngle;    // cos(GetFOV() * 0.5)
    vec3  color;
    float intensity;
    uint  attenuationTextureIndex; // radial atten LUT (same shape as PointLight)
    uint  goboTextureIndex;        // INVALID_TEXTURE_INDEX → no gobo
    uint  shadowEnabled;           // 0 or 1
    uint  _pad0;
    mat4  viewProjection;          // light-space ViewProj for gobo UVs
};

// Per-frame UBO contents (std140). On the GLSL side this struct is wrapped
// in a uniform block at set=1, binding=0 (see per_frame.resource.glsl).
struct PerFrameConstants {
    mat4  invViewRotationMat;
    mat4  viewMat;
    mat4  invViewMat;
    mat4  projMat;
    mat4  invProjMat;        // inverse of projMat — for WorldPosFromDepth

    float worldFogStart;
    float worldFogLength;
    float oneMinusFogAlpha;
    float fogFalloffExp;
    vec4  worldFogColor;

    vec2  viewTexel;
    vec2  viewportSize;
    float afT;
    uint  totalFrames;       // monotonic frame counter for randSeed inputs
    float cameraFov;         // main camera vertical FOV (radians)
    uint  pointLightCount;   // active entries in the bindless pointLights[] SSBO
    float fireflyClampThreshold;  // luminance cap applied in surfel raytrace + integrate
    float zNear;             // main camera near plane (view-space, positive)
    float zFar;              // main camera far plane (view-space, positive)
    uint  spotLightCount;    // active entries in the bindless spotLights[] SSBO
    uint  _pad0;
    uint  _pad1;
    uint  _pad2;
};

#ifdef __cplusplus
// C++ twins of the surfel structs defined in amnesia/glsl/host_device.h.
// Kept inside the __cplusplus guard so they don't collide with the GLSL
// definitions when this header is included from a shader. Scalar layout on
// the GLSL side packs without alignment padding, which already matches the
// natural C++ layout of `float[3]` + `float`/`uint` fields used here. The
// static_asserts below catch any drift.
struct MSMEData {
    vec3  mean;
    float vbbr;
    vec3  shortMean;
    float inconsistency;
    vec3  variance;
    float pad;
};

struct SurfelCounter {
    uint aliveSurfelCnt;
    uint deadSurfelCnt;
    uint dirtySurfelCnt;
    uint surfelRayCnt;
};

struct Surfel {
    vec3 position;  float radius;
    vec3 radiance;  uint  normal;
    uint objID;
    uint rayOffset;
    uint rayCount;
    uint irradiance;
    MSMEData msmeData;
};

struct SurfelRecycleInfo {
    uint life;
    uint frame;
    uint status;
    uint lastSeenFrame;
};

struct SurfelRay {
    uint  surfelID;
    uint  dir_o;
    float pdf;
    float pad;
    vec3  radiance;
    float t;
};

struct CellInfo {
    uint surfelOffset;
    uint surfelCount;
};

struct CellCounter {
    uint totalCellCount;
    uint aliveSurfelInCell;
};

// Historical C++ names; the canonical names match the GLSL twins above.
using ObjectGPUData      = UniformObject;
using DiffuseMaterialGPU = DiffuseMaterial;

} // namespace hpl
#endif

#endif // FORWARD_SHARED_H
