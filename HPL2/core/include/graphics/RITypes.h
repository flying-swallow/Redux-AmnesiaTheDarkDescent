
#ifndef RI_TYPES_H
#define RI_TYPES_H

#include "graphics/RIBarrier.h"
#include "graphics/RIDefines.h"
#include "graphics/RIFormat.h"
#include <cassert>
#include <cstring>

#ifdef DEVICE_SUPPORT_VULKAN
#include "volk.h"
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include "vk_mem_alloc.h"
#endif

// Macro anguish
#ifdef LoadBitmap
#undef LoadBitmap
#endif

#ifdef SendMessage
#undef SendMessage
#endif

#ifdef CreateEvent
#undef CreateEvent
#endif

#ifdef CreateWindow
#undef CreateWindow
#endif

#include <cstdio>
#include <stdint.h>
#include <variant>
#include <vector>
#undef DestroyAll
#undef ButtonPress

#include "system/Hasher.h"
#include "system/LowLevelSystem.h"

#define R_VK_ADD_STRUCT(current, next)                                         \
  {                                                                            \
    void *__pNext = (void *)((current)->pNext);                                \
    (current)->pNext = (next);                                                 \
    (next)->pNext = __pNext;                                                   \
  }
#define VK_WrapResult(res)                                                     \
  __VK_WrapResult(res, __FILE__, __FUNCTION__, __LINE__)

static inline bool __VK_WrapResult(VkResult result, const char *sourceFilename,
                                   const char *functionName, int sourceLine) {
  if (result != VK_SUCCESS) {
    hpl::Log("RI: VK %i, file %s:%i (%s)\n", result, sourceFilename, sourceLine,
             functionName);
    return false;
  }
  return true;
}

#define RI_QUEUE_GRAPHICS_BIT 0x1
#define RI_QUEUE_COMPUTE_BIT 0x2
#define RI_QUEUE_TRANSFER_BIT 0x4
#define RI_QUEUE_SPARSE_BINDING_BIT 0x8
#define RI_QUEUE_VIDEO_DECODE_BIT 0x10
#define RI_QUEUE_VIDEO_ENCODE_BIT 0x20
#define RI_QUEUE_PROTECTED_BIT 0x40
#define RI_QUEUE_OPTICAL_FLOW_BIT_NV 0x80
#define RI_QUEUE_INVALID 0x0

enum RIPresetLevel_e {
  RI_GPU_PRESET_NONE = 0,
  RI_GPU_PRESET_OFFICE,  // This means unsupported
  RI_GPU_PRESET_VERYLOW, // Mostly for mobile GPU
  RI_GPU_PRESET_LOW,
  RI_GPU_PRESET_MEDIUM,
  RI_GPU_PRESET_HIGH,
  RI_GPU_PRESET_ULTRA,
  RI_GPU_PRESET_COUNT
};

enum RITextureViewType_s {
  RI_VIEWTYPE_SHADER_RESOURCE_1D,
  RI_VIEWTYPE_SHADER_RESOURCE_1D_ARRAY,
  RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_1D,
  RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_1D_ARRAY,
  RI_VIEWTYPE_SHADER_RESOURCE_2D,
  RI_VIEWTYPE_SHADER_RESOURCE_2D_ARRAY,
  RI_VIEWTYPE_SHADER_RESOURCE_CUBE,
  RI_VIEWTYPE_SHADER_RESOURCE_CUBE_ARRAY,
  RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_2D,
  RI_VIEWTYPE_SHADER_RESOURCE_STORAGE_2D_ARRAY,

  RI_VIEWTYPE_COLOR_ATTACHMENT,
  RI_VIEWTYPE_DEPTH_STENCIL_ATTACHMENT,
  RI_VIEWTYPE_DEPTH_READONLY_STENCIL_ATTACHMENT,
  RI_VIEWTYPE_DEPTH_ATTACHMENT_STENCIL_READONLY,
  RI_VIEWTYPE_DEPTH_STENCIL_READONLY,
  RI_VIEWTYPE_SHADING_RATE_ATTACHMENT
};

enum RITextureUsageBits_e {
  RI_USAGE_NONE = 0,
  RI_USAGE_SHADER_RESOURCE = 0x1,
  RI_USAGE_SHADER_RESOURCE_STORAGE = 0x2,
  RI_USAGE_COLOR_ATTACHMENT = 0x4,
  RI_USAGE_DEPTH_STENCIL_ATTACHMENT = 0x8,
  RI_USAGE_SHADING_RATE = 0x10,
};

enum RISampleCount_e {
  RI_SAMPLE_COUNT_1 = 1,
  RI_SAMPLE_COUNT_2 = 2,
  RI_SAMPLE_COUNT_4 = 4,
  RI_SAMPLE_COUNT_8 = 8,
  RI_SAMPLE_COUNT_16 = 16,
  RI_SAMPLE_COUNT_COUNT = 5,
};

enum RIDeviceAPI_e {
  RI_DEVICE_API_UNKNOWN,
  RI_DEVICE_API_VK,
  RI_DEVICE_API_D3D11,
  RI_DEVICE_API_D3D12,
  RI_DEVICE_API_MTL
};

enum RISwapchainFormat_e {
  RI_SWAPCHAIN_BT709_G10_16BIT,
  RI_SWAPCHAIN_BT709_G22_8BIT,
  RI_SWAPCHAIN_BT709_G22_10BIT,
  RI_SWAPCHAIN_BT2020_G2084_10BIT
};

enum RIQueueType_e {
  RI_QUEUE_GRAPHICS,
  RI_QUEUE_COMPUTE,
  RI_QUEUE_COPY,
  RI_QUEUE_LEN
};

enum RIAdapterType_e {
  RI_ADAPTER_TYPE_OTHER,
  RI_ADAPTER_TYPE_CPU,
  RI_ADAPTER_TYPE_VIRTUAL_GPU,
  RI_ADAPTER_TYPE_INTEGRATED_GPU,
  RI_ADAPTER_TYPE_DISCRETE_GPU,
};

enum RIResult_e {
  RI_INCOMPLETE_DEVICE = -2,
  RI_FAIL = -1,
  RI_SUCCESS = 0,
  RI_INCOMPLETE
};

enum RIBufferUsage_e {
  RI_BUFFER_USAGE_NONE = 0,
  RI_BUFFER_USAGE_SHADER_RESOURCE = 0x1,
  RI_BUFFER_USAGE_SHADER_RESOURCE_STORAGE = 0x2,
  RI_BUFFER_USAGE_VERTEX_BUFFER = 0x4,
  RI_BUFFER_USAGE_INDEX_BUFFER = 0x8,
  RI_BUFFER_USAGE_CONSTANT_BUFFER = 0x10,
  RI_BUFFER_USAGE_ARGUMENT_BUFFER = 0x20,

  RI_BUFFER_USAGE_SCRATCH = 0x40,
  RI_BUFFER_USAGE_BINDING_TABLE = 0x80,
  RI_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPT = 0x100,
  RI_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE = 0x200,
};

enum RITextureType_e { RI_TEXTURE_1D, RI_TEXTURE_2D, RI_TEXTURE_3D };

enum RIVendor_e { RI_UNKNOWN, RI_NVIDIA, RI_AMD, RI_INTEL };

