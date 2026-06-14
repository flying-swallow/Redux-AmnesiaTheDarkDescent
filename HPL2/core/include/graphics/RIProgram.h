
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

// The engine's external/bindless descriptor-set handle. The *handle* is
// backend-agnostic (union, like RIBuffer/RITexture in RITypes.h) so RIProgram's
// public API can name it on both backends: the Vulkan arm carries the
// layout/pool/set; the Metal arm a pre-encoded argument buffer bound at the
// external set index. The set *construction* methods
// (initialize/writeDescriptors/destroy) remain Vulkan-only for now — the Metal
// producer (argument-buffer encode) is deferred with the DXR passes (see
// METAL_EXCLUDE in the shader build).
class RIBindlessDescriptorSet {
public:
  union {
#if (DEVICE_IMPL_VULKAN)
    struct {
      VkDescriptorSetLayout m_bindlessSetLayout;
      VkDescriptorPool m_bindlessPool;
      VkDescriptorSet m_bindlessSet;
    } vk;
#endif
#if (DEVICE_IMPL_MTL)
    struct {
      // Pre-encoded argument buffer, bound at the external set index, plus the
      // retained encoder reused by writeDescriptors.
      MTL::Buffer *argumentBuffer;
      MTL::ArgumentEncoder *encoder;
    } mtl;
#endif
  };

  RIBindlessDescriptorSet() {
#if (DEVICE_IMPL_VULKAN)
    vk.m_bindlessSetLayout = VK_NULL_HANDLE;
    vk.m_bindlessPool = VK_NULL_HANDLE;
    vk.m_bindlessSet = VK_NULL_HANDLE;
#endif
#if (DEVICE_IMPL_MTL)
    mtl.argumentBuffer = nullptr;
    mtl.encoder = nullptr;
#endif
  }

#if (DEVICE_IMPL_MTL)
  // Non-array set-0 resources (buffers + single textures) collected at
  // writeDescriptors time, made resident on the encoder by bindExternalSet
  // (argument-buffer contents are not auto-resident on Metal). The two big
  // bindless texture arrays (binding 0/1) are excluded — making 32768 textures
  // resident per dispatch is impractical; they need an MTL::Heap + useHeap.
  std::vector<MTL::Resource *> mtlResident;
#endif

  // Backend-neutral bindless layout entry. descriptorType is RIDescriptorType_e;
  // stageFlags is an RIShaderStageBits_e mask and flags an
  // RIBindlessBindingFlagBits_e mask (both Vulkan-only in meaning — Metal argument
  // buffers ignore stage visibility and update-after-bind).
  struct Binding {
    uint32_t binding;
    uint8_t descriptorType; // RIDescriptorType_e
    uint32_t descriptorCount;
    uint32_t stageFlags; // RIShaderStageBits_e
    uint32_t flags;      // RIBindlessBindingFlagBits_e
  };

  struct WriteBinding {
    uint32_t binding;
    uint32_t arrayElement;
    RIDescriptor descriptor;
  };

  // Vulkan builds the descriptor-set layout/pool/set (pool sizes derived from
  // `bindings`); Metal builds the argument encoder + Shared argument buffer.
  void initialize(RIDevice *device, std::span<const Binding> bindings);
  void writeDescriptors(RIDevice *device,
                        std::span<const WriteBinding> writes);
  void destroy(RIDevice *device);
};

class RIProgram {
public:
  static constexpr size_t DESCRIPTOR_SET_MAX = 4;
  static constexpr size_t MAX_VERTEX_ATTRIBUTES = 16;

  // Pipeline description + handle types (RIGraphicsPipelineDesc,
  // RIComputePipelineDesc, RIPipeline, RIRayTracingPipeline) live in RITypes.h.

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
#if (DEVICE_IMPL_VULKAN)
    union {
      struct {
        VkDescriptorSetLayout setLayout;
      } vk;
    };
#endif
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

  // Metal binding kind, derived from the logical RIDescriptorType_e (see
  // RIProgramBinding). Stored in MtlArg / consumed by bindDescriptors.
  enum RIMtlBindingKind {
    RI_MTL_BIND_BUFFER,        // [[buffer(N)]] (constant*/device*)
    RI_MTL_BIND_TEXTURE,       // [[texture(N)]]
    RI_MTL_BIND_SAMPLER,       // [[sampler(N)]]
    RI_MTL_BIND_ACCEL,         // acceleration structure
    RI_MTL_BIND_PUSH_CONSTANT, // setBytes target, [[buffer(N)]]
    RI_MTL_BIND_BINDLESS_ARG,  // unbounded array -> argument buffer at [[buffer(N)]]
  };

