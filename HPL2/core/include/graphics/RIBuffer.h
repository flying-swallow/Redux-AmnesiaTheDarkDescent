#ifndef RI_BUFFER_H
#define RI_BUFFER_H

#include "graphics/RIDefines.h"
#include "graphics/RIPreamble.h"
#include "system/Hasher.h"
#include <cstring>
#include <optional>
#include <stdint.h>


struct RIDevice;
struct RIRenderer;

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
  RI_BUFFER_USAGE_TRANSFER_SRC = 0x400,
  RI_BUFFER_USAGE_TRANSFER_DST = 0x800,
  RI_BUFFER_USAGE_INDIRECT = 0x1000,
  // Buffer must be addressable as a raw GPU pointer (Vulkan BDA /
  // VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT).
  RI_BUFFER_USAGE_DEVICE_ADDRESS = 0x2000,
};

// Where a buffer's memory lives, expressed backend-neutrally. Maps to VMA
// memory usage + flags on Vulkan.
enum RIMemoryLocation_e {
  // Device-local, not host-mapped (VMA AUTO_PREFER_DEVICE). mappedAddress is
  // null; seed via the resource uploader.
  RI_MEMORY_DEVICE,
  // Persistently mapped for sequential host writes
  // (VMA AUTO + MAPPED + HOST_ACCESS_SEQUENTIAL_WRITE).
  RI_MEMORY_HOST_UPLOAD,
  // Persistently mapped for random host reads — a GPU->CPU readback
  // destination (VMA AUTO + MAPPED + HOST_ACCESS_RANDOM). mappedAddress is
  // host-readable; invalidate the allocation before reading if it landed in
  // HOST_CACHED memory.
  RI_MEMORY_HOST_READBACK,
};

// Backend-neutral buffer creation descriptor consumed by RIBuffer::create.
struct RIBufferDesc {
  uint64_t size;
  uint32_t usage; // RIBufferUsage_e bitmask
  RIMemoryLocation_e location;
  uint64_t alignment; // 0 = no special alignment requirement
};

struct RIBuffer {
  RIBuffer() { memset(this, 0, sizeof(*this)); }

  void dispose(struct RIDevice *device);
  // Backend-neutral buffer factory: does all the VMA work, sets mappedAddress
  // for host-upload buffers, and stamps the resource cookie. The cookie is a
  // globally-unique random value (handle reuse makes the backend handle unsafe
  // as an identity); pass `hash` to override when a stable/shared cookie is
  // genuinely wanted.
  static struct RIBuffer create(struct RIDevice *device,
                                const struct RIBufferDesc &desc,
                                std::optional<hash_t> hash = {});
  void setDebugObjectName(struct RIDevice *device, const char *name);
  uint64_t GetDeviceHandle(struct RIDevice *device);
  bool isEmpty() const;

  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      struct VmaAllocation_T *allocation;
      VkBuffer buffer;
    } vk;
#endif
  };
  void *mappedAddress;
  // Stable identity / descriptor-set cache key, stamped at creation from the
  // backend handle (0 == empty). RIDescriptor derives its cookie from this.
  hash_t cookie;
};

#endif // RI_BUFFER_H