enum RITopology_e {
  RI_TOPOLOGY_POINT_LIST,
  RI_TOPOLOGY_LINE_LIST,
  RI_TOPOLOGY_LINE_STRIP,
  RI_TOPOLOGY_TRIANGLE_LIST,
  RI_TOPOLOGY_TRIANGLE_STRIP,
  RI_TOPOLOGY_LINE_LIST_WITH_ADJACENCY,
  RI_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY,
  RI_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY,
  RI_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY,
  RI_TOPOLOGY_PATCH_LIST
};

// R - fragment's depth or stencil reference
// D - depth or stencil buffer
enum RICompareFunc_e {
  RI_COMPARE_NONE,         // test is disabled
  RI_COMPARE_ALWAYS,       // true
  RI_COMPARE_NEVER,        // false
  RI_COMPARE_EQUAL,        // R == D
  RI_COMPARE_NOT_EQUAL,    // R != D
  RI_COMPARE_LESS,         // R < D
  RI_COMPARE_LESS_EQUAL,   // R <= D
  RI_COMPARE_GREATER,      // R > D
  RI_COMPARE_GREATER_EQUAL // R >= D
};

enum RICullMode_e {
  RI_CULL_MODE_NONE = 0,
  RI_CULL_MODE_FRONT = 0x1,
  RI_CULL_MODE_BACK = 0x2,
  RI_CULL_MODE_BOTH = RI_CULL_MODE_FRONT | RI_CULL_MODE_BACK
};

enum RIIndexType_e { RI_INDEX_TYPE_16, RI_INDEX_TYPE_32 };

enum RIAccelStructureType_e {
  RI_ACCEL_STRUCTURE_TYPE_BOTTOM_LEVEL,
  RI_ACCEL_STRUCTURE_TYPE_TOP_LEVEL
};

enum RIAccelStructureBuildBits_e {
  RI_ACCEL_BUILD_NONE = 0,
  RI_ACCEL_BUILD_ALLOW_UPDATE = 0x1,
  RI_ACCEL_BUILD_ALLOW_COMPACTION = 0x2,
  RI_ACCEL_BUILD_ALLOW_DATA_ACCESS = 0x4,
  RI_ACCEL_BUILD_PREFER_FAST_TRACE = 0x8,
  RI_ACCEL_BUILD_PREFER_FAST_BUILD = 0x10,
  RI_ACCEL_BUILD_MINIMIZE_MEMORY = 0x20
};

enum RIAccelGeometryType_e {
  RI_ACCEL_GEOMETRY_TYPE_TRIANGLES,
  RI_ACCEL_GEOMETRY_TYPE_AABBS
};

enum RIAccelGeometryBits_e {
  RI_ACCEL_GEOMETRY_NONE = 0,
  RI_ACCEL_GEOMETRY_OPAQUE = 0x1,
  RI_ACCEL_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION = 0x2
};

enum RIAccelInstanceBits_e {
  RI_ACCEL_INSTANCE_NONE = 0,
  RI_ACCEL_INSTANCE_TRIANGLE_CULL_DISABLE = 0x1,
  RI_ACCEL_INSTANCE_TRIANGLE_FLIP_FACING = 0x2,
  RI_ACCEL_INSTANCE_FORCE_OPAQUE = 0x4,
  RI_ACCEL_INSTANCE_FORCE_NON_OPAQUE = 0x8
};

// requires src and dst, both built with RI_ACCEL_BUILD_ALLOW_UPDATE
enum RIAccelBuildMode_e {
  RI_ACCEL_BUILD_MODE_BUILD,
  RI_ACCEL_BUILD_MODE_UPDATE
};

// matches VkAabbPositionsKHR layout
struct RIAccelAabb_s {
  RIAccelAabb_s() { memset(this, 0, sizeof(*this)); }
  float minX, minY, minZ;
  float maxX, maxY, maxZ;
};

//// callers fill a buffer of these and pass it as the TLAS instance buffer;
//// matches VkAccelerationStructureInstanceKHR layout (64 bytes)
//struct RIAccelInstance_s {
//  RIAccelInstance_s() { memset(this, 0, sizeof(*this)); }
//  float matrix[3][4];
//  uint32_t instanceCustomIndex : 24;
//  uint32_t mask : 8;
//  uint32_t shaderBindingTableRecordOffset : 24;
//  uint32_t flags : 8; // RIAccelInstanceBits_e
//  uint64_t
//      accelerationStructureDeviceAddress; // RIAccelStructure_s::getDeviceAddress
//};
//static_assert(
//    sizeof(struct RIAccelInstance_s) == 64,
//    "RIAccelInstance_s must match VkAccelerationStructureInstanceKHR layout");

struct RIAccelTrianglesDesc_s {
  struct RIBuffer_s *vertexBuffer;
  uint64_t vertexOffset;
  uint32_t vertexNum;
  uint16_t vertexStride;
  enum RI_Format_e vertexFormat;

  struct RIBuffer_s *indexBuffer; // optional, NULL = unindexed
  uint64_t indexOffset;
  uint32_t indexNum;
  enum RIIndexType_e indexType;

  struct RIBuffer_s
      *transformBuffer; // optional, points to RIAccelTransform_s entries
  uint64_t transformOffset;
};

struct RIAccelAabbsDesc_s {
  struct RIBuffer_s *buffer; // points to RIAccelAabb_s entries
  uint64_t offset;
  uint32_t num;
  uint32_t stride;
};

struct RIAccelGeometryDesc_s {
  RIAccelGeometryDesc_s() { memset(this, 0, sizeof(*this)); }
  enum RIAccelGeometryType_e type;
  uint32_t flags; // RIAccelGeometryBits_e
  union {
    struct RIAccelTrianglesDesc_s triangles;
    struct RIAccelAabbsDesc_s aabbs;
  };
};

enum RIColorWriteMask_e {
  RI_COLOR_WRITE_NONE = 0,
  RI_COLOR_WRITE_R = 0x1,
  RI_COLOR_WRITE_G = 0x2,
  RI_COLOR_WRITE_B = 0x4,
  RI_COLOR_WRITE_A = 0x8,

  RI_COLOR_WRITE_RGB = RI_COLOR_WRITE_R | RI_COLOR_WRITE_G | RI_COLOR_WRITE_B,

  RI_COLOR_WRITE_RGBA =
      RI_COLOR_WRITE_R | RI_COLOR_WRITE_G | RI_COLOR_WRITE_B | RI_COLOR_WRITE_A
};

// S0 - source color 0
// S1 - source color 1
// D - destination color
// C - blend constants, set by "CmdSetBlendConstants"
enum RIBlendFactor_e {          // RGB                               ALPHA
  RI_BLEND_ZERO,                // 0                                 0
  RI_BLEND_ONE,                 // 1                                 1
  RI_BLEND_SRC_COLOR,           // S0.r, S0.g, S0.b                  S0.a
  RI_BLEND_ONE_MINUS_SRC_COLOR, // 1 - S0.r, 1 - S0.g, 1 - S0.b      1 - S0.a
  RI_BLEND_DST_COLOR,           // D.r, D.g, D.b                     D.a
  RI_BLEND_ONE_MINUS_DST_COLOR, // 1 - D.r, 1 - D.g, 1 - D.b         1 - D.a
  RI_BLEND_SRC_ALPHA,           // S0.a                              S0.a
  RI_BLEND_ONE_MINUS_SRC_ALPHA, // 1 - S0.a                          1 - S0.a
  RI_BLEND_DST_ALPHA,           // D.a                               D.a
  RI_BLEND_ONE_MINUS_DST_ALPHA, // 1 - D.a                           1 - D.a
  RI_BLEND_CONSTANT_COLOR,      // C.r, C.g, C.b                     C.a
  RI_BLEND_ONE_MINUS_CONSTANT_COLOR, // 1 - C.r, 1 - C.g, 1 - C.b         1 -
                                     // C.a
  RI_BLEND_CONSTANT_ALPHA,           // C.a                               C.a
  RI_BLEND_ONE_MINUS_CONSTANT_ALPHA, // 1 - C.a                           1 -
                                     // C.a
  RI_BLEND_SRC_ALPHA_SATURATE,       // min(S0.a, 1 - D.a)                1
  RI_BLEND_SRC1_COLOR,               // S1.r, S1.g, S1.b                  S1.a
  RI_BLEND_ONE_MINUS_SRC1_COLOR, // 1 - S1.r, 1 - S1.g, 1 - S1.b      1 - S1.a
  RI_BLEND_SRC1_ALPHA,           // S1.a                              S1.a
  RI_BLEND_ONE_MINUS_SRC1_ALPHA  // 1 - S1.a                          1 - S1.a
};

