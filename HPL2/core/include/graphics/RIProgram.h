
#ifndef RI_PROGRAM_H
#define RI_PROGRAM_H

#include "system/Hasher.h"
#include <array>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "resources/FileSearcher.h"
#include "system/SystemTypes.h"

#include "RIDescriptorSetAllocator.h"
#include "RITypes.h"

namespace hpl {

class RIBindlessDescriptorSet {
public:
  struct {
    VkDescriptorSetLayout m_bindlessSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_bindlessPool = VK_NULL_HANDLE;
    VkDescriptorSet m_bindlessSet = VK_NULL_HANDLE;
  } vk;

  RIBindlessDescriptorSet() {}

  struct Binding {
    uint32_t binding;
    VkDescriptorType descriptorType;
    uint32_t descriptorCount;
    VkShaderStageFlags stageFlags;
    VkDescriptorBindingFlags flags;
  };

  struct WriteBinding {
    uint32_t binding;
    uint32_t arrayElement;
    RIDescriptor descriptor;
  };

  void writeDescriptors(RIDevice *device,
                        std::span<const WriteBinding> writes);

  void initialize(RIDevice *device, std::span<const Binding> bindings,
                  std::span<const VkDescriptorPoolSize> poolSizes);

  void destroy(RIDevice *device) {
    if (vk.m_bindlessPool != VK_NULL_HANDLE) {
      vkDestroyDescriptorPool(device->vk.device, vk.m_bindlessPool, NULL);
      vk.m_bindlessPool = VK_NULL_HANDLE;
      vk.m_bindlessSet = VK_NULL_HANDLE;
    }
    if (vk.m_bindlessSetLayout != VK_NULL_HANDLE) {
      vkDestroyDescriptorSetLayout(device->vk.device, vk.m_bindlessSetLayout,
                                   NULL);
      vk.m_bindlessSetLayout = VK_NULL_HANDLE;
    }
  }
};

class RIProgram {
public:
  static constexpr size_t DESCRIPTOR_SET_MAX = 4;
  static constexpr size_t MAX_VERTEX_ATTRIBUTES = 16;
  struct PipelineSlot {
    union {
#if (DEVICE_IMPL_VULKAN)
      struct {
        VkPipeline handle;
      } vk;
#endif
    };
  };

  // Ray-tracing pipeline cache slot. Carries the SBT buffer + the four
  // strided-device-address regions vkCmdTraceRaysKHR needs so traceRays()
  // can dispatch without recomputing them.
  struct RTPipelineSlot {
    union {
#if (DEVICE_IMPL_VULKAN)
      struct {
        VkPipeline handle;
        VkBuffer sbtBuffer;
        VmaAllocation sbtAlloc;
        VkStridedDeviceAddressRegionKHR raygenRegion;
        VkStridedDeviceAddressRegionKHR missRegion;
        VkStridedDeviceAddressRegionKHR hitRegion;
        VkStridedDeviceAddressRegionKHR callableRegion;
      } vk;
#endif
    };
  };

  struct DescriptorBinding {
    DescriptorBinding() : handle(), registerOffset(0) {}
    explicit DescriptorBinding(const char *name, const RIDescriptor &desc,
                               uint32_t registerOffset = 0)
        : handle(DescriptorBindingID::Create(name)),
          registerOffset(registerOffset), descriptor(desc) {}

    struct DescriptorBindingID handle;
    uint32_t registerOffset;
    struct RIDescriptor descriptor;
  };

  struct DescriptorSetSlot {
    union {
#if (DEVICE_IMPL_VULKAN)
      struct {
        VkDescriptorSetLayout setLayout;
      } vk;
#endif
    };
    struct RIDescriptorSetAlloc alloc; // the set allocator
    uint16_t samplerMaxNum;
    uint16_t combinedImageSamplerMaxNum;
    uint16_t constantBufferMaxNum;
    uint16_t dynamicConstantBufferMaxNum;
    uint16_t textureMaxNum;
    uint16_t storageTextureMaxNum;
    uint16_t bufferMaxNum;
    uint16_t storageBufferMaxNum;
    uint16_t structuredBufferMaxNum;
    uint16_t storageStructuredBufferMaxNum;
    uint16_t accelerationStructureMaxNum;
    // External: set layout is borrowed from the caller; the program does
    // not allocate, cache, or write any descriptor set for this slot.
    // The caller is responsible for binding the descriptor set via
    // vkCmdBindDescriptorSets at the matching set index.
    bool isExternal;
  };
  struct ShaderBinary {
    std::vector<char> buf;
    // SPIR-V entry-point function name. Defaults to "main" (GLSL +
    // single-entry-point HLSL); Slang shaders compiled with
    // `-fvk-use-entrypoint-name` keep their function name in OpEntryPoint
    // (e.g. "csMain", "collectCellInfo"), which must match
    // VkPipelineShaderStageCreateInfo::pName exactly.
    std::string entryPoint = "main";
  };

  struct BindingReflection {
    hash_t hash;
    uint16_t isArray : 1;
    uint16_t dimCount : 8;
    uint16_t set : 3;
    uint16_t baseRegisterIndex;
  };

