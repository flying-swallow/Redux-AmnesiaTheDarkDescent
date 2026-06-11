#include "graphics/RIProgramHelpers.h"

#include <array>

namespace hpl {

void LoadSlangCompute(RIDevice *device, RIProgram &prog,
                      cResources *resources, const char *name,
                      const char *entryPoint,
                      std::span<const VkDescriptorSetLayout> externalLayouts) {
  auto bin = RIProgram::loadShaderStage(resources->GetFileSearcher(), name);
  std::array<RIProgram::ModuleStage, 1> stages = {RIProgram::ModuleStage{
      RIProgram::PROGRAM_STAGE_COMPUTE, bin, entryPoint}};
  prog.initialize(device, stages, externalLayouts);
}

void LoadSlangGraphics(RIDevice *device, RIProgram &prog,
                       cResources *resources, const char *vertName,
                       const char *fragName, const char *vertEntryPoint,
                       const char *fragEntryPoint,
                       std::span<const VkDescriptorSetLayout> externalLayouts) {
  if (vertName && fragName && std::string_view(vertName) == fragName) {
    auto bin = RIProgram::loadShaderStage(resources->GetFileSearcher(), vertName);
    std::array<RIProgram::ModuleStage, 2> stages = {
        RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, bin,
                               vertEntryPoint},
        RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, bin,
                               fragEntryPoint}};
    prog.initialize(device, stages, externalLayouts);
    return;
  }
  auto vsBin = RIProgram::loadShaderStage(resources->GetFileSearcher(), vertName);
  auto fsBin = RIProgram::loadShaderStage(resources->GetFileSearcher(), fragName);
  std::array<RIProgram::ModuleStage, 2> stages = {
      RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, vsBin,
                             vertEntryPoint},
      RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, fsBin,
                             fragEntryPoint}};
  prog.initialize(device, stages, externalLayouts);
}

} // namespace hpl