enum RIWindowType_e {
  RI_WINDOW_UNKNOWN,
  RI_WINDOW_X11,
  RI_WINDOW_WIN32,
  RI_WINDOW_METAL,
  RI_WINDOW_WAYLAND
};

struct RIBuffer_s {
  RIBuffer_s() { memset(this, 0, sizeof(*this)); }

  void dispose(struct RIDevice_s *device);
  static struct RIBuffer_s VK_createFromVMA(struct RIDevice_s *device,
                                            VkBufferCreateInfo *vk,
                                            VmaAllocationCreateInfo *info);
  void setDebugObjectName(struct RIDevice_s *device, const char *name);
  uint64_t GetDeviceHandle(struct RIDevice_s *device);

  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      struct VmaAllocation_T *allocation;
      VkBuffer buffer;
    } vk;
#endif
  };
  void *mappedAddress;
};

struct RITexture_s {
  RITexture_s() { memset(this, 0, sizeof(*this)); }
  // True when no backing image was created (zeroed handle); the device
  // resolves which backend's handle to check.
  bool isEmpty(const struct RIDevice_s *device) const;
  // Destroys the image (and its VMA allocation when this texture owns one;
  // an allocation-less image, e.g. a swapchain bridge, is destroyed bare)
  // and nulls the handles.
  void dispose(struct RIDevice_s *device);
  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      VkImage image;
      struct VmaAllocation_T *allocation;
    } vk;
#endif
  };
};

struct RITextureView_s {
  RITextureView_s() { memset(this, 0, sizeof(*this)); }
  // Destroys the image view and zeroes the struct.
  void dispose(struct RIDevice_s *device);
  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      VkImageView image;
    } vk;
#endif
  };
};

enum RIDescriptorFlags_e {
  RI_VK_DESC_BEGIN = 0,
  RI_VK_DESC_OWN_SAMPLER = 0x1,   // owns the backing assets VKImage, VkBuffer
  RI_VK_DESC_OWN_IMAGE_VIEW = 0x2 // owns the backing sampler
};

struct DescriptorBindingID {
  DescriptorBindingID() { memset(this, 0, sizeof(*this)); }
  const char *name;
  hash_t hash;
  static DescriptorBindingID Create(const char *name) {
    struct DescriptorBindingID key;
    key.name = name;
    key.hash = hash_data(HASH_INITIAL_VALUE, name, strlen(name));
    return key;
  }
};

static inline struct DescriptorBindingID
CreateDescriptorBindingID(const char *name) {
  struct DescriptorBindingID key;
  key.name = name;
  key.hash = hash_data(HASH_INITIAL_VALUE, name, strlen(name));
  return key;
}

struct RIAccelStructure_s;

struct RIDescriptor_s {
  RIDescriptor_s() { memset(this, 0, sizeof(*this)); }

  // Call after configuring the descriptor: computes the identity cookie from
  // the backend handles (type-specific hash) and resolves any attached
  // buffer/accel-structure handle into the inline vk fields. A descriptor
  // with cookie == 0 reads as empty (isEmpty()).
  void finalize(struct RIDevice_s *device);
  // Convenience overload: attaches the acceleration structure, then
  // finalizes as above.
  void finalize(struct RIDevice_s *device, struct RIAccelStructure_s *as);

  // Destroys only the backend objects this descriptor owns (sampler /
  // image view, per the RI_VK_DESC_OWN_* flags); handles it merely
  // references are left alone. Leaves the descriptor zeroed so pooled slots
  // read as empty again (isEmpty() checks cookie == 0).
  void dispose(struct RIDevice_s *device);

  bool isEmpty() const { return cookie == 0; }

  // The backing image view for image-type descriptors; empty otherwise.
  struct RITextureView_s textureView() const;
  // unique id to mark the descriptor
  hash_t cookie;
  uint8_t flags;
  struct RIBuffer_s *buffer;
  struct RITexture_s *texture;
  struct RIAccelStructure_s *accelStructure;
  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      VkDescriptorType type;
      union {
        struct VkDescriptorImageInfo image;
        struct VkDescriptorBufferInfo buffer;
        VkAccelerationStructureKHR accelStructure;
      };
    } vk;
#endif
  };
};

struct RIAccelStructure_s {
  RIAccelStructure_s() { memset(this, 0, sizeof(*this)); }

  // Creates VkAccelerationStructureKHR backed by desc->storage at
  // desc->storageOffset. Caller must allocate desc->storage with
  // VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR and
  // VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, sized at least
  // desc->storageSize bytes.
  int init(struct RIDevice_s *device, const struct RIAccelStructureDesc_s *desc);
  void dispose(struct RIDevice_s *device);
  uint64_t getDeviceAddress(struct RIDevice_s *device) const;
  void setDebugObjectName(struct RIDevice_s *device, const char *name);

  enum RIAccelStructureType_e type;
  uint32_t flags; // RIAccelStructureBuildBits_e snapshot
  //uint64_t buildScratchSize;
  //uint64_t updateScratchSize;
  //uint64_t storageOffset;
  //struct RIBuffer_s storage; // caller-owned backing buffer
  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      VkAccelerationStructureKHR handle;
      VkDeviceAddress deviceAddress;
    } vk;
#endif
  };
};

// Deferred-destroy handle for the per-frame freelist: a by-value copy of the
// owning RI struct, drained with std::visit -> dispose(device) once the frame
// slot's ring fence has signaled (see RIBootstrap::BeginActiveSet).
using RIFreeHandle = std::variant<struct RIBuffer_s, struct RITexture_s,
                                  struct RITextureView_s, struct RIAccelStructure_s>;

struct RIAccelStructureDesc_s {
  RIAccelStructureDesc_s() { memset(this, 0, sizeof(*this)); }

  // Query backing-storage and scratch sizes before RIAccelStructure_s::init;
  // any out-pointer may be NULL. For BLAS, geometries describes the geometry
  // layout used to compute sizes; the actual vertex/index buffer addresses
  // don't need to be valid until the build command.
  void getMemoryReqs(struct RIDevice_s *device, uint64_t *outStorageSize,
                     uint64_t *outBuildScratchSize,
                     uint64_t *outUpdateScratchSize) const;

  enum RIAccelStructureType_e type;
  uint32_t flags; // RIAccelStructureBuildBits_e
  uint32_t geometryOrInstanceNum; // BLAS: geometry count, TLAS: max instance count
  const struct RIAccelGeometryDesc_s *geometries; // BLAS only; NULL for TLAS
  struct RIBuffer_s *storage;
  uint64_t storageOffset;
  uint64_t storageSize; // from getMemoryReqs
};