  // One logical resource binding, visible across the `stages` set, with the
  // per-backend placement bundled in one entry. Replaces the old per-stage,
  // per-backend RIVkBinding/RIMtlBinding tables (a resource shared by VS+FS is
  // a single entry with stages == VERTEX|FRAGMENT). `name` is the logical
  // resource name (the string passed to DescriptorBindingID::Create at the
  // bind sites). Metal index spaces are per stage, so when slangc assigns a
  // resource different [[*(N)]] indices per stage, author one entry per stage.
  struct RIProgramBinding {
    const char *name;
    uint8_t type;    // RIDescriptorType_e (drives the VK type + the Metal kind)
    uint16_t count;  // array length; 0/1 == single
    uint32_t stages; // RIShaderStageBits_e bitmask (combined across stages)
    struct {
      uint8_t set;
      uint8_t binding;
    } vk; // Vulkan {set,binding} (== [vk::binding(binding,set)])
    struct {
      uint8_t reg;
      uint8_t space;
    } d3d; // reserved for the future D3D12 backend
    struct {
      uint16_t index; // per-stage Metal [[kind(N)]] index; kind derived from type
    } mtl;
  };
  // mtl.index sentinel: the resource exists on Vulkan (needs a set-layout slot)
  // but slangc dropped it from the Metal kernel (no [[*(N)]] arg) — skip on Metal.
  static constexpr uint16_t RI_MTL_NONE = 0xFFFF;

  struct ModuleStage {
    uint8_t stage;
    std::span<char> data;
    // Optional override of the entry-point function name. Leave as "main"
    // for GLSL or default-named HLSL. Slang shaders compiled with
    // `-fvk-use-entrypoint-name` emit OpEntryPoint with the source
    // function name; set this to match (e.g. "csMain", "collectCellInfo").
    const char *entryPoint = "main";
    // Metal [[buffer(N)]] slot this stage reads the push constant from. The PC
    // size/visibility live on RIProgramDescriptor; the slot is per stage because
    // slangc can assign it different indices per stage (e.g. VS buffer1 when a
    // UBO occupies buffer0, FS buffer0 when no buffer precedes it). Ignored
    // unless this stage's bit is set in RIProgramDescriptor::pushConstantStages.
    uint16_t pushConstantMtlIndex = 0;
  };

  // Bundles everything initialize() needs. `bindings` is the unified,
  // combined-stage binding list; `pushConstant*` describe the (single) push
  // constant block, if any (its per-stage Metal slot lives on each ModuleStage).
  // `externalSets` (optional) lets the caller hand in pre-built external
  // descriptor-set handles, positional by set index (nullptr = program-managed
  // slot): the program marks that slot external and skips alloc/write/bind for
  // it, and the caller binds it via bindExternalSet.
  struct RIProgramDescriptor {
    std::span<ModuleStage> stages;
    std::span<const RIProgramBinding> bindings = {};
    std::span<RIBindlessDescriptorSet *const> externalSets = {};
    uint16_t pushConstantSize = 0;       // 0 = none
    uint32_t pushConstantStages = 0;     // RIShaderStageBits_e mask
  };

