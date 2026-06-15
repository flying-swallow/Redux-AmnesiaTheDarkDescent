
#ifndef RI_TYPES_H
#define RI_TYPES_H

#include "graphics/RIBarrier.h"
#include "graphics/RIBuffer.h"
#include "graphics/RIDefines.h"
#include "graphics/RIFormat.h"
#include "graphics/RITexture.h"
#include "graphics/RITextureView.h"
#include <atomic>
#include <cassert>
#include <cstring>
#include <optional>

#include "graphics/RIPreamble.h"

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

// Backend-neutral descriptor type (RIDescriptor::type). Mapped to VkDescriptorType
// at bind via ri_vk_BindlessDescriptorType. The engine uses separate sampled
// images + samplers (no combined-image-sampler).
enum RIDescriptorType_e {
  RI_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
  RI_DESCRIPTOR_TYPE_STORAGE_IMAGE,
  RI_DESCRIPTOR_TYPE_SAMPLER,
  RI_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
  RI_DESCRIPTOR_TYPE_STORAGE_BUFFER,
  RI_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE,
};

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
struct RIAccelAabb {
  RIAccelAabb() { memset(this, 0, sizeof(*this)); }
  float minX, minY, minZ;
  float maxX, maxY, maxZ;
};

struct RIAccelTrianglesDesc {
  struct RIBuffer *vertexBuffer;
  uint64_t vertexOffset;
  uint32_t vertexNum;
  uint16_t vertexStride;
  enum RI_Format_e vertexFormat;

  struct RIBuffer *indexBuffer; // optional, NULL = unindexed
  uint64_t indexOffset;
  uint32_t indexNum;
  enum RIIndexType_e indexType;

  struct RIBuffer
      *transformBuffer; // optional, points to RIAccelTransform entries
  uint64_t transformOffset;
};

struct RIAccelAabbsDesc {
  struct RIBuffer *buffer; // points to RIAccelAabb entries
  uint64_t offset;
  uint32_t num;
  uint32_t stride;
};

struct RIAccelGeometryDesc {
  RIAccelGeometryDesc() { memset(this, 0, sizeof(*this)); }
  enum RIAccelGeometryType_e type;
  uint32_t flags; // RIAccelGeometryBits_e
  union {
    struct RIAccelTrianglesDesc triangles;
    struct RIAccelAabbsDesc aabbs;
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

// Intrusive, atomically reference-counted shared owner of a raw RI handle.
// `T` must expose `void dispose(RIDevice*)` (RITexture / RITextureView /
// RIBuffer / RISampler / RIAccelStructure). The last reference to drop disposes
// the handle IMMEDIATELY — callers are responsible for GPU-safety (e.g. parking
// the owner in FrameDeferral) where the resource may still be in flight.
// `renderer` + `device` are carried explicitly (flat, no RIDevice::renderer
// back-pointer traversal).
template<typename T>
class RISharedPointer {
public:
  struct RIInternal {
    RIDevice* device;
    std::atomic<unsigned int> references;
    T value;
  };

  RISharedPointer() = default;

  // Adopts `value`; starts the reference count at 1.
  RISharedPointer(RIDevice* device, T value)
      : m_internal(new RIInternal{device, 1, value}) {}

  RISharedPointer(const RISharedPointer& other)
      : m_internal(other.m_internal) {
    if (m_internal)
      m_internal->references.fetch_add(1, std::memory_order_relaxed);
  }
  RISharedPointer& operator=(const RISharedPointer& other) {
    if (m_internal == other.m_internal) // same internal (incl. self) — alias safe
      return *this;
    reset();
    m_internal = other.m_internal;
    if (m_internal)
      m_internal->references.fetch_add(1, std::memory_order_relaxed);
    return *this;
  }

  RISharedPointer(RISharedPointer&& other) noexcept
      : m_internal(other.m_internal) {
    other.m_internal = nullptr;
  }
  RISharedPointer& operator=(RISharedPointer&& other) noexcept {
    if (this == &other)
      return *this;
    reset();
    m_internal = other.m_internal;
    other.m_internal = nullptr;
    return *this;
  }

  ~RISharedPointer() { reset(); }