struct RIBuildBlasDesc_s {
  RIBuildBlasDesc_s() { memset(this, 0, sizeof(*this)); }
  struct RIAccelStructure_s *dst;
  struct RIAccelStructure_s *src; // NULL unless mode==UPDATE
  enum RIAccelBuildMode_e mode;
  const struct RIAccelGeometryDesc_s *geometries;
  uint32_t geometryNum;
  struct RIBuffer_s *scratchBuffer;
  uint64_t scratchOffset;

};

struct RIBuildTlasDesc_s {
  RIBuildTlasDesc_s() { memset(this, 0, sizeof(*this)); }
  struct RIAccelStructure_s *dst;
  struct RIAccelStructure_s *src; // NULL unless mode==UPDATE
  enum RIAccelBuildMode_e mode;
  uint32_t instanceNum;
  struct RIBuffer_s *instanceBuffer; // RIAccelInstance_s entries
  uint64_t instanceOffset;
  struct RIBuffer_s *scratchBuffer;
  uint64_t scratchOffset;
};

struct RIRect_s {
  RIRect_s() { memset(this, 0, sizeof(*this)); }
  int16_t x;
  int16_t y;
  int16_t width;
  int16_t height;
};

struct RIViewport_s {
  RIViewport_s() { memset(this, 0, sizeof(*this)); }
  float x;
  float y;
  float width;
  float height;
  float depthMin;
  float depthMax;
  bool originBottomLeft; // expects "isViewportOriginBottomLeftSupported"
};

struct RIPool_s {
  RIPool_s() { memset(this, 0, sizeof(*this)); }
  // Creates the command pool on the queue's family.
  void init(struct RIDevice_s *device, struct RIQueue_s *queue);
  // Resets every command buffer allocated from the pool.
  void reset(struct RIDevice_s *device);
  void dispose(struct RIDevice_s *device);
  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      VkQueue queue;
      VkCommandPool pool;
    } vk;
#endif
  };
};

struct RICmd_s {
  RICmd_s() { memset(this, 0, sizeof(*this)); }

  // Allocates the command buffer from the pool.
  void init(struct RIDevice_s *device, struct RIPool_s *pool);
  // Begins/ends recording (one-time-submit).
  void begin(struct RIDevice_s *device);
  void end(struct RIDevice_s *device);
  // Returns the command buffer to its pool and clears the handles.
  void dispose(struct RIDevice_s *device);

  // Leaf dispatch/draw command methods. Pipeline binding is done separately
  // (RIProgram::bindPipeline / bindComputePipeline / bindRayTracingPipeline);
  // these are the "go" calls that issue the actual work. Core Vulkan 1.0 —
  // no RIDevice_s needed because there's no fn-pointer indirection.
  void dispatch(uint32_t groupCountX, uint32_t groupCountY,
                uint32_t groupCountZ);
  void dispatchIndirect(struct RIBuffer_s *buffer, VkDeviceSize offset);
  void draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex,
            uint32_t firstInstance);
  void drawIndexed(uint32_t indexCount, uint32_t instanceCount,
                   uint32_t firstIndex, int32_t vertexOffset,
                   uint32_t firstInstance);
  void drawIndirect(struct RIBuffer_s *buffer, VkDeviceSize offset,
                    uint32_t drawCount, uint32_t stride);
  void drawIndexedIndirect(struct RIBuffer_s *buffer, VkDeviceSize offset,
                           uint32_t drawCount, uint32_t stride);

  // Acceleration-structure build commands; numDescs structures are submitted
  // in a single backend call. Caller-supplied scratchBuffer must include
  // VK_BUFFER_USAGE_STORAGE_BUFFER_BIT and
  // VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, with scratchOffset aligned to
  // minAccelerationStructureScratchOffsetAlignment. Input vertex/index/
  // instance buffers must include
  // VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR and
  // VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT.
  void buildBlas(struct RIDevice_s *device,
                 const struct RIBuildBlasDesc_s *descs, uint32_t numDescs);
  void buildTlas(struct RIDevice_s *device,
                 const struct RIBuildTlasDesc_s *descs, uint32_t numDescs);

  // Emit pipeline barriers from RI resource-state transitions (see
  // RIBarrier.h). All groups are batched into a single backend barrier
  // command (vkCmdPipelineBarrier2); any count may be zero. The template
  // parameters MemN/BufN/TexN are the stack capacities reserved for the
  // backend barrier scratch arrays (compile-time sized); a capacity of 0
  // moves that group to the heap instead, for dynamically sized batches.
  template <uint32_t MemN, uint32_t BufN, uint32_t TexN>
  void resourceBarrier(uint32_t memoryBarrierNum,
                       const struct RIMemoryBarrier_s *memoryBarriers,
                       uint32_t bufferBarrierNum,
                       const struct RIBufferBarrier_s *bufferBarriers,
                       uint32_t textureBarrierNum,
                       const struct RITextureBarrier_s *textureBarriers) {
    if (memoryBarrierNum + bufferBarrierNum + textureBarrierNum == 0)
      return;

#if (DEVICE_IMPL_VULKAN)
    ScratchBuffer<VkMemoryBarrier2, MemN> memScratch;
    ScratchBuffer<VkBufferMemoryBarrier2, BufN> bufScratch;
    ScratchBuffer<VkImageMemoryBarrier2, TexN> imgScratch;
    VkMemoryBarrier2 *mem = memScratch.get(memoryBarrierNum);
    VkBufferMemoryBarrier2 *buf = bufScratch.get(bufferBarrierNum);
    VkImageMemoryBarrier2 *img = imgScratch.get(textureBarrierNum);

    for (uint32_t i = 0; i < memoryBarrierNum; i++) {
      const struct RIMemoryBarrier_s &src = memoryBarriers[i];
      VkMemoryBarrier2 &dst = mem[i];
      dst = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
      dst.srcStageMask = ri_vk_RIStageBitsToVK(src.beforeStages, src.before);
      dst.srcAccessMask = ri_vk_RIResourceStateToAccess(src.before);
      dst.dstStageMask = ri_vk_RIStageBitsToVK(src.afterStages, src.after);
      dst.dstAccessMask = ri_vk_RIResourceStateToAccess(src.after);
    }

    for (uint32_t i = 0; i < bufferBarrierNum; i++) {
      const struct RIBufferBarrier_s &src = bufferBarriers[i];
      VkBufferMemoryBarrier2 &dst = buf[i];
      dst = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
      dst.srcStageMask = ri_vk_RIStageBitsToVK(src.beforeStages, src.before);
      dst.srcAccessMask = ri_vk_RIResourceStateToAccess(src.before);
      dst.dstStageMask = ri_vk_RIStageBitsToVK(src.afterStages, src.after);
      dst.dstAccessMask = ri_vk_RIResourceStateToAccess(src.after);
      dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      dst.buffer = src.buffer->vk.buffer;
      dst.offset = src.offset;
      dst.size = src.size ? src.size : VK_WHOLE_SIZE;
    }

    for (uint32_t i = 0; i < textureBarrierNum; i++) {
      const struct RITextureBarrier_s &src = textureBarriers[i];
      VkImageMemoryBarrier2 &dst = img[i];
      dst = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
      dst.srcStageMask = ri_vk_RIStageBitsToVK(src.beforeStages, src.before);
      dst.srcAccessMask = ri_vk_RIResourceStateToAccess(src.before);
      dst.dstStageMask = ri_vk_RIStageBitsToVK(src.afterStages, src.after);
      dst.dstAccessMask = ri_vk_RIResourceStateToAccess(src.after);
      dst.oldLayout = ri_vk_RIResourceStateToImageLayout(src.before);
      dst.newLayout = ri_vk_RIResourceStateToImageLayout(src.after);
      dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      dst.image = src.texture->vk.image;
      dst.subresourceRange = VkImageSubresourceRange{
          ri_vk_RIBarrierAspectToVK(src.aspect),
          src.baseMip,
          src.mipCount ? src.mipCount : VK_REMAINING_MIP_LEVELS,
          src.baseLayer,
          src.layerCount ? src.layerCount : VK_REMAINING_ARRAY_LAYERS,
      };
    }

    VkDependencyInfo dependencyInfo = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependencyInfo.memoryBarrierCount = memoryBarrierNum;
    dependencyInfo.pMemoryBarriers = mem;
    dependencyInfo.bufferMemoryBarrierCount = bufferBarrierNum;
    dependencyInfo.pBufferMemoryBarriers = buf;
    dependencyInfo.imageMemoryBarrierCount = textureBarrierNum;
    dependencyInfo.pImageMemoryBarriers = img;
    vkCmdPipelineBarrier2(vk.cmd, &dependencyInfo);
#endif
  }

  // Single-barrier conveniences.
  void memoryBarrier(const struct RIMemoryBarrier_s &barrier) {
    resourceBarrier<1, 0, 0>(1, &barrier, 0, NULL, 0, NULL);
  }
  void bufferBarrier(const struct RIBufferBarrier_s &barrier) {
    resourceBarrier<0, 1, 0>(0, NULL, 1, &barrier, 0, NULL);
  }
  void textureBarrier(const struct RITextureBarrier_s &barrier) {
    resourceBarrier<0, 0, 1>(0, NULL, 0, NULL, 1, &barrier);
  }
  // Fixed-capacity texture-batch convenience; N is the stack capacity.
  template <uint32_t N>
  void textureBarriers(uint32_t num, const struct RITextureBarrier_s *barriers) {
    resourceBarrier<0, 0, N>(0, NULL, 0, NULL, num, barriers);
  }

  // Bind a single index buffer. Takes an RIBuffer_s* (the RI abstraction)
  // rather than a backend handle so the same call site survives a future
  // DX12 backend.
  void bindIndexBuffer(struct RIBuffer_s *buffer, VkDeviceSize offset,
                       VkIndexType indexType);

  // Bind `count` vertex buffers. The template parameter N is only the stack
  // capacity reserved for the backend handle scratch array (compile-time
  // sized, no heap); `count` is the actual number bound and must be <= N.
  // `buffers` is a raw RIBuffer_s* array of length `count` (a null entry
  // binds nothing); `offsets` is a parallel byte-offset array. e.g. for a
  // fixed 5-stream layout where all 5 are live:
  // cmd->bindVertexBuffers<5>(0, 5, bufs). RIBuffer_s* keeps the call site
  // backend-agnostic for the planned DX12 path.
  template <uint32_t N>
  void bindVertexBuffers(uint32_t firstBinding, uint32_t count,
                         struct RIBuffer_s *const *buffers,
                         const VkDeviceSize *offsets) {
    assert(count <= N);
#if (DEVICE_IMPL_VULKAN)
    VkBuffer vkBufs[N];
    for (uint32_t i = 0; i < count; ++i)
      vkBufs[i] = buffers[i] ? buffers[i]->vk.buffer : VK_NULL_HANDLE;
    vkCmdBindVertexBuffers(vk.cmd, firstBinding, count, vkBufs, offsets);
#endif
  }

  // Convenience overload binding `count` streams at offset 0.
  template <uint32_t N>
  void bindVertexBuffers(uint32_t firstBinding, uint32_t count,
                         struct RIBuffer_s *const *buffers) {
    const VkDeviceSize offsets[N] = {};
    bindVertexBuffers<N>(firstBinding, count, buffers, offsets);
  }

  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      VkCommandPool pool;
      VkCommandBuffer cmd;
    } vk;