  enum ProgramStages {
    PROGRAM_STAGE_VERTEX,
    PROGRAM_STAGE_FRAGMENT,
    PROGRAM_STAGE_COMPUTE,
    PROGRAM_STAGE_RAYGEN,
    PROGRAM_STAGE_MISS,
    PROGRAM_STAGE_CLOSEST_HIT,
    PROGRAM_STAGE_ANY_HIT,
    PROGRAM_STAGE_INTERSECTION,
    PROGRAM_STAGE_CALLABLE,
    PROGRAM_STAGES_MAX
  };

  struct BindlessLayout {};

  struct ModuleStage {
    uint8_t stage;
    std::span<char> data;
    // Optional override of the entry-point function name. Leave as "main"
    // for GLSL or default-named HLSL. Slang shaders compiled with
    // `-fvk-use-entrypoint-name` emit OpEntryPoint with the source
    // function name; set this to match (e.g. "csMain", "collectCellInfo").
    const char *entryPoint = "main";
  };
  const struct BindingReflection *
  findReflection(const struct DescriptorBindingID &handle);
  // `externalLayouts` (optional) lets the caller hand in a pre-built
  // VkDescriptorSetLayout for any set index. For each set with a non-null
  // entry the program uses that layout in its pipeline layout, marks the
  // slot as external, and skips all descriptor-set allocation / writes /
  // binds for it. The caller binds those sets directly (e.g. via
  // bindBindlessDescriptorSet).
  void initialize(RIDevice *device, std::span<ModuleStage> init,
                  std::span<const VkDescriptorSetLayout> externalLayouts = {});
  static std::vector<char> loadShaderStage(cFileSearcher *searcher,
                                           const tString &asName);
  void bindPipeline(struct RIDevice *device, struct RICmd *cmd,
                    hash_t pipelineHash, const char *debugName,
                    VkGraphicsPipelineCreateInfo *pipelineCreateInfo);
  void bindComputePipeline(struct RIDevice *device, struct RICmd *cmd,
                           hash_t pipelineHash, const char *debugName,
                           VkComputePipelineCreateInfo *pipelineCreateInfo);
  // Bind a Vulkan ray-tracing pipeline. The cache slot also owns the SBT
  // (built from `vkGetRayTracingShaderGroupHandlesKHR`) so traceRays can
  // dispatch without recomputing the strided regions. The caller need only
  // fill in pNext / flags / maxPipelineRayRecursionDepth on
  // pipelineCreateInfo — pStages, pGroups, layout are filled here from
  // the program's shaderBin and pipelineLayout.
  void bindRayTracingPipeline(
      struct RIDevice *device, struct RICmd *cmd, hash_t pipelineHash,
      const char *debugName,
      VkRayTracingPipelineCreateInfoKHR *pipelineCreateInfo);
  // Issue vkCmdTraceRaysKHR against the cached SBT for `pipelineHash`.
  // The pipeline must have been created earlier via bindRayTracingPipeline.
  void traceRays(struct RICmd *cmd, hash_t pipelineHash, uint32_t width,
                 uint32_t height, uint32_t depth);
  void bindDescriptors(
      struct RIDevice *device, struct RICmd *cmd, uint32_t frameIndex,
      DescriptorBinding *binding, size_t bindingCount,
      VkPipelineBindPoint bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS);
  // Bind an externally-owned bindless descriptor set at `setIndex` against
  // this program's pipeline layout. The matching slot in `programDescriptors`
  // must have been registered as external via `externalLayouts` at
  // initialize(); the program does not allocate or write descriptors for it.
  void bindBindlessDescriptorSet(
      struct RICmd *cmd, RIBindlessDescriptorSet *bindless, uint32_t setIndex,
      VkPipelineBindPoint bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS);
#if (DEVICE_IMPL_VULKAN)
  VkPipelineLayout getPipelineLayout() const { return impl.vk.pipelineLayout; }
#endif
  explicit RIProgram() {}

private:
  RIProgram(const RIProgram &) = delete;
  RIProgram(RIProgram &&) = delete;
  RIProgram &operator=(RIProgram &&) = delete;

  union __impl {
    struct {
#if (DEVICE_IMPL_VULKAN)
      struct {
        VkShaderStageFlags shaderStageFlags;
        uint32_t size;
      } pushConstant;
      VkPipelineLayout pipelineLayout;
#endif
    } vk;
  } impl{};
  RIDevice *device = NULL;
  uint16_t reflection_len = 0;
  uint32_t vertex_input_mask = 0;
  std::array<uint32_t, MAX_VERTEX_ATTRIBUTES> vertex_input_format{};
  bool hasPushConstant = false;
  std::array<struct DescriptorSetSlot, DESCRIPTOR_SET_MAX> programDescriptors{};
  std::array<ShaderBinary, PROGRAM_STAGES_MAX> shaderBin;
  std::unordered_map<hash_t, PipelineSlot> pipeline;
  std::unordered_map<hash_t, RTPipelineSlot> rtPipeline;
  std::vector<BindingReflection> bindingReflection;
};
} // namespace hpl
#endif