  // Detach without disposing: hands ownership of one reference back to the
  // caller. Like std::unique_ptr::release.
  T* Release() {
    T* h = m_internal ? &m_internal->value : nullptr;
    m_internal = nullptr;
    return h;
  }

  T* Get() const { return m_internal ? &m_internal->value : nullptr; }
  T* operator->() const { return &m_internal->value; }
  T& operator*() const { return m_internal->value; }
  explicit operator bool() const { return m_internal != nullptr; }
  bool IsValid() const { return m_internal != nullptr; }
  // True when there is no owned handle, or the owned handle is an uncreated
  // (empty) RI resource. Lets a deferral queue skip parking null handles.
  bool isEmpty() const {
    return !m_internal || m_internal->value.isEmpty();
  }

private:
  // Drop one reference; dispose the handle and free the control block once the
  // count hits zero.
  void reset() {
    if (m_internal &&
        m_internal->references.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      m_internal->value.dispose(m_internal->device);
      delete m_internal;
    }
    m_internal = nullptr;
  }

  RIInternal* m_internal = nullptr;
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

struct RIAccelStructure;

// Owned backend sampler object. The only descriptor-referenced resource that
// owns a backend handle; created/cached once (RIBootstrap filter cache) and
// referenced by RIDescriptor::sampler. Freed via dispose().
struct RISampler {
  RISampler() { memset(this, 0, sizeof(*this)); }
  void dispose(struct RIDevice *device);
  bool isEmpty() const;
  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      VkSampler sampler;
    } vk;
#endif
#if (DEVICE_IMPL_MTL)
    struct {
      MTL::SamplerState *sampler;
    } mtl;
#endif
  };
  hash_t cookie;
};

struct RIDescriptor {
  RIDescriptor() { memset(this, 0, sizeof(*this)); }

  // Backend-neutral descriptor builders: reference the RI object + set the
  // binding params. The descriptor `cookie` (descriptor-set cache key) is
  // DERIVED from the referenced resource's own cookie folded with the binding
  // parameters (descriptor type; buffer offset/range) — callers no longer pass
  // one. A resource with cookie == 0 (uncreated) yields an empty descriptor, so
  // isEmpty() still holds. `state` selects the VK image layout. The `device`
  // param is unused (no resolution here) but kept for call-site stability.
  static RIDescriptor uniformBuffer(struct RIDevice *device,
                                    struct RIBuffer *buffer, uint64_t offset,
                                    uint64_t range);
  static RIDescriptor storageBuffer(struct RIDevice *device,
                                    struct RIBuffer *buffer, uint64_t offset,
                                    uint64_t range);
  static RIDescriptor sampledImage(struct RIDevice *device,
                                   struct RITextureView *view,
                                   enum RIResourceState_e state =
                                       RI_RESOURCE_STATE_SHADER_RESOURCE);
  static RIDescriptor storageImage(struct RIDevice *device,
                                   struct RITextureView *view);
  static RIDescriptor accelerationStructure(struct RIDevice *device,
                                            struct RIAccelStructure *as);
  static RIDescriptor sampler(struct RIDevice *device,
                              struct RISampler *sampler);

  bool isEmpty() const { return cookie == 0; }

  // Backend handle accessors — read the resolved handle stored inline at build
  // time (the builders resolve while the referenced RI object is still alive, so
  // a descriptor never depends on that object outliving it).
#if (DEVICE_IMPL_VULKAN)
  VkImageView vkImageView() const;
  VkBuffer vkBuffer() const;
  VkSampler vkSampler() const;
  VkAccelerationStructureKHR vkAccel() const;
  VkImageLayout vkLayout() const;
#endif

  // unique id / descriptor-set cache key (0 == empty)
  hash_t cookie;
  // Backend-neutral descriptor type (RIDescriptorType_e).
  uint8_t type;
  // Resolved backend descriptor info, filled in by the builders. Exactly one
  // union member is live per `type`; VkDescriptorImageInfo / VkDescriptorBufferInfo
  // are what the descriptor-set writer consumes directly.
  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      union {
        VkDescriptorImageInfo image;   // sampler + imageView + imageLayout
        VkDescriptorBufferInfo buffer; // buffer + offset + range
        VkAccelerationStructureKHR accelStructure;
      };
    } vk;
#endif
  };
};