#endif
  };
};

// Compile-time maximums; runtime poolCount/cmdPerPool are passed to
// RICommandRingBuffer_s::init.
#define RI_COMMAND_RING_POOL_COUNT 8
#define RI_COMMAND_RING_CMD_PER_POOL 32

struct RICommandRingElement_s {
  RICommandRingElement_s() { memset(this, 0, sizeof(*this)); }
  // Blocks until the element's fence signals (no-op without sync primitives).
  void wait(struct RIDevice_s *device);
  struct RICmd_s *cmds;
  uint32_t numCmds;
  struct RIPool_s *pool;
  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      VkSemaphore semaphore;
      VkFence fence;
    } vk;
#endif
  };
};

template <uint32_t MaxPoolCount = RI_COMMAND_RING_POOL_COUNT,
          uint32_t CmdPerPool = RI_COMMAND_RING_CMD_PER_POOL>
struct RICommandRingBuffer_s {
  RICommandRingBuffer_s() { memset(this, 0, sizeof(*this)); }
  static constexpr uint32_t MAX_POOL_COUNT = MaxPoolCount;
  static constexpr uint32_t CMD_PER_POOL = CmdPerPool;

  // Defined at the bottom of this header once RIDevice_s is complete
  // (same precedent as RISwapchain_s::dispose).
  void init(struct RIDevice_s *device, struct RIQueue_s *queue,
            uint32_t poolCount, uint32_t cmdPerPool, bool syncPrimitives);
  void dispose(struct RIDevice_s *device);
  // Rotates to the next pool and rewinds the cmd/fence cursors.
  void advance();
  // Claims numCmds command buffers plus a fence slot from the current pool
  // and advances the cursors.
  struct RICommandRingElement_s acquire(struct RIDevice_s *device,
                                        uint32_t numCmds);

  uint32_t poolIndex;
  uint32_t cmdIndex;
  uint32_t fenceIndex;

  uint32_t poolCount;
  uint32_t cmdPerPool;
  bool syncPrimitive;

  struct RIPool_s pools[MaxPoolCount];
  struct RICmd_s cmds[MaxPoolCount][CmdPerPool];

  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      VkFence fences[MaxPoolCount][CmdPerPool];
      VkSemaphore semaphores[MaxPoolCount][CmdPerPool];
    } vk;
#endif
  };
};

struct RIQueue_s {
  RIQueue_s() { memset(this, 0, sizeof(*this)); }
  void waitIdle(struct RIDevice_s *device);
  uint32_t getFlags(const struct RIRenderer_s *renderer) const {
#if (DEVICE_IMPL_VULKAN)
    return (vk.queueFlags & VK_QUEUE_GRAPHICS_BIT ? RI_QUEUE_GRAPHICS_BIT : 0) |
           (vk.queueFlags & VK_QUEUE_COMPUTE_BIT ? RI_QUEUE_COMPUTE_BIT : 0) |
           (vk.queueFlags & VK_QUEUE_TRANSFER_BIT ? RI_QUEUE_TRANSFER_BIT : 0) |
           (vk.queueFlags & VK_QUEUE_SPARSE_BINDING_BIT ? RI_QUEUE_SPARSE_BINDING_BIT : 0) |
           (vk.queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR ? RI_QUEUE_VIDEO_DECODE_BIT : 0) |
           (vk.queueFlags & VK_QUEUE_VIDEO_ENCODE_BIT_KHR ? RI_QUEUE_VIDEO_ENCODE_BIT : 0) |
           (vk.queueFlags & VK_QUEUE_PROTECTED_BIT ? RI_QUEUE_PROTECTED_BIT : 0) |
           (vk.queueFlags & VK_QUEUE_OPTICAL_FLOW_BIT_NV ? RI_QUEUE_OPTICAL_FLOW_BIT_NV : 0);
#endif
    return 0;
  }
  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      VkQueueFlags queueFlags;
      uint16_t queueFamilyIdx;
      uint16_t slotIdx;
      VkQueue queue;
    } vk;
