#ifndef RI_DESCRIPTOR_H
#define RI_DESCRIPTOR_H

// Descriptors, samplers and acceleration structures, grouped by use case
// (mirrors ref_nri/ri_descriptor.h). Depends only on the prelude + resource
// leaf headers; RIDevice is used by pointer only, so a forward declaration is
// enough (this stays below RIDevice.h in the include layering).
#include "graphics/RIPreamble.h"
#include "graphics/RIBarrier.h"    // RIResourceState_e (RIDescriptor::sampledImage)
#include "graphics/RIBuffer.h"     // RIBuffer (descriptor / accel geometry refs)
#include "graphics/RIFormat.h"     // RI_Format_e (RIAccelTrianglesDesc)
#include "graphics/RIPipeline.h"   // RIIndexType_e (RIAccelTrianglesDesc)
#include "graphics/RITextureView.h" // RITextureView (image descriptors)
#include "system/Hasher.h"         // hash_t / hash_data / HASH_INITIAL_VALUE
#include <cstring>                 // memset / strlen

struct RIDevice;

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

enum RIDescriptorFlags_e {
  RI_VK_DESC_BEGIN = 0,
  RI_VK_DESC_OWN_SAMPLER = 0x1,   // owns the backing assets VKImage, VkBuffer
  RI_VK_DESC_OWN_IMAGE_VIEW = 0x2 // owns the backing sampler
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
struct RIAccelStructureDesc;

// Owned backend sampler object. The only descriptor-referenced resource that
// owns a backend handle; created/cached once (cGraphics filter cache) and
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

#endif // RI_DESCRIPTOR_H