struct RIAccelStructure {
  RIAccelStructure() { memset(this, 0, sizeof(*this)); }

  // Creates VkAccelerationStructureKHR backed by desc->storage at
  // desc->storageOffset. Caller must allocate desc->storage with
  // VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR and
  // VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, sized at least
  // desc->storageSize bytes.
  int init(struct RIDevice *device, const struct RIAccelStructureDesc *desc);
  void dispose(struct RIDevice *device);
  bool isEmpty() const;
  uint64_t getDeviceAddress(struct RIDevice *device) const;
  void setDebugObjectName(struct RIDevice *device, const char *name);

  enum RIAccelStructureType_e type;
  uint32_t flags; // RIAccelStructureBuildBits_e snapshot
  // uint64_t buildScratchSize;
  // uint64_t updateScratchSize;
  // uint64_t storageOffset;
  // struct RIBuffer storage; // caller-owned backing buffer
  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      VkAccelerationStructureKHR handle;
      VkDeviceAddress deviceAddress;
    } vk;
#endif
  };
  hash_t cookie;
};

struct RIAccelStructureDesc {
  RIAccelStructureDesc() { memset(this, 0, sizeof(*this)); }

  // Query backing-storage and scratch sizes before RIAccelStructure::init;
  // any out-pointer may be NULL. For BLAS, geometries describes the geometry
  // layout used to compute sizes; the actual vertex/index buffer addresses
  // don't need to be valid until the build command.
  void getMemoryReqs(struct RIDevice *device, uint64_t *outStorageSize,
                     uint64_t *outBuildScratchSize,
                     uint64_t *outUpdateScratchSize) const;

  enum RIAccelStructureType_e type;
  uint32_t flags; // RIAccelStructureBuildBits_e
  uint32_t
      geometryOrInstanceNum; // BLAS: geometry count, TLAS: max instance count
  const struct RIAccelGeometryDesc *geometries; // BLAS only; NULL for TLAS
  struct RIBuffer *storage;
  uint64_t storageOffset;
  uint64_t storageSize; // from getMemoryReqs
};

struct RIBuildBlasDesc {
  RIBuildBlasDesc() { memset(this, 0, sizeof(*this)); }
  struct RIAccelStructure *dst;
  struct RIAccelStructure *src; // NULL unless mode==UPDATE
  enum RIAccelBuildMode_e mode;
  const struct RIAccelGeometryDesc *geometries;
  uint32_t geometryNum;
  struct RIBuffer *scratchBuffer;
  uint64_t scratchOffset;
};

struct RIBuildTlasDesc {
  RIBuildTlasDesc() { memset(this, 0, sizeof(*this)); }
  struct RIAccelStructure *dst;
  struct RIAccelStructure *src; // NULL unless mode==UPDATE
  enum RIAccelBuildMode_e mode;
  uint32_t instanceNum;
  struct RIBuffer *instanceBuffer; // RIAccelInstance entries
  uint64_t instanceOffset;
  struct RIBuffer *scratchBuffer;
  uint64_t scratchOffset;
};

struct RIRect {
  RIRect() { memset(this, 0, sizeof(*this)); }
  int16_t x;
  int16_t y;
  int16_t width;
  int16_t height;
};

struct RIViewport {
  RIViewport() { memset(this, 0, sizeof(*this)); }
  float x;
  float y;
  float width;
  float height;
  float depthMin;
  float depthMax;
  bool originBottomLeft; // expects "isViewportOriginBottomLeftSupported"
};

struct RIPool {
  RIPool() { memset(this, 0, sizeof(*this)); }
  // Creates the command pool on the queue's family.
  void init(struct RIDevice *device, struct RIQueue *queue);
  // Resets every command buffer allocated from the pool.
  void reset(struct RIDevice *device);
  void dispose(struct RIDevice *device);
  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      VkQueue queue;
      VkCommandPool pool;
    } vk;
#endif
  };
};