#endif
  };
};

template <uint32_t MaxImageCount = RI_MAX_SWAPCHAIN_IMAGES>
struct RISwapchain_s {
  RISwapchain_s() { memset(this, 0, sizeof(*this)); }
  static constexpr uint32_t MAX_IMAGE_COUNT = MaxImageCount;

  // Destroys the per-image semaphores, the swapchain and the surface (the
  // swapchain images themselves are owned by the swapchain). Defined at the
  // bottom of this header once RIDevice_s/RIRenderer_s are complete.
  void dispose(struct RIDevice_s *device);

  struct RIQueue_s *presentQueue;
  uint16_t imageCount;
  uint16_t width;
  uint16_t height;
  uint32_t format; // RI_Format_e
  struct RITexture_s textures[MaxImageCount];
  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      uint32_t frameIndex;
      uint32_t textureIndex;
      uint64_t presentID;
      VkSwapchainKHR swapchain;
      VkSurfaceKHR surface;
      VkImage images[MaxImageCount];
      VkSemaphore imageAcquireSem[MaxImageCount];
      VkSemaphore finishSem[MaxImageCount];
    } vk;
#endif
  };
};

struct RIRenderer_s {
  RIRenderer_s() { memset(this, 0, sizeof(*this)); }
  // Loads the loader (volk), creates the instance and optional debug
  // messenger. Returns RI_SUCCESS/RI_FAIL.
  int init(const struct RIBackendInit_s *init);
  int enumerateAdapters(struct RIPhysicalAdapter_s *adapters,
                        uint32_t *numAdapters);
  // Destroys the debug messenger and the instance. Instance-level — takes no
  // device (the device is normally gone by the time this runs); loader
  // teardown (volkFinalize) stays with ShutdownRIRenderer.
  void dispose();
  uint8_t api; // RIDeviceAPI_e
  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      uint32_t apiVersion;
      VkInstance instance;
      VkDebugUtilsMessengerEXT debugMessageUtils;
    } vk;
#endif
  };
};

struct RIBackendInit_s {
  uint8_t api; // RIDeviceAPI_e
  const char *applicationName;
  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      uint32_t enableValidationLayer : 1;
      size_t numFilterLayers;
      // Was `const char* filterLayers[];` — a C99 flexible-array member.
      // MSVC rejects FAMs inside an anonymous union when the enclosing
      // struct is stack-allocated (error C2466 in Graphics.cpp::Init).
      // No call site currently writes to this field; if filter-layer
      // support is added later, allocate the array externally and point
      // here.
      const char *const *filterLayers;
    } vk;
#endif
#if (DEVICE_IMPL_MTL)
#endif
  };
};

struct RIPhysicalAdapter_s {
  RIPhysicalAdapter_s() { memset(this, 0, sizeof(*this)); }
  char name[256];
  uint64_t luid;
  uint64_t videoMemorySize;
  uint64_t systemMemorySize;
  uint32_t deviceId;
  uint8_t vendor;      // RIVendor_e
  uint8_t presetLevel; // RIPresetLevel_e
  uint8_t type;        // RIAdapterType_e

  // Viewports
  uint32_t viewportMaxNum;
  int32_t viewportBoundsRange[2];

  // Attachments
  uint16_t attachmentMaxDim;
  uint16_t attachmentLayerMaxNum;
  uint16_t colorAttachmentMaxNum;

  // Multi-sampling
  uint8_t colorSampleMaxNum;
  uint8_t depthSampleMaxNum;
  uint8_t stencilSampleMaxNum;
  uint8_t zeroAttachmentsSampleMaxNum;
  uint8_t textureColorSampleMaxNum;
  uint8_t textureIntegerSampleMaxNum;
  uint8_t textureDepthSampleMaxNum;
  uint8_t textureStencilSampleMaxNum;
  uint8_t storageTextureSampleMaxNum;

  // Resource dimensions
  uint16_t texture1DMaxDim;
  uint16_t texture2DMaxDim;
  uint16_t texture3DMaxDim;
  uint16_t textureArrayLayerMaxNum;
  uint32_t typedBufferMaxDim;

  // Memory
  uint64_t deviceUploadHeapSize; // ReBAR
  uint32_t memoryAllocationMaxNum;
  uint32_t samplerAllocationMaxNum;
  uint32_t constantBufferMaxRange;
  uint32_t storageBufferMaxRange;
  uint32_t bufferTextureGranularity;
  uint64_t bufferMaxSize;

  // Memory alignment
  uint32_t uploadBufferTextureRowAlignment;
  uint32_t uploadBufferOffsetAlignment;
  uint32_t bufferShaderResourceOffsetAlignment;
  uint32_t constantBufferOffsetAlignment;
  // uint32_t scratchBufferOffsetAlignment;
  // uint32_t shaderBindingTableAlignment;

  // Pipeline layout
  // D3D12 only: rootConstantSize + descriptorSetNum * 4 + rootDescriptorNum * 8
  // <= 256 (see "FitPipelineLayoutSettingsIntoDeviceLimits")
  uint32_t pipelineLayoutDescriptorSetMaxNum;
  uint32_t pipelineLayoutRootConstantMaxSize;
  uint32_t pipelineLayoutRootDescriptorMaxNum;

  // Descriptor set
  uint32_t descriptorSetSamplerMaxNum;
  uint32_t descriptorSetConstantBufferMaxNum;
  uint32_t descriptorSetStorageBufferMaxNum;
  uint32_t descriptorSetTextureMaxNum;
  uint32_t descriptorSetStorageTextureMaxNum;

  // Shader resources
  uint32_t perStageDescriptorSamplerMaxNum;
  uint32_t perStageDescriptorConstantBufferMaxNum;
  uint32_t perStageDescriptorStorageBufferMaxNum;
  uint32_t perStageDescriptorTextureMaxNum;
  uint32_t perStageDescriptorStorageTextureMaxNum;
  uint32_t perStageResourceMaxNum;

  // Vertex shader
  uint32_t vertexShaderAttributeMaxNum;
  uint32_t vertexShaderStreamMaxNum;
  uint32_t vertexShaderOutputComponentMaxNum;

  // Tessellation shaders
  float tessControlShaderGenerationMaxLevel;
  uint32_t tessControlShaderPatchPointMaxNum;
  uint32_t tessControlShaderPerVertexInputComponentMaxNum;
  uint32_t tessControlShaderPerVertexOutputComponentMaxNum;
  uint32_t tessControlShaderPerPatchOutputComponentMaxNum;
  uint32_t tessControlShaderTotalOutputComponentMaxNum;
  uint32_t tessEvaluationShaderInputComponentMaxNum;
  uint32_t tessEvaluationShaderOutputComponentMaxNum;

  // Geometry shader
  uint32_t geometryShaderInvocationMaxNum;
  uint32_t geometryShaderInputComponentMaxNum;
  uint32_t geometryShaderOutputComponentMaxNum;
  uint32_t geometryShaderOutputVertexMaxNum;
  uint32_t geometryShaderTotalOutputComponentMaxNum;

