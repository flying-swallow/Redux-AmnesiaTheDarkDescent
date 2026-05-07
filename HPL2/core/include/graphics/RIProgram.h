
#ifndef RI_PROGRAM_H
#define RI_PROGRAM_H

#include "system/Hasher.h"
#include <array>
#include <span>
#include <unordered_map>
#include <vector>

#include "resources/FileSearcher.h"
#include "system/SystemTypes.h"

#include "RIDescriptorSetAllocator.h"
#include "RITypes.h"

namespace hpl {
class RIProgram {
public:
  static constexpr size_t DESCRIPTOR_SET_MAX = 3;
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

  struct DescriptorBinding {
    struct DescriptorBindingID handle;
    uint32_t registerOffset;
    struct RIDescriptor_s descriptor;
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
    uint16_t constantBufferMaxNum;
    uint16_t dynamicConstantBufferMaxNum;
    uint16_t textureMaxNum;
    uint16_t storageTextureMaxNum;
    uint16_t bufferMaxNum;
    uint16_t storageBufferMaxNum;
    uint16_t structuredBufferMaxNum;
    uint16_t storageStructuredBufferMaxNum;
    uint16_t accelerationStructureMaxNum;
  };
  struct ShaderBinary {
    std::vector<char> buf;
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
    PROGRAM_STAGES_MAX
  };

  struct ModuleStage {
    uint8_t stage;
    std::span<char> data;
  };
  const struct BindingReflection *
  findReflection(const struct DescriptorBindingID &handle);
  void initialize(RIDevice_s *device, std::span<ModuleStage> init);
  static std::vector<char> loadShaderStage(cFileSearcher *searcher,
                                           const tString &asName);
  void bindPipeline(struct RIDevice_s *device, struct RICmd_s *cmd,
                    hash_t pipelineHash, const char *debugName,
                    VkGraphicsPipelineCreateInfo *pipelineCreateInfo);
  void bindComputePipeline(struct RIDevice_s *device, struct RICmd_s *cmd,
                           hash_t pipelineHash, const char *debugName,
                           VkComputePipelineCreateInfo *pipelineCreateInfo);
  void bindDescriptors(
      struct RIDevice_s *device, struct RICmd_s *cmd, uint32_t frameIndex,
      DescriptorBinding *binding, size_t bindingCount,
      VkPipelineBindPoint bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS);
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
  } impl;
  RIDevice_s *device = NULL;
  uint16_t reflection_len = 0;
  uint32_t vertex_input_mask = 0;
  std::array<uint32_t, MAX_VERTEX_ATTRIBUTES> vertex_input_format{};
  bool hasPushConstant = false;
  std::array<struct DescriptorSetSlot, DESCRIPTOR_SET_MAX> programDescriptors;
  std::array<ShaderBinary, PROGRAM_STAGES_MAX> shaderBin;
  std::unordered_map<hash_t, PipelineSlot> pipeline;
  std::vector<BindingReflection> bindingReflection;
};
} // namespace hpl
#endif