// ---------------------------------------------------------------------------
// Backend-neutral command-buffer types (RICmd abstraction — stage 1: additive
// types only; the RICmd struct itself is migrated in a later stage). Lifted
// from feature/metal-backend (b908af4). These are pure additions and do not
// change any existing API, so they keep the Vulkan build green on their own.
// ---------------------------------------------------------------------------

typedef uint64_t RIDeviceSize;

enum RIAttachmentLoadOp_e {
  RI_ATTACHMENT_LOAD_OP_LOAD,
  RI_ATTACHMENT_LOAD_OP_CLEAR,
  RI_ATTACHMENT_LOAD_OP_DONT_CARE,
};

enum RIAttachmentStoreOp_e {
  RI_ATTACHMENT_STORE_OP_STORE,
  RI_ATTACHMENT_STORE_OP_DONT_CARE,
};

struct RIClearValue {
  float color[4];
  float depth;
  uint32_t stencil;
};

// One color or depth/stencil attachment for RICmd::vk_d3d12_beginRendering /
// mtl_encoderDraw. `view` references the RITextureView abstraction
// (vk.image / mtl.view), so the same call site works on either backend.
struct RIRenderingAttachment {
  struct RITextureView view;
  uint8_t loadOp;  // RIAttachmentLoadOp_e
  uint8_t storeOp; // RIAttachmentStoreOp_e
  // Depth attachment only: bind as DEPTH_READ_ONLY_OPTIMAL (depth-tested but
  // not written) instead of DEPTH_STENCIL_ATTACHMENT_OPTIMAL. Ignored by Metal,
  // which expresses read-only depth through the pipeline's depth-stencil state.
  bool readOnly;
  // Depth/stencil attachment: when the view's format carries a stencil aspect,
  // set hasStencil to also bind it as a stencil attachment with its own
  // load/store (a pass may load depth but clear stencil). clearValue.stencil
  // supplies the clear. When hasStencil, the depth aspect binds as
  // DEPTH_ATTACHMENT_OPTIMAL and the stencil aspect as
  // STENCIL_ATTACHMENT_OPTIMAL.
  bool hasStencil;
  uint8_t stencilLoadOp;  // RIAttachmentLoadOp_e
  uint8_t stencilStoreOp; // RIAttachmentStoreOp_e
  struct RIClearValue clearValue;
};

struct RIBeginRenderingDesc {
  struct RIRect renderArea;
  uint32_t colorCount;
  const struct RIRenderingAttachment *colors;
  const struct RIRenderingAttachment *depthStencil; // nullable
};

struct RIBufferTextureCopyDesc {
  RIDeviceSize bufferOffset;
  uint32_t bufferRowLength;   // texels (Vulkan VkBufferImageCopy)
  uint32_t bufferImageHeight; // texels (Vulkan VkBufferImageCopy)
  uint32_t bytesPerRow;       // bytes  (Metal copyFromBuffer)
  uint32_t bytesPerImage;     // bytes  (Metal copyFromBuffer)
  uint32_t mipLevel;
  uint32_t arrayLayer;
  int32_t x, y, z;
  uint32_t width, height, depth;
};

struct RIImageCopyDesc {
  uint32_t srcMipLevel;
  uint32_t srcArrayLayer;
  int32_t srcX, srcY, srcZ;
  uint32_t dstMipLevel;
  uint32_t dstArrayLayer;
  int32_t dstX, dstY, dstZ;
  uint32_t width, height, depth;
};

// RIProgram lives in namespace hpl; forward-declare it so RICmd can take it by
// reference (vk_d3d12_setPushConstants) without pulling in RIProgram.h here.
namespace hpl {
class RIProgram;
}

struct RICmd {
  RICmd() { memset(this, 0, sizeof(*this)); }

  // Allocates the command buffer from the pool.
  void init(struct RIDevice *device, struct RIPool *pool);
  // Begins/ends recording (one-time-submit).
  void begin(struct RIDevice *device);
  void end(struct RIDevice *device);
  // Returns the command buffer to its pool and clears the handles.
  void dispose(struct RIDevice *device);
  bool isEmpty() const;

