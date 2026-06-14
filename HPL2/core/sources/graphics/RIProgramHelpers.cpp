#include "graphics/RIProgramHelpers.h"

#include <array>

namespace hpl {

void LoadSlangCompute(RIDevice *device, RIProgram &prog,
                      cResources *resources, const char *name,
                      const char *entryPoint,
                      std::span<RIBindlessDescriptorSet *const> externalSets,
                      std::span<const RIProgram::RIProgramBinding> bindings,
                      uint16_t pushConstantSize,
                      uint16_t pushConstantMtlIndex) {
  auto bin = RIProgram::loadShaderStage(resources->GetFileSearcher(), name);
  std::array<RIProgram::ModuleStage, 1> stages = {RIProgram::ModuleStage{
      RIProgram::PROGRAM_STAGE_COMPUTE, bin, entryPoint}};
  stages[0].pushConstantMtlIndex = pushConstantMtlIndex;
  RIProgram::RIProgramDescriptor desc = {};
  desc.stages = stages;
  desc.bindings = bindings;
  desc.externalSets = externalSets;
  desc.pushConstantSize = pushConstantSize;
  desc.pushConstantStages = pushConstantSize ? RI_SHADER_STAGE_COMPUTE : 0u;
  prog.initialize(device, desc);
}

void LoadSlangGraphics(RIDevice *device, RIProgram &prog,
                       cResources *resources, const char *vertName,
                       const char *fragName, const char *vertEntryPoint,
                       const char *fragEntryPoint,
                       std::span<RIBindlessDescriptorSet *const> externalSets,
                       std::span<const RIProgram::RIProgramBinding> bindings,
                       uint16_t pushConstantSize, uint32_t pushConstantStages,
                       uint16_t pushConstantMtlIndex) {
  const bool shared =
      vertName && fragName && std::string_view(vertName) == fragName;
  auto vsBin = RIProgram::loadShaderStage(resources->GetFileSearcher(), vertName);
  auto fsBin = shared ? std::vector<char>{}
                      : RIProgram::loadShaderStage(resources->GetFileSearcher(), fragName);
  std::array<RIProgram::ModuleStage, 2> stages = {
      RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, vsBin,
                             vertEntryPoint},
      RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT,
                             shared ? vsBin : fsBin, fragEntryPoint}};
  // Both stages share the same PC Metal slot here; programs whose VS/FS read the
  // push constant from different [[buffer(N)]] slots build the descriptor
  // directly (setting each ModuleStage::pushConstantMtlIndex).
  stages[0].pushConstantMtlIndex = pushConstantMtlIndex;
  stages[1].pushConstantMtlIndex = pushConstantMtlIndex;
  RIProgram::RIProgramDescriptor desc = {};
  desc.stages = stages;
  desc.bindings = bindings;
  desc.externalSets = externalSets;
  desc.pushConstantSize = pushConstantSize;
  desc.pushConstantStages = pushConstantStages;
  prog.initialize(device, desc);
}

} // namespace hpl