  const struct BindingReflection *
  findReflection(const struct DescriptorBindingID &handle);
  void initialize(RIDevice *device, const RIProgramDescriptor &desc);
  static std::vector<char> loadShaderStage(cFileSearcher *searcher,
                                           const tString &asName);
  // Backend-neutral pipeline binding. Builds (and caches by pipelineHash) the
  // backend pipeline state from the neutral descriptor and binds it. This is
  // the API render code should use; the Vk*CreateInfo overloads below are the
  // legacy Vulkan-only path being migrated off.
  void bindPipeline(struct RIDevice *device, struct RICmd *cmd,
                    hash_t pipelineHash, const char *debugName,
                    const RIGraphicsPipelineDesc &desc);
  void bindComputePipeline(struct RIDevice *device, struct RICmd *cmd,
                           hash_t pipelineHash, const char *debugName,
                           const RIComputePipelineDesc &desc);
#if (DEVICE_IMPL_VULKAN)
  void bindPipeline(struct RIDevice *device, struct RICmd *cmd,
                    hash_t pipelineHash, const char *debugName,
                    VkGraphicsPipelineCreateInfo *pipelineCreateInfo);
  void bindComputePipeline(struct RIDevice *device, struct RICmd *cmd,
                           hash_t pipelineHash, const char *debugName,
                           VkComputePipelineCreateInfo *pipelineCreateInfo);
#endif
  // Backend-neutral ray-tracing pipeline binding. Builds (and caches by
  // pipelineHash) the backend RT pipeline from the program's RT shaderBin
  // stages: on Vulkan a VkRayTracingPipeline + SBT (built from
  // `vkGetRayTracingShaderGroupHandlesKHR`) so traceRays can dispatch without
  // recomputing the strided regions; the desc only carries maxRecursionDepth/
  // flags (stages/groups/layout are filled from the program). Metal RT is a
  // compute-pipeline model and not implemented yet.
  void bindRayTracingPipeline(struct RIDevice *device, struct RICmd *cmd,
                              hash_t pipelineHash, const char *debugName,
                              const RIRayTracingPipelineDesc &desc);
  // Issue vkCmdTraceRaysKHR against the cached SBT for `pipelineHash`.
  // The pipeline must have been created earlier via bindRayTracingPipeline.
  void traceRays(struct RICmd *cmd, hash_t pipelineHash, uint32_t width,
                 uint32_t height, uint32_t depth);
  // Backend-neutral push-constants upload. Uses the program's reflected
  // push-constant stage flags + pipeline layout; on Metal this becomes
  // setBytes() on the active encoder at the push-constant buffer slot.
  // Replaces direct vkCmdPushConstants(getPipelineLayout(), ...).
  void pushConstants(struct RICmd *cmd, const void *data, uint32_t size,
                     uint32_t offset = 0);
  void bindDescriptors(
      struct RIDevice *device, struct RICmd *cmd, uint32_t frameIndex,
      DescriptorBinding *binding, size_t bindingCount,
      RIPipelineBindPoint_e bindPoint = RI_PIPELINE_BIND_GRAPHICS);
  // Bind an externally-owned descriptor set at `setIndex` against this program.
  // The matching slot in `programDescriptors` must have been registered as
  // external via `externalSets` at initialize(); the program does not allocate
  // or write descriptors for it. On Vulkan this is vkCmdBindDescriptorSets
  // against the pipeline layout; on Metal the handle's argument buffer is bound
  // at the set index on the active encoder.
  void bindExternalSet(struct RICmd *cmd, RIBindlessDescriptorSet *set,
                       uint32_t setIndex,
                       RIPipelineBindPoint_e bindPoint = RI_PIPELINE_BIND_GRAPHICS);
#if (DEVICE_IMPL_VULKAN)
  VkPipelineLayout getPipelineLayout() const { return impl.vk.pipelineLayout; }
#endif
  // Backend-neutral "has this program been initialized?" check. `device` is set
  // at the top of initialize(); a default-constructed/never-loaded program has
  // it NULL. Replaces the Vulkan-only getPipelineLayout()==VK_NULL_HANDLE guard.
  bool isValid() const { return device != NULL; }
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
  std::unordered_map<hash_t, RIPipeline> pipeline;
  std::unordered_map<hash_t, RIRayTracingPipeline> rtPipeline;
  std::vector<BindingReflection> bindingReflection;
#if (DEVICE_IMPL_MTL)
  // Per-stage Metal binding map built from ModuleStage::mtlBindings: logical
  // resource name hash -> {kind, flat per-stage [[*(N)]] index}. Indexed by
  // ProgramStages. Push-constant index per stage cached separately (-1 = none).
  struct MtlArg {
    uint16_t index;
    uint8_t kind; // RIMtlBindingKind
  };
  std::unordered_map<hash_t, MtlArg> mtlArgByStage[PROGRAM_STAGES_MAX];
  int16_t mtlPushConstantIndex[PROGRAM_STAGES_MAX] = {-1, -1, -1, -1, -1,
                                                      -1, -1, -1, -1};
#endif
};
} // namespace hpl
#endif