  // Leaf dispatch/draw command methods. Pipeline binding is done separately
  // (RIProgram::bindPipeline / bindComputePipeline / bindRayTracingPipeline);
  // these are the "go" calls that issue the actual work. The device's renderer
  // selects the active backend (is_target_selected); on Metal these route
  // through the open encoder, on Vulkan they record vkCmd* into vk.cmd.
  void dispatch(struct RIDevice *device, uint32_t groupCountX,
                uint32_t groupCountY, uint32_t groupCountZ);
  void dispatchIndirect(struct RIDevice *device, struct RIBuffer *buffer,
                        RIDeviceSize offset);
  void draw(struct RIDevice *device, uint32_t vertexCount,
            uint32_t instanceCount, uint32_t firstVertex,
            uint32_t firstInstance);
  void drawIndexed(struct RIDevice *device, uint32_t indexCount,
                   uint32_t instanceCount, uint32_t firstIndex,
                   int32_t vertexOffset, uint32_t firstInstance);
  void drawIndirect(struct RIDevice *device, struct RIBuffer *buffer,
                    RIDeviceSize offset, uint32_t drawCount, uint32_t stride);
  void drawIndexedIndirect(struct RIDevice *device, struct RIBuffer *buffer,
                           RIDeviceSize offset, uint32_t drawCount,
                           uint32_t stride);

  // [vk/mtl] Buffer-to-buffer copy. Vulkan records vkCmdCopyBuffer; Metal opens
  // a blit encoder and calls copyFromBuffer.
  void copyBuffer(struct RIDevice *device, struct RIBuffer *src,
                  RIDeviceSize srcOffset, struct RIBuffer *dst,
                  RIDeviceSize dstOffset, RIDeviceSize size);

  // [vk/mtl] Buffer-to-texture copy of a single subresource region. The desc
  // carries the staging layout in both texel (Vulkan) and byte (Metal) form.
  void copyBufferToTexture(struct RIDevice *device, struct RIBuffer *src,
                           struct RITexture *dst,
                           const struct RIBufferTextureCopyDesc &desc);

  // [vk/mtl] Image-to-image copy of a single 1:1 region (no scaling). Caller
  // owns the surrounding barriers.
  void copyImage(struct RIDevice *device, struct RITexture *src,
                 struct RITexture *dst, const struct RIImageCopyDesc &desc);

  // [vk/mtl] Clear a storage image (mip 0, layer 0) in GENERAL layout.
  void clearStorageImage(struct RIDevice *device, struct RITexture *image,
                         const float color[4]);

  // [vk/d3d12] Dynamic-rendering scope (vkCmdBeginRendering/EndRendering).
  // Metal uses mtl_encoderDraw / mtl_encoderEnd instead (kept as separate
  // APIs).
  void vk_d3d12_beginRendering(struct RIDevice *device,
                               const struct RIBeginRenderingDesc &desc);
  void vk_d3d12_endRendering(struct RIDevice *device);

  void setViewport(struct RIDevice *device,
                   const struct RIViewport &viewport);
  void setScissor(struct RIDevice *device, const struct RIRect &scissor);

  // [vk/d3d12] Push constants. Metal supplies the same data inline via the
  // [[buffer(0)]] push-constant block (setBytes) at bind/draw time, so it has
  // no discrete command here (vk_d3d12_-prefixed, like beginRendering/barriers).
  // The stage flags and layout come from the program's reflection, so the call
  // site only supplies the data range.
  void vk_d3d12_setPushConstants(struct RIDevice *device,
                                 hpl::RIProgram &program, uint32_t offset,
                                 uint32_t size, const void *data);

  // Acceleration-structure build commands; numDescs structures are submitted
  // in a single backend call. Caller-supplied scratchBuffer must include
  // VK_BUFFER_USAGE_STORAGE_BUFFER_BIT and
  // VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, with scratchOffset aligned to
  // minAccelerationStructureScratchOffsetAlignment. Input vertex/index/
  // instance buffers must include
  // VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR and
  // VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT. Takes the device, which supplies
  // VMA / device address / scratch alignment; the active backend comes from
  // RIIsTargetSelected.
  void buildBlas(struct RIDevice *device,
                 const struct RIBuildBlasDesc *descs, uint32_t numDescs);
  void buildTlas(struct RIDevice *device,
                 const struct RIBuildTlasDesc *descs, uint32_t numDescs);