  // Fragment shader
  uint32_t fragmentShaderInputComponentMaxNum;
  uint32_t fragmentShaderOutputAttachmentMaxNum;
  uint32_t fragmentShaderDualSourceAttachmentMaxNum;

  // Compute shader
  uint32_t computeShaderSharedMemoryMaxSize;
  uint32_t computeShaderWorkGroupMaxNum[3];
  uint32_t computeShaderWorkGroupInvocationMaxNum;
  uint32_t computeShaderWorkGroupMaxDim[3];

  // Ray tracing
  uint32_t rayTracingShaderGroupIdentifierSize;
  uint32_t rayTracingShaderTableMaxStride;
  uint32_t rayTracingShaderRecursionMaxDepth;
  uint32_t rayTracingGeometryObjectMaxNum;
  uint32_t accelerationStructureScratchOffsetAlignment;

  // Mesh shaders
  // uint32_t meshControlSharedMemoryMaxSize;
  // uint32_t meshControlWorkGroupInvocationMaxNum;
  // uint32_t meshControlPayloadMaxSize;
  // uint32_t meshEvaluationOutputVerticesMaxNum;
  // uint32_t meshEvaluationOutputPrimitiveMaxNum;
  // uint32_t meshEvaluationOutputComponentMaxNum;
  // uint32_t meshEvaluationSharedMemoryMaxSize;
  // uint32_t meshEvaluationWorkGroupInvocationMaxNum;

  // Precision bits
  uint32_t viewportPrecisionBits;
  uint32_t subPixelPrecisionBits;
  uint32_t subTexelPrecisionBits;
  uint32_t mipmapPrecisionBits;

  // Other
  uint64_t timestampFrequencyHz;
  uint32_t drawIndirectMaxNum;
  float samplerLodBiasMin;
  float samplerLodBiasMax;
  float samplerAnisotropyMax;
  int32_t texelOffsetMin;
  uint32_t texelOffsetMax;
  int32_t texelGatherOffsetMin;
  uint32_t texelGatherOffsetMax;
  uint32_t clipDistanceMaxNum;
  uint32_t cullDistanceMaxNum;
  uint32_t combinedClipAndCullDistanceMaxNum;
  // uint8_t shadingRateAttachmentTileSize;
  // uint8_t shaderModel; // major * 10 + minor

  // Tiers (0 - unsupported)
  // 1 - 1/2 pixel uncertainty region and does not support post-snap degenerates
  // 2 - reduces the maximum uncertainty region to 1/256 and requires post-snap
  // degenerates not be culled 3 - maintains a maximum 1/256 uncertainty region
  // and adds support for inner input coverage, aka "SV_InnerCoverage"
  // uint8_t conservativeRasterTier;

  // 1 - a single sample pattern can be specified to repeat for every pixel
  // ("locationNum / sampleNum" must be 1 in "CmdSetSampleLocations") 2 - four
  // separate sample patterns can be specified for each pixel in a 2x2 grid
  // ("locationNum / sampleNum" can be up to 4 in "CmdSetSampleLocations")
  // uint8_t sampleLocationsTier;

  // 1 - DXR 1.0: full raytracing functionality, except features below
  // 2 - DXR 1.1: adds - ray query, "CmdDispatchRaysIndirect", "GeometryIndex()"
  // intrinsic, additional ray flags & vertex formats
  uint8_t rayTracingTier;

  // 1 - shading rate can be specified only per draw
  // 2 - adds: per primitive shading rate, per "shadingRateAttachmentTileSize"
  // shading rate, combiners, "SV_ShadingRate" support
  // uint8_t shadingRateTier;

  // 1 - unbound arrays with dynamic indexing
  // 2 - D3D12 dynamic resources:
  // https://microsoft.github.io/DirectX-Specs/d3d/HLSL_SM_6_6_DynamicResources.html
  uint8_t bindlessTier;

  // Features
  uint32_t isTextureFilterMinMaxSupported : 1;
  uint32_t isLogicFuncSupported : 1;
  uint32_t isDepthBoundsTestSupported : 1;
  uint32_t isDrawIndirectCountSupported : 1;
  uint32_t isIndependentFrontAndBackStencilReferenceAndMasksSupported : 1;
  // uint32_t isLineSmoothingSupported : 1;
  uint32_t isCopyQueueTimestampSupported : 1;
  // uint32_t isMeshShaderPipelineStatsSupported : 1;
  uint32_t isEnchancedBarrierSupported : 1; // aka - can "Layout" be ignored?
  uint32_t isMemoryTier2Supported
      : 1; // a memory object can support resources from all 3 categories
           // (buffers, attachments, all other textures)
  uint32_t isDynamicDepthBiasSupported : 1;
  // uint32_t isAdditionalShadingRatesSupported : 1;
  uint32_t isViewportOriginBottomLeftSupported : 1;
  uint32_t isRegionResolveSupported : 1;

  // Shader features
  uint32_t isShaderNativeI16Supported : 1;
  uint32_t isShaderNativeF16Supported : 1;
  uint32_t isShaderNativeI32Supported : 1;
  uint32_t isShaderNativeF32Supported : 1;
  uint32_t isShaderNativeI64Supported : 1;
  uint32_t isShaderNativeF64Supported : 1;
  uint32_t isShaderAtomicsI16Supported : 1;
  // uint32_t isShaderAtomicsF16Supported : 1;
  uint32_t isShaderAtomicsI32Supported : 1;
  // uint32_t isShaderAtomicsF32Supported : 1;
  uint32_t isShaderAtomicsI64Supported : 1;
  // uint32_t isShaderAtomicsF64Supported : 1;

  // Emulated features
  uint32_t isDrawParametersEmulationEnabled : 1;

  //// Extensions (unexposed are always supported)
  // uint32_t isSwapChainSupported : 1;	// swapchain Support
  uint32_t isRayQuerySupported
      : 1; // VK_KHR_ray_query / DXR 1.1 inline ray queries
  // uint32_t isMeshShaderSupported : 1; // meshshader support

  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      uint32_t apiVersion;
      VkPhysicalDevice physicalDevice;

      uint32_t isSwapChainSupported : 1; // swapchain Support
      uint32_t isBufferDeviceAddressSupported : 1;
      uint32_t isAMDDeviceCoherentMemorySupported : 1;
      uint32_t isPresentIDSupported : 1;
      // uint32_t YCbCrExtension : 1;
      // uint32_t FillModeNonSolid : 1;
      // uint32_t KHRRayQueryExtension : 1;
      // uint32_t AMDGCNShaderExtension : 1;
      // uint32_t AMDDrawIndirectCountExtension : 1;
      // uint32_t AMDShaderInfoExtension : 1;
      // uint32_t DescriptorIndexingExtension : 1;
      // uint32_t DynamicRenderingExtension : 1;
      // uint32_t ShaderSampledImageArrayDynamicIndexingSupported : 1;
      // uint32_t BufferDeviceAddressSupported : 1;
      // uint32_t DrawIndirectCountExtension : 1;
      // uint32_t DedicatedAllocationExtension : 1;
      // uint32_t DebugMarkerExtension : 1;
      // uint32_t MemoryReq2Extension : 1;
      // uint32_t FragmentShaderInterlockExtension : 1;
      // uint32_t BufferDeviceAddressExtension : 1;
      uint32_t accelerationStructureExtension : 1;
      uint32_t rayTracingPipelineExtension : 1;
      uint32_t rayQueryExtension : 1;
      // uint32_t ShaderAtomicInt64Extension : 1;
      // uint32_t BufferDeviceAddressFeature : 1;
      // uint32_t ShaderFloatControlsExtension : 1;
      // uint32_t Spirv14Extension : 1;
      uint32_t deferredHostOperationsExtension : 1;
      // uint32_t DeviceFaultExtension : 1;
      // uint32_t DeviceFaultSupported : 1;
      // uint32_t ASTCDecodeModeExtension : 1;
      // uint32_t DeviceMemoryReportExtension : 1;
      // uint32_t AMDBufferMarkerExtension : 1;
      // uint32_t AMDDeviceCoherentMemoryExtension : 1;
      // uint32_t AMDDeviceCoherentMemorySupported : 1;
    } vk;