  // Emit pipeline barriers from RI resource-state transitions (see
  // RIBarrier.h). All groups are batched into a single backend barrier
  // command (vkCmdPipelineBarrier2); any count may be zero. The template
  // parameters MemN/BufN/TexN are the stack capacities reserved for the
  // backend barrier scratch arrays (compile-time sized); a capacity of 0
  // moves that group to the heap instead, for dynamically sized batches.
  template <uint32_t MemN, uint32_t BufN, uint32_t TexN>
  void vk_d3d12_resourceBarrier(
      uint32_t memoryBarrierNum, const struct RIMemoryBarrier *memoryBarriers,
      uint32_t bufferBarrierNum, const struct RIBufferBarrier *bufferBarriers,
      uint32_t textureBarrierNum,
      const struct RITextureBarrier *textureBarriers) {
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
      const struct RIMemoryBarrier &src = memoryBarriers[i];
      VkMemoryBarrier2 &dst = mem[i];
      dst = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
      dst.srcStageMask = ri_vk_RIStageBitsToVK(src.beforeStages, src.before);
      dst.srcAccessMask = ri_vk_RIResourceStateToAccess(src.before);
      dst.dstStageMask = ri_vk_RIStageBitsToVK(src.afterStages, src.after);
      dst.dstAccessMask = ri_vk_RIResourceStateToAccess(src.after);
    }

    for (uint32_t i = 0; i < bufferBarrierNum; i++) {
      const struct RIBufferBarrier &src = bufferBarriers[i];
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
      const struct RITextureBarrier &src = textureBarriers[i];
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

  // Single-barrier conveniences (vk_d3d12_-prefixed: no-op on Metal, which
  // tracks hazards automatically).
  void vk_d3d12_memoryBarrier(const struct RIMemoryBarrier &barrier) {
    vk_d3d12_resourceBarrier<1, 0, 0>(1, &barrier, 0, NULL, 0, NULL);
  }
  void vk_d3d12_bufferBarrier(const struct RIBufferBarrier &barrier) {
    vk_d3d12_resourceBarrier<0, 1, 0>(0, NULL, 1, &barrier, 0, NULL);
  }
  void vk_d3d12_textureBarrier(const struct RITextureBarrier &barrier) {
    vk_d3d12_resourceBarrier<0, 0, 1>(0, NULL, 0, NULL, 1, &barrier);
  }
  // Fixed-capacity texture-batch convenience; N is the stack capacity.
  template <uint32_t N>
  void vk_d3d12_textureBarriers(uint32_t num,
                                const struct RITextureBarrier *barriers) {
    vk_d3d12_resourceBarrier<0, 0, N>(0, NULL, 0, NULL, num, barriers);
  }

  // Bind a single index buffer. Takes an RIBuffer* (the RI abstraction)
  // rather than a backend handle so the same call site survives a future
  // DX12 backend.
  void bindIndexBuffer(struct RIDevice *device, struct RIBuffer *buffer,
                       RIDeviceSize offset, enum RIIndexType_e indexType);

  // Bind `count` vertex buffers. The template parameter N is only the stack
  // capacity reserved for the backend handle scratch array (compile-time
  // sized, no heap); `count` is the actual number bound and must be <= N.
  // `buffers` is a raw RIBuffer* array of length `count` (a null entry
  // binds nothing); `offsets` is a parallel byte-offset array. e.g. for a
  // fixed 5-stream layout where all 5 are live:
  // cmd->bindVertexBuffers<5>(0, 5, bufs). RIBuffer* keeps the call site
  // backend-agnostic for the planned DX12 path.
  template <uint32_t N>
  void bindVertexBuffers(uint32_t firstBinding, uint32_t count,
                         struct RIBuffer *const *buffers,
                         const RIDeviceSize *offsets) {
    assert(count <= N);
#if (DEVICE_IMPL_VULKAN)
    VkBuffer vkBufs[N];
    for (uint32_t i = 0; i < count; ++i)
      vkBufs[i] = buffers[i] ? buffers[i]->vk.buffer : VK_NULL_HANDLE;
    vkCmdBindVertexBuffers(vk.cmd, firstBinding, count, vkBufs, offsets);
#endif
#if (DEVICE_IMPL_MTL)
    // Streams bind at the top of the buffer table (RI_MTL_VertexBufferIndex) —
    // the same mapping the pipeline's MTLVertexDescriptor uses. A null entry
    // binds nothing.
    assert(mtl.render && "bindVertexBuffers requires an open render encoder");
    for (uint32_t i = 0; i < count; ++i)
      if (buffers[i])
        mtl.render->setVertexBuffer(buffers[i]->mtl.buffer,
                                    (NS::UInteger)offsets[i],
                                    RI_MTL_VertexBufferIndex(firstBinding + i));
#endif
  }

  // Convenience overload binding `count` streams at offset 0.
  template <uint32_t N>
  void bindVertexBuffers(uint32_t firstBinding, uint32_t count,
                         struct RIBuffer *const *buffers) {
    const RIDeviceSize offsets[N] = {};
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

struct RIQueue {
  RIQueue() { memset(this, 0, sizeof(*this)); }
  void waitIdle(struct RIDevice *device);
  uint32_t getFlags(const struct RIRenderer *renderer) const {
#if (DEVICE_IMPL_VULKAN)
    return (vk.queueFlags & VK_QUEUE_GRAPHICS_BIT ? RI_QUEUE_GRAPHICS_BIT : 0) |
           (vk.queueFlags & VK_QUEUE_COMPUTE_BIT ? RI_QUEUE_COMPUTE_BIT : 0) |
           (vk.queueFlags & VK_QUEUE_TRANSFER_BIT ? RI_QUEUE_TRANSFER_BIT : 0) |
           (vk.queueFlags & VK_QUEUE_SPARSE_BINDING_BIT
                ? RI_QUEUE_SPARSE_BINDING_BIT
                : 0) |
           (vk.queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR
                ? RI_QUEUE_VIDEO_DECODE_BIT
                : 0) |
           (vk.queueFlags & VK_QUEUE_VIDEO_ENCODE_BIT_KHR
                ? RI_QUEUE_VIDEO_ENCODE_BIT
                : 0) |
           (vk.queueFlags & VK_QUEUE_PROTECTED_BIT ? RI_QUEUE_PROTECTED_BIT
                                                   : 0) |
           (vk.queueFlags & VK_QUEUE_OPTICAL_FLOW_BIT_NV
                ? RI_QUEUE_OPTICAL_FLOW_BIT_NV
                : 0);
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

template <uint32_t MaxImageCount = RI_MAX_SWAPCHAIN_IMAGES> struct RISwapchain {
  RISwapchain() { memset(this, 0, sizeof(*this)); }
  static constexpr uint32_t MAX_IMAGE_COUNT = MaxImageCount;

  // Destroys the per-image semaphores, the swapchain and the surface (the
  // swapchain images themselves are owned by the swapchain). Defined at the
  // bottom of this header once RIDevice/RIRenderer are complete.
  void dispose(struct RIDevice *device);

  struct RIQueue *presentQueue;
  uint16_t imageCount;
  uint16_t width;
  uint16_t height;
  uint32_t format; // RI_Format_e
  struct RITexture textures[MaxImageCount];
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

struct RIRenderer {
  RIRenderer() { memset(this, 0, sizeof(*this)); }
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

// There is only ever one renderer per process. Rather than threading a
// RIRenderer through the API (or exposing a global object), the single instance
// lives at file scope in RIRenderer.cpp and is reached through these top-level
// functions — they are the application's highest-level entry points and assume
// the renderer has been initialized.
int InitRIRenderer(const struct RIBackendInit *init);
int EnumerateRIAdapters(struct RIPhysicalAdapter *adapters,
                        uint32_t *numAdapters);
// Destroys the debug messenger + instance and tears down the loader
// (volkFinalize). Instance-level — the device is normally gone by the time
// this runs.
void ShutdownRIRenderer();

#if DEVICE_MULTI_BACKEND
// Active backend (RIDeviceAPI_e); defined in RIRenderer.cpp. Only needed when
// more than one backend is compiled in (otherwise it is known at compile time).
uint8_t RIActiveBackendApi();
#elif DEVICE_IMPL_VULKAN
#define RI_ACTIVE_BACKEND_API RI_DEVICE_API_VK
#elif DEVICE_IMPL_MTL
#define RI_ACTIVE_BACKEND_API RI_DEVICE_API_MTL
#elif DEVICE_IMPL_D3D12
#define RI_ACTIVE_BACKEND_API RI_DEVICE_API_D3D12
#elif DEVICE_IMPL_D3D11
#define RI_ACTIVE_BACKEND_API RI_DEVICE_API_D3D11
#endif

// True when the renderer's active backend matches `targetApi` (RIDeviceAPI_e).
// static inline so single-backend builds fold this to a compile-time constant,
// letting the optimizer drop the dead backend branches at every call site
// (the RICmd Vulkan/Metal paths). Multi-backend builds read the active backend.
static inline bool RIIsTargetSelected(uint8_t targetApi) {
#if DEVICE_MULTI_BACKEND
  return targetApi == RIActiveBackendApi();
#else
  assert(targetApi == RI_ACTIVE_BACKEND_API); // single backend: must match
  (void)targetApi;
  return true;
#endif
}

#if (DEVICE_IMPL_VULKAN)
VkInstance RIGetVkInstance();
#endif

struct RIBackendInit {
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

struct RIPhysicalAdapter {
  RIPhysicalAdapter() { memset(this, 0, sizeof(*this)); }
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

struct RIDevice {
  RIDevice() { memset(this, 0, sizeof(*this)); }
  // Creates the logical device, queues and VMA allocator on the adapter
  // selected in init->physicalAdapter (RIDeviceDesc lives in RIRenderer.h).
  int init(struct RIDeviceDesc *init);
  void dispose();
  struct RIPhysicalAdapter physicalAdapter;
  struct RIQueue queues[RI_QUEUE_LEN];
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

inline bool RITexture::isEmpty() const {
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK))
    return vk.image == VK_NULL_HANDLE;
#endif
  assert(false && "unhandled backend");
  return true;
}

inline bool RIBuffer::isEmpty() const {
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK))
    return vk.buffer == VK_NULL_HANDLE;
#endif
  assert(false && "unhandled backend");
  return true;
}

inline bool RITextureView::isEmpty() const {
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK))
    return vk.image == VK_NULL_HANDLE;
#endif
  assert(false && "unhandled backend");
  return true;
}

inline bool RICmd::isEmpty() const {
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK))
    return vk.cmd == VK_NULL_HANDLE || vk.pool == VK_NULL_HANDLE;
#endif
  assert(false && "unhandled backend");
  return true;
}

inline bool RISampler::isEmpty() const {
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK))
    return vk.sampler == VK_NULL_HANDLE;
#endif
  assert(false && "unhandled backend");
  return true;
}