#endif
#if (DEVICE_IMPL_MTL)
    struct {

    } mtl;
#endif
  };
};

struct RIDevice_s {
  RIDevice_s() { memset(this, 0, sizeof(*this)); }
  // Creates the logical device, queues and VMA allocator on the adapter
  // selected in init->physicalAdapter (RIDeviceDesc_s lives in RIRenderer.h).
  int init(struct RIRenderer_s *renderer, struct RIDeviceDesc_s *init);
  void dispose();
  struct RIPhysicalAdapter_s physicalAdapter;
  struct RIRenderer_s *renderer;
  struct RIQueue_s queues[RI_QUEUE_LEN];
  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      uint32_t maintenance5Features : 1;
      uint32_t conservaitveRasterTier : 1;
      uint32_t swapchainMutableFormat : 1;
      uint32_t memoryBudget : 1;
      VkDevice device;
      VmaAllocator vmaAllocator;
    } vk;
#endif
#if (DEVICE_IMPL_MTL)
#endif
  };
};

static inline bool IsRICmdValid(struct RIRenderer_s *renderer,
                                struct RICmd_s *cmd) {
#if (DEVICE_IMPL_VULKAN)
  return cmd->vk.pool && cmd->vk.cmd;
#endif
  return false;
}

static inline bool IsRIBufferValid(struct RIRenderer_s *renderer,
                                   const struct RIBuffer_s *handle) {
#if (DEVICE_IMPL_VULKAN)
  return handle && handle->vk.buffer != NULL;
#endif
  return false;
}

static inline bool IsRITextureValid(struct RIRenderer_s *renderer,
                                    const struct RITexture_s *handle) {
#if (DEVICE_IMPL_VULKAN)
  return handle && handle->vk.image != NULL;
#endif
  return false;
}

inline bool RITexture_s::isEmpty(const struct RIDevice_s *device) const {
  switch (device->renderer->api) {
#if (DEVICE_IMPL_VULKAN)
  case RI_DEVICE_API_VK:
    return vk.image == VK_NULL_HANDLE;
#endif
  default:
    assert(false && "unhandled backend");
    return true;
  }
}

template <uint32_t MaxImageCount>
inline void RISwapchain_s<MaxImageCount>::dispose(struct RIDevice_s *device) {
#if (DEVICE_IMPL_VULKAN)
  for (uint32_t p = 0; p < MaxImageCount; p++) {
    if (vk.imageAcquireSem[p])
      vkDestroySemaphore(device->vk.device, vk.imageAcquireSem[p], NULL);
    if (vk.finishSem[p])
      vkDestroySemaphore(device->vk.device, vk.finishSem[p], NULL);
  }
  if (vk.swapchain)
    vkDestroySwapchainKHR(device->vk.device, vk.swapchain, NULL);
  if (vk.surface)
    vkDestroySurfaceKHR(device->renderer->vk.instance, vk.surface, NULL);
#endif
}

template <uint32_t MaxPoolCount, uint32_t CmdPerPool>
inline void RICommandRingBuffer_s<MaxPoolCount, CmdPerPool>::init(
    struct RIDevice_s *device, struct RIQueue_s *queue, uint32_t poolCount,
    uint32_t cmdPerPool, bool syncPrimitives) {
  assert(poolCount > 0 && poolCount <= MaxPoolCount);
  assert(cmdPerPool > 0 && cmdPerPool <= CmdPerPool);
  memset(this, 0, sizeof(*this));
  this->poolCount = poolCount;
  this->cmdPerPool = cmdPerPool;
  this->syncPrimitive = syncPrimitives;

  poolIndex = 0;
  cmdIndex = 0;
  fenceIndex = 0;

  for (uint32_t poolIdx = 0; poolIdx < poolCount; poolIdx++) {
    pools[poolIdx].init(device, queue);
    for (uint32_t cmdIdx = 0; cmdIdx < cmdPerPool; cmdIdx++) {
      cmds[poolIdx][cmdIdx].init(device, &pools[poolIdx]);
#if (DEVICE_IMPL_VULKAN)
      if (syncPrimitives) {
        VkSemaphoreCreateInfo semaphoreCreateInfo = {
            VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_WrapResult(vkCreateSemaphore(device->vk.device, &semaphoreCreateInfo,
                                        NULL, &vk.semaphores[poolIdx][cmdIdx]));

        VkFenceCreateInfo fenceCreateInfo = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fenceCreateInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_WrapResult(vkCreateFence(device->vk.device, &fenceCreateInfo, NULL,
                                    &vk.fences[poolIdx][cmdIdx]));
      }
#endif
    }
  }
}

template <uint32_t MaxPoolCount, uint32_t CmdPerPool>
inline void
RICommandRingBuffer_s<MaxPoolCount, CmdPerPool>::dispose(struct RIDevice_s *device) {
  for (uint32_t poolIdx = 0; poolIdx < poolCount; poolIdx++) {
    for (uint32_t cmdIdx = 0; cmdIdx < cmdPerPool; cmdIdx++) {
      cmds[poolIdx][cmdIdx].dispose(device);
#if (DEVICE_IMPL_VULKAN)
      if (syncPrimitive) {
        vkDestroySemaphore(device->vk.device, vk.semaphores[poolIdx][cmdIdx], NULL);
        vkDestroyFence(device->vk.device, vk.fences[poolIdx][cmdIdx], NULL);
      }
#endif
    }
    pools[poolIdx].dispose(device);
  }
}

template <uint32_t MaxPoolCount, uint32_t CmdPerPool>
inline void RICommandRingBuffer_s<MaxPoolCount, CmdPerPool>::advance() {
  poolIndex = (poolIndex + 1) % poolCount;
  cmdIndex = 0;
  fenceIndex = 0;
}

template <uint32_t MaxPoolCount, uint32_t CmdPerPool>
inline struct RICommandRingElement_s
RICommandRingBuffer_s<MaxPoolCount, CmdPerPool>::acquire(
    struct RIDevice_s *device, uint32_t numCmds) {
  struct RICommandRingElement_s result;
  memset(&result, 0, sizeof(struct RICommandRingElement_s));

  assert(numCmds <= cmdPerPool);
  assert(numCmds + cmdIndex <= cmdPerPool);

  result.cmds = &cmds[poolIndex][cmdIndex];
  result.numCmds = numCmds;
  result.pool = &pools[poolIndex];
#if (DEVICE_IMPL_VULKAN)
  if (syncPrimitive) {
    result.vk.semaphore = vk.semaphores[poolIndex][fenceIndex];
    result.vk.fence = vk.fences[poolIndex][fenceIndex];
  }
#endif

  fenceIndex += 1;
  cmdIndex += numCmds;

  return result;
}

#endif