inline bool RIAccelStructure::isEmpty() const {
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK))
    return vk.handle == VK_NULL_HANDLE;
#endif
  assert(false && "unhandled backend");
  return true;
}

template <uint32_t MaxImageCount>
inline void RISwapchain<MaxImageCount>::dispose(struct RIDevice *device) {
#if (DEVICE_IMPL_VULKAN)
  if (RIIsTargetSelected(RI_DEVICE_API_VK)) {
    for (uint32_t p = 0; p < MaxImageCount; p++) {
      if (vk.imageAcquireSem[p])
        vkDestroySemaphore(device->vk.device, vk.imageAcquireSem[p], NULL);
      if (vk.finishSem[p])
        vkDestroySemaphore(device->vk.device, vk.finishSem[p], NULL);
    }
    if (vk.swapchain)
      vkDestroySwapchainKHR(device->vk.device, vk.swapchain, NULL);
    if (vk.surface)
      vkDestroySurfaceKHR(RIGetVkInstance(), vk.surface, NULL);
  }
#endif
}

// RICommandRingBuffer / RICommandRingElement live in their own header; included
// here (after RIDevice/RIRenderer are complete) so the template method bodies
// see the full definitions. Backward-compatible: anything including RITypes.h
// still gets the ring API.
#include "graphics/RICommandRingBuffer.h"

#endif
